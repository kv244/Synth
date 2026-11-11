# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
"""Renders audio through the NeonSynth VST3 and draws the manual's measured figures.

    python docs/figures/render_plots.py --vst3 path/to/NeonSynth.vst3 [--out docs/images]

Figures written (SVG + 300 dpi PNG):
    fig02-waveforms                   the four oscillator waveforms and their spectra
    fig04-envelope                    the amplitude envelope, measured from a render
    fig05-filter                      a sawtooth before and after the low-pass filter
    fig07-sample-granular-schematic  sample and granular guide (schematic over the
                                      built-in pad's rendered waveform)

Every render: 48 kHz, a fresh plugin instance (reset), velocity 100, GAIN 0 dB,
WIDTH 0% (both channels identical; the left one is analysed), RUN off.
No render is normalised except where a figure says so. The settings used are
also written to render-settings.json beside the images.
See docs/figures/README.md for dependencies.
"""
import argparse
import json
import pathlib

import mido
import numpy as np
import pedalboard
from scipy.signal import windows

import figstyle as fs

SR = 48000
VELOCITY = 100
BASE = dict(source="Oscillator", output_gain_db=0.0, stereo_width=0.0, sequencer_on=False)


def render(vst, settings, notes, seconds):
    """notes: list of (midi note, on time s, off time s). Returns the left channel."""
    plugin = pedalboard.load_plugin(str(vst))
    for key, value in {**BASE, **settings}.items():
        setattr(plugin, key, value)
    for key, value in {**BASE, **settings}.items():          # every value must have landed
        got = getattr(plugin, key)
        ok = got == value if isinstance(value, (str, bool)) else abs(float(got) - value) <= 0.01 * max(0.1, abs(value))
        assert ok, (key, value, got)
    events = []
    for note, on, off in notes:
        events += [mido.Message("note_on", note=note, velocity=VELOCITY, time=on),
                   mido.Message("note_off", note=note, velocity=0, time=off)]
    audio = plugin.process(events, seconds, SR, reset=True)
    assert np.all(np.isfinite(audio))
    assert np.array_equal(audio[0], audio[1]), "WIDTH 0% should give identical channels"
    return audio[0].astype(np.float64)


def spectrum_dbfs(x):
    """Blackman-Harris windowed FFT, scaled so a full-scale sine reads 0 dBFS."""
    w = windows.blackmanharris(len(x))
    mag = np.abs(np.fft.rfft(x * w)) * 2.0 / w.sum()
    return np.fft.rfftfreq(len(x), 1.0 / SR), 20 * np.log10(np.maximum(mag, 1e-12))


def log_peaks(freq, db, lo=20.0, hi=24000.0, bands=1200):
    """Largest value in each of `bands` log-spaced bands: keeps every peak, shrinks the SVG."""
    edges = np.geomspace(lo, hi, bands + 1)
    idx = np.searchsorted(freq, edges)
    f, d = [], []
    for a, b in zip(idx[:-1], idx[1:]):
        if b > a:
            k = a + int(np.argmax(db[a:b]))
            f.append(freq[k])
            d.append(db[k])
    return np.array(f), np.array(d)


def freq_axis(ax):
    ax.set_xscale("log")
    ax.set_xlim(20, 24000)
    ax.set_xticks([20, 100, 1000, 10000, 20000])
    ax.set_xticklabels(["20", "100", "1k", "10k", "20k"])
    ax.set_xlabel("Frequency (Hz)")


# ---------------------------------------------------------------- figure 2
WAVES = [
    ("Sine", "Pure and round: one frequency, nothing above it."),
    ("Triangle", "Soft, flute-like: odd harmonics that fall away quickly."),
    ("Square", "Hollow, clarinet-like: odd harmonics only, falling more slowly."),
    ("Sawtooth", "Bright and buzzy: every harmonic, the most edge."),
]
FIG2 = dict(note=69, hz=440.0, seconds=2.0, hold=1.9, attack=0.005, decay=0.100, sustain=1.0, release=0.150,
            filter_enabled=False, analysis=(1.0, 0.5))   # analysis: start s, length s (0.5 s of steady sustain)


