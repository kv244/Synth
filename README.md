# Neon Synth

[![Build](https://github.com/kv244/Synth/actions/workflows/build.yml/badge.svg)](https://github.com/kv244/Synth/actions/workflows/build.yml)
[![Lint](https://github.com/kv244/Synth/actions/workflows/lint.yml/badge.svg)](https://github.com/kv244/Synth/actions/workflows/lint.yml)

A futuristic, neon-styled virtual analog synthesizer built with JUCE and C++17,
by **Razvan Julian Petrescu** / **AudioDSP**.

## Features

- **4 Waveforms** — Sine, Square, Triangle, Sawtooth
- **Low-pass Filter** — adjustable cutoff (20 Hz – 20 kHz) with on/off bypass toggle
- **8-voice Polyphony** — up to 8 simultaneous notes with voice stealing
- **Release tail** — smooth ~300 ms exponential fade on note-off
- **Post-filter waveform display** — live output visualiser in the UI
- **Custom neon GUI** — background image, cyan/magenta controls
- **MIDI** — works with any MIDI controller or keyboard
- **DAW automation** — all parameters are automatable in VST3 hosts

---

## Building

### Prerequisites

| Tool | Version |
|---|---|
| CMake | 3.21+ |
| JUCE | 8.x (download separately) |
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
Parameters: ['waveform', 'cutoff', 'filterEnabled', 'bypass']

  PASS  Single note             RMS=0.5038
  PASS  Polyphony (4ch)         single=0.5038  chord=1.0947
  PASS  Polyphony (8ch)         RMS=1.5011
  PASS  Voice stealing          RMS=1.4997
  PASS  Release tail            tail RMS=0.488542
  PASS  Filter cutoff           HFR  200Hz=0.0064  18kHz=0.0130
  PASS  Waveforms               Sine=0.0000  Square=0.0266  Triangle=0.0001  Sawtooth=0.0040
  PASS  Velocity scaling        vel20=0.1523  vel120=0.6112

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

### What each test checks

| Test | What it verifies |
|---|---|
| Single note | Basic audio output — RMS above noise floor |
| Polyphony (4ch) | 4-voice chord is significantly louder than a single note |
| Polyphony (8ch) | All 8 voices fire simultaneously without dropouts |
| Voice stealing | 9th simultaneous note steals a voice without crashing or silence |
| Release tail | Audio persists ~300 ms after note-off (exponential decay) |
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

# Build MIDI messages (time field = sample offset)
SR = 44100
msgs = [
    mido.Message("note_on",  note=60, velocity=100, time=0),
    mido.Message("note_off", note=60, velocity=0,   time=int(1.0 * SR)),
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
    msgs.append(mido.Message("note_on",  note=note, velocity=90, time=0))
for note in chord:
    msgs.append(mido.Message("note_off", note=note, velocity=0,  time=int(1.5 * SR)))

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
| `LICENSE` | MIT licence |
