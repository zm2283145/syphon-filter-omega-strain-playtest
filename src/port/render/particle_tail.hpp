#pragma once
// Port-only (132vc2, research/VITA.md "Main-thread CPU (132vc2)"): a list-5120 particle object's draw tail off the
// simulation thread. On the PS Vita the scene draw's particle hooks cost the simulation thread about 21 ms a frame:
// each object's recorded VU1 programs and sprite programs (prepare_particle_groups, a pool call of its own per
// object, the caller waiting for its last batch) and its vertices (ParticleRenderer::draw).
//
// start_particle_tail takes what the object's draw recorded (its batches, the DeferredProgram records, its frame
// block) right after the draw ran, with the textures and the definition words the sprites read (+d0, and the effect
// name for an error) copied as they are now, and hands the work to the pool as one task: every recorded program in
// batch order, then every batch's sprite program, then ParticleRenderer::build_sprites; the simulation thread goes on
// with the scene draw. RenderThread::reserve keeps the object's place in the frame. finish_particle_tail (the
// simulation thread, before the frame is handed off: after the frame's other draws) waits for the task, rethrows its
// error (a program's before any sprite program's, each in batch order, as the serial path), adds the counts, and
// records the uploads and draws into the kept place (RenderThread::fill + ParticleRenderer::emit_sprites). The render
// thread executes the frame in the same order as when every object drew in turn: the same vertices, the same commands.
// The task reads only its own copies (the definitions may be rewritten by later draws meanwhile; the emitters, read
// only for an error's text, live until the next step).
#include "particle_asset.hpp"
#include "particle_emitter.hpp"
#include "particle_renderer.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <string>
#include <vector>

namespace omega {

class WorkerPool;

struct ParticleTail {
    std::vector<particle::ParticleBatch> batches;
    std::vector<particle::DeferredProgram> records;
    std::vector<std::unique_ptr<particle::ParticleDefinition>> defs;  // copies (+d0, the effect name)
    std::vector<const gfx::Texture*> views;                           // by copy, in defs order
    std::array<particle::Qword, 4> cull{};
    particle::SpriteFrame frame;
    bool skip_heat_haze = false, screen_fx = false, frame_copy = false;
    float width = 0, height = 0;
    std::size_t slot = 0;  // RenderThread::reserve's
    std::string source;    // the play's capture-frame label (empty: not counted)
    // Out (the task):
    std::vector<particle::ParticleSpriteBatch> sprites;
    ParticleRenderer::PreparedSprites prepared;
    std::exception_ptr error;
    std::uint64_t work_us = 0;
};

// `batches` is moved from. `records` are the draw's (copied).
std::shared_ptr<ParticleTail> start_particle_tail(WorkerPool& pool, const ParticleRenderer& renderer,
                                                  std::vector<particle::ParticleBatch>&& batches,
                                                  const std::vector<particle::DeferredProgram>& records,
                                                  const ParticleFrame& frame, bool skip_heat_haze, std::size_t slot);
// The task's work (also what the serial order computes): public for the tests.
void run_particle_tail(ParticleTail& tail);

// Profile (the PS Vita play log, totals since the start; microseconds): tails started, their batches, the tasks'
// work, the simulation thread's wait for them at the finish, and its emit time.
struct ParticleTailProfile {
    std::uint64_t tails = 0, batches = 0, work_us = 0, wait_us = 0, emit_us = 0;
};
ParticleTailProfile particle_tail_profile();
void particle_tail_count_finish(std::uint64_t wait_us, std::uint64_t emit_us);

// Port-only switch: the play uses the tails where it may (the PS Vita: on by default; vita_test_particle_tail_off.txt
// clears it; the PC: --particle-tail, a diagnostic).
inline std::atomic<bool> g_particle_tail{false};

}  // namespace omega
