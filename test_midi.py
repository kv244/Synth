#!/usr/bin/env python3
# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""
NeonSynth automated tests using pedalboard + mido.
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
import re
import struct
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
    "build", "NeonSynth_artefacts", "Release",
    "VST3", "NeonSynth.vst3", "Contents", "x86_64-win", "NeonSynth.vst3",
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


def onset_times(audio: np.ndarray) -> list[float]:
    """Times (s) of the first non-silent sample after at least 20 ms of digital silence.

    Exact to the sample when notes are separated by true silence (short release),
    which is how the sequencer timing test sets them up."""
    loud = np.abs(audio[0]) + np.abs(audio[1]) > 1e-7
    gap = int(0.02 * SAMPLE_RATE)
    onsets, last_loud = [], -gap
    for i in np.flatnonzero(loud):
        if i - last_loud >= gap:
            onsets.append(i / SAMPLE_RATE)
        last_loud = i
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
    """At 120 BPM in 1/8 steps, notes start on the pattern's steps (0.25 s apart) to within 2 samples."""
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
    worst_samples = worst * SAMPLE_RATE
    assert worst_samples <= 2.0, f"Onsets off by up to {worst_samples:.0f} samples ({worst * 1000:.2f} ms)"
    return f"{len(found)} notes, worst timing error {worst_samples:.0f} samples ({worst * 1000:.3f} ms)"


def test_headroom(plugin):
    """16 full-velocity square voices plus +6 dB output gain stay under the -1 dBFS ceiling,
    while a single note (well below the -6 dBFS knee) passes through unclipped."""
    gain = find_param(plugin, "gain")
    saved = plugin.parameters[gain].raw_value
    plugin.parameters[gain].raw_value = 1.0                      # +6 dB, the maximum
    loud = render(plugin, list(range(48, 64)), velocity=127, waveform=1, cutoff_hz=18000)
    plugin.parameters[gain].raw_value = saved
    single = render(plugin, [60], velocity=100, waveform=1, cutoff_hz=18000)
    ceiling = 10 ** (-1 / 20)
    peak_loud, peak_single = float(np.abs(loud).max()), float(np.abs(single).max())
    assert peak_loud <= ceiling + 1e-6, f"Peak {20 * np.log10(peak_loud):+.2f} dBFS exceeds the -1 dBFS ceiling"
    assert peak_single < 10 ** (-6 / 20), f"Single note peaked at {20 * np.log10(peak_single):+.2f} dBFS, inside the knee"
    return f"16 voices +6 dB: {20 * np.log10(peak_loud):+.2f} dBFS  single note: {20 * np.log10(peak_single):+.2f} dBFS"


def time_to_raw(seconds: float, maximum: float) -> float:
    """Normalised value of an envelope time parameter (0.001 s .. maximum, skew 0.3)."""
    return float(((seconds - 0.001) / (maximum - 0.001)) ** 0.3)


def max_step(audio: np.ndarray) -> float:
    """Largest sample-to-sample change on either channel: a click shows up as a big step."""
    return float(np.abs(np.diff(audio, axis=1)).max())


# A full-velocity sine at C4 changes by at most 2*pi*f/fs*A ~ 0.0093 per sample on its own.
CLICK_THRESHOLD = 0.02


class Params:
    """Context manager: set raw parameter values, restore them afterwards."""

    def __init__(self, plugin, **raw_values):
        self.plugin, self.raw_values = plugin, raw_values

    def __enter__(self):
        self.saved = {name: self.plugin.parameters[name].raw_value for name in self.raw_values}
        for name, value in self.raw_values.items():
            self.plugin.parameters[name].raw_value = value
        return self.plugin

    def __exit__(self, *exc):
        for name, value in self.saved.items():
            self.plugin.parameters[name].raw_value = value
        return False


