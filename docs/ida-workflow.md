# IDA Pro + ida-pro-mcp workflow

This explains *how Claude and the user collaborate inside IDA Pro* during an RE
session. It documents the configuration that's already in place plus the step-by-step
loop for editing the database from Claude.

---

## What's already configured

`.claude/settings.json`:

```json
{
  "mcpServers": {
    "ida-pro": {
      "url": "http://127.0.0.1:13337/sse"
    }
  }
}
```

This points Claude Code at the SSE endpoint exposed by **ida-pro-mcp**
(https://github.com/mrexodia/ida-pro-mcp). The server is started by the IDA
plugin, *inside* the running IDA Pro process, and only exists while a database
is open in IDA.

**Implication:** Claude can only act on the binary that's currently open in
IDA Pro. Open `ida/Launcher.exe.i64` to work on the launcher; close it and
open `ida/Wilbur.exe.i64` to work on the game.

---

## Per-session setup (you, the user)

1. **Start IDA Pro** and open the relevant database from `ida/`.
2. **Start the MCP plugin** in IDA: `Edit → Plugins → MCP` (or the keyboard
   shortcut shown in the plugin's submenu — typically `Ctrl+Alt+M`). The status
   bar should show something like *"MCP server listening on 127.0.0.1:13337"*.
3. **Start (or restart) Claude Code** in this project. On startup it reads
   `.claude/settings.json` and connects to the MCP endpoint.
4. Verify the connection: in Claude, ask something simple like "list the first
   10 named functions". If MCP isn't connected the request will fail with a
   clear error.

---

## The MCP tools we use most

A short tour of what's available (full list in
[ida-pro-mcp/README.md](https://github.com/mrexodia/ida-pro-mcp/blob/main/README.md)):

| Tool | Purpose |
|---|---|
| `lookup_funcs`, `list_funcs` | Find functions by name pattern. |
| `decompile`                   | Hex-Rays pseudocode for a function. |
| `disasm`                      | Raw disassembly for an address range. |
| `xrefs_to`, `xrefs_to_field`  | Find callers / readers / writers. |
| `callees`                     | List functions called from a given function. |
| `imports`                     | Walk the IAT — useful for finding D3D9, kernel32 calls. |
| `find`, `find_regex`, `find_bytes`, `find_insns` | Search by string, byte-pattern, or instruction. |
| `get_string`, `get_bytes`, `get_int` | Read constants / strings at addresses. |
| `set_comments`, `rename`, `set_type`, `declare_type` | **Write back** — annotate the database. |
| `patch_asm`, `put_int`        | Patch instructions / values. Use very sparingly — see below. |
| `analyze_funcs`, `infer_types`, `define_func`, `define_code`, `undefine` | Tell IDA about function boundaries. |
| `read_struct`, `search_structs` | Work with declared structs. |
| `idalib_*`                    | (Headless mode only) Open/switch databases programmatically. |

We **don't** use `patch_asm` or `put_int` to modify the binary on disk during
analysis — the `.i64` is the workspace, the binary stays unchanged. Patches
happen at runtime via the ASI mod, not at analysis time. The exception is
explicit experimental hex-hacks (e.g. validating the WSGF aspect-ratio float)
in a *copy* of Wilbur.exe placed in `_inspect/`, never in `Game/`.

---

## How Claude annotates the database

**The critical rule: never write to the database without telling the user.**

When Claude renames a function, declares a type, or sets a comment, that
mutation is **immediately saved into your `.i64`**. There's no "preview". So
the agreed protocol is:

1. Claude proposes the change in chat first ("I think `sub_4012A0` is the camera
   FOV setter — want me to rename it to `Camera::SetFov` and add a summary
   comment?").
2. User confirms (or course-corrects).
3. Claude calls the relevant MCP tool.
4. Claude states what was changed.

For batch annotations (e.g. "rename all 12 functions on this call chain"),
Claude lists them all up front, waits for approval, then applies as a batch.

If the user wants Claude to just go ("annotate freely, I'll review the
diff later"), the user says so explicitly. Default is *propose first*.

---

## Saving and committing

- **IDA's autosave** (`Options → Autosave`) protects the database mid-session.
- On exit, IDA writes the `.i64`. Both `*.i64` and the unpacked dump are
  gitignored — see [ida/README.md](../ida/README.md) for the rationale.
- The narrative work product (the `research/findings/*.md` files Claude writes
  alongside its RE work) **is** committed and is what gives the .i64 its
  context.

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Claude reports "MCP server unreachable" | IDA plugin not started, or IDA not running, or wrong DB. Re-do "Per-session setup". |
| MCP plugin doesn't appear in Edit → Plugins | Open a binary first (the plugin only registers once a DB is open). |
| Renames work but comments don't show up | Some clients cache; press F5 in the function view, or close-reopen the function. |
| Claude wants a function I haven't analyzed | Ask Claude to run `analyze_funcs` first, or extend auto-analysis manually in IDA (`Options → Reanalyze program`). |
| `decompile` returns empty | Hex-Rays isn't loaded, or the function is too small / has no proper prologue. Use `disasm` instead. |
