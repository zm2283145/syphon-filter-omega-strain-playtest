#include "pickup_menu.hpp"

#include <stdexcept>

namespace omega {
namespace {

float as_time(std::int32_t value) {
    return static_cast<float>(value);  // cvt.s.w
}

std::int32_t add(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) + static_cast<std::uint32_t>(b));
}

// 002535e0: the child's element, or 0 when the child is missing.
std::uint32_t lookup(PickupMenuHost& host, std::uint32_t node, std::string_view name) {
    const std::uint32_t child = host.find_child(node, name);
    return child ? host.create_element(child) : 0;
}

// 00251040: bytes +10 / +11 = 1, then the pickup text colour.
void entry_text(PickupMenuHost& host, std::uint32_t text) {
    host.text_byte(text, 0x10, 1);
    host.text_byte(text, 0x11, 1);
    host.set_colour(text, kPickupMenuTextColour);
}

// 0036b180(0, 0x68, "pickup_menu.cc", line), then 00259420 when it succeeds.
std::uint32_t preview(PickupMenuHost& host, std::uint32_t line) {
    const std::uint32_t block = host.allocate(kPickupMenuPreviewSize, "pickup_menu.cc", line);
    return block ? host.construct_preview(block) : 0;
}

PickupMenuEntry& slot(PickupMenu& menu, int index) {
    if (index >= kPickupMenuEntries)
        throw std::runtime_error("00253070: a sixth pickup menu entry would overwrite the menu at +90");
    return menu.entries[static_cast<std::size_t>(index)];
}

}  // namespace

void pickup_menu_entry(PickupMenuEntry& entry, const PickupMenuPanels& panels, PickupMenuHost& host,
                       std::uint8_t kind, std::uint32_t item, std::int32_t base) {
    entry.kind = kind;
    entry.item = item;
    entry.word_08 = 0;
    entry.element_14 = host.create_element(panels.panel1);
    host.evaluate(entry.element_14, as_time(base));
    host.fade(entry.element_14, as_time(add(base, kPickupMenuEntryFade)));
    entry_text(host, host.element_text(entry.element_14));
    entry.preview_0c = preview(host, kPickupMenuPreviewLine);
    if (base != 0) {
        host.set_visible(panels.panel2, 0, 1);
        return;
    }
    host.set_visible(panels.panel2, 1, 1);
    entry.element_18 = host.create_element(panels.panel2);
    host.evaluate(entry.element_18, as_time(base));
    host.fade(entry.element_18, as_time(add(base, kPickupMenuEntryFade)));
    entry_text(host, host.element_text(entry.element_18));
    entry.preview_10 = preview(host, kPickupMenuPanelPreviewLine);
}

