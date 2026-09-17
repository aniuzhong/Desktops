# wilx

Header-only, WIL-style extensions for Virtual Desktop.

## House rules

- Build on wil's **public API only**; `wil::details` is reference material,
  never a dependency — copy and own its evolution if ever truly needed.
- C++23 and up: no historical back-compat layers.
- A helper enters wilx only if it passes all three checks: no business
  vocabulary in names or signatures, no touching app globals, and usable
  as-is by any Win32 program. wilx provides primitives and failure semantics;
  the app provides policy (caching, confirmation dialogs, relaunch logic).
- Grow on demand: one failure regime until a caller needs another; no
  speculative overloads.

## Files

| File | Theme |
|---|---|
| `desktop.h` | The window station / desktop subsystem: its three user-object families (`for_each_desktop`, `for_each_desktop_window`, `for_each_window_station`) share the one Enum\* trampoline in `wilx::details` — the shape wil's `windowing.h` uses for its three window enumerators — plus the name queries that read a desktop's identity back and the one that places a process in its session |
| `toolhelp.h` | System process/thread iteration (`for_each_process`, `for_each_thread`) — snapshot iteration machinery |

File names follow wil's two patterns only: a **singular domain noun** (`desktop.h`, mirroring
`filesystem.h` / `registry.h` / `windowing.h`) or **`<domain>_helpers.h`** — the second tier over a
domain, whatever number its noun is (`registry_helpers.h` over `registry.h`, also `rpc_helpers.h`,
`token_helpers.h`, `win32_helpers.h`). A bare plural is neither.

There is no drawer: wilx is two domain headers. Wil's `win32_helpers.h` exists because it holds
dozens of unrelated single-pattern helpers; a lone stray helper belongs to the domain it serves, and
a `<domain>_helpers.h` of its own waits until the domain has a second tier to hold.

Machinery stays with the API family that owns it, in `details` (as in wil's `windowing.h`); it earns
its own header only when a *second* family needs it — the same grow-on-demand rule as everything else.

## Naming grammar

Every new name must answer these questions; deviations are declared in the
header that owns the name.

| Question | Encoding |
|---|---|
| Knowledge source | `PascalCase` = Win32 vocabulary (contract follows the API's docs); `lowerCamel` = invented abstraction (contract lives in this header) |
| Types are their own case | `unique_*` / `shared_*` follow wil's RAII spelling: lower_snake always, whatever the functions around them do |
| Failure contract | `Get*` throws on failure; `TryGet*` fail-soft, failure returned as data; `*NoThrow` / `*_nothrow` = exceptions banned, failure comes back as an `HRESULT`. No `FailFast` variant exists yet: wil ships them (`GetCurrentProcessExecutionOptionFailFast`) and wilx grows one only when a caller wants it |
| Availability | Every helper lives inside the `WINAPI_FAMILY_PARTITION` its API really has (see Availability below) |
| Result that must be used | `[[nodiscard]]` only when discarding the result discards the whole call — conversions, comparisons, RAII acquisitions, the way wil marks `compare_string_ordinal` and leaves `GetModuleFileNameW` bare |
| Ownership | `unique_*` / `shared_*` = RAII; the handle type encodes the deleter |
| Shape | `for_each_*` = callback-driven algorithm; callback returns void (continue), bool (false stops), or HRESULT (S_OK continues) |
| API mirroring | The `W` suffix is kept iff the wrapped API has W/A duality and the function is (a narrowing of) that API (`TrySearchPathW`); narrowings and multi-API composites are descriptive without `W` (`TryGetThreadDesktopName` — GetThreadDesktop + GetUserObjectInformationW) |
| Placement | A theme header is earned by machinery or mass; a single-pattern helper lives in the domain header it belongs to |

## Failure regime

`TryGet*` is total fail-soft: empty result == failure (invalid handle,
insufficient rights, OOM), never an exception. Deliberately broader than wil's
TryGet family, which still surfaces unexpected failures via HRESULT or throw.
The `*NoThrow` cores beside them are wil's error-code currency exactly as wil
documents it: failure comes back as an **HRESULT**, always `HRESULT_FROM_WIN32`
of a real Win32 code, so `HRESULT_CODE` recovers what `GetLastError` would have
said (`ERROR_INVALID_HANDLE` stands in when these user-object APIs failed
without setting last error at all). Callers that must distinguish failures read
that HRESULT; `TryGet*` stays the fail-soft default. The same seam exists in the
`for_each_*` enumerators: a denied enumeration is indistinguishable from an
empty one; grow a failure-carrying variant only when a caller truly needs it.

The exception regime is present too, on the routines that have any caller: the
`GetThreadDesktopName` query throws (the `GetFileInfoNoThrow` / `GetFileInfo` pair), and
`for_each_desktop` / `for_each_desktop_window` / `for_each_window_station` are
the throwing siblings of the `*_nothrow` enumerators. Both sit behind
`WIL_ENABLE_EXCEPTIONS`, like wil's own exception-based routines — an
exception-free build simply does not see them.

Handing a throwing callback to a `*_nothrow` enumerator is a contract
violation, not a degraded mode: the exception would have to cross the C
callback boundary, which is undefined behaviour, so the trampolines carry
`WI_NOEXCEPT` and the process terminates. Measured: wil's
`for_each_window_nothrow` and wilx's `for_each_desktop_nothrow` both terminate
on MSVC — the annotation only makes an already-fatal mistake defined.

## Availability

Everything in wilx is `WINAPI_PARTITION_DESKTOP`, because every API it wraps is:
window stations, desktops, their windows, their names, and Toolhelp snapshots
are all desktop-only. Should a genuinely partition-agnostic helper arrive (wil's
`FormatMessageW`/`WideCharToMultiByte`-based ones are not partitioned, and
neither would ours be), it stands outside the guard. Nothing in wilx claims
availability it has not checked.

## Deliberate deviations from wil

- `std::` instead of wil's internal `wistd::`
- C++23 concepts instead of `always_false` + `static_assert`
- No `W` suffix on invented composite names (reserved for API mirrors with A/W duality)
- Total fail-soft `TryGet*` (wil's still surfaces unexpected failures)
- `toolhelp.h` iterates inline (no C trampoline), so callback exceptions
  propagate and there is no `*_nothrow` split — declared in that header
