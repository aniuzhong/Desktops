#include <format>

#include <wil/resource.h>

#include "logging.h"
#include "panel.h"
#include "wilx/win32_helpers.h"

namespace {

constexpr wchar_t kMutexName[]   = L"Local\\VirtualDesktop_{44D28BCA-7F46-4af2-A1FF-36EE0DAC7CD2}";
constexpr wchar_t kGoHomeEvent[] = L"Local\\VirtualDesktop_{44D28BCA-7F46-4af2-A1FF-36EE0DAC7CD2}-go-home";

}  // namespace

int APIENTRY wWinMain(_In_ HINSTANCE instance, _In_opt_ HINSTANCE, _In_ LPWSTR, _In_ int) {
    desktops::log::Init();
    desktops::log::Info(std::format(L"Application starting, pid {}", GetCurrentProcessId()));

    // Session-scoped single instance. A second launch — from any desktop — is
    // the go-home recall: signal the resident panel and exit.
    wil::unique_mutex_nothrow instance_mutex;
    bool already_exists = false;
    if (!instance_mutex.try_create(kMutexName, 0, MUTEX_ALL_ACCESS, nullptr, &already_exists)) {
        desktops::log::Warn(std::format(L"mutex unavailable (error {}), exiting", GetLastError()));
        return 0;
    }
    if (already_exists) {
        desktops::log::Info(L"instance already running; signaling go-home");
        wil::unique_handle go_home(OpenEventW(EVENT_MODIFY_STATE, FALSE, kGoHomeEvent));
        if (go_home) {
            SetEvent(go_home.get());
        }
        return 0;
    }

    // The panel lives on Default only. Launching from elsewhere is a refusal,
    // never a relocation — the user sees the box on the desktop they are on.
    const std::wstring thread_desktop = wilx::TryGetThreadDesktopName();
    if (_wcsicmp(thread_desktop.c_str(), L"Default") != 0) {
        desktops::log::Warn(std::format(L"launched on '{}', refusing", thread_desktop));
        MessageBoxW(nullptr,
                    L"Virtual Desktop runs on the Default desktop.\nSwitch back to "
                    L"Default and start it there.",
                    L"Virtual Desktop", MB_ICONINFORMATION | MB_TOPMOST | MB_TASKMODAL);
        return 0;
    }

    wil::unique_handle go_home(CreateEventW(nullptr, FALSE, FALSE, kGoHomeEvent));
    if (!go_home) {
        desktops::log::Err(L"[startup] go-home event creation failed", GetLastError());
        return -1;
    }

    const int exit_code = desktops::panel::run(instance, go_home.get());
    desktops::log::Info(std::format(L"Exiting with {}", exit_code));
    return exit_code;
}