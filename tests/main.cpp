#include "common.h"

int main()
{
    const bool desktop = DesktopTests();
    const bool toolhelp = ToolhelpTests();
    return (desktop && toolhelp) ? 0 : 1;
}
