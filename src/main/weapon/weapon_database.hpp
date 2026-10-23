#pragma once
// Weapon database COMMON\WEAPONS.WDB (COMBAT.md §1): the loader 00147400 and
// the record lookup 00147840 of the database object *004ffd30. The file is a
// header {word 4, record count, two unread words} followed by 0x140-byte
// records from +0x10. Records keep all their bytes; the accessors below name
// only the fields the recovered code reads (COMBAT.md field map), everything
// else stays raw. Runtime fields (+11c, +130..+13c) are cleared by the loader
// and filled by resource code that is not native (model, HUD icon, tracer).
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace omega {

inline constexpr std::size_t weapon_record_size=0x140;
inline constexpr std::uint32_t weapon_file_version=4;

struct WeaponRecord {
    std::array<std::uint8_t,weapon_record_size> bytes{};
    std::int32_t word(std::size_t offset) const;
    std::uint8_t byte(std::size_t offset) const;
    float real(std::size_t offset) const;
    // NUL-terminated text of at most `limit` bytes starting at `offset`.
    std::string text(std::size_t offset,std::size_t limit) const;
    void set_word(std::size_t offset,std::int32_t value);
    void set_byte(std::size_t offset,std::uint8_t value);
    void set_real(std::size_t offset,float value);
    void set_text(std::size_t offset,std::size_t limit,const std::string& value);

    std::string fire_sound_10() const{return text(0x10,16);}      // 0013ff40
    std::string reload_sound_20() const{return text(0x20,16);}    // 0013fb90
    std::uint8_t weapon_class_30() const{return byte(0x30);}      // -> slot via 00489d60
    std::int32_t near_34() const{return word(0x34);}
    std::int32_t mid_38() const{return word(0x38);}
    std::int32_t far_3c() const{return word(0x3c);}               // also the hitscan length
    std::int32_t accuracy_near_40() const{return word(0x40);}     // percent (0022f5b0)
    std::int32_t accuracy_mid_44() const{return word(0x44);}
    std::uint8_t item_kind_48() const{return byte(0x48);}
    std::int32_t accuracy_far_4c() const{return word(0x4c);}
    float aim_time_near_50() const{return real(0x50);}
    float aim_time_far_54() const{return real(0x54);}
    std::int32_t burst_5c() const{return word(0x5c);}
    std::int32_t damage_mid_64() const{return word(0x64);}
    std::int32_t damage_near_68() const{return word(0x68);}
    std::int32_t damage_far_6c() const{return word(0x6c);}
    std::int32_t clip_70() const{return word(0x70);}
    std::int32_t clips_74() const{return word(0x74);}
    std::uint8_t full_auto_78() const{return byte(0x78);}
    std::string display_name_80() const{return text(0x80,32);}
    std::int32_t tracer_interval_a0() const{return word(0xa0);}
    float rate_a4() const{return real(0xa4);}
    float noise_radius_b8() const{return real(0xb8);}
    std::uint8_t damage_type_bc() const{return byte(0xbc);}
    float explosion_radius_c4() const{return real(0xc4);}         // > 0: 0021a200 at the end of travel
    float explosion_outer_c8() const{return real(0xc8);}          // the bullet blast's f14 (002774a0, 132uk6)
    std::uint8_t projectile_d4() const{return byte(0xd4);}        // 1/2: launched projectile
    std::int32_t impact_effect_d8() const{return word(0xd8);}     // -1: none (00232460)
    std::uint8_t hold_class_f0() const{return byte(0xf0);}
    std::int32_t word_f4() const{return word(0xf4);}             // the inventory sends the largest carried value (00142e80)
    std::int32_t penetration_f8() const{return word(0xf8);}       // bullet +10 (request +24)
    float impact_noise_radius_fc() const{return real(0xfc);}      // AI sound at a static impact
    std::int32_t power_104() const{return word(0x104);}           // bullet speed; >= 200 enables the hit roll
    std::int32_t ammo_type_10c() const{return word(0x10c);}
    std::int32_t hud_icon_11c() const{return word(0x11c);}       // runtime; the HUD icon of hold class 7
    std::uint8_t looping_118() const{return byte(0x118);}
    std::int32_t tracer_resource_134() const{return word(0x134);} // runtime; >= 0 allocates a tracer object
    std::uint8_t keep_loaded_139() const{return byte(0x139);}
    std::uint8_t shake_13a() const{return byte(0x13a);}
    std::int32_t model_13c() const{return word(0x13c);}           // runtime attached-model resource (0 none)
};

class WeaponDatabase {
public:
    // 00147400 after 003cc060 read the file: header word 0 must be 4 (the
    // original returns 0 then; here it throws), the record count is word 1
    // (a count <= 0 loads nothing), records start at +0x10 (the original
    // does not check the file size; a short file throws here). Per record:
    // +138 = 0, +130 = +134 = -1, +13c = +11c = 0, and for damage type
    // +bc == 1 the three damage words +64/+68/+6c are divided by 5 (signed,
    // toward zero). The per-record name registration (00330110 / 00147600
    // into 00532b10) is register_weapon_unlocks (award_registry.hpp), run over
    // display_name_80 of each record.
    static WeaponDatabase load(std::span<const std::uint8_t> file);
    // Records taken as already loaded (synthetic databases for tests).
    explicit WeaponDatabase(std::vector<WeaponRecord> records);
    std::size_t size() const{return records_.size();}
    // 00147840: an ID outside [0, count) selects record 0. An empty database
    // throws (the original would read past the header).
    std::size_t index(std::int32_t id) const;
    const WeaponRecord& record(std::int32_t id) const{return records_[index(id)];}
    WeaponRecord& mutable_record(std::int32_t id){return records_[index(id)];}
    const std::vector<WeaponRecord>& records() const{return records_;}
    // Header words +8 and +c (not read by the loader), kept raw.
    std::array<std::uint32_t,2> header_words{};
private:
    std::vector<WeaponRecord> records_;
};

// 00489d60: weapon class (record +30) -> inventory slot. Classes >= 10 read
// past the table in the original; here they throw.
std::uint8_t weapon_slot_of_class(std::uint8_t weapon_class);

// The reticle name list *(db + d4) that the weapon resource loader 001457c0
// builds (0x10-byte list from 00146020: +0 capacity, +4 count, +8 data; the
// HUD's 00245e30 reads it). The data are pointers to record +90; here the
// record indices.
struct WeaponReticleList {
    std::uint32_t capacity=0;
    std::vector<std::size_t> records;
};
// The C string at record +90 (00128e80 / 003eeb30 read it to its NUL, which
// may lie past the 16-byte field; one missing before the record end throws).
std::string weapon_reticle_name(const WeaponRecord& record);
// 00145d58..00145e0c over the records in order: a nonzero signed byte +90
// searches the list for its name with 00128e80 (case folded through the ctype
// table 0x499c71; a compared byte >= 0x80 throws: the original indexes the
// table with a negative offset), +130 = the found index, else the count and
// the name is appended (00146000 -> 00139200: capacity 1, then doubled); a
// zero byte +90 sets +130 = -1. (The model / impact parts of the same loop,
// 00145e10..00145ec4, are not ported.)
WeaponReticleList build_weapon_reticles(WeaponDatabase& db);

}
