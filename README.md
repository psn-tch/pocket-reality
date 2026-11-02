# Pocket Reality

A tiny Mac OS 9 Platinum-style system for the **HP Jornada 680**, a 133 MHz
handheld from 1999 with no floating-point hardware and 16 MB of RAM.

It boots from a CompactFlash card into a small Finder and comes with:

- **Pocket Write**: a plain text editor with live spell checking, autosave,
  crash recovery and optional per-story passwords.
- **Pocket Code**: write and run Lua programs on the device itself, with
  drawing, keyboard and stylus input.
- **Doom and Wolfenstein 3D**: silent ports, tuned for this screen. You supply
  the game data.
- **The desktop**: Pocket HD and a Trash, folders, app switching, a battery
  percentage, and brightness and contrast controls.

> **Made with AI.** Pocket Reality was written by [Claude](https://claude.com/claude-code),
> Anthropic's AI, working with its human owner, who set the goals, tested every
> build on a real Jornada, and decided what stayed. Almost all of the code here was
> written by AI.

Version 0.9.2. It runs on real hardware, but it is a hobby project for one
machine, so expect rough edges. Doom has a rare crash that is still unexplained.

## What you need

- An HP Jornada 680, and a CompactFlash card of about 4 GB.
- Linux on a PC to build everything.
- The JLime "Donkey" image for the Jornada, which provides the
  Linux 2.6.17 system this runs on. It is started from Windows CE with `shlo.exe`.

**Starting Linux wipes the Jornada's Windows CE RAM.** Back up anything in it first.

## Building

Toolchains and third-party sources are not in this repository. Put them here:

| Path | What |
| --- | --- |
| `tools/sh3/` | A static `sh3-linux-musl` C toolchain, from [musl-cross-make](https://github.com/richfelker/musl-cross-make) |
| `tools/sh3-cxx/` | The same with C++, for Wolfenstein 3D |
| `vendor/lua/lua-5.4.7/` | [Lua 5.4.7](https://www.lua.org) |
| `vendor/doomgeneric/` | [doomgeneric](https://github.com/ozkl/doomgeneric) |
| `vendor/wolf4sdl/` | [Wolf4SDL](https://github.com/fabiangreffrath/wolf4sdl), SDL2 version |
| `vendor/jlime-donkey/` | The JLime image |

Then:

    make            # the desktop, Pocket Write and Pocket Code
    make test       # every test, on the PC
    make -C src/doom
    make -C src/wolf

`build/` holds the results.

## Art and game data

Nothing Apple or commercial is in this repository, and none should be added.

With no art installed, the desktop draws plain stand-in shapes and works the
same. To use your own images, convert them with `scripts/make-art.py`.

The Games folder includes launchers for Doom and Wolfenstein 3D, but no game
files. Without them, the launchers say what is missing. Doom looks for
`doom/Doomu.wad` on the card, then for `doom/freedoom1.wad`, and Wolfenstein 3D
for lower-case `.wl1` files in `wolf3d/`.

The easy free option for Doom is [Freedoom: Phase 1](https://freedoom.github.io),
which is free to share: copy its `freedoom1.wad` to the `doom` folder. For
Wolfenstein 3D, the build expects the shareware v1.4 files, which you can find
online. Otherwise, use data you own. Freedoom has not been tried on a Jornada yet,
and it is larger, so it may need more memory than Doom.

## Writing to a card

`scripts/write-card.sh` and `scripts/update-card.sh` **only work on one specific
card**: they refuse anything else, by exact device ID and size. Read them and
change those values for yours before using them, and be certain which disk is
your card.

## Controls

| Where | Keys |
| --- | --- |
| Finder | Arrows to move, Enter to open, Backspace to go up, Esc for menus, Tab to list open apps |
| Pocket Write | Esc for menus, Ctrl+S to save, Ctrl+P for a password, Ctrl+B for the screen |
| Pocket Code | Ctrl+R to run, Esc to stop. See `Reference.lua` |
| Doom and Wolfenstein | Arrows, Ctrl to fire, Alt to strafe, Space for doors, Shift to run, ` to switch apps |

A forgotten password cannot be recovered. Passwords hide what a story says, not
its name, and programs in Pocket Code can reach every file on the card.

## Licences

Pocket Reality's own code is under the [MIT licence](LICENSE). Bundled and
linked third-party work keeps its own terms: see [THIRD-PARTY.md](THIRD-PARTY.md).
Builds of Doom are under the GPL, and Wolfenstein 3D's code has its own
non-commercial terms, so check those before sharing binaries.

This is an unofficial fan project. It is not affiliated with Apple, HP, id
Software or anyone else named here, and all trademarks belong to their owners.
