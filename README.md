# SMB1 Classic Multiplayer

A fan-made, source-available fork of [SMB Vanilla](https://github.com/nukep/SMB-Vanilla) focused on **ghost-style online co-op** for **Windows** and the **Raspberry Pi Zero 2 W**.

> **Status:** playable prototype. Single-player is complete, and online co-op works
> on Windows (two players, ghost-style). The Raspberry Pi build is still pending.

## What is this?

SMB Vanilla is a C decompilation/port of the NES games *Super Mario Bros.* and *Super Mario Bros.: The Lost Levels*. This fork keeps the classic game and adds a lightweight online layer where two players share a room but play their own instance of the level.

The other player appears as a **ghost**: you can see their position and animation, but they do not collide with your enemies, blocks or physics. Interaction happens through events:

- **Shared power-ups:** if one player grabs a mushroom, fire flower or star, the other gets it too.
- **Eco blocks:** the level itself is shared. Bricks you break and `?` blocks you use disappear for both players, and power-ups pop out of the same block in both worlds.
- **Shared progress:** you wait for each other at the flagpole, and when you die you respawn on your partner's screen instead of the checkpoint.

This is intentionally **not** a shared-world co-op and **not** split-screen. Each player has their own camera on their own device. It is inspired by the ghost online mode of *Super Mario Bros. Wonder*.

## Current status

Implemented in the Windows build:

**Single-player**

- Infinite lives.
- No time limit (timer frozen at 999) and no time bonus at the flagpole.
- Keyboard and SDL2 gamepad support.
- Fullscreen toggle with **F11** / **Alt+Q**; the NES image is stretched to fill the window.
- Windows build launches as a GUI app, with no extra console window.
- In-game pause menu with **Continue** and **Main Menu** (returns to the
  Singleplayer/Multiplayer menu, not to the game title).
- **Skins**: Mario, Luigi, Ice Mario and Fire Mario, chosen from the main menu or
  the multiplayer menu with Left/Right.

**Online co-op (2 players)**

- Main menu with **Singleplayer** and **Multiplayer**.
- Multiplayer menu with **Host Game** / **Join Game**, 4-letter code entry and the
  skin selector.
- Ghost rendering: the other player's Mario is sent at ~30 Hz and drawn in your
  level, with their skin.
- Shared power-ups, eco bricks, eco `?` blocks, flag waits and respawns next to
  your partner (see [Co-op rules](#co-op-rules)).
- In-game overlay with the room code, connection state and the real network error
  message if something fails.

Still planned:

- Distinctive rendering for eco blocks (so you can tell which blocks are shared).
- Reconnection, chat and emotes.
- Raspberry Pi Zero 2 W build.

## Controls

Keyboard:

- `W A S D` — move
- `J` — run / fire (B)
- `K` — jump (A)
- `Enter` — Start
- `\` — Select
- `F11` — toggle fullscreen
- `Alt+Q` — toggle fullscreen
- While paused: `Up`/`Down` select, `Space` chooses, `Enter` continues
- In the menus: `W`/`S` or arrows move, `Enter`/`Space` choose, and on the
  **SKIN** entry `Left`/`Right` change your skin

Gamepad (SDL2, first controller):

- D-pad or left stick — move
- `A` — jump
- `B` or `X` — run / fire
- `Start` — Start
- `Back` — Select

## Co-op rules

- **Online only.** No local co-op and no split-screen: each player runs the game
  on their own machine.
- **Infinite lives.** Dying respawns you; there is no Game Over.
- **No time limit.** The timer is frozen and the hurry-up/death-by-time logic is
  disabled, with no time bonus at the flagpole.
- **Flag wait:** the first player to finish stops at the flagpole and waits. The
  level advances when the second player finishes (or after 60 seconds, or if they
  disconnect or die).
- **Death:** you respawn on your partner's screen, not at the checkpoint, with the
  area around them already loaded.
- **Skins are cosmetic:** they only change colors. The SMB1 engine shares the
  player palette with power-ups, so mushrooms and fire flowers are tinted to match
  your skin (this is intended).

See [docs/DISENO.md](docs/DISENO.md) for the full design and [docs/ROADMAP.md](docs/ROADMAP.md) for the implementation order.

## Requirements

- A **legally obtained** Super Mario Bros. NES ROM (`smb.nes`). **No ROM or original Nintendo assets are included in this repository.**
- Windows: C/C++ compiler, Meson, SDL3 (preferred) or SDL2.
- Raspberry Pi: Raspberry Pi OS, SDL2, Meson, Ninja and a C/C++ compiler.

## Build on Windows

### MSYS2 + MinGW (tested)

Install the toolchain from the MSYS2 MinGW64 shell:

```sh
pacman -S --needed mingw-w64-x86_64-gcc mingw-w64-x86_64-meson mingw-w64-x86_64-ninja mingw-w64-x86_64-pkgconf mingw-w64-x86_64-SDL2 mingw-w64-x86_64-libwebsockets
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
sudo apt install build-essential meson ninja-build libsdl2-dev libwebsockets-dev
meson setup build -Dsdl2=true -Dopengl=false
meson compile -C build
./build/src/platform/smbvanilla
```

- `-Dopengl=false` selects the software renderer, because the Zero 2 W only supports OpenGL ES 2.0 (the OpenGL renderer requires OpenGL 3.3).
- Target: 60 fps at the original 256x240 resolution, scaled to the display.
- Not tested on the Pi yet; the launcher and `.deb` packaging are still to do.

## Online

The room/relay server lives in its own repository:
[smb1-classic-multiplayer-server](https://github.com/chumimario02/smb1-classic-multiplayer-server).
It provides 4-letter room codes and a WebSocket relay. Run it with `npm start`
(defaults to port 8080).

How to play:

1. Start the server.
2. In the game: **Multiplayer → Host Game**. The room code appears at the top-left
   (`SALA XXXX`).
3. On the other PC: **Multiplayer → Join Game**, type that code and press Enter.
4. Both press **Enter** on the title screen to start the level.

Keep the host window open: if the host leaves, the room closes and the other
player can no longer join.

The command-line path is still available for testing:

```sh
# host
smbvanilla.exe smb.nes --online-host --room TEST --server ws://127.0.0.1:8080 --name HOST

# client
smbvanilla.exe smb.nes --online-join TEST --server ws://127.0.0.1:8080 --name CLIENT
```

For a remote server use `wss://your-server.example`. The status overlay appears
at the top-left while online is active.

### Known limitations

- Eco blocks only sync when the other player has that screen loaded (roughly two
  screens around their camera). If they are far away, the block stays as it is in
  their world until they get there.
- Enemies, coins, score and lives are **not** shared: each player has their own.
- The ghost is visual only. It never collides with your world.

## Troubleshooting on Windows

**"This app has been blocked by your system administrator" / "Una directiva de
Control de aplicaciones bloqueó este archivo"**

Windows 11 enables **Smart App Control** automatically on clean installs. After a
few days it switches from evaluation to enforced mode and blocks any unsigned
executable without cloud reputation, which includes every freshly compiled build
of this game. There is no per-app exclusion.

Disable it once:

*Settings → Privacy & security → Windows Security → App & browser control →
Smart App Control → Off* (then reboot).

Microsoft does not allow re-enabling it without reinstalling Windows, so read
their warning before turning it off.

## Credits and license

- Based on [SMB Vanilla](https://github.com/nukep/SMB-Vanilla) by nukep and contributors. Licensed under **GPL-3.0**; see [LICENSE.md](LICENSE.md).
- NES sound emulation: **Nes_Snd_Emu** by James Athey and **emu2413** by digital-sound-antiques.
- Original SMB disassembly reference: doppelganger.
- *Super Mario Bros.* is a trademark of Nintendo. This project is not affiliated with Nintendo. No ROMs or original Nintendo assets are included; you must provide your own legally obtained ROM. This is a non-commercial fan project.
