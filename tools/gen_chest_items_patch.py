#!/usr/bin/env python3
"""Regenerate data/engus/chest_items.json (item name -> ItemLotParam row ids).

Sources, merged in order (later wins on conflicts by union):
 1. the current file (kept as-is on first run; it is the live-extraction baseline)
 2. the public "Elden Ring Item Lot and Shop Analysis" dump
    (https://soulsmods.github.io/data/er/items.html, ItemLotParam_map and
    ItemLotParam_enemy sections; fetched and cached under tools/er_items.html)

The baseline missed whole item families (shop-only rewards, great runes, many
Ashes of War, DLC drops), which left chests that received such an item without
any candidate grant flag — they could never be marked opened in a randomizer
run. The dump adds ~1900 names; both sources are vanilla data, so the union is
safe.

Entries with no vanilla lot row (shop/event rewards only, e.g. "Agheel's
Flame") keep an empty list: the name is recognized but contributes no flags.

Usage:
    python3 tools/gen_chest_items_patch.py            # fetch + merge + write
    python3 tools/gen_chest_items_patch.py --no-fetch # use cached tools/er_items.html
"""
import json
import html
import os
import re
import sys
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
TARGET = os.path.join(HERE, "..", "EROverlay", "src", "boss", "data", "engus", "chest_items.json")
CACHE = os.path.join(HERE, "er_items.html")
URL = "https://soulsmods.github.io/data/er/items.html"

# Curated additions/overrides verified against the same dump (kept for provenance
# even after the bulk merge; the bulk merge reproduces these values).
EXTRA = {
    "Agheel's Flame": [],
    "Ash of War: Kick": [],
    "Battle Axe": [],
    "Maliketh's Black Blade": [],
    "Prisoner Clothing": [],
    "Spellproof Pickled Liver": [],
}


def fetch(force=True):
    if not force and os.path.exists(CACHE):
        return open(CACHE, encoding="utf-8").read()
    with urllib.request.urlopen(URL, timeout=60) as r:
        data = r.read().decode("utf-8")
    with open(CACHE, "w", encoding="utf-8") as f:
        f.write(data)
    return data


def parse_dump(content):
    """name -> sorted lot ids from the ItemLotParam_map/_enemy sections."""
    sec = re.split(r"<h2>(.*?)</h2>", content)
    sections = {sec[i]: sec[i + 1] for i in range(1, len(sec), 2)}
    name2lots = {}
    for secname in ("ItemLotParam_map", "ItemLotParam_enemy"):
        for line in sections.get(secname, "").splitlines():
            m = re.match(r"^(\d+) ", line)
            if not m:
                continue
            lotid = int(m.group(1))
            rest = line[m.end():]
            # cut the leading [context] group (may contain nested brackets)
            if rest.startswith("["):
                depth = 0
                for i, ch in enumerate(rest):
                    if ch == "[":
                        depth += 1
                    elif ch == "]":
                        depth -= 1
                        if depth == 0:
                            rest = rest[i + 1:].lstrip()
                            break
            for part in rest.split(", "):
                part = part.strip()
                p2 = re.sub(r"\s*- flag \d+\s*$", "", part)
                p2 = re.sub(r"\s+\d+x\s*$", "", p2)
                p2 = re.sub(r"\s*\((?:Goods|Weapon|Protector|Head|Body|Arms|Legs|Talisman|FCS|Generator|Accessory|Booster) \d+\)\s*$", "", p2)
                name = re.sub(r"\s+", " ", html.unescape(p2).strip())
                if not name or name == "[unknown]" or "[ERROR]" in name or not re.search(r"[A-Za-z]{2}", name):
                    continue
                name2lots.setdefault(name, set()).add(lotid)
    return {k: sorted(v) for k, v in name2lots.items()}


def main():
    no_fetch = "--no-fetch" in sys.argv
    content = fetch(force=not no_fetch)
    web = parse_dump(content)
    with open(TARGET, encoding="utf-8") as f:
        data = json.load(f)
    before = len(data)
    added = updated = 0
    for name, lots in web.items():
        if not lots:
            continue
        if name in data:
            merged = sorted(set(data[name]) | set(lots))
            if merged != data[name]:
                data[name] = merged
                updated += 1
        else:
            data[name] = lots
            added += 1
    for name, lots in EXTRA.items():
        if name not in data:
            data[name] = lots
            added += 1
    with open(TARGET, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False, indent=1)
        f.write("\n")
    print(f"chest_items.json: {before} -> {len(data)} entries ({added} added, {updated} merged from the web dump)")


if __name__ == "__main__":
    main()
