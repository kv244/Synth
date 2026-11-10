# NeonSynth - Copyright (C) 2026 Razvan Julian Petrescu
# SPDX-License-Identifier: AGPL-3.0-only
"""Checks the user manual against the source and prints it to PDF.

    python docs/build_manual.py --check                  # checks only
    python docs/build_manual.py --out build/manual       # checks, then writes the PDF

The check fails (exit 1) when the manual does not match the code it ships with:
  * "Applies to" and the source tag must name the version in CMakeLists.txt;
  * every relative link and image must point at a file that exists;
  * the cover (docs/images/cover.json) must have been redrawn for that version.
The manual in the repository is never changed. The PDF is built from a copy that
gets a line stating the version, the commit and the build date (the edition
number and date at the top of the manual are still written by hand).

Needs: pip install markdown playwright && playwright install chromium
"""
import argparse
import datetime
import html
import json
import os
import pathlib
import re
import subprocess
import sys
import unicodedata

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
MANUAL = HERE / "NeonSynth-User-Manual.md"


def project_version():
    text = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
    return re.search(r"project\(NeonSynth VERSION\s+(\d+\.\d+\.\d+)", text).group(1)


def check(md, version):
    problems = []
    m = re.search(r"\*\*Applies to:\*\*\s*NeonSynth v(\d+\.\d+\.\d+), source tag `v(\d+\.\d+\.\d+)`", md)
    if not m:
        problems.append("the 'Applies to' line is missing or not in the usual form")
    else:
        for found in m.groups():
            if found != version:
                problems.append(f"the manual says v{found} but CMakeLists.txt says v{version}: "
                                f"update 'Applies to' (and the edition) for this release")
                break
    for link in re.findall(r"\]\(([^)\s]+)\)", md):
        if re.match(r"[a-z]+:", link) or link.startswith("#"):
            continue
        target = link.split("#")[0]
        if target and not (MANUAL.parent / target).exists():
            problems.append(f"broken relative link: {link}")
    cover = HERE / "images" / "cover.json"
    if cover.exists():
        drawn = json.loads(cover.read_text(encoding="utf-8")).get("version")
        if drawn != version:
            problems.append(f"the cover says v{drawn} but CMakeLists.txt says v{version}: run "
                            f"docs/figures/cover.py --date \"<release date>\"")
    return problems


def github_slug(value, separator="-"):
    """Heading ids the way GitHub makes them, so the manual's #anchors work in the PDF too."""
    value = unicodedata.normalize("NFKD", value).lower().strip()
    value = re.sub(r"[^\w\- ]", "", value)
    return value.replace(" ", separator)


CSS = """
@page { size: A4; margin: 25mm 25mm 22mm 25mm; }
html { font-family: 'Liberation Sans', 'DejaVu Sans', Arial, sans-serif; font-size: 10pt; line-height: 1.42; color: #111; }
body { margin: 0; }
h1 { font-size: 24pt; margin: 0 0 6mm; }
h2 { font-size: 16pt; margin: 9mm 0 3mm; break-after: avoid; border-bottom: 0.4pt solid #888; padding-bottom: 1mm; }
h3 { font-size: 12.5pt; margin: 6mm 0 2mm; break-after: avoid; }
h4 { font-size: 10.5pt; margin: 4mm 0 1mm; break-after: avoid; }
p, li { orphans: 3; widows: 3; }
a { color: #0a4f66; text-decoration: none; }
img { max-width: 100%; height: auto; display: block; margin: 3mm auto 1.5mm; }
p:has(> img) { break-after: avoid; break-inside: avoid; margin: 0; }
p:has(> img) + p { font-size: 9pt; break-before: avoid; }
table { border-collapse: collapse; width: 100%; margin: 3mm 0; font-size: 9pt; break-inside: avoid; }
th, td { border: 0.4pt solid #777; padding: 1.2mm 2mm; vertical-align: top; text-align: left; }
th { background: #e6e6e6; }
code { font-family: 'Liberation Mono', 'DejaVu Sans Mono', monospace; font-size: 9pt; }
pre { font-size: 9pt; background: #f2f2f2; padding: 2mm; break-inside: avoid; white-space: pre-wrap; }
hr { border: 0; border-top: 0.4pt solid #888; margin: 5mm 0; }
.buildinfo { font-size: 8.5pt; color: #444; margin-top: 8mm; }
"""


def to_html(md, version, sha):
    import markdown
    built = datetime.date.today().isoformat()
    md = md.rstrip() + (f"\n\n<p class=\"buildinfo\">PDF built {built} from NeonSynth v{version}"
                        f"{f', commit {sha}' if sha else ''}.</p>\n")
    body = markdown.markdown(
        md, extensions=["tables", "fenced_code", "toc", "sane_lists"],
        extension_configs={"toc": {"slugify": github_slug}})
    return (f"<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\">"
            f"<title>NeonSynth User Manual</title><style>{CSS}</style></head><body>{body}</body></html>")


def print_pdf(html_text, pdf_path):
    from playwright.sync_api import sync_playwright
    page_html = MANUAL.parent / ".manual-print.html"      # next to the manual so images resolve
    page_html.write_text(html_text, encoding="utf-8")
    try:
        with sync_playwright() as p:
            browser = p.chromium.launch()
            page = browser.new_page()
            page.goto(page_html.as_uri())
            page.wait_for_load_state("networkidle")
            page.evaluate("Promise.all(Array.from(document.images).map(i => i.decode().catch(() => null)))")
            page.pdf(path=str(pdf_path), prefer_css_page_size=True, print_background=True,
                     display_header_footer=True,
                     header_template="<span></span>",
                     footer_template="<div style=\"font-size:8pt;width:100%;text-align:center;"
                                     "font-family:sans-serif;color:#444\">NeonSynth User Manual &#8211; "
                                     "page <span class=\"pageNumber\"></span> of "
                                     "<span class=\"totalPages\"></span></div>")
            browser.close()
    finally:
        page_html.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--check", action="store_true", help="check only; write nothing")
    ap.add_argument("--out", type=pathlib.Path, default=ROOT / "build" / "manual")
    a = ap.parse_args()

    version = project_version()
    md = MANUAL.read_text(encoding="utf-8")
    problems = check(md, version)
    for p in problems:
        print(f"::error file=docs/NeonSynth-User-Manual.md::{p}" if os.environ.get("GITHUB_ACTIONS") else f"error: {p}")
    if problems:
        return 1
    print(f"manual matches v{version}")
    if a.check:
        return 0

    try:
        sha = subprocess.run(["git", "rev-parse", "--short", "HEAD"], cwd=ROOT, capture_output=True,
                             text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        sha = ""
    a.out.mkdir(parents=True, exist_ok=True)
    pdf = a.out / f"NeonSynth-User-Manual-{version}.pdf"
    print_pdf(to_html(md, version, sha), pdf)
    print(f"wrote {pdf} ({pdf.stat().st_size // 1024} KiB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
