# third_party/

Vendored dependencies. We pin everything to commits / tags rather than
floating on `master` — reproducible builds matter for a preservation project.

## minhook

[TsudaKageyu/minhook](https://github.com/TsudaKageyu/minhook) — MIT-licensed
inline-hook library, x86 + x64. The de-facto choice for Windows game mods.

To add it as a submodule (run from the project root, `MeetTheRobinsons/`):

```powershell
git submodule add https://github.com/TsudaKageyu/minhook.git src/mtr-asi/third_party/minhook
git -C src/mtr-asi/third_party/minhook checkout v1.3.3
git submodule update --init --recursive
```

The CMakeLists.txt for mtr-asi expects MinHook at
`src/mtr-asi/third_party/minhook/CMakeLists.txt`.

## imgui

[ocornut/imgui](https://github.com/ocornut/imgui) — MIT-licensed immediate-mode UI.
The mod's primary in-game menu is built on top of ImGui; the **docking** branch is
preferred (still source-compatible with master). Backends used:
`backends/imgui_impl_win32.cpp` and `backends/imgui_impl_dx9.cpp`. Rendered from the
hooked `IDirect3DDevice9::EndScene`.

```powershell
git submodule add -b docking https://github.com/ocornut/imgui.git src/mtr-asi/third_party/imgui
git -C src/mtr-asi/third_party/imgui checkout v1.91.5-docking
git submodule update --init --recursive
```

ImGui has no upstream CMakeLists.txt; our own `src/mtr-asi/CMakeLists.txt`
compiles the necessary source files into a static `imgui` target.

## (planned) mINI

[pulzed/mINI](https://github.com/pulzed/mINI) — single-header INI parser, MIT.
Will be vendored when we wire up `config.cpp`.

```powershell
# When ready:
git submodule add https://github.com/pulzed/mINI.git src/mtr-asi/third_party/mINI
git -C src/mtr-asi/third_party/mINI checkout 0.9.18
```

---

## On Ultimate ASI Loader

We do **not** vendor [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)
because it's a *binary* dependency for the user, not a build-time dep for us.
Users download `dinput8.dll` (or whichever proxy) directly from upstream releases
and drop it into `Game/`. Our mod just builds a regular DLL renamed to `.asi`;
the loader does the loading.
