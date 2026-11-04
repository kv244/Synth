#!/usr/bin/env python3
"""
BasicSynth automated tests using pedalboard + mido.
Loads the VST3 directly — no MIDI ports, loopMIDI, or DAW required.

  pip install pedalboard mido numpy sounddevice soundfile

Run tests:
  python test_midi.py

Play a rendered example through speakers:
  python test_midi.py --play

Save a rendered example to WAV:
  python test_midi.py --save output.wav
"""

import os
import sys
import argparse
import numpy as np

try:
    import pedalboard
    import mido
except ImportError as e:
    sys.exit(f"Missing dependency: {e}\n  pip install pedalboard mido numpy sounddevice soundfile")


# ---------------------------------------------------------------------------
# Audio output helpers
# ---------------------------------------------------------------------------

def play_audio(audio: np.ndarray, sample_rate: int = 44100):
    """Play a (channels, samples) float32 array through the default audio device."""
    try:
        import sounddevice as sd
    except ImportError:
        sys.exit("sounddevice not installed:  pip install sounddevice")
    # sounddevice expects (samples, channels)
    sd.play(audio.T, sample_rate)
    sd.wait()


def save_wav(audio: np.ndarray, path: str, sample_rate: int = 44100):
    """Save a (channels, samples) float32 array to a WAV file."""
    try:
        import soundfile as sf
    except ImportError:
        sys.exit("soundfile not installed:  pip install soundfile")
    sf.write(path, audio.T, sample_rate)
    print(f"Saved: {path}")

# ---------------------------------------------------------------------------
# Config
# ---------------------------------------------------------------------------

SAMPLE_RATE = 44100
BLOCK_SIZE  = 512

# pedalboard requires the inner DLL, not the outer bundle folder
VST3_PATH = os.path.join(
    os.path.dirname(os.path.abspath(__file__)),
    "build", "BasicSynth_artefacts", "Release",
    "VST3", "BasicSynth.vst3", "Contents", "x86_64-win", "BasicSynth.vst3",
)

# ---------------------------------------------------------------------------
# Plugin helpers
# ---------------------------------------------------------------------------

def load_plugin() -> pedalboard.VST3Plugin:
    return pedalboard.load_plugin(VST3_PATH)


def _filter_param_name(plugin) -> str | None:
    """Return the filter-enabled parameter name regardless of case/separator style."""
    for name in plugin.parameters:
        if "filter" in name.lower() and "enabled" in name.lower():
            return name
    return None


# Cutoff range in the plugin: 20 Hz - 20 kHz, skewed so 1 kHz sits mid-knob.
# This mirrors juce::NormalisableRange::setSkewForCentre (1000.0f).
CUTOFF_MIN, CUTOFF_MAX, CUTOFF_CENTRE = 20.0, 20000.0, 1000.0
CUTOFF_SKEW = np.log(0.5) / np.log((CUTOFF_CENTRE - CUTOFF_MIN) / (CUTOFF_MAX - CUTOFF_MIN))


def cutoff_to_normalised(cutoff_hz: float) -> float:
    proportion = (cutoff_hz - CUTOFF_MIN) / (CUTOFF_MAX - CUTOFF_MIN)
    return float(np.clip(proportion, 0.0, 1.0) ** CUTOFF_SKEW)


def set_params(plugin, cutoff_hz: float = 2000.0, waveform: int = 0,
               filter_on: bool = True):
    # raw_value is the VST3 normalized [0, 1] representation
    cutoff_norm   = cutoff_to_normalised(cutoff_hz)
    waveform_norm = waveform / 3.0
    plugin.parameters["cutoff"].raw_value   = float(np.clip(cutoff_norm,   0.0, 1.0))
    plugin.parameters["waveform"].raw_value = float(np.clip(waveform_norm, 0.0, 1.0))
    name = _filter_param_name(plugin)
    if name:
        plugin.parameters[name].raw_value = 1.0 if filter_on else 0.0


def make_msgs(notes: list[int], velocity: int, note_dur_s: float) -> list:
    """Build mido note-on / note-off messages. Time is in seconds."""
    msgs = []
    for note in notes:
        msgs.append(mido.Message("note_on",  note=note, velocity=velocity, time=0.0))
    for note in notes:
        msgs.append(mido.Message("note_off", note=note, velocity=0, time=note_dur_s))
    return msgs


