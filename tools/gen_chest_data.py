#!/usr/bin/env python3
"""Generate the Chest Challenge overlay data file (data/engus/chests.json).

Joins the chest-challenge.aegis.games exports (regions.json, chests.json,
mobs.json) with the existing boss overlay data (bosses.json):

  * region tree copied 1:1 from the site (3 roots -> groups -> leaves),
  * chests placed in their site leaf region (flag 0 until cross-matched),
  * bosses from bosses.json re-attached to the site region that contains them
    (name-based matching, normalized to absorb Duo/parenthetical variants).

Output schema mirrors bosses.json but with a nested tree:

  [
    {"region_name": "Overground", "regions": [], "children": [
       {"region_name": "Greater Limgrave", "regions": [1800,...], "children": [
          {"region_name": "Limgrave", "regions": [...],
           "chests": [{"chest": ..., "place": ..., "flag_id": 0, "site_id": 21}],
           "bosses": [{"boss": ..., "flag_id": ...}]}
       ]}
    ]}
  ]
"""
import json
import re
import sys

HERE = "/workspaces/ERTracker"

def norm(n):
    n = re.sub(r"^\[\d+\]\s*", "", n)
    n = n.replace("\u2019", "'")
    n = n.replace("(x2)", "").replace("(x3)", "")
    n = n.replace(" Duo", "").replace(" & ", " and ")
    n = re.sub(r"\s+", " ", n).strip().lower()
    return n

# --- load ------------------------------------------------------------------
site_regions = json.load(open(f"{HERE}/regions.json"))["data"]
site_chests = json.load(open(f"{HERE}/chests.json"))["data"]
site_mobs = json.load(open(f"{HERE}/mobs.json"))["data"]
overlay = json.load(open(f"{HERE}/bosses.json"))

# --- boss matching ----------------------------------------------------------
# normalized site name -> mob entries
mobs_by_name = {}
for m in site_mobs:
    mobs_by_name.setdefault(norm(m["name"]), []).append(m)

def find_mob(name, expected_leaves):
    """Match an overlay boss to a site mob.

    Priority: (1) same normalized name inside one of the expected site leaves,
    (2) same name anywhere, (3) every '&'-part matching a single site entry's
    parts. Ambiguous names (Tree Sentinel, Night's Cavalry, Deathbird, Watchdog
    appear in several regions) resolve via the expected leaves first.
    """
    key = norm(name)
    expected = set(expected_leaves)
    # 1. name match inside the curated region
    for cand in mobs_by_name.get(key, []):
        if cand["region"] in expected:
            return cand
    # 2. multi-part match inside the curated region
    parts = [norm(p) for p in re.split(r"\s*&\s*", name)]
    if len(parts) > 1:
        for site_name, cands in mobs_by_name.items():
            site_parts = set(re.split(r"\s*&\s*", site_name))
            if parts and all(p in site_parts for p in parts):
                for cand in cands:
                    if cand["region"] in expected:
                        return cand
    # 3. name match anywhere (last resort)
    cands = mobs_by_name.get(key)
    if cands:
        return cands[0]
    return None

# overlay region name -> site leaf name (hand-curated, verified against data)
OVERLAY_TO_SITE = {
    "Limgrave": ["Limgrave"],
    "Weeping Peninsula": ["Weeping Peninsula"],
    "Stormveil Castle": ["Stormveil Castle"],
    "Liurnia of the Lakes": ["Liurnia Eastern Coast", "Liurnia Western Coast",
                             "Liurnia Lakebed", "Caria Manor"],
    "Academy of Raya Lucaria": ["Academy of Raya Lucaria"],
    "Ainsel": ["Nokstella, Eternal City"],
    "Siofra River": ["Siofra River"],
    "Caelid": ["Caelid", "Redmane Castle"],
    "Nokron": ["Nokron, Eternal City"],
    "Altus Plateau": ["Altus Plateau"],
    "Capital Outskirts": ["Capital Outskirts"],
    "Mt. Gelmir": ["Mt. Gelmir"],
    "Volcano Manor": ["Volcano Manor"],
    "Deeproot Depths": ["Deeproot Depths"],
    "Ainsel River": ["Ainsel River", "Lake of Rot"],
    "Leyndell, Royal Capital": ["Leyndell, Royal Capital", "Sewers & Shunning Grounds"],
    "Forbidden Lands": ["Forbidden Lands"],
    "Mountaintops of the Giants": ["Mountaintops of the Giants"],
    "Crumbling Farum Azula": ["Crumbling Farum Azula"],
    "Moonlight Altar": ["Moonlight Altar"],
    "Greyoll's Dragonbarrow": ["Dragonbarrow"],
    "Mohgwyn Dynasty Mausoleum": ["Mohgwyn Palace"],
    "Consecrated Snowfield": ["Consecrated Snowfield"],
    "Miquella's Haligtree": ["Miquella's Haligtree", "Elphael Brace of the Haligtree"],
    "Leyndell, Ashen Capital": ["Leyndell, Capital of Ash"],
    "Gravesite Plain": ["Gravesite Plain", "Castle Ensis", "Belurat, Tower Settlement"],
    "Cerulean Coast": ["Cerulean Coast", "Stone Coffin Fissure"],
    "Scadu Altus": ["Scadu Altus"],
    "Rauh Base": ["Rauh Base"],
    "Shadow Keep": ["Shadow Keep"],
    "Stone Coffin Fissure": ["Stone Coffin Fissure"],
    "Scaduview": ["Scaduview", "Hinterlands"],
    "Cathedral of Manus Metyr": ["Scadu Altus"],
    "Passage to Abyssal Woods": ["Abyssal Woods"],
    "Abyssal Woods": ["Abyssal Woods", "Midra's Manse"],
    "Charo's Hidden Grave from Jagged Peak": ["Charo's Hidden Grave"],
    "Jagged Peak": ["Jagged Peak"],
    "Ancient Ruins of Rauh": ["Ancient Ruins of Rauh"],
    "Enir-Ilim": ["Enir-Ilim"],
}

