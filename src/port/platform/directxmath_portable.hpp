#pragma once
// Port-only (checkpoint 132vt2, research/VITA.md "The play host"): the few DirectXMath names scene_renderer.cpp uses
// for its camera matrices, for the non-Windows builds (the PS Vita; 132ps5c: the PS5) (the Windows build includes
// <DirectXMath.h>). Portable scalar C++ with no platform API (moved from src/port/vita in 132ps5c). Each function
// reproduces the operation order of DirectXMath's SSE2 path, which the x64 build compiles (no /arch flag), so the
// matrices are the same floats on both targets when the inputs are: XMMatrixMultiply adds (x*r0 + z*r2) + (y*r1 +
// w*r3); XMVector3Dot is (x*x' + y*y') + z*z'; XMVectorNegate is 0 - v (+0 for a zero lane, as _mm_sub_ps);
// XMScalarSinCos is DirectXMath's 11/10-degree minimax polynomial, not the C library's sin/cos. Cortex-A9's VFPv3 /
// NEON multiply-accumulate is unfused (it rounds the product), so a contracted a*b+c still rounds as the SSE pair.
// The PS5 (x86-64, clang) compiles it without FMA and with -ffp-contract=off (cmake/ps5/OmegaPs5.cmake).
// Never part of the Windows build.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace DirectX {

inline constexpr float XM_PI = 3.141592654F;
inline constexpr float XM_2PI = 6.283185307F;
inline constexpr float XM_1DIV2PI = 0.159154943F;
inline constexpr float XM_PIDIV2 = 1.570796327F;

inline constexpr float XMConvertToRadians(float degrees) noexcept { return degrees * (XM_PI / 180.0F); }

struct XMFLOAT4 {
    float x, y, z, w;
};
struct XMFLOAT4X4 {
    float m[4][4];
};

struct XMVECTOR {
    float v[4];
};
struct XMMATRIX {
    XMVECTOR r[4];
};

inline XMVECTOR XMVectorSet(float x, float y, float z, float w) noexcept { return {{x, y, z, w}}; }
inline XMVECTOR XMVectorSubtract(XMVECTOR a, XMVECTOR b) noexcept {
    return {{a.v[0] - b.v[0], a.v[1] - b.v[1], a.v[2] - b.v[2], a.v[3] - b.v[3]}};
}
inline XMVECTOR XMVectorNegate(XMVECTOR a) noexcept { return XMVectorSubtract({{0, 0, 0, 0}}, a); }

inline XMVECTOR XMVector3Dot(XMVECTOR a, XMVECTOR b) noexcept {
    const float d = (a.v[0] * b.v[0] + a.v[1] * b.v[1]) + a.v[2] * b.v[2];
    return {{d, d, d, d}};
}
inline XMVECTOR XMVector3Cross(XMVECTOR a, XMVECTOR b) noexcept {
    return {{a.v[1] * b.v[2] - a.v[2] * b.v[1], a.v[2] * b.v[0] - a.v[0] * b.v[2], a.v[0] * b.v[1] - a.v[1] * b.v[0], 0.0F}};
}
inline XMVECTOR XMVector3Normalize(XMVECTOR a) noexcept {
    const float length_sq = (a.v[0] * a.v[0] + a.v[1] * a.v[1]) + a.v[2] * a.v[2];
    const float length = std::sqrt(length_sq);
    XMVECTOR r{};
    if (length_sq == INFINITY) {
        constexpr std::uint32_t qnan = 0x7fc00000U;
        float n;
        std::memcpy(&n, &qnan, sizeof n);
        return {{n, n, n, n}};
    }
    if (length != 0.0F)
        for (int i = 0; i < 4; ++i) r.v[i] = a.v[i] / length;
    return r;
}

inline XMMATRIX XMMatrixTranspose(const XMMATRIX& m) noexcept {
    XMMATRIX t;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) t.r[i].v[j] = m.r[j].v[i];
    return t;
}

