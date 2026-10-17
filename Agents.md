# Agent notes

## Project

Rodip is a Linux instant replay recorder. The current code is a Qt 6 Widgets application with an FFmpeg subprocess backend.

## Build rules

- Use C++20 and CMake.
- Keep UI work in Qt and capture/encoding backend work in isolated components as the project grows.
- Do not add comments for obvious code. Explain non-obvious capture, timing, synchronization, and buffer logic where needed.
- Preserve GPU-vendor independence: software encoding is the baseline; VAAPI/NVENC are optional acceleration paths.
- Treat X11 and Wayland capture as separate backends. Wayland screen capture must be permissioned through XDG Desktop Portal and PipeWire.
- Do not represent VRAM as a portable storage backend. Hardware encoding and temporary buffer placement are separate settings.
- Keep user settings persistent and fail with a visible status when a selected system dependency is unavailable.

## Current follow-up work

- Split the current UI, settings, and FFmpeg segment lifecycle out of `src/main.cpp` into independently reviewable components.
- Handle monitor hotplug, portal stream format renegotiation, and PipeWire stream teardown under all failure paths.
- Add integration coverage for segment rollover, audio combinations, errors, and both desktop backends.
- Expand hotkey configuration and show the active portal trigger returned by the compositor.
