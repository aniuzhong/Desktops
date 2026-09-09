#include "logging.h"

#include <filesystem>
#include <format>

#include <shlobj.h>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/rotating_file_sink.h>

#include "wilx/win32_helpers.h"

namespace {

void write(spdlog::level::level_enum level, std::wstring_view message) {
    spdlog::log(level, "{}", wilx::TryGetUtf8String(message));
}

}  // namespace

namespace desktops {
namespace log {

void Init() {
    try {
        wil::unique_cotaskmem_string app_data;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, app_data.put()))) {
            return;
        }
        const std::filesystem::path log_directory = std::filesystem::path(app_data.get()) / L"VirtualDesktop" / L"logs";
        std::filesystem::create_directories(log_directory);
        auto file_sink = std::make_shared<spdlog::sinks::rotating_file_sink_mt>(wilx::TryGetUtf8String((log_directory / L"virtualdesktop.log").wstring()), 1024 * 1024, 1);
        auto logger = std::make_shared<spdlog::logger>("desktops", std::move(file_sink));
        logger->set_pattern("[%Y-%m-%d %T.%e] [P%P T%t] [%l] %v");
        logger->flush_on(spdlog::level::info);
        spdlog::set_level(spdlog::level::debug);
        spdlog::set_default_logger(std::move(logger));
    } catch (const std::exception&) {
        //
    }
}

void Debug(std::wstring_view message) {
    write(spdlog::level::debug, message);
}

void Info(std::wstring_view message) {
    write(spdlog::level::info, message);
}

void Warn(std::wstring_view message) {
    write(spdlog::level::warn, message);
}

void Err(std::wstring_view message) {
    write(spdlog::level::err, message);
}

void Err(std::wstring_view message, DWORD last_error) {
    if (last_error) {
        write(spdlog::level::err, std::format(L"{} (error {}: {})", message, last_error, wilx::TryGetWin32ErrorMessage(last_error)));
    } else {
        Err(message);
    }
}

}  // namespace log
}  // namespace desktops
