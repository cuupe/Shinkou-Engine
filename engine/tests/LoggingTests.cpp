#include "shinkou/log/Log.h"

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>

int main() {
    const auto directory = std::filesystem::temp_directory_path() / "shinkou-engine-logging-test";
    std::error_code error;
    std::filesystem::remove_all(directory, error);

    shinkou::log::LogConfig config;
    config.directory = directory;
    config.fileName = "test.log";
    config.console = false;
    config.asynchronous = false;
    config.flushInterval = std::chrono::milliseconds::zero();
    config.level = shinkou::log::Level::Trace;
    assert(shinkou::log::initialize(config));
    assert(shinkou::log::initialized());
    SHINKOU_LOG_INFO("logging test value={}", 42);
    SHINKOU_LOG_ERROR("logging test error");
    shinkou::log::flush();

    const auto stats = shinkou::log::stats();
    assert(stats.initialized);
    assert(stats.filePath == directory / "test.log");
    std::ifstream input(stats.filePath);
    const std::string contents((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    assert(contents.find("logging test value=42") != std::string::npos);
    assert(contents.find("logging test error") != std::string::npos);

    shinkou::log::shutdown();
    assert(!shinkou::log::initialized());
    std::filesystem::remove_all(directory, error);
    return 0;
}
