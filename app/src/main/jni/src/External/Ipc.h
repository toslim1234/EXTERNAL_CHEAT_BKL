// Text command layer for external mode.
//
// The app talks to the daemon over the existing abstract socket. Binary frames
// keep carrying Request/Response; a frame with Mode == 250 carries one command
// line instead, and the reply is a plain string frame. Everything here is
// deliberately line-oriented so it can be driven from Java, a shell, or adb.
#ifndef BKL_EXT_IPC_H
#define BKL_EXT_IPC_H

#include <string>

namespace bkl {
namespace ext {

// Executes one command and returns the reply (may contain newlines).
std::string handle_command(const std::string &line);

const char *command_help();

} // namespace ext
} // namespace bkl

#endif // BKL_EXT_IPC_H