def test_release_mid_decay(plugin):
    """Sustain 0, note released halfway through a 1 s decay: it fades over the release time.

    juce::ADSR (used before v1.4.0) recomputed its release rate from the sustain
    level on every block, so with Sustain 0 the note stopped dead - a click."""
    set_params(plugin)
    with Params(plugin, **{find_param(plugin, "sustain"): 0.0,
                           find_param(plugin, "decay"): time_to_raw(1.0, 5.0)}):
        msgs = [mido.Message("note_on", note=60, velocity=127, time=0.0),
                mido.Message("note_off", note=60, velocity=0, time=0.5)]
        audio = plugin.process(msgs, 1.2, SAMPLE_RATE, reset=True)
    off = int(0.5 * SAMPLE_RATE)
    tail = rms(audio[:, off + int(0.05 * SAMPLE_RATE): off + int(0.15 * SAMPLE_RATE)])   # 50-150 ms after
    step = max_step(audio)
    assert tail > 1e-3, f"Note stopped dead at its note-off — tail RMS {tail:.6f}"
    assert step < CLICK_THRESHOLD, f"Click: a {step:.4f} step between samples"
    return f"tail RMS={tail:.4f}  largest step={step:.4f}"


def test_sustain_change_while_held(plugin):
    """Turning Sustain from 100% to 20% while a note is held ramps the level instead of jumping."""
    set_params(plugin)
    sustain = find_param(plugin, "sustain")
    with Params(plugin, **{sustain: 1.0}):
        first = plugin.process([mido.Message("note_on", note=60, velocity=127, time=0.0)],
                               0.5, SAMPLE_RATE, reset=True)
        plugin.parameters[sustain].raw_value = 0.2
        second = plugin.process([], 0.3, SAMPLE_RATE, reset=False)
    audio = np.concatenate([first, second], axis=1)
    step = max_step(audio)
    later = float(np.abs(second[:, int(0.1 * SAMPLE_RATE):]).max())
    assert step < CLICK_THRESHOLD, f"Click: a {step:.4f} step when Sustain changed"
    assert later < 0.25 * 0.3, f"Level did not follow the new sustain (peak {later:.3f})"
    return f"largest step={step:.4f}  peak after the change={later:.3f}"


JUCE_XML_MAGIC = 0x21324356   # header of juce::AudioProcessor::copyXmlToBinary ("VC2!")
JUCE_BASE64 = ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+"


def juce_xml_from_binary(data: bytes) -> tuple[str, bytes]:
    """Split a copyXmlToBinary block into (xml text, bytes that follow it)."""
    magic, length = struct.unpack("<II", data[:8])
    assert magic == JUCE_XML_MAGIC, "not a JUCE binary XML block"
    return data[8:8 + length].rstrip(b"\0").decode("utf-8"), data[8 + length:]


def juce_xml_to_binary(xml: str) -> bytes:
    text = xml.encode("utf-8") + b"\0"
    return struct.pack("<II", JUCE_XML_MAGIC, len(text)) + text


def juce_base64_decode(text: str) -> bytes:
    """juce::MemoryBlock::fromBase64Encoding: "<size>.<chars>", 6 bits per char, least significant first."""
    size, chars = text.split(".", 1)
    n = sum(JUCE_BASE64.index(c) << (6 * i) for i, c in enumerate(chars))
    return n.to_bytes(int(size), "little")


