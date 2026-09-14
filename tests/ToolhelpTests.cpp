#include <windows.h>

#include <wilx/toolhelp.h>

#include "common.h"

bool ToolhelpTests()
{
    bool foundSelfProcess = false;
    wilx::for_each_process([&](const PROCESSENTRY32W& entry) {
        foundSelfProcess = foundSelfProcess || entry.th32ProcessID == GetCurrentProcessId();
        return true;
    });
    CHECK(foundSelfProcess);

    bool foundSelfThread = false;
    wilx::for_each_thread([&](const THREADENTRY32& entry) {
        foundSelfThread = foundSelfThread || entry.th32ThreadID == GetCurrentThreadId();
        return true;
    });
    CHECK(foundSelfThread);

    // Stopping the snapshot walk early is the only control flow available.
    size_t visited = 0;
    wilx::for_each_process([&](const PROCESSENTRY32W&) -> bool { ++visited; return false; });
    CHECK(visited == 1);

    return true;
}
