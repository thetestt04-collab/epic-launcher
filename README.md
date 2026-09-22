# EpicGamesLauncher

EpicGamesLauncher is a Windows packet-delay utility built with C++17, IUP, and WinDivert.

## Build

Requirements:

- 64-bit Windows
- MSYS2 MinGW-w64 GCC (the default script expects `C:\msys64\mingw64\bin`)
- PowerShell 5 or newer and the Windows `curl.exe` command

Install the pinned, checksum-verified dependencies and build:

```powershell
powershell -ExecutionPolicy Bypass -File .\setup-deps.ps1
.\build.bat
```

The deployable files are written to `build`. Run `EpicGamesLauncher.exe` as Administrator because WinDivert requires elevation.

## Architecture

- `divert.cpp` owns the WinDivert handle and packet-processing threads.
- `packet.cpp` implements the intrusive packet queue and heap fallback.
- `packet_pool.cpp` provides the synchronized fixed-size allocation pool used by the hot path.
- `lag.cpp` buffers and releases TCP/UDP packets according to the configured delay.
- `main.cpp` owns the IUP interface, configuration, ping sampling, and application lifecycle.
- `hotkey.cpp` is the single hotkey handler: one key table drives parsing, display names, capture, and typing-key warnings.
- `update.cpp` checks a JSON version feed over HTTPS and compares it with `APP_VERSION` in `version.h`.

Third-party binaries are intentionally excluded from Git. Their versions and SHA-256 checksums are pinned in `setup-deps.ps1`.

`setup-deps.ps1` strips WinDivert to the files this 64-bit-only project uses.
Kept under `external/WinDivert-2.2.2-A`:

- `include/windivert.h` (compile time)
- `x64/WinDivert.lib` (link time)
- `x64/WinDivert.dll` and `x64/WinDivert64.sys` (copied to `build` at runtime)
- `LICENSE` (kept because the DLL/driver are redistributed)

Removed: the entire `x86` tree, all `x64/*.exe` sample tools, and the
upstream docs (`doc/`, `CHANGELOG`, `README`, `VERSION`).

## Usage

Run `EpicGamesLauncher.exe` as Administrator. Press the configured hotkey
(default `MOUSE5`) to toggle the active filter on/off for the selected game
filter. Right-click the tray icon to open the main window or exit. Latency is
sampled via ICMP and shown in the Wi-Fi indicator and status bar. The `Change`
button next to the hotkey readout rebinds the toggle key.

## Configuration

`config.json` is copied next to the executable. It contains the hotkey, ping target, and an ordered list of named WinDivert filter expressions. Invalid or missing configuration falls back to safe built-in defaults and reports the reason in the status bar.

The configured mouse hotkey accepts rapid press/release cycles without an artificial cooldown. The static Wi-Fi indicator changes strength and color with latency: green means good, amber means elevated, and red means poor or unavailable.

## Hotkey

The filter toggle hotkey defaults to `MOUSE5`. Press `Change` next to the hotkey readout to rebind it in place: press a single key, a combo with any of `CTRL`/`ALT`/`SHIFT`/`WIN` (`CTRL+K`, `ALT+INSERT`, `CTRL+ALT+M`, `WIN+G`, `CTRL+SHIFT+F24`), or mouse button 4/5 — with or without modifiers (`CTRL+MOUSE5`). Letters, digits, `F1`–`F24`, navigation keys, arrows, numpad, punctuation, `SPACE`/`TAB`/`ENTER`/`BACKSPACE`, and browser/media keys all work. `Esc` cancels. Extra mouse buttons (DPI, thumb, sniper): press the button while rebinding — if it sends a key event it binds directly; otherwise remap it to an unused key such as `F13` or `PAUSE` in your mouse vendor software, then bind that key here. The binding is written back to `config.json`, so it persists across restarts. Single letter/digit keys toggle even while typing elsewhere — prefer a combo or mouse button if that gets in the way.

## Updates

`src/version.h` holds `APP_VERSION`. Point `updateCheckUrl` at a JSON version
feed and `updatePageUrl` at the matching releases page, so the Functions
panel's Check button (plus a silent check at startup) just works. The updater
accepts feeds such as `{"version": "1.1.0", "url": "https://example.com/releases"}`.
Leave `updateCheckUrl` empty to disable update traffic. `.\test.bat` covers the
version parsing in `tests/update_tests.cpp`.
