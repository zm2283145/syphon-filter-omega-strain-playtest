#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega {

struct Script;

// The surface material table (research/GENERATOR_RUNTIME.md "Checkpoint 119m"):
// the global at 0053bb90 (+0 the name map, +4 the row vector, byte +8 set once
// 00409020 has run). A row (0x124 bytes) is a 12-byte name string then five
// 0x38-byte properties indexed by MaterialProperties_MatPropName. The COL loader
// (003c20c8) writes each surface record's byte 1 with lookup(the name at +2),
// and the landing / footstep effect 00188fb0 reads rows[that byte].
struct MaterialProperty {
    std::string effect;           // +0 (12-byte string)
    std::int32_t effect_id{-1};   // +c: -1 named (resolved by 004089f0), -2 "none"
    std::int32_t bank{-1};        // +10: the sound bank, -1 no sound named, -2 "none"
    std::string sound;            // +14
    std::string decal;            // +20
    std::int32_t decal_id{-1};    // +2c
    float size{};                 // +30
    std::uint8_t blend{};         // +34
    bool operator==(const MaterialProperty&) const = default;
};

// 00409750: the default property (empty strings, ids -1, size 0.2).
MaterialProperty default_material_property();
// 004095a0(out, effect, bank, sound, decal, size, blend). A null string is nullptr.
MaterialProperty make_material_property(const char* effect, std::int32_t bank, const char* sound,
                                        const char* decal, float size, std::uint8_t blend);
// 00128e80: the ctype-table case-insensitive compare (A-Z fold to a-z).
int material_stricmp(const char* a, const char* b);

inline constexpr std::size_t material_property_count = 5;
inline constexpr std::size_t material_row_limit = 0x80;

struct MaterialRow {
    std::string name;
    std::array<MaterialProperty, material_property_count> props;
};

class MaterialPropertyTable {
public:
    // 00409020: the map, the vector and row 0 "DEFAULT" with five defaults.
    void initialise();
    bool initialised() const { return initialised_; }
    // 004085a0(table, name, matprop, prop): initialises when needed; an existing
    // name has props[matprop] replaced, a new one (while fewer than 0x80 rows)
    // gets a row of defaults with props[matprop] set. False when full.
    bool add(const std::string& name, std::size_t matprop, const MaterialProperty& prop);
    // 00408470(table, name): 0 when not initialised, null, empty or unknown;
    // otherwise the row index (case-sensitive match).
    std::uint8_t lookup(const char* name) const;
    // 00189560: rows[index] (the original does not check the index).
    const MaterialRow& row(std::size_t index) const;
    const std::vector<MaterialRow>& rows() const { return rows_; }
    // 004089f0(table, level file), at level load after the PLR and NIE loads
    // (initialising first when needed): for every row and property in order,
    // a non-empty effect name gets +c = 00398070(the particle library, name)
    // (request) and is cleared; a non-empty decal name takes the id of an
    // equal name seen earlier in this call (a local std::map, case-sensitive),
    // otherwise +2c = 00380a40(*00539248, the name in the level file, with
    // .tdx appended unless strstr finds it) (texture, not checked), and is
    // cleared.
    void resolve_effects(const std::function<std::int32_t(const std::string&)>& request,
                         const std::function<std::int32_t(const std::string&)>& texture);

private:
    bool initialised_{};
    std::vector<MaterialRow> rows_;
    std::map<std::string, std::uint8_t> index_;
};

// The COL v>=3 surface fixup 003c20c8: the table index for a raw 16-byte COL
// surface record, by the NUL-terminated name at bytes 2..15 (throws without a NUL).
std::uint8_t material_surface_index(const MaterialPropertyTable& table, std::span<const std::uint8_t> surface);

// init::preload on its own script context: every game::AddMaterialProperties
// (00267620 / 002675b0 / 00267540 by argument count -> 00267690 -> 004095a0 +
// 004085a0). Any other native throws naming it.
MaterialPropertyTable run_material_preload(const std::map<std::string, Script>& modules);

}
