# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""
Checks that sessions saved with an earlier release still sound the same.

Renders a fixed set of sessions through two builds of the VST3, the earlier
release and the current one, and requires the audio to be identical sample for
sample. Every parameter added since a release starts at a neutral default, so a
session that never touches the new controls must render bit-identically: this
is the check behind the roadmap's "defaults neutral" rule.

The sessions only set parameters both builds have. Each build renders in its
own process, so the two plugins (which share a plugin ID) never meet.

    python tests/compare_versions.py OLD.vst3 NEW.vst3

Exits non-zero, naming the sessions that differ, if any does.
"""
import subprocess
import sys
import tempfile
from pathlib import Path

import numpy as np

SR = 48000


def notes(*events):
    """events: (note, on_seconds, off_seconds, velocity)"""
    import mido
    out = []
    for n, on, off, vel in events:
        out += [mido.Message("note_on", note=n, velocity=vel, time=on),
                mido.Message("note_off", note=n, time=off)]
    return out


CHORD = [(48, 0.0, 1.6, 100), (60, 0.1, 1.5, 90), (64, 0.2, 1.4, 70), (67, 0.3, 1.3, 50)]
LINE = [(36 + 7 * i, 0.25 * i, 0.25 * i + 0.2, 40 + 10 * i) for i in range(8)]

# name: (parameters, notes, seconds). Parameter names as pedalboard spells them.
SESSIONS = {
    "defaults-chord": ({}, CHORD, 2.2),
    "defaults-line": ({}, LINE, 2.5),
    "square-no-filter": (dict(waveform="Square", filter_enabled=False), CHORD, 2.2),
    "triangle-dark": (dict(waveform="Triangle", cutoff=300.0), LINE, 2.5),
    "saw-envelope": (dict(waveform="Sawtooth", attack=0.2, decay=0.4, sustain=0.3, release=0.8,
                          cutoff=5000.0, stereo_width=1.0), CHORD, 2.6),
    "saw-cutoff-low-gain": (dict(waveform="Sawtooth", cutoff=150.0, output_gain_db=-12.0, stereo_width=0.0), CHORD, 2.2),
    "loud-into-clipper": (dict(waveform="Sawtooth", output_gain_db=6.0, filter_enabled=False), CHORD, 2.2),
    "chop-noise-fold": (dict(waveform="Sawtooth", chop_overtone=150.0, chop_comb=3, noise=20.0,
                             fold_drive=6.0, fold_mix=70.0), CHORD, 2.2),
    "random-source": (dict(source="Random", chop_overtone=60.0, fold_mix=30.0), LINE, 2.5),
    "sample-loop": (dict(source="Sample", sample_loop="Loop", sample_start=0.1, sample_loop_start=0.3,
                         sample_loop_end=0.6, sample_crossfade=30.0), CHORD, 2.2),
    "sample-one-shot": (dict(source="Sample", sample_loop="One-shot", sample_root=48), LINE, 2.5),
    "sample-until-release": (dict(source="Sample", sample_loop="Loop until release"), CHORD, 2.6),
    "granular": (dict(source="Granular", grain_position=0.3, grain_scan=0.5, grain_size=120.0,
                      grain_density=60.0, grain_spray=0.4, grain_pitch_jitter=20.0), CHORD, 2.4),
    "granular-frozen": (dict(source="Granular", grain_scan=0.0, grain_size=40.0, grain_density=150.0), LINE, 2.5),
    # The sequencer in Transpose mode: it plays only while a key is held, so nothing sounds before the
    # render starts. (Layer mode with Sync off is not repeatable under pedalboard, which runs the
    # plugin while it applies parameters, so the free-running clock has already started: two
    # renders of the same build differ. StepSequencerTests cover its timing instead.)
    "sequencer-transpose": (dict(sequencer_sync=False, sequencer_mode="Transpose", sequencer_rate="1/8T",
                                 sequencer_on=True), [(64, 0.0, 2.0, 100)], 2.5),
    "sequencer-swing-gate": (dict(sequencer_sync=False, sequencer_mode="Transpose", sequencer_tempo=140.0,
                                  sequencer_rate="1/16", sequencer_swing=0.3, sequencer_gate=0.8, sequencer_on=True),
                             [(55, 0.0, 1.0, 100), (60, 1.2, 2.2, 100)], 2.6),
}


def render(vst: str, out: str) -> None:
    import pedalboard
    probe = pedalboard.load_plugin(vst)
    known = set(probe.parameters.keys())
    results, skipped = {}, {}
    for name, (params, events, seconds) in SESSIONS.items():
        plugin = pedalboard.load_plugin(vst)
        missing = [k for k in params if k not in known]
        if missing:
            skipped[name] = missing
            continue
        for key, value in params.items():
            setattr(plugin, key, value)
        results[name] = plugin.process(notes(*events), seconds, SR, buffer_size=512, reset=True)
    np.savez(out, **results)
    if skipped:
        print(f"{vst}: sessions skipped, parameters missing: {skipped}", file=sys.stderr)


def main() -> int:
    if len(sys.argv) == 4 and sys.argv[1] == "render":
        render(sys.argv[2], sys.argv[3])
        return 0
    if len(sys.argv) != 3:
        print(__doc__)
        return 2

    old_vst, new_vst = sys.argv[1], sys.argv[2]
    with tempfile.TemporaryDirectory() as tmp:
        outs = []
        for i, vst in enumerate((old_vst, new_vst)):
            out = str(Path(tmp) / f"render{i}.npz")
            subprocess.run([sys.executable, __file__, "render", vst, out], check=True)
            outs.append(np.load(out))
        old, new = outs

        failed = []
        for name in SESSIONS:
            if name not in old.files or name not in new.files:
                print(f"  SKIP  {name} (a parameter it sets is missing from one build)")
                continue
            a, b = old[name], new[name]
            if not np.isfinite(b).all():
                failed.append(name)
                print(f"  FAIL  {name}: the new build's output is not finite")
            elif a.shape == b.shape and np.array_equal(a, b):
                peak = float(np.max(np.abs(b)))
                print(f"  PASS  {name}: identical ({b.shape[1]} samples, peak {peak:.3f})")
            else:
                failed.append(name)
                if a.shape != b.shape:
                    print(f"  FAIL  {name}: shapes differ, {a.shape} vs {b.shape}")
                else:
                    diff = np.abs(a - b)
                    first = int(np.argmax(diff.max(axis=0) > 0))
                    print(f"  FAIL  {name}: max difference {diff.max():.3g}, first at sample {first} "
                          f"({first / SR:.4f} s), {int((diff > 0).sum())} samples differ")

    print(f"\n{'ALL IDENTICAL' if not failed else 'DIFFERENT: ' + ', '.join(failed)}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
