# 3D Grottos

A [Majora's Mask: Recompiled](https://github.com/Zelda64Recomp/Zelda64Recomp) mod that replaces the flat grotto sprite with an actual 3d model!

![Link standing at the edge of a grotto](https://raw.githubusercontent.com/laurhinch/mm-3d-grottos/main/docs/screenshot.png)

Requires Zelda 64: Recompiled 1.2.2 or newer.

## Configuration Options

- 3D Grotto Holes (on): turn the mod off without uninstalling it
- Pit Depth (60): 20 to 150 world units
- Pit Width (100%): 80% to 120% of the vanilla hole
- Pit Style (Earthen): Earthen is dirt fading to black, Shadowed is darker, Void is plain black like vanilla
- Soil Rim (on): a soft ring of dirt that blends the edge into the ground

## Building

Needs `clang` with MIPS support, `lld`, `make`, `zip`, and [RecompModTool](https://github.com/N64Recomp/N64Recomp/releases/tag/mod-tool-release) in the repo root.

```
git clone --recurse-submodules https://github.com/laurhinch/mm-3d-grottos
cd mm-3d-grottos
./package.sh
```

The mod ends up in `build/` and the Thunderstore zip in `dist/`. The versions in `mod.toml` and `thunderstore/manifest.json` have to match or `package.sh` stops.

Built from the [MMRecompModTemplate](https://github.com/Zelda64Recomp/MMRecompModTemplate) using the [Majora's Mask decompilation](https://github.com/zeldaret/mm).