def render(plugin, notes: list[int], *,
           velocity: int = 100,
           note_dur_s: float = 1.2,
           total_dur_s: float = 2.0,
           cutoff_hz: float = 2000.0,
           waveform: int = 0) -> np.ndarray:
    """Render simultaneous notes, return float32 array (channels, samples)."""
    set_params(plugin, cutoff_hz=cutoff_hz, waveform=waveform)
    msgs = make_msgs(notes, velocity, note_dur_s)
    return plugin.process(msgs, total_dur_s, SAMPLE_RATE, reset=True)

# ---------------------------------------------------------------------------
# Analysis helpers
# ---------------------------------------------------------------------------

def rms(audio: np.ndarray) -> float:
    return float(np.sqrt(np.mean(audio ** 2)))


def high_freq_ratio(audio: np.ndarray, threshold_hz: float = 4000.0) -> float:
    """Fraction of spectral power above threshold_hz (mono mix)."""
    mono  = audio[0] + audio[1]
    fft   = np.abs(np.fft.rfft(mono)) ** 2
    freqs = np.fft.rfftfreq(len(mono), 1.0 / SAMPLE_RATE)
    total = fft.sum()
    return float(fft[freqs >= threshold_hz].sum() / total) if total > 1e-12 else 0.0

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------

def test_single_note(plugin):
    """Synth produces audio for a single note."""
    audio = render(plugin, [60])
    r = rms(audio)
    assert r > 1e-4, f"Expected audio output, got RMS {r:.6f}"
    return f"RMS={r:.4f}"


def test_polyphony_chord(plugin):
    """4-voice chord is louder than 1 voice — confirms polyphony."""
    r1 = rms(render(plugin, [60]))
    r4 = rms(render(plugin, [60, 64, 67, 72]))
    assert r4 > r1 * 1.5, (
        f"4-voice chord ({r4:.4f}) should be significantly louder "
        f"than single note ({r1:.4f})"
    )
    return f"single={r1:.4f}  chord={r4:.4f}"


def test_polyphony_8_voices(plugin):
    """All 8 voices active simultaneously."""
    notes = [60, 62, 64, 65, 67, 69, 71, 72]
    r = rms(render(plugin, notes))
    assert r > 0.01, f"Expected significant output with 8 voices, got RMS {r:.6f}"
    return f"RMS={r:.4f}"


def test_voice_stealing(plugin):
    """17 simultaneous notes with a 16-voice limit — must not crash or go silent."""
    notes = list(range(48, 65))
    r = rms(render(plugin, notes))
    assert r > 1e-4, f"Voice steal caused silence — RMS {r:.6f}"
    return f"RMS={r:.4f}"


def test_release_tail(plugin):
    """Audio persists ~300 ms after note-off (tail-off implementation check)."""
    set_params(plugin)
    msgs = [
        mido.Message("note_on",  note=60, velocity=100, time=0.0),
        mido.Message("note_off", note=60, velocity=0,   time=0.5),
    ]
    audio = plugin.process(msgs, 1.5, SAMPLE_RATE, reset=True)

    # Sample the 550–750 ms window (post note-off)
    s = int(0.55 * SAMPLE_RATE)
    e = int(0.75 * SAMPLE_RATE)
    tail = rms(audio[:, s:e])
    assert tail > 1e-5, f"No release tail detected — RMS {tail:.8f}"
    return f"tail RMS={tail:.6f}"


def test_filter_low_cutoff(plugin):
    """
    Low cutoff (200 Hz) must attenuate high frequencies relative to 18 kHz cutoff.
    Analysis threshold is 1.5 kHz: a 2nd-order IIR at 200 Hz gives ~37 dB of
    separation there, making the ratio clear even with a single-pole filter.
    (4 kHz is too high — both values become noise-floor tiny and the ratio is
    unreliable across platforms.)
    """
    thresh = 1500.0
    hfr_low  = high_freq_ratio(render(plugin, [48], cutoff_hz=200,   waveform=1), thresh)
    hfr_high = high_freq_ratio(render(plugin, [48], cutoff_hz=18000, waveform=1), thresh)
    assert hfr_low < hfr_high * 0.5, (
        f"Filter not attenuating — HFR @1.5kHz: cutoff=200Hz:{hfr_low:.4f}  "
        f"cutoff=18kHz:{hfr_high:.4f}"
    )
    return f"HFR@1.5kHz  200Hz={hfr_low:.4f}  18kHz={hfr_high:.4f}"


