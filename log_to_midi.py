#!/usr/bin/env python3
"""Convert a NeonSynth --midilog text log into a Standard MIDI File (.mid).

Usage:
    python log_to_midi.py <input.log> [output.mid]

The log format (one event per line) is produced by NeonSynthProcessor.cpp:
    [6.262s] Note on A2 Velocity 51 Channel 1
    [6.752s] Note off A2 Velocity 0 Channel 1
    [6.802s] All notes off Channel 1
"""
import argparse
import re
import sys

import mido

LINE_RE = re.compile(
    r"\[(?P<time>[\d.]+)s\]\s+"
    r"Note (?P<kind>on|off) (?P<note>[A-Ga-g]#?-?\d+) "
    r"Velocity (?P<velocity>\d+) Channel (?P<channel>\d+)"
)
ALL_NOTES_OFF_RE = re.compile(r"\[(?P<time>[\d.]+)s\]\s+All notes off Channel (?P<channel>\d+)")

TICKS_PER_BEAT = 480
MICROSECONDS_PER_BEAT = 500000  # 120 BPM, matches log's real-time seconds 1:1 at this tempo


def seconds_to_ticks(seconds: float) -> int:
    beats = seconds * (MICROSECONDS_PER_BEAT / 1_000_000) ** -1
    return round(beats * TICKS_PER_BEAT)


def parse_log(path):
    events = []  # (seconds, mido.Message)
    active_notes = {}  # channel -> set of note numbers currently on

    with open(path, "r") as f:
        for line_num, line in enumerate(f, 1):
            # Older captures may contain stray NUL bytes between entries
            # (JUCE's writeString() null-terminates each call); strip them.
            line = line.replace("\x00", "").strip()
            if not line:
                continue

            m = LINE_RE.match(line)
            if m:
                seconds = float(m.group("time"))
                note_num = _note_name_to_number(m.group("note"))
                velocity = int(m.group("velocity"))
                channel = int(m.group("channel")) - 1  # MIDI channels are 0-indexed in mido
                kind = m.group("kind")

                msg_type = "note_on" if kind == "on" and velocity > 0 else "note_off"
                events.append((seconds, mido.Message(
                    msg_type, note=note_num, velocity=velocity, channel=channel)))

                notes_on_channel = active_notes.setdefault(channel, set())
                if msg_type == "note_on":
                    notes_on_channel.add(note_num)
                else:
                    notes_on_channel.discard(note_num)
                continue

            m = ALL_NOTES_OFF_RE.match(line)
            if m:
                seconds = float(m.group("time"))
                channel = int(m.group("channel")) - 1
                for note_num in list(active_notes.get(channel, set())):
                    events.append((seconds, mido.Message(
                        "note_off", note=note_num, velocity=0, channel=channel)))
                active_notes[channel] = set()
                continue

            print(f"warning: skipping unrecognised line {line_num}: {line}", file=sys.stderr)

    return events


NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def _note_name_to_number(name: str) -> int:
    m = re.match(r"([A-Ga-g])(#?)(-?\d+)", name)
    letter, sharp, octave = m.group(1).upper(), m.group(2), int(m.group(3))
    pitch_class = NOTE_NAMES.index(letter + sharp)
    # JUCE's MidiMessage::getNoteName uses octave numbering where C3 == MIDI note 60.
    return pitch_class + (octave + 2) * 12


def build_midi_file(events, output_path):
    events.sort(key=lambda e: e[0])

    mid = mido.MidiFile(ticks_per_beat=TICKS_PER_BEAT)
    track = mido.MidiTrack()
    mid.tracks.append(track)
    track.append(mido.MetaMessage("set_tempo", tempo=MICROSECONDS_PER_BEAT, time=0))

    last_ticks = 0
    for seconds, msg in events:
        ticks = seconds_to_ticks(seconds)
        delta = max(0, ticks - last_ticks)
        track.append(msg.copy(time=delta))
        last_ticks = ticks

    track.append(mido.MetaMessage("end_of_track", time=0))
    mid.save(output_path)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", help="path to the midi.log file")
    parser.add_argument("output", nargs="?", help="output .mid path (default: input with .mid extension)")
    args = parser.parse_args()

    output = args.output or re.sub(r"\.log$", "", args.input) + ".mid"

    events = parse_log(args.input)
    if not events:
        print("error: no MIDI events found in log", file=sys.stderr)
        sys.exit(1)

    build_midi_file(events, output)
    print(f"Wrote {len(events)} events to {output}")


if __name__ == "__main__":
    main()
