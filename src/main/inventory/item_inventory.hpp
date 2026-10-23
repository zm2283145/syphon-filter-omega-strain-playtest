#pragma once
// Mission items (ITEM_INVENTORY.md): the global item table game::RegisterItem
// fills (vector 0x587ff0, inventory.cc) and an actor's +0x3528 item list of
// {item index, count} pairs. The row's +0x28 resource handle is loaded by
// 004703b0 (level load, through the weapon model library) and freed by
// 004702a0 (teardown); pickups, HUD display and the multiplayer sends are
// outside this component.

#include "localization.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

struct ItemDefinition {
    std::int32_t name_id{};          // +0: 003fdb40(key)
    std::int32_t description_id{};   // +4: 003fdb40(sprintf("D_%s", key))
    std::string resource;            // +8: strcpy into 32 bytes
    std::int32_t resource_28{};      // +28: 004703b0's 00147150 slot (0: none loaded)
    bool operator==(const ItemDefinition&) const = default;
};

class ItemTable {
public:
    // 00470450: appends a row without a duplicate check. A key whose "D_"
    // form or a resource that would overflow its 32-byte buffer throws.
    void register_item(const Localization& localization, std::string_view key, std::string_view resource);
    // The linear search of 00470a70 / 004709c0 / 004705d0: the first row
    // whose +0 equals the key's intern ID, -1 when none (a key missing from
    // the pools is -1 and matches a row registered with a missing key).
    std::int32_t index_of(const Localization& localization, std::string_view key) const;
    const std::vector<ItemDefinition>& items() const { return items_; }
    // Row +0x28 (004703b0 writes it through 00147150). An index outside the
    // table throws (the original reads past the rows).
    void set_resource_28(std::int32_t index, std::int32_t handle);
    std::int32_t resource_28(std::int32_t index) const;
    const ItemDefinition& at(std::int32_t index) const;
private:
    std::vector<ItemDefinition> items_;
};

struct ItemStack {
    std::int32_t index{};
    std::int32_t count{};
    bool operator==(const ItemStack&) const = default;
};

// One actor's +0x3528 list (constructed empty by 001b6a60).
class ItemInventory {
public:
    // 00470870 (solo): a negative index is ignored; the item's pair gets
    // count += n, or {index, n} is appended.
    void add(std::int32_t index, std::int32_t count);
    // 004706c0 (solo): n < held subtracts n and returns n; otherwise the
    // count becomes 0 (the pair stays) and the old count is returned. A
    // negative index or a missing pair returns 0.
    std::int32_t remove(std::int32_t index, std::int32_t count);
    // 00470560: the pair's count, 0 when absent.
    std::int32_t count(std::int32_t index) const;
    // 004704f0: any pair with a nonzero count.
    bool any() const;
    const std::vector<ItemStack>& stacks() const { return stacks_; }
private:
    ItemStack* find(std::int32_t index);
    std::vector<ItemStack> stacks_;
};

}
