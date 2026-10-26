#pragma once
// Port-only (checkpoint 132mt3, research/THREADING.md step 5): the CPU draw preparation of the play's render pass
// spread over the WorkerPool (worker_pool.hpp), merged in the original order so the results are the same bytes as
// the single-threaded path. Only pure computation moves: every game-state write stays on the simulation thread in
// the original order.
//
//  * Particles: a draw run under DeferredDraws records its VU1 MSCALs (ParticleSystem::deferred) while it writes the
//    emitter / definition words and the VU uploads as before; prepare_particle_groups then runs each batch's
//    recorded program (run_deferred_program) and its sprite program (particle_sprite_batch) on the pool, one batch
//    per index, and keeps the drawn sprite batches in batch order.
//  * Characters: each drawn actor's far-LOD skinning, gore subtracts and overlay vertices are built into its own
//    piece on the pool (play_main's build_actor_mesh); append_actor_piece then appends the pieces in actor order with
//    the offsets character_mesh would have given and the same running bounds.
// With 0 workers nothing is deferred and every call runs inline: the old path.
#include "actor_hit_volumes.hpp"
#include "animated_character.hpp"
#include "geometry.hpp"
#include "particle_asset.hpp"
#include "particle_emitter.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <vector>
#if defined(__vita__)
#include <atomic>
#endif

