#pragma once
// Bullet manager draw 002755f0 and tracer geometry 002757a0. The manager
// visits the 50 active records and enters the renderer for a tracer (+117)
// or particle (+110). Retail weapon records leave the particle resource +134
// at -1, so tracer records use the eight-vertex diamond prism below.
#include "bullet_pool.hpp"
#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace omega {
enum class BulletTracerFilter : std::uint8_t { nearest };
struct BulletTracerMaterial {
    std::int32_t texture{-1};             // record +11c: *00236f20(0x1a), tracer01.tdx
    std::uint64_t gs_alpha{0x0000008000000044ULL};
    BulletTracerFilter filter{BulletTracerFilter::nearest}; // render mode 5: TEX1 MMAG/MMIN nearest
    bool operator==(const BulletTracerMaterial&) const=default;
};
using BulletTracerUvTable=std::array<RootVector,16>;
// cBullet::sUvSets at 004a90a0, filled by __sinit_bullet.cc
// 004d2420..004d2644. Each phase selects four consecutive STQ rows.
const BulletTracerUvTable& bullet_tracer_uv_sets();
struct BulletTracerMesh {
    std::array<RootVector,8> vertices{};
    std::array<RootVector,8> uv{};
    std::array<RootVector,8> colour{};
    std::array<std::uint16_t,36> indices{};
    BulletTracerMaterial material{};
    std::uint8_t phase{};                  // record +140: 0, 4, 8 or 12
};
std::optional<BulletTracerMesh> bullet_tracer_mesh(const BulletRecord& bullet);
std::vector<BulletTracerMesh> bullet_tracer_meshes(const BulletPool& bullets);
}
