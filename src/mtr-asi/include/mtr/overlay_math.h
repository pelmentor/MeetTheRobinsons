// Shared projection / clip / NDC math for world-space debug overlays.
//
// Used by:
//   - mtr::trigger_overlay (AABB wireframes)
//   - mtr::npc_overlay     (text labels at NPC world pos)
//
// All routines operate in D3D9 conventions:
//   - Row-vector × matrix (D3DMATRIX is row-major; v' = v * M).
//   - Row 3 of the world matrix = camera world position (rigid LookAt-RH).
//   - Clip-space frustum (D3D9):  -w <= x,y <= w   AND   0 <= z <= w.
//   - NDC → screen with Y axis inverted (D3D9 vs OpenGL):
//       screen_y = (1 - (ndc_y * 0.5 + 0.5)) * viewport_height
//
// Inline-only — no separate .cpp. Single source of truth so a sign-flip
// fix lands in both overlays at once.

#pragma once

#include <cmath>
#include <cstdint>

namespace mtr::overlay_math {

struct Vec4 { float x, y, z, w; };

// Row-vector left-multiplied by row-major 4×4 matrix. M[r*4+c] layout
// matches D3DMATRIX::m[row][col].
inline Vec4 row_mul(const Vec4& v, const float M[16]) {
    Vec4 out;
    out.x = v.x * M[0]  + v.y * M[4]  + v.z * M[8]   + v.w * M[12];
    out.y = v.x * M[1]  + v.y * M[5]  + v.z * M[9]   + v.w * M[13];
    out.z = v.x * M[2]  + v.y * M[6]  + v.z * M[10]  + v.w * M[14];
    out.w = v.x * M[3]  + v.y * M[7]  + v.z * M[11]  + v.w * M[15];
    return out;
}

// out = A * B, both row-major.
inline void mat_mul(const float A[16], const float B[16], float out[16]) {
    for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
            float s = 0.0f;
            for (int k = 0; k < 4; ++k) {
                s += A[r * 4 + k] * B[k * 4 + c];
            }
            out[r * 4 + c] = s;
        }
    }
}

inline bool is_finite_vec4(const Vec4& v) {
    return std::isfinite(v.x) && std::isfinite(v.y)
        && std::isfinite(v.z) && std::isfinite(v.w);
}

// Six planes in D3D9 clip space. A point is inside plane i iff
// dist_i(p) >= 0. Layout matches mtr::trigger_overlay::plane_dist.
//   0: w + x  (left)
//   1: w - x  (right)
//   2: w + y  (bottom)
//   3: w - y  (top)
//   4: z      (near — D3D9 z range is [0, w], NOT [-w, w])
//   5: w - z  (far)
inline float plane_dist(int plane, const Vec4& v) {
    switch (plane) {
        case 0: return v.w + v.x;
        case 1: return v.w - v.x;
        case 2: return v.w + v.y;
        case 3: return v.w - v.y;
        case 4: return v.z;
        case 5: return v.w - v.z;
    }
    return 0.0f;
}

// Single-point frustum test for D3D9 clip space. Returns true iff the
// point is inside ALL 6 frustum planes. Use for label-style overlays
// where partial clipping doesn't make sense (you either show the label
// or you don't).
//
// Critical for performance with many labels: rejects all off-screen
// labels (in front of camera but outside lateral / vertical / depth
// frustum) so we don't waste AddText/snprintf on them.
inline bool point_in_frustum(const Vec4& clip) {
    if (!is_finite_vec4(clip))    return false;
    if (clip.w <= 1.0e-4f)        return false;  // behind near plane
    for (int p = 0; p < 6; ++p) {
        if (plane_dist(p, clip) < 0.0f) return false;
    }
    return true;
}

// 4D homogeneous lerp — clipped value = a + t * (b - a) component-wise.
inline Vec4 vec4_lerp(const Vec4& a, const Vec4& b, float t) {
    Vec4 r;
    r.x = a.x + t * (b.x - a.x);
    r.y = a.y + t * (b.y - a.y);
    r.z = a.z + t * (b.z - a.z);
    r.w = a.w + t * (b.w - a.w);
    return r;
}

// Homogeneous parametric line clip in clip space, BEFORE perspective
// divide. Used for line-segment rendering where partial visibility
// matters (e.g. AABB edges crossing the near plane).
//
// Returns false if the line is fully outside any one plane (whole-line
// reject). When endpoints straddle a plane, lerps the endpoint at
// t = d_a / (d_a - d_b). Never divides by w until all 6 planes survive.
inline bool clip_segment(Vec4 a, Vec4 b, Vec4* out_a, Vec4* out_b) {
    if (!is_finite_vec4(a) || !is_finite_vec4(b)) return false;
    for (int p = 0; p < 6; ++p) {
        const float d_a = plane_dist(p, a);
        const float d_b = plane_dist(p, b);
        const bool a_in = d_a >= 0.0f;
        const bool b_in = d_b >= 0.0f;
        if (!a_in && !b_in) return false;
        if (a_in && b_in)   continue;
        const float denom = d_a - d_b;
        if (std::fabs(denom) < 1.0e-7f) return false;
        const float t = d_a / denom;
        if (!std::isfinite(t)) return false;
        if (a_in) b = vec4_lerp(a, b, t);
        else      a = vec4_lerp(a, b, t);
        if (!is_finite_vec4(a) || !is_finite_vec4(b)) return false;
    }
    *out_a = a;
    *out_b = b;
    return true;
}

// NDC → screen with D3D9 Y-flip. Caller must guarantee clip.w > 0
// (point passed point_in_frustum or survived clip_segment).
inline void ndc_to_screen(const Vec4& clip, float vp_w, float vp_h,
                          float* sx, float* sy) {
    const float inv_w = 1.0f / clip.w;
    const float ndc_x = clip.x * inv_w;
    const float ndc_y = clip.y * inv_w;
    *sx = (ndc_x * 0.5f + 0.5f) * vp_w;
    *sy = (1.0f - (ndc_y * 0.5f + 0.5f)) * vp_h;     // D3D9 Y-flip
}

} // namespace mtr::overlay_math
