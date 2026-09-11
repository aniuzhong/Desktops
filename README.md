# Desktops

- [Desktops](#desktops)
  - [Known Problems](#known-problems)

## Known Problems

- Input Methods are not available in non-**Default** Desktop
- Opening pwsh with Windows Terminal by default lands in a path where desktop creation fails
- Huorong's behavior engine (火绒) blocks `notepad.exe` launched with a
  backslash path onto a non-default desktop: `NtCreateUserProcess` never
  returns, every dock thread is suspended from outside, and the dock is
  silently terminated 35-76s later. The forward-slash spelling evades the
  pattern - `Dock::launchNotePad` encodes this; do not "clean it up".
  blocks `CreateProcessW` for ~30s before proceeding.
