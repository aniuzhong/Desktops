# Desktops

- [Desktops](#desktops)
  - [Known Problems](#known-problems)

## Known Problems

- Input Methods are not available in non-**Default** Desktop
- Opening pwsh/cmd by hand with Windows Terminal as the default terminal lands on the Default desktop: the console is delegated to the packaged WindowsTerminal.exe, which ignores lpDesktop. The dock's CMD/PowerShell buttons avoid it by hosting through an explicit `conhost.exe <client>` launch, which bypasses the delegation and creates the window on the target desktop
- Huorong's behavior engine (火绒) blocks `notepad.exe` launched with a
  backslash path onto a non-default desktop: `NtCreateUserProcess` never
  returns, every dock thread is suspended from outside, and the dock is
  silently terminated 35-76s later. The forward-slash spelling evades the
  pattern - `Dock::launchNotePad` encodes this; do not "clean it up".
  blocks `CreateProcessW` for ~30s before proceeding.
