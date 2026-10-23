#include "item_inventory.hpp"

#include <stdexcept>

namespace omega {
namespace {

// The EE's wrapping 32-bit addu/subu.
std::int32_t wrap(std::int64_t value) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(static_cast<std::uint64_t>(value)));
}

}

void ItemTable::register_item(const Localization& localization, std::string_view key, std::string_view resource) {
    const std::string description = "D_" + std::string(key);
    // sprintf and strcpy into 32-byte stack/row buffers.
    if (description.size() >= 32) throw std::runtime_error("RegisterItem key overflows the description buffer");
    if (resource.size() >= 32) throw std::runtime_error("RegisterItem resource overflows its 32-byte field");
    items_.push_back({localization.lookup(key), localization.lookup(description), std::string(resource)});
}

std::int32_t ItemTable::index_of(const Localization& localization, std::string_view key) const {
    const auto id = localization.lookup(key);
    for (std::size_t i = 0; i < items_.size(); ++i)
        if (items_[i].name_id == id) return static_cast<std::int32_t>(i);
    return -1;
}

const ItemDefinition& ItemTable::at(std::int32_t index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= items_.size())
        throw std::runtime_error("item table row " + std::to_string(index) + " is outside the table (*00587ff8 + index * 0x2c)");
    return items_[static_cast<std::size_t>(index)];
}

void ItemTable::set_resource_28(std::int32_t index, std::int32_t handle) {
    at(index);
    items_[static_cast<std::size_t>(index)].resource_28 = handle;
}

std::int32_t ItemTable::resource_28(std::int32_t index) const { return at(index).resource_28; }

ItemStack* ItemInventory::find(std::int32_t index) {
    for (auto& stack : stacks_)
        if (stack.index == index) return &stack;
    return nullptr;
}

void ItemInventory::add(std::int32_t index, std::int32_t count) {
    if (index < 0) return;
    if (auto* stack = find(index)) {
        stack->count = wrap(static_cast<std::int64_t>(stack->count) + count);
        return;
    }
    stacks_.push_back({index, count});
}

std::int32_t ItemInventory::remove(std::int32_t index, std::int32_t count) {
    if (index < 0) return 0;
    auto* stack = find(index);
    if (!stack) return 0;
    if (count < stack->count) {
        stack->count = wrap(static_cast<std::int64_t>(stack->count) - count);
        return count;
    }
    const auto held = stack->count;
    stack->count = 0;
    return held;
}

std::int32_t ItemInventory::count(std::int32_t index) const {
    for (const auto& stack : stacks_)
        if (stack.index == index) return stack.count;
    return 0;
}

bool ItemInventory::any() const {
    for (const auto& stack : stacks_)
        if (stack.count != 0) return true;
    return false;
}

}