def juce_base64_encode(data: bytes) -> str:
    n = int.from_bytes(data, "little")
    return f"{len(data)}." + "".join(JUCE_BASE64[(n >> (6 * i)) & 63] for i in range((len(data) * 8 + 5) // 6))


def state_xml(plugin) -> str:
    """The plugin's own saved state (what getStateInformation wrote), as XML text.

    pedalboard wraps a VST3's state as <VST3PluginState><IComponent>base64</IComponent>...,
    and JUCE's VST3 wrapper appends its private data after the plugin's block."""
    outer, _ = juce_xml_from_binary(plugin.raw_state)
    component = re.search(r"<IComponent>([^<]*)</IComponent>", outer).group(1)
    inner, _ = juce_xml_from_binary(juce_base64_decode(component))
    return inner


def load_state_xml(plugin, xml: str):
    """Replace the plugin's own state with `xml`, keeping pedalboard's and JUCE's wrapping."""
    outer, _ = juce_xml_from_binary(plugin.raw_state)
    component = re.search(r"<IComponent>([^<]*)</IComponent>", outer).group(1)
    _, private = juce_xml_from_binary(juce_base64_decode(component))
    encoded = juce_base64_encode(juce_xml_to_binary(xml) + private)
    plugin.raw_state = juce_xml_to_binary(outer.replace(component, encoded))


def sequencer_onsets(plugin) -> int:
    """Number of notes the sequencer plays in 4 s at 1/8, 120 BPM: the 16-step pattern once."""
    set_params(plugin, filter_on=False)
    fast = {find_param(plugin, "attack"): 0.0, find_param(plugin, "release"): 0.0,
            find_param(plugin, "sequencer", "rate"): 0.0}
    with SequencerOn(plugin, **fast):
        audio = plugin.process([], 4.0, SAMPLE_RATE, reset=True)
    return len(onset_times(audio))


def test_state_round_trip(plugin):
    """The pattern is saved in the state and restored; a state without one loads the default."""
    xml = state_xml(plugin)
    assert 'stateVersion="3"' in xml, "state has no stateVersion attribute (or not version 3)"
    assert "<Pattern>" in xml, "state has no pattern"

    # Only step 0 on: one note per pass through the pattern
    def step(i):
        return f'<Step on="{1 if i == 0 else 0}" note="60" velocity="100"/>'
    single = xml[:xml.index("<Pattern>")] + "<Pattern>" + "".join(step(i) for i in range(16)) + \
        xml[xml.index("</Pattern>"):]
    load_state_xml(plugin, single)
    one_step = sequencer_onsets(plugin)

    # Same state with the pattern removed: back to the default pattern (12 steps on)
    without = xml[:xml.index("<Pattern>")] + xml[xml.index("</Pattern>") + len("</Pattern>"):]
    load_state_xml(plugin, single)
    load_state_xml(plugin, without)
    default = sequencer_onsets(plugin)
    load_state_xml(plugin, xml)

    assert one_step == 1, f"Edited pattern did not load: {one_step} notes, expected 1"
    assert default == len(DEFAULT_PATTERN_ON_STEPS), \
        f"State without a pattern kept the previous one: {default} notes, expected {len(DEFAULT_PATTERN_ON_STEPS)}"
    return f"one-step pattern: {one_step} note  no pattern: {default} notes (default)"


SAMPLE_ELEMENT = re.compile(r"<Sample\b[^>]*/>")


def with_sample_element(xml: str, element: str | None) -> str:
    """The state XML with its <Sample .../> element replaced (or removed when element is None)."""
    replacement = element or ""
    if SAMPLE_ELEMENT.search(xml):
        return SAMPLE_ELEMENT.sub(lambda _: replacement, xml, count=1)
    return xml.replace("</Pattern>", "</Pattern>" + replacement, 1)


def sample_attributes(xml: str) -> dict:
    match = SAMPLE_ELEMENT.search(xml)
    assert match, "state has no <Sample> element"
    return dict(re.findall(r'(\w+)="([^"]*)"', match.group(0)))


def write_test_wav(path: str, seconds: float = 0.5, hz: float = 330.0, rate: int = 44100):
    import wave
    t = np.arange(int(seconds * rate)) / rate
    data = (0.5 * np.sin(2 * np.pi * hz * t) * 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(data.tobytes())


def test_sample_state(plugin):
    """The sample is saved in the state: embedded as FLAC, by path, or as the built-in pad.

    A file referenced by path loads and gets embedded; a second instance then
    restores it from the embedded copy even after the file is gone. A sample
    that can't be found keeps its reference (so saving doesn't lose it), and a
    v1.4 state without a <Sample> element loads the built-in pad."""
    import tempfile
    from xml.sax.saxutils import quoteattr
    original = state_xml(plugin)
    assert sample_attributes(original), "state has no <Sample> element"

    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as folder:
        wav = os.path.join(folder, "neon-test.wav")
        write_test_wav(wav)

        load_state_xml(plugin, with_sample_element(original, f'<Sample name="neon-test.wav" path={quoteattr(wav)}/>'))
        by_path = sample_attributes(state_xml(plugin))
        assert by_path.get("name") == "neon-test.wav" and "default" not in by_path, f"file by path did not load: {by_path}"
        assert re.fullmatch(r"[0-9a-f]{16}", by_path.get("hash", "")), f"no content hash: {by_path}"
        assert len(by_path.get("flac", "")) > 1000, "the loaded sample was not embedded as FLAC"
        saved = state_xml(plugin)

        os.remove(wav)   # from here on only the embedded copy exists
        other = load_plugin()
        load_state_xml(other, saved)
        restored = sample_attributes(state_xml(other))
        assert restored.get("hash") == by_path["hash"], f"embedded sample restored with a different hash: {restored.get('hash')}"
        del other

        missing_path = os.path.join(folder, "gone.wav")
        load_state_xml(plugin, with_sample_element(original,
                       f'<Sample name="gone.wav" path={quoteattr(missing_path)} hash="0123456789abcdef"/>'))
        kept = sample_attributes(state_xml(plugin))
        assert kept.get("path") == missing_path and kept.get("hash") == "0123456789abcdef" and "default" not in kept, \
            f"missing sample's reference was not kept: {kept}"
        set_params(plugin)
        still_plays = rms(render(plugin, [60], note_dur_s=0.3))
        assert still_plays > 1e-3, "plugin went silent after a missing sample"

    v14 = with_sample_element(original, None).replace('stateVersion="3"', 'stateVersion="2"')
    load_state_xml(plugin, v14)
    old = sample_attributes(state_xml(plugin))
    load_state_xml(plugin, original)
    assert old.get("default") == "1", f"v1.4 state did not load the built-in pad: {old}"
    return f"embedded FLAC {len(by_path['flac']) // 1024} KB, hash {by_path['hash']}; missing file kept; v1.4 state -> default pad"


def peak_frequency(audio: np.ndarray) -> float:
    """Frequency of the strongest FFT bin (mono mix, Hann window), interpolated between bins."""
    mono = (audio[0] + audio[1]) * np.hanning(audio.shape[1])
    spectrum = np.abs(np.fft.rfft(mono))
    k = int(np.argmax(spectrum[1:-1])) + 1
    a, b, c = np.log(spectrum[k - 1:k + 2] + 1e-12)
    offset = 0.5 * (a - c) / (a - 2 * b + c)
    return (k + offset) * SAMPLE_RATE / len(mono)


def test_sample_pitch(plugin):
    """Source = Sample plays the loaded file pitched across the keyboard; One-shot stops at the end.

    A 1 s C4 sine (261.63 Hz) is loaded through the state; with Root at C4,
    C3, C4 and C5 must sound at 130.8, 261.6 and 523.3 Hz (within 0.5%)."""
    import tempfile
    from xml.sax.saxutils import quoteattr
    original = state_xml(plugin)
    saved = {name: p.raw_value for name, p in plugin.parameters.items()}
    try:
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as folder:
            wav = os.path.join(folder, "c4.wav")
            write_test_wav(wav, seconds=1.0, hz=261.6256)
            load_state_xml(plugin, with_sample_element(original, f'<Sample name="c4.wav" path={quoteattr(wav)}/>'))

        set_params(plugin, filter_on=False)
        plugin.parameters["source"].raw_value = 0.5                     # Oscillator / Sample / Granular
        plugin.parameters["sample_root"].raw_value = 60 / 127
        plugin.parameters["sample_loop"].raw_value = 0.5                # Loop
        found = {}
        for note, expected in ((48, 130.81), (60, 261.63), (72, 523.25)):
            audio = plugin.process([mido.Message("note_on", note=note, velocity=100, time=0.0)], 1.5, SAMPLE_RATE, reset=True)
            found[note] = peak_frequency(audio[:, int(0.2 * SAMPLE_RATE):])
            assert abs(found[note] / expected - 1) < 0.005, f"note {note} played at {found[note]:.2f} Hz, expected {expected}"

        # One-shot: a 1 s sample held for 2 s goes silent after it ends (plus the 2 ms fade).
        plugin.parameters["sample_loop"].raw_value = 0.0
        audio = plugin.process([mido.Message("note_on", note=60, velocity=100, time=0.0)], 2.0, SAMPLE_RATE, reset=True)
        during, after = rms(audio[:, int(0.2 * SAMPLE_RATE):int(0.9 * SAMPLE_RATE)]), rms(audio[:, int(1.1 * SAMPLE_RATE):])
        assert during > 1e-2 and after < 1e-6, f"one-shot did not stop at the end: RMS during {during:.4f}, after {after:.6f}"
    finally:
        for name, value in saved.items():
            plugin.parameters[name].raw_value = value
        load_state_xml(plugin, original)
    return "  ".join(f"{n}: {f:.1f} Hz" for n, f in found.items()) + "  one-shot ends"


def test_granular(plugin):
    """Source = Granular plays a cloud of grains, pitched by the key, and repeats exactly.

    A C4 sine is loaded through the state. C5 played on the granular source
    must peak at 523 Hz, and two renders of the same MIDI (after a reset)
    must be identical: the grains are random but seeded."""
    import tempfile
    from xml.sax.saxutils import quoteattr
    original = state_xml(plugin)
    saved = {name: p.raw_value for name, p in plugin.parameters.items()}
    try:
        with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as folder:
            wav = os.path.join(folder, "c4.wav")
            write_test_wav(wav, seconds=2.0, hz=261.6256)
            load_state_xml(plugin, with_sample_element(original, f'<Sample name="c4.wav" path={quoteattr(wav)}/>'))

        set_params(plugin, filter_on=False)
        plugin.parameters["source"].raw_value = 1.0                     # Granular
        plugin.parameters["sample_root"].raw_value = 60 / 127
        plugin.parameters[find_param(plugin, "grain", "spray")].raw_value = 0.3
        plugin.parameters[find_param(plugin, "grain", "jitter")].raw_value = 0.2   # 20 cents
        msgs = [mido.Message("note_on", note=72, velocity=100, time=0.0)]
        first = plugin.process(msgs, 1.5, SAMPLE_RATE, reset=True)
        second = plugin.process(msgs, 1.5, SAMPLE_RATE, reset=True)
        level = rms(first[:, int(0.3 * SAMPLE_RATE):])
        pitch = peak_frequency(first[:, int(0.3 * SAMPLE_RATE):])
        assert level > 1e-2, f"granular source is silent (RMS {level:.5f})"
        assert abs(pitch / 523.25 - 1) < 0.01, f"C5 on the granular source peaks at {pitch:.1f} Hz, expected 523.3"
        difference = float(np.max(np.abs(first - second)))
        assert difference == 0.0, f"two renders of the same MIDI differ by up to {difference:.3g}: grains are not seeded"
    finally:
        for name, value in saved.items():
            plugin.parameters[name].raw_value = value
        load_state_xml(plugin, original)
    return f"RMS {level:.3f}  peak {pitch:.1f} Hz  repeat renders identical"


def test_keyboard_on_sequencer_channel(plugin):
    """A keyboard note on MIDI channel 16 survives the sequencer playing the same pitch.

    The sequencer plays on channel 16. Before v1.4.0 a keyboard sending on
    channel 16 shared it, so the sequencer's C3 note-off also stopped a held
    keyboard C3.

    The keyboard note starts 10 ms after the step. Through pedalboard's VST3
    host, a host note-on landing on the very sample of a sequencer note-on of
    the same pitch never reaches the synth; driving processBlock directly, the
    plugin plays both, so that case is a host artefact, not a plugin bug."""
    original = state_xml(plugin)
    set_params(plugin)
    with SequencerOn(plugin):
        xml = state_xml(plugin)
        steps = "".join(f'<Step on="{1 if i == 0 else 0}" note="48" velocity="100"/>' for i in range(16))
        load_state_xml(plugin, xml[:xml.index("<Pattern>")] + "<Pattern>" + steps + xml[xml.index("</Pattern>"):])
        held = [mido.Message("note_on", channel=15, note=48, velocity=100, time=0.01)]   # mido channel 15 = MIDI 16
        audio = plugin.process(held, 1.5, SAMPLE_RATE, reset=True)
    load_state_xml(plugin, original)
    # The sequencer's step-0 C3 ends after 62.5 ms plus a 300 ms release; the
    # held keyboard C3 must still sound well after that.
    later = rms(audio[:, int(0.6 * SAMPLE_RATE):])
    assert later > 1e-3, f"Keyboard note on channel 16 was cut off by the sequencer — RMS {later:.6f}"
    return f"held note RMS after the sequencer note ended={later:.4f}"


def test_midi_log_per_instance(plugin):
    """Two plugin instances logging MIDI write two separate files instead of fighting over one."""
    import gc
    import glob
    import tempfile
    with tempfile.TemporaryDirectory(ignore_cleanup_errors=True) as folder:
        base = os.path.join(folder, "midi.log")
        previous = os.environ.get("NEONSYNTH_MIDILOG")
        os.environ["NEONSYNTH_MIDILOG"] = base
        try:
            first, second = load_plugin(), load_plugin()
        finally:
            if previous is None:
                del os.environ["NEONSYNTH_MIDILOG"]
            else:
                os.environ["NEONSYNTH_MIDILOG"] = previous
        first.process([mido.Message("note_on", note=60, velocity=100, time=0.0)], 0.2, SAMPLE_RATE, reset=True)
        second.process([mido.Message("note_on", note=72, velocity=100, time=0.0)], 0.2, SAMPLE_RATE, reset=True)
        del first, second
        gc.collect()   # the plugins' destructors flush and close their logs
        # pedalboard creates extra probe instances while loading, which open
        # (empty) logs of their own, so only the logs that saw a note count.
        logs = sorted(glob.glob(os.path.join(folder, "midi*.log")))
        played = [(os.path.basename(f), open(f).read()) for f in logs]
        played = [(name, text) for name, text in played if "Note on" in text]
    assert len(played) == 2, f"Expected two logs with a note, found {[name for name, _ in played]} among {len(logs)} logs"
    assert all(text.count("Note on") == 1 for _, text in played), "Each log should hold exactly its own instance's note"
    return "two instances wrote " + " and ".join(name for name, _ in played)


def test_close_events_sample_accurate(plugin):
    """Two notes 20 samples apart in the same block each start on their own sample.

    juce::Synthesiser's default handles events less than 32 samples after the
    previous one together, so before v1.4.0 the second note started 20 samples
    early. With Width at 100%, C2 is hard left (silent on the right) and C6 hard
    right, so the right channel's first sample is the second note's start."""
    set_params(plugin)
    width = find_param(plugin, "width")
    saved = plugin.parameters[width].raw_value
    plugin.parameters[width].raw_value = 1.0
    first, second = 4410, 4430                      # samples
    msgs = [mido.Message("note_on", note=36, velocity=100, time=first / SAMPLE_RATE),
            mido.Message("note_on", note=84, velocity=100, time=second / SAMPLE_RATE)]
    audio = plugin.process(msgs, 0.3, SAMPLE_RATE, reset=True)
    plugin.parameters[width].raw_value = saved
    start_left = int(np.flatnonzero(np.abs(audio[0]) > 1e-7)[0])
    start_right = int(np.flatnonzero(np.abs(audio[1]) > 1e-7)[0])
    assert abs(start_left - first) <= 1, f"First note started at sample {start_left}, expected {first}"
    assert abs(start_right - second) <= 1, f"Second note started at sample {start_right}, expected {second}"
    return f"notes at samples {start_left} and {start_right} (expected {first}, {second})"


def test_no_stuck_note_on_mode_change(plugin):
    """A key pressed in Layer and released after switching to Transpose must still stop.

    Before v1.4.0 the note-off was filtered out with the note-ons, so the voice
    sustained forever. Transpose with no key held plays nothing, so any sound
    after the release time is the stuck keyboard note."""
    set_params(plugin)
    plugin.process([mido.Message("note_on", note=60, velocity=100, time=0.0)],
                   0.5, SAMPLE_RATE, reset=True)                       # Layer, sequencer off: key down
    mode = find_param(plugin, "sequencer", "mode")
    with SequencerOn(plugin, **{mode: 1.0}):                          # Transpose (last choice)
        audio = plugin.process([mido.Message("note_off", note=60, velocity=0, time=0.0)],
                               1.5, SAMPLE_RATE, reset=False)          # key up after the switch
    after_release = rms(audio[:, int(0.8 * SAMPLE_RATE):])             # release is 300 ms by default
    assert after_release < 1e-5, f"Note still sounding after its note-off — RMS {after_release:.6f}"
    return f"RMS after release={after_release:.7f}"


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
    ("No stuck note",     test_no_stuck_note_on_mode_change),
    ("Close events",      test_close_events_sample_accurate),
    ("Headroom",          test_headroom),
    ("Release mid-decay", test_release_mid_decay),
    ("Sustain change",    test_sustain_change_while_held),
    ("State round trip",  test_state_round_trip),
    ("Sample state",      test_sample_state),
    ("Sample pitch",      test_sample_pitch),
    ("Granular",          test_granular),
    ("Channel 16 key",    test_keyboard_on_sequencer_channel),
    ("Log per instance",  test_midi_log_per_instance),
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
    parser = argparse.ArgumentParser(description="NeonSynth test suite")
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
        os.environ["NEONSYNTH_MIDILOG"] = os.path.abspath(args.midilog)

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