void pickup_menu_build(PickupMenu& menu, PickupMenuPanels& panels, PickupMenuHost& host,
                       std::uint32_t screen, const PickupMenuBackpack& backpack,
                       std::uint32_t inventory, std::uint32_t owner_check, std::uint32_t drop) {
    menu.backpack = backpack.address;
    for (PickupMenuEntry& entry : menu.entries) {  // 001004b0 with 00251350
        entry.preview_0c = 0;
        entry.preview_10 = 0;
        entry.element_14 = 0;
        entry.element_18 = 0;
    }
    menu.count = 0;
    menu.cursor = 0;
    menu.start = drop ? kPickupMenuDropStart : 0;
    menu.menu_9c = lookup(host, screen, "itempickupmenu");
    menu.triangle_a0 = lookup(host, screen, "trianglebutton");
    menu.x_a4 = lookup(host, screen, "xbutton");
    menu.inventory_a8 = lookup(host, host.element_node(menu.menu_9c), "inventorytextpanel");
    menu.pickup_ac = lookup(host, host.element_node(menu.menu_9c), "pickuptextpanel");
    if (drop) {
        menu.triangle_drop_b0 = lookup(host, screen, "triangle_drop");
        menu.x_drop_b4 = lookup(host, screen, "x_drop");
    } else {
        menu.x_drop_b4 = 0;
        menu.triangle_drop_b0 = 0;
    }

    const std::vector<std::uint32_t> items = host.list_items(backpack.address, inventory, kPickupMenuEntries);
    if (items.size() > static_cast<std::size_t>(kPickupMenuEntries))
        throw std::runtime_error("002553e0: more than 5 items for the pickup menu");
    const std::uint32_t node = host.element_node(menu.menu_9c);
    panels.panel1 = host.find_child(node, "panel1");
    panels.panel2 = host.find_child(node, "panel2");

    int next = 0;  // the entry pointer s2
    for (std::uint32_t item : items) {
        pickup_menu_entry(slot(menu, next++), panels, host, 0, item, menu.start);
        ++menu.count;
    }
    const bool state_e50 = owner_check != 0 && host.owner_state_e50(backpack.address) != 0;
    const bool state_d90 = owner_check != 0 && host.owner_state_d90(backpack.address) != 0;
    const std::int32_t ammunition = backpack.word_2e4 ? backpack.word_2e8 : 0;
    if (menu.count + int(state_e50) + int(state_d90) < kPickupMenuEntries && ammunition > 0) {
        pickup_menu_entry(slot(menu, next++), panels, host, 2, 0xffffffffu,
                          static_cast<std::int32_t>(drop & 0xff));
        ++menu.count;
    }
    if (state_e50) {
        pickup_menu_entry(slot(menu, next++), panels, host, 3, 0xffffffffu, 0);
        ++menu.count;
    }
    if (state_d90) {
        pickup_menu_entry(slot(menu, next++), panels, host, 4, 0xffffffffu, 0);
        ++menu.count;
    }
    if (backpack.owner_300 != 0 && backpack.owner_key_0c == backpack.key_304) {
        for (const auto& [item, word] : backpack.owner_pairs) {
            if (menu.count >= kPickupMenuEntries || word == 0)
                continue;
            PickupMenuEntry& entry = slot(menu, next++);
            pickup_menu_entry(entry, panels, host, 5, item, 0);
            entry.word_08 = 0;
            ++menu.count;
        }
    }

    host.evaluate(menu.menu_9c, as_time(menu.start));
    host.fade(menu.menu_9c, as_time(add(menu.start, static_cast<std::int32_t>(
                                                        static_cast<std::uint32_t>(kPickupMenuEntryFade) *
                                                        static_cast<std::uint32_t>(menu.count - 1)))));
    host.evaluate(menu.triangle_a0, as_time(menu.start));
    host.fade(menu.triangle_a0, as_time(add(menu.start, kPickupMenuButtonFade)));
    if (drop) {
        host.set_text(menu.x_a4, "");
        host.set_text(menu.x_drop_b4, host.get_string("Drop"));
        host.set_text(menu.triangle_drop_b0, host.get_string("Cancel"));
        host.set_text(menu.triangle_a0, "");
        host.set_text(menu.inventory_a8, "");
        host.set_text(menu.pickup_ac, "");
    } else {
        host.set_text(menu.triangle_a0, host.get_string("Exit"));
        host.set_text(menu.x_a4, host.get_string("Swap"));
        const std::uint32_t player = host.player(kPickupMenuPlayerList, -1);
        host.set_text(menu.inventory_a8, host.player_name(player));
        const std::uint32_t inventory_text = host.element_text(menu.inventory_a8);
        host.text_byte(inventory_text, 0x10, 2);
        host.set_colour(inventory_text, kPickupMenuTextColour);
        host.text_byte(inventory_text, 0x5b, 1);
        const std::string available = host.get_string("Available");
        host.set_text(menu.pickup_ac, available);
        const std::uint32_t pickup_text = host.element_text(menu.pickup_ac);
        host.text_byte(pickup_text, 0x10, 2);
        host.set_colour(pickup_text, kPickupMenuTextColour);
    }
    host.evaluate(menu.x_a4, as_time(menu.start));
    host.fade(menu.x_a4, as_time(add(menu.start, kPickupMenuButtonFade)));
}

void pickup_menu_previous(PickupMenu& menu, PickupMenuHost& host) {
    if (menu.cursor <= 0)
        return;
    host.post_sound("GS_SELECT", kPickupMenuPlayerList, 1);
    --menu.cursor;
}

