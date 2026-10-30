# TypeTide for Windows

Native Win32 C++20 port of TypeTide — system-wide AI translation & inline
rewrite. WinHTTP handles networking, UI Automation captures selections, and a
bundled llama.cpp CPU runtime powers the built-in model. No Electron.

## Feature overview

| | Windows |
|---|---|
| Read · translate selection | Alt+D → popup |
| Write · rewrite in place | Ctrl+Shift+R |
| Selection capture | UI Automation TextPattern → clipboard fallback |
| Undo-safe replace | synthetic Ctrl+V |
| Backends | OpenAI-compatible API (plain HTTP allowed for LAN endpoints) |
| Model picker | Fetch models from {base}/models · hand-typed names |
| Floating icon / auto-translate on select | low-level mouse hook (dedicated hook thread) |
| Retarget language in popup | ✅ |
| Styles (Faithful/Formal/Casual/Polished) | ✅ |
| Launch at login | HKCU Run key |
| Per-app skip list | ✅ (exe name) |
| Update check | GitHub Releases (repo from TYPETIDE_UPDATE_REPO) |
| Settings | native tabbed window |
| Shortcut actions / verification | ✅ |
| First-run backend check | ✅ |
| Local privacy-safe diagnostics | ✅ |
| System light/dark settings UI | ✅ |
| Permissions needed | none |

Settings persist to `%APPDATA%\TypeTide\settings.json`
(override dir with the `TYPETIDE_DATA_DIR` env var; set `TYPETIDE_DEBUG=1` to
write `debug.log` beside it).

Cloud API presets are available for OpenAI, OpenRouter, and DeepSeek, alongside
any custom OpenAI-compatible `/chat/completions` endpoint. In normal use the API
key is stored in Windows Credential Manager; it is never written back to
`settings.json`.

The shortcut editor supports Ctrl, Shift, Alt, and the Windows key. Each of the
two shortcuts independently chooses smart/native/foreign direction and popup or
in-place replacement. Either shortcut can be cleared; keep at least one configured
for keyboard triggering.

## Build

Requirements: Windows 10+, Visual Studio 2022+ (MSVC toolset, CMake, Ninja —
all bundled with the "Desktop development with C++" workload).

```powershell
# from a VS Developer PowerShell / Command Prompt
cd windows
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build          # → build\TypeTide.exe
```

The tray icon (`assets/app.ico`) ships in the repo; regenerate it from the
brand master with `scripts\make-icon.ps1` if the branding changes.

## Test

```powershell
# unit-style self-tests (JSON, language detection, settings, shared contracts,
# privacy-safe diagnostics, prompts)
build\TypeTide.exe --selftest

# + a real streaming translation through the configured backend
build\TypeTide.exe --selftest-translate
```

End-to-end (drives the real app + a mock backend + a scratch editor window;
uses synthetic input, so keep hands off the keyboard while it runs):

```powershell
# terminal 1 — mock backend (OpenAI SSE + model list on :8199)
scripts\mock-openai.ps1

# terminal 2 — point the app at a scratch data dir with mock settings, then:
$env:TYPETIDE_DATA_DIR = "$env:TEMP\typetide-e2e"
scripts\test-e2e.ps1        # WinForms host → clipboard-fallback capture path
scripts\test-e2e-wpf.ps1    # WPF host → UI Automation TextPattern path
```

For the complete release-candidate matrix (hotkey conflicts, first-run checks,
dark mode, clipboard restoration, cloud streaming, and installer smoke
tests), follow [`TESTING.md`](TESTING.md) on a Windows machine.

## Architecture

```
main.cpp             single instance, COM init, message loop
App                  orchestrator: hotkeys, flows, thread marshaling (WM_APP_*)
SelectionCapture     UIA TextPattern selection (+ rect) → synthetic Ctrl+C fallback
TextReplacer         clipboard paste replace (undo-safe), clipboard restored
Translator           prompts · OpenAI SSE streaming · /models list · LRU cache (worker threads)
Http                 WinHTTP streaming POST / GET
PopupWindow          no-activate topmost popup: streaming text, Copy/Replace,
                     retarget menu, Esc / click-outside dismiss, dark mode, DPI
SelectionIcon        floating icon next to selection
SelectionMonitor     WH_MOUSE_LL mouse-up → UIA selection (icon / auto trigger)
SettingsWindow       tabbed native settings (instant apply)
TrayIcon / Hotkeys / LaunchAtLogin / UpdateChecker / Settings / Json / Language
```

Threading rule: all UI on the main thread, and the main thread **never
sleeps** — a stalled message loop gets the low-level hooks silently removed
by the OS. Anything that waits (clipboard-fallback capture, paste
replacement, HTTP) runs on worker threads and posts `WM_APP_*` messages with
heap payloads (receiver frees). Workers never read `Settings::shared()`
directly; config is snapshotted on the main thread and passed in.

Reliability notes:
- Paste replacement uses clipboard delayed rendering (`WM_RENDERFORMAT`) to
  know when the target actually consumed the paste before restoring the
  original clipboard.
- `settings.json` writes are atomic (temp file + rename).
- Unhandled exceptions write a minidump to `%APPDATA%\TypeTide\crashes\`
  (the 5 most recent are kept) — attach one when reporting a crash.
- The tray icon re-registers itself after an explorer.exe restart.
- Selections over 5000 characters are rejected with a friendly message.

## Known limitations

- Some apps (many Electron apps, some WinForms controls) don't expose UIA
  TextPattern — capture falls back to an invisible copy and the popup anchors
  at the cursor instead of the selection.
- `RegisterHotKey` fails if another app owns the combo; TypeTide warns once and
  you can pick a different shortcut in Settings → Shortcuts.

