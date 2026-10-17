# Rodip

Rodip is a Linux instant replay recorder. It continuously keeps a rolling series of short video/audio segments while the application is open and joins the most recent configured interval into an MP4 file when requested.

## Current implementation

- Qt 6 desktop interface with display, audio source, replay duration from 5 seconds to 15 minutes, frame rate, buffer location, encoder, hotkey and output-folder settings.
- X11 screen capture via FFmpeg `x11grab`.
- Wayland screen capture through the XDG Desktop Portal ScreenCast API and PipeWire.
- Optional system audio capture from the current default output, plus selectable microphone capture through PulseAudio or PipeWire's PulseAudio compatibility layer.
- Rolling MPEG-TS segment buffer in `/dev/shm` (RAM-backed when available) or the user's cache directory.
- Automatic encoding prefers NVIDIA NVENC, then VAAPI when supported by the local FFmpeg and drivers, and falls back to low-CPU software H.264. Encoders can also be selected manually.
- User-configurable global save hotkey (default `Ctrl+Alt+Z`, interpreted using physical QWERTY key positions) on X11 and Wayland through the XDG Desktop Portal. Settings persist between runs.

The application is independent of GPU vendor when using software encoding. Hardware encoders require a compatible driver and FFmpeg build. The VAAPI path currently assumes `/dev/dri/renderD128`; systems with multiple render nodes may need a later device selector.

## Requirements

- C++20 compiler and CMake 3.21+
- Qt 6 Widgets and Qt DBus (6.4 or newer)
- PipeWire development library (`libpipewire-0.3`)
- A desktop session providing XDG Desktop Portal ScreenCast and GlobalShortcuts backends for Wayland capture/hotkeys
- FFmpeg with `x11grab`, `pulse`, `libx264`, and the codecs/devices you want to use
- `pactl` for enumerating audio sources
- X11 development headers for X11 global hotkeys

On Debian/Ubuntu, the typical packages are `build-essential cmake qt6-base-dev libx11-dev libpipewire-0.3-dev ffmpeg pulseaudio-utils`.

## Build and run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/rodip
```

## Platform status

X11 and Wayland use separate capture paths. Wayland displays are selected in the desktop portal's permission dialog; the screen selector in Rodip is used by the X11 backend. The Wayland portal and its compositor backend must be installed and running. The implementation currently captures one selected monitor stream at a time.

The temporary buffer can reside in RAM-backed `/dev/shm` or on disk. Rodip falls back to disk when `/dev/shm` does not have enough free space for the configured buffer. VRAM is not exposed as a general-purpose buffer by Linux graphics stacks; GPU encoding accelerates compression but does not make the rolling buffer reside in VRAM. Rodip does not claim VRAM buffering.

## User data

Preferences use Qt's per-user settings storage. Buffer segments are temporary and are removed as they age out. Saved replays go to the selected folder, defaulting to `~/Videos/Rodip` where available.
