#!/usr/bin/env python3
"""Offline simulation of the corrected chest-flag attribution pipeline.

Replays tools logic against a spoiler log + the shipped dataset:
 1. parse item placements (fixed "8x" strip, all "Replaces" lines)
 2. name each dataset chest by reversing chest_items.json on its vanilla lot
 3. match each chest to the log line(s) that replaced its vanilla item
    (exact name; region-word tie-break; union on residual ties)
 4. resolve placed item -> lot rows -> candidate grant flags (items.json lot->flag)
Reports per-region counts + the chests that remain unattributable.
"""
import json
import re
import sys

LOG = sys.argv[1] if len(sys.argv) > 1 else "/workspaces/ERTracker/2026-10-16_11.04.04_log_468546657_35494.txt"
ROOT = "/workspaces/ERTracker"


def strip_count(s):
    s = s.strip().rstrip(".")
    if s.endswith("x"):
        t = s[:-1]
        while t and t[-1].isdigit():
            t = t[:-1]
        if t.endswith(" "):
            s = t
    scan = len(s)
    while scan > 0 and s[scan - 1].isdigit():
        scan -= 1
    if scan > 0 and scan < len(s) and s[scan - 1] == " ":
        s = s[:scan - 1]
    elif scan > 1 and scan < len(s) and s[scan - 1] == "x" and s[scan - 2] == " ":
        s = s[:scan - 2]
    if s.endswith(" x"):
        s = s[:-2]
    return s.strip()


STOP = {"a", "an", "the", "in", "of", "at", "on", "to", "and", "by", "near", "from", "with", "for", "one", "two", "top"}


def words(t):
    out, cur = [], ""
    for ch in t:
        if ch.isalnum():
            cur += ch.lower()
        elif cur:
            out.append(cur)
            cur = ""
    if cur:
        out.append(cur)
    return out


def match_region(hay, region, phrase):
    h = words(hay)
    n = words(region + " " + phrase)
    s = 0
    for x in n:
        if x in STOP or len(x) < 3:
            continue
        for y in h:
            if y == x or (len(x) >= 5 and len(y) >= 5 and y.startswith(x)):
                s += 1
                break
    return s


def main():
    items = json.load(open(f"{ROOT}/EROverlay/src/boss/data/engus/chest_items.json"))
    ds = json.load(open(f"{ROOT}/EROverlay/src/boss/data/engus/chests.json"))
    # stand-in for the in-game ItemLotParam walk: lot id -> getItemFlagId from the
    # public item-lot dump (tools/fetch_er_items.py)
    name2lotflags = {}
    try:
        raw = json.load(open("/tmp/er_name2lotflags.json"))
        name2lotflags = {k: {tuple(x) for x in v} for k, v in raw.items()}
    except FileNotFoundError:
        markers = json.load(open(f"{ROOT}/items.json"))["markers"]
        lot2flag = {m["lot"]: m["flag"] for m in markers if m.get("lot") and m.get("flag")}
        for name, lots in items.items():
            for lot in lots:
                if lot in lot2flag:
                    name2lotflags.setdefault(name, set()).add((lot, lot2flag[lot]))
    itemlot2flag = {}
    for name, pairs in name2lotflags.items():
        for lot, flag in pairs:
            if flag:
                itemlot2flag.setdefault(name, set()).add(flag)

    lines = open(LOG, encoding="utf-8").read().splitlines()
    placements = []
    for ln in lines:
        rp = ln.find(". Replaces ")
        if rp < 0:
            continue
        ip = ln.find(" in ")
        if ip < 0 or ip >= rp:
            continue
        item = strip_count(ln[:ip])
        cp = ln.find(": ", ip)
        if cp < 0 or cp >= rp:
            continue
        region = ln[ip + 4:cp]
        phrase = ln[cp + 2:rp]
        replaced = strip_count(ln[rp + len(". Replaces "):])
        if item:
            placements.append((item, region, phrase, replaced))

    def walk(ns, path=()):
        for n in ns:
            if "children" in n:
                yield from walk(n["children"], path + (n["region_name"],))
            else:
                yield path + (n["region_name"],), n

    inv = {lot: name for name, ls in items.items() for lot in ls}
    stats = {}
    unresolved_all = []
    for path, node in walk(ds):
        grp = path[1] if len(path) > 1 else path[0]
        st = stats.setdefault(grp, {"total": 0, "attributed": 0, "ambiguous": 0, "vanilla": 0, "unresolved": 0})
        for c in node.get("chests", []):
            st["total"] += 1
            van = inv.get(c["lot_id"])
            if van is None:
                st["unresolved"] += 1
                unresolved_all.append((grp, c["chest"], "vanilla item name unknown (lot %d)" % c["lot_id"]))
                continue
            v = strip_count(van)
            hits = [p for p in placements if p[3] == v]
            if not hits:
                st["vanilla"] += 1
                continue
            scored = sorted(((match_region(c["chest"] + " " + (c.get("place") or ""), p[1], p[2]), p) for p in hits), key=lambda t: -t[0])
            best = scored[0][0]
            bests = [p for s, p in scored if s == best] if best > 0 else [p for _, p in scored]
            # candidate flags = union over tied placements of the placed item's lot flags
            cands = set()
            known = False
            for p in bests:
                fl = itemlot2flag.get(p[0])
                if fl is not None:
                    known = True
                    cands |= fl
            own = {c["flag_id"]}
            try:
                own.add(name2lotflags.get(inv.get(c["lot_id"]), []) and 0)
            except Exception:
                pass
            cands -= own
            if not known or not cands:
                st["unresolved"] += 1
                unresolved_all.append((grp, c["chest"], f"item '{bests[0][0]}' has no usable lot flags (replaces {v})"))
                continue
            st["attributed"] += 1
            if len(cands) > 1:
                st["ambiguous"] += 1
    total = {"total": 0, "attributed": 0, "ambiguous": 0, "vanilla": 0, "unresolved": 0}
    for grp, st in stats.items():
        for k in total:
            total[k] += st[k]
    print("== per region ==")
    for grp, st in sorted(stats.items()):
        print(f"  {grp:28} total={st['total']:3} attributed={st['attributed']:3} "
              f"ambiguous={st['ambiguous']:2} vanilla-kept={st['vanilla']:2} unresolved={st['unresolved']:2}")
    print(f"== TOTAL: {total['total']} chests: {total['attributed']} attributed, "
          f"{total['ambiguous']} ambiguous, {total['vanilla']} vanilla-kept, {total['unresolved']} unresolved")
    if unresolved_all:
        print("== unresolved chests ==")
        for grp, n, why in unresolved_all:
            print(f"  [{grp}] {n}: {why}")


if __name__ == "__main__":
    main()