def fig02(vst, out):
    a0 = int(FIG2["analysis"][0] * SR)
    shown = int(round(SR * 3 / FIG2["hz"]))                   # three cycles
    renders = {}
    for name, _ in WAVES:
        s = dict(waveform=name, attack=FIG2["attack"], decay=FIG2["decay"], sustain=FIG2["sustain"],
                 release=FIG2["release"], filter_enabled=False)
        x = render(vst, s, [(FIG2["note"], 0.0, FIG2["hold"])], FIG2["seconds"])
        renders[name] = x[a0: a0 + int(SR * 0.5)]
    peak = max(np.max(np.abs(r)) for r in renders.values())
    ylim = np.ceil(peak * 1.15 * 20) / 20

    fig = fs.figure(205)
    gs = fig.add_gridspec(4, 2, width_ratios=[1, 1.45], hspace=0.95, wspace=0.28,
                          left=0.09, right=0.985, top=0.93, bottom=0.075)
    t_ms = np.arange(shown) / SR * 1000
    for row, (name, prompt) in enumerate(WAVES):
        x = renders[name]
        axw = fig.add_subplot(gs[row, 0])
        axw.axhline(0, color=fs.LIGHT, lw=0.6)
        axw.plot(t_ms, x[:shown], color=fs.INK, lw=1.1)
        axw.set_ylim(-ylim, ylim)
        axw.set_xlim(0, t_ms[-1])
        axw.set_ylabel("Level (FS)")
        axw.set_title(f"{name}: waveform", loc="left")
        if row == 3:
            axw.set_xlabel("Time (ms)")
        axs = fig.add_subplot(gs[row, 1])
        f, d = log_peaks(*spectrum_dbfs(x))
        axs.plot(f, d, color=fs.INK, lw=0.8)
        axs.set_ylim(-110, 0)
        axs.set_yticks([-100, -80, -60, -40, -20, 0])
        axs.set_ylabel("dBFS")
        freq_axis(axs)
        if row != 3:
            axs.set_xlabel("")
        axs.set_title(f"{name}: spectrum", loc="left")
        axw.text(0.0, -0.42 if row == 3 else -0.30, f"Listen for: {prompt}", transform=axw.transAxes,
                 fontsize=9, style="italic", va="top")
    fig.suptitle(f"A4 ({FIG2['hz']:.0f} Hz), velocity {VELOCITY}, filter off; same scales in every row; not normalised",
                 x=0.09, ha="left", y=0.985, fontsize=9, fontweight="normal")
    return fs.save(fig, out, "fig02-waveforms"), dict(peak_fs=float(peak), ylim_fs=float(ylim))


# ---------------------------------------------------------------- figure 4
FIG4 = dict(note=93, attack=0.300, decay=0.300, sustain=0.5, release=0.500, key_down=0.100, key_up=1.300,
            early_up=0.250, seconds=2.2, frame_ms=2.0)    # A6 (1760 Hz) sine: fast enough to follow in 2 ms frames


def envelope_of(x, frame):
    n = len(x) // frame
    return np.abs(x[: n * frame]).reshape(n, frame).max(axis=1), (np.arange(n) + 0.5) * frame / SR


