# ERTracker — Randomizer-aware Boss Overlay for Elden Ring

An in-game boss tracker for the [Elden Ring Item and Enemy Randomizer](https://www.nexusmods.com/eldenring/mods/428).

Vanilla trackers check bosses by **location**: the game records a defeat with the event flag of
the boss that *belongs* there. With the randomizer, killing Messmer where the Tree Sentinel
normally stands would check "Tree Sentinel". **ERTracker fixes that** — it reads the randomizer's
spoiler log to know which enemy was placed at each location, and shows *that* boss instead:

* The boss list shows the placed enemy's name **once it is defeated** (spoiler-free by default):
  the Church of Elleh slot reads "Tree Sentinel" while alive, then reveals
  "Messmer the Impaler" when you kill it.
* A **"Boss defeated" toast** appears in-game the moment a boss dies, naming the placed enemy.
* A collapsible **"Remaining pool"** section lists every boss still alive, grouped by placed
  enemy identity — locations stay hidden, so it never spoils where the next boss hides:
  `Malenia, Blade of Miquella (x2 remaining of 3 in run)`.
* An optional **"Hide defeated"** filter shortens the list to what is left to kill.
* Covers the base game (165 bosses) and Shadow of the Erdtree (42 bosses).
* Without a spoiler log (or for an items-only run), the overlay behaves exactly like the
  vanilla [EROverlay](https://github.com/soarqin/EROverlay) boss list.

## How it works

```
Randomizer run ──writes──> spoiler_logs/<date>_log_<seed>_<hash>.txt
                           "Replacing Tree Sentinel (#1042360800) in Church of Elleh:
                            Messmer the Impaler (#21010800) from Shadow Keep"
                                        │ parsed at game start
                                        ▼
             entity ID ──via diste/Base/enemy.txt──> defeat event flag
                                        │
Game memory (event flags, ~2 Hz) ───────┴──> overlay UI + toasts + pool
```

1. The randomizer writes its placements to a spoiler log at every randomization.
2. At game start, the Boss overlay parses that log (`Replacing <placed name> (#<targetId>) in
   <place>: ...` lines from the `Boss placements` / `Miniboss placements` sections) and the
   randomizer's enemy data, which maps every enemy's entity ID to its defeat event flag.
3. The mapping keys placements by **defeat flag** — the same flag the overlay already polls in
   game memory in real time. Some bosses have entity IDs that differ from their defeat flags
   (festival Radahn, Night's Cavalry, Fire Giant, Count Ymir, duo companions...); the enemy
   data resolves all of them, plus duo/trio fights which share one flag.

This project is a fork of [soarqin/EROverlay](https://github.com/soarqin/EROverlay) (MIT):
the Boss overlay plugin was extended with the spoiler-log parser (`src/boss/randolog.cpp`) and
the randomizer UI. It only *reads* files the randomizer's license explicitly permits sharing.

## Installation

1. Build on Windows (see [Building](#building)) or grab a release.
2. Install the overlay like vanilla EROverlay: rename `EROverlay.dll` to `winhttp.dll` beside
   `eldenring.exe` (with the `configs/` and `data/` folders and `overlays/Boss.dll`), or load it
   with ModEngine2 / EldenModLoader / me3.
3. Point it at your randomizer install in `configs/boss.ini`:

   ```ini
   [boss]
   randomizer_spoiler_log=C:\Games\EldenRingRandomizer\spoiler_logs
   randomizer_enemy_data=C:\Games\EldenRingRandomizer\diste\Base\enemy.txt
   ```

   Both paths are optional: a `spoiler_logs` folder and a `diste` folder beside `EROverlay.dll`
   are detected automatically (e.g. when the overlay is installed inside the randomizer folder).
   `randomizer_spoiler_log` accepts a directory (newest log used) or a single log file.
4. Randomize a run **with enemy randomization enabled**, then launch the game. The overlay log
   prints `Loaded N randomizer placements from ...` on success.

> The randomizer writes the spoiler log at randomization time, and the game must be restarted
> after randomizing anyway — so the mapping is always in sync with the loaded run.

### Multiple spoiler logs

The randomizer keeps every past run's log in `spoiler_logs/`. When configured with a directory,
ERTracker picks the log with the **newest modification time** among `*_log_*.txt` files. Since the
randomizer requires a game restart after randomizing, the newest log always matches the loaded
run. Files without boss placements (e.g. items-only runs, or a randomization that errored out)
yield no placements and the tracker falls back to vanilla behavior rather than showing wrong data.

To track a specific older run instead, point `randomizer_spoiler_log` directly at that log file.
The full list header shows the loaded log's filename (`Randomizer: <name>`, full path on hover),
so you can always confirm which run is being tracked.

## Configuration (`configs/boss.ini`)

| Key | Default | Description |
|---|---|---|
| `randomizer_spoiler_log` | *(empty)* | Spoiler log file, or directory whose newest `*_log_*.txt` is used. Empty = `spoiler_logs` folder beside the loader. |
| `randomizer_enemy_data` | *(empty)* | Randomizer's `diste/Base/enemy.txt` (entity → defeat flag table). Empty = `diste` folder beside the loader. Without it most bosses still match; the ~12 special ones do not. |
| `randomizer_notifications` | `true` | Show a "Boss defeated: ..." toast when a boss dies. |
| `randomizer_reveal` | `on_kill` | `on_kill`: reveal the placed name after the kill (spoiler-free). `always`: show it from the start. |
| `randomizer_pool` | `true` | Collapsible "Remaining pool" section: alive bosses grouped by identity, locations hidden. |
| `randomizer_pool_filter` | `false` | Add a "Hide defeated" checkbox above the boss list. |

All other keys (`toggle_full_mode`, `panel_pos`, challenge mode, revive...) behave exactly as in
vanilla EROverlay — see the comments in `configs/boss.ini` and the
[vanilla README](EROverlay/src/boss/README.md).

## Boss data

`data/<lang>/bosses.json` (shipped for 15 languages) lists every tracked boss with its location
and defeat flag — 207 bosses, already matching the randomizer's entity IDs (including the
Fire Giant defeat-flag fix). You don't need to change anything.

The `bosses.json` at the repository root is an optional variant with display prefixes (`[N]`
scalability hints from the author's setup) and the same defeat flags; copy it over
`data/engUS/bosses.json` if you want that style. `tools/user_bosses.example.json` documents
the format, and `tools/bossdata_validate.cpp` validates edits.

## Building

Requires Visual Studio 2022 (MSVC, C++23) and CMake ≥ 3.13 — full details in
[EROverlay/AGENTS.md](EROverlay/AGENTS.md).

```bat
cd EROverlay
cmake -B build -G "Visual Studio 17 2022" -A x64 -T v142 -DRELEASE_USE_STATIC_CRT=OFF -DRELEASE_USE_LTO=ON .
cmake --build build --config Release
```

Artifacts: `build/bin/EROverlay.dll` (loader) and `build/bin/overlays/Boss.dll` (plugin).
`dist.bat` produces distributable zips.

## Tests

The spoiler-log parser is platform-independent and tested with a standalone harness
(no Windows required):

```sh
g++ -std=c++23 -Wall -Wextra -o randolog_test tools/randolog_test.cpp && ./randolog_test
```

`tools/bossdata_validate.cpp` checks a `bosses.json` against the loader's expectations:

```sh
g++ -std=c++23 -I EROverlay/deps/nlohmann_json/include -o bossdata_validate tools/bossdata_validate.cpp
./bossdata_validate bosses.json
```

## Updating

Between releases, only these files change: `EROverlay.dll`, `overlays\Boss.dll` and the language
datasets under `data\`. **Do not overwrite your `configs\` folder** — it holds your personal
settings (`boss_kill_text` with deaths/IGT, challenge mode, randomizer paths, panel position...).
Extract new releases elsewhere and copy over everything except `configs\`.

## Credits

* [soarqin/EROverlay](https://github.com/soarqin/EROverlay) by Soar Qin (MIT) — overlay loader,
  Boss plugin, rendering, memory reading; this fork extends the Boss plugin.
* [Elden Ring Item and Enemy Randomizer](https://www.nexusmods.com/eldenring/mods/428) by
  thefifthmatt — the mod this tracker integrates with (spoiler log + enemy data formats).
* [ELDEN RING Internal Menu](https://github.com/NightFyre/ELDENRING-INTERNAL),
  minhook, imgui, stb, nlohmann/json, Pattern16, fmt, Open Sans — upstream EROverlay dependencies.

## License

EROverlay is MIT-licensed; this fork follows the same license — see
[EROverlay/LICENSE](EROverlay/LICENSE). The randomizer itself is a separate mod by thefifthmatt
(source-available license); this project only reads the spoiler log and enemy data files that
its license explicitly permits sharing. Not affiliated with or endorsed by FromSoftware or
Bandai Namco.