void pickup_menu_next(PickupMenu& menu, PickupMenuHost& host) {
    if (menu.cursor + 1 >= menu.count)
        return;
    host.post_sound("GS_SELECT", kPickupMenuPlayerList, 1);
    ++menu.cursor;
}

void pickup_menu_destroy(PickupMenu& menu, PickupMenuHost& host) {
    host.close_backpack(menu.backpack);
    host.remove_element(menu.menu_9c);
    host.remove_element(menu.triangle_a0);
    host.remove_element(menu.x_a4);
    host.remove_element(menu.inventory_a8);
    host.remove_element(menu.pickup_ac);
    if (menu.triangle_drop_b0)
        host.remove_element(menu.triangle_drop_b0);
    if (menu.x_drop_b4)
        host.remove_element(menu.x_drop_b4);
    for (auto entry = menu.entries.rbegin(); entry != menu.entries.rend(); ++entry) {  // 00100440, 00251290
        if (entry->preview_0c)
            host.release_preview(entry->preview_0c, kPickupMenuPreviewRelease);
        if (entry->preview_10)
            host.release_preview(entry->preview_10, kPickupMenuPreviewRelease);
        if (entry->element_14)
            host.remove_element(entry->element_14);
        if (entry->element_18)
            host.remove_element(entry->element_18);
    }
}

namespace {

// 00128bd0 into a 0x40-byte stack buffer (sp+1d0 / sp+190 of 002526f0, sp+...
// of 00251920); a longer text would run into the neighbouring locals.
template <typename... Args>
std::string format_n(const char* where, int limit, const char* format, Args... args) {
    char buffer[0x200];
    const int n = std::snprintf(buffer, sizeof buffer, format, args...);
    if (n < 0 || n >= limit)
        throw std::runtime_error(std::string(where) + ": a pickup menu text overruns its stack buffer");
    return std::string(buffer, static_cast<std::size_t>(n));
}

template <typename... Args>
std::string format64(const char* where, const char* format, Args... args) {
    char buffer[0x200];
    const int n = std::snprintf(buffer, sizeof buffer, format, args...);
    if (n < 0 || n >= 0x40)
        throw std::runtime_error(std::string(where) + ": a pickup menu text of 0x40 bytes or more overruns its stack buffer");
    return std::string(buffer, static_cast<std::size_t>(n));
}

std::int32_t multiply(std::int32_t a, std::int32_t b) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(a) * static_cast<std::uint32_t>(b));
}

// Preview +c is written without a null check (sw / sb at +60 / +64).
void preview_c(PickupMenuFrameHost& host, std::uint32_t preview, std::uint32_t word_60, std::uint8_t byte_64) {
    if (!preview)
        throw std::runtime_error("002526f0: the entry's +c preview is null (the original writes through it)");
    host.set_preview(preview, word_60, byte_64);
}

void preview_10(PickupMenuFrameHost& host, std::uint32_t preview, std::uint32_t word_60, std::uint8_t byte_64) {
    if (preview)
        host.set_preview(preview, word_60, byte_64);
}

const PickupMenuAmmo& ammo_of(const std::optional<PickupMenuAmmo>& ammo, const char* where) {
    if (!ammo)
        throw std::runtime_error(std::string(where) +
                                 ": 00255500 found no backpack entry; its outputs are uninitialised stack words");
    return *ammo;
}

std::uint32_t record_preview(const PickupMenuRecord& record) {
    return record.hold_f0 == 7 ? record.icon_11c : record.model_13c;
}

PickupMenuEntry& selected(PickupMenu& menu) {
    if (menu.cursor < 0 || menu.cursor >= kPickupMenuEntries)
        throw std::runtime_error("pickup menu: the cursor +94 is outside the five entries");
    return menu.entries[static_cast<std::size_t>(menu.cursor)];
}

void cycle_selected(PickupMenu& menu, PickupMenuFrameHost& host, std::int32_t dir) {
    PickupMenuEntry& entry = selected(menu);
    const std::int32_t item = host.cycle(menu.backpack, static_cast<std::int32_t>(entry.item), dir);
    if (item == static_cast<std::int32_t>(entry.item))
        return;
    host.post_sound("GS_SELECT", kPickupMenuPlayerList, 1);
    entry.item = static_cast<std::uint32_t>(item);
}

}  // namespace

