# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
# Licensed under the GNU Affero General Public License v3; see LICENSE.
"""
Code statistics for the plugin's own source, as a Markdown report.

Counts lines per part of the repository and measures the cyclomatic complexity of
every function in the plugin (the headers in the repository root and
NeonSynthProcessor.cpp) with lizard. Cyclomatic complexity is the number of
independent paths through a function: under 10 is easy to test, over 20 is hard.

    pip install lizard
    python tests/complexity_report.py            # prints the report
    python tests/complexity_report.py --max 30   # also exits non-zero if a function is above 30

CI runs it on every push and pull request and shows the report on the run's summary
page. It is a report, not a gate: without --max it always exits 0. The number says
nothing about thread hand-offs, timing or numerics, which is where audio code is hard.
"""
import argparse
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

import lizard

ROOT = Path(__file__).resolve().parent.parent
BANDS = [(1, 5, "simple"), (6, 10, "moderate"), (11, 20, "complex"), (21, 10**6, "hard to test")]


def tracked(*patterns):
    out = subprocess.run(["git", "ls-files", *patterns], cwd=ROOT, check=True, capture_output=True, text=True)
    return [ROOT / line for line in out.stdout.splitlines() if line]


def lines(files):
    return sum(len(f.read_text(encoding="utf-8", errors="replace").splitlines()) for f in files)


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--max", type=int, metavar="N", help="exit non-zero if any function's complexity is above N")
    args = parser.parse_args()

    plugin = tracked("*.h", "NeonSynthProcessor.cpp")
    plugin = [f for f in plugin if f.parent == ROOT]
    parts = [
        ("The plugin (headers and NeonSynthProcessor.cpp)", plugin),
        ("C++ tests and benchmark", tracked("tests/*.cpp")),
        ("Python (tests, manual, figures, demo)", tracked("*.py")),
        ("Build, CI and installer", tracked(".github/*", "CMakeLists.txt", "installer.nsi")),
    ]

    functions = []   # (complexity, lines of code, file, name)
    for f in plugin:
        for fn in lizard.analyze_file(str(f)).function_list:
            functions.append((fn.cyclomatic_complexity, fn.nloc, f.name, fn.name))
    if not functions:
        print("no functions found", file=sys.stderr)
        return 1
    functions.sort(reverse=True)
    values = sorted(c for c, *_ in functions)

    print("## Code statistics\n")
    print("| Part | Lines |\n|---|---:|")
    total = 0
    for name, files in parts:
        n = lines(files)
        total += n
        print(f"| {name} | {n:,} |")
    print(f"| **Total** | **{total:,}** |\n")

    print(f"### Complexity of the plugin: {len(functions)} functions, "
          f"mean {sum(values) / len(values):.1f}, median {values[len(values) // 2]}, highest {values[-1]}\n")
    print("| Cyclomatic complexity | Functions | Share |\n|---|---:|---:|")
    for low, high, label in BANDS:
        n = sum(1 for v in values if low <= v <= high)
        span = f"{low}-{high}" if high < 10**6 else f"over {low - 1}"
        print(f"| {span} ({label}) | {n} | {100 * n / len(values):.0f}% |")

    print("\n### The ten most complex functions\n")
    print("| Function | File | Complexity | Lines |\n|---|---|---:|---:|")
    for c, nloc, file, name in functions[:10]:
        print(f"| `{name}` | `{file}` | {c} | {nloc} |")

    per_file = defaultdict(list)
    for c, nloc, file, name in functions:
        per_file[file].append(c)
    print("\n### By file\n")
    print("| File | Functions | Total complexity | Highest |\n|---|---:|---:|---:|")
    for file, cs in sorted(per_file.items(), key=lambda kv: -sum(kv[1])):
        print(f"| `{file}` | {len(cs)} | {sum(cs)} | {max(cs)} |")

    if args.max is not None and values[-1] > args.max:
        over = [f"{name} ({c})" for c, _, _, name in functions if c > args.max]
        print(f"\ncomplexity above {args.max}: {', '.join(over)}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
