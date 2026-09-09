#include "runner_events.h"

#include <mutex>
#include <utility>

namespace desktops
{
    namespace
    {
        // Stands in for the Manager-owned queue until step 4 makes Manager a class.
        std::mutex g_lock;
        std::deque<RunnerEvent> g_events;
    }

    void PostRunnerEvent(HWND panel, RunnerEvent event)
    {
        {
            const std::scoped_lock lock(g_lock);
            g_events.push_back(std::move(event));
        }
        PostMessageW(panel, kWmAppEventsPending, 0, 0);
    }

    std::deque<RunnerEvent> TakeRunnerEvents()
    {
        std::deque<RunnerEvent> taken;
        const std::scoped_lock lock(g_lock);
        taken.swap(g_events);
        return taken;
    }
}