# site leaf name -> site id
leaf_by_name = {}
def collect_leaves(rs):
    for r in rs:
        ch = r.get("children") or []
        if ch:
            collect_leaves(ch)
        else:
            leaf_by_name[r["name"]] = r
collect_leaves(site_regions)

# --- build bosses per site leaf ---------------------------------------------
bosses_by_leaf = {}   # leaf name -> [boss entry]
unmatched_bosses = []
for region in overlay:
    site_leaves = OVERLAY_TO_SITE.get(region["region_name"])
    if site_leaves is None:
        unmatched_bosses.append((region["region_name"], "no site mapping"))
        continue
    for b in region["bosses"]:
        mob = find_mob(b["boss"], site_leaves)
        leaf = site_leaves[0]
        if mob and mob["region"] in leaf_by_name:
            # trust the site's region for this boss (it may be more granular)
            leaf = mob["region"]
        bosses_by_leaf.setdefault(leaf, []).append({
            "boss": b["boss"],
            "place": b["place"],
            "flag_id": b["flag_id"],
        })

# report boss coverage
total_bosses = sum(len(v) for v in bosses_by_leaf.values())
print(f"bosses attached to site regions: {total_bosses}/{sum(len(r['bosses']) for r in overlay)}")
for name, why in unmatched_bosses:
    print(f"  ! overlay region without mapping: {name} ({why})")

# --- build chests per leaf ---------------------------------------------------
chests_by_leaf = {}
for c in site_chests:
    chests_by_leaf.setdefault(c["region"], []).append({
        "chest": c["name"],
        "place": c.get("dungeon") or "",
        "flag_id": 0,  # filled by the item-lot cross-match (extract_items.py)
        "site_id": c["id"],
    })
print(f"chests placed: {sum(len(v) for v in chests_by_leaf.values())}")

# --- assemble the tree -------------------------------------------------------
def build_node(r):
    ch = r.get("children") or []
    node = {"region_name": r["name"], "regions": []}
    if ch:
        node["children"] = [build_node(c) for c in ch]
    else:
        node["chests"] = chests_by_leaf.get(r["name"], [])
        node["bosses"] = bosses_by_leaf.get(r["name"], [])
    return node

tree = [build_node(r) for r in site_regions]

out = f"{HERE}/overlay_chests.json"
with open(out, "w", encoding="utf-8") as f:
    json.dump(tree, f, ensure_ascii=False, indent=1)

# --- stats -------------------------------------------------------------------
def count_nodes(nodes, depth=0):
    n_leaves = n_chests = n_bosses = 0
    for nd in nodes:
        if "children" in nd:
            a, b, c = count_nodes(nd["children"], depth + 1)
            n_leaves += a; n_chests += b; n_bosses += c
        else:
            n_leaves += 1
            n_chests += len(nd["chests"])
            n_bosses += len(nd["bosses"])
    return n_leaves, n_chests, n_bosses

leaves, chests_n, bosses_n = count_nodes(tree)
print(f"\n-> {out}")
print(f"   leaves={leaves} chests={chests_n} bosses={bosses_n}")

# sanity: every overlay boss is somewhere in the tree
if bosses_n != sum(len(r["bosses"]) for r in overlay):
    print("   !! some bosses were dropped during re-attachment")
    sys.exit(1)
