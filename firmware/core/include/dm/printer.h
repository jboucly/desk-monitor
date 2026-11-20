// 3D printer session (transport-free). See protocol/README.md, "3D printer".
//
// The platform owns the WebSocket to the printer and forwards open/close/message events here. A driver
// (one per protocol family, chosen from the printer catalog protocol/printers.json) turns the printer's
// frames into a common model, published on the bus:
//   printer               state, job, progress, temperatures
//   printer/files         the last printed files, newest first
//   printer/thumb/<path>  a file's thumbnail, THUMB_SIZE x THUMB_SIZE RGB565 ({"id", "w", "h", "data"})
#pragma once

#include <cmath>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "dm/bus.h"

namespace dm {

/**
 * @description One entry of the printer catalog (generated from protocol/printers.json).
 */
struct PrinterModelInfo {
  const char *id;
  const char *brand;
  const char *name;
  const char *driver;
  int port;  // default port of the driver
};

/**
 * @description Finds a model of the catalog by id. Returns null for unknown ids.
 */
const PrinterModelInfo *find_printer_model(const std::string &id);

/**
 * @description Live state of a printer, in the common model of every driver.
 */
struct PrinterStatus {
  // "connecting", "offline", "idle", "printing", "paused", "finished", "stopped", "error"
  std::string state = "connecting";
  std::string file;  // path of the current or last job on the printer
  double progress = NAN, elapsed = NAN, remaining = NAN;  // percent, seconds
  int layer = -1, layers = -1;
  double nozzle = NAN, nozzle_target = NAN, bed = NAN, bed_target = NAN;
  int light = -1;  // -1 unknown, 0 off, 1 on
  std::string error;
};

/**
 * @description A finished print of the printer's history.
 */
struct PrinterJob {
  std::string path;
  double when = NAN;      // start, Unix seconds
  double duration = NAN;  // seconds
  std::string result;     // "done", "failed" or "" when unknown
};

/**
 * @description Everything a driver knows about the printer.
 */
struct PrinterData {
  PrinterStatus status;
  std::vector<PrinterJob> history;  // newest first
  std::set<std::string> files;      // paths of the files stored on the printer
  bool has_files = false;           // `files` was received at least once
};

/**
 * @description Display parts of a sliced file name: "deck_saver.stl_PLA_4h51m8s.gcode" gives the name
 * "deck saver", the material "PLA" and the slicer's estimate (17468 s).
 */
struct PrinterFileName {
  std::string name;
  std::string material;
  double estimate = NAN;
};

class PrinterDriver;

/**
 * @description Protocol state machine of the connection to one 3D printer. Thread-safe.
 */
class PrinterSession {
 public:
  using Sender = std::function<void(const std::string &text)>;
  using Fetcher = std::function<void(const std::string &url, std::function<void(std::string body)> done)>;

  // Side of the thumbnails, in pixels
  static constexpr int THUMB_SIZE = 64;
  // Color the thumbnails' transparency is blended over: widgets draw them in a box of this color
  static constexpr uint32_t THUMB_BACKGROUND = 0x1B2234;
  // Entries of printer/files
  static constexpr int MAX_RECENT = 12;

  explicit PrinterSession(Bus &bus);
  ~PrinterSession();

  /**
   * @description Chooses the printer (from the settings) and resets the state.
   * @param model Catalog id, e.g. "creality_cr10se". Empty or unknown turns the session off.
   * @param host IP address or name of the printer.
   * @param port 0 for the driver's default port.
   * @returns The WebSocket URL the platform must open, or an empty string when there is no printer.
   */
  std::string configure(const std::string &model, const std::string &host, int port);

  /**
   * @description Sets the function that writes a text frame on the socket. Called without any lock held.
   */
  void set_sender(Sender sender);

  /**
   * @description Sets the HTTP download used for thumbnails. `done` may run on any thread.
   */
  void set_fetcher(Fetcher fetcher);

  /**
   * @description The socket is open: the driver asks for the state and the files.
   */
  void on_open();

  /**
   * @description The socket closed: the printer shows as offline until it reconnects.
   */
  void on_close();

  /**
   * @description Handles one complete text frame.
   */
  void on_message(const std::string &text);

  /**
   * @description Sends a command: "pause", "resume", "stop", "start" (arg = file path) or "light" (arg = "on"
   * or "off").
   * @returns False when the printer is not connected or does not support the command.
   */
  bool command(const std::string &name, const std::string &arg = "");

  /**
   * @description Downloads and decodes a file's thumbnail once, then publishes it on "printer/thumb/<path>".
   * Downloads run one at a time.
   */
  void request_thumbnail(const std::string &path);

  /**
   * @description State for the REST API (GET /api/printer): the "printer" bus value.
   */
  std::string status_json();

  /**
   * @description Splits a sliced file name (path or not) into its display parts.
   */
  static PrinterFileName describe_file(const std::string &path);

 private:
  void publish_status_locked();
  void publish_files_locked();
  void fetch_next_thumbnail();
  void flush();
  cJSON *status_tree_locked() const;

  Bus &bus_;
  std::mutex mutex_;
  std::unique_ptr<PrinterDriver> driver_;
  const PrinterModelInfo *model_ = nullptr;
  std::string host_;
  Sender sender_;
  Fetcher fetcher_;
  std::vector<std::string> outbox_;
  bool connected_ = false;
  PrinterData data_;
  // Thumbnails already requested (done or failed), and the ones waiting for a download
  std::set<std::string> thumbs_requested_;
  std::vector<std::string> thumbs_queue_;
  bool thumb_busy_ = false;
  unsigned generation_ = 0;  // bumped by configure(): late downloads of the previous printer are dropped
};

}  // namespace dm
