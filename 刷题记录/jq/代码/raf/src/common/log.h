#pragma once

#include <spdlog/spdlog.h>

#include <raf/logging.h>

namespace raf::detail {

// The logger every RAF_LOG_* macro resolves against. Never null.
spdlog::logger& log();

}  // namespace raf::detail

// SPDLOG_ACTIVE_LEVEL, set from the RAF_LOG_LEVEL cache variable, compiles the
// levels below it out entirely, so the append path pays nothing -- not even
// the string literals -- for its trace calls in a default build.
#define RAF_LOG_TRACE(...) SPDLOG_LOGGER_TRACE(&::raf::detail::log(), __VA_ARGS__)
#define RAF_LOG_DEBUG(...) SPDLOG_LOGGER_DEBUG(&::raf::detail::log(), __VA_ARGS__)
#define RAF_LOG_INFO(...) SPDLOG_LOGGER_INFO(&::raf::detail::log(), __VA_ARGS__)
#define RAF_LOG_WARN(...) SPDLOG_LOGGER_WARN(&::raf::detail::log(), __VA_ARGS__)
#define RAF_LOG_ERROR(...) SPDLOG_LOGGER_ERROR(&::raf::detail::log(), __VA_ARGS__)
