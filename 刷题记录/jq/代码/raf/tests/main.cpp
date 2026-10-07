#include <gtest/gtest.h>
#include <spdlog/spdlog.h>

#include <cstdlib>
#include <memory>

#include <raf/logging.h>

int main(int argc, char** argv) {
    ::testing::InitGoogleTest(&argc, argv);
    // Several tests put damage on disk on purpose and raf reports it at error
    // level. Keep the output clean unless a run asks to see it.
    auto logger = raf::logger();
    const char* level = std::getenv("RAF_TEST_LOG");
    logger->set_level(level != nullptr ? spdlog::level::from_str(level) : spdlog::level::off);
    return RUN_ALL_TESTS();
}
