#include "script_objects_runtime.hpp"
#include "object_dynamic_id.hpp"

#include <limits>
#include <stdexcept>

namespace omega {
namespace {
void valid_id(Word id) {
    if ((id & 0x80000000U) != 0)
        throw std::runtime_error("Runtime registration ID contains projection bit");
}
}

void RuntimeObjectRegistry::add_scope(Word scope, std::optional<Word> parent) {
    if (scopes_.contains(scope)) throw std::runtime_error("Duplicate runtime scope");
    if (parent && !scopes_.contains(*parent)) throw std::runtime_error("Unknown runtime parent scope");
    // Parents must already exist and never change, so cycles cannot be formed.
    scopes_.emplace(scope, parent);
}

void RuntimeObjectRegistry::set_current_scope(Word scope) {
    if (!scopes_.contains(scope)) throw std::runtime_error("Unknown current runtime scope");
    current_scope_ = scope;
}

std::uint64_t RuntimeObjectRegistry::generation() {
    if (next_generation_ == std::numeric_limits<std::uint64_t>::max())
        throw std::runtime_error("Runtime identity generation exhausted");
    return next_generation_++;
}

RuntimeObjectHandle RuntimeObjectRegistry::insert(Word id, Word scope, bool npc) {
    valid_id(id);
    if (!scopes_.contains(scope)) throw std::runtime_error("Unknown object registration scope");
    if (objects_.contains(id)) throw std::runtime_error("Duplicate runtime object ID");
    const auto token = generation();
    objects_.emplace(id, Entry{scope, npc, token, {}});
    return {RuntimeObjectHandle::Kind::world_object, id, token};
}

RuntimeObjectHandle RuntimeObjectRegistry::register_world_object(Word id, Word scope) {
    return insert(id, scope, false);
}

RuntimeObjectHandle RuntimeObjectRegistry::register_npc(Word id, Word scope) {
    return insert(id, scope, true);
}

RuntimeObjectHandle RuntimeObjectRegistry::create_world_object(Word category, Word scope) {
    // 0040c480 -> 0040c080: the same visible lookup as 0040c540.
    const Word id = object_dynamic_id(counters_, category, [this](Word key) { return resolve(key).generation != 0; });
    return insert(id, scope, false);
}

RuntimeObjectRegistry::Entry& RuntimeObjectRegistry::entry(Word id) {
    valid_id(id);
    const auto found = objects_.find(id);
    if (found == objects_.end()) throw std::runtime_error("Unknown runtime object ID");
    return found->second;
}

RuntimeObjectHandle RuntimeObjectRegistry::attach(Word id, RuntimeObjectHandle::Kind kind) {
    auto& owner = entry(id);
    if (!owner.npc) throw std::runtime_error("Only admitted NPCs support controller projection");
    if (kind != RuntimeObjectHandle::Kind::ai && kind != RuntimeObjectHandle::Kind::player)
        throw std::runtime_error("Invalid runtime controller kind");
    const RuntimeObjectHandle controller{kind, id, generation()};
    owner.controller = controller;
    return controller;
}

void RuntimeObjectRegistry::detach(Word id) {
    auto& owner = entry(id);
    if (!owner.npc) throw std::runtime_error("Only admitted NPCs have controller attachments");
    owner.controller.reset();
}

void RuntimeObjectRegistry::hide(Word id) {
    auto& owner = entry(id);
    if (owner.hidden) throw std::runtime_error("Runtime object already out of the traversal tree");
    owner.hidden = true;
}

void RuntimeObjectRegistry::remove(Word id) {
    entry(id); // Unknown/remasked IDs are errors, not silent lifecycle changes.
    objects_.erase(id);
}

bool RuntimeObjectRegistry::visible(Word scope) const {
    if (!current_scope_) throw std::runtime_error("Runtime lookup has no current scope");
    for (std::optional<Word> cursor = scope; cursor; cursor = scopes_.at(*cursor))
        if (*cursor == *current_scope_) return true;
    return false;
}

RuntimeObjectHandle RuntimeObjectRegistry::resolve(Word raw_id) const {
    if (!current_scope_) throw std::runtime_error("Runtime lookup has no current scope");
    const Word id = raw_id & 0x7fffffffU;
    const auto found = objects_.find(id);
    if (found == objects_.end() || found->second.hidden || !visible(found->second.scope)) return {};
    const auto& owner = found->second;
    if ((raw_id & 0x80000000U) == 0 && owner.controller) return *owner.controller;
    return {RuntimeObjectHandle::Kind::world_object, id, owner.generation};
}

Word RuntimeObjectRegistry::id_word(RuntimeObjectHandle handle) const {
    if (handle.generation == 0) return 0xffffffffU;
    // The original holds raw pointers; a stale handle has no defined ID.
    if (!alive(handle)) throw std::runtime_error("ID of a stale runtime object");
    return handle.kind == RuntimeObjectHandle::Kind::world_object ? handle.id | 0x80000000U : handle.id;
}

bool RuntimeObjectRegistry::alive(RuntimeObjectHandle handle) const {
    if (handle.generation == 0) return false;
    const auto found = objects_.find(handle.id);
    if (found == objects_.end()) return false;
    if (handle.kind == RuntimeObjectHandle::Kind::world_object)
        return handle.generation == found->second.generation;
    return found->second.controller && handle == *found->second.controller;
}

RuntimeObjectHandle RuntimeObjectRegistry::get_gobj(RuntimeObjectHandle controller) const {
    if ((controller.kind != RuntimeObjectHandle::Kind::ai && controller.kind != RuntimeObjectHandle::Kind::player)
        || !alive(controller)) throw std::runtime_error("GetGOBJ requires a live controller owner");
    const auto& owner = objects_.at(controller.id);
    return {RuntimeObjectHandle::Kind::world_object, controller.id, owner.generation};
}

} // namespace omega
