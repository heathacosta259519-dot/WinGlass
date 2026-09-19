# Changelog

All notable changes to WinGlass are documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project uses [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

Entries are added when a fix, feature or other change is **finished** — this is
not a commit-by-commit log, and not the internal development journal.

## [1.2.0] - 2026-09-19

### Added

- **Flexible per-app matching** — rules can now match a process name, window
  class, title substring, or Store/MSIX package identity (AUMID). All supplied
  matchers must match; more-specific rules take precedence over broad ones.
- **Store application picker** — the settings editor can enumerate installed
  Store applications and create rules for their package identity, avoiding the
  shared `ApplicationFrameHost.exe` process-name limitation.
- **Bilingual interface** — the resident tray process and settings editor now
  provide Simplified Chinese and English text, selectable with `ui_language`
  or inherited from the Windows display language. Diagnostics verify language
  tables and the editor verifies that English labels fit their controls.
- **Optional update check** — the resident process can check the latest GitHub
  release at startup, display a notification-area alert, and offer a download
  command. `check_updates` controls the feature; local parser and version
  comparison diagnostics are available without making a network request.
- **Backdrop Alpha lab** — `--backdrop-alpha-lab` places normal and layered
  Acrylic panes over a moving compositor surface for an explicit visual choice
  of the future adjustable-backdrop route. The paired self-test checks API
  acceptance without opening visible windows.
- **Adjustable Acrylic backdrop opacity** — `backdrop_alpha` now controls the
  layered live-Acrylic helper per focused/unfocused state, including smooth
  focus transitions, YAML/INI parsing, and the settings editor. Existing
  `glass_opacity` configurations retain their tint-density behaviour.

### Changed

- **Settings editor layout** — global settings, per-app rules and blacklist
  management are separated into tabs. Per-app rules now expose their matcher
  fields directly rather than limiting rules to executable names.
- **Release metadata** — all three executables now report version `1.2.0`.

### Fixed

- **Configuration editor data safety** — saving now creates a byte-for-byte
  `config.yaml.bak` snapshot before replacement and refuses to overwrite a
  configuration changed outside the editor. This makes fields not yet modeled
  by the editor recoverable rather than silently losing them forever.

## [1.1.0] - 2026-09-12

### Added

- **Application icon** — shipped inside all three executables and used by the
  taskbar and the notification area, with 16/32/48/64/128/256 px frames. The
  SVG and PNG next to it are design sources and are not consumed by the build.
- **Full-screen exclusion** — borderless full-screen surfaces (browser video,
  games) keep their original pixel-perfect look; the glass is reapplied the
  moment the window leaves full-screen. Configurable per state and per
  application, and switchable from the config editor.
- **Wallpaper palette extraction (experimental)** — paste a screenshot of the
  bare desktop into the config editor to get the ten most frequent colours,
  then apply one to the global tint or to every rule. Works with any wallpaper
  source, including live-wallpaper software that has no readable image file.
- `--palette-self-test` for the config editor, which prints how a clipboard
  image was decoded and which colours came out of it.
- **Version information in the executables** — all three report `1.1.0` in the
  file properties, each with its own file description and original filename.

### Changed

- **Per-application rules are now picked from a dropdown** in the running-process
  list, instead of requiring the executable name to be typed.
- **The English README is now the primary document** (`README.md`); the
  Simplified-Chinese one is `README.zh-CN.md`.
- **The build writes everything into `release`** — executables, intermediate
  files, the example configuration, `LICENSE` and a short distribution readme.
  The source tree no longer accumulates build output.

### Fixed

- **Tooltip windows are no longer targets.** Native `tooltips_class32` /
  `msctls_tooltip32` and browser classes containing `tooltip` are skipped by
  class name, without size or title heuristics.

## [1.0.0] - 2026-09-12

Initial public release.

- **Global frosted glass for Windows 11** — for every ordinary desktop window,
  an acrylic backdrop is composited by DWM underneath it, so the blur is of the
  real desktop: moving windows, video playback and animations all track live.
- **No injection, no hooking, no patching.** The only change made to a target
  window is adding the `WS_EX_LAYERED` extended style; everything else is
  WinGlass's own helper windows.
- **Tint is decoupled from blur and from the text.** Colour lives on its own
  layer below the window content, so it physically cannot blend into glyphs —
  and it can be used with blur switched off.
- **The effect survives losing focus.** It does not sit on top of the system's
  material state machine, so a window stays glassy instead of degrading into a
  flat grey slab.
- **Separate focused and unfocused parameters** with a smooth transition
  between them.
- **Three-tier rules** — `blacklist` > per-application > `global`, with
  unspecified fields inherited. `config.yaml` with hot reload in about 120 ms,
  plus a fallback `config.ini` flavour.
- **A GUI config editor** (`winglass-config.exe`) covering the master switch,
  both states' opacity, glass on/off, colour, density, animation duration and
  the blacklist.
- **Notification-area controls** — open the config editor, open the config file,
  reload the effect, toggle autostart for the current user, exit.
- **Lossless restore.** Normal exit, a crash or being killed from Task Manager
  all put every window style back, via a recovery journal and a separate
  watchdog process; a force-killed process leaves no translucent orphans behind.
- **Light on resources** — idle CPU ≈ 1% of one core, ~3.3 MB private memory.
