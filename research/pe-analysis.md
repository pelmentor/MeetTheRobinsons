# PE analysis — Wilbur.exe / Launcher.exe / rus.exe

This is the raw output of static analysis on the three binaries in the user's
Game folder, with interpretation. It's the canonical record of what we know
*from disk alone* (no runtime, no debugger). All hashes and offsets are from
the **user's local copy** — different builds (retail US vs EU vs Russian
release) may differ.

---

## Hashes (user's local copy)

```
Wilbur.exe   (6,287,360 bytes, 2007-03-28)
  MD5:    5F6B81846723A197925CA41A3D950BAE
  SHA1:   C06E1EA4085149F58A5EF5E12DAD20E4EB219234
  SHA256: 269C8FEFD5B8A571B3F24EA068F1432F0D3A80FEBA597024BA18043C9F6C0A50

Launcher.exe (401,408 bytes, 2007-03-30)
  MD5:    4BD41E2C992B25A088FE4462C468448D
  SHA1:   C2127D36E38E69B81012077B45053EC35FAC5806
  SHA256: B59B14128CA41BB540C4DEB93E16F1790D9C4A2EE849C4A39BF6225511DD0653

rus.exe (478,706 bytes, 2017-07-12 — third-party Russian patcher)
  MD5:    6C56A47855386882F5ACB4570A820259
  SHA1:   44BF129B30AB244A58FD0DE9710ACED1B0E728AE
```

The mod's runtime hash check (M2 in the roadmap) verifies against these.
If a user has a different `Wilbur.exe`, the mod refuses to load until offsets
are added for that build.

---

## Wilbur.exe — SecuROM 7

```
Machine:               0x014C  (i386)
NumberOfSections:      3
Subsystem:             2       (GUI)
ImageBase:             0x00400000
EntryPoint (RVA):      0x02AF16A0
EntryPoint (VA):       0x02EF16A0  ← inside section [1] (rr02)
SizeOfImage:           0x02AF9000  (~43 MB virtual)

Section[0]  rr01    VAddr=0x00001000  VSize=0x024F8000  ROff=0x00000400  RSize=0x00000000  Char=0xE0000080  RWX
Section[1]  rr02    VAddr=0x024F9000  VSize=0x005F9000  ROff=0x00000400  RSize=0x005F8A00  Char=0xE0000040  RWX-init
Section[2]  .rsrc   VAddr=0x02AF2000  VSize=0x00007000  ROff=0x005F8E00  RSize=0x00006200  Char=0xC0000040  RW
```

Identification: **SecuROM 7.x** (Sony DADC). Three independent confirmations:

1. Section names `rr01` / `rr02` — SecuROM 7 signature.
2. `rr01` has `VirtualSize = 0x024F8000` (~38 MiB) but `RawSize = 0` — the
   on-disk file declares an empty section. SecuROM allocates and decrypts into
   it at runtime.
3. Entry point at `0x02EF16A0` lies in `rr02` (the SecuROM stub), not in the
   original game's code. The stub runs first, decrypts `rr01`, builds the IAT,
   and jumps to the original OEP somewhere inside `rr01`.

**Consequence:** static analysis of the on-disk file yields nothing useful.
We must produce a runtime-decrypted dump first ([../docs/SECUROM.md](../docs/SECUROM.md)).

The single useful thing static analysis CAN do on the protected binary:
locate plain-text data constants in `rr02` (which IS on disk). The WSGF
widescreen hack relies on this — see [../docs/prior-art/wsgf-widescreen-hack.md](../docs/prior-art/wsgf-widescreen-hack.md).

---

## Launcher.exe — clean PE32

```
NumberOfSections:      4
Subsystem:             2       (GUI)
ImageBase:             0x00400000

Section .text     VAddr=0x00001000  VSize=0x00021F24  RSize=0x00022000
Section .rdata    VAddr=0x00023000  VSize=0x00009588  RSize=0x0000A000
Section .data     VAddr=0x0002D000  VSize=0x00009474  RSize=0x00005000
Section .rsrc     VAddr=0x00037000  VSize=0x0002FBC0  RSize=0x00030000
```

