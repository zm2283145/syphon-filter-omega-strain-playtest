#include "draw_prepare.hpp"

#include "character_model.hpp"

#include "worker_pool.hpp"
#include "zone_profile.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>
#if defined(__vita__)
#include <psp2/kernel/processmgr.h>  // 132vg2: the particle prepare's timing (PS Vita only)
#endif

#if defined(__arm__) && defined(__ARM_NEON)
#include <arm_neon.h>
#define OMEGA_MESH_NEON 1
#else
#define OMEGA_MESH_NEON 0
#endif

namespace omega {

#if OMEGA_MESH_NEON
namespace {
bool mesh_neon_mode() {
    std::uint32_t fpscr;
    asm volatile("vmrs %0, fpscr" : "=r"(fpscr));
    return (fpscr & 0x01f70000u) == 0x01000000u;  // FZ set; RMode nearest; Stride, Len 0
}
}  // namespace
#endif

DeferredDraws::DeferredDraws(particle::ParticleSystem& system, std::vector<particle::DeferredProgram>& records,
                             const WorkerPool& pool)
    : system_(system), previous_(system.deferred) {
    if (pool.workers() == 0) return;
    records.clear();
    system_.deferred = &records;
}

DeferredDraws::~DeferredDraws() { system_.deferred = previous_; }

#if defined(OMEGA_ZONE_PROFILE)
namespace {
struct PrepareCounters {
    std::atomic<std::uint64_t> calls{0}, single_calls{0}, batches{0}, programs{0}, particles{0}, drawn{0}, program_us{0},
        sprite_us{0}, wall_us{0};
};
PrepareCounters g_prepare;
std::uint64_t prepare_now_us() { return zone::now_ns() / 1000u; }
}  // namespace
#endif

#if defined(OMEGA_ZONE_PROFILE)
namespace {
struct ActorCounters {
    std::atomic<std::uint64_t> tasks{0}, lod0{0}, lod1{0}, hidden{0}, vertices{0}, skin_us{0}, task_us{0};
    std::atomic<std::uint64_t> lod0_us{0}, lod1_us{0}, hidden_us{0};
};
ActorCounters g_actors;
std::atomic<bool> g_actor_bench_requested{false}, g_actor_bench_ready{false};
std::mutex g_actor_bench_mutex;
ActorBench g_actor_bench;
}  // namespace
#endif

ActorPiecesProfile actor_pieces_profile() {
    ActorPiecesProfile p;
#if defined(OMEGA_ZONE_PROFILE)
    p.tasks = g_actors.tasks.load();
    p.lod0 = g_actors.lod0.load();
    p.lod1 = g_actors.lod1.load();
    p.hidden = g_actors.hidden.load();
    p.vertices = g_actors.vertices.load();
    p.skin_us = g_actors.skin_us.load();
    p.task_us = g_actors.task_us.load();
    p.lod0_us = g_actors.lod0_us.load();
    p.lod1_us = g_actors.lod1_us.load();
    p.hidden_us = g_actors.hidden_us.load();
#endif
    return p;
}

void request_actor_bench() {
#if defined(OMEGA_ZONE_PROFILE)
    g_actor_bench_requested.store(true);
#endif
}
bool actor_bench_due() {
#if defined(OMEGA_ZONE_PROFILE)
    return g_actor_bench_requested.load(std::memory_order_relaxed) && g_actor_bench_requested.exchange(false);
#else
    return false;
#endif
}
void actor_bench_store(const ActorBench& bench) {
#if defined(OMEGA_ZONE_PROFILE)
    std::lock_guard lock(g_actor_bench_mutex);
    g_actor_bench = bench;
    g_actor_bench_ready.store(true);
#else
    (void)bench;
#endif
}
bool actor_bench_take(ActorBench& bench) {
#if defined(OMEGA_ZONE_PROFILE)
    if (!g_actor_bench_ready.exchange(false)) return false;
    std::lock_guard lock(g_actor_bench_mutex);
    bench = g_actor_bench;
    return true;
#else
    (void)bench;
    return false;
#endif
}

void actor_pieces_count(int lod, std::size_t vertices, std::uint64_t skin_us, std::uint64_t task_us) {
#if defined(OMEGA_ZONE_PROFILE)
    g_actors.tasks.fetch_add(1, std::memory_order_relaxed);
    (lod == 0 ? g_actors.lod0 : lod > 0 ? g_actors.lod1 : g_actors.hidden).fetch_add(1, std::memory_order_relaxed);
    g_actors.vertices.fetch_add(vertices, std::memory_order_relaxed);
    g_actors.skin_us.fetch_add(skin_us, std::memory_order_relaxed);
    g_actors.task_us.fetch_add(task_us, std::memory_order_relaxed);
    (lod == 0 ? g_actors.lod0_us : lod > 0 ? g_actors.lod1_us : g_actors.hidden_us).fetch_add(task_us, std::memory_order_relaxed);
#else
    (void)lod, (void)vertices, (void)skin_us, (void)task_us;
#endif
}

std::uint64_t prepare_profile_clock_us() {
#if defined(OMEGA_ZONE_PROFILE)
    return prepare_now_us();
#else
    return 0;
#endif
}

#if defined(OMEGA_ZONE_PROFILE)
namespace {
std::mutex g_bench_mutex;
std::atomic<bool> g_bench_kept{false};
particle::DeferredProgram g_bench_record;
void bench_keep(const particle::DeferredProgram& p) {
    if (g_bench_kept.load(std::memory_order_relaxed) || (p.block[0xb][0] & 0x7fffu) < 16) return;
    std::lock_guard lock(g_bench_mutex);
    if (g_bench_kept.load()) return;
    g_bench_record = p;
    g_bench_kept.store(true);
}
}  // namespace
#endif

bool particle_bench(unsigned runs, double (&us_per_particle)[3], double (&us_one_batch)[3], unsigned& particles) {
#if defined(OMEGA_ZONE_PROFILE)
    if (!g_bench_kept.load()) return false;
    particle::DeferredProgram record;
    {
        std::lock_guard lock(g_bench_mutex);
        record = g_bench_record;
    }
    particles = record.block[0xb][0] & 0x7fffu;
    particle::DeferredProgram one = record;  // hw5: the same batch cut to its first particle
    one.block[0xb][0] = (one.block[0xb][0] & ~0x7fffu) | 1u;
    const bool speculation = particle::particle_neon_speculation();
    auto state = std::make_unique<particle::Vu1State>();
    const auto time = [&](const particle::DeferredProgram& program) {
        std::uint64_t total = 0;
        for (unsigned k = 0; k < runs; ++k) {
            auto& s = *state;
            s.vf = {};
            s.vf[0] = {0, 0, 0, 0x3f800000u};
            s.acc = {};
            s.i = s.q = s.r = 0;
            s.vi = {};
            particle::ParticleBatch batch;
            const std::uint64_t t0 = prepare_now_us();
            try {
                particle::run_deferred_program(program, batch, &s);
            } catch (...) {
            }
            total += prepare_now_us() - t0;
        }
        return runs ? double(total) / double(runs) : 0.0;
    };
    for (int mode = 0; mode < 3; ++mode) {
        particle::set_particle_neon_speculation(mode != 0);
        particle::set_particle_neon_registers(mode == 2);
        us_per_particle[mode] = particles ? time(record) / double(particles) : 0.0;
        us_one_batch[mode] = time(one);
    }
    particle::set_particle_neon_speculation(speculation);
    particle::set_particle_neon_registers(true);
    return true;
#else
    (void)runs, (void)us_per_particle, (void)us_one_batch, (void)particles;
    return false;
#endif
}

ParticlePrepareProfile particle_prepare_profile() {
    ParticlePrepareProfile p;
#if defined(OMEGA_ZONE_PROFILE)
    p.calls = g_prepare.calls.load();
    p.single_calls = g_prepare.single_calls.load();
    p.batches = g_prepare.batches.load();
    p.programs = g_prepare.programs.load();
    p.particles = g_prepare.particles.load();
    p.drawn = g_prepare.drawn.load();
    p.program_us = g_prepare.program_us.load();
    p.sprite_us = g_prepare.sprite_us.load();
    p.wall_us = g_prepare.wall_us.load();
#endif
    return p;
}

void prepare_particle_groups(WorkerPool& pool, std::vector<ParticleGroup>& groups,
                             const std::vector<particle::DeferredProgram>& records, bool skip_heat_haze) {
    struct Item {
        ParticleGroup* group;
        particle::ParticleBatch* batch;
        std::optional<particle::ParticleSpriteBatch> sprites;
        std::exception_ptr program_error, sprite_error;
    };
    if (pool.workers() == 0) {
        bool recorded = false;
        for (const auto& g : groups)
            for (const auto& b : *g.batches) recorded |= b.deferred != 0;
        if (!recorded) {  // the old path: each group's particle_sprites in order
            for (auto& g : groups) g.sprites = particle::particle_sprites(*g.batches, g.cull, g.frame, skip_heat_haze);
            return;
        }
    }
    std::vector<Item> items;
    for (auto& g : groups) {
        g.sprites.clear();
        for (auto& b : *g.batches) items.push_back({&g, &b, std::nullopt, nullptr, nullptr});
    }
#if defined(OMEGA_ZONE_PROFILE)
    const std::uint64_t wall0 = prepare_now_us();
#endif
#if defined(__vita__)
    const std::uint64_t vg_wall0 = sceKernelGetProcessTimeWide();  // 132vg2
#endif
    pool.for_each(items.size(), [&](std::size_t i) {
        Item& it = items[i];
#if defined(__vita__)
        std::uint64_t vg_t0 = sceKernelGetProcessTimeWide();  // 132vg2
#endif
#if defined(OMEGA_ZONE_PROFILE)
        const std::uint64_t t0 = prepare_now_us();
#endif
        if (const auto r = it.batch->deferred) {
            try {
                if (r > records.size()) throw std::logic_error("prepare_particle_groups: a batch names no record");
                particle::run_deferred_program(records[r - 1], *it.batch);
            } catch (...) {
                it.program_error = std::current_exception();
                return;
            }
#if defined(__vita__)
            const std::uint64_t vg_t1 = sceKernelGetProcessTimeWide();
            g_particle_prepare_times.program_us += vg_t1 - vg_t0;
            ++g_particle_prepare_times.programs;
            vg_t0 = vg_t1;
#endif
#if defined(OMEGA_ZONE_PROFILE)
            g_prepare.programs.fetch_add(1, std::memory_order_relaxed);
            bench_keep(records[r - 1]);
#endif
        }
#if defined(OMEGA_ZONE_PROFILE)
        const std::uint64_t t1 = prepare_now_us();
        g_prepare.program_us.fetch_add(t1 - t0, std::memory_order_relaxed);
        g_prepare.particles.fetch_add(it.batch->position.size(), std::memory_order_relaxed);
#endif
        try {
            it.sprites = particle::particle_sprite_batch(*it.batch, it.group->cull, it.group->frame, skip_heat_haze);
        } catch (...) {
            it.sprite_error = std::current_exception();
        }
#if defined(__vita__)
        g_particle_prepare_times.sprite_us += sceKernelGetProcessTimeWide() - vg_t0;
#endif
#if defined(OMEGA_ZONE_PROFILE)
        g_prepare.sprite_us.fetch_add(prepare_now_us() - t1, std::memory_order_relaxed);
        if (it.sprites) g_prepare.drawn.fetch_add(1, std::memory_order_relaxed);
#endif
    });
#if defined(__vita__)
    g_particle_prepare_times.wall_us += sceKernelGetProcessTimeWide() - vg_wall0;
    g_particle_prepare_times.batches += items.size();
    ++g_particle_prepare_times.calls;
#endif
#if defined(OMEGA_ZONE_PROFILE)
    if (!items.empty()) {
        g_prepare.wall_us.fetch_add(prepare_now_us() - wall0, std::memory_order_relaxed);
        g_prepare.calls.fetch_add(1, std::memory_order_relaxed);
        g_prepare.single_calls.fetch_add(items.size() == 1, std::memory_order_relaxed);
        g_prepare.batches.fetch_add(items.size(), std::memory_order_relaxed);
    }
#endif
    // The serial order: every program ran in the draw (the first failure there), then each group's sprites.
    for (const auto& it : items)
        if (it.program_error) std::rethrow_exception(it.program_error);
    for (auto& it : items) {
        if (it.sprite_error) std::rethrow_exception(it.sprite_error);
        if (it.sprites) it.group->sprites.push_back(std::move(*it.sprites));
    }
}

std::vector<particle::ParticleSpriteBatch> prepare_particle_sprites(WorkerPool& pool,
                                                                    std::vector<particle::ParticleBatch>& batches,
                                                                    const std::vector<particle::DeferredProgram>& records,
                                                                    const std::array<particle::Qword, 4>& cull,
                                                                    const particle::SpriteFrame& frame,
                                                                    bool skip_heat_haze) {
    if (pool.workers() == 0) {
        bool recorded = false;
        for (const auto& b : batches) recorded |= b.deferred != 0;
        if (!recorded) return particle::particle_sprites(batches, cull, frame, skip_heat_haze);  // the old path
    }
    std::vector<ParticleGroup> groups(1);
    groups[0].batches = &batches;
    groups[0].cull = cull;
    groups[0].frame = frame;
    prepare_particle_groups(pool, groups, records, skip_heat_haze);
    return std::move(groups[0].sprites);
}

// Moved from play_main.cpp unchanged (checkpoint 132mt3).
void character_mesh(Mesh& mesh,const std::vector<AnimatedCharacterGeometry>& parts,const std::vector<bool>& textured_parts,bool append,bool visible,
                    const std::vector<std::uint32_t>* textures,const std::vector<std::vector<CharacterGoreSubtract>>* gore){
    if(!append){mesh.vertices.clear();mesh.indices.clear();mesh.surfaces.clear();mesh.materials.clear();}
    bool first=mesh.vertices.empty();
    for(std::size_t index=0;index<parts.size();++index){
        // An actor that is not drawn keeps its material slots: the overlay images are indexed by material.
        if(!visible){MeshMaterial hidden_material;hidden_material.texture=static_cast<std::uint32_t>(mesh.materials.size());mesh.materials.push_back(hidden_material);continue;}
        const auto& part=parts[index];
        const bool textured=index<textured_parts.size()&&textured_parts[index];
        const std::array<float,4> base_color=textured?std::array<float,4>{1,1,1,1}:std::array<float,4>{.85f,.62f,.38f,1};
        const auto first_index=static_cast<std::uint32_t>(mesh.indices.size());
        const auto base=static_cast<std::uint32_t>(mesh.vertices.size());
        const std::vector<CharacterGoreSubtract>* part_gore=gore&&index<gore->size()&&!(*gore)[index].empty()?&(*gore)[index]:nullptr;
        if(part_gore&&part_gore->size()!=part.mesh.vertices.size())throw std::runtime_error("Character gore: subtracts differ from the part's vertices");
#if OMEGA_MESH_NEON
        // 132vc1 hw6 (port-side speed only; the same bounds): on the PS Vita the running bounds stay in NEON registers
        // (std::min(a, p) is p < a ? p : a and std::max(b, p) is b < p ? p : b, lane by lane, vertex by vertex), not
        // a VFP compare and status transfer per coordinate. Skinned positions are never denormal (flush-to-zero).
        const bool neon_bounds=mesh_neon_mode()&&!part.mesh.vertices.empty();
        float32x4_t lo=vdupq_n_f32(0),hi=vdupq_n_f32(0);
        if(neon_bounds&&!first){
            lo=float32x4_t{mesh.bounds.minimum.x,mesh.bounds.minimum.y,mesh.bounds.minimum.z,0};
            hi=float32x4_t{mesh.bounds.maximum.x,mesh.bounds.maximum.y,mesh.bounds.maximum.z,0};
        }
#endif
        for(std::size_t k=0;k<part.mesh.vertices.size();++k){
            const auto& v=part.mesh.vertices[k];
            const Vec3 p{v.position[0],v.position[1],v.position[2]};
            mesh.vertices.push_back({p,{v.normal[0],v.normal[1],v.normal[2]},v.uv,base_color});
            if(part_gore){const auto s=(*part_gore)[k];
                mesh.vertices.back().gore={s.red/128.0F,s.green_blue/128.0F,s.green_blue/128.0F};}
#if OMEGA_MESH_NEON
            if(neon_bounds){
                const float32x4_t q=vld1q_f32(v.position.data());
                if(first){lo=hi=q;first=false;}
                else{lo=vbslq_f32(vcltq_f32(q,lo),q,lo);hi=vbslq_f32(vcltq_f32(hi,q),q,hi);}
                continue;
            }
#endif
            if(first){mesh.bounds={p,p};first=false;}
            else{auto& a=mesh.bounds.minimum;auto& b=mesh.bounds.maximum;
                a={std::min(a.x,p.x),std::min(a.y,p.y),std::min(a.z,p.z)};
                b={std::max(b.x,p.x),std::max(b.y,p.y),std::max(b.z,p.z)};}
        }
#if OMEGA_MESH_NEON
        if(neon_bounds){
            mesh.bounds.minimum={vgetq_lane_f32(lo,0),vgetq_lane_f32(lo,1),vgetq_lane_f32(lo,2)};
            mesh.bounds.maximum={vgetq_lane_f32(hi,0),vgetq_lane_f32(hi,1),vgetq_lane_f32(hi,2)};
        }
#endif
        for(const auto& t:part.mesh.triangles)for(auto i:t)mesh.indices.push_back(base+i);
        const auto material_index=static_cast<std::uint32_t>(mesh.materials.size());
        MeshMaterial material;material.texture=textures&&index<textures->size()?(*textures)[index]:material_index;mesh.materials.push_back(material);
        mesh.surfaces.push_back({material_index,first_index,static_cast<std::uint32_t>(mesh.indices.size())-first_index});
    }
}

void hidden_actor_materials(Mesh& mesh, std::size_t part_count, bool append) {
    if (!append) {
        mesh.vertices.clear();
        mesh.indices.clear();
        mesh.surfaces.clear();
        mesh.materials.clear();
    }
    for (std::size_t index = 0; index < part_count; ++index) {
        MeshMaterial hidden_material;
        hidden_material.texture = static_cast<std::uint32_t>(mesh.materials.size());
        mesh.materials.push_back(hidden_material);
    }
}

namespace {

// Vertices (or indices) per task of the parallel variants.
constexpr std::size_t mesh_chunk = 16384;

struct ChunkBounds {
    Vec3 minimum, maximum;
    bool any = false;
};

// The chunks' bounds into the mesh's, in order (`first`: the mesh had no vertex).
void merge_bounds(Mesh& mesh, bool& first, const std::vector<ChunkBounds>& chunks) {
    for (const auto& b : chunks) {
        if (!b.any) continue;
        if (first) {
            mesh.bounds = {b.minimum, b.maximum};
            first = false;
            continue;
        }
        auto& a = mesh.bounds.minimum;
        auto& c = mesh.bounds.maximum;
        a = {std::min(a.x, b.minimum.x), std::min(a.y, b.minimum.y), std::min(a.z, b.minimum.z)};
        c = {std::max(c.x, b.maximum.x), std::max(c.y, b.maximum.y), std::max(c.z, b.maximum.z)};
    }
}

// fn(begin, end, chunk) over [0, n) in mesh_chunk pieces: one piece inline, more on the pool.
template <class Fn> void chunks_of(WorkerPool& pool, std::size_t n, std::size_t count, Fn&& fn) {
    if (count <= 1 || pool.workers() == 0) {
        for (std::size_t c = 0; c < count; ++c) fn(c * mesh_chunk, std::min(n, (c + 1) * mesh_chunk), c);
        return;
    }
    pool.for_each(count, [&](std::size_t c) { fn(c * mesh_chunk, std::min(n, (c + 1) * mesh_chunk), c); });
}

}  // namespace

void character_mesh(WorkerPool& pool,Mesh& mesh,const std::vector<AnimatedCharacterGeometry>& parts,const std::vector<bool>& textured_parts,bool append,bool visible,
                    const std::vector<std::uint32_t>* textures,const std::vector<std::vector<CharacterGoreSubtract>>* gore,
                    const std::vector<PartSource>* sources){
    const auto source_of=[&](std::size_t index)->const PartSource*{
        return sources&&index<sources->size()&&((*sources)[index].fill||(*sources)[index].skin)?&(*sources)[index]:nullptr;};
    // The vectors are sized once for every part: growing a vector value-initialises its new elements serially
    // (MeshVertex is 60 bytes); without append, a mesh kept from the last frame is only truncated (all output is
    // overwritten below), so a steady replacement draw grows nothing.
    std::size_t vertex_total=append?mesh.vertices.size():0,index_total=append?mesh.indices.size():0;
    if(visible)for(std::size_t index=0;index<parts.size();++index){
        const PartSource* source=source_of(index);
        if(source&&source->skin)continue;  // 132md8: drawn from its model's GPU buffers
        vertex_total+=source?source->count:parts[index].mesh.vertices.size();
        index_total+=3*(source?source->triangles.size():parts[index].mesh.triangles.size());
    }
    if(!append){mesh.surfaces.clear();mesh.materials.clear();}
    std::size_t vertex_cursor=append?mesh.vertices.size():0,index_cursor=append?mesh.indices.size():0;
    bool first=vertex_cursor==0&&!(append&&has_skin_surface(mesh));
    mesh.vertices.resize(vertex_total);
    mesh.indices.resize(index_total);
    std::vector<ChunkBounds> bounds;
    for(std::size_t index=0;index<parts.size();++index){
        if(!visible){MeshMaterial hidden_material;hidden_material.texture=static_cast<std::uint32_t>(mesh.materials.size());mesh.materials.push_back(hidden_material);continue;}
        const auto& part=parts[index];
        const bool textured=index<textured_parts.size()&&textured_parts[index];
        const std::array<float,4> base_color=textured?std::array<float,4>{1,1,1,1}:std::array<float,4>{.85f,.62f,.38f,1};
        const auto first_index=static_cast<std::uint32_t>(index_cursor);
        const auto base=static_cast<std::uint32_t>(vertex_cursor);
        const PartSource* source=source_of(index);
        if(source&&source->skin){
            if(source->bounded)
                merge_bounds(mesh,first,{ChunkBounds{{source->lo[0],source->lo[1],source->lo[2]},{source->hi[0],source->hi[1],source->hi[2]},true}});
            const auto material_index=static_cast<std::uint32_t>(mesh.materials.size());
            MeshMaterial material;material.texture=textures&&index<textures->size()?(*textures)[index]:material_index;mesh.materials.push_back(material);
            mesh.surfaces.push_back({material_index,0,static_cast<std::uint32_t>(3*source->triangles.size()),source->skin});
            continue;
        }
        const std::span<const std::array<std::uint32_t,3>> triangles=source?source->triangles:std::span<const std::array<std::uint32_t,3>>(part.mesh.triangles);
        const std::size_t nv=source?source->count:part.mesh.vertices.size(),nt=triangles.size();
        const std::vector<CharacterGoreSubtract>* part_gore=gore&&index<gore->size()&&!(*gore)[index].empty()?&(*gore)[index]:nullptr;
        if(part_gore&&part_gore->size()!=nv)throw std::runtime_error("Character gore: subtracts differ from the part's vertices");
        vertex_cursor+=nv;index_cursor+=3*nt;
        const std::size_t vertex_chunks=(nv+mesh_chunk-1)/mesh_chunk,triangle_chunks=(nt+mesh_chunk-1)/mesh_chunk;
        bounds.assign(vertex_chunks,{});
        MeshVertex* out=mesh.vertices.data()+base;
        std::uint32_t* indices=mesh.indices.data()+first_index;
        chunks_of(pool,std::max(nv,nt),std::max(vertex_chunks,triangle_chunks),[&](std::size_t,std::size_t,std::size_t c){
            const std::size_t k0=c*mesh_chunk,k1=std::min(nv,(c+1)*mesh_chunk);
            if(k0<k1&&source){
                source->fill(k0,k1,out+k0);
                Vec3 lo=out[k0].position,hi=lo;
                for(std::size_t k=k0;k<k1;++k){
                    auto& v=out[k];
                    v.color=base_color;
                    v.gore={};
                    if(part_gore){const auto s=(*part_gore)[k];v.gore={s.red/128.0F,s.green_blue/128.0F,s.green_blue/128.0F};}
                    const Vec3 p=v.position;
                    lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
                    hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
                }
                bounds[c]={lo,hi,true};
            }else if(k0<k1){
                Vec3 lo{part.mesh.vertices[k0].position[0],part.mesh.vertices[k0].position[1],part.mesh.vertices[k0].position[2]},hi=lo;
                for(std::size_t k=k0;k<k1;++k){
                    const auto& v=part.mesh.vertices[k];
                    const Vec3 p{v.position[0],v.position[1],v.position[2]};
                    out[k]={p,{v.normal[0],v.normal[1],v.normal[2]},v.uv,base_color};
                    if(part_gore){const auto s=(*part_gore)[k];
                        out[k].gore={s.red/128.0F,s.green_blue/128.0F,s.green_blue/128.0F};}
                    lo={std::min(lo.x,p.x),std::min(lo.y,p.y),std::min(lo.z,p.z)};
                    hi={std::max(hi.x,p.x),std::max(hi.y,p.y),std::max(hi.z,p.z)};
                }
                bounds[c]={lo,hi,true};
            }
            for(std::size_t t=c*mesh_chunk,end=std::min(nt,(c+1)*mesh_chunk);t<end;++t)
                for(std::size_t j=0;j<3;++j)indices[3*t+j]=base+triangles[t][j];
        });
        merge_bounds(mesh,first,bounds);
        const auto material_index=static_cast<std::uint32_t>(mesh.materials.size());
        MeshMaterial material;material.texture=textures&&index<textures->size()?(*textures)[index]:material_index;mesh.materials.push_back(material);
        mesh.surfaces.push_back({material_index,first_index,static_cast<std::uint32_t>(3*nt)});
    }
}

bool has_skin_surface(const Mesh& mesh) {
    return std::any_of(mesh.surfaces.begin(), mesh.surfaces.end(), [](const MeshSurface& s) { return s.skin != 0; });
}

PartSource replacement_source(const gfx::ReplacementDraw& draw) {
    PartSource s;
    s.count = draw.model->vertices.size();
    s.triangles = draw.model->triangles;
    s.fill = [&draw](std::size_t begin, std::size_t end, MeshVertex* out) {
        for (std::size_t k = begin; k < end; ++k) {
            std::array<float, 3> p, q;
            gfx::skin_replacement_vertex(*draw.model, draw.palette, draw.offsets, k, p, q);
            auto& v = out[k - begin];
            v.position = {p[0], p[1], p[2]};
            v.normal = {q[0], q[1], q[2]};
            v.uv = draw.model->vertices[k].uv;
        }
    };
    return s;
}

void append_actor_piece(WorkerPool& pool, Mesh& mesh, const Mesh& piece, std::size_t listed_textures) {
    const auto vertex_base = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto index_base = static_cast<std::uint32_t>(mesh.indices.size());
    const auto material_base = static_cast<std::uint32_t>(mesh.materials.size());
    bool first = mesh.vertices.empty() && !has_skin_surface(mesh);
    const std::size_t nv = piece.vertices.size(), ni = piece.indices.size();
    mesh.vertices.resize(vertex_base + nv);
    mesh.indices.resize(index_base + ni);
    const std::size_t vertex_chunks = (nv + mesh_chunk - 1) / mesh_chunk, index_chunks = (ni + mesh_chunk - 1) / mesh_chunk;
    MeshVertex* out = mesh.vertices.data() + vertex_base;
    std::uint32_t* indices = mesh.indices.data() + index_base;
    chunks_of(pool, std::max(nv, ni), std::max(vertex_chunks, index_chunks), [&](std::size_t, std::size_t, std::size_t c) {
        const std::size_t k0 = c * mesh_chunk, k1 = std::min(nv, (c + 1) * mesh_chunk);
        if (k0 < k1) std::copy(piece.vertices.begin() + static_cast<std::ptrdiff_t>(k0),
                               piece.vertices.begin() + static_cast<std::ptrdiff_t>(k1), out + k0);
        for (std::size_t k = c * mesh_chunk, end = std::min(ni, (c + 1) * mesh_chunk); k < end; ++k)
            indices[k] = vertex_base + piece.indices[k];
    });
    // The piece's bounds are its vertices' (character_mesh widened them in order): merged as one chunk.
    // 132md8: a GPU-skinned surface's bounds are in the piece's too.
    if (nv || has_skin_surface(piece)) merge_bounds(mesh, first, {ChunkBounds{piece.bounds.minimum, piece.bounds.maximum, true}});
    for (std::size_t j = 0; j < piece.materials.size(); ++j) {
        auto m = piece.materials[j];
        if (j >= listed_textures) m.texture += material_base;
        mesh.materials.push_back(std::move(m));
    }
    for (auto s : piece.surfaces) {
        s.material += material_base;
        if (!s.skin) s.first_index += index_base;  // 132md8: a skin's indices are its model's
        mesh.surfaces.push_back(s);
    }
}

void append_actor_piece(Mesh& mesh, const Mesh& piece, std::size_t listed_textures) {
    const auto vertex_base = static_cast<std::uint32_t>(mesh.vertices.size());
    const auto index_base = static_cast<std::uint32_t>(mesh.indices.size());
    const auto material_base = static_cast<std::uint32_t>(mesh.materials.size());
    bool first = mesh.vertices.empty() && !has_skin_surface(mesh);
    // 132md8: a piece with GPU-skinned surfaces brings its bounds whole (its CPU vertices are in them).
    if (has_skin_surface(piece)) {
        if (first) mesh.bounds = piece.bounds;
        else {
            auto& a = mesh.bounds.minimum;
            auto& b = mesh.bounds.maximum;
            const auto& c = piece.bounds.minimum;
            const auto& d = piece.bounds.maximum;
            a = {std::min(a.x, c.x), std::min(a.y, c.y), std::min(a.z, c.z)};
            b = {std::max(b.x, d.x), std::max(b.y, d.y), std::max(b.z, d.z)};
        }
        first = false;
    }
    for (const auto& v : piece.vertices) {
        const Vec3 p = v.position;
        if (first) {
            mesh.bounds = {p, p};
            first = false;
        } else {
            auto& a = mesh.bounds.minimum;
            auto& b = mesh.bounds.maximum;
            a = {std::min(a.x, p.x), std::min(a.y, p.y), std::min(a.z, p.z)};
            b = {std::max(b.x, p.x), std::max(b.y, p.y), std::max(b.z, p.z)};
        }
    }
    mesh.vertices.insert(mesh.vertices.end(), piece.vertices.begin(), piece.vertices.end());
    mesh.indices.reserve(mesh.indices.size() + piece.indices.size());
    for (const auto i : piece.indices) mesh.indices.push_back(vertex_base + i);
    for (std::size_t j = 0; j < piece.materials.size(); ++j) {
        auto m = piece.materials[j];
        if (j >= listed_textures) m.texture += material_base;
        mesh.materials.push_back(std::move(m));
    }
    for (auto s : piece.surfaces) {
        s.material += material_base;
        if (!s.skin) s.first_index += index_base;  // 132md8: a skin's indices are its model's
        mesh.surfaces.push_back(s);
    }
}

// 132vc1 (port-only): append_actor_pieces, draw_prepare.hpp.
void append_actor_pieces(WorkerPool& pool, Mesh& mesh, std::span<const Mesh* const> pieces,
                         std::span<const std::size_t> listed_textures,
                         const std::function<void(std::size_t, std::size_t, std::size_t, std::size_t)>& after_piece) {
    const std::size_t n = pieces.size();
    if (listed_textures.size() != n) throw std::logic_error("append_actor_pieces: one texture count per piece");
    bool skin = false;
    std::size_t vertex_total = 0, index_total = 0;
    for (const Mesh* piece : pieces) {
        skin = skin || has_skin_surface(*piece);
        vertex_total += piece->vertices.size();
        index_total += piece->indices.size();
    }
    mesh.surfaces.clear();
    mesh.materials.clear();
    if (skin) {  // 132md8's GPU-skinned surfaces: the one-by-one appends
        mesh.vertices.clear();
        mesh.indices.clear();
        for (std::size_t k = 0; k < n; ++k) {
            const std::size_t v0 = mesh.vertices.size(), s0 = mesh.surfaces.size();
            append_actor_piece(mesh, *pieces[k], listed_textures[k]);
            after_piece(k, v0, mesh.vertices.size(), s0);
        }
        return;
    }
    // Sized once (a mesh kept from the last frame is only truncated: every element is written below).
    mesh.vertices.resize(vertex_total);
    mesh.indices.resize(index_total);
    std::vector<std::size_t> vertex_base(n), index_base(n);
    std::size_t vertex_cursor = 0, index_cursor = 0;
    for (std::size_t k = 0; k < n; ++k) {
        const Mesh& piece = *pieces[k];
        vertex_base[k] = vertex_cursor;
        index_base[k] = index_cursor;
        const auto material_base = static_cast<std::uint32_t>(mesh.materials.size());
        const std::size_t s0 = mesh.surfaces.size();
        for (std::size_t j = 0; j < piece.materials.size(); ++j) {
            auto m = piece.materials[j];
            if (j >= listed_textures[k]) m.texture += material_base;
            mesh.materials.push_back(std::move(m));
        }
        for (auto s : piece.surfaces) {
            s.material += material_base;
            s.first_index += static_cast<std::uint32_t>(index_cursor);
            mesh.surfaces.push_back(s);
        }
        vertex_cursor += piece.vertices.size();
        index_cursor += piece.indices.size();
        after_piece(k, vertex_base[k], vertex_cursor, s0);
    }
    // The vertices and offset indices on the pool, each piece into its own range; a piece with a NaN position is
    // noted (its bounds are then widened vertex by vertex below, as append_actor_piece does).
    std::vector<std::uint8_t> nan(n, 0);
    pool.for_each(n, [&](std::size_t k) {
        const Mesh& piece = *pieces[k];
        MeshVertex* out = mesh.vertices.data() + vertex_base[k];
        bool any_nan = false;
        for (std::size_t i = 0; i < piece.vertices.size(); ++i) {
            out[i] = piece.vertices[i];
            const Vec3& p = piece.vertices[i].position;
            any_nan = any_nan || p.x != p.x || p.y != p.y || p.z != p.z;
        }
        nan[k] = any_nan;
        std::uint32_t* indices = mesh.indices.data() + index_base[k];
        const auto base = static_cast<std::uint32_t>(vertex_base[k]);
        for (std::size_t i = 0; i < piece.indices.size(); ++i) indices[i] = base + piece.indices[i];
    });
    // The bounds as the appends widen them vertex by vertex: with no NaN, the running std::min / std::max over a
    // piece's vertices from the mesh's bounds equal those of the mesh's bounds and the piece's own (character_mesh
    // widened them over the same vertices in the same order, the first of equal values kept at every step).
    bool first = true;
    for (std::size_t k = 0; k < n; ++k) {
        const Mesh& piece = *pieces[k];
        if (piece.vertices.empty()) continue;
        auto& a = mesh.bounds.minimum;
        auto& b = mesh.bounds.maximum;
        if (nan[k]) {
            for (const auto& v : piece.vertices) {
                const Vec3 p = v.position;
                if (first) {
                    mesh.bounds = {p, p};
                    first = false;
                } else {
                    a = {std::min(a.x, p.x), std::min(a.y, p.y), std::min(a.z, p.z)};
                    b = {std::max(b.x, p.x), std::max(b.y, p.y), std::max(b.z, p.z)};
                }
            }
            continue;
        }
        const Vec3 c = piece.bounds.minimum, d = piece.bounds.maximum;
        if (first) {
            mesh.bounds = {c, d};
            first = false;
        } else {
            a = {std::min(a.x, c.x), std::min(a.y, c.y), std::min(a.z, c.z)};
            b = {std::max(b.x, d.x), std::max(b.y, d.y), std::max(b.z, d.z)};
        }
    }
}

}  // namespace omega

