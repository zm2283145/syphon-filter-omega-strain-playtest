// Port-only (132vc2): see particle_tail.hpp.
#include "particle_tail.hpp"

#include "worker_pool.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <utility>

namespace omega {

namespace {
std::uint64_t now_us() {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}
std::atomic<std::uint64_t> g_tails{0}, g_batches{0}, g_work_us{0}, g_wait_us{0}, g_emit_us{0};
}  // namespace

std::shared_ptr<ParticleTail> start_particle_tail(WorkerPool& pool, const ParticleRenderer& renderer,
                                                  std::vector<particle::ParticleBatch>&& batches,
                                                  const std::vector<particle::DeferredProgram>& records,
                                                  const ParticleFrame& frame, bool skip_heat_haze, std::size_t slot) {
    auto tail = std::make_shared<ParticleTail>();
    tail->batches = std::move(batches);
    tail->records = records;
    tail->cull = frame.cull;
    tail->frame = frame.sprite;
    tail->skip_heat_haze = skip_heat_haze;
    tail->screen_fx = renderer.screen_fx_ready();
    tail->frame_copy = renderer.frame_copy_ready();
    tail->slot = slot;
    tail->prepared.targets = renderer.targets();
    tail->width = static_cast<float>(renderer.target_width());
    tail->height = static_cast<float>(renderer.target_height());
    std::vector<const particle::ParticleDefinition*> originals;
    originals.reserve(tail->batches.size());
    tail->defs.reserve(tail->batches.size());
    tail->views.reserve(tail->batches.size());
    for (auto& b : tail->batches) {
        if (!b.def) continue;
        const auto it = std::find(originals.begin(), originals.end(), b.def);
        std::size_t k = static_cast<std::size_t>(it - originals.begin());
        if (it == originals.end()) {
            originals.push_back(b.def);
            auto copy = std::make_unique<particle::ParticleDefinition>();
            // The words particle_sprite_batch reads: +d0 (ALPHA_1 / the haze GIF tags) and the effect name (an error).
            std::memcpy(copy->bytes.data() + 0xd0, b.def->bytes.data() + 0xd0, 16);
            std::memcpy(copy->bytes.data() + particle::def::effect_name, b.def->bytes.data() + particle::def::effect_name,
                        16);
            tail->views.push_back(renderer.texture_view(b.def));
            tail->defs.push_back(std::move(copy));
        }
        b.def = tail->defs[k].get();
    }
    g_tails.fetch_add(1, std::memory_order_relaxed);
    g_batches.fetch_add(tail->batches.size(), std::memory_order_relaxed);
    pool.submit([tail] { run_particle_tail(*tail); });
    return tail;
}

void run_particle_tail(ParticleTail& t) {
    const std::uint64_t t0 = now_us();
    try {
        // prepare_particle_groups' serial order: every recorded program (the first failure), then the sprites.
        std::exception_ptr program_error, sprite_error;
        for (std::size_t i = 0; i < t.batches.size(); ++i) {
            auto& b = t.batches[i];
            if (const auto r = b.deferred) {
                try {
                    if (r > t.records.size()) throw std::logic_error("prepare_particle_groups: a batch names no record");
                    particle::run_deferred_program(t.records[r - 1], b);
                } catch (...) {
                    if (!program_error) program_error = std::current_exception();
                }
            }
        }
        if (program_error) std::rethrow_exception(program_error);
        t.sprites.clear();
        t.sprites.reserve(t.batches.size());
        for (std::size_t i = 0; i < t.batches.size(); ++i) {
            std::optional<particle::ParticleSpriteBatch> s;
            try {
                s = particle::particle_sprite_batch(t.batches[i], t.cull, t.frame, t.skip_heat_haze);
            } catch (...) {
                if (!sprite_error) sprite_error = std::current_exception();
                continue;
            }
            if (sprite_error) continue;
            if (s) t.sprites.push_back(std::move(*s));
        }
        if (sprite_error) std::rethrow_exception(sprite_error);
        ParticleRenderer::build_sprites(
            t.sprites,
            [&t](const particle::ParticleDefinition* def) -> const gfx::Texture* {
                for (std::size_t k = 0; k < t.defs.size(); ++k)
                    if (t.defs[k].get() == def) return t.views[k];
                return nullptr;
            },
            t.screen_fx, t.frame_copy, t.width, t.height, t.prepared);
    } catch (...) {
        t.error = std::current_exception();
    }
    t.work_us = now_us() - t0;
    g_work_us.fetch_add(t.work_us, std::memory_order_relaxed);
}

ParticleTailProfile particle_tail_profile() {
    return {g_tails.load(), g_batches.load(), g_work_us.load(), g_wait_us.load(), g_emit_us.load()};
}

void particle_tail_count_finish(std::uint64_t wait_us, std::uint64_t emit_us) {
    g_wait_us.fetch_add(wait_us, std::memory_order_relaxed);
    g_emit_us.fetch_add(emit_us, std::memory_order_relaxed);
}

}  // namespace omega
