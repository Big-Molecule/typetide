# TypeTide — Agent Guide

System-wide **AI translation + inline rewrite** for Windows (this fork is Windows-only; the upstream macOS app is not maintained here):

- `windows/` — Win32 C++20 tray app (CMake, no third-party deps)

It does two things:

- **Read**: select text → shortcut / floating icon / auto → translation popup.
- **Write**: type in native language → shortcut → translated & replaced in place.

## Architecture

```
Shortcut / SelectionMonitor ─► SelectionCapture ─► Translator ─► provider
                                      │                         │
                              Popup / TextReplacer  ◄──── streamed result
```

Backend: any OpenAI-compatible endpoint
(`/chat/completions`, SSE streaming; model list via `GET {base}/models`).
10 languages, native/foreign pair, per-shortcut direction (auto-detect or
fixed), 4 rewrite styles.

## Windows (`windows/`)

Pure Win32 + WinHTTP + UI Automation, C++20, CMake + Ninja, zero third-party
dependencies (tiny JSON parser included in `src/Json.h`).

- `src/App.*` — orchestrator, owns the message-only window that receives hotkeys and worker notifications; also dispatches hook events (`hooks::MouseMessage` / `KeyMessage`) and paces streaming UI updates on a 33 ms timer.
- `src/SelectionCapture.*` — UI Automation TextPattern selection + synthetic Ctrl+C clipboard fallback (clipboard always restored); also returns the selection's screen rect for popup anchoring.
- `src/TextReplacer.*` — synthetic Ctrl+V paste (undo-safe), clipboard restored after.
- `src/Translator.*` — provider interface; OpenAI SSE streaming over WinHTTP; in-memory LRU cache; `ListCloudModels`/`FetchCloudModelsAsync` for the `/models` picker.
- `src/Language.*` — 10 languages, prompts, detection (script ranges + Latin stopword scoring).
- `src/PopupWindow.*` — streaming translation popup (Copy / Replace, Esc / click-outside dismiss). Skips `SetWindowPos` when its size is unchanged and steps height in 24 px increments while streaming.
- `src/Hooks.*` — shared low-level mouse/keyboard hooks on a dedicated hook thread; callbacks only queue events for the main thread (never do work there). Escape swallowing is decided synchronously via lock-free atomic gates.
- `src/SelectionIcon.*` / `src/SelectionMonitor.*` — floating icon & auto-translate triggers (WH_MOUSE_LL).
- `src/SettingsWindow.*` — native settings dialog (tabs: General, Backend, Language, Shortcuts, Behavior, About). The cloud model control is an editable combo plus a Fetch models button.
- `src/Settings.*` — JSON settings at `%APPDATA%\TypeTide\settings.json`.
- `src/TrayIcon.*`, `src/Hotkeys.*`, `src/LaunchAtLogin.*` (HKCU Run key), `src/UpdateChecker.*` — repo comes from the `TYPETIDE_UPDATE_REPO` CMake cache variable.

### Conventions & gotchas

- All UI on the main thread, and the main thread NEVER sleeps (a stalled message loop gets WH_*_LL hooks silently removed). Waiting work (clipboard-fallback capture, paste replace, HTTP) lives on worker threads posting `WM_APP_*` messages with heap payloads (receiver frees).
- Workers must not read `Settings::shared()` — snapshot config on the main thread and pass it in (see `BackendConfig` in Translator.cpp, `FetchInstalledAsync`).
- Paste replace uses clipboard delayed rendering (WM_RENDERFORMAT = "target consumed the paste"); settings saves are atomic (tmp+rename); crashes dump minidumps to `%APPDATA%\TypeTide\crashes`.
- `RegisterHotKey` fails if another app owns the combo — surface it, don't crash.
- `App` answers `WM_CLOSE` by quitting through the same path as the tray "Exit" item, so installers (Restart Manager) can close it automatically.
- `TypeTide.rc` and `TypeTide.manifest` must stay pure ASCII — the RC compiler reads UTF-8-no-BOM files in the ANSI codepage and any non-ASCII byte turns into mojibake in the version info (which the installer's close-applications list displays).
- Plain-HTTP OpenAI-compatible endpoints are allowed here (upstream restricts to https/loopback) — keep the missing-key requirement for remote endpoints.

### Build

```powershell
cd windows
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
```

## Releases

One version: the root `VERSION` file, synced by
`scripts/bump-version.sh` (default patch+1; `--minor` / `--major` / explicit).
It writes windows CMakeLists/rc/manifest — when touching the manifest remember
only the app's own assemblyIdentity may change; the Common-Controls
`version="6.0.0.0"` must stay or the exe fails SxS at start.

- `scripts/release-windows.ps1` (on Windows) → builds via
  `windows/scripts/build-release.ps1` (Release exe + self-test gate + Inno
  Setup installer from `windows/installer/TypeTide.iss` + portable zip), then
  `gh release create`-if-missing + upload. Needs `gh auth login` once.
  `UpdateChecker` reads `releases/latest` at the repo in `TYPETIDE_UPDATE_REPO`
  (default `Big-Molecule/typetide`).

Keep `README.md` free of the app's release version — link to `../../releases`,
refer to artifacts as `TypeTide-Setup-x.y.z.exe` style, never a pinned literal
version. Platform/dependency versions (Windows 10+, C++20) are fine.

## Current Product Priorities

- Use **TypeTide** consistently in repository metadata, update feeds, documentation, UI, and package names; treat Saypick only as historical naming.
- Track selection-capture success, replacement success, time to first translated token, total latency, cancellation, and provider failure locally during development without collecting user text.
- Maintain tests for language direction, smart detection, prompt construction, streaming assembly, and clipboard restoration.
- Keep the streaming pipeline responsive: hook callbacks stay cheap, translation deltas stay batched — no per-token UI messages.