def test_waveforms(plugin):
    """Each waveform produces measurably different harmonic content."""
    names = ["Sine", "Square", "Triangle", "Sawtooth"]
    hfr = [
        high_freq_ratio(render(plugin, [60], cutoff_hz=18000, waveform=i))
        for i in range(4)
    ]
    result = "  ".join(f"{names[i]}={hfr[i]:.4f}" for i in range(4))
    assert hfr[0] < hfr[1], \
        f"Sine should have less HFR than Square: {hfr[0]:.4f} vs {hfr[1]:.4f}"
    assert hfr[0] < hfr[3], \
        f"Sine should have less HFR than Sawtooth: {hfr[0]:.4f} vs {hfr[3]:.4f}"
    # Square and Triangle differ (square has more odd harmonics)
    assert hfr[1] != hfr[2], \
        f"Square and Triangle should differ: both {hfr[1]:.4f}"
    return result


def test_velocity_scaling(plugin):
    """Higher velocity produces louder output."""
    r_soft = rms(render(plugin, [60], velocity=20))
    r_loud = rms(render(plugin, [60], velocity=120))
    assert r_loud > r_soft * 1.5, (
        f"Velocity not scaling amplitude: soft={r_soft:.4f}  loud={r_loud:.4f}"
    )
    return f"vel20={r_soft:.4f}  vel120={r_loud:.4f}"


# ---------------------------------------------------------------------------
# Step sequencer
# ---------------------------------------------------------------------------

# Steps that are on in the plugin's default pattern (see loadDefaultPattern()).
DEFAULT_PATTERN_ON_STEPS = [0, 2, 3, 4, 6, 7, 8, 10, 11, 12, 14, 15]


def find_param(plugin, *words: str) -> str:
    """Return the name of the parameter whose name contains all of `words`.

    pedalboard turns display names into identifiers ("Sequencer On" ->
    "sequencer_on"), so matching on words keeps the tests independent of
    the exact spelling."""
    for name in plugin.parameters:
        if all(w in name.lower() for w in words):
            return name
    raise AssertionError(f"No parameter matching {words} in {list(plugin.parameters)}")


class SequencerOn:
    """Context manager: switch the sequencer on (internal clock), restore every parameter afterwards."""

    def __init__(self, plugin, **raw_values):
        self.plugin = plugin
        self.raw_values = raw_values

    def __enter__(self):
        self.saved = {name: p.raw_value for name, p in self.plugin.parameters.items()}
        self.plugin.parameters[find_param(self.plugin, "sequencer", "on")].raw_value = 1.0
        self.plugin.parameters[find_param(self.plugin, "sequencer", "sync")].raw_value = 0.0  # internal tempo
        for name, value in self.raw_values.items():
            self.plugin.parameters[name].raw_value = value
        return self.plugin

    def __exit__(self, *exc):
        for name, value in self.saved.items():
            self.plugin.parameters[name].raw_value = value
        return False


def onset_times(audio: np.ndarray, frame: int = 64) -> list[float]:
    """Times (s) where the signal rises out of silence: crude but robust for gated notes."""
    mono = np.abs(audio[0] + audio[1])
    frames = len(mono) // frame
    env = mono[: frames * frame].reshape(frames, frame).max(axis=1)
    threshold = 0.1 * env.max()
    quiet_frames_needed = int(0.02 * SAMPLE_RATE / frame)  # 20 ms of quiet before a new onset
    onsets, quiet = [], quiet_frames_needed
    for i, level in enumerate(env):
        if level > threshold:
            if quiet >= quiet_frames_needed:
                onsets.append(i * frame / SAMPLE_RATE)
            quiet = 0
        else:
            quiet += 1
    return onsets


def test_sequencer_plays(plugin):
    """With the sequencer on, the synth plays the pattern with no MIDI input at all."""
    set_params(plugin)
    with SequencerOn(plugin):
        audio = plugin.process([], 2.0, SAMPLE_RATE, reset=True)
    r = rms(audio)
    assert r > 1e-3, f"Sequencer produced no sound — RMS {r:.6f}"
    silent = rms(plugin.process([], 1.0, SAMPLE_RATE, reset=True))
    assert silent < 1e-5, f"Sound with the sequencer off and no MIDI — RMS {silent:.6f}"
    return f"on RMS={r:.4f}  off RMS={silent:.6f}"


