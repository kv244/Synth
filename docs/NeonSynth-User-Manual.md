# NeonSynth User Manual

NeonSynth is a learning synthesizer: a place to hear what a waveform sounds like, see what changes when you shape it, and explore the code that makes it happen. Its oscillator, sample, random, and granular engines provide different starting points for that exploration, with source code available to read, build, and modify ([NeonSynth project](https://github.com/kv244/Synth)).

For musicians, begin with one note and one waveform, without stacked oscillator layers obscuring what you are hearing. For coders, follow the same sound into the C++ implementation, from waveform generation through envelope, filter, and output; the interface and the code become two views of the same instrument ([voice architecture](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

**Instrument:** NeonSynth by Razvan Julian Petrescu / AudioDSP  
**Edition:** 1.0, prepared 7 November 2026  
**Applies to:** NeonSynth v1.5.0, source revision `9a21c0ed95c283c0027158ee6df686dc7fa03b6d`  
**Formats:** VST3 instrument and standalone application; a Windows installer is supplied for this release ([v1.5.0 release](https://github.com/kv244/Synth/releases/tag/v1.5.0)).

### About this manual

The supplied running demonstration was reviewed visually, and the instructions were checked against the release source code and project documentation. This is not a certification of compatibility with every audio device or DAW; the application was not independently installed or auditioned while preparing this manual.

Suggested settings in the sound-design chapter are starting points, not factory presets or measured audio examples. Begin at a low listening level and adjust by ear.

## Contents

- [Getting started](#getting-started)
- [Learning by listening](#learning-by-listening)
- [Finding your way around](#finding-your-way-around)
- [Choosing a sound source](#choosing-a-sound-source)
- [Shaping a sound](#shaping-a-sound)
- [Sample playback](#sample-playback)
- [Granular synthesis](#granular-synthesis)
- [Step sequencer](#step-sequencer)
- [Output and visual feedback](#output-and-visual-feedback)
- [Saving, automation, and MIDI](#saving-automation-and-midi)
- [Sound-design starting points](#sound-design-starting-points)
- [Troubleshooting](#troubleshooting)
- [Learning through the source code](#learning-through-the-source-code)
- [Technical reference](#technical-reference)

## Getting started

### Install the Windows release

Download `NeonSynth-1.5.0-Windows-Setup.exe` from the [v1.5.0 release page](https://github.com/kv244/Synth/releases/tag/v1.5.0). The installer provides the VST3 instrument and an optional standalone application ([installation documentation](https://github.com/kv244/Synth)).

| Component | Default location |
|---|---|
| VST3 instrument | `C:\Program Files\Common Files\VST3\NeonSynth.vst3` |
| Standalone application | `C:\Program Files\AudioDSP\Neon Synth\NeonSynth.exe` |

These are the documented default installation locations; the standalone installation directory can be changed in the installer ([installation documentation](https://github.com/kv244/Synth)).

### Make your first sound without a MIDI keyboard

Use this sequence for an uncomplicated standalone check. The built-in sequencer generates the notes, so no external MIDI input is required ([standalone instructions](https://github.com/kv244/Synth)).

1. Launch `NeonSynth.exe`.
2. Open **Options → Audio/MIDI Settings**.
3. Select your audio output device and enable its output channels.
4. Set **Source** to **Oscillator** and **Waveform** to **Sawtooth**.
5. Set **GAIN** to approximately −12 dB and begin with your speakers or headphones turned down.
6. Select sequencer mode **Layer**, turn **SYNC** off, and set **TEMPO** to approximately 120 BPM.
7. Press **RUN**.

The factory pattern should now provide a repeating starting point. **RUN** is off on a new instance, so the instrument is initially silent unless it receives MIDI notes; in standalone operation, the sequencer can use its own clock ([default parameters and pattern](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

Once sound is present, move **Cutoff** to hear the brightness change, then try the other waveforms. Press **RUN** again to stop the pattern; notes already sounding are released rather than held indefinitely ([sequencer and control guide](https://github.com/kv244/Synth)).

### Play from a MIDI controller

In standalone mode, enable the controller in **Options → Audio/MIDI Settings**, leave **RUN** off, and play the keyboard. In a DAW, load NeonSynth on an instrument/MIDI track, route the controller to that track, and enable the host's monitoring or record-arm function as required by the host ([MIDI setup guide](https://github.com/kv244/Synth)).

The main editor does not provide an on-screen piano keyboard. If you have no hardware controller, use the sequencer or route a virtual MIDI keyboard into the instrument ([standalone usage guide](https://github.com/kv244/Synth)).

### Use NeonSynth in a DAW

Rescan your DAW's VST3 instruments if NeonSynth does not appear after installation, then look for the **AudioDSP** manufacturer. With **SYNC** enabled, press **RUN** in NeonSynth and start the DAW transport to hear its sequence; the **HOST** tempo readout indicates that the host clock is being used ([VST3 and sequencer instructions](https://github.com/kv244/Synth)).

NeonSynth is an instrument, not an effect for processing a live audio input. To use recorded audio as a sound source, load it into the Sample or Granular engine ([processor implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

## Learning by listening

### One sound source per voice, not one note at a time

In Oscillator mode, each voice starts with one selected waveform, making it straightforward to compare shapes without a second oscillator or layered patch complicating the result. NeonSynth remains polyphonic: it can play up to 16 notes simultaneously; “one source per voice” does not mean “only one note at a time,” and the Granular engine builds its selected source from multiple grains ([voice architecture](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h), [instrument features](https://github.com/kv244/Synth)).

The exercises below deliberately use one held note at first. Listen before looking at the display, then use Scope and Spectrum to connect what you hear with what you see.

### Listening exercise: meet the four waveforms

Use the following setup to minimise distractions. This is an educational comparison, not a factory preset or a loudness-calibrated test.

1. Turn **RUN** off and select **Source → Oscillator**.
2. Turn the **FILTER** off, set **WIDTH** to 0%, and begin with **GAIN** around −18 dB.
3. Set **ATK** to approximately 5 ms, **DEC** to 100 ms, **SUS** to 100%, and **REL** to 150 ms.
4. Hold a comfortable mid-range note, such as A4, using a similar playing velocity for each comparison.
5. Compare **Sine**, **Triangle**, **Square**, and **Sawtooth** without changing the note or envelope.
6. Repeat the comparison in **SCOPE**, then in **SPECTRUM**.

The waveform choices and displays are supplied by the [instrument controls](https://github.com/kv244/Synth). If one option sounds louder, lower the listening level rather than treating extra loudness as an improvement; focus on differences in character.

Ask yourself: which sounds smoothest, which feels hollow, and which has the most edge? These are listening prompts, not right-or-wrong labels; the point is to develop your own recognition of each shape.

### Listening exercise: hear what a filter removes

Keep the note and envelope unchanged, choose Sawtooth, then enable the filter and slowly move Cutoff down and back up. Switch to Sine and repeat the sweep, observing both the audible change and the spectrum display.

This isolates the relationship between the source and the filter: you are changing one stage while leaving the others alone. Bypass the filter periodically so your ear has an unfiltered reference; the available cutoff and bypass controls are documented in the [filter guide](https://github.com/kv244/Synth).

### Listening exercise: separate tone from articulation

Keep one waveform and one cutoff setting, then compare a fast attack with a slow one; next compare short and long releases. Finally, lower Sustain and adjust Decay, listening to how the same source can behave like a pluck or a sustained instrument.

Only after those differences become familiar should you add chords, sequencing, and other source engines. This is a suggested learning order rather than a restriction on the instrument.

## Finding your way around

The interface groups sound generation and shaping on the left, output monitoring on the right, sample controls below them, and sequencing along the bottom. Control colours provide additional guidance: cyan for source/output functions, amber for the envelope and sequencer, and magenta for filtering and granular parameters ([interface implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h)).

| Area | What you do there |
|---|---|
| OSCILLATOR | Select the sound source and, when applicable, its waveform. |
| ENVELOPE | Set the level shape of each note with ATK, DEC, SUS, and REL. |
| FILTER | Enable the low-pass filter and adjust its cutoff. |
| VOICES | See which of the 16 voices are active. |
| OUTPUT | Choose Scope or Spectrum, watch the meters, and set gain and width. |
| SAMPLE | Load audio, choose sample-loop behaviour, or shape a granular cloud. |
| SEQUENCER | Start a pattern, edit its steps, and set timing and keyboard interaction. |

The area names and functions above follow the [editor layout](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h). Drag the window edge or resize corner to change the interface size; its supported range is 75–200% of the base size, with the aspect ratio preserved ([interface implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h)).

Click **?** for quick help. Click the overlay or press **Esc** to return to the controls ([quick-help instructions](https://github.com/kv244/Synth)).

## Choosing a sound source

**Source** selects one of four engines, not four layers mixed together. The selected engine feeds a common envelope, filter, and output path ([voice architecture](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

| Source | Description | Useful starting applications |
|---|---|---|
| Oscillator | Plays Sine, Square, Triangle, or Sawtooth. | Bass, leads, simple sustained tones. |
| Sample | Plays the loaded recording at pitches determined by MIDI notes and ROOT. | Pitched recordings, looped pads, short sampled sounds. |
| Granular | Builds a cloud of short fragments from the loaded sample. | Frozen textures, moving atmospheres, scattered fragments. |
| Random | Plays a pitched, evolving waveform made from 16 random steps per cycle. | Changing digital tones and animated timbres. |

Engine behaviour is documented in the [project guide](https://github.com/kv244/Synth); the applications above are suggested uses rather than built-in patch categories.

### Oscillator waveforms

Select **Oscillator** first, then choose **Sine**, **Square**, **Triangle**, or **Sawtooth** from **Waveform**. The waveform selector is dimmed when another source is selected because it does not choose the waveform of that other engine ([source controls](https://github.com/kv244/Synth)).

For a first comparison, follow the one-note exercise above with the filter bypassed. Then enable it and repeat the comparison, so you can distinguish the waveform itself from the effect of filtering.

### Random is pitched, not a noise generator

The Random source repeats a 16-step shape at the note's frequency while gradually replacing its step values. Its pitch remains defined while its timbre changes, rather than behaving as an unpitched white-noise source ([Random source documentation](https://github.com/kv244/Synth)).

### When a source change takes effect

The engine is selected at note-on, so changing **Source** does not immediately convert every held note into the newly selected engine. Release and retrigger notes, or let the sequencer trigger its next steps, to hear the new selection ([note-start implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

## Shaping a sound

### Envelope: ATK, DEC, SUS, REL

The amplitude envelope sets how each note grows, settles, and fades. Attack, Decay, and Release are times; Sustain is a level, not a duration ([envelope parameters](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

| Control | Meaning | Range | Initial value |
|---|---|---|---|
| ATK | Time to rise to the attack peak. | 1 ms–5 s | 10 ms |
| DEC | Time to move from the peak to the sustain level. | 1 ms–5 s | 100 ms |
| SUS | Level maintained while the note remains held. | 0–100% | 70% |
| REL | Fade time after note release. | 1 ms–10 s | 300 ms |

Ranges and initial values are taken from the [parameter definitions](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp). They describe a new instance, not settings inherited from a saved session.

For a pluck, try a short attack, a moderate decay, and low sustain; for a pad, try a slower attack and longer release. If a sample ends first, a long envelope cannot create audio beyond the available sample, unless playback is looped ([sample/voice behaviour](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

The envelope graph shows the shape and the positions of sounding voices. Sustain changes are smoothed, and a note released during Attack or Decay still begins its release from the level it has reached ([envelope and visualiser documentation](https://github.com/kv244/Synth)).

### Low-pass filter

Enable **ON** to use the filter, then lower **Cutoff** to remove more high-frequency content; disable **ON** to bypass it. The cutoff range is 20 Hz–20 kHz, initially 2 kHz, although the engine limits the actual frequency to 45% of the audio sample rate when necessary ([filter parameters](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp), [voice filter implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

The filter is a two-pole low-pass with fixed Q, not a user-adjustable resonant ladder filter. There is no separate resonance control or dedicated filter envelope in this version ([voice filter implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

## Sample playback

### Load a recording

Click **LOAD**, or drag one WAV, AIFF, or FLAC file onto the window, then select **Source → Sample**. The sample label shows the name, duration, and sample rate; its tooltip exposes the full path when available, and the adjacent status area reports loading problems ([sample controls and loading](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h)).

Click **PAD** to restore the built-in sample. This is a four-second pad rooted at MIDI note 60, labelled C4, and provides a ready-made source for both Sample and Granular operation ([built-in sample](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SampleLoader.h)).

### What happens to an imported file

NeonSynth averages the input channels to mono, keeps at most the first 60 seconds, and normalises the resulting sample to a −1 dBFS peak before preparing its internal 24-bit representation. Importing a stereo file therefore does not preserve its original stereo image; stereo output is subsequently created through voice panning ([sample preparation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SampleData.h)).

This processing does not overwrite the original file. Empty or effectively silent input is rejected, including a stereo recording that cancels to silence when its channels are averaged ([sample loading](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SampleLoader.h), [sample preparation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SampleData.h)).

### Set the pitch, start, and loop

The Sample control row contains the following parameters. Percentage positions are fractions of the prepared sample's full length ([parameter definitions](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

| Control | Range / initial value | Function |
|---|---|---|
| ROOT | MIDI 0–127 / 60 (C4) | The key that plays the sample at its original playback rate. |
| START | 0–100% / 0% | Where a new note starts reading. |
| L.START | 0–100% / 0% | Beginning of the repeat region. |
| L.END | 0–100% / 100% | End of the repeat region. |
| XFADE | 5–50 ms / 10 ms | Requested crossfade duration at the loop join. |

Values and labels follow the [parameter definitions](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp) and [sample interface](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h).

ROOT tells the instrument how to map the recording; it does not analyse the recording's musical pitch. Set it to match your source, then play above or below it to transpose playback ([sample engine documentation](https://github.com/kv244/Synth)).

Drag the cyan start marker or the amber loop brackets in the waveform display, or use the corresponding knobs. Clicking away from the markers sets the start position, and white lines show the read positions of active Sample voices ([waveform editing guide](https://github.com/kv244/Synth)).

### Choose a loop mode

| Mode | Behaviour |
|---|---|
| One-shot | Reads forward to the end without looping. Note release still invokes the amplitude envelope's Release stage. |
| Loop | Repeats the selected region while the voice sounds, including its release tail. |
| Loop until release | Repeats while the key is held, then exits looping and continues towards the end while the envelope releases. |

These modes control sample playback, not whether the amplitude envelope applies; a short Release can make the sound fade before the file's end is heard ([sample-mode documentation](https://github.com/kv244/Synth), [voice note-release implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

For a smooth sustained sound, begin with a steady section of the recording and adjust XFADE by ear. Root, start, loop, and crossfade settings are captured when a Sample note begins, so retrigger the note after editing them; an already sounding note continues with its captured settings ([voice parameter handling](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

## Granular synthesis

### Build a cloud from the sample

Select **Source → Granular** after loading a recording or restoring **PAD**. The engine plays many short, smoothly windowed fragments of that sample, and the Sample knob row changes to ROOT, POS, SCAN, SIZE, DENS, SPRAY, and JITTER ([granular interface](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h), [grain engine](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/GrainCloud.h)).

| Control | Range | Initial value | What to listen for |
|---|---|---|---|
| ROOT | MIDI 0–127 | 60 (C4) | The sample's pitch reference. |
| POS | 0–100% | 50% | The source region around which fragments are selected. |
| SCAN | −2× to +2× | 0×, frozen | Movement of the grain-selection position through the file. |
| SIZE | 5–500 ms | 80 ms | Short fragments versus longer pieces of the recording. |
| DENS | 1–200 grains/s | 30 grains/s | Sparse events versus overlapping clouds. |
| SPRAY | 0–100% | 10% | How widely fragment positions scatter around the selection position. |
| JITTER | 0–100 cents | 0 cents | Random pitch variation, up to the chosen amount in either direction. |

Ranges and initial values come from the [parameter definitions](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp); their playback meaning follows the [grain engine](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/GrainCloud.h).

### Freeze, scan, and scatter

At **SCAN = 0**, the selection position does not advance automatically, but the grains still play and new grains continue to be generated. Positive SCAN moves selection forward through the file; negative SCAN moves it backwards, which is not the same as reversing audio playback inside each grain ([scan implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/GrainCloud.h)).

Drag the magenta position marker to explore different regions, then add SPRAY to vary which nearby fragments are chosen. Grain settings shape newly generated grains rather than rewriting fragments already playing, so changes unfold across the cloud instead of replacing it abruptly ([grain parameter behaviour](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/GrainCloud.h)).

Granular mode uses the sample as a whole rather than the Sample engine's loop region; the loop-mode selector is dimmed in this mode. ROOT is shared with Sample playback, but Sample START, loop boundaries, and XFADE are not granular loop controls ([granular settings](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp), [interface behaviour](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h)).

Each voice supports up to 32 simultaneous grains; further grain onsets are skipped while all slots are busy. The display shows up to eight grain positions per voice, so visible ticks are a guide to activity, not a complete grain counter ([grain capacity](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/GrainCloud.h), [display telemetry](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

## Step sequencer

### Start and synchronise a pattern

The sequencer repeats a fixed 16-step pattern, with one note, velocity, and enabled/disabled state per step. It generates notes internally for NeonSynth; it is not a MIDI-output sequencer for controlling other instruments ([sequencer guide](https://github.com/kv244/Synth)).

| Control | Operation |
|---|---|
| RUN | Enables or stops sequencing; initially off. |
| SYNC | Uses the host tempo and transport when the host supplies them; initially on. |
| TEMPO | Internal clock, 40–240 BPM; initially 120 BPM. |
| Rate | 1/8, 1/16, 1/32, 1/8T, or 1/16T; initially 1/16. |
| Mode | Layer, Replace, or Transpose; initially Layer. |
| SWING | 0–100%; delays alternate steps by up to half a step; initially 0%. |
| GATE | 5–100% of the nominal step duration; initially 50%. |

Controls and initial values follow the [sequencer documentation](https://github.com/kv244/Synth) and [parameter definitions](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp).

When **SYNC** is enabled in a compatible DAW, the sequence follows the host's transport position and stops when the host stops. In standalone operation, or when no usable host timing is available, it uses TEMPO instead; turning SYNC off explicitly selects the internal clock ([transport handling](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

For a straightforward 16-step loop in 4/4, begin with **1/16**, zero swing, and a 50% gate. The **T** rate options provide triplet subdivisions; reducing GATE shortens the interval before note-off, but a long envelope release can still make successive notes overlap ([sequencer controls](https://github.com/kv244/Synth)).

### Edit the step grid

| Gesture | Result |
|---|---|
| Click a step | Switches it on or off. |
| Drag up or down | Changes pitch in semitone steps. |
| Shift-drag or right-drag | Changes velocity. |
| Mouse wheel | Raises or lowers pitch by one semitone per wheel event. |
| Shift + mouse wheel | Changes velocity in increments of four. |

The grid supports MIDI notes 0–127 and enabled-step velocities 1–127; the cyan outline marks the current step, and each cell's lower bar indicates its velocity ([step-grid implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SequencerView.h)).

To create a new pattern, first switch unwanted steps off, drag the remaining steps to their pitches, and adjust their velocities for accents. There is no separate pattern-bank or pattern-length control in this version ([step-grid implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SequencerView.h), [editor controls](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h)).

### Choose how the keyboard interacts

- **Layer:** Keyboard notes play alongside the pattern.
- **Replace:** The running sequencer supplies notes; incoming keyboard note-ons do not play additional notes.
- **Transpose:** Hold a key to trigger the pattern at an offset relative to MIDI note 60, labelled C4; the most recently pressed held key determines the offset.

These behaviours apply while RUN is on; with the sequencer off, keyboard note input plays normally. In Transpose mode, hold C4 for the unshifted pattern, C5 for an octave up, or C3 for an octave down, remembering that some hosts use different octave names for the same MIDI note numbers ([keyboard and transpose handling](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

If RUN is lit but Transpose mode is silent, hold a key and, with host synchronisation active, start the DAW transport. This mode intentionally waits for keyboard input rather than free-running independently ([transpose and transport documentation](https://github.com/kv244/Synth)).

## Output and visual feedback

### Gain and stereo width

**GAIN** controls output level from −48 dB to +6 dB, initially 0 dB; **WIDTH** controls pitch-based voice placement from 0% to 100%, initially 60%. At zero width, voices are centred; increasing width places notes below C4 towards the left and notes above C4 towards the right, reaching the extremes two octaves either side at full width ([output parameters](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp), [voice panning](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

WIDTH is not chorus, delay, or a stereo image restored from an imported recording. A single C4 note remains centred even at full width, so try notes on either side of it or a chord when evaluating the control ([voice panning](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

### Scope, spectrum, voices, and meters

- **SCOPE:** Displays the output waveform and uses the lowest active note as its pitch reference.
- **SPECTRUM:** Displays frequency content with the filter-response curve.
- **VOICES:** Provides one light per voice, with brightness following its envelope.
- **Envelope graph:** Shows the note shape with moving voice-position dots.
- **L/R meter:** Shows output levels, peak hold, and a clip indicator; click to reset the meter.

These visualisers are described in the [display guide](https://github.com/kv244/Synth); the scope and spectrum use the stereo mid signal, `(L + R) / 2`, rather than separate left/right traces ([display processing](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h)).

The output soft clipper leaves signals below −6 dBFS unchanged and constrains peaks to below approximately −1 dBFS. Its indicator signals peak shaping, not a guarantee against clipping later in the DAW chain or excessive listening volume; lower GAIN when you want less peak shaping ([output protection](https://github.com/kv244/Synth)).

## Saving, automation, and MIDI

### Save the sound and pattern

In a DAW, save your project or use the host's plugin-state/preset facilities to retain NeonSynth's settings. Its saved state includes the parameters, the 16-step pattern, the selected Scope/Spectrum view, and sample information; individual step values are saved state rather than separate automatable host parameters ([state implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

The custom editor has no dedicated patch browser or Save Patch button. Use the host's facilities where available, and verify a saved project by closing and reopening it before relying on it for an important session ([editor controls](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h)).

### Keep sample-based sessions portable

Custom samples are stored inside the plugin state as prepared mono 24-bit FLAC when the encoded payload is no larger than 10 MiB. Larger samples, or samples whose embedded encoding is unavailable, depend on their saved file path instead ([embedding rules](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SampleLoader.h)).

On restoration, NeonSynth tries the embedded audio first, then the saved path; if neither can be loaded, it uses the built-in pad and retains the missing-file reference. Hearing a pad instead of your recording can therefore indicate a missing sample rather than a lost pattern or envelope setting ([sample restoration](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

For dependable archiving, keep original samples with the DAW project even when embedding appears to work. For a missing recording, load the intended file again, retrigger notes, and resave the session.

### Automate controls

Use the DAW's automation facilities for the exposed parameters, including source, waveform, envelope, cutoff, output controls, sample settings, granular settings, and sequencer timing/mode controls. The Scope/Spectrum choice and individual sequencer steps are not independent automation parameters ([parameter and state definitions](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp), [view-state handling](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h)).

Automation follows each control's playback semantics: a source or Sample-loop change takes effect at a subsequent note, while granular changes shape newly generated grains. Do not expect every automated change to rewrite audio already sounding ([voice parameter handling](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h), [grain settings](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/GrainCloud.h)).

### MIDI scope and limitations

NeonSynth responds to MIDI note pitch and velocity, with up to 16 simultaneous voices and voice stealing when that pool is occupied. Incoming keyboard channels are consolidated internally so keyboard notes and sequencer notes do not cut each other off solely because they share a pitch ([MIDI routing](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

Pitch-wheel and general controller callbacks do not provide voice-level modulation in this version, so do not expect pitch bend or the modulation wheel to alter the sound. There is no dedicated MIDI-learn interface or external MIDI-output function exposed by the instrument ([voice callbacks](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h), [editor controls](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h), [project configuration](https://github.com/kv244/Synth)).

## Sound-design starting points

These are suggested manual settings, not supplied factory presets or claims about auditioned results. Start with GAIN around −12 dB, keep playback modest, and adjust each sound to the recording, notes, and mix you are using.

### Rounded sequencer bass

- **Source:** Oscillator; Waveform: Sawtooth.
- **Envelope:** ATK 5 ms, DEC 180 ms, SUS 35%, REL 100 ms.
- **Filter:** On; Cutoff approximately 700 Hz.
- **Output:** WIDTH 0%.
- **Sequencer:** Layer, 1/16, GATE 40%; start with the default pattern.

Try raising Cutoff gradually while the pattern runs, then compare Square with Sawtooth. For a softer phrase, increase Release slightly before increasing Gate.

### Slow sampled pad

- **Source:** Sample; press PAD and set ROOT to C4.
- **Loop:** Loop; START 0%, L.START approximately 25%, L.END approximately 75%, XFADE 20 ms.
- **Envelope:** ATK 800 ms, DEC 600 ms, SUS 75%, REL 2 s.
- **Filter:** On; Cutoff approximately 3 kHz.
- **Output:** WIDTH 70%; RUN off.

Play a chord and hold it long enough to hear the sustain region, then release it. Retrigger the notes after changing the loop boundaries.

### Frozen granular texture

- **Source:** Granular; begin with PAD or a short textured recording.
- **Cloud:** POS 50%, SCAN 0, SIZE 150 ms, DENS 35/s, SPRAY 10%, JITTER 5 cents.
- **Envelope:** ATK 500 ms, DEC 300 ms, SUS 80%, REL 2 s.
- **Filter:** On; Cutoff approximately 4 kHz.

Hold a note and move POS slowly to explore the recording. Keep SCAN at zero initially so you can hear position changes without automatic movement.

### Moving granular atmosphere

- **Source:** Granular; load a recording with contrasting sections.
- **Cloud:** POS 20%, SCAN +0.20×, SIZE 100 ms, DENS 45/s, SPRAY 25%, JITTER 12 cents.
- **Envelope:** ATK 300 ms, DEC 500 ms, SUS 70%, REL 1.5 s.
- **Output:** WIDTH approximately 60%; RUN off.

Try a negative SCAN value to move selection backwards through the recording, then reduce DENS for a more fragmented texture. Reduce the number of held notes if the processing load becomes excessive.

### Evolving random pluck

- **Source:** Random.
- **Envelope:** ATK 3 ms, DEC 250 ms, SUS 0%, REL 120 ms.
- **Filter:** On; Cutoff approximately 2.5 kHz.
- **Sequencer:** 1/8 or 1/16; GATE 35%; a few velocity accents.

Let the pattern repeat before deciding how much variation you want. Try opening the filter for more of the waveform's changing character.

## Troubleshooting

### No sound

Work through the signal path rather than changing several settings at once. In standalone mode, confirm the output device and enabled channels; in a DAW, confirm the instrument track, monitoring, and MIDI routing ([setup guidance](https://github.com/kv244/Synth)).

- **No voice lights:** Check that MIDI notes are arriving, or select Layer, turn SYNC off, and press RUN for an internal-clock test.
- **RUN on but silent:** Check for Transpose mode waiting for a held key, or SYNC waiting for the DAW transport.
- **Voice lights but little output:** Raise Cutoff, check GAIN, and check for a very slow Attack or zero Sustain after the decay has completed.
- **Very low notes barely audible:** Try a higher octave or Sawtooth/Square instead of Sine/Triangle, particularly on laptop speakers.

These checks reflect the documented [no-sound guidance](https://github.com/kv244/Synth), [sequencer modes](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp), and [voice controls](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h).

### A control seems to do nothing

- **Waveform dimmed:** Select Oscillator; it does not change Sample, Granular, or Random.
- **Cutoff dimmed:** Enable the filter.
- **Loop selector dimmed:** You are using Granular, whose controls do not use the Sample loop mode.
- **Source or loop edits not heard:** Release and retrigger the note.
- **WIDTH not obvious:** Try a chord spanning notes below and above C4.
- **Pitch/modulation wheel has no effect:** These are not implemented as voice modulation controls.

These are expected behaviours of the [editor](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthEditor.h) and [voice engine](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h), rather than necessarily a device fault.

### Sample sounds different or will not load

Import processing folds channels to mono, normalises the peak, and limits duration to 60 seconds; this can explain a changed stereo image, changed level, or shortened recording. Check the displayed status, try a supported WAV/AIFF/FLAC file with clearly audible content, and check the source file's mono compatibility if stereo cancellation is suspected ([sample preparation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SampleData.h), [loader](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SampleLoader.h)).

If a reopened project plays the default pad, reload the intended file and save the project again. A missing external sample can cause the engine to fall back to the pad when no usable embedded copy is available ([restoration behaviour](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

### Notes overlap, cut short, or sound distorted

A long Release can overlap several sequencer steps, while a very short Release can fade out a one-shot sample before it reaches its end. More than 16 occupied voices requires voice stealing, and high combined levels can engage the output soft clipper ([polyphony and envelope behaviour](https://github.com/kv244/Synth), [voice implementation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h)).

Try shortening Release, reducing the number of simultaneous notes, or lowering GAIN. When shaping a sampled sustain, select a steady loop region and compare different crossfade settings before assuming the recording itself is unsuitable.

### Crackles or dropouts

As a troubleshooting approach, increase the audio buffer size, reduce the number of held notes and granular density/size, and compare with a single Oscillator voice. Heavy granular settings can keep many grains active in every voice; the repository's benchmark figures are development-machine measurements, not a performance guarantee for your system ([performance documentation](https://github.com/kv244/Synth)).

### NeonSynth is missing from the DAW

Check the VST3 installation location and run the DAW's plugin rescan. This release is configured for VST3 and Standalone, not AU, AAX, or VST2; before version 1.4 it was called BasicSynth, and the installer removes the old BasicSynth VST3 bundle to prevent duplicate listings ([format and upgrade documentation](https://github.com/kv244/Synth)).

## Learning through the source code

### An instrument you can inspect

The source is available in the public [NeonSynth repository](https://github.com/kv244/Synth), alongside the build instructions, tests, and support utilities. Treat the instrument as an executable explanation: hear a change, find its control in the code, and trace how its value affects the samples being produced.

You do not need to understand the whole application before investigating one part. Start with the relationship between a single waveform and a single voice, then work outwards towards polyphony, sample playback, and the sequencer.

### A guided reading path

| Start here | What to look for | Connect it to the instrument |
|---|---|---|
| `Oscillator.h` | Waveform generation, phase, and band-limiting. | Compare Sine, Triangle, Square, and Sawtooth on the same note. |
| `VoiceEnvelope.h` | Envelope stages and their time/level behaviour. | Move ATK, DEC, SUS, and REL while watching the graph. |
| `NeonSynthVoice.h` | One note's source, envelope, filter, and panning. | Follow a note from source generation to stereo placement. |
| `NeonSynthProcessor.cpp` | Parameter ranges/defaults, MIDI routing, output processing, and state. | Match a displayed control to its stored value and playback effect. |
| `SampleData.h` / `SampleLoader.h` | File preparation, mono conversion, normalisation, and embedded audio. | Compare an imported recording with its prepared sample. |
| `SampleReader.h` | Pitch mapping and crossfaded loop playback. | Investigate ROOT, loop boundaries, and XFADE. |
| `GrainCloud.h` | Grain scheduling, windows, scan positions, and SIMD paths. | Connect SIZE, DENS, and SCAN to individual fragments. |
| `StepSequencer.h` / `SequencerView.h` | Timing engine versus editable grid. | See how a visual step becomes a timed note event. |
| `Visualisers.h` / `NeonSynthEditor.h` | Display processing and control attachments. | Understand how sound and parameter state reach the screen. |
| `tests/` | Focused engine tests and the render benchmark. | Read the expected behaviour before changing its implementation. |

The reading path follows the files and architecture in the [source repository](https://github.com/kv244/Synth). It is an editorial guide, not a requirement to read every file in this order.

### Make a small change, then test it

A useful first experiment is to change a default envelope time in `createParameterLayout()`, rebuild, and open a fresh instance to hear the result. Existing saved sessions can restore their own settings, so they are not a reliable way to check that a new default has taken effect ([parameter definitions and state restoration](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

For a deeper exercise, inspect why a waveform change waits for a cycle boundary or how grain size and density affect overlap. Make one change at a time, work on a branch, and run the relevant tests before comparing the result with the original; the repository documents dedicated oscillator, envelope, sample, granular, sequencer, and real-time-safety tests ([test documentation](https://github.com/kv244/Synth)).

Source availability is an invitation to inspect and experiment, not a promise that every edit is safe for real-time audio. In particular, avoid adding file I/O, blocking work, or heap allocation to the audio callback; the project includes a real-time-safety test specifically checking rendering-time allocation/deallocation ([test and architecture documentation](https://github.com/kv244/Synth)).

### Working with AI coding tools

Use AI assistance to ask focused questions about an existing function, propose a small change, or help explain a failing test. Keep responsibility for the result: compare the answer with the actual implementation, compile it, test it, and listen at a safe level rather than assuming plausible code will produce correct audio.

The project identifies Claude-assisted development, but Claude is not a runtime requirement for playing the instrument. Build dependencies and application behaviour are described in the [repository documentation](https://github.com/kv244/Synth).

## Technical reference

### Signal path

The voice engine places its amplitude envelope before the low-pass filter and panner; voices are then summed, followed by output gain and soft clipping. This is the implemented path, rather than a generic synthesis diagram ([voice engine](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h), [processor](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp)).

```text
MIDI keyboard / DAW notes ─┐
                          ├─> voice allocation, up to 16 voices
Internal step sequencer ──┘

Per voice:
selected source → amplitude envelope → low-pass filter/bypass → pitch-based pan

Combined output:
voice sum → output gain → soft clipper → mono/stereo output and visualisers
```

### Behaviour worth remembering

| Feature | Implementation detail |
|---|---|
| Source changes | Selected when a note starts. |
| Sample playback settings | Captured when a note starts. |
| Granular settings | Shape subsequent grains; position remains live. |
| Oscillator waveform changes | Applied at a cycle boundary. |
| Filter cutoff, gain, and panning | Smoothed to reduce abrupt changes. |
| Sequencer events | Scheduled at sample offsets and rendered at their MIDI event positions. |
| Random / granular variation | Seeded for reproducible renders from the same reset state and event sequence. |
| Imported sample audio | Prepared mono audio; original file is not modified. |

These details follow the [voice engine](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthVoice.h), [processor](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/NeonSynthProcessor.cpp), [grain engine](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/GrainCloud.h), and [sample preparation](https://github.com/kv244/Synth/blob/9a21c0ed95c283c0027158ee6df686dc7fa03b6d/SampleData.h).

### Optional MIDI diagnostics

Launch the standalone application with `--midilog`, or set `NEONSYNTH_MIDILOG` before starting a host, to log received MIDI messages. Use an absolute path to avoid uncertainty about the working directory; logging is a diagnostic facility, not an audio recorder ([MIDI logging documentation](https://github.com/kv244/Synth)).

```powershell
& "C:\Program Files\AudioDSP\Neon Synth\NeonSynth.exe" --midilog "C:\Users\YourName\Documents\NeonSynth-midi.log"
```

The included `log_to_midi.py` utility converts a captured text log into a MIDI file, while `test_midi.py` can render demonstration audio when its optional dependencies are installed. These are developer/support tools rather than controls in the instrument editor ([project utilities](https://github.com/kv244/Synth)).

### Building from source

The repository specifies C++17, CMake 3.21 or newer, and a separately supplied JUCE checkout; its current README identifies JUCE 9.0.0. Follow the version-specific build instructions when setting up your development environment ([build documentation](https://github.com/kv244/Synth)).

```bash
cmake -S . -B build -DJUCE_DIR="/path/to/JUCE" -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

Use the repository's build instructions for platform dependencies and optional tests. The supplied Windows release should not be taken as evidence that prebuilt macOS or Linux installers are available ([build and release documentation](https://github.com/kv244/Synth)).

### Reporting a reproducible problem

Record the application version, operating system, standalone/DAW context, audio sample rate and buffer size, selected source, RUN/SYNC/mode settings, and a short sequence of actions that reproduces the issue. For sample problems, include the file format, length, channel count, displayed status message, and whether it fails on first load or after reopening a saved session.

Keep a copy of the project and source sample before experimenting with a workaround. A small reproducible example is more useful than a screenshot of unexplained silence.
