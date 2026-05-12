// MSAA (multisample anti-aliasing) override.
//
// Overrides D3DPRESENT_PARAMETERS at CreateDevice / Reset time so the
// D3D9 device runs with the requested multisample count. Under DXVK
// (Wilbur's runtime path post Phase-C), this maps to native Vulkan
// VK_SAMPLE_COUNT_xx_BIT — hardware MSAA on the GPU, not a post-process.
//
// Caps gating: samples back down through 16/8/4/2/NONE until both the
// back-buffer format and the auto-depth-stencil format pass
// IDirect3D9::CheckDeviceMultiSampleType. SwapEffect is forced to
// DISCARD when MSAA > NONE (D3D9 spec requires it).

#pragma once

#include <cstdint>

struct IDirect3D9;
struct IDirect3DDevice9;
struct _D3DPRESENT_PARAMETERS_;
typedef struct _D3DPRESENT_PARAMETERS_ D3DPRESENT_PARAMETERS;
enum _D3DDEVTYPE : int;
typedef enum _D3DDEVTYPE D3DDEVTYPE;

namespace mtr::msaa {

bool     enabled();
void     set_enabled(bool v);

uint32_t sample_count();           // 0 (off) / 2 / 4 / 8 / 16
void     set_sample_count(uint32_t n);

uint32_t actual_count();           // last-applied count after cap-check
uint32_t actual_quality();         // last-applied quality

// Called from d3d9_hook. Mutates pp->MultiSampleType /
// MultiSampleQuality (and SwapEffect when needed) before forwarding to
// the engine's real CreateDevice/Reset.
void apply_for_create_device(D3DPRESENT_PARAMETERS* pp,
                             IDirect3D9* d3d,
                             unsigned int adapter,
                             D3DDEVTYPE devtype);
void apply_for_reset(IDirect3DDevice9* dev, D3DPRESENT_PARAMETERS* pp);

} // namespace mtr::msaa
