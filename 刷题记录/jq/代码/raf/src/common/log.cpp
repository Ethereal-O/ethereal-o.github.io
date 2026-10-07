#include "common/log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <atomic>
#include <mutex>
#include <vector>

namespace raf {
namespace {

std::mutex g_mutex;
std::shared_ptr<spdlog::logger> g_logger;
// Loggers that setLogger() replaced. Logging is lock-free -- it dereferences
// g_raw without taking g_mutex -- so a replaced logger has to stay alive for
// the rest of the process rather than be freed under a concurrent log call.
std::vector<std::shared_ptr<spdlog::logger>> g_retired;
std::atomic<spdlog::logger*> g_raw{nullptr};

// Built by hand rather than through spdlog::stderr_color_mt() so that raf
// never claims a name in the application's global spdlog registry.
std::shared_ptr<spdlog::logger> makeDefaultLogger() {
    auto sink = std::make_shared<spdlog::sinks::stderr_color_sink_mt>();
    auto logger = std::make_shared<spdlog::logger>("raf", std::move(sink));
    logger->set_level(spdlog::level::warn);
    return logger;
}

// Caller must hold g_mutex.
void install(std::shared_ptr<spdlog::logger> logger) {
    if (g_logger) {
        g_retired.push_back(std::move(g_logger));
    }
    g_logger = std::move(logger);
    g_raw.store(g_logger.get(), std::memory_order_release);
}

}  // namespace

void setLogger(std::shared_ptr<spdlog::logger> logger) {
    std::lock_guard<std::mutex> guard(g_mutex);
    install(logger ? std::move(logger) : makeDefaultLogger());
}

std::shared_ptr<spdlog::logger> logger() {
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_logger) {
        install(makeDefaultLogger());
    }
    return g_logger;
}

namespace detail {

spdlog::logger& log() {
    spdlog::logger* raw = g_raw.load(std::memory_order_acquire);
    if (raw != nullptr) {
        return *raw;
    }
    std::lock_guard<std::mutex> guard(g_mutex);
    if (!g_logger) {
        install(makeDefaultLogger());
    }
    return *g_logger;
}

}  // namespace detail
}  // namespace raf