def fig04(vst, out):
    p = FIG4
    s = dict(waveform="Sine", attack=p["attack"], decay=p["decay"], sustain=p["sustain"], release=p["release"],
             filter_enabled=False)
    frame = int(SR * p["frame_ms"] / 1000)
    full = render(vst, s, [(p["note"], p["key_down"], p["key_up"])], p["seconds"])
    early = render(vst, s, [(p["note"], p["key_down"], p["early_up"])], p["seconds"])
    env, t = envelope_of(full, frame)
    env_e, _ = envelope_of(early, frame)
    ref = env.max()                      # normalised: 1 = the attack peak of the full note
    env, env_e = env / ref, env_e / ref

    fig = fs.figure(118)
    gs = fig.add_gridspec(2, 1, height_ratios=[1.55, 1], hspace=0.62, left=0.12, right=0.985, top=0.89, bottom=0.1)
    ax = fig.add_subplot(gs[0])
    kd, ku = p["key_down"], p["key_up"]
    a_end, d_end, r_end = kd + p["attack"], kd + p["attack"] + p["decay"], ku + p["release"]
    for x0, x1, label in [(kd, a_end, "ATK\n300 ms"), (a_end, d_end, "DEC\n300 ms"), (ku, r_end, "REL\n500 ms")]:
        ax.axvspan(x0, x1, color=fs.LIGHT, alpha=0.55, lw=0)
        ax.annotate("", xy=(x0, 1.12), xytext=(x1, 1.12), arrowprops=dict(arrowstyle="<->", lw=0.8, color=fs.INK))
        ax.text((x0 + x1) / 2, 1.16, label, ha="center", va="bottom", fontsize=9)
    ax.plot(t, env, color=fs.INK, lw=1.3, label="Measured envelope")
    ax.axhline(p["sustain"], color=fs.ACCENT, ls=(0, (5, 3)), lw=1.0)
    ax.text(1.0, p["sustain"] - 0.05, "SUS = 50%: a level, not a time", color=fs.ACCENT, fontsize=8.5, ha="center", va="top")
    for x, label, ha, dx in [(kd, "key down", "right", -0.012), (ku, "key up", "left", 0.012)]:
        ax.axvline(x, color=fs.INK, ls=":", lw=0.9)
        ax.text(x + dx, 0.04, label, rotation=90, va="bottom", ha=ha, fontsize=8.5)
    ax.set_xlim(0, p["seconds"])
    ax.set_ylim(0, 1.45)
    ax.set_yticks([0, 0.5, 1.0])
    ax.set_ylabel("Level\n(1 = attack peak)")
    ax.set_title("Note held for 1.2 s", loc="left", pad=3)

    ax2 = fig.add_subplot(gs[1], sharex=ax)
    ax2.plot(t, env_e, color=fs.INK, lw=1.3)
    eu = p["early_up"]
    lvl = np.interp(eu, t, env_e)
    ax2.axvspan(eu, eu + p["release"], color=fs.LIGHT, alpha=0.55, lw=0)
    ax2.annotate("", xy=(eu, 0.62), xytext=(eu + p["release"], 0.62),
                 arrowprops=dict(arrowstyle="<->", lw=0.8, color=fs.INK))
    ax2.text(eu + 0.03, 0.68, "REL 500 ms, from the level reached", ha="left", va="bottom", fontsize=9)
    for x, label in [(kd, "key down"), (eu, "key up")]:
        ax2.axvline(x, color=fs.INK, ls=":", lw=0.9)
    ax2.text(eu + 0.012, 0.04, "key up", rotation=90, va="bottom", fontsize=8.5)
    ax2.text(kd - 0.012, 0.04, "key down", rotation=90, va="bottom", ha="right", fontsize=8.5)
    ax2.set_ylim(0, 1.0)
    ax2.set_yticks([0, 0.5, 1.0])
    ax2.set_ylabel("Level")
    ax2.set_xlabel("Time (s)")
    ax2.set_title("Released after 150 ms, during the attack", loc="left")
    fig.suptitle("ATK 300 ms, DEC 300 ms, SUS 50%, REL 500 ms; A6 sine, peak level per 2 ms frame",
                 x=0.12, ha="left", y=0.985, fontsize=9, fontweight="normal")
    meas = dict(attack_peak_fs=float(ref), level_at_early_release=float(lvl),
                sustain_measured=float(np.median(env[(t > d_end + 0.1) & (t < ku - 0.05)])),
                silent_after_release=bool(env[t > r_end + 0.01].max() < 1e-3))
    return fs.save(fig, out, "fig04-envelope"), meas


# ---------------------------------------------------------------- figure 5
FIG5 = dict(note=57, hz=220.0, cutoff=1000.0, seconds=2.0, hold=1.9, attack=0.005, decay=0.100, sustain=1.0,
            release=0.150, analysis=(1.0, 0.5))


