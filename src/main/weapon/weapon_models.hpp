#pragma once
// The weapon database's model resources (Checkpoint 131, COMBAT.md "Checkpoint 131"): the
// preload 00145710 / unload 00145630 of a record's model (+13c), the loader 00147150 (the
// member "<record +0 up to '.'>.HOG" of the archive db+1c, GAMEDATA/COMMON/WPNHOG.HOG), the
// clones 00146b80 for every record naming the same HOG (+0 -> +13c, +120 -> +11c), the load
// callback 00147110 -> 00146fc0 (0038cd90, gun_model_load.hpp) and the propagation 001468e0,
// and the level's preload list of 001457c0 (item 0x67 and the POP WPN: section).
//
// The original loads asynchronously (003eba50 / 003eae60): a model stays +417 = 0xff (not
// drawn) until the callback ran. Here the callback runs at once, inside 00147150.
#include "gun_model_load.hpp"
#include "equipment_loadout.hpp"
#include "weapon_database.hpp"
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega {

// The archive db+1c (opened by 001450e0 at 00145108 from "%s\COMMON\WPNHOG.HOG"): member
// names in index order and a reader by index.
struct WeaponModelArchive {
    std::vector<std::string> names;
    std::function<std::vector<std::uint8_t>(std::size_t index)> read;
    // An archive Hog_Load holds in memory (a mission WPN.HOG: 003f96e0 with +14c set): at most `count` of the bytes after
    // member `index` (the next member's start). Unset for db+1c.
    std::function<std::vector<std::uint8_t>(std::size_t index,std::size_t count)> following;
};

// 0036def0 -> 0036d2a0: the first member (in index order) whose name equals `name` under the
// case folding of 00128e80 (the ctype table; a byte >= 0x80 is outside it and throws).
std::optional<std::size_t> weapon_archive_member(const std::vector<std::string>& names,std::string_view name);

// The name compare of 00146b80 / 001468e0: '.' ends a name like NUL, a..z fold to upper case
// (signed bytes: >= 0x80 are not folded); true when both end together.
bool weapon_model_name_equal(std::string_view a,std::string_view b);

// 001457c0's list: item 0x67, then the POP's WPN: section (00178bb0 finds the tag word
// 0x3a4e5057 at a 4-aligned offset: the section must be unique here) as a count of groups,
// each a word n and n item words; an item already listed is skipped; more than 128 throw.
std::vector<std::int32_t> weapon_level_preload_list(std::span<const std::uint8_t> population);

class WeaponModelLibrary {
public:
    // A texture manager record (*00539248 +8[id]): `refs` is its +64 reference count (3bba10 sets 1; 00380a40
    // and 00380fb0 add one; 003813a0 drops one). A record released to 0 is destroyed (00380280 with
    // 003813a0's a2 = 0): its image is freed (`tdx` empty) and its id is no longer found by name.
    struct Texture { std::string member; std::vector<std::uint8_t> tdx; std::int32_t refs{}; };
    // `error_tdx` answers 00381000 (the default texture: GAMEDATA/COMMON/ERROR.TDX).
    WeaponModelLibrary(WeaponDatabase& database,WeaponModelArchive archive,std::vector<std::uint8_t> error_tdx);
    // 00145710(db, item, a2): the record is db+14 + item * 0x140 (no clamp: an item outside
    // the table throws).
    void preload(std::int32_t item,std::uint8_t keep_139);
    // 00145630(db, item, a2). a2 != 0 with the count at 0 and the model's +41c <= 0 frees through
    // 00146460(db, model name) (Checkpoint 132f4).
    void unload(std::int32_t item,std::uint8_t free);
    // 001457c0: the table db+b8..+d0 (00489cc0's seven names, upper-cased in place) is
    // cleared; with `mission` (GAMEDATA\<mission>\WPN.HOG, 003f96e0 with its +14c set)
    // 00146040 loads it; then the preload loop 00145710(db, list[i], 1) in order; then
    // 00145c30: a table entry still 0 gets a model constructed but not loaded
    // (0038d530(m, name k, k, 0, 0, 0, 0, -1), +41c 1); then the record pass: a record whose
    // +120 contains ".skl" / ".SKL" (00129c80) and whose +11c is 0 loads it (00147150(db,
    // +120, +11c, i, 1, -1, -1)). The pass's other lookups (+90 scope 00146000, the +120
    // particle and +e0 effect 00398070) are not hosted.
    void level_preload(std::span<const std::int32_t> items,const WeaponModelArchive* mission=nullptr);
    // 00146040(db, archive): every member of the mission archive in index order. A member
    // whose name equals table name k (00146b80's compare: '.' ends a name, a..z fold) gets a
    // new model at db+b8+4k (0038d530(m, table name k, k, 0, 0, 0, 0, 0) and 0038cd90 on the
    // member), for each such k; any other member is loaded into a temporary model
    // (0038d530(t, member name, 0, ...0)) that 00146b80 copies into every record naming it at
    // +0 (+13c: +40a the record, +40d / +40e its +cc / +108, +404 / +408 from 00147880) or
    // +120 (+11c: +40a, +419 1).
    void load_mission_archive_146040(const WeaponModelArchive& mission);
    // Checkpoint 132zf5: 004703b0's per-row 00147150(db, row+8, row+28, -1, 0, 0, 0) (level
    // load 00135894): `resource` up to '.' plus ".HOG" in db+1c; absent, the slot stays 0 (the
    // return). Otherwise the slot's model (0036b180(0x420), 0038d530(m, name, -1, 0, 1, 0, 0),
    // no 00147880 for a negative item), 00146b80's clones into records naming the HOG, and the
    // load (00147110 -> 00146fc0: 0038cd90, then 001468e0). Returns the slot's handle.
    std::int32_t load_item_147150(const std::string& resource);
    // db+b8+4k (k < 7): the table's model handle, 0 when none. k 5 is the shells model db+cc.
    std::int32_t table_model(std::size_t k) const;
    // 0055de90: the shells model's +3c0 (part 0's LOD 0 texture id) 00147880 stored last.
    std::optional<std::int16_t> shell_texture() const{return shell_texture_;}

    // Model handles are the values stored in record +13c / +11c and the attach tables:
    // small positive integers (below the loadout's stand-ins 0x10000).
    bool contains(std::int32_t handle) const;
    // Port-only (132md3): handles issued so far run 1..handle_limit() (freed ones fail contains).
    std::int32_t handle_limit() const{return static_cast<std::int32_t>(models_.size());}
    const GunModel& model(std::int32_t handle) const;
    std::span<const std::uint8_t> hog(std::int32_t handle) const;
    AttachedModel attached(std::int32_t handle) const;           // +40a / +40c (0038c7d0)
    // 0013ff40's muzzle effect flag: the model's +411 (the flare part) is set (!= 0xff).
    bool flare_411(std::int32_t handle) const;
    // 00385370: the model's +130 (the muzzle offset 0038cd90 accumulates).
    RootVector muzzle_130(std::int32_t handle) const;
    // The texture ids 0038cd90 stored at +3c0 / +3d0 index this list.
    const std::vector<Texture>& textures() const{return textures_;}
    // Bumped whenever a model or texture is added or replaced.
    std::uint32_t generation() const{return generation_;}
    // 00147880(db, record +10c) queries that found a shell record (the model's +404 / +408
    // were written).
    std::size_t shell_records() const{return shell_records_;}
    // 00146460's frees (0036b130), the 0038cc70 texture releases (003813a0(*00539248, id, 0)) and the
    // records those releases destroyed (their count reached 0).
    std::size_t models_freed() const{return models_freed_;}
    std::size_t texture_releases() const{return texture_releases_;}
    std::size_t textures_destroyed() const{return textures_destroyed_;}
    const WeaponDatabase& database() const{return db_;}
private:
    struct Entry {
        GunModel model;
        std::shared_ptr<const std::vector<std::uint8_t>> hog;
        bool freed=false;   // 00146460's 0036b130: the handle is no longer valid
    };
    WeaponRecord& record_at(std::int32_t item);
    Entry& entry(std::int32_t handle);
    const Entry& entry(std::int32_t handle) const;
    std::int32_t allocate(const GunModel& model,const std::shared_ptr<const std::vector<std::uint8_t>>& hog);
    std::int32_t word_41c(std::int32_t handle) const;
    void set_word_41c(std::int32_t handle,std::int32_t value);
    // 00147880(db, ammo type): n = 00489ce0[type]; with db+cc and n below its +40c, the LOD 0
    // record +2c0 + 16n (and 0055de90 = its +3c0).
    std::optional<GunShellRecord> shell_147880(const WeaponRecord& record);
    // 0038cd90 on `model` with the texture context on `hog` (0036e1b0). `following` (count -> bytes) gives the memory
    // after the gun HOG when it lies in an archive image (load_mission_archive_146040).
    void load_member(GunModel& model,const std::shared_ptr<const std::vector<std::uint8_t>>& hog,
        const std::function<std::vector<std::uint8_t>(std::size_t)>& following={});
    // 00147150(db, name, slot, item, t0, t1, t2); slot: the record offset (+13c / +11c).
    void load_147150(const std::string& name,std::size_t slot,std::int32_t item,std::uint8_t t0,std::uint8_t t1,std::uint8_t t2);
    // 00146460(db, name): pass 1 sums the +41c of every record's model named name (+0 -> +13c,
    // +120 -> +11c) and stops at a model whose +418 is 0; with the sum <= 0 pass 2 releases the first
    // (0038cc70) and frees each (0036b130), clearing the record's slot.
    void free_146460(const std::string& name);
    // 0038cc70: +418 set cancels the archive request (none outstanding here) and clears it; +417 < 0
    // becomes -2; else the parts' textures are released (003813a0) and +417 = -3.
    void release_38cc70(std::int32_t handle);
    void clone_146b80(const GunModel& source,const std::shared_ptr<const std::vector<std::uint8_t>>& hog);
    void propagate_1468e0(const GunModel& source,const std::shared_ptr<const std::vector<std::uint8_t>>& hog);
    std::int16_t texture(const std::string& member,const std::vector<std::uint8_t>& hog);
    std::int16_t error_texture();
    // 003813a0(tm, id, 0): an id whose record is live drops a reference; at 0 the record is destroyed.
    void release_texture_3813a0(std::int16_t id);
    WeaponDatabase& db_;
    WeaponModelArchive archive_;
    std::vector<std::uint8_t> error_tdx_;
    std::vector<Entry> models_;
    std::vector<Texture> textures_;
    std::optional<std::int16_t> error_index_;
    std::uint32_t generation_{};
    std::size_t shell_records_{};
    std::size_t models_freed_{};
    std::size_t texture_releases_{};
    std::size_t textures_destroyed_{};
    std::array<std::int32_t,7> table_{};
    std::optional<std::int16_t> shell_texture_;
};

}
