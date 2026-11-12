# SLOPPY CAMERA FX

Combat and weather lens effects for Skyrim Special Edition / Anniversary Edition. One SKSE plugin
(`SloppyCameraFX.dll`) with two switchable modules on a shared core. By nicktheduke.

Works on its own, or with Community Shaders, Bottled Shaders or ENB (tested on vanilla, Bottled Shaders and ENBSeries 0.505). Not for VR. Tested on AE 1.6.1170.

Download and full feature list: Nexus Mods (SLOPPY CAMERA FX).

## Triggering effects from another mod

Any mod can ask for a lens effect with a plain SKSE mod event. No dependency, and nothing happens if SLOPPY CAMERA FX
is not installed.

```papyrus
SendModEvent("SloppyCameraFX_Trigger", "blood", 1.0)   ; one-shot, strength 0-1 (omit for full strength)
SendModEvent("SloppyCameraFX_Hold", "frost", 0.8)      ; held at a level 0-1, send 0 to let go
SendModEvent("SloppyCameraFX_ReleaseAll")              ; lets go of every held effect
```

- Trigger names: `blood`, `vampire_blood`, `ichor`, `oil`, `draugr_dust`, `bone_dust`, `sap`, `atronach_embers`,
  `atronach_ice`, `atronach_sparks`, `ghost_mist`, `venom`, `web`, `ice`, `fire`, `shock`, `grit`, `ash_puff`, `steam`,
  `stomp`, `shout`, `flash`, `splash`, `shake`, `shake_power`, `shake_bash`, `shake_arrow`, `shake_blocked`,
  `shake_big`, `shake_stagger`.
- Hold names: `poison`, `dragonfire`, `rime`, `shock_lasting`, `stamina_breath`, `frost`, `rain`, `sun`, `moon`,
  `heat_haze`, `ash`, `dust`, `fog`, `embers`, `cold_breath`, `entry_fog`.
- A hold lasts 30 seconds unless it is sent again, and holds are dropped when a save loads.
- Requests go through the player's own switches, strengths and Photosensitive Safe Mode. The "Other Mods" menu page can
  turn them all off. Unknown names are written to `SloppyCameraFX.log`.
- To test without Papyrus: `Data\SKSE\Plugins\SloppyCameraFX\ApiTest.txt`, one
  `<seconds> <Trigger|Hold|ReleaseAll> [name] [value]` per line, is played after every save load (see `core/Api.cpp`).

## Layout

- `core/` plugin entry, the tone map hook (chained after Community Shaders' own hook, before the HUD), shader compile
  and disk cache, SKSE Menu Framework menu, `Settings.json`, and `LensShared.h` (one lens budget and breathing clock
  shared by both modules).
- `combat/` the Combat module: blood, venom, webs, ice, fire, shock, poison, dragonfire, rime, steam, stomp dust,
  grit, ash, stamina breath, impact flash, slash streak, shout ripple, hit shake.
- `weather/` the Weather module: frost (Survival Mode cold, snowfall, blizzards, snow on the ground), rain drops,
  water after swimming, sun and moon on the lens, heat haze and shimmer, ash, embers, dust, fog, cold breath, entry fog.
- `data/` what ships under `Data\` (shaders in `SKSE\Plugins\SloppyCameraFX\Shaders\`, lens textures and
  `LensWeathers.ini` in `SKSE\Plugins\SloppyCameraFX\Weather\`).
- `include/` SKSE Menu Framework API header.
- `extern/` CommonLibSSE-NG 6.7.1 and hde64, built from source with the plugin.
- `licenses/` license files of the vcpkg libraries compiled in.

## Building

Windows, Visual Studio 2022 (MSVC, C++23), CMake 3.21+.

1. Install the dependencies with vcpkg, triplet `x64-windows-static-md-release`:

   ```
   vcpkg install spdlog fmt directxtk directxmath nlohmann-json rapidcsv xbyak --triplet x64-windows-static-md-release
   ```

2. Configure and build, pointing `SCFX_VCPKG` at that installed prefix (default is `deps/vcpkg` in this folder):

   ```
   cmake -B build -G "Visual Studio 17 2022" -A x64 -DSCFX_VCPKG=C:/vcpkg/installed/x64-windows-static-md-release
   cmake --build build --config Release --target SloppyCameraFX
   ```

3. Copy `build/Release/SloppyCameraFX.dll` to `Data\SKSE\Plugins\`, and everything under `data/` to `Data\`.

## License

GPL-3.0-or-later, see `LICENSE.txt`. Third-party parts and their licenses are in `THIRD-PARTY-NOTICES.txt`.