inline XMMATRIX XMMatrixMultiply(const XMMATRIX& a, const XMMATRIX& b) noexcept {
    XMMATRIX m;
    for (int i = 0; i < 4; ++i) {
        const float x = a.r[i].v[0], y = a.r[i].v[1], z = a.r[i].v[2], w = a.r[i].v[3];
        for (int j = 0; j < 4; ++j) {
            const float vx = x * b.r[0].v[j], vy = y * b.r[1].v[j], vz = z * b.r[2].v[j], vw = w * b.r[3].v[j];
            m.r[i].v[j] = (vx + vz) + (vy + vw);
        }
    }
    return m;
}
inline XMMATRIX operator*(const XMMATRIX& a, const XMMATRIX& b) noexcept { return XMMatrixMultiply(a, b); }

inline XMMATRIX XMMatrixLookToLH(XMVECTOR eye, XMVECTOR direction, XMVECTOR up) noexcept {
    const XMVECTOR r2 = XMVector3Normalize(direction);
    const XMVECTOR r0 = XMVector3Normalize(XMVector3Cross(up, r2));
    const XMVECTOR r1 = XMVector3Cross(r2, r0);
    const XMVECTOR neg_eye = XMVectorNegate(eye);
    XMMATRIX m;
    m.r[0] = {{r0.v[0], r0.v[1], r0.v[2], XMVector3Dot(r0, neg_eye).v[0]}};
    m.r[1] = {{r1.v[0], r1.v[1], r1.v[2], XMVector3Dot(r1, neg_eye).v[0]}};
    m.r[2] = {{r2.v[0], r2.v[1], r2.v[2], XMVector3Dot(r2, neg_eye).v[0]}};
    m.r[3] = {{0, 0, 0, 1}};
    return XMMatrixTranspose(m);
}
inline XMMATRIX XMMatrixLookToRH(XMVECTOR eye, XMVECTOR direction, XMVECTOR up) noexcept {
    return XMMatrixLookToLH(eye, XMVectorNegate(direction), up);
}
inline XMMATRIX XMMatrixLookAtRH(XMVECTOR eye, XMVECTOR focus, XMVECTOR up) noexcept {
    return XMMatrixLookToLH(eye, XMVectorSubtract(eye, focus), up);
}

inline void XMScalarSinCos(float* sin_out, float* cos_out, float value) noexcept {
    float quotient = XM_1DIV2PI * value;
    quotient = value >= 0.0F ? static_cast<float>(static_cast<int>(quotient + 0.5F))
                             : static_cast<float>(static_cast<int>(quotient - 0.5F));
    float y = value - XM_2PI * quotient;
    float sign = +1.0F;
    if (y > XM_PIDIV2) {
        y = XM_PI - y;
        sign = -1.0F;
    } else if (y < -XM_PIDIV2) {
        y = -XM_PI - y;
        sign = -1.0F;
    }
    const float y2 = y * y;
    *sin_out = (((((-2.3889859e-08F * y2 + 2.7525562e-06F) * y2 - 0.00019840874F) * y2 + 0.0083333310F) * y2 - 0.16666667F) * y2 + 1.0F) * y;
    const float p = ((((-2.6051615e-07F * y2 + 2.4760495e-05F) * y2 - 0.0013888378F) * y2 + 0.041666638F) * y2 - 0.5F) * y2 + 1.0F;
    *cos_out = sign * p;
}

inline XMMATRIX XMMatrixPerspectiveFovRH(float fov_y, float aspect, float near_z, float far_z) noexcept {
    float sin_fov, cos_fov;
    XMScalarSinCos(&sin_fov, &cos_fov, 0.5F * fov_y);
    const float range = far_z / (near_z - far_z);
    const float height = cos_fov / sin_fov;
    XMMATRIX m{};
    m.r[0].v[0] = height / aspect;
    m.r[1].v[1] = height;
    m.r[2].v[2] = range;
    m.r[2].v[3] = -1.0F;
    m.r[3].v[2] = range * near_z;
    return m;
}

inline void XMStoreFloat4(XMFLOAT4* out, XMVECTOR v) noexcept { *out = {v.v[0], v.v[1], v.v[2], v.v[3]}; }
inline void XMStoreFloat4x4(XMFLOAT4X4* out, const XMMATRIX& m) noexcept {
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) out->m[i][j] = m.r[i].v[j];
}

}  // namespace DirectX
