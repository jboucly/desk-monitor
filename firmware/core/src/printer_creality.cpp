// Creality stock firmware (Creality OS, a Klipper fork): WebSocket on port 9999, no authentication.
// Checked on a CR-10 SE (see protocol/README.md, "3D printer"):
// - the printer pushes partial JSON objects by itself: temperatures every second, progress, and a full
//   state object at connection (only the changed fields afterwards);
// - {"method":"get","params":{"reqGcodeFile":1}} answers retGcodeFileInfo (files on the printer);
// - {"method":"get","params":{"reqHistory":1}} answers historyList (last prints);
// - thumbnails are PNGs served at http://<host>/downloads/humbnail/<file name>.png ("humbnail" sic).
// Checked during a print on the CR-10 SE: state 1 (printing, also during the calibration before the first
// layer), 4 (stopped), 5 (paused), and every command. A stop sent during the bed calibration only applies
// once the calibration ends.
#include <cstring>

#include "dm/json.h"
#include "printer_driver.h"

namespace dm {

namespace {

/**
 * @description Reads a number sent either as a JSON number or as a string ("62.28"). NaN when missing.
 */
double number(const cJSON *obj, const char *key) {
  const cJSON *item = json::get(obj, key);
  if (cJSON_IsNumber(item)) return item->valuedouble;
  if (cJSON_IsString(item) && *item->valuestring) {
    char *end = nullptr;
    const double value = strtod(item->valuestring, &end);
    if (end != item->valuestring) return value;
  }
  return NAN;
}

/**
 * @description Copies a number into `out` when present. Returns true when the value changed.
 */
bool merge(const cJSON *obj, const char *key, double &out) {
  const double value = number(obj, key);
  if (std::isnan(value) || value == out) return false;
  out = value;
  return true;
}

bool merge(const cJSON *obj, const char *key, int &out) {
  const double value = number(obj, key);
  if (std::isnan(value) || static_cast<int>(value) == out) return false;
  out = static_cast<int>(value);
  return true;
}

/**
 * @description Common state of the "state" code. Seen on a CR-10 SE: 1 printing, 2 finished, 4 stopped,
 * 5 paused. 0 and 3 follow the reverse-engineered clients.
 */
const char *state_name(int code) {
  switch (code) {
    case 0: return "idle";
    case 1: return "printing";
    case 2: return "finished";
    case 3: return "error";
    case 4: return "stopped";
    case 5: return "paused";
    default: return "idle";
  }
}

/**
 * @description Percent-encodes a URL path segment (spaces and non-ASCII bytes of file names).
 */
std::string url_encode(const std::string &text) {
  static const char *HEX = "0123456789ABCDEF";
  std::string out;
  for (unsigned char c : text) {
    if (isalnum(c) || strchr("-_.~", c)) {
      out += static_cast<char>(c);
    } else {
      out += '%';
      out += HEX[c >> 4];
      out += HEX[c & 15];
    }
  }
  return out;
}

std::string request(const char *method, const char *key, cJSON *value) {
  cJSON *root = cJSON_CreateObject();
  cJSON_AddStringToObject(root, "method", method);
  cJSON_AddItemToObject(cJSON_AddObjectToObject(root, "params"), key, value);
  std::string text = json::print(root);
  cJSON_Delete(root);
  return text;
}

class CrealityDriver : public PrinterDriver {
 public:
  std::string url(const std::string &host, int port) const override {
    return "ws://" + host + ":" + std::to_string(port) + "/";
  }

  std::vector<const char *> caps() const override { return {"pause", "resume", "stop", "start", "light"}; }

  void on_open(std::vector<std::string> &out) override { refresh_files(out); }

  void refresh_files(std::vector<std::string> &out) override {
    out.push_back(request("get", "reqGcodeFile", cJSON_CreateNumber(1)));
    out.push_back(request("get", "reqHistory", cJSON_CreateNumber(1)));
  }

