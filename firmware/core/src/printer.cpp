#include "dm/printer.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "dm/cover.h"
#include "dm/json.h"
#include "printer_driver.h"

namespace dm {

namespace {

// Catalog generated from protocol/printers.json
const PrinterModelInfo MODELS[] = {
#include "generated/printers.inc"
};

/**
 * @description Creates the driver named in the catalog, or null for an unknown driver.
 */
std::unique_ptr<PrinterDriver> make_driver(const std::string &name) {
  if (name == "creality_ws") return make_creality_driver();
  return nullptr;
}

bool ends_with_ci(const std::string &text, const char *suffix) {
  const size_t n = strlen(suffix);
  if (text.size() < n) return false;
  for (size_t i = 0; i < n; i++)
    if (tolower(static_cast<unsigned char>(text[text.size() - n + i])) != suffix[i]) return false;
  return true;
}

/**
 * @description Parses a slicer duration such as "4h51m8s" or "29m". NaN when it is not one.
 */
double parse_duration(const std::string &text) {
  if (text.empty() || !isdigit(static_cast<unsigned char>(text[0]))) return NAN;
  double total = 0, value = 0;
  bool digits = false;
  for (char c : text) {
    if (isdigit(static_cast<unsigned char>(c))) {
      value = value * 10 + (c - '0');
      digits = true;
    } else if ((c == 'h' || c == 'm' || c == 's') && digits) {
      total += value * (c == 'h' ? 3600 : c == 'm' ? 60 : 1);
      value = 0;
      digits = false;
    } else {
      return NAN;
    }
  }
  return digits ? NAN : total;
}

/**
 * @description True for a material code of a slicer name: "PLA", "PETG", "PLA-CF", "ASA"…
 */
bool is_material(const std::string &text) {
  if (text.size() < 2 || text.size() > 10) return false;
  bool letter = false;
  for (char c : text) {
    if (isupper(static_cast<unsigned char>(c))) letter = true;
    else if (!isdigit(static_cast<unsigned char>(c)) && c != '-' && c != '+') return false;
  }
  return letter;
}

void add_number(cJSON *obj, const char *key, double value) {
  if (!std::isnan(value)) cJSON_AddNumberToObject(obj, key, value);
}

}  // namespace

const PrinterModelInfo *find_printer_model(const std::string &id) {
  for (const auto &model : MODELS)
    if (id == model.id) return &model;
  return nullptr;
}

PrinterFileName PrinterSession::describe_file(const std::string &path) {
  PrinterFileName out;
  const size_t slash = path.rfind('/');
  std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::string base = name;
  for (const char *ext : {".gcode", ".bgcode", ".gco", ".g"}) {
    if (ends_with_ci(name, ext)) {
      name.resize(name.size() - strlen(ext));
      break;
    }
  }
  // Slicer suffix: "_<material>_<estimate>" (Creality Print, Orca…)
  const size_t last = name.rfind('_');
  if (last != std::string::npos) {
    const double estimate = parse_duration(name.substr(last + 1));
    if (!std::isnan(estimate)) {
      out.estimate = estimate;
      name.resize(last);
      const size_t prev = name.rfind('_');
      if (prev != std::string::npos && is_material(name.substr(prev + 1))) {
        out.material = name.substr(prev + 1);
        name.resize(prev);
      }
    }
  }
  for (const char *ext : {".stl", ".3mf", ".obj", ".step"}) {
    if (ends_with_ci(name, ext)) {
      name.resize(name.size() - strlen(ext));
      break;
    }
  }
  // Underscores read as spaces on the screen
  std::string pretty;
  for (char c : name) {
    const char ch = c == '_' ? ' ' : c;
    if (ch == ' ' && (pretty.empty() || pretty.back() == ' ')) continue;
    pretty += ch;
  }
  while (!pretty.empty() && pretty.back() == ' ') pretty.pop_back();
  out.name = pretty.empty() ? base : pretty;
  return out;
}

PrinterSession::PrinterSession(Bus &bus) : bus_(bus) {}

PrinterSession::~PrinterSession() = default;

std::string PrinterSession::configure(const std::string &model, const std::string &host, int port) {
  std::lock_guard<std::mutex> lock(mutex_);
  generation_++;
  model_ = find_printer_model(model);
  driver_ = model_ && !host.empty() ? make_driver(model_->driver) : nullptr;
  host_ = host;
  connected_ = false;
  outbox_.clear();
  data_ = PrinterData();
  thumbs_requested_.clear();
  thumbs_queue_.clear();
  thumb_busy_ = false;
  if (!driver_) {
    data_.status.state = "off";
    publish_status_locked();
    bus_.post("printer/files", cJSON_CreateObject());
    return "";
  }
  publish_status_locked();
  return driver_->url(host, port > 0 ? port : model_->port);
}

void PrinterSession::set_sender(Sender sender) {
  std::lock_guard<std::mutex> lock(mutex_);
  sender_ = std::move(sender);
}

void PrinterSession::set_fetcher(Fetcher fetcher) {
  std::lock_guard<std::mutex> lock(mutex_);
  fetcher_ = std::move(fetcher);
}

void PrinterSession::on_open() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!driver_) return;
    connected_ = true;
    driver_->on_open(outbox_);
  }
  flush();
}

void PrinterSession::on_close() {
  std::lock_guard<std::mutex> lock(mutex_);
  if (!driver_) return;
  const bool was_connected = connected_;
  connected_ = false;
  outbox_.clear();
  // The last job and files stay; temperatures and progress are stale
  if (was_connected || data_.status.state == "connecting") {
    PrinterStatus offline;
    offline.state = "offline";
    offline.file = data_.status.file;
    data_.status = offline;
    publish_status_locked();
  }
}

void PrinterSession::on_message(const std::string &text) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!driver_) return;
    const std::string before = data_.status.state;
    const unsigned changes = driver_->on_message(text, data_, outbox_);
    const std::string &after = data_.status.state;
    if (before != after) {
      // End of a print: tell the user, and fetch the history that now holds it
      const bool was_running = before == "printing" || before == "paused";
      if (was_running && (after == "finished" || after == "stopped" || after == "error" || after == "idle")) {
        driver_->refresh_files(outbox_);
        const std::string name = describe_file(data_.status.file).name;
        const char *what = after == "finished" ? "Impression terminée" : after == "error" ? "Erreur d'impression" : "Impression arrêtée";
        cJSON *toast = cJSON_CreateObject();
        cJSON_AddStringToObject(toast, "text", name.empty() ? what : (std::string(what) + " : " + name).c_str());
        bus_.post("sys/toast", toast);
      }
    }
    if (changes & PrinterDriver::STATUS) publish_status_locked();
    if (changes & PrinterDriver::FILES) publish_files_locked();
  }
  flush();
}

bool PrinterSession::command(const std::string &name, const std::string &arg) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!driver_ || !connected_ || !sender_) return false;
    if (!driver_->command(name, arg, outbox_)) return false;
    // A new print shows up in the history when it ends; the file list may have changed meanwhile
    if (name == "start") driver_->refresh_files(outbox_);
  }
  flush();
  return true;
}

void PrinterSession::request_thumbnail(const std::string &path) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!driver_ || path.empty() || thumbs_requested_.count(path)) return;
    thumbs_requested_.insert(path);
    thumbs_queue_.push_back(path);
  }
  fetch_next_thumbnail();
}

void PrinterSession::fetch_next_thumbnail() {
  // The fetcher is called without the lock: it may call `done` at once on failure
  std::string path, url;
  Fetcher fetcher;
  unsigned generation;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (thumb_busy_ || thumbs_queue_.empty() || !fetcher_ || !driver_) return;
    path = thumbs_queue_.front();
    thumbs_queue_.erase(thumbs_queue_.begin());
    url = driver_->thumbnail_url(host_, path);
    if (url.empty()) return;
    thumb_busy_ = true;
    fetcher = fetcher_;
    generation = generation_;
  }
  fetcher(url, [this, path, generation](std::string body) {
    std::vector<uint8_t> pixels;
    const bool ok = !body.empty() && decode_png_thumbnail(body, THUMB_SIZE, THUMB_BACKGROUND, pixels);
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (generation != generation_) return;
      thumb_busy_ = false;
      if (ok) bus_.post("printer/thumb/" + path, cover_json(path, THUMB_SIZE, pixels));
    }
    fetch_next_thumbnail();
  });
}

