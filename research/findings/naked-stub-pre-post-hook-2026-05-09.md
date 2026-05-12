# Naked-stub PRE/POST hook pattern (2026-05-09)

Status: **shipped + verified working**. Used by `widget_probe` to hook
`sub_4E9350` (SubmitSprite) for the UI element identity refactor.
Reusable for any future MinHook detour that needs to capture
caller-side register state AND/OR pair the original function's return
value with PRE-call data.
Governing rule: [feedback_no_crutches.md (RULE №1)](../../memory/feedback_no_crutches.md).

## When to use this pattern

Default to a normal C++ MinHook detour (`int __cdecl my_hook(args...)`).
The naked-stub pattern is overkill unless you need at least one of:

1. **Caller-side register values at hook entry.** A normal C++ detour
   runs MSVC's prologue first, which may clobber `EBP`/`EBX`/`ESI`/
   `EDI` before your code sees them. If the caller is `__thiscall` and
   keeps `this` in a callee-saved register (very common), you cannot
   recover `this` from a normal detour — you can only see the args on
   the stack.

2. **The original function's return value PAIRED with PRE-call data.**
   A normal detour can do `auto r = orig(...); /* post */ return r;` —
   that works for simple cases. The naked-stub becomes useful when the
   PRE-call dispatcher is heavy (custom signature, state capture
   into globals) and you want a single dispatcher entry point that's
   independent of the C++ function signature.

3. **You need the trampoline call to run with the ORIGINAL stack
   layout** (not the layout MSVC would build for your detour). A few
   x86 calling conventions or hand-rolled engine code make assumptions
   about `[esp+N]` that a C++-prologue-mangled stack would violate.

For widget_probe, all three applied: `this` is in callee-saved regs
for some callers, we wanted to pair the SpriteEntry* return with the
widget_name captured PRE-call, and using `__cdecl` arg-passing for the
dispatcher made it independent of sub_4E9350's 8-arg signature.

## The pattern

### Layout

```
┌──────────────────────────┐
│ engine call site         │  push args; call sub_X
│                          │       │
│                          │       v
└──────────────────────────┘   ┌─────────────────────────────┐
                                │ MinHook JMP at sub_X+0      │
                                │       │                     │
                                │       v                     │
                                │   hk_X_naked (this file)    │
                                │   ┌─────────────────────┐   │
                                │   │ PRE-dispatch        │   │
                                │   │  ─pushad            │   │
                                │   │  ─push esp          │   │
                                │   │  ─call dispatch_pre │   │
                                │   │  ─add esp, 4        │   │
                                │   │  ─popad             │   │
                                │   ├─────────────────────┤   │
                                │   │ FORWARD to trampoline│   │
                                │   │  ─pop static_slot   │   │
                                │   │  ─call [trampoline] │   │
                                │   │  ─push static_slot  │   │
                                │   ├─────────────────────┤   │
                                │   │ POST-dispatch       │   │
                                │   │  ─push eax (save)   │   │
                                │   │  ─push eax (arg)    │   │
                                │   │  ─call dispatch_post│   │
                                │   │  ─add esp, 4        │   │
                                │   │  ─pop eax           │   │
                                │   ├─────────────────────┤   │
                                │   │ ret                 │   │
                                │   └─────────────────────┘   │
                                └─────────────────────────────┘
```

### Code template

