#pragma once

#include <windows.h>

#include <string>
#include <string_view>

namespace desktops
{
    // Knowledge base for starting applications on a named desktop.
    //
    // The public surface is a single Launch(): it classifies the input and the
    // situation, then forwards to a private per-application recipe that knows how
    // to bring that application up in a *usable* state on that desktop.
    //
    // Stateless and parameterized by desktop name: no window, no thread affinity,
    // so a caller on any desktop can use it directly. Instances are never shared
    // between threads — the Sink decides where failures surface, so it must
    // belong to exactly one caller.
    class Runner
    {
    public:
        // Receives launch failures. Invoked on the thread that called Launch(), so
        // each sink renders on the desktop its owner lives on.
        struct Sink
        {
            virtual ~Sink() = default;

            virtual void OnLaunchFailed(const std::wstring& desktop, const std::wstring& detail) = 0;
        };

        struct LaunchResult
        {
            DWORD pid = 0;
            bool arrived = false;
        };

        explicit Runner(Sink& sink);

        LaunchResult Launch(const std::wstring& desktop, const std::wstring& rawInput);

    private:
        enum class Recipe
        {
            kUrl,
            kResolvedProgram,
            kPowerShell5,
            kPowerShell7,
            kCmd,
        };

        // Pure: input -> recipe. No I/O, no state — this is the half of the
        // knowledge base that can be tested without a desktop.
        Recipe Classify(std::wstring_view input) const;

        LaunchResult launch_directory(const std::wstring& desktop, const std::wstring& path);

        LaunchResult launch_url(const std::wstring& desktop, const std::wstring& url);

        LaunchResult launch_program(const std::wstring& desktop, const std::wstring& input);

        // Windows PowerShell 5.1: always present, hosted by conhost.
        LaunchResult launch_powershell5(const std::wstring& desktop);

        // PowerShell 7 (pwsh): may be an MSIX alias whose window belongs to
        // another process, so its arrival cannot be judged by its own pid.
        LaunchResult launch_powershell7(const std::wstring& desktop);

        LaunchResult launch_cmd(const std::wstring& desktop);

        LaunchResult start(const std::wstring& desktop, const std::wstring& command);

        void report(const std::wstring& desktop, const std::wstring& detail);

        Sink& sink_;
    };
}
