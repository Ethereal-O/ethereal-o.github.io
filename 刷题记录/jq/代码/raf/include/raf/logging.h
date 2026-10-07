// Optional logging hook. <raf/raf.h> stays free of spdlog; an application that
// wants raf's diagnostics in its own sink includes this header instead.
#pragma once

#include <memory>

namespace spdlog {
class logger;
}

namespace raf {

// Routes raf's log records to `logger`. Passing nullptr restores the default
// logger, which writes warnings and above to stderr.
void setLogger(std::shared_ptr<spdlog::logger> logger);

// The logger raf is currently using.
std::shared_ptr<spdlog::logger> logger();

}  // namespace raf