def test_sequencer_timing(plugin):
    """At 120 BPM in 1/8 steps, notes start exactly on the pattern's steps (0.25 s apart)."""
    set_params(plugin, filter_on=False)
    fast_envelope = {
        find_param(plugin, "attack"): 0.0,     # 1 ms
        find_param(plugin, "release"): 0.0,    # 1 ms, so each gated note ends cleanly
        find_param(plugin, "sequencer", "rate"): 0.0,   # 1/8 (first choice)
    }
    with SequencerOn(plugin, **fast_envelope):
        audio = plugin.process([], 4.0, SAMPLE_RATE, reset=True)   # 16 steps at 1/8, 120 BPM

    expected = [s * 0.25 for s in DEFAULT_PATTERN_ON_STEPS]
    found = onset_times(audio)
    assert len(found) == len(expected), f"Expected {len(expected)} notes, found {len(found)}: {found}"
    worst = max(abs(f - e) for f, e in zip(found, expected))
    assert worst < 0.005, f"Onsets off by up to {worst * 1000:.1f} ms"
    return f"{len(found)} notes, worst timing error {worst * 1000:.2f} ms"


# ---------------------------------------------------------------------------
# Runner
# ---------------------------------------------------------------------------

TESTS = [
    ("Single note",       test_single_note),
    ("Polyphony (4ch)",   test_polyphony_chord),
    ("Polyphony (8ch)",   test_polyphony_8_voices),
    ("Voice stealing",    test_voice_stealing),
    ("Release tail",      test_release_tail),
    ("Filter cutoff",     test_filter_low_cutoff),
    ("Waveforms",         test_waveforms),
    ("Velocity scaling",  test_velocity_scaling),
    ("Sequencer plays",   test_sequencer_plays),
    ("Sequencer timing",  test_sequencer_timing),
]


def demo_render(plugin) -> np.ndarray:
    """Render a short musical demo: C major arpeggio, sawtooth, warm filter."""
    # Sawtooth + ~1 kHz cutoff gives a warm, analog character vs chiptune square
    set_params(plugin, cutoff_hz=1100, waveform=3)
    notes      = [60, 64, 67, 72, 67, 64, 60]
    note_dur_s = 0.38                      # note duration in seconds
    gap        = int(0.32 * SAMPLE_RATE)
    chunk_dur  = 0.9                        # enough room for release tail
    total_secs = gap * len(notes) / SAMPLE_RATE + chunk_dur
    total      = np.zeros((2, int(total_secs * SAMPLE_RATE)), dtype=np.float32)
    for i, note in enumerate(notes):
        offset = i * gap
        msgs = [
            mido.Message("note_on",  note=note, velocity=100, time=0.0),
            mido.Message("note_off", note=note, velocity=0,   time=note_dur_s),
        ]
        chunk = plugin.process(msgs, chunk_dur, SAMPLE_RATE, reset=True)
        end = min(offset + chunk.shape[1], total.shape[1])
        total[:, offset:end] += chunk[:, : end - offset]
    return total


def main():
    parser = argparse.ArgumentParser(description="BasicSynth test suite")
    parser.add_argument("--play", action="store_true",
                        help="render a demo and play it through speakers")
    parser.add_argument("--save", metavar="FILE",
                        help="render a demo and save it to a WAV file")
    parser.add_argument("--midilog", metavar="FILE",
                        help="path to write midi logs (processed by plugin)")
    parser.add_argument("--vst3", metavar="PATH",
                        help="inner VST3 binary to test (default: the build/ Release artefact)")
    args = parser.parse_args()

    global VST3_PATH
    if args.vst3:
        VST3_PATH = os.path.abspath(args.vst3)

    if args.midilog:
        # The plugin reads this when it is constructed, so set it before loading.
        os.environ["BASICSYNTH_MIDILOG"] = os.path.abspath(args.midilog)

    if not os.path.exists(VST3_PATH):
        sys.exit(
            f"VST3 not found:\n  {VST3_PATH}\n"
            "Build the project first (cmake --build build --config Release)."
        )

    print("Loading plugin …")
    plugin = load_plugin()
    print(f"Parameters: {list(plugin.parameters.keys())}\n")

    if args.play or args.save:
        audio = demo_render(plugin)
        if args.save:
            save_wav(audio, args.save, SAMPLE_RATE)
        if args.play:
            print("Playing …")
            play_audio(audio, SAMPLE_RATE)
        return

    passed = failed = 0
    for label, fn in TESTS:
        try:
            detail = fn(plugin)
            print(f"  PASS  {label:<22}  {detail}")
            passed += 1
        except AssertionError as e:
            print(f"  FAIL  {label:<22}  {e}")
            failed += 1
        except Exception as e:
            print(f"  ERROR {label:<22}  {e}")
            failed += 1

    print(f"\n{passed}/{passed + failed} passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    main()
