#include "common.h"

int main()
{
    const bool desktop = DesktopTests();
    const bool toolhelp = ToolhelpTests();
    const bool helpers = Win32HelpersTests();
    return (desktop && toolhelp && helpers) ? 0 : 1;
}
