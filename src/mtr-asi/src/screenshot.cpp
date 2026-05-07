#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace mtr::log { void info(const char* fmt, ...); }

namespace mtr::screenshot {

namespace {

std::atomic<bool>     g_request{false};
std::atomic<int>      g_counter{0};
std::atomic<uint64_t> g_last_path_tag{0}; // for status line
char                  g_last_path[MAX_PATH] = {};

#pragma pack(push, 1)
struct BMPFileHeader {
    uint16_t magic;       // 'BM'
    uint32_t fileSize;
    uint16_t reserved1;
    uint16_t reserved2;
    uint32_t dataOffset;
};
struct BMPInfoHeader {
    uint32_t headerSize;  // 40
    int32_t  width;
    int32_t  height;      // positive => bottom-up
    uint16_t planes;
    uint16_t bpp;
    uint32_t compression;
    uint32_t imageSize;
    int32_t  xPixelsPerMeter;
    int32_t  yPixelsPerMeter;
    uint32_t colorsUsed;
    uint32_t importantColors;
};
#pragma pack(pop)

bool ensure_dir(const char* dir) {
    DWORD attr = GetFileAttributesA(dir);
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return true;
    return CreateDirectoryA(dir, nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

void make_filename(char* out, size_t cap) {
    SYSTEMTIME t; GetLocalTime(&t);
    int n = g_counter.fetch_add(1) & 0xFFFF;
    sprintf_s(out, cap, "screenshots\\mtr_%04u%02u%02u_%02u%02u%02u_%03u.bmp",
              t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, n);
}

bool write_bmp_24(const char* path, UINT w, UINT h, const void* src, int srcPitch, D3DFORMAT fmt) {
    // BMP 24-bit rows are aligned up to 4 bytes, bottom-up.
    const int row_bytes = static_cast<int>(((w * 3 + 3) / 4) * 4);
    const int data_size = row_bytes * static_cast<int>(h);

    BMPFileHeader fh{};
    fh.magic      = 0x4D42; // 'BM'
    fh.fileSize   = sizeof(BMPFileHeader) + sizeof(BMPInfoHeader) + data_size;
    fh.dataOffset = sizeof(BMPFileHeader) + sizeof(BMPInfoHeader);

    BMPInfoHeader ih{};
    ih.headerSize  = 40;
    ih.width       = static_cast<int32_t>(w);
    ih.height      = static_cast<int32_t>(h);
    ih.planes      = 1;
    ih.bpp         = 24;
    ih.compression = 0;
    ih.imageSize   = static_cast<uint32_t>(data_size);

    FILE* f = nullptr;
    if (fopen_s(&f, path, "wb") != 0 || !f) {
        mtr::log::info("screenshot: fopen_s failed for %s", path);
        return false;
    }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);

    std::vector<uint8_t> row(static_cast<size_t>(row_bytes), 0);
    const uint8_t* base = static_cast<const uint8_t*>(src);

    // Both X8R8G8B8 and A8R8G8B8 have byte order in memory (LE): B, G, R, X/A.
    // BMP wants B, G, R per pixel — direct copy of first 3 bytes per pixel.
    const bool is_argb_like =
        (fmt == D3DFMT_X8R8G8B8 || fmt == D3DFMT_A8R8G8B8);

    for (int y = static_cast<int>(h) - 1; y >= 0; --y) {
        const uint8_t* srcRow = base + y * srcPitch;
        if (is_argb_like) {
            for (UINT x = 0; x < w; ++x) {
                row[x * 3 + 0] = srcRow[x * 4 + 0]; // B
                row[x * 3 + 1] = srcRow[x * 4 + 1]; // G
                row[x * 3 + 2] = srcRow[x * 4 + 2]; // R
            }
        } else {
            // Unknown format: dump zeros and warn once.
            static bool warned = false;
            if (!warned) {
                mtr::log::info("screenshot: unsupported backbuffer format %u; pixels will be black", fmt);
                warned = true;
            }
            std::memset(row.data(), 0, static_cast<size_t>(row_bytes));
        }
        fwrite(row.data(), static_cast<size_t>(row_bytes), 1, f);
    }
    fclose(f);
    return true;
}

bool capture_now(IDirect3DDevice9* dev) {
    if (!dev) return false;

    IDirect3DSurface9* bb = nullptr;
    HRESULT hr = dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    if (FAILED(hr) || !bb) {
        mtr::log::info("screenshot: GetBackBuffer failed hr=0x%08lX", hr);
        return false;
    }

    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);

    IDirect3DSurface9* sys = nullptr;
    hr = dev->CreateOffscreenPlainSurface(desc.Width, desc.Height, desc.Format,
                                          D3DPOOL_SYSTEMMEM, &sys, nullptr);
    if (FAILED(hr) || !sys) {
        mtr::log::info("screenshot: CreateOffscreenPlainSurface failed hr=0x%08lX (%ux%u fmt=%u)",
                       hr, desc.Width, desc.Height, desc.Format);
        bb->Release();
        return false;
    }

    hr = dev->GetRenderTargetData(bb, sys);
    if (FAILED(hr)) {
        mtr::log::info("screenshot: GetRenderTargetData failed hr=0x%08lX", hr);
        sys->Release(); bb->Release();
        return false;
    }

    D3DLOCKED_RECT lr{};
    hr = sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
    if (FAILED(hr)) {
        mtr::log::info("screenshot: LockRect failed hr=0x%08lX", hr);
        sys->Release(); bb->Release();
        return false;
    }

    if (!ensure_dir("screenshots")) {
        mtr::log::info("screenshot: cannot create 'screenshots' dir (err=%lu)", GetLastError());
        sys->UnlockRect(); sys->Release(); bb->Release();
        return false;
    }

    char path[MAX_PATH];
    make_filename(path, sizeof(path));
    bool ok = write_bmp_24(path, desc.Width, desc.Height, lr.pBits, lr.Pitch, desc.Format);

    sys->UnlockRect();
    sys->Release();
    bb->Release();

    if (ok) {
        strcpy_s(g_last_path, sizeof(g_last_path), path);
        g_last_path_tag.fetch_add(1);
        mtr::log::info("screenshot: wrote %s (%ux%u fmt=%u)", path, desc.Width, desc.Height, desc.Format);
    }
    return ok;
}

} // namespace

void request() { g_request.store(true); }

void try_capture(IDirect3DDevice9* dev) {
    if (!g_request.exchange(false)) return;
    capture_now(dev);
}

const char* last_path()      { return g_last_path; }
uint64_t    last_path_tag()  { return g_last_path_tag.load(); }

} // namespace mtr::screenshot
