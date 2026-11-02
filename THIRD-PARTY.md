# Third-party work

## In this repository

- `src/third_party/monocypher.*`: [Monocypher](https://monocypher.org) 4.0.2. See
  `src/third_party/MONOCYPHER-LICENCE.md`.
- `src/ui/font_data.h`: generated from
  [Chicago Kare](https://github.com/KingDuane/Chicago-Kare) by Duane King. Its licence is below.

## Used to build, not in this repository

- [Lua](https://www.lua.org): MIT.
- [doomgeneric](https://github.com/ozkl/doomgeneric): GPL. `src/doom/pocket_doom.c` is a backend
  for it, so a built Doom is covered by the GPL.
- [Wolf4SDL](https://github.com/fabiangreffrath/wolf4sdl): see its own licence files.
  `src/wolf` only holds this project's backend and SDL stand-ins, but a built game
  falls under Wolf4SDL's terms, which forbid selling it.

Freedoom works with Pocket Reality but is not included. It is
[BSD-licensed](https://github.com/freedoom/freedoom/blob/master/COPYING.adoc).

## Chicago Kare licence

MIT License

Copyright (c) 2024 Duane King

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
