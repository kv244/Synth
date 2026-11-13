# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""
Renders the demo piece, docs/demo/neonsynth-demo.wav, from the NeonSynth VST3.
The WAV is not kept in git (it is in .gitignore). One copy, the latest, is the asset
of the 'demo' release, which the README links to; after a re-render replace it with
    gh release upload demo docs/demo/neonsynth-demo.wav --clobber

Every sound is NeonSynth: six instances of the plugin, one per part, each
rendered offline with pedalboard and summed. Nothing else touches the audio
except a gain per part, a fade at the end and peak normalisation.

    python docs/demo/make_demo.py path/to/NeonSynth.vst3 [out.wav]

The parts (12 bars at 100 BPM, Am - F - C - G):
  pad     Granular: a slow-scanning grain cloud of the built-in pad, wide, filter opening
  bass    Random source (16 drifting steps) through CHOP and the wavefolder, filter wobbling
  arp     Sawtooth, CHOP overtone and comb sweeping, folded, sixteenth notes
  seq     the built-in step sequencer in Transpose mode with swing, one held key per bar
  hats    Oscillator faded fully into NOISE, very short envelope
  lead    Sample: the built-in pad looped, a slow melody over the last four bars

Parameters that move during the piece are set every 25 ms between process calls.
The Random source and the grain cloud are random, so two renders differ slightly.
"""
import math
import sys
from pathlib import Path

import mido
import numpy as np
import pedalboard
from pedalboard.io import AudioFile

SR = 48000
BPM = 100.0
BEAT = 60.0 / BPM
BAR = 4 * BEAT
BARS = 12
TAIL = 4.0
LENGTH = BARS * BAR + TAIL
STEP = 0.025   # seconds between automation updates

# One chord per bar: root (MIDI, around C3), and the pad voicing.
CHORDS = [
    (45, [57, 60, 64, 71]),   # Am(add9)
    (41, [53, 57, 60, 64]),   # Fmaj7
    (48, [55, 60, 64, 67]),   # C
    (43, [55, 59, 62, 69]),   # G(add9)
]


def chord(bar):
    return CHORDS[bar % len(CHORDS)]


def plugin_path(vst):
    path = Path(vst).resolve()
    inner = path / "Contents" / "x86_64-win" / path.name
    return str(inner if sys.platform == "win32" and inner.is_file() else path)


def render(vst, params, events, automation=None):
    """events: (note, on_seconds, off_seconds, velocity). automation: seconds -> {parameter: value}."""
    plugin = pedalboard.load_plugin(vst)
    for key, value in params.items():
        setattr(plugin, key, value)

    # Work in whole samples: a message that lands a rounding error before the end of a chunk
    # would be dropped, so each one is given to the chunk its sample falls in.
    chunk_samples = int(round(STEP * SR))
    chunks = int(round(LENGTH / STEP))
    messages = [[] for _ in range(chunks)]
    for note, on, off, velocity in events:
        for when, message in ((on, mido.Message("note_on", note=note, velocity=velocity)),
                              (off, mido.Message("note_off", note=note))):
            sample = int(round(when * SR))
            index = min(chunks - 1, sample // chunk_samples)
            messages[index].append(message.copy(time=(sample - index * chunk_samples) / SR))

    out = []
    for index in range(chunks):
        if automation is not None:
            for key, value in automation(index * STEP).items():
                setattr(plugin, key, value)
        out.append(plugin.process(sorted(messages[index], key=lambda m: m.time), STEP, SR,
                                  buffer_size=256, reset=index == 0))
    return np.concatenate(out, axis=1)


def sweep(t, start, end, low, high):
    """Exponential glide from low to high between two times, held outside them."""
    x = min(1.0, max(0.0, (t - start) / (end - start)))
    return low * (high / low) ** x


def pad_part(vst):
    events = []
    for bar in range(BARS):
        for i, note in enumerate(chord(bar)[1]):
            events.append((note, bar * BAR + 0.03 * i, (bar + 1) * BAR - 0.05, 70 + 6 * i))
    return render(vst, dict(source="Granular", grain_position=0.25, grain_scan=0.15, grain_size=180.0,
                            grain_density=45.0, grain_spray=0.5, grain_pitch_jitter=14.0,
                            attack=1.2, decay=1.0, sustain=0.8, release=2.8, stereo_width=1.0, cutoff=500.0),
                  events,
                  lambda t: dict(cutoff=sweep(t, 0.0, 8 * BAR, 500.0, 5000.0),
                                 grain_position=0.25 + 0.2 * math.sin(2 * math.pi * t / (8 * BAR))))


def bass_part(vst):
    rhythm = [(0.0, 0.9, 112), (1.5, 0.4, 84), (2.0, 0.9, 104), (3.0, 0.4, 90), (3.5, 0.4, 76)]   # beats: start, length, velocity
    events = []
    for bar in range(2, BARS):
        root = chord(bar)[0] - 12
        for start, length, velocity in rhythm:
            note = root + (12 if start == 3.5 else 0)
            events.append((note, bar * BAR + start * BEAT, bar * BAR + (start + length) * BEAT - 0.02, velocity))
    return render(vst, dict(source="Random", chop_overtone=70.0, chop_comb=1, fold_drive=4.0, fold_mix=35.0,
                            attack=0.004, decay=0.25, sustain=0.55, release=0.12, stereo_width=0.0, cutoff=300.0),
                  events,
                  lambda t: dict(cutoff=320.0 * 2.0 ** (1.3 * (0.5 + 0.5 * math.sin(2 * math.pi * t / (2 * BEAT)))),
                                 fold_mix=sweep(t, 6 * BAR, 10 * BAR, 35.0, 80.0)))


def arp_part(vst):
    events = []
    for bar in range(4, BARS):
        tones = sorted(chord(bar)[1]) + [n + 12 for n in sorted(chord(bar)[1])]
        order = [0, 2, 4, 1, 5, 3, 6, 2, 7, 4, 6, 3, 5, 1, 4, 2]
        for i in range(16):
            on = bar * BAR + i * BEAT / 4
            events.append((tones[order[i]] + 12, on, on + BEAT / 4 * 0.7, 118 if i % 4 == 0 else 62 + 9 * (i % 3)))
    return render(vst, dict(waveform="Sawtooth", chop_overtone=0.0, chop_comb=2, fold_drive=5.0, fold_mix=45.0,
                            attack=0.002, decay=0.14, sustain=0.15, release=0.2, stereo_width=0.9, cutoff=1500.0),
                  events,
                  lambda t: dict(chop_overtone=100.0 + 95.0 * math.sin(2 * math.pi * (t - 4 * BAR) / (4 * BAR)),
                                 chop_comb=2 if t < 8 * BAR else 5,
                                 cutoff=sweep(t, 4 * BAR, 11 * BAR, 1200.0, 9000.0)))


def sequencer_part(vst):
    # One held key per bar; Transpose shifts the built-in pattern by (key - C4).
    events = [(chord(bar)[0] + 12 + 3, bar * BAR, (bar + 1) * BAR - 0.03, 96) for bar in range(6, BARS)]
    return render(vst, dict(waveform="Square", sequencer_sync=False, sequencer_mode="Transpose", sequencer_tempo=BPM,
                            sequencer_rate="1/16", sequencer_swing=0.22, sequencer_gate=0.45, sequencer_on=True,
                            noise=6.0, fold_drive=3.0, fold_mix=25.0,
                            attack=0.002, decay=0.09, sustain=0.2, release=0.08, stereo_width=0.4, cutoff=2600.0),
                  events,
                  lambda t: dict(cutoff=2600.0 * 2.0 ** (0.8 * math.sin(2 * math.pi * t / BAR))))


def hats_part(vst):
    events = []
    for bar in range(4, BARS):
        for eighth in range(8):
            on = bar * BAR + eighth * BEAT / 2
            if eighth % 2 == 1:                       # offbeat hat
                events.append((96, on, on + 0.03, 92))
            elif eighth in (2, 6):                    # backbeat: a longer, lower burst
                events.append((60, on, on + 0.11, 118))
            if bar >= 8 and eighth % 2 == 1:          # sixteenth ghost notes in the last four bars
                events.append((100, on + BEAT / 4, on + BEAT / 4 + 0.02, 48))
    return render(vst, dict(waveform="Square", noise=100.0, fold_drive=7.0, fold_mix=55.0,
                            attack=0.001, decay=0.05, sustain=0.0, release=0.06, stereo_width=0.7, cutoff=14000.0),
                  events)


def lead_part(vst):
    melody = [(76, 0.0, 3.0), (79, 3.0, 1.0), (77, 4.0, 2.5), (72, 6.5, 1.5),
              (76, 8.0, 2.0), (79, 10.0, 1.0), (84, 11.0, 1.0), (83, 12.0, 4.0)]   # note, start beat, beats
    start = 8 * BAR
    events = [(note, start + beat * BEAT, start + (beat + beats) * BEAT - 0.05, 100) for note, beat, beats in melody]
    return render(vst, dict(source="Sample", sample_loop="Loop", sample_root=72, sample_start=0.05,
                            sample_loop_start=0.2, sample_loop_end=0.7, sample_crossfade=40.0,
                            attack=0.15, decay=0.6, sustain=0.75, release=1.8, stereo_width=0.5, cutoff=3500.0),
                  events,
                  lambda t: dict(cutoff=sweep(t, 8 * BAR, 12 * BAR, 2200.0, 7000.0)))


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    vst = plugin_path(sys.argv[1])
    out = Path(sys.argv[2]) if len(sys.argv) > 2 else Path(__file__).with_name("neonsynth-demo.wav")

    parts = [("pad", pad_part, 0.75), ("bass", bass_part, 0.95), ("arp", arp_part, 0.55),
             ("seq", sequencer_part, 0.5), ("hats", hats_part, 0.4), ("lead", lead_part, 0.7)]
    mix = np.zeros((2, int(round(LENGTH / STEP)) * int(round(STEP * SR))), dtype=np.float64)
    for name, part, gain in parts:
        audio = part(vst)
        peak = float(np.max(np.abs(audio)))
        print(f"  {name:5s} peak {peak:.3f}, rms {float(np.sqrt(np.mean(audio ** 2))):.3f}")
        if not np.isfinite(audio).all() or peak == 0.0:
            print(f"  {name}: silent or not finite")
            return 1
        mix += gain * audio[:, :mix.shape[1]]

    fade = int(2.5 * SR)                                        # fade the tail out
    mix[:, -fade:] *= np.linspace(1.0, 0.0, fade) ** 2
    mix *= 10 ** (-1.0 / 20) / np.max(np.abs(mix))              # peak at -1 dBFS

    with AudioFile(str(out), "w", SR, 2, bit_depth=16) as f:
        f.write(mix.astype(np.float32))
    print(f"wrote {out} ({mix.shape[1] / SR:.1f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
