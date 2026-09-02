# /MNT Tensor Konsole Visualiser

A locked-60-FPS, native C++ terminal audio visualiser built for KDE Konsole. It captures the audio you are currently hearing (the default PulseAudio/PipeWire sink monitor) and renders a dense, full-screen TrueColor plasma field that reacts to it in real time.

> This is **not** a bar visualiser like CAVA. Particle **density** is driven by volume, and particle **motion** is driven by the bass/mid/treble equaliser bands, producing an asymmetric, audio-reactive plasma field.

## Requirements

- Linux with a C++20 compiler (`g++` recommended)
- `pulseaudio-utils` or the PipeWire-Pulse compatibility layer (`pactl`, `parec`)
- A TrueColor-capable terminal (KDE Konsole recommended)

## Install

```bash
git clone https://github.com/MaanavNagda/mnt-tensor-konsole-visualiser.git
cd mnt-tensor-konsole-visualiser
chmod +x install.sh
./install.sh
```

This builds the binary with aggressive optimisation flags and installs it to `~/.local/bin/mnt_tensor_visualizer`.

## Manual build

```bash
g++ -O3 -pthread -std=c++20 -o mnt_tensor_visualizer mnt_tensor_visualizer.cpp
./mnt_tensor_visualizer
```

## Usage

```bash
mnt_tensor_visualizer
```

The Konsole tab title is automatically set to `/MNT Tensor Visualiser` while running.

### Controls

- `q` — quit and restore the terminal
- `Ctrl-C` — also quits

### Behaviour

- **Density** (how many cells are lit) scales with the current audio **volume**.
- **Motion** (how fast the field animates/warps) is driven independently by the **bass**, **mid**, and **treble** bands.
- If the default sink is **muted**, density drops to near-zero.
- It does not matter whether the output is speakers, headphones, Bluetooth, or a dummy sink — if audio is leaving the system mixer, it is visualised.

## Architecture

- Two-thread design: an audio thread (`parec` capture + 1024-point in-place FFT) and a render thread on a locked 16.6 ms (`clock_nanosleep`, `TIMER_ABSTIME`) loop.
- Spectrum data (bass/mid/treble/volume) is passed lock-free via `std::atomic<float>`.
- A single flat `std::string` frame buffer is reserved once per terminal size and reused every frame (zero per-frame heap allocation).
- Every frame starts with `\x1b[H` (cursor home) — no screen-clear escape codes are used mid-loop.
- The entire frame (all TrueColor ANSI codes) is flushed with one `write(STDOUT_FILENO, ...)` call per frame.
- Column width is clamped to `cols - 1` to avoid Konsole's line-wrap overhead.

See `plan_cpp.txt` for the full design notes.

## Uninstall

```bash
rm ~/.local/bin/mnt_tensor_visualizer
```
