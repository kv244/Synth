# NeonSynth: developer notes

Building, testing and the layout of the source. For playing the instrument, read the
[user manual](NeonSynth-User-Manual.md); for an overview, the [README](../README.md).

Paths in this file are relative to the repository root.

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
| Standalone | `build/NeonSynth_artefacts/Release/Standalone/NeonSynth.exe` |
| VST3 | `build/NeonSynth_artefacts/Release/VST3/NeonSynth.vst3/` |

---

## Sequencer engine notes

- Every step is placed on an exact sample, derived from the transport each block, so timing does not drift and does not depend on the host's buffer size. The synth also starts every note on its exact sample (it renders up to each MIDI event rather than in 32-sample chunks).
- Small irregularities in the host's beat position — a tempo change inside a block, rounding — of up to 2 ms are treated as continuous playback; only a real jump (a loop or locate) releases held notes.
- With SYNC on in a DAW, the sequencer follows the DAW's transport: press play in the DAW to hear it.
- The sequencer and the keyboard never cut off each other's notes, even on the same pitch: the sequencer plays on its own MIDI channel, and keyboard input is moved to channel 1 whatever channel it arrives on.
- The pattern (16 × on / note / velocity) is saved in the plugin state with the session and presets. Sessions saved with earlier versions load with the default pattern.
- The engine is `StepSequencer.h`; it depends only on `juce_audio_basics` and is covered by its own unit tests (`tests/StepSequencerTests.cpp`, see [Unit tests and benchmark](#unit-tests-and-benchmark)). The controls are described in the manual's sequencer chapter.

---

## Unit tests and benchmark

Configure with `-DNEONSYNTH_BUILD_TESTS=ON` to add the unit tests and the benchmark. `ctest` runs every unit test:

```bash
cmake -S . -B build -DJUCE_DIR="/path/to/JUCE" -DNEONSYNTH_BUILD_TESTS=ON
cmake --build build --config Release --target unit_tests RenderBenchmark
ctest --test-dir build -C Release --output-on-failure
```

| Program | What it checks |
|---|---|
| `StepSequencerTests` | Sequencer timing, gate, swing, rates, block-size independence, stop and loop handling, tolerance of small transport drift, triggers disabled, retriggering, step packing |
| `VoiceEnvelopeTests` | The voice envelope: identical to `juce::ADSR` with fixed settings, releases that survive parameter updates, sustain slew, retriggering, release-time changes |
| `OscillatorTests` | The oscillators: pitch and DC offset of every waveform, level at high pitch, band-limited saw step, waveform changes waiting for the cycle boundary, phase kept across pitch changes; the Random source's repeatability, range, step statistics (one level replaced per cycle), pitch, and band-limited steps; aliasing of square, saw and triangle against v1.5.0 at C7 and C8, harmonic levels, the phase-step limit |
| `ChopTests` | CHOP: Overtone 0% bit-identical to no CHOP, 100% silences and 200% inverts the chopped steps, Comb 1 / 2 / 8, band-limited edges, fading out as steps get too short, the Random source |
| `SculptTests` | Noise and Fold: bit-identical when neutral, repeatable noise at a steady level across the knob, fold aliasing against a plain fold, peak, DC, a flat response at partial mix |
| `SampleDataTests` | The sample: mono mix, −1 dBFS normalisation, 24-bit grid, 60 s cap, decimated copies (pitch kept, aliasing removed), exact FLAC round trip, file loading, the built-in pad, and the lock-free hand-off freeing replaced samples only when safe, including 2000 swaps under load |
| `SampleReaderTests` | Sample playback: pitch tracking (and across sample rates), which band-limited copy each pitch reads, no aliasing at +24 semitones, steady level through crossfaded loops, smooth off-cycle loops, One-shot ends, Loop until release, start offset, the sample reference count |
| `GranularTests` | The granular source: bit-identical renders from the same seed, grain counts at 5 / 50 / 200 per second, steady level across size and density, pitch, the 32-grain cap, scan speed and freeze, retrigger without cutting grains, stopping, the sample reference |
| `RealtimeSafetyTests` | Hooks `operator new`/`delete` and fails if `processBlock` allocates or frees while rendering chords, the sequencer, every waveform and every source as another thread keeps replacing the sample |
| `ParameterLayoutTests` | Every parameter's ID, place, type, range, skew, default and choices against `tests/parameter-layout.txt`: parameters may only be appended (hosts store sessions by ID and normalised value). After appending one, update the reference with `ParameterLayoutTests --write` |
| `EditorTests` | Opens the editor offscreen and drives it with mouse events: every parameter has a control, each source shows its own controls with nothing overlapping, knobs, boxes and buttons write their parameter in one gesture and follow it, the step grid, the CHOP strip, the sample markers, the four envelope points, the view and help buttons (Esc closes help from any control), a replaced sample being freed once its notes end, and a background load that is overtaken; then paints every view at 75%, 100% and 150% for every source. Finds controls only by component ID, so it carries over to a new layout. On Linux it needs an X display (`xvfb-run`) |
| `RenderBenchmark` | Times `processBlock` directly (no host) for idle, one note, 16 voices, 16 voices on each of the Sample, Random and Granular sources, 16 sculpted voices (CHOP, Noise and Fold all on), and a dense sequencer + chords load |

### Performance

`RenderBenchmark` on the development machine, 48 kHz with 512-sample blocks (share of one CPU core in real time):

| Load | CPU |
|---|---|
| Idle | 0.01% |
| One note | 0.10% |
| 16 voices at full velocity | 1.7% |
| 16 voices playing the built-in sample, looped | 1.0% |
| 16 voices on the Random source | 1.1% |
| 16 sawtooth voices with CHOP (Overtone 100%, Comb 4), Noise 30% and Fold (Drive 5, 100%) | 5.0% |
| 16 voices on the granular source, 32 grains each (200 grains/s of 250 ms: the worst case) | 6.2% |
| Sequencer at 1/32, 240 BPM, plus an 8-note chord every 250 ms | 1.3% |

The grain loop has three versions of the same arithmetic, picked at run time: AVX2 (8 samples per step, when the CPU has it), SSE2 (4 per step, every x86-64 CPU) and scalar. They render bit-identical audio, so a session sounds the same on every machine; `GranularTests` checks this. Per grain and output sample on the development machine (Core Ultra 9 185H): scalar 4.7 ns, SSE2 2.3 ns, AVX2 2.2 ns. The loop is bound by its four reads per sample, which is why the wider AVX2 registers gain little.

Timing the plugin through pedalboard is not a reliable CPU measure: render times there varied several-fold with the output level alone.

---

## Python Testing

The included `test_midi.py` script loads the VST3 directly from Python — no
MIDI ports, loopMIDI, or DAW required.

### Install dependencies

```bash
pip install -r requirements-test.txt     # pedalboard, mido, numpy (pinned)
```

### Run all tests

```bash
python test_midi.py
```

Expected output:

```
Loading plugin …
Parameters: ['source', 'waveform', 'cutoff', 'filter_enabled', 'attack', … ]

  PASS  Single note             RMS=0.0799
  PASS  Polyphony (4ch)         single=0.0799  chord=0.1596
  PASS  Polyphony (8ch)         RMS=0.2234
  PASS  Voice stealing          RMS=0.3051
  PASS  Release tail            tail RMS=0.052122
  PASS  Filter cutoff           HFR@1.5kHz  200Hz=0.0000  18kHz=0.0315
  PASS  Waveforms               Sine=0.0000  Square=0.0199  Triangle=0.0001  Sawtooth=0.0313
  PASS  Velocity scaling        vel20=0.0160  vel120=0.0959
  PASS  Sequencer plays         on RMS=0.1119  off RMS=0.000000
  PASS  Sequencer timing        12 notes, worst timing error 0 samples (0.000 ms)
  PASS  No stuck note           RMS after release=0.0000000
  PASS  Close events            notes at samples 4411 and 4431 (expected 4410, 4430)
  PASS  Headroom                16 voices +6 dB: -1.00 dBFS  single note: -12.14 dBFS
  PASS  Release mid-decay       tail RMS=0.0607  largest step=0.0093
  PASS  Sustain change          largest step=0.0093  peak after the change=0.050
  PASS  State round trip        one-step pattern: 1 note  no pattern: 12 notes (default)
  PASS  Sample state            embedded FLAC 39 KB, hash 7cc6a876420f1755; missing file kept; v1.4 state -> default pad
  PASS  Sample pitch            48: 130.8 Hz  60: 261.6 Hz  72: 523.2 Hz  one-shot ends
  PASS  Granular                RMS 0.055  peak 524.3 Hz  repeat renders identical
  PASS  Random                  RMS 0.077  correlation 0.93 at one period, 0.46 off it  repeat renders identical
  PASS  Chop state              open 0.079  default 0.056 (70%)  all chopped 0.00000  v1.5 state -> default
  PASS  Sculpt                  peak 523.3 Hz (harmonic 2 of C4)  above 2 kHz: 70.9% sculpted vs 7.5% plain
  PASS  Channel 16 key          held note RMS after the sequencer note ended=0.0975
  PASS  Log per instance        two instances wrote midi-2.log and midi-4.log

24/24 passed
```

### Old sessions still sound the same

`tests/compare_versions.py` renders 16 sessions (every source, CHOP, Noise and
Fold, the sequencer, the clipper) through two builds and requires the audio to
be identical sample for sample. New parameters start at neutral defaults, so a
session saved with an earlier release must not change. CI builds the v1.6.0
release once from its tag, caches it, and compares every pull request with it.

```bash
python tests/compare_versions.py old/NeonSynth.vst3 build/NeonSynth_artefacts/Release/VST3/NeonSynth.vst3
```

### Render and hear audio

`docs/demo/make_demo.py` renders the demo piece linked from the README: six instances of the
VST3, one per part, summed and saved as a WAV file. It is also the fullest example of driving
the plugin from Python (parameters by name, MIDI at exact samples, automation every 25 ms).

```bash
python docs/demo/make_demo.py build/NeonSynth_artefacts/Release/VST3/NeonSynth.vst3 demo.wav
```

The Random and Granular parts are random, so two renders differ slightly.

### MIDI Logging

You can enable logging of all received MIDI messages to a text file by passing the `--midilog <file>` option. Timestamps are in seconds of audio time since the plugin started processing, so they stay correct in offline renders that run faster than real time:

```bash
# Log MIDI events to a file during testing
python test_midi.py --midilog log.txt
```

> [!NOTE]
> `test_midi.py` turns the path into an absolute one and passes it to the plugin through the `NEONSYNTH_MIDILOG` environment variable, so `log.txt` lands in your current directory. In a DAW, set `NEONSYNTH_MIDILOG` (preferably to an absolute path) before starting the host. Each plugin instance writes its own file: the first uses the name as given, later ones add `-2`, `-3`, … before the extension (`log.txt`, `log-2.txt`, …). Formatting and disk writes happen on a background thread, never on the audio thread.

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
python log_to_midi.py build/NeonSynth_artefacts/Release/Standalone/midi.log
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
| Sequencer plays | With the sequencer on, the pattern plays with no MIDI input; with it off, the output is silent |
| Sequencer timing | At 120 BPM in 1/8 steps, every note of the default pattern starts on its step, to within 2 samples |
| No stuck note | A key pressed in Layer and released after switching to Transpose still stops |
| Close events | Two notes 20 samples apart each start on their own sample |
| Headroom | 16 full-velocity voices at +6 dB gain peak at −1 dBFS; a single note passes through unclipped |
| Release mid-decay | With Sustain 0, a note released during its decay fades over the Release time without a click |
| Sustain change | Changing Sustain while a note is held ramps the level instead of clicking |
| State round trip | The pattern is saved and restored; a state without a pattern loads the default |
| Chop state | CHOP's pattern is saved and restored, and changes the level as expected (none, every second step, all chopped); a state from before v1.6.0 gets the default pattern |
| Sculpt | A chopped, noisy, folded sawtooth keeps its pitch and puts far more of its energy above 2 kHz than the plain one |
| Granular | With Source = Granular and a C4 sine loaded, C5 peaks at 523 Hz, and two renders of the same MIDI are identical |
| Random | With Source = Random, C4 repeats itself one period later (pitched, not noise), and two renders of the same MIDI are identical |
| Sample pitch | With Source = Sample and a C4 sine loaded, C3 / C4 / C5 sound at 130.8 / 261.6 / 523.3 Hz; One-shot goes silent when the file ends |
| Sample state | A file named in the state loads and is embedded as FLAC; another instance restores it from the embedded copy after the file is deleted; a missing sample keeps its reference; a v1.4 state loads the built-in pad |
| Channel 16 key | A keyboard note on channel 16 survives the sequencer playing the same pitch |
| Log per instance | Two instances logging MIDI each write their own file |

### Using the API directly

You can also drive the synth from your own Python scripts:

```python
import pedalboard, mido, numpy as np

SR = 48000
VST3 = (
    r"build\NeonSynth_artefacts\Release\VST3"
    r"\NeonSynth.vst3\Contents\x86_64-win\NeonSynth.vst3"
)

plugin = pedalboard.load_plugin(VST3)

# Set parameters by name, in their own units (print(plugin.parameters.keys()) lists them)
plugin.cutoff = 500.0          # Hz
plugin.waveform = "Square"

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
# Low cutoff → dark tone
plugin.cutoff = 200.0
audio_dark = plugin.process(msgs, 2.5, SR, reset=True)

# High cutoff → bright tone
plugin.cutoff = 16000.0
audio_bright = plugin.process(msgs, 2.5, SR, reset=True)
```

---

## Project Structure

| File | Purpose |
|---|---|
| `NeonSynthVoice.h` | Voice and sound classes — sound source, filter, envelope, pan |
| `Oscillator.h` | The band-limited oscillators (sine, square, triangle, sawtooth) with 4-point band-limited steps and corners, the Random source's step wave, and CHOP |
| `Sculpt.h` | Noise and Fold: white-noise crossfade and a 2× oversampled sine wavefolder |
| `ShapeView.h` | The SHAPE strip's drawing of one cycle, with CHOP step editing |
| `SampleData.h` | A prepared sample: mono, normalised, 24-bit grid, with 1/2- and 1/4-rate copies |
| `SampleSlot.h` | Lock-free hand-off of the current sample to the audio thread; frees replaced samples only when safe |
| `SampleLoader.h` | Loads WAV, AIFF and FLAC files and embedded FLAC; renders the built-in pad |
| `SampleReader.h` | Plays a note from the sample: pitch, cubic interpolation, band-limited copies, crossfaded loops |
| `GrainCloud.h` | The granular source: grain scheduler, 32-grain pool, Hann window, scan, spray and jitter |
| `SampleView.h` | The sample's waveform in the editor, with draggable markers and live playheads |
| `NeonSynthProcessor.h/cpp` | `AudioProcessor` — parameter management, MIDI, state |
| `NeonSynthEditor.h` | Neon UI — layout, controls, resizing, the 30 Hz timer that feeds the visualisers |
| `MidiLogger.h` | Real-time-safe MIDI logging (lock-free FIFO + background writer thread) |
| `StepSequencer.h` | Step sequencer engine — timing, swing, gate, note-off queue, lock-free pattern |
| `SequencerView.h` | The 16-step grid in the editor (editing and playhead) |
| `HelpOverlay.h` | Quick-help page opened by the editor's ? button |
| `VoiceEnvelope.h` | The voice's ADSR envelope (slewed sustain, release rate fixed at note-off) |
| `tests/StepSequencerTests.cpp` | Unit tests for the sequencer engine (`-DNEONSYNTH_BUILD_TESTS=ON`) |
| `tests/VoiceEnvelopeTests.cpp` | Unit tests for the voice envelope |
| `tests/OscillatorTests.cpp` | Unit tests for the oscillators, including Random |
| `tests/ChopTests.cpp` | Unit tests for CHOP |
| `tests/SculptTests.cpp` | Unit tests for Noise and Fold |
| `tests/SampleDataTests.cpp` | Unit tests for the sample, its loader and the hand-off |
| `tests/SampleReaderTests.cpp` | Unit tests for sample playback |
| `tests/GranularTests.cpp` | Unit tests for the granular source |
| `tests/RealtimeSafetyTests.cpp` | Fails if `processBlock` allocates |
| `tests/RenderBenchmark.cpp` | CPU benchmark of `processBlock` |
| `tests/ParameterLayoutTests.cpp`, `tests/parameter-layout.txt` | The parameter-layout test and its reference |
| `tests/EditorTests.cpp` | Offscreen editor tests (gestures and painting) |
| `tests/complexity_report.py` | Lines of code and the cyclomatic complexity of every plugin function, as Markdown (`pip install lizard`); CI runs it on every push and pull request |
| `tests/compare_versions.py` | Renders a set of sessions through two VST3 builds and requires identical audio (CI compares against the v1.6.0 release) |
| `Visualisers.h` | Oscilloscope, spectrum analyser, ADSR graph, voice lights and L/R meter (message thread only) |
| `CMakeLists.txt` | Build config — Standalone + VST3, JUCE path, the test programs |
| `icon.png` | Application / plugin icon |
| `test_midi.py` | Automated Python test suite |
| `log_to_midi.py` | Converts a captured `midi.log` into a Standard MIDI File |
| `docs/NeonSynth-User-Manual.md` | The user manual |
| `docs/DEVELOPMENT.md` | This file |
| `docs/demo/` | The demo piece and the script that renders it from the VST3 |
| `docs/build_manual.py` | Checks the manual against the release (version, cover, links) and prints it to PDF |
| `docs/figures/` | Scripts that draw the manual's figures and cover, with a README on remaking them |
| `docs/images/` | The manual's figures and cover (SVG and PNG), and the raw application captures |
| `docs/requirements-manual.txt` | Pinned Python dependencies for the PDF build |
| `pyproject.toml` | Ruff lint configuration |
| `.github/workflows/test.yml` | CI on every PR — Windows build, unit tests, Python suite, the comparison with the reference release, pluginval (strictness 10, as at release), CPU benchmark |
| `.github/workflows/build.yml` | CI — CMake build and unit tests on Ubuntu (GCC) |
| `.github/workflows/lint.yml` | CI — ruff lint of every Python file, and the code statistics report (lines of code, cyclomatic complexity per function) on the run's Summary page |
| `.github/workflows/manual.yml` | CI — checks the manual against the version and builds its PDF |
| `.github/workflows/release.yml` | Release on a version tag — manual check, all tests, pluginval (strictness 10), installer, GitHub Release with the manual PDF |
| `requirements-test.txt` | Pinned Python dependencies for `test_midi.py` |
| `installer.nsi` | NSIS installer script (VST3 + Standalone) |
| `LICENSE` | GNU Affero General Public License v3 |

---

## Upgrading from BasicSynth

Before v1.4 the plugin was called **BasicSynth**. It is now **NeonSynth** (`NeonSynth.vst3`, `NeonSynth.exe`). The plugin's internal ID is unchanged, so DAW projects saved with BasicSynth open with NeonSynth and keep their settings. The installer removes the old `BasicSynth.vst3` so your DAW doesn't list the plugin twice. The Standalone now stores its settings under the new name, so pick your audio and MIDI devices once more in **Options → Audio/MIDI Settings**. The `BASICSYNTH_MIDILOG` environment variable still works; `NEONSYNTH_MIDILOG` is the new name.
