#pragma once

#include <string>

namespace desktops
{
    // Knowledge base for starting applications on a named desktop: resolves user
    // input along the standard Run ladder (folder, URL, executable path, PATH,
    // App Paths, file association) and launches it there.
    //
    // Stateless and parameterized by desktop name: no window, no thread affinity,
    // so a caller on any desktop can use it directly.
    class Runner
    {
    public:
        void Launch(const std::wstring& desktop, const std::wstring& rawInput);

    private:
        void start(const std::wstring& desktop, const std::wstring& command);

        void report(const std::wstring& detail);
    };
}
