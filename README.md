# SMB1 Classic Multiplayer

A fan-made, source-available fork of [SMB Vanilla](https://github.com/nukep/SMB-Vanilla) focused on **ghost-style online co-op** for **Windows** and the **Raspberry Pi Zero 2 W**.

> **Status:** early development. The upstream single-player game works; the online mode and the co-op rules are designed but not implemented yet.

## What is this?

SMB Vanilla is a C decompilation/port of the NES games *Super Mario Bros.* and *Super Mario Bros.: The Lost Levels*. This fork keeps the classic game and adds a lightweight online layer where two players share a room but play their own instance of the level.

The other player appears as a **ghost**: you can see their position and animation, but they do not collide with your enemies, blocks or physics. Interaction happens through events:

- **Shared power-ups:** if one player grabs a mushroom or fire flower, the other gets it too.
- **Eco blocks:** blocks that exist only in one player's world. Breaking one sends a reward or opens a path for the other player.
- **Emotes / revives:** small interactions to keep the run cooperative without syncing the whole world.

This is intentionally **not** a shared-world co-op and **not** split-screen. Each player has their own camera on their own device. It is inspired by the ghost online mode of *Super Mario Bros. Wonder*.

## Current status

Implemented in the Windows build:

- Infinite lives.
- No time limit (timer frozen at 999).
- Keyboard and SDL2 gamepad support.
- Fullscreen toggle with **F11** (aspect-correct 4:3).
- Windows build launches as a GUI app, with no extra console window.

Still planned:

- Ghost-style online co-op.
- Shared power-ups.
- Eco blocks.
- Level-completion waits and the death/respawn rules.

## Controls

Keyboard:

- `W A S D` — move
- `J` — run / fire (B)
- `K` — jump (A)
- `Enter` — Start
- `\` — Select
- `F11` — toggle fullscreen
- `Alt+Q` — toggle fullscreen

Gamepad (SDL2, first controller):

- D-pad or left stick — move
- `A` — jump
- `B` or `X` — run / fire
- `Start` — Start
- `Back` — Select

## Planned co-op rules

- **Online only.** No local co-op.
- **Infinite lives.** Dying respawns you; there is no Game Over.
- **No time limit.** The timer is frozen and the hurry-up/death-by-time logic is disabled.
- **Level completion waits:** the first player to finish waits at the flagpole. When the second finishes, both advance.
- **If the second player dies after the first finished,** the second also completes the level, so nobody gets stuck.
- **Death:** respawn next to your friend on safe ground, with brief invulnerability, or at the last checkpoint if they are in another section.
- **Both dead:** both return to the last checkpoint.

See [docs/DISENO.md](docs/DISENO.md) for the full design and [docs/ROADMAP.md](docs/ROADMAP.md) for the implementation order.

## Requirements

- A **legally obtained** Super Mario Bros. NES ROM (`smb.nes`). **No ROM or original Nintendo assets are included in this repository.**
- Windows: C/C++ compiler, Meson, SDL3 (preferred) or SDL2.
- Raspberry Pi: Raspberry Pi OS, SDL2, Meson, Ninja and a C/C++ compiler.

## Build on Windows

### MSYS2 + MinGW (tested)

Install the toolchain from the MSYS2 MinGW64 shell:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-meson mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-SDL2
```

Then build. The script uses the OpenGL renderer by default:

```sh
./tools/build-windows.sh
```

If your GPU or driver does not support OpenGL 3.3, build with the software renderer:

```sh
./tools/build-windows.sh false
```

### MSVC + SDL3

See [BUILDING.md](BUILDING.md).

Then put `smb.nes` next to the executable (or set `rom_filename` in `smbvanilla.ini`) and run:

```sh
build/src/platform/smbvanilla.exe
```

## Build on Raspberry Pi Zero 2 W

```sh
sudo apt install build-essential meson ninja-build libsdl2-dev
meson setup build -Dsdl2=true -Dopengl=false
meson compile -C build
./build/src/platform/smbvanilla
```

- `-Dopengl=false` selects the software renderer, because the Zero 2 W only supports OpenGL ES 2.0 (the OpenGL renderer requires OpenGL 3.3).
- Target: 60 fps at the original 256x240 resolution, scaled to the display.
- Gamepad support and a fullscreen launcher are on the roadmap.

## Online server

The planned room/relay server is [smb1r-online-server](https://github.com/chumimario02/smb1r-online-server): room codes plus a WebSocket relay. The client will send ghost state at a low rate (around 10 Hz) and discrete events for power-ups, eco blocks and revives. It is not wired into the game yet.

## Credits and license

- Based on [SMB Vanilla](https://github.com/nukep/SMB-Vanilla) by nukep and contributors. Licensed under **GPL-3.0**; see [LICENSE.md](LICENSE.md).
- NES sound emulation: **Nes_Snd_Emu** by James Athey and **emu2413** by digital-sound-antiques.
- Original SMB disassembly reference: doppelganger.
- *Super Mario Bros.* is a trademark of Nintendo. This project is not affiliated with Nintendo. No ROMs or original Nintendo assets are included; you must provide your own legally obtained ROM. This is a non-commercial fan project.