std::string PrinterSession::status_json() {
  std::lock_guard<std::mutex> lock(mutex_);
  cJSON *tree = status_tree_locked();
  std::string text = json::print(tree);
  cJSON_Delete(tree);
  return text;
}

cJSON *PrinterSession::status_tree_locked() const {
  const PrinterStatus &s = data_.status;
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "state", s.state.c_str());
  if (model_) {
    cJSON_AddStringToObject(root, "model", model_->id);
    cJSON_AddStringToObject(root, "model_name", (std::string(model_->brand) + " " + model_->name).c_str());
  }
  if (driver_) {
    cJSON *caps = cJSON_AddArrayToObject(root, "caps");
    for (const char *cap : driver_->caps()) cJSON_AddItemToArray(caps, cJSON_CreateString(cap));
  }
  if (!s.file.empty()) {
    const PrinterFileName file = describe_file(s.file);
    cJSON_AddStringToObject(root, "file", s.file.c_str());
    cJSON_AddStringToObject(root, "name", file.name.c_str());
    if (!file.material.empty()) cJSON_AddStringToObject(root, "material", file.material.c_str());
    add_number(root, "estimate", file.estimate);
  }
  add_number(root, "progress", s.progress);
  add_number(root, "elapsed", s.elapsed);
  add_number(root, "remaining", s.remaining);
  if (s.layers > 0) {
    cJSON_AddNumberToObject(root, "layer", std::max(s.layer, 0));
    cJSON_AddNumberToObject(root, "layers", s.layers);
  }
  add_number(root, "nozzle", s.nozzle);
  add_number(root, "nozzle_target", s.nozzle_target);
  add_number(root, "bed", s.bed);
  add_number(root, "bed_target", s.bed_target);
  if (s.light >= 0) cJSON_AddBoolToObject(root, "light", s.light == 1);
  if (s.calibrate >= 0) cJSON_AddBoolToObject(root, "calibrate", s.calibrate == 1);
  if (!s.error.empty()) cJSON_AddStringToObject(root, "error", s.error.c_str());
  return root;
}

void PrinterSession::publish_status_locked() { bus_.post("printer", status_tree_locked()); }

void PrinterSession::publish_files_locked() {
  cJSON *root = cJSON_CreateObject();
  cJSON *recent = cJSON_AddArrayToObject(root, "recent");
  std::set<std::string> seen;
  for (const PrinterJob &job : data_.history) {
    if (static_cast<int>(seen.size()) >= MAX_RECENT) break;
    if (!seen.insert(job.path).second) continue;
    const PrinterFileName file = describe_file(job.path);
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "path", job.path.c_str());
    cJSON_AddStringToObject(item, "name", file.name.c_str());
    if (!file.material.empty()) cJSON_AddStringToObject(item, "material", file.material.c_str());
    add_number(item, "estimate", file.estimate);
    add_number(item, "when", job.when);
    add_number(item, "duration", job.duration);
    if (!job.result.empty()) cJSON_AddStringToObject(item, "result", job.result.c_str());
    // A file deleted from the printer cannot be printed again
    cJSON_AddBoolToObject(item, "exists", !data_.has_files || data_.files.count(job.path) > 0);
    cJSON_AddItemToArray(recent, item);
  }
  bus_.post("printer/files", root);
}

void PrinterSession::flush() {
  std::vector<std::string> batch;
  Sender sender;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    batch.swap(outbox_);
    sender = sender_;
  }
  if (!sender) return;
  for (const auto &text : batch) sender(text);
}

}  // namespace dm