bool pickup_menu_update(PickupMenu& menu, PickupMenuFrameHost& host) {
    const std::int32_t backpack_ammo = host.backpack_ammunition();   // sp+160
    std::string ammo_label = host.get_string("ammo");                // sp+15c
    if (!host.ammo_label_48af08())
        ammo_label.clear();
    const bool drop = menu.start != 0;
    if (drop) {
        const std::uint8_t held = host.held_84();
        menu.entries[0].item = static_cast<std::uint32_t>(held == 6 ? -1 : host.slot_item(held));
    }
    if (menu.count > kPickupMenuEntries)
        throw std::runtime_error("002526f0: the entry count +90 is beyond the five entries");
    for (std::int32_t i = 0; i < menu.count; ++i) {
        PickupMenuEntry& entry = menu.entries[static_cast<std::size_t>(i)];
        std::string text1, text2;
        switch (entry.kind) {
        case 0: {
            const std::int32_t item = static_cast<std::int32_t>(entry.item);
            std::int32_t s6 = host.slot_of_item(item);
            if (drop)
                s6 = static_cast<std::int8_t>(host.held_84());
            const std::uint8_t slot = static_cast<std::uint8_t>(s6);
            const std::int32_t held_item = host.slot_item(slot);
            const bool dual = slot == 0 && host.slot_item(1) >= 0;
            const bool same = held_item == item;
            const bool unlocked = slot == 0 && !dual && same &&
                                  host.agent_unlock(host.player(kPickupMenuPlayerList, -1), 1) != 0;
            host.cycle(menu.backpack, item, 0);
            host.cycle(menu.backpack, item, 1);
            const bool takeable = (host.take(menu.backpack, item, 1) & 0xff) != 0;
            const bool slot4 = slot == 4;
            const bool no_clip = host.has_clip(item) == 0;
            const bool flagged = (host.slot_flag(menu.backpack, item) & 0xff) != 0;
            const std::optional<PickupMenuAmmo> ammo = host.ammunition(menu.backpack, item);
            if (held_item < 0) {
                preview_c(host, entry.preview_0c, 0, 7);
            } else {
                const PickupMenuRecord held = host.record(held_item);
                std::int32_t spare = host.spare(s6);
                const std::int32_t full = multiply(held.clip_70, host.clips_for_actor(held.clips_74));
                std::int32_t clip = host.clip(s6);
                if (slot4) {
                    text1 = format64("002526f0", "(%d)", spare);
                } else if (!no_clip) {
                    if (dual) {
                        clip /= 2;
                        spare /= 2;
                    }
                    const std::string full_text = spare == full ? host.get_string("AmmoFull") : std::string();
                    const std::string dual_text = dual ? host.get_string("DualWield") : std::string();
                    text1 = format64("002526f0", "%s (%d/%d)\n%s", dual_text.c_str(), clip, spare, full_text.c_str());
                }
                preview_c(host, entry.preview_0c, record_preview(held), 7);
            }
            const PickupMenuRecord offered = host.record(item);
            if (!takeable || (same && flagged)) {
                preview_10(host, entry.preview_10, 0, 7);
            } else if (same && !unlocked && !slot4 && ammo_of(ammo, "002526f0").total != 0) {
                preview_10(host, entry.preview_10, 0, 1);
                text2 = format64("002526f0", "%s(%d)", ammo_label.c_str(), ammo->total);
            } else {
                preview_10(host, entry.preview_10, record_preview(offered), 7);
                if (slot4)
                    text2 = format64("002526f0", "(%d)", ammo_of(ammo, "002526f0").spare);
                else if (!no_clip)
                    text2 = format64("002526f0", "(%d/%d)", ammo_of(ammo, "002526f0").clip, ammo->spare);
            }
            break;
        }
        case 2:
            preview_c(host, entry.preview_0c, 0, 2);
            text1 = format64("002526f0", "%d", host.armor_33a0());
            preview_10(host, entry.preview_10, 0, 2);
            text2 = format64("002526f0", "%d", backpack_ammo);
            break;
        case 3:
        case 4:
            preview_10(host, entry.preview_10, 0, entry.kind == 3 ? 4 : 6);
            preview_c(host, entry.preview_0c, 0, 7);
            break;
        case 5: {
            // 00252cf4: an owner failing the check passes a null list to 00470560 (its +4 read).
            if (!host.owner_valid())
                throw std::runtime_error("00252d88: 00470560 on a null owner list (reads address 4)");
            const std::int32_t item = static_cast<std::int32_t>(entry.item);
            const std::uint32_t icon = host.item_icon(item);   // the delay slot's *(*00587ff8 + item * 0x2c + 0x28)
            const std::int32_t offered = host.owner_item_count(item);
            if (offered != 0) {
                preview_10(host, entry.preview_10, icon, 7);
                if (offered >= 2)
                    text2 = format64("002526f0", "%d", offered);
            } else {
                preview_10(host, entry.preview_10, 0, 7);
            }
            const std::int32_t held = host.actor_item_count(item);
            if (held != 0) {
                preview_c(host, entry.preview_0c, icon, 7);
                if (held >= 2)
                    text1 = format64("002526f0", "%d", held);
            } else {
                preview_c(host, entry.preview_0c, 0, 7);
            }
            break;
        }
        default:
            throw std::runtime_error("002526f0: a kind-1 / unknown entry formats stale text buffers");
        }
        host.set_element_data(entry.element_14, entry.preview_0c);
        if (entry.element_18)
            host.set_element_data(entry.element_18, entry.preview_10);
        host.set_text(entry.element_14, text1);
        if (entry.element_18)
            host.set_text(entry.element_18, text2);
        const float start = as_time(menu.start);
        if (i == menu.cursor)
            host.anim_range(entry.element_14, 20.0f + start, 35.0f + start, 1);
        else
            host.fade(entry.element_14, 10.0f + start);
        if (entry.element_18)
            host.fade(entry.element_18, 10.0f + start);
    }
    return menu.count > 0;
}

