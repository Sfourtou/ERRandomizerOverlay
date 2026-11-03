# ELDEN RING Overlay Hook

## NOTE
* The current core uses UI_SCENE rendering. Static address checks pass for game versions 1.05–1.17.1; 1.02–1.04.1 currently do not render overlays.
* You can use `EROverlay.dll` to load multiple overlays, I will keep the back compatibility so that you just need to make sure `Overlay Loader` version meets the minimal requirements for certain overlay.

## USAGE

* Edit the INI files in `configs/`, save as UTF-8 and restart the game after changes.
* For Chinese comments, back up your customized file, copy the matching template from `configs_CN/` into `configs/` and reapply your settings. Keys and default values are identical; the loader reads `configs/`.
* Inject the mod to Elden Ring, you can either:
  + Rename `EROverlay.dll` to `winhttp.dll` and put it aside `eldenring.exe` (don't forget folders `configs` and `data`).
  + Load `EROverlay.dll` with any mod loader ([EldenModLoader](https://www.nexusmods.com/eldenring/mods/117), [ModEngine2](https://github.com/soulsmods/ModEngine2) or [me3](https://github.com/garyttierney/me3)).
  + Run `injector.exe` to inject (not recommended, because that this method is not very stable and is blocked by some security softwares).
* Wait few seconds until the mod is fully loaded, press `=` to toggle mini/full mode.
* To revive a defeated boss, set `allow_revive=true` in `configs/boss.ini`, restart the game, and click its checked box in the full list. Revival restores the configured primary and arena flags; travel or reload the area afterwards. See `docs/boss-revival.md` in the Boss package for shared arena and quest entry behavior.

## RANDOMIZER INTEGRATION (Elden Ring Item and Enemy Randomizer)

The boss tracker can display which boss the [Item and Enemy Randomizer](https://www.nexusmods.com/eldenring/mods/428)
placed at each vanilla boss location, instead of only showing the vanilla boss of that location.

* Set `randomizer_spoiler_log` in `configs/boss.ini` to the randomizer's `spoiler_logs` directory
  (or directly to a `*_log_*.txt` spoiler log). When left empty, a `spoiler_logs` folder beside
  `EROverlay.dll` is used automatically when present.
* The mapping is read once at game start: randomize first, then launch the game with the overlay.
* In the full boss list, a checkbox shows the placed enemy's name only after that boss is
  defeated (`randomizer_reveal=on_kill`, the default, avoids spoilers). With
  `randomizer_reveal=always` the placed name is shown from the start. The tooltip shows the
  location and the vanilla boss, plus the placed enemy (with its vanilla home) once revealed.
* When a boss dies, a toast shows the placed enemy's name; set `randomizer_notifications=false`
  to disable toasts.
* A collapsible **"Remaining pool"** section (`randomizer_pool=true`, default) lists the bosses
  still alive, grouped by placed enemy identity — without revealing their locations. When the
  randomizer placed several copies of a boss, the group shows the copy counts, e.g.
  `Malenia, Blade of Miquella (x2 remaining of 3 in run)`.
* `randomizer_pool_filter=true` adds a **"Hide defeated"** checkbox above the list; defeated
  bosses and fully-defeated regions disappear until the checkbox is cleared.
* Without a spoiler log (or for an enemies-not-randomized run, e.g. items-only), the tracker
  behaves exactly like the vanilla overlay.

## [LICENSE](https://github.com/soarqin/EROverlay/blob/master/LICENSE)

## CREDITS
* modified from [ELDEN RING Internal Menu](https://github.com/NightFyre/ELDENRING-INTERNAL)
* [minhook](https://github.com/TsudaKageyu/minhook)
* [imgui](https://github.com/ocornut/imgui)
* [stb](https://github.com/nothings/stb)
* [JSON for Modern C++](https://github.com/nlohmann/json)
* [Pattern16](https://github.com/Dasaav-dsv/Pattern16)
* [Open Sans](https://fonts.google.com/specimen/Open+Sans)
