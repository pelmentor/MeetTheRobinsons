# Reverse engineering — workflow and conventions

This is the agreed style guide for *how* we record findings, so future-us
(and anyone reading the .i64 / `research/`) can follow the work without
having to re-do it.

---

## The two-channel rule

We keep findings in **two synchronised places**:

1. **The IDA database (`ida/*.i64`)** — facts about the binary that fit in
   IDA's data model:
   - **Function names** (set via the MCP `rename` tool — see
     [ida-workflow.md](ida-workflow.md)).
   - **Function and parameter types** (`set_type`, `infer_types`).
   - **Repeatable comments** (visible at every xref) and **anterior comments**
     (visible above the function — used for the "summary" of what a function does).
   - **Stack frame variable names** (`declare_stack`).
   - **Struct layouts** (`declare_type` for the C-style declaration).

2. **`research/findings/<topic>.md`** — narrative explanations:
   - What this system does in the game ("the camera handles…").
   - How the C++ layer ties to the Lua/script layer (if at all).
   - Cross-cutting traces (e.g. "resolution flows from `Launcher.ini` →
     command-line → `D3DPRESENT_PARAMETERS::BackBufferWidth`").
   - Diagrams.
   - References to external sources (PCGW, WSGF, blog posts).

**Don't put narrative in IDA comments and don't put offset tables in
markdown.** Each tool serves one purpose.

---

## Naming conventions inside IDA

We're working with a 2007-era C++ engine (Avalanche Software's tech). Use
these prefixes/suffixes consistently — they make IDA's symbol search far
more useful:

### Functions

| Pattern | Meaning | Example |
|---|---|---|
| `Foo::Bar`              | Inferred class method (IDA renders `__thiscall`-with-`this`-as-first-arg this way) | `Camera::SetFov` |
| `sub_<RVA>`             | Untouched IDA default | leave as-is until you understand it |
| `nullsub_<n>`           | Empty function (just `ret`) | leave as-is |
| `j_<target>`            | Jump-thunk (IAT, /Gw stuff) | leave as-is |
| `g_<global>`            | Global variable | `g_pD3DDevice`, `g_pCamera` |
| `RTTI_*`                | RTTI metadata | leave as-is |
| `?<mangled>` / `<class>::<method>` | Demangled C++ | preserve namespace if obvious |
| `Hk_<original>`         | Reserved prefix used **in our mod source** for trampoline targets — do NOT use inside the IDA database itself |

For functions where you're 80%+ confident of the purpose but not certain, prefix
with **`maybe_`**: `maybe_Camera__UpdateProjection`. This signals to a future reader
that the name is provisional.

For functions that are *fragments* you've identified (e.g. an inner block after
function-splitting): prefix with **`frag_`**.

### Globals

- `g_<lowerCamel>` for variables: `g_renderDevice`, `g_currentMenu`.
- `kFoo` for compile-time constants: `kAspectRatio4_3`, `kMaxFps`.
- `RVA: 0x???? — see <topic>.md` as a repeatable comment for any global the docs
  describe in detail.

### Stack frames

- Preserve VC++-runtime decoration where IDA produced something like `var_28`
  *only* if you don't know what it is. Once known, rename.
- Use Hungarian-ish prefixes for clarity: `i` for int, `p` for pointer,
  `s` for string, `f` for float, `v` for vector.

### Structures

- Reverse-engineer with `struct` declarations passed to `declare_type`.
- Name them by responsibility, **not** by RTTI guess: `CameraData`, not `CCamera_v2`.
- Keep a copy of the `.h` declaration in `research/findings/<topic>.md` so a
  reader sees the layout without opening IDA.
- Field names: `<lowerCamel>` for non-pointers, prefix `m_` only if the original
  symbols suggest the engine used that style (it'll show up in RTTI strings).

---

## Comment style inside IDA

- **Anterior comment** on every function we've named: a single paragraph (≤ 6
  lines). State *what the function does* and *its key inputs / outputs*. Don't
  paste pseudocode — that's already visible.
- **Repeatable comments** on key call sites and globals. Repeatable comments
  show up at every xref, so a reader chasing a global's accesses sees the
  context everywhere.
- **In-line (regular) comments** sparingly, only where the disassembly's
  meaning isn't obvious from the names. E.g. for a magic constant: `; 1.333… =
  4:3 aspect; see prior-art/wsgf-widescreen-hack.md`.
- **Don't** narrate the obvious. `mov eax, [ebp+arg_4]   ; load arg_4 into eax`
  is anti-information.

---

## Workflow for a single RE session

For each system you tackle (camera, menu, renderer, …):

1. **Set the goal** for the session (one sentence). Example: "Identify the
   function that fills `D3DPRESENT_PARAMETERS`."
2. **Find an entry point.** Two reliable techniques:
   - String search (Alt+T or `find` MCP tool) for keywords: `"d3d"`, `"present"`,
     `"resolution"`, `"camera"`, `"fov"`, `"main_menu"`, etc.
   - Import xrefs: jump to `IDirect3D9::CreateDevice` and look at its callers.
3. **Pseudocode-first.** Decompile the function (`decompile` MCP tool). Read it
   end-to-end. Don't disassemble unless Hex-Rays gives up.
4. **Annotate as you understand.** Rename → comment → declare types. Keep
   making things readable; don't try to understand the whole function at once.
5. **Trace one level out.** Look at the function's callers and callees once.
   Rename the obvious ones. Stop before you go on a tangent.
6. **Write the narrative.** When you're done with this session's piece, write
   (or update) `research/findings/<topic>.md`. State: what does this do, where
   does it live, what calls it, what does it call. Link function VAs.
7. **Commit.** Git-track the markdown change. (The .i64 is gitignored — its
   updates are local.)

---

## Working with two binaries (Launcher.exe vs Wilbur.exe)

`Launcher.exe` and `Wilbur.exe` share *very little* code (different VC++
runtimes, different time stamps), so don't expect cross-binary symbol matching
to work. But the **launcher does know things** that are useful for reverse-engineering
the game:

- The launcher reads/writes the same `*.ini` file the game does. RE'ing the
  launcher's INI parser tells you the schema → tells you what fields the game
  also reads.
- The launcher includes the same string keys for resolutions ("1280x1024", etc.)
  used by the game's command-line parser (`-dxresolution=`).
- The launcher's UI strings (resource section) hint at what's user-configurable.

So: when stuck on Wilbur, sometimes the answer is in Launcher.

---

## Security / safety norms

- **Never delete the original `Wilbur.exe.i64` if it has user work in it** — even
  if it was made from the protected binary. Rename to `Wilbur.exe.broken.i64`
  and start a fresh DB from the dump.
- **Don't run `rus.exe`, `UNWISE.EXE`, or any other unverified executable** in
  the Game folder. We've already confirmed `rus.exe` is a benign Russification
  patcher, but the principle stands.
- **The Game/ folder is the user's licensed install.** Treat it as read-only.
  Any patching we do happens to *copies* in `_inspect/` or via runtime hooks.