bool pickup_menu_confirm(PickupMenu& menu, PickupMenuFrameHost& host) {
    const RootVector position = host.actor_position();
    host.post_sound("GS_CONFIRM", kPickupMenuPlayerList, 1);
    bool result = false;
    PickupMenuEntry& entry = selected(menu);
    const std::uint32_t bp = menu.backpack;
    switch (entry.kind) {
    case 0: {
        const std::int32_t item = static_cast<std::int32_t>(entry.item);
        std::int32_t s2 = host.slot_of_item(item);
        if (menu.start != 0) {
            s2 = static_cast<std::int8_t>(host.held_84());
            if (static_cast<std::uint8_t>(s2) >= 6)
                return true;
        }
        const std::uint8_t slot = static_cast<std::uint8_t>(s2);
        const std::int32_t held_item = host.slot_item(slot);
        const bool slot0 = slot == 0;
        const bool has_second = host.slot_item(1) >= 0;
        const bool dual = slot0 && has_second;
        bool takeable = (host.take(bp, item, 1) & 0xff) != 0;
        bool same = takeable && held_item == item;
        if (same && host.slot_flag(bp, item) != 0) {
            same = false;
            takeable = false;
        }
        const bool unlocked = slot0 && !dual && same &&
                              host.agent_unlock(host.player(kPickupMenuPlayerList, -1), 1) != 0;
        const std::optional<PickupMenuAmmo> ammo = host.ammunition(bp, item);
        if (same && !unlocked && takeable) {
            const std::int32_t rounds = host.add_spare(s2, ammo_of(ammo, "00251920").total, 0);
            host.remove_ammunition(bp, item, rounds);
            if (rounds) {
                host.positional_sound(kPickupMenuSoundAmmo, position);
                if (host.backpack_60() == 1)
                    host.crate_interact(bp, -1, -1, 5);   // 00251c68: +20 stays 5 (the sb 2 is the sound message's)
            }
            break;
        }
        if (static_cast<std::int32_t>(entry.item) < 0)
            break;
        const bool swap = !same && held_item >= 0;
        const bool swap_dual = slot0 && has_second;
        const bool held_slot = slot == host.held_84();
        std::int32_t taken = -1, put = -1, put_spare = 0, put_clip = 0;
        if (swap) {
            put_spare = host.spare(s2);
            put_clip = host.clip(s2);
            if (swap_dual) {
                host.remove(1);
                put_spare /= 2;
                put_clip /= 2;
            }
            host.remove(s2);
        }
        if (takeable) {
            host.positional_sound(kPickupMenuSoundWeapon, position);
            const PickupMenuRecord record = host.record(item);
            if (unlocked)
                s2 = 1;
            host.add(item);
            const PickupMenuAmmo& a = ammo_of(ammo, "00251920");
            host.set_clip(s2, a.clip);
            const std::int32_t rounds = host.add_spare(s2, a.spare, 0);
            const std::int32_t next = host.cycle(bp, item, 0);
            if (record.hold_f0 == 1)
                host.remove_ammunition(bp, item, rounds);
            else
                host.take(bp, item, 0);
            taken = static_cast<std::int32_t>(entry.item);
            const std::string name = host.get_string(record.name_80);
            const std::string picked = host.get_string("PickupWpn");
            host.hud_message(format64("00251920", "%s %s", name.c_str(), picked.c_str()));
            if (held_slot && swap)
                host.equip(s2);
            if (host.take(bp, item, 1) == 0)
                entry.item = static_cast<std::uint32_t>(next);
        }
        if (swap) {
            host.mark_30c(bp);
            host.add_weapon(bp, held_item, put_spare, put_clip, 0);
            entry.item = static_cast<std::uint32_t>(held_item);
            put = held_item;
            if (host.record(held_item).class_30 == 6)
                throw std::runtime_error("0025200c: the IR goggle switch 00199590 is not ported");
            if (swap_dual)
                host.add_weapon(bp, held_item, put_spare, put_clip, 0);
        }
        if (taken < 0 && put < 0)
            break;
        if (host.backpack_60() == 1)
            host.crate_interact(bp, taken, put, 2);   // 0025206c
        if (put != 0)
            host.clear_selection_a4();
        if (menu.start > 0)
            result = true;
        break;
    }
    case 2: {
        const std::int32_t old_max = host.max_armor_339c();
        const std::int32_t old_armor = host.armor_33a0();
        if (host.backpack_60() == 1)
            host.crate_interact(bp, -1, -1, 2);   // 0025211c
        const auto [armor, max_armor] = host.take_armor(bp);
        host.set_armor(armor, max_armor);
        host.add_armor(bp, old_armor, old_max);
        if (old_armor != 0)
            host.mark_30c(bp);
        break;
    }
    case 3:
        if (host.owner_valid())
            host.owner_pickup_message();   // 0025220c
        result = true;
        break;
    case 4:
        if (host.owner_valid())
            host.owner_disguise_taken();   // 002522c4..00252344
        result = true;
        break;
    case 5:
        if (entry.word_08 == 0) {
            if (!host.owner_valid())
                throw std::runtime_error("002523a4: the kind-5 take reads a null owner's +3528 (address 3528)");
            const std::int32_t item = static_cast<std::int32_t>(entry.item);
            entry.word_08 = static_cast<std::uint32_t>(host.owner_remove_item(item, 0x2710));
            const std::int32_t count = static_cast<std::int32_t>(entry.word_08);
            host.actor_add_item(item, count);
            const std::optional<std::string> name = host.item_name(item);
            const std::string picked = host.get_string("PickupWpn");
            if (!name)
                throw std::runtime_error("002523f8: 00470270 gave a null item name to sprintf's %s");
            host.hud_message(format_n("00251920", 0x80, "%s %s", name->c_str(), picked.c_str()));
            host.inventory_message(bp, item, count);
        }
        break;
    default:
        break;
    }
    host.pickup_ammunition(bp);
    host.share_dual();
    if (host.multiplayer_5721c8() && menu.start != 0)
        throw std::runtime_error("002524a8: the multiplayer drop message (002ebaf8 / 003c9e30) is not ported");
    return result;
}

void pickup_menu_left(PickupMenu& menu, PickupMenuFrameHost& host) {
    cycle_selected(menu, host, 1);
}

void pickup_menu_right(PickupMenu& menu, PickupMenuFrameHost& host) {
    cycle_selected(menu, host, 0);
}

}  // namespace omega
