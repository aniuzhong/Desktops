# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

### Changed

### Fixed

## [0.2.0] - 2026-09-17

### Added
- Minimized windows can be listed and restored from the dock's right-click menu

### Changed
- Release pipeline: the test suite now runs as a gate (`ctest`), and the tag's version must equal `project()` and `vcpkg.json` before anything is published
- Released artifact carries the version in its file name (`Desktops-<version>.exe`)
- The session id is read once through `wilx::GetCurrentSessionIdNoThrow` instead of a local wrapper; the instance pipe and the panel's tag are built from it

### Fixed
- One `QApplication` per dock process: a second one silently replaced `qApp` and was destroyed after Qt had torn down - that was the dock's exit AV
- Log rotation could lose the backup while another process was rotating, and log lines no longer carried their category

## [0.1.4] - 2026-09-16

### Added
- Version resource in the exe; `project(Desktops VERSION ...)` in CMakeLists.txt is its single source

### Changed
- Release pipeline: the vcpkg binary cache moved from `actions/cache` to a NuGet feed on GitHub Packages, and the runner is pinned to `windows-2025`

### Fixed
- Release notes were never extracted from the CHANGELOG (the regex did not match across lines), so every release shipped the fallback text
- A `workflow_dispatch` run on a branch created a release named after the branch

## [0.1.3] - 2026-09-15

### Added
- Wallpaper on every managed desktop, taken from the Windows desktop image

### Changed
- Dock is a full-width bar; its colours live in `resources/dock.qss`

### Fixed
- Switching to a desktop with a non-ASCII name failed, and its windows had no taskbar icon
- Desktop creation failed when Windows Terminal is the default terminal
- HICON destroyed twice, and a second dock installed on the same desktop
- Wallpaper could end up above the dock bar

## [0.1.2] - 2026-09-11

### Fixed
- CMD and NotePad launches carry their own path spelling: each spelling has its own trap on this machine

## [0.1.1] - 2026-09-10

### Added
- PowerShell, CMD, NotePad and Run buttons in the dock

## [0.1.0] - 2026-09-10

### Added
- First release: a panel on the Default desktop and one dock process per extra desktop, with create and switch

[Unreleased]: https://github.com/aniuzhong/Desktops/compare/v0.2.0...HEAD
[0.2.0]: https://github.com/aniuzhong/Desktops/compare/v0.1.4...v0.2.0
[0.1.4]: https://github.com/aniuzhong/Desktops/compare/v0.1.3...v0.1.4
[0.1.3]: https://github.com/aniuzhong/Desktops/compare/v0.1.2...v0.1.3
[0.1.2]: https://github.com/aniuzhong/Desktops/compare/v0.1.1...v0.1.2
[0.1.1]: https://github.com/aniuzhong/Desktops/compare/v0.1.0...v0.1.1
[0.1.0]: https://github.com/aniuzhong/Desktops/releases/tag/v0.1.0