def fig05(vst, out):
    p = FIG5
    a0, n = int(p["analysis"][0] * SR), int(p["analysis"][1] * SR)
    base = dict(waveform="Sawtooth", attack=p["attack"], decay=p["decay"], sustain=p["sustain"], release=p["release"])
    before = render(vst, {**base, "filter_enabled": False}, [(p["note"], 0.0, p["hold"])], p["seconds"])[a0:a0 + n]
    after = render(vst, {**base, "filter_enabled": True, "cutoff": p["cutoff"]},
                   [(p["note"], 0.0, p["hold"])], p["seconds"])[a0:a0 + n]
    shown = int(round(SR * 3 / p["hz"]))
    t_ms = np.arange(shown) / SR * 1000
    ylim = np.ceil(max(np.abs(before).max(), np.abs(after).max()) * 1.15 * 20) / 20

    fig = fs.figure(150)
    gs = fig.add_gridspec(2, 2, height_ratios=[1, 1.35], hspace=0.55, wspace=0.3,
                          left=0.115, right=0.985, top=0.92, bottom=0.09)
    for col, (x, title) in enumerate([(before, "Filter off (bypassed)"), (after, "Filter on, cutoff 1 kHz")]):
        ax = fig.add_subplot(gs[0, col])
        ax.axhline(0, color=fs.LIGHT, lw=0.6)
        ax.plot(t_ms, x[:shown], color=fs.INK, lw=1.1)
        ax.set_ylim(-ylim, ylim)
        ax.set_xlim(0, t_ms[-1])
        ax.set_xlabel("Time (ms)")
        ax.set_ylabel("Level (FS)")
        ax.set_title(title, loc="left")
    fb, db_b = spectrum_dbfs(before)
    fa, db_a = spectrum_dbfs(after)
    ax = fig.add_subplot(gs[1, :])
    f1, d1 = log_peaks(fb, db_b)
    f2, d2 = log_peaks(fa, db_a)
    ax.plot(f1, d1, color=fs.GREY, lw=0.9, label="Filter off")
    ax.plot(f2, d2, color=fs.INK, lw=0.9, ls=(0, (4, 1.5)), label="Filter on (dashed)")
    # Measured attenuation at each harmonic against a 2-pole Butterworth low-pass.
    harm = p["hz"] * np.arange(1, int(20000 / p["hz"]) + 1)
    k = np.round(harm / (SR / len(before))).astype(int)
    att = db_a[k] - db_b[k]
    ideal = -10 * np.log10(1 + (harm / p["cutoff"]) ** 4)
    ax.axvline(p["cutoff"], color=fs.ACCENT, lw=1.0)
    ax.text(p["cutoff"] * 1.06, -8, "Cutoff 1 kHz", color=fs.ACCENT, fontsize=9, va="top")
    ax.set_ylim(-110, 0)
    ax.set_yticks([-100, -80, -60, -40, -20, 0])
    ax.set_ylabel("dBFS")
    freq_axis(ax)
    ax.legend(loc="lower left", frameon=False)
    ax.set_title("Spectrum: 12 dB per octave above the cutoff", loc="left")
    fig.suptitle(f"Sawtooth A3 ({p['hz']:.0f} Hz), velocity {VELOCITY}; same scales in every panel; not normalised",
                 x=0.12, ha="left", y=0.985, fontsize=9, fontweight="normal")
    meas = dict(max_deviation_from_butterworth_db_below_10k=float(np.max(np.abs((att - ideal)[harm < 10000]))),
                attenuation_at_1k_db=float(np.interp(p["cutoff"], harm, att)),
                attenuation_at_4k_db=float(np.interp(4 * p["cutoff"], harm, att)))
    return fs.save(fig, out, "fig05-filter"), meas


# ---------------------------------------------------------------- figure 6
FIG6 = dict(note=60, seconds=4.3, hold=4.2, start=0.10, loop_start=0.30, loop_end=0.75, xfade_ms=50.0,
            pos=0.55, spray=0.25, size_ms=150.0, grains=7)


def minmax(x, bins):
    n = len(x) // bins
    r = x[: n * bins].reshape(bins, n)
    return r.min(axis=1), r.max(axis=1)


