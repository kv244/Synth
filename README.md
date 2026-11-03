# Neon Synth

[![Build](https://github.com/kv244/Synth/actions/workflows/build.yml/badge.svg)](https://github.com/kv244/Synth/actions/workflows/build.yml)
[![Lint](https://github.com/kv244/Synth/actions/workflows/lint.yml/badge.svg)](https://github.com/kv244/Synth/actions/workflows/lint.yml)
[![Release](https://github.com/kv244/Synth/actions/workflows/release.yml/badge.svg)](https://github.com/kv244/Synth/actions/workflows/release.yml)
[![Latest release](https://img.shields.io/github/v/release/kv244/Synth)](https://github.com/kv244/Synth/releases/latest)

A futuristic, neon-styled virtual analog synthesizer built with JUCE and C++17,
by **Razvan Julian Petrescu** / **AudioDSP**.

![Neon Synth screenshot](screenshot.png)

## Features

- **4 Waveforms** — Sine, Square, Triangle, Sawtooth
- **Low-pass Filter** — adjustable cutoff (20 Hz – 20 kHz) with on/off bypass toggle
- **16-voice Polyphony** — up to 16 simultaneous notes, with click-free voice stealing
- **ADSR envelope** — linear segments whose durations match the knob values
- **Band-limited oscillators** — PolyBLEP square and sawtooth to reduce aliasing
- **Output gain** — host-automatable output level (−48 dB to +6 dB)
- **Post-filter waveform display** — live output visualiser in the UI
- **Custom neon GUI** — background image, cyan/magenta controls
- **MIDI** — works with any MIDI controller or keyboard
- **MIDI Logging** — optional, real-time-safe logging of received MIDI messages to a file (`BASICSYNTH_MIDILOG=<file>`, or `--midilog <file>` in the Standalone)
- **DAW automation** — all parameters are automatable in VST3 hosts

---

## Download

Pre-built Windows installer: **[Latest release →](https://github.com/kv244/Synth/releases/latest)**

Run `NeonSynth-x.y.z-Windows-Setup.exe`. The wizard installs the VST3 to
`C:\Program Files\Common Files\VST3\` and optionally the standalone app to
`C:\Program Files\AudioDSP\Neon Synth\`.

---

## Building

### Prerequisites

| Tool | Version |
|---|---|
| CMake | 3.21+ |
| JUCE | 9.0.0 (download separately) |
| C++ compiler | MSVC 2022 / Clang / GCC with C++17 |

### Steps

```bash
cmake -S . -B build \
      -DJUCE_DIR="/path/to/JUCE" \
      -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

On Windows (PowerShell):

```powershell
cmake -S . -B build `
      -DJUCE_DIR="C:/path/to/JUCE" `
      -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

### Build outputs

| Format | Path |
|---|---|
| Standalone | `build/BasicSynth_artefacts/Release/Standalone/BasicSynth.exe` |
| VST3 | `build/BasicSynth_artefacts/Release/VST3/BasicSynth.vst3/` |

---

## Usage

### Standalone

Run `BasicSynth.exe`. Click **Options → Audio/MIDI Settings** to select your
audio output and MIDI input device, then play from any connected MIDI keyboard.

You can also launch the standalone application with the `--midilog <file>` parameter, or set the `BASICSYNTH_MIDILOG` environment variable, to log real-time MIDI input:
```bash
BasicSynth.exe --midilog log.txt
```

### VST3

Copy the `BasicSynth.vst3` bundle to your DAW's VST3 folder
(typically `C:\Program Files\Common Files\VST3\`). The plugin appears under
the **AudioDSP** manufacturer in your DAW's instrument browser.

### Controls

| Control | Colour | Description |
|---|---|---|
| Waveform | Cyan | Oscillator shape — Sine / Square / Triangle / Sawtooth |
| Filter ON | Magenta | Toggle button — enables / bypasses the filter (dims the cutoff slider when off) |
| Cutoff | Magenta | Low-pass filter cutoff frequency (20 Hz – 20 kHz) |
| Output | Cyan | Live post-filter waveform display |

---

## Python Testing

The included `test_midi.py` script loads the VST3 directly from Python — no
MIDI ports, loopMIDI, or DAW required.

### Install dependencies

```bash
pip install pedalboard mido numpy sounddevice soundfile
```

### Run all tests

```bash
python test_midi.py
```

Expected output:

```
Loading plugin …
Parameters: ['waveform', 'cutoff', 'filter_enabled', 'attack', 'decay', 'sustain', 'release', 'output_gain_db', 'bypass']

  PASS  Single note             RMS=0.0799
  PASS  Polyphony (4ch)         single=0.0799  chord=0.1596
  PASS  Polyphony (8ch)         RMS=0.2247
  PASS  Voice stealing          RMS=0.3135
  PASS  Release tail            tail RMS=0.052122
  PASS  Filter cutoff           HFR@1.5kHz  200Hz=0.0000  18kHz=0.0283
  PASS  Waveforms               Sine=0.0000  Square=0.0154  Triangle=0.0000  Sawtooth=0.0245
  PASS  Velocity scaling        vel20=0.0160  vel120=0.0959

8/8 passed
```

### Render and hear audio

The script can also render a demo arpeggio and play it through your speakers
or save it to a WAV file — no MIDI keyboard or DAW needed.

```bash
# play through speakers
python test_midi.py --play

# save to WAV
python test_midi.py --save output.wav

# both at once
python test_midi.py --play --save output.wav
```

The demo renders a C major arpeggio with a sawtooth oscillator and a 1.1 kHz
filter cutoff — warm and analog rather than chiptune.

### MIDI Logging

You can enable logging of all received MIDI messages to a text file by passing the `--midilog <file>` option. Timestamps are in seconds of audio time since the plugin started processing, so they stay correct in offline renders that run faster than real time:

```bash
# Log MIDI events to a file during testing
python test_midi.py --midilog log.txt
```

> [!NOTE]
> `test_midi.py` turns the path into an absolute one and passes it to the plugin through the `BASICSYNTH_MIDILOG` environment variable, so `log.txt` lands in your current directory. In a DAW, set `BASICSYNTH_MIDILOG` (preferably to an absolute path) before starting the host. Formatting and disk writes happen on a background thread, never on the audio thread.

Example log output:
```text
[0.025s] Note on C3 Velocity 127 Channel 1
[0.028s] All notes off Channel 1
[0.029s] Note on C3 Velocity 120 Channel 1
[0.118s] Note off C3 Velocity 0 Channel 1
```

> [!NOTE]
> Logging writes raw UTF-8 text via `OutputStream::write()`. Earlier builds used
> `OutputStream::writeString()`, which null-terminates every call and left a stray
> `\0` byte glued to the front of each line after the first — logs captured with
> those builds still parse fine with `log_to_midi.py` below, which strips them.

### Converting a log to a .mid file

`log_to_midi.py` turns a captured `midi.log` into a Standard MIDI File that
Windows Media Player, a DAW, or a notation app can open directly:

```bash
python log_to_midi.py build/BasicSynth_artefacts/Release/Standalone/midi.log
# writes midi.mid next to the input file
```

Pass an explicit output path as a second argument to override the default.
Note on/off pairs and "All notes off" are converted to proper MIDI events at
a fixed 120 BPM tempo, with real time preserved via tick timing.

### What each test checks

| Test | What it verifies |
|---|---|
| Single note | Basic audio output — RMS above noise floor |
| Polyphony (4ch) | 4-voice chord is significantly louder than a single note |
| Polyphony (8ch) | All 8 voices fire simultaneously without dropouts |
| Voice stealing | 17th simultaneous note steals a voice without crashing or silence |
| Release tail | Audio persists after note-off for the 300 ms release (linear decay) |
| Filter cutoff | High-frequency energy is lower at 200 Hz cutoff than at 18 kHz |
| Waveforms | Sine, Square, Triangle, Sawtooth produce distinct harmonic content |
| Velocity scaling | Higher velocity produces proportionally louder output |

### Using the API directly

You can also drive the synth from your own Python scripts:

```python
import pedalboard, mido, numpy as np

VST3 = (
    r"build\BasicSynth_artefacts\Release\VST3"
    r"\BasicSynth.vst3\Contents\x86_64-win\BasicSynth.vst3"
)

plugin = pedalboard.load_plugin(VST3)

# Set parameters (raw_value is VST3-normalised 0–1)
plugin.parameters["cutoff"].raw_value  = (500 - 20) / (20000 - 20)   # ~500 Hz
plugin.parameters["waveform"].raw_value = 1 / 3                        # Square

# Build MIDI messages (time field = offset in seconds)
msgs = [
    mido.Message("note_on",  note=60, velocity=100, time=0.0),
    mido.Message("note_off", note=60, velocity=0,   time=1.0),
]

# Render 2 seconds of audio → numpy array (channels, samples)
audio = plugin.process(msgs, 2.0, SR, reset=True)
print(audio.shape, audio.max())
```

#### Play a chord

```python
chord = [60, 64, 67, 72]   # C major
msgs  = []
for note in chord:
    msgs.append(mido.Message("note_on",  note=note, velocity=90, time=0.0))
for note in chord:
    msgs.append(mido.Message("note_off", note=note, velocity=0,  time=1.5))

audio = plugin.process(msgs, 2.5, SR, reset=True)
```

#### Sweep the filter

```python
import numpy as np

# Low cutoff → dark tone
plugin.parameters["cutoff"].raw_value = (200 - 20) / (20000 - 20)
audio_dark = plugin.process(msgs, 2.5, SR, reset=True)

# High cutoff → bright tone
plugin.parameters["cutoff"].raw_value = (16000 - 20) / (20000 - 20)
audio_bright = plugin.process(msgs, 2.5, SR, reset=True)
```

---

## Project Structure

| File | Purpose |
|---|---|
| `BasicSynth.h` | Voice and sound classes — oscillator, filter, envelope |
| `BasicSynthProcessor.h/cpp` | `AudioProcessor` — parameter management, MIDI, state |
| `BasicSynthEditor.h` | Neon UI — waveform selector, filter slider, output display |
| `CMakeLists.txt` | Build config — Standalone + VST3, JUCE path, binary resources |
| `gui_background.png` | Neon grid background image (embedded as binary data) |
| `icon.png` | Application / plugin icon |
| `test_midi.py` | Automated Python test suite |
| `pyproject.toml` | Ruff lint configuration |
| `.github/workflows/build.yml` | CI — CMake build on Ubuntu |
| `.github/workflows/lint.yml` | CI — Python ruff lint |
| `.github/workflows/release.yml` | CI — builds installer and publishes GitHub Release on tag push |
| `installer.nsi` | NSIS installer script (VST3 + Standalone) |
| `LICENSE` | MIT licence |
