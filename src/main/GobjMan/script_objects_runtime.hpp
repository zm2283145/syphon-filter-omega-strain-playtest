#pragma once

#include "script.hpp"

#include <array>

namespace omega {

// Explicit diagnostic ownership, not a reproduction of mission construction.
// Handles may only be used with the registry that issued them.
class RuntimeObjectRegistry {
public:
    RuntimeObjectRegistry() = default;
    RuntimeObjectRegistry(const RuntimeObjectRegistry&) = delete;
    RuntimeObjectRegistry& operator=(const RuntimeObjectRegistry&) = delete;
    void add_scope(Word scope, std::optional<Word> parent = {});
    void set_current_scope(Word scope);
    RuntimeObjectHandle register_world_object(Word id, Word scope);
    RuntimeObjectHandle register_npc(Word id, Word scope);
    // 0040c240(&id, category) then registration: the category (low byte)
    // counter at 00571780 is pre-incremented, wrapping from 0x10000 to 0,
    // until 0040c480 finds no visible object with (category << 16) | counter.
    // The counters start at zero (0040c2d0). A full category throws (the
    // original would loop forever). The ID is object_dynamic_id.
    RuntimeObjectHandle create_world_object(Word category, Word scope);
    RuntimeObjectHandle attach(Word id, RuntimeObjectHandle::Kind kind);
    void detach(Word id);
    // The object left the traversal tree (the flush 0040c5c0 -> 0040b4b0 of a
    // 0040b720 removal): 0040c540 / 0040c480 no longer find it, so resolve
    // gives null, while handles already held stay alive until remove.
    void hide(Word id);
    void remove(Word id);
    RuntimeObjectHandle resolve(Word raw_id) const;
    // 003e04c0: the ID word for a live handle (bit 31 for the registered
    // object itself, clear for its controller); null gives 0xffffffff.
    Word id_word(RuntimeObjectHandle handle) const;
    // Retained handles remain alive across scope changes; lookup visibility is
    // deliberately distinct from lifetime. Null and stale handles are not alive.
    bool alive(RuntimeObjectHandle handle) const;
    RuntimeObjectHandle get_gobj(RuntimeObjectHandle controller) const;

private:
    struct Entry {
        Word scope{};
        bool npc{};
        std::uint64_t generation{};
        std::optional<RuntimeObjectHandle> controller;
        bool hidden{};
    };
    std::map<Word, std::optional<Word>> scopes_;
    std::optional<Word> current_scope_;
    std::map<Word, Entry> objects_;
    std::uint64_t next_generation_{1};
    std::array<Word, 256> counters_{};
    std::uint64_t generation();
    RuntimeObjectHandle insert(Word id, Word scope, bool npc);
    Entry& entry(Word id);
    bool visible(Word scope) const;
};

} // namespace omega
