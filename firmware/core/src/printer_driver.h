// Protocol drivers of the 3D printer session (private to core). One driver per family of protocols,
// named in protocol/printers.json ("driver").
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "dm/printer.h"

namespace dm {

/**
 * @description Translates one printer protocol to the common model. Called by PrinterSession with its lock
 * held: a driver never calls back into the session.
 */
class PrinterDriver {
 public:
  // What a frame changed (bit mask returned by on_message)
  enum Change : unsigned { STATUS = 1, FILES = 2 };

  virtual ~PrinterDriver() = default;

  /**
   * @description WebSocket URL of the printer.
   */
  virtual std::string url(const std::string &host, int port) const = 0;

  /**
   * @description Commands the driver supports, as sent to PrinterSession::command(). "calibrate" means the
   * printer can skip the bed calibration before a print.
   */
  virtual std::vector<const char *> caps() const = 0;

  /**
   * @description Frames to send once the socket is open (state, files and history requests).
   */
  virtual void on_open(std::vector<std::string> &out) = 0;

  /**
   * @description Parses one frame into `data`. Frames to send back (heartbeat answers…) go to `out`.
   * @returns A mask of Change values.
   */
  virtual unsigned on_message(const std::string &text, PrinterData &data, std::vector<std::string> &out) = 0;

  /**
   * @description Asks for the history and the file list again (after a print ends).
   */
  virtual void refresh_files(std::vector<std::string> &out) = 0;

  /**
   * @description Builds the frames of a command (see PrinterSession::command()).
   * @returns False when the command is not supported.
   */
  virtual bool command(const std::string &name, const std::string &arg, std::vector<std::string> &out) = 0;

  /**
   * @description HTTP URL of a file's thumbnail (a PNG), or an empty string when the printer has none.
   */
  virtual std::string thumbnail_url(const std::string &host, const std::string &path) const = 0;
};

/**
 * @description Creality printers with the stock firmware (Creality OS): WebSocket on port 9999, the protocol
 * of Creality Print. See protocol/README.md, "3D printer".
 */
std::unique_ptr<PrinterDriver> make_creality_driver();

}  // namespace dm