```cpp
// File-scope (NOT inside an unnamed namespace — keeps extern-C linkage clean).
extern "C" void* g_orig_SubmitSprite_trampoline = nullptr;
extern "C" void* g_caller_ret_save              = nullptr;

// PUSHAD on x86 stores: EAX, ECX, EDX, EBX, ESP_before, EBP, ESI, EDI
// in that order (high addr to low addr). After PUSHAD, [esp+0] = EDI,
// [esp+28] = EAX. So our "PushadBlock" struct, when the dispatcher
// reads it, has fields in order: EDI, ESI, EBP, ESP_unused, EBX, EDX,
// ECX, EAX, then ret_addr (saved by the engine's `call`), then args.
struct PushadBlock {
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    uint32_t ret_addr;
    // followed by N cdecl args, then caller's stack frame above
};

extern "C" void __cdecl widget_probe_dispatch_pre(const void* pushad_block);
extern "C" void __cdecl widget_probe_dispatch_post(void* return_value);

extern "C" __declspec(naked) void __cdecl hk_SubmitSprite_naked() {
    __asm {
        ; STACK ON ENTRY: [caller_ret(esp+0), arg1(esp+4), ..., arg8(esp+32)]

        ; --- PRE: snapshot register state ---
        pushad
        push    esp                                     ; pass &pushad_block
        call    widget_probe_dispatch_pre
        add     esp, 4                                  ; cdecl arg cleanup
        popad

        ; --- Forward to trampoline ---
        ; CRITICAL: store engine's caller_ret in a STATIC slot, NOT a
        ; register. The trampoline is __cdecl (free to clobber EAX/ECX/
        ; EDX). If we used `pop ecx; call [tramp]; push ecx; ret`, the
        ; trampoline would clobber ECX and the push+ret would jump to
        ; garbage -> ACCESS_VIOLATION at runtime.
        pop     dword ptr [g_caller_ret_save]
        call    dword ptr [g_orig_SubmitSprite_trampoline]
        push    dword ptr [g_caller_ret_save]

        ; STACK NOW: [caller_ret, arg1, ..., arg8].  EAX = original return.

        ; --- POST: pair return value with PRE-captured state ---
        push    eax                                     ; save return value
        push    eax                                     ; pass as cdecl arg
        call    widget_probe_dispatch_post
        add     esp, 4
        pop     eax                                     ; restore return

        ret
    }
}
```

### MinHook setup (unchanged from normal detours)

```cpp
MH_STATUS s = MH_CreateHook(reinterpret_cast<void*>(kFnVA),
                            reinterpret_cast<void*>(&hk_SubmitSprite_naked),
                            &g_orig_SubmitSprite_trampoline);
MH_EnableHook(reinterpret_cast<void*>(kFnVA));
```

## Pitfalls + lessons

### 1. The pop-register / push-register pattern doesn't work for forwarding

The "obvious" pattern `pop ecx; call [trampoline]; push ecx; ret` fails
because the trampoline (= original function in `__cdecl`) is allowed
to clobber EAX/ECX/EDX. After `call [trampoline]` returns, ECX holds
GARBAGE, and `push ecx; ret` will jump to garbage and crash.

**Always use a static memory slot** (`g_caller_ret_save`) for the
forward-call save. Single-threaded sprite submission makes this safe.

### 2. `extern "C"` symbols + unnamed namespaces conflict

Don't put `extern "C"` declarations inside `namespace { ... }`.
Internal linkage (unnamed namespace) and external linkage (extern "C")
are mutually exclusive. MSVC silently accepts the conflict but then
your inline-asm `call symbol` may fail to resolve at link time, OR
work in debug but break in release. Keep extern-C symbols at file
scope.

### 3. Inline asm + C++ exception handling

`__declspec(naked)` is fine — there's no MSVC-managed prologue, so no
exception unwind tables. But the dispatcher functions you call from
naked asm CAN have C++ destructor-having locals (std::scoped_lock,
std::string, etc) — those work normally because they're called like
any other C++ function.

### 4. Register-candidate filtering matters for hot paths

When sub_4E9350 fires ~100x per frame and you scan EBX/EBP/ESI/EDI as
`this` candidates, naive filtering `cand >= 0x400000` admits MANY
junk values that pass through `VirtualQuery` (slow Win32 API). Tighten
to the actual heap range (`0x00800000..0x10000000` for Wilbur). Saved
us a hang in the first integration test.

## Reference

- Implementation:
  [src/mtr-asi/src/widget_probe.cpp](../../src/mtr-asi/src/widget_probe.cpp)
- Header: [src/mtr-asi/include/mtr/widget_probe.h](../../src/mtr-asi/include/mtr/widget_probe.h)
- Phase 0.5 RE findings:
  [research/findings/ui-widget-system-phase0-2026-05-09.md](./ui-widget-system-phase0-2026-05-09.md)
- MinHook (vendored): `src/mtr-asi/third_party/minhook`
- x86 PUSHAD reference: Intel SDM Vol. 2A §3.2 "PUSHA/PUSHAD"