  unsigned on_message(const std::string &text, PrinterData &data, std::vector<std::string> &out) override {
    auto root = json::parse(text);
    if (!cJSON_IsObject(root.get())) return 0;
    const cJSON *r = root.get();

    // Keep-alive of Creality Print: answered with "ok"
    if (!strcmp(json::str(r, "ModeCode"), "heart_beat")) {
      out.push_back("ok");
      return 0;
    }

    unsigned changes = 0;
    PrinterStatus &s = data.status;
    if (merge(r, "nozzleTemp", s.nozzle)) changes |= STATUS;
    if (merge(r, "bedTemp0", s.bed)) changes |= STATUS;
    if (merge(r, "targetNozzleTemp", s.nozzle_target)) changes |= STATUS;
    if (merge(r, "targetBedTemp0", s.bed_target)) changes |= STATUS;
    if (merge(r, "printProgress", s.progress)) changes |= STATUS;
    if (merge(r, "printJobTime", s.elapsed)) changes |= STATUS;
    if (merge(r, "printLeftTime", s.remaining)) changes |= STATUS;
    if (merge(r, "layer", s.layer)) changes |= STATUS;
    if (merge(r, "TotalLayer", s.layers)) changes |= STATUS;
    if (merge(r, "lightSw", s.light)) changes |= STATUS;

    const double code = number(r, "state");
    if (!std::isnan(code) && s.state != state_name(static_cast<int>(code))) {
      s.state = state_name(static_cast<int>(code));
      changes |= STATUS;
    }
    const cJSON *file = json::get(r, "printFileName");
    if (cJSON_IsString(file) && s.file != file->valuestring) {
      s.file = file->valuestring;
      changes |= STATUS;
    }
    if (const cJSON *err = json::get(r, "err")) {
      const double errcode = number(err, "errcode");
      const std::string error = !std::isnan(errcode) && errcode != 0 ? "Erreur " + std::to_string(static_cast<int>(errcode)) : "";
      if (error != s.error) {
        s.error = error;
        changes |= STATUS;
      }
    }

    // Files on the printer: "dir:name:size:?:mtime:?:thumbnail" entries separated by ';'
    if (const cJSON *info = json::get(r, "retGcodeFileInfo")) {
      data.files.clear();
      const std::string list = json::str(info, "fileInfo");
      size_t start = 0;
      while (start < list.size()) {
        size_t end = list.find(';', start);
        if (end == std::string::npos) end = list.size();
        const std::string entry = list.substr(start, end - start);
        const size_t dir_end = entry.find(':');
        const size_t name_end = dir_end == std::string::npos ? std::string::npos : entry.find(':', dir_end + 1);
        if (name_end != std::string::npos)
          data.files.insert(entry.substr(0, dir_end) + "/" + entry.substr(dir_end + 1, name_end - dir_end - 1));
        start = end + 1;
      }
      data.has_files = true;
      changes |= FILES;
    }

    // Last prints, newest first
    const cJSON *history = json::get(r, "historyList");
    if (cJSON_IsArray(history)) {
      data.history.clear();
      const cJSON *item = nullptr;
      cJSON_ArrayForEach(item, history) {
        PrinterJob job;
        job.path = json::str(item, "filename");
        if (job.path.empty()) continue;
        job.when = number(item, "starttime");
        job.duration = number(item, "usagetime");
        const double finish = number(item, "printfinish");
        job.result = std::isnan(finish) ? "" : finish == 1 ? "done" : "failed";
        data.history.push_back(std::move(job));
      }
      changes |= FILES;
    }
    return changes;
  }

  bool command(const std::string &name, const std::string &arg, std::vector<std::string> &out) override {
    if (name == "pause" || name == "resume") {
      out.push_back(request("set", "pause", cJSON_CreateNumber(name == "pause" ? 1 : 0)));
    } else if (name == "stop") {
      out.push_back(request("set", "stop", cJSON_CreateNumber(1)));
    } else if (name == "start" && !arg.empty()) {
      out.push_back(request("set", "opGcodeFile", cJSON_CreateString(("printprt:" + arg).c_str())));
    } else if (name == "light") {
      out.push_back(request("set", "lightSw", cJSON_CreateNumber(arg == "on" ? 1 : 0)));
    } else {
      return false;
    }
    return true;
  }

  std::string thumbnail_url(const std::string &host, const std::string &path) const override {
    const size_t slash = path.rfind('/');
    const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    return "http://" + host + "/downloads/humbnail/" + url_encode(name) + ".png";
  }
};

}  // namespace

std::unique_ptr<PrinterDriver> make_creality_driver() { return std::make_unique<CrealityDriver>(); }

}  // namespace dm