def fig06(vst, out):
    p = FIG6
    s = dict(source="Sample", waveform="Sine", attack=0.001, decay=0.001, sustain=1.0, release=0.010,
             filter_enabled=False, sample_root=60, sample_loop="One-shot", sample_start=0.0)
    pad = render(vst, s, [(p["note"], 0.0, p["hold"])], p["seconds"])
    nz = np.nonzero(np.abs(pad) > 1e-6)[0]
    pad = pad[nz[0]: nz[-1] + 1]                # the pad as played at its root key
    length_s = len(pad) / SR
    lo, hi = minmax(pad, 700)
    xs = np.linspace(0, 1, len(lo))
    amp = max(-lo.min(), hi.max()) * 1.05

    fig = fs.figure(198)
    gs = fig.add_gridspec(3, 2, height_ratios=[1.05, 1.05, 0.95], width_ratios=[1.45, 1], hspace=0.75, wspace=0.28,
                          left=0.03, right=0.985, top=0.95, bottom=0.04)

    def wave(ax, title):
        ax.fill_between(xs, lo, hi, color=fs.GREY, lw=0)
        ax.set_xlim(0, 1)
        ax.set_ylim(-amp * 2.0, amp * 1.75)
        ax.set_yticks([])
        ax.spines["left"].set_visible(False)
        ax.set_xticks([0, 0.25, 0.5, 0.75, 1])
        ax.set_xticklabels(["0%", "25%", "50%", "75%", "100%"])
        ax.set_title(title, loc="left")

    # (a) Sample
    ax = fig.add_subplot(gs[0, 0])
    wave(ax, "a  Sample: where a note reads")
    top, bot = amp * 1.7, -amp * 1.4
    ax.axvline(p["start"], color=fs.INK, lw=1.4)
    ax.text(p["start"], top, " START", va="top", fontsize=9, fontweight="bold")
    for x, label in [(p["loop_start"], "L.START"), (p["loop_end"], "L.END")]:
        ax.axvline(x, color=fs.AMBER, lw=1.4, ls=(0, (6, 2)))
        ax.text(x, top, " " + label, va="top", fontsize=9, fontweight="bold", color=fs.AMBER)
    ax.annotate("", xy=(p["loop_start"], -amp * 1.25), xytext=(p["loop_end"], -amp * 1.25),
                arrowprops=dict(arrowstyle="->", lw=1.0, color=fs.AMBER, connectionstyle="arc3,rad=0.25"))
    ax.text((p["loop_start"] + p["loop_end"]) / 2, -amp * 1.8, "loop: jumps back after the crossfade",
            ha="center", va="center", fontsize=8.5, style="italic")
    ax.annotate("", xy=(p["start"] + 0.12, -amp * 1.25), xytext=(p["start"], -amp * 1.25),
                arrowprops=dict(arrowstyle="->", lw=1.0))
    ax.text(p["start"] + 0.005, -amp * 1.1, "reads forward", fontsize=8.5, style="italic", va="bottom")

    # (b) the crossfade, zoomed in
    axz = fig.add_subplot(gs[0, 1])
    xf = np.linspace(0, 1, 200)
    axz.plot(xf, np.cos(xf * np.pi / 2), color=fs.INK, lw=1.3, label="material before L.END")
    axz.plot(xf, np.sin(xf * np.pi / 2), color=fs.AMBER, lw=1.3, ls=(0, (5, 2)), label="material from L.START")
    axz.set_xlim(0, 1)
    axz.set_ylim(0, 1.6)
    axz.set_xticks([0, 1])
    axz.set_xticklabels(["L.END\n− XFADE", "L.END"])
    axz.get_xticklabels()[0].set_ha("left")
    axz.get_xticklabels()[1].set_ha("right")
    axz.set_yticks([0, 1])
    axz.set_ylabel("Gain")
    axz.legend(loc="upper center", frameon=False, fontsize=8, handlelength=2.2)
    axz.set_title("b  Loop crossfade", loc="left")

    # (c) Granular
    ax = fig.add_subplot(gs[1, 0])
    wave(ax, "c  Granular: where grains are taken")
    half = p["spray"] / 2
    ax.axvspan(p["pos"] - half, p["pos"] + half, color=fs.LIGHT, alpha=0.9, lw=0, zorder=0)
    ax.axvline(p["pos"], color=fs.ACCENT, lw=1.6)
    ax.text(p["pos"], top, " POS", va="top", fontsize=9, fontweight="bold", color=fs.ACCENT)
    ax.annotate("", xy=(p["pos"] - half, amp * 1.3), xytext=(p["pos"] + half, amp * 1.3),
                arrowprops=dict(arrowstyle="<->", lw=0.8))
    ax.text(p["pos"] + half + 0.01, amp * 1.3, "spray range\n(SPRAY 25%)", va="center", fontsize=8.5, style="italic")
    rng = np.random.default_rng(4)
    size = p["size_ms"] / 1000 / length_s
    starts = np.sort(rng.uniform(p["pos"] - half, p["pos"] + half - size, p["grains"]))
    for i, g0 in enumerate(starts):
        u = np.linspace(0, 1, 60)
        bell = (4 * u * (1 - u)) ** 2
        base = bot * 0.35 - (i % 3) * amp * 0.18
        ax.fill_between(g0 + u * size, base, base + bell * amp * 0.16, color=fs.ACCENT, alpha=0.75, lw=0)
    ax.annotate("", xy=(p["pos"] - 0.20, -amp * 1.25), xytext=(p["pos"], -amp * 1.25),
                arrowprops=dict(arrowstyle="->", lw=1.4, color=fs.INK))
    ax.text(0.02, -amp * 1.8, "SCAN < 0: the selection moves backwards",
            ha="left", va="center", fontsize=8.5, style="italic")

    axg = fig.add_subplot(gs[1, 1])
    u = np.linspace(0, 1, 200)
    axg.fill_between(u, 0, (4 * u * (1 - u)) ** 2, color=fs.ACCENT, alpha=0.75, lw=0)
    axg.annotate("", xy=(0, -0.12), xytext=(1, -0.12), arrowprops=dict(arrowstyle="<->", lw=0.8),
                 annotation_clip=False)
    axg.text(0.5, -0.2, "SIZE (5–500 ms)", ha="center", va="top", fontsize=9)
    axg.annotate("", xy=(0.85, 1.12), xytext=(0.2, 1.12), arrowprops=dict(arrowstyle="->", lw=1.0))
    axg.text(0.52, 1.2, "each grain plays forwards", ha="center", fontsize=8.5, style="italic")
    axg.set_xlim(-0.05, 1.05)
    axg.set_ylim(-0.45, 1.5)
    axg.axis("off")
    axg.set_title("d  One grain", loc="left")

    # (e) loop modes
    ax = fig.add_subplot(gs[2, :])
    ax.set_xlim(0, 1.04)
    ax.set_ylim(-0.5, 3.2)
    ax.axis("off")
    ax.set_title("e  Loop modes (Sample only): the read position over time", loc="left")
    rows = [("One-shot", 2.3), ("Loop", 1.3), ("Loop until release", 0.3)]
    held, x0 = 0.62, 0.22
    for label, y in rows:
        ax.text(0.0, y, label, va="center", fontsize=9, fontweight="bold")
    def seg(y, a, b, **k):
        ax.annotate("", xy=(b, y), xytext=(a, y), arrowprops=dict(arrowstyle="->", lw=1.1, **k))
    seg(2.3, x0, 0.97)
    ax.text(0.975, 2.3, "end", va="center", fontsize=8.5)
    for i in range(4):
        seg(1.3, x0 + i * 0.19, x0 + (i + 1) * 0.19 - 0.01, color=fs.AMBER)
    ax.text(0.97, 0.95, "…until the voice ends", ha="right", va="top", fontsize=8.5, style="italic")
    for i in range(2):
        seg(0.3, x0 + i * 0.19, x0 + (i + 1) * 0.19 - 0.01, color=fs.AMBER)
    seg(0.3, x0 + 2 * 0.19, 0.97)
    ax.text(0.975, 0.3, "end", va="center", fontsize=8.5)
    ax.axvline(held, ymin=0.1, ymax=0.84, color=fs.INK, ls=":", lw=0.9)
    ax.text(held, -0.4, "key up", ha="center", fontsize=8.5)
    ax.text(x0, -0.4, "key down", ha="center", fontsize=8.5)
    ax.text(0.99, 2.95, "short repeating arrows = passes from L.START to L.END", ha="right", fontsize=8.5, color=fs.AMBER)
    meas = dict(pad_length_s=float(length_s))
    return fs.save(fig, out, "fig07-sample-granular-schematic"), meas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vst3", required=True)
    ap.add_argument("--out", default=str(pathlib.Path(__file__).resolve().parents[1] / "images"))
    ap.add_argument("--revision", default="unknown", help="git revision of the plugin build, recorded in the JSON")
    args = ap.parse_args()
    out = pathlib.Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    record = dict(revision=args.revision, pedalboard=pedalboard.__version__, sample_rate=SR, velocity=VELOCITY,
                  common=BASE, figures={})
    for name, fn, settings in [("fig02-waveforms", fig02, FIG2), ("fig04-envelope", fig04, FIG4),
                               ("fig05-filter", fig05, FIG5), ("fig07-sample-granular-schematic", fig06, FIG6)]:
        files, meas = fn(args.vst3, out)
        record["figures"][name] = dict(settings=settings, measured=meas)
        print(name, json.dumps(meas))
    (out / "render-settings.json").write_text(json.dumps(record, indent=2) + "\n")


if __name__ == "__main__":
    main()
