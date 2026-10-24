# VolumeLift

Free, open-source system-wide volume booster for Windows 10/11 (x64).

VolumeLift raises the loudness of **everything** your PC plays — up to **+24 dB** — with a
built-in peak limiter so the extra volume doesn't turn into distortion. It runs inside the
Windows audio engine as an *Audio Processing Object* (APO), so it works with every app,
without per-app hooks or virtual audio devices.

> **Status: early preview (v0.x).** The DSP is unit-tested in CI; installation onto real audio
> devices has had limited testing. Read [Risks](#risks) before installing.

## Install

1. Download `VolumeLift-win-x64.zip` from [Releases](../../releases) and extract it anywhere.
2. Run `VolumeLift.exe` and click **Install driver** (asks for administrator rights).
   Windows Audio restarts for a second.
3. Move the slider. Close the window to keep VolumeLift in the tray.

To remove it, click **Uninstall driver**. That restores every registry value and permission
the installer changed, then deletes the installed files.

Command line (elevated): `VolumeLift.exe --install` / `VolumeLift.exe --uninstall`.
The install log is written to `%ProgramData%\VolumeLift\install.log`.

## How it works

```
any app ─► Windows audio engine (audiodg.exe) ─► VolumeLiftApo.dll ─► speakers
                                                     ▲
VolumeLift.exe ── HKLM\SOFTWARE\VolumeLift ──────────┘ (gain, limiter, on/off)
```

- **`VolumeLiftApo.dll`** is an endpoint effect (EFX) APO. For each block of float32 audio it
  applies a smoothed gain (20 ms glide) and then a peak limiter (instant attack, 150 ms release,
  -0.3 dBFS ceiling). With the limiter off, samples are clipped at full scale. Settings arrive
  through a registry change notification on a worker thread, so the real-time path never
  blocks or allocates.
- **`VolumeLift.exe`** is the tray app and installer. Installing:
  - copies both files to `%ProgramFiles%\VolumeLift` and registers the COM class and APO;
  - for each active playback device, sets VolumeLift as the endpoint effect in
    `MMDevices\Audio\Render\{device}\FxProperties` and turns audio enhancements on, after
    backing up the original values and key permissions to `HKLM\SOFTWARE\VolumeLift\Backup`;
  - sets `DisableProtectedAudioDG=1` so the unsigned APO can load;
  - restarts the Windows Audio service.

## Risks

- **Replaces the device's own endpoint effect.** If your sound driver ships its own effects
  (e.g. Realtek, Dolby, DTS), the endpoint one is switched off while VolumeLift is installed.
  Uninstall restores it.
- **Don't combine with another booster or Equalizer APO** on the same device.
- **Loud.** +24 dB is 16× the amplitude. Start low, especially with headphones.
- **Unsigned binaries.** SmartScreen may warn about them. Build from source if you prefer;
  the release zip includes its SHA-256.
- New devices plugged in after install need **Install driver** again.

## Build from source

Requires Visual Studio 2022 (Desktop C++ workload, Windows 10/11 SDK) and CMake 3.21+.

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Outputs: `build/Release/VolumeLift.exe` and `build/Release/VolumeLiftApo.dll`. CI builds every
push, and tags named `v*` publish a release.

## Project layout

| Path | What |
|---|---|
| `src/dsp/Booster.h` | Gain + limiter DSP (header-only, real-time safe) |
| `src/apo/` | APO COM object, class factory, registration, settings watcher |
| `src/app/` | Tray UI and the installer |
| `tests/` | Offline DSP tests |

## Provenance

VolumeLift is an independent implementation written from Microsoft's public APO documentation
and standard DSP techniques. It contains no code, data or assets from any other volume-booster
product.

## License

[MIT](LICENSE)
