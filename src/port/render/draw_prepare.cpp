#include "draw_prepare.hpp"

#include "character_model.hpp"

#include "worker_pool.hpp"

#include <algorithm>
#include <exception>
#include <optional>
#include <stdexcept>
#include <utility>

namespace omega {

DeferredDraws::DeferredDraws(particle::ParticleSystem& system, std::vector<particle::DeferredProgram>& records,
                             const WorkerPool& pool)
    : system_(system), previous_(system.deferred) {
    if (pool.workers() == 0) return;
    records.clear();
    system_.deferred = &records;
}

DeferredDraws::~DeferredDraws() { system_.deferred = previous_; }

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
    pool.for_each(items.size(), [&](std::size_t i) {
        Item& it = items[i];
        if (const auto r = it.batch->deferred) {
            try {
                if (r > records.size()) throw std::logic_error("prepare_particle_groups: a batch names no record");
                particle::run_deferred_program(records[r - 1], *it.batch);
            } catch (...) {
                it.program_error = std::current_exception();
                return;
            }
        }
        try {
            it.sprites = particle::particle_sprite_batch(*it.batch, it.group->cull, it.group->frame, skip_heat_haze);
        } catch (...) {
            it.sprite_error = std::current_exception();
        }
    });
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
        for(std::size_t k=0;k<part.mesh.vertices.size();++k){
            const auto& v=part.mesh.vertices[k];
            const Vec3 p{v.position[0],v.position[1],v.position[2]};
            mesh.vertices.push_back({p,{v.normal[0],v.normal[1],v.normal[2]},v.uv,base_color});
            if(part_gore){const auto s=(*part_gore)[k];
                mesh.vertices.back().gore={s.red/128.0F,s.green_blue/128.0F,s.green_blue/128.0F};}
            if(first){mesh.bounds={p,p};first=false;}
            else{auto& a=mesh.bounds.minimum;auto& b=mesh.bounds.maximum;
                a={std::min(a.x,p.x),std::min(a.y,p.y),std::min(a.z,p.z)};
                b={std::max(b.x,p.x),std::max(b.y,p.y),std::max(b.z,p.z)};}
        }
        for(const auto& t:part.mesh.triangles)for(auto i:t)mesh.indices.push_back(base+i);
        const auto material_index=static_cast<std::uint32_t>(mesh.materials.size());
        MeshMaterial material;material.texture=textures&&index<textures->size()?(*textures)[index]:material_index;mesh.materials.push_back(material);
        mesh.surfaces.push_back({material_index,first_index,static_cast<std::uint32_t>(mesh.indices.size())-first_index});
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

}  // namespace omega
