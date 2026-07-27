#!/usr/bin/env python3
"""Scrape runeshape combinations from poe2db.tw into combinations.json.

Usage:
    pip install requests beautifulsoup4
    python tools/scrape_poe2db.py                       # writes RuneHelper/resources/combinations.json
    python tools/scrape_poe2db.py -o out.json           # custom output path
    python tools/scrape_poe2db.py --dump-html page.html # save raw HTML for selector debugging
    python tools/scrape_poe2db.py --from-html page.html # parse a saved HTML file instead of fetching

Run this after every PoE2 patch that changes combinations. The C++ app loads
the JSON from (in order): %APPDATA%/Denz/RuneHelper/combinations.json,
combinations.json next to the exe, RuneHelper/resources/combinations.json.

poe2db markup changes occasionally. The parser tries several strategies and
prints per-category counts at the end; compare them against the category
counts shown on the page (e.g. "Currency /92"). If counts are off, re-run
with --dump-html and adjust the selectors below.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import re
import sys
from pathlib import Path

URL = "https://poe2db.tw/Runeshape_Combinations"
UA = "expeditionWiz-scraper/1.0 (+https://github.com/imbermuda/expeditionWiz)"

RUNE_SUFFIX = re.compile(r"\s*Rune$", re.IGNORECASE)
COUNT_RE = re.compile(r"^(?:x\s*)?(\d+)\s*x?$", re.IGNORECASE)
LEVEL_RE = re.compile(r"L(?:v|vl|evel)\.?\s*(\d+)", re.IGNORECASE)

CATEGORY_HINTS = {
    "alloy": "alloy",
    "currency": "currency",
    "gem": "gem",
    "rune": "rune",
    "unique": "unique",
}


def norm_rune(name: str) -> str:
    """'Fire_Rune' / 'Fire Rune' -> 'Fire'."""
    name = name.replace("_", " ").strip()
    return RUNE_SUFFIX.sub("", name).strip()


def fetch_html(dump_path: str | None) -> str:
    import requests

    resp = requests.get(URL, headers={"User-Agent": UA}, timeout=30)
    resp.raise_for_status()
    if dump_path:
        Path(dump_path).write_text(resp.text, encoding="utf-8")
        print(f"raw HTML saved to {dump_path}")
    return resp.text


def parse(html: str) -> list[dict]:
    from bs4 import BeautifulSoup

    soup = BeautifulSoup(html, "html.parser")
    combos: list[dict] = []
    seen: set[tuple] = set()

    # Strategy: a combination card is any container that holds >= 2 links to
    # rune pages (href containing 'Rune') AND a non-rune item link/heading
    # (the output). We walk leaf-most containers first to avoid double counts.
    def rune_links(el) -> list[str]:
        out = []
        for a in el.find_all("a", href=True):
            href = a["href"]
            m = re.search(r"/([A-Za-z_]+_Rune)(?:$|[/?#])", href)
            if m:
                out.append(norm_rune(m.group(1)))
        return out

    def output_candidates(el) -> list[str]:
        names = []
        # item links that are not rune links
        for a in el.find_all("a", href=True):
            if re.search(r"_Rune(?:$|[/?#])", a["href"]):
                # crafted runes CAN be outputs (e.g. 'Greater Desert Rune');
                # keep them only if the link text has a Lesser/Greater/qualifier
                text = a.get_text(strip=True)
                if re.match(r"^(Lesser|Greater|Ancient|Grand)\b", text):
                    names.append(text)
                continue
            text = a.get_text(strip=True)
            if text and len(text) > 2:
                names.append(text)
        for h in el.find_all(re.compile("^h[1-6]$")):
            text = h.get_text(strip=True)
            if text:
                names.append(text)
        return names

    candidates = soup.find_all(True)
    for el in candidates:
        runes = rune_links(el)
        if len(runes) < 2 or len(runes) > 10:
            continue
        # leaf-most: skip if a child container would match identically
        child_match = False
        for child in el.find_all(True, recursive=False):
            if len(rune_links(child)) == len(runes):
                child_match = True
                break
        if child_match:
            continue

        outputs = [o for o in output_candidates(el) if norm_rune(o) not in runes]
        if not outputs:
            continue
        output = outputs[0]

        text = el.get_text(" ", strip=True)
        count = 1
        mcount = re.search(re.escape(output) + r"\s*x\s*(\d+)", text)
        if mcount:
            count = int(mcount.group(1))
        level = 0
        mlevel = LEVEL_RE.search(text)
        if mlevel:
            level = int(mlevel.group(1))

        category = "unknown"
        # look upward for a category section heading
        parent = el
        for _ in range(6):
            parent = parent.parent
            if parent is None:
                break
            pid = " ".join(filter(None, [parent.get("id", ""), " ".join(parent.get("class", []))])).lower()
            for hint, cat in CATEGORY_HINTS.items():
                if hint in pid:
                    category = cat
                    break
            if category != "unknown":
                break

        key = (output, tuple(sorted(runes)))
        if key in seen:
            continue
        seen.add(key)
        combos.append(
            {
                "output": output,
                "count": count,
                "level": level,
                "category": category,
                "runes": runes,
            }
        )

    return combos


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", "--output", default="RuneHelper/resources/combinations.json")
    ap.add_argument("--dump-html", default=None)
    ap.add_argument("--from-html", default=None)
    args = ap.parse_args()

    html = Path(args.from_html).read_text(encoding="utf-8") if args.from_html else fetch_html(args.dump_html)
    combos = parse(html)

    if not combos:
        print("ERROR: parsed 0 combinations — poe2db markup likely changed.", file=sys.stderr)
        print("Re-run with --dump-html page.html and inspect/adjust selectors.", file=sys.stderr)
        return 1

    by_cat: dict[str, int] = {}
    for c in combos:
        by_cat[c["category"]] = by_cat.get(c["category"], 0) + 1

    doc = {
        "source": URL,
        "generated": _dt.date.today().isoformat(),
        "league": "Runes of Aldur",
        "complete": True,
        "combinations": sorted(combos, key=lambda c: (c["category"], c["output"])),
    }
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(doc, indent=2), encoding="utf-8")

    print(f"wrote {len(combos)} combinations to {out}")
    print("per category:", json.dumps(by_cat, indent=2))
    print("sanity-check these against the poe2db page header counts (expect ~321 total).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
