# Prior art — WSGF "Disney's Meet the Robinsons" widescreen hack

Distillation of the published static hex-patch from the Widescreen Gaming Forum
([wsgf.org/dr/disneys-meet-robinsons](https://www.wsgf.org/dr/disneys-meet-robinsons/en),
local copy at [`reference/Disney's Meet the Robinsons _ WSGF_widescreen_HACK.pdf`](../../reference/)).

This is not our solution — we want a runtime menu, not a static binary patch — but it
gives us **two extremely valuable clues** about the engine.

---

## The patch (verbatim from WSGF)

### Wilbur.exe

Find float bytes for `1.333333f` (= 4:3 = the engine's hardcoded aspect):

```
AB AA AA 3F   →   CD CC CC 3F     (=  1.6      = 16:10)
AB AA AA 3F   →   39 8E E3 3F     (=  1.7777…  = 16:9)
AB AA AA 3F   →   AB AA AA 40     (=  5.333…   = 48:9 / triple-screen ultrawide)
```

These are little-endian IEEE-754 single-precision floats. Decoded:

| Bytes (LE)        | Float                  | Aspect ratio |
|-------------------|-----------------------|--------------|
| `AB AA AA 3F`     | `1.3333333` (4/3)     | 4:3 — original |
| `CD CC CC 3F`     | `1.6`        (8/5)    | 16:10 |
| `39 8E E3 3F`     | `1.7777778` (16/9)    | 16:9 |
| `AB AA AA 40`     | `5.3333335` (16/3)    | 48:9 (triple-1080p surround) |

### Launcher.exe

Find the ASCII string `"1280x1024"` in `.rdata` and replace with your target
resolution (e.g. `"2560x1440"`). The launcher hands this off to the game via
the `-dxresolution=` CLI flag.

### Command-line alternative

```
Wilbur.exe -dxresolution=WIDTHxHEIGHT [-dxwindowed]
```

This works without any binary patching — the game's argv parser already accepts it.

---

## What this tells us about the engine

### Clue 1 — There's exactly one aspect-ratio constant in `.rdata`

Many engines compute aspect at runtime as `width/height`. This one stores the
ratio as a **literal `1.333333f` constant** in the data section. Implications:

- Whatever function computes the projection matrix loads this constant
  via `fld dword ptr [<address>]` or `movss xmm?, [<address>]`.
- That function is either **the** projection-matrix builder or a
  helper called by it.
- Once Wilbur.exe is dumped (post-SecuROM), we can find this constant
  by byte-pattern (`AB AA AA 3F`), then xref it. The xrefs are our
  reverse-engineering entry points for the camera. **This is gold.**
- The 48:9 hack writing `1.5.333…` (which is *5×* the 4:3 ratio, not the
  actual 48:9 ratio of `48/9 = 5.333…`) tells us the engine **uses this value
  as a direct multiplier** for one of the projection-matrix terms — almost
  certainly `proj[0][0]` (horizontal scale). It's not a "true aspect ratio"
  field, it's a fudge constant the original code applies to the FOV.

### Clue 2 — Resolution is wired in as a launcher-passed string

The game itself doesn't read a resolution from a config file — it reads it
from `argv` (passed by `Launcher.exe`). So the parsing path is:

```
Launcher.exe   ─reads─►  Launcher.ini / registry
              ─writes─►  cmdline:  Wilbur.exe -dxresolution=WxH
                              │
                              ▼
Wilbur.exe argv parser  ─►  D3DPRESENT_PARAMETERS::BackBufferWidth/Height
```

For our M1 (resolution selector in the *game's* main menu), we need to
either:
1. **Patch the resolution after `D3DPRESENT_PARAMETERS` is built** —
   requires hooking the device-create path and forcing a reset/re-create
   when the user picks a new resolution in-game.
2. **Catch the argv parse** and inject our own value before the game even
   calls `CreateDevice` — only useful for the *initial* resolution, not
   for live-switching from the menu.

(1) is the right answer. (2) might be a useful extra for "remember last
chosen resolution between sessions".

---

## Why we still need to unpack Wilbur.exe

The hex-hack works on the protected binary because **the float `1.333333` lives
in `.rdata`** which (probably) ended up in `rr02` post-link, and `rr02` has its
raw bytes on disk (`RawSize = 0x5F8A00`). SecuROM doesn't bother encrypting all
data — it focuses on code. So pure-data patches survive.

But for an ASI mod we need to *call* and *hook* code, all of which is in the
encrypted `rr01`. There's no way around dumping. See [../SECUROM.md](../SECUROM.md).

---

## To-do for our project (drives M3/M4)

- [ ] Once Wilbur.exe is dumped: locate the four bytes `AB AA AA 3F` in the
      dump's `.rdata` via `find_bytes`.
- [ ] List all xrefs to that address.
- [ ] For each xref, decompile the containing function. The right one will be
      doing matrix math (`fmul`, `fdiv`, or SSE `mulss` against camera FOV).
- [ ] Name that function `Camera::ApplyAspectRatio` (provisional). Save the
      finding to `research/findings/camera.md`.
- [ ] In our M7 implementation, hook this function so we can replace the
      hardcoded `1.333333` with `width / height` (from the runtime
      `D3DPRESENT_PARAMETERS`).