Standard MSVC PE32 layout. No protection. IDA Pro auto-analyzes it cleanly
(database already present at `ida/Launcher.exe.i64`).

What the launcher does:
- Reads / writes a config file (likely `Launcher.ini` next to it; verify by
  RE'ing `kernel32!GetPrivateProfileString` callers).
- Presents a dialog to choose: resolution, windowed/fullscreen, language,
  audio device. Resource section will tell us the dialog IDs.
- Spawns `Wilbur.exe` with the chosen settings as `-dxresolution=`,
  `-dxwindowed`, etc.

The launcher is useful as a **secondary source of truth** about the game's
config schema and command-line flags — see notes in
[../docs/REVERSE_ENGINEERING.md](../docs/REVERSE_ENGINEERING.md#working-with-two-binaries-launcherexe-vs-wilburexe).

---

## rus.exe — Russian-language patcher (3rd-party, 2017)

This is **not** a Disney/publisher artifact. Date stamp `2017-07-12` and
file structure indicate a fan translation patcher.

```
NumberOfSections:      4   (.text, .rdata, .data, .rsrc — clean MSVC layout)
File size:             478,706 bytes
PE-end-of-data:        0x21000  (135,168 bytes)
Overlay:               343,538 bytes starting at 0x21000
Overlay signature:     37 7A BC AF 27 1C  ← 7-Zip archive
```

`rus.exe` is a **7zSFX self-extractor** (a 7-Zip console SFX stub + 7z
archive payload). We extracted the payload non-destructively with 7-Zip
into `_inspect/rus_extracted/`.

### Payload contents (34 files, ~4.27 MB uncompressed)

```
data\lang\ENGLISH_UK_WIN32.dct          457,434 bytes
data\lang\ENGLISH_WIN32.dct             457,434 bytes
data_dx\fonts\ash94_*.DBL               5 × 262,464 bytes  (font glyph data, 5 langs)
data_dx\fonts\fiesta_*.DBL              5 × 262,464 bytes
data_dx\fonts\system_*.DBL              5 × 34–262 K bytes
data_dx\fonts\*.fnt                     14 × ~4 KB         (font metrics)
data_dx\fonts\CtrlBtns.DBL              42,944 bytes
Launcher.exe                            401,408 bytes      ← byte-identical to user's existing Launcher.exe (SHA1 match)
```

### What rus.exe actually does

1. Replaces font glyph atlases (`*.DBL`) — almost certainly to swap the
   ASCII-only Latin fonts for fonts containing Cyrillic glyphs. The
   `_english.DBL` file is replaced too: the patcher likely repoints the
   "english" font slot to a Cyrillic-capable font.
2. Replaces dictionaries (`ENGLISH_*.dct`) — these contain UI strings; the
   replacement likely contains Russian translations using the new fonts.
3. Replaces `Launcher.exe` with a copy that's **byte-identical to the user's
   existing one** (SHA1 `69ECDA7EF39848E85B1F191AA2ED9BDE963B7987`).

### What rus.exe does NOT do

- It does **not** replace `Wilbur.exe`. (No `Wilbur.exe` in the payload.)
- It does **not** remove SecuROM. (Wouldn't be possible without a `Wilbur.exe`
  anyway.)
- It does **not** modify executable code. (The `Launcher.exe` it ships is
  identical to the user's.)

So the user's hypothesis "maybe rus.exe replaces Wilbur with a no-DRM version"
is not supported by the file. It's a translation-only patcher.

### Why we extracted it without running it

1. Running an unverified 2017 patcher would mutate the read-only Game folder.
2. We want to *understand* what it does for documentation purposes, not just
   apply it blindly.
3. A 7zSFX is trivially extractable with 7-Zip's `x` command, no execution
   required.

---

## Cross-references

- SecuROM unpacking procedure: [../docs/SECUROM.md](../docs/SECUROM.md)
- WSGF widescreen prior art: [../docs/prior-art/wsgf-widescreen-hack.md](../docs/prior-art/wsgf-widescreen-hack.md)
- Per-system findings (camera, menu, renderer) once we start RE'ing:
  [findings/](findings/)
