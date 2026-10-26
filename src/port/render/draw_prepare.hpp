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
// Port-only (132vc1, research/VITA.md "Render pass on the Vita's cores (132vc1)"): prepare_particle_groups' timing,
// totals since the start, kept only in the profile build (OMEGA_ZONE_PROFILE: the Vita's; zeros elsewhere).
// Microseconds; program_us / sprite_us add every thread's time (the work), wall_us is the callers' (the wait).
struct ParticlePrepareProfile {
    std::uint64_t calls = 0;        // prepare_particle_groups calls with at least one batch
    std::uint64_t single_calls = 0; // ... with one batch (nothing to spread)
    std::uint64_t batches = 0;
    std::uint64_t programs = 0;     // recorded MSCALs run (run_deferred_program)
    std::uint64_t particles = 0;    // the programs' particles (the sprite programs' inputs)
    std::uint64_t drawn = 0;        // sprite batches kept (not culled)
    std::uint64_t program_us = 0, sprite_us = 0, wall_us = 0;
};
ParticlePrepareProfile particle_prepare_profile();
// Port-only (132vc1): build_actor_mesh's pool tasks (play_main), the same profile build only. skin_us: the LOD0
// skinning (AnimatedCharacter::geometry's deferred materialisation, timed first); task_us: the rest of the task (gore,
// the far LOD's skin_parts, character_mesh). All threads' time.
struct ActorPiecesProfile {
    std::uint64_t tasks = 0, lod0 = 0, lod1 = 0, hidden = 0, vertices = 0, skin_us = 0, task_us = 0;
    std::uint64_t lod0_us = 0, lod1_us = 0, hidden_us = 0;  // task_us by the task's kind
};
// Port-only (132vc1 hw5), the profile build: one far-LOD actor's overlay work run on one thread with nothing else at
// the same time, `runs` times (build_actor_mesh, on request): the placed palette, skin_parts and character_mesh in
// microseconds, the first run (the caches as the frame left them) and the mean of the others.
struct ActorBench {
    unsigned runs = 0, parts = 0, chunks = 0, vertices = 0;
    double palette_first = 0, skin_first = 0, mesh_first = 0, palette_rest = 0, skin_rest = 0, mesh_rest = 0;
    // hw6: one LOD0 actor's geometry() skinning (AnimatedCharacter::timed_skinning), the same runs: palette, the parts
    // without morph, the face / hand parts; first run and the mean of the others. lod0_vertices 0: none was drawn.
    unsigned lod0_parts = 0, lod0_vertices = 0;
    double lod0_first[3]{}, lod0_rest[3]{};
};
void request_actor_bench();
bool actor_bench_due();  // true once per request
void actor_bench_store(const ActorBench& bench);
bool actor_bench_take(ActorBench& bench);  // the stored result, once
ActorPiecesProfile actor_pieces_profile();
// Adds one task's figures (lod: 0, 1, or -1 hidden).
void actor_pieces_count(int lod, std::size_t vertices, std::uint64_t skin_us, std::uint64_t task_us);
// The profile clock in microseconds (0 without OMEGA_ZONE_PROFILE).
std::uint64_t prepare_profile_clock_us();
// Port-only (132vc1), the profile build: the first recorded MSCAL of 16 particles or more that prepare_particle_groups
// met is kept; particle_bench runs it `runs` times on the calling thread in each particle mode (0 exact, 1 the
// speculative iterations, 2 the register-resident ones; the modes are restored after) and gives microseconds per
// particle (program and batch read-back). False when nothing was kept yet. hw5: us_one_batch is the same record cut
// to one particle, in microseconds per batch (the batch's fixed cost plus one particle).
bool particle_bench(unsigned runs, double (&us_per_particle)[3], double (&us_one_batch)[3], unsigned& particles);
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

// Port-only (132vc1): character_mesh(mesh, parts, textured, append, visible = false) for `part_count` parts without
// the parts' geometry: a hidden actor keeps one material slot per part (texture = its index), nothing else, so its
// caller need not materialise the actor's deferred skinning (AnimatedCharacter::geometry) to draw nothing.
void hidden_actor_materials(Mesh& mesh, std::size_t part_count, bool append);

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

// Port-only (132vc1, research/VITA.md "Render pass on the Vita's cores (132vc1)"): `mesh` cleared, then for each piece
// k in order append_actor_piece(mesh, *pieces[k], listed_textures[k]) followed by after_piece(k, v0, v1, s0) (its
// vertices [v0, v1), its first surface s0; after_piece may reorder the surfaces from s0, as play_main's record() does),
// with the same result: the materials and surfaces appended on this thread, the vertices and offset indices copied
// by the pool into a mesh sized once, the bounds widened from each piece's own (vertex by vertex for a piece with a
// NaN position). A piece with a GPU-skinned surface (132md8) makes it the one-by-one appends.
void append_actor_pieces(WorkerPool& pool, Mesh& mesh, std::span<const Mesh* const> pieces,
                         std::span<const std::size_t> listed_textures,
                         const std::function<void(std::size_t, std::size_t, std::size_t, std::size_t)>& after_piece);

}  // namespace omega