namespace omega {

#if defined(__vita__)
// Port-only, PS Vita only (132vg2, research/VITA.md "Vita render path (132vg2)"): prepare_particle_groups' time
// split for the play log (timing only): the calls, the batches, the recorded VU1 programs run, the summed worker
// time in the programs (run_deferred_program) and in the sprite programs (particle_sprite_batch), and the calls'
// wall time (microseconds, totals since the start).
struct ParticlePrepareTimes {
    std::atomic<std::uint64_t> calls{0}, batches{0}, programs{0}, program_us{0}, sprite_us{0}, wall_us{0};
};
inline ParticlePrepareTimes g_particle_prepare_times;

// Port-only, PS Vita only (132vg2, research/VITA.md "Vita render path (132vg2)"): the scene frame's model filing
// (play_main, 0013e250) tested against the drawn (wide) view's clip with the scene frame's culler 003c8990, which
// the port did not apply to the drawn models: the frames, the models filed, those outside the view (code 0x80), and
// their surfaces and indices. With g_scene_cull (default; vita_test_scene_cull_off.txt clears it) those models are not
// drawn: they produce no pixels.
struct SceneCullStats {
    std::atomic<std::uint64_t> frames{0}, models{0}, culled{0}, surfaces{0}, culled_surfaces{0}, indices{0},
        culled_indices{0};
};
inline SceneCullStats g_scene_cull_stats;
inline bool g_scene_cull = true;
#endif

class WorkerPool;
namespace gfx {
struct ReplacementDraw;
}

// Sets system.deferred to `records` (cleared first) for its lifetime when the pool has workers; null again after.
class DeferredDraws {
public:
    DeferredDraws(particle::ParticleSystem& system, std::vector<particle::DeferredProgram>& records, const WorkerPool& pool);
    ~DeferredDraws();
    DeferredDraws(const DeferredDraws&) = delete;
    DeferredDraws& operator=(const DeferredDraws&) = delete;

private:
    particle::ParticleSystem& system_;
    std::vector<particle::DeferredProgram>* previous_;
};

// One object's (or effect component's) batches with the frame its sprites are drawn under.
struct ParticleGroup {
    std::vector<particle::ParticleBatch>* batches = nullptr;
    std::array<particle::Qword, 4> cull{};
    particle::SpriteFrame frame;
    std::vector<particle::ParticleSpriteBatch> sprites;  // out: particle_sprites(*batches, cull, frame, skip)
};

// Runs the groups' recorded MSCALs (a batch whose `deferred` names a record in `records`) and the sprite programs,
// spread over the pool, and fills each group's `sprites` in batch order. Afterwards every batch is filled as the
// serial draw fills it. A failure is the one the serial order meets first: a program's before any sprite program's,
// each in batch order (groups in order).
void prepare_particle_groups(WorkerPool& pool, std::vector<ParticleGroup>& groups,
                             const std::vector<particle::DeferredProgram>& records, bool skip_heat_haze);
// One group: the drawn sprite batches of `batches`.
std::vector<particle::ParticleSpriteBatch> prepare_particle_sprites(WorkerPool& pool,
                                                                    std::vector<particle::ParticleBatch>& batches,
                                                                    const std::vector<particle::DeferredProgram>& records,
                                                                    const std::array<particle::Qword, 4>& cull,
                                                                    const particle::SpriteFrame& frame,
                                                                    bool skip_heat_haze);

// Character parts as one untextured mesh (vertices already world-placed); moved here from play_main.cpp unchanged
// (checkpoint 132mt3) so that the worker path and the tests share it.
// One surface per part; material i samples overlay texture i when present
// (white base colour), otherwise the diagnostic flat colour.
// With `append`, the parts are added after those already in the mesh (several
// characters in one overlay); material i is then the i-th part overall.
// `textures`, when given, names part i's overlay image instead (a far LOD image).
// `gore`, when given, holds part i's per-vertex character gore subtracts
// (character_gore_vertices; empty: none) for a LOD0 draw.
void character_mesh(Mesh& mesh,const std::vector<AnimatedCharacterGeometry>& parts,const std::vector<bool>& textured_parts,bool append=false,bool visible=true,
                    const std::vector<std::uint32_t>* textures=nullptr,const std::vector<std::vector<CharacterGoreSubtract>>* gore=nullptr);

// Appends `piece` (one character_mesh call's output on an empty mesh) to `mesh` as that call with append on `mesh`
// gives: vertices as they are, indices + the vertex offset, surfaces with the material and index offsets, materials
// (the first `listed_textures` took their texture from a far-LOD list and keep it; the others name their own material
// index: + the material offset), and the bounds widened vertex by vertex in order (the first vertex of an empty mesh
// sets them).
void append_actor_piece(Mesh& mesh, const Mesh& piece, std::size_t listed_textures);

// Checkpoint 132md7 (port-only, research/MODDING.md "Speed"): the same two with each large part's (piece's) vertices and
// indices written by the pool in chunks (one chunk, or no workers: on this thread). The output equals the serial
// functions' (the bounds are the chunks' minima / maxima combined in order; a NaN position may widen them differently).
// For a mod's replacement parts (hundreds of thousands of vertices); the disc's draws keep the serial functions.
// `sources` (optional, per part; a source without `fill`: the part's geometry) draws a part from its source instead of
// its geometry: `count` vertices, fill(begin, end, out) writing out[k - begin]'s position, normal and uv for each k in
// [begin, end) (on any thread; a failure is rethrown, the lowest range's), the colour and gore as for a geometry
// vertex, the triangles' corners indexing them. A mod replacement is so skinned straight into the mesh, the vertex
// values those of its skinned geometry (gfx::skin_replacement_vertex).
// character_mesh sizes the vertex and index arrays once for all its parts (append false: resized, not cleared, so a
// mesh kept from the last frame is only truncated): growing a vector writes its new elements on this thread.
// Checkpoint 132md8: a source with `skin` k (> 0) is skinned on the GPU (scene_renderer.hpp OverlaySkin k - 1): it adds
// no vertex or index, only its material and a surface {material, 0, 3 * triangles, k} (indices into the model's own
// buffer), and widens the bounds by lo / hi (when `bounded`: gfx::replacement_bounds).
struct PartSource {
    std::size_t count = 0;
    std::span<const std::array<std::uint32_t, 3>> triangles;
    std::function<void(std::size_t begin, std::size_t end, MeshVertex* out)> fill;
    std::uint32_t skin = 0;
    std::array<float, 3> lo{}, hi{};
    bool bounded = false;
};
void character_mesh(WorkerPool& pool,Mesh& mesh,const std::vector<AnimatedCharacterGeometry>& parts,const std::vector<bool>& textured_parts,bool append=false,
                    bool visible=true,const std::vector<std::uint32_t>* textures=nullptr,const std::vector<std::vector<CharacterGoreSubtract>>* gore=nullptr,
                    const std::vector<PartSource>* sources=nullptr);
// A mod replacement part's source: `draw` (kept alive while the source is used) skinned vertex by vertex
// (gfx::skin_replacement_vertex), its triangles; the mesh is the one its skinned geometry (skin_replacement_draw_into)
// gives.
PartSource replacement_source(const gfx::ReplacementDraw& draw);
void append_actor_piece(WorkerPool& pool, Mesh& mesh, const Mesh& piece, std::size_t listed_textures);
// 132md8: the mesh has a GPU-skinned surface (bounds without vertices).
bool has_skin_surface(const Mesh& mesh);

}  // namespace omega
