// NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
// SPDX-License-Identifier: AGPL-3.0-only
// Licensed under the GNU Affero General Public License v3; see LICENSE.

/*
    Parameter layout: fails if a parameter was renamed, moved, re-ranged or
    removed, or its default or choice list changed.

    Hosts store automation and sessions by parameter ID and normalised value, so
    any of those changes silently breaks projects saved with an earlier version.
    The roadmap's rules allow only one change: new parameters appended at the
    end. This test writes one line per parameter (ID, version hint, type, name,
    range and skew, default, steps, choices) and compares the lines with the
    reference in tests/parameter-layout.txt:

      - every reference line must still be there, unchanged and in its place;
      - lines after the reference's last one are parameters appended since; the
        test passes and lists them, and the reference is updated on purpose:
            ParameterLayoutTests --write

    Build with CMake:  cmake -B build -DJUCE_DIR=... -DNEONSYNTH_BUILD_TESTS=ON
                       cmake --build build --target ParameterLayoutTests
    Run the resulting ParameterLayoutTests executable; it exits non-zero on failure.
*/
#include "NeonSynthProcessor.h"
#include <cstdio>

namespace
{
    juce::String number (double value)
    {
        char text[32];
        std::snprintf (text, sizeof (text), "%.6g", value);   // six digits: the same on MSVC and glibc
        return text;
    }

    juce::String describe (int index, const juce::RangedAudioParameter& p)
    {
        juce::String line;
        line << index << " " << p.getParameterID() << " v" << p.getVersionHint() << " \"" << p.getName (100) << "\" ";

        const auto& range = p.getNormalisableRange();
        const double defaultValue = p.convertFrom0to1 (p.getDefaultValue());

        if (auto* choice = dynamic_cast<const juce::AudioParameterChoice*> (&p))
            line << "choice [" << choice->choices.joinIntoString (", ") << "] default " << choice->choices[juce::roundToInt (defaultValue)];
        else if (dynamic_cast<const juce::AudioParameterBool*> (&p) != nullptr)
            line << "bool default " << (defaultValue >= 0.5 ? "on" : "off");
        else
            line << (dynamic_cast<const juce::AudioParameterInt*> (&p) != nullptr ? "int" : "float")
                 << " " << number (range.start) << " to " << number (range.end)
                 << " step " << number (range.interval)
                 << " skew " << number (range.skew) << (range.symmetricSkew ? " symmetric" : "")
                 << " default " << number (defaultValue);

        if (p.getLabel().isNotEmpty())
            line << " label \"" << p.getLabel() << "\"";
        if (! p.isAutomatable())
            line << " not-automatable";
        return line;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInit;   // the processor's editor code needs the message manager

    NeonSynthProcessor processor;
    juce::StringArray lines;
    for (auto* param : processor.getParameters())
    {
        auto* ranged = dynamic_cast<juce::RangedAudioParameter*> (param);
        if (ranged == nullptr)
        {
            std::printf ("FAIL  parameter %d is not a RangedAudioParameter\n", lines.size());
            return 1;
        }
        lines.add (describe (lines.size(), *ranged));
    }

    const auto reference = juce::File (NEONSYNTH_TESTS_DIR).getChildFile ("parameter-layout.txt");

    if (argc > 1 && juce::String (argv[1]) == "--write")
    {
        const bool written = reference.replaceWithText (lines.joinIntoString ("\n") + "\n", false, false, "\n");
        std::printf ("%s %s (%d parameters)\n", written ? "Wrote" : "Could not write", reference.getFullPathName().toRawUTF8(), lines.size());
        return written ? 0 : 1;
    }

    juce::StringArray expected;
    expected.addLines (reference.loadFileAsString());
    expected.removeEmptyStrings();
    if (expected.isEmpty())
    {
        std::printf ("FAIL  no reference at %s (run with --write to create it)\n", reference.getFullPathName().toRawUTF8());
        return 1;
    }

    int failures = 0;
    for (int i = 0; i < expected.size(); ++i)
    {
        const auto actual = i < lines.size() ? lines[i] : juce::String ("(missing)");
        if (actual != expected[i])
        {
            std::printf ("FAIL  parameter %d changed\n        was: %s\n        now: %s\n",
                         i, expected[i].toRawUTF8(), actual.toRawUTF8());
            ++failures;
        }
    }

    std::printf ("  %s  the %d parameters in the reference are unchanged, in order\n",
                 failures == 0 ? "PASS" : "FAIL", expected.size());

    for (int i = expected.size(); i < lines.size(); ++i)
        std::printf ("  NEW   %s\n", lines[i].toRawUTF8());
    if (lines.size() > expected.size())
        std::printf ("  %d parameter%s appended since the reference; update it with --write\n",
                     lines.size() - expected.size(), lines.size() - expected.size() == 1 ? "" : "s");

    if (failures > 0)
        std::printf ("\nA parameter may only be appended, never renamed, moved, re-ranged or removed: hosts\n"
                     "store sessions and automation by ID and normalised value (roadmap rules 2 and 3).\n");

    std::printf ("\n%s (%d failure%s)\n", failures == 0 ? "ALL PASSED" : "FAILED", failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
