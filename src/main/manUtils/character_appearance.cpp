#include "character_appearance.hpp"
#include "vu_float.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
using Quad = std::array<std::uint8_t, 16>;
constexpr std::size_t pairs_at = 0x40, pair_stride = 0x20, capacity = AppearancePacking::capacity;
[[noreturn]] void fail(const char* what) { throw std::runtime_error(std::string("Character appearance: ") + what); }

// Static-initialized cache fields of 0055dda0 (boot image): pattern +0 "1belt_BLA_BLAN_",
// pattern +10 "1belt_BLA_NONE_", masks +20 (byte 9), +40.. as below.
bool in_type_slot(std::size_t i) { return i >= 1 && i <= 5; }       // +a0
bool in_identity(std::size_t i) { return i != 14; }                 // +80

std::uint8_t byte_at(const AppearancePacking& s, std::size_t at) {
    if (at >= capacity) fail("read outside the 0x440-byte manifest");
    if (!s.initialized[at]) fail("reads a manifest byte the packer did not write");
    return s.bytes[at];
}
Quad quad(const AppearancePacking& s, std::size_t at) {
    Quad q{};
    for (std::size_t i = 0; i < 16; ++i) q[i] = byte_at(s, at + i);
    return q;
}
void put(AppearancePacking& s, std::size_t at, const Quad& q) {
    if (at + 16 > capacity) fail("pair table grows past 0x440 bytes");
    for (std::size_t i = 0; i < 16; ++i) { s.bytes[at + i] = q[i]; s.initialized[at + i] = true; }
}
bool nonzero(const Quad& q) { return std::any_of(q.begin(), q.end(), [](std::uint8_t b) { return b != 0; }); }
// Bytes 6..8 (mask +60) equal those of pattern +0: "BLA".
bool blank_set(const Quad& q) { return q[6] == 'B' && q[7] == 'L' && q[8] == 'A'; }
std::size_t first_of(std::size_t pair) { return pairs_at + pair * pair_stride; }
std::size_t second_of(std::size_t pair) { return first_of(pair) + 16; }
std::string c_string(const Quad& q) {
    std::string s;
    for (const auto b : q) { if (!b) return s; s.push_back(static_cast<char>(b)); }
    fail("16-byte name has no terminator");
}
Quad to_quad(std::string_view text) {
    if (text.size() > 15) fail("name longer than 15 characters");
    Quad q{};
    std::copy(text.begin(), text.end(), q.begin());
    return q;
}
// 1298b8 strncmp(a, b, n) == 0 over NUL-terminated byte strings.
bool prefix_equal(const std::uint8_t* a, const char* b, std::size_t n) {
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != static_cast<std::uint8_t>(b[i])) return false;
        if (!a[i]) return true;
    }
    return true;
}
bool is_digit(std::uint8_t b) { return b >= '0' && b <= '9'; }

// 00406750 (inlined twice in 00406950): the first digit at byte 9 of a pair's names.
char tone_digit(const AppearancePacking& s) {
    for (std::size_t k = 0; k < 32; ++k) {
        const auto first = first_of(k);
        if (const auto b = byte_at(s, first + 9); is_digit(b)) return static_cast<char>(b);
        if (const auto b = byte_at(s, first + 0x19); is_digit(b)) return static_cast<char>(b);
        if (!byte_at(s, first + 0x10)) return '_';
    }
    return '_';
}
// 00406dc0: pairs until a zero 128-bit first name.
std::size_t pair_count(const AppearancePacking& s) {
    std::size_t n = 0;
    while (first_of(n) + 16 <= capacity && nonzero(quad(s, first_of(n)))) ++n;
    if (first_of(n) + 16 > capacity) fail("pair table has no terminator");
    return n;
}
// 004067e0: 2 for a "BODY" part; 1 / 0 for the first '1' / '2' first name; at the
// terminator 1 when it is pair 0, else 2.
int body_kind(const AppearancePacking& s) {
    for (std::size_t k = 0; k < 32; ++k) {
        const auto q = quad(s, first_of(k));
        if (prefix_equal(q.data() + 1, "BODY", 4)) return 2;
        if (q[0] == '1') return 1;
        if (q[0] == '2') return 0;
        if (!q[0]) return k == 0 ? 1 : 2;
    }
    return 2;
}
// 00406e00: delete pairs with "BLA" in either name, shifting the rest down up to
// and including the first pair whose texture slot is zero.
void remove_blank_pairs(AppearancePacking& s) {
    std::size_t k = 0;
    while (nonzero(quad(s, first_of(k)))) {
        if (!blank_set(quad(s, first_of(k))) && !blank_set(quad(s, second_of(k)))) { ++k; continue; }
        if (!nonzero(quad(s, second_of(k)))) fail("00406e00 would not terminate (blank pair without texture)");
        std::size_t at = k;
        do {
            put(s, first_of(at), quad(s, first_of(at + 1)));
            put(s, second_of(at), quad(s, second_of(at + 1)));
            ++at;
        } while (nonzero(quad(s, second_of(at))));
    }
}
}  // namespace
void appearance_remove_blank_pairs(AppearancePacking& s) { remove_blank_pairs(s); }
namespace {
// 00406950 for a non-PC_MALE name with serial >= 0.
std::optional<std::size_t> merge_variant(AppearancePacking& s, std::int32_t serial, std::size_t count,
                                         const std::string& name, std::span<const AppearanceRegistryEntry> registry) {
    if (registry.empty()) return std::nullopt;
    auto starts = [&](const AppearanceRegistryEntry& e) { return e.key.compare(0, name.size(), name) == 0; };
    std::size_t first = 0;
    while (first < registry.size() && !starts(registry[first])) ++first;
    if (first == registry.size()) return std::nullopt;
    std::size_t end = first + 1;
    while (end < registry.size() && starts(registry[end])) ++end;
    if (end - first == 1) return std::nullopt;
    const auto chosen = first + static_cast<std::size_t>(serial) % (end - first);
    const auto& v = registry[chosen].packing;
    const char digit = tone_digit(v);
    if (digit != '_' && digit != tone_digit(s)) {
        for (std::size_t k = 0; nonzero(quad(s, second_of(k))); ++k) {
            auto second = quad(s, second_of(k));
            if (second[9] == '_') continue;
            second[9] = static_cast<std::uint8_t>(digit);
            put(s, second_of(k), second);
        }
    }
    put(s, 0x00, quad(v, 0x00));
    put(s, 0x10, quad(v, 0x10));
    std::size_t n = count, j = 0;
    for (; nonzero(quad(v, first_of(j))); ++j) {
        const auto vf = quad(v, first_of(j));
        std::size_t t = 0;
        for (; t < n; ++t) {
            const auto f = quad(s, first_of(t));
            bool same = true;
            for (std::size_t i = 0; i < 16; ++i) if (in_type_slot(i) && f[i] != vf[i]) same = false;
            if (same) break;
        }
        if (t == n) {
            put(s, first_of(n + 2), quad(s, first_of(n + 1)));
            put(s, second_of(n + 2), quad(s, second_of(n + 1)));
            put(s, first_of(n + 1), quad(s, first_of(n)));
            put(s, second_of(n + 1), quad(s, second_of(n)));
            ++n;
        }
        if (!blank_set(quad(s, first_of(t)))) {
            put(s, second_of(t), quad(v, second_of(j)));
            put(s, first_of(t), vf);
        }
    }
    if (const auto tail = quad(v, second_of(j)); nonzero(tail)) put(s, second_of(n), tail);
    return chosen;
}
AppearanceMeshRequest mesh_request(const Quad& q) {
    AppearanceMeshRequest r{c_string(q), blank_set(q), {}};
    if (r.blank) return r;
    for (std::size_t lod = 0; lod < 2; ++lod) {
        auto key = q;
        key[14] = static_cast<std::uint8_t>('0' + lod);
        std::string member;
        for (const auto b : key) { if (!b) break; member.push_back(static_cast<char>(b)); }
        r.members[lod] = member + ".SKM";
    }
    return r;
}
AppearanceTextureRequest texture_request(const Quad& q) {
    AppearanceTextureRequest r{c_string(q), false, {}};
    r.blank = blank_set(q) && !prefix_equal(q.data(), "_MAKE", 5);
    if (r.blank) return r;
    const auto formatted = r.name + ".TDX";
    if (formatted.size() == 14) fail("texture name of 10 characters (00403a20 overwrites its terminator)");
    for (std::size_t lod = 0; lod < 2; ++lod) {
        r.members[lod] = formatted;
        if (formatted.size() > 14) r.members[lod][14] = static_cast<char>('0' + lod);
    }
    return r;
}
std::string slot_string(const AppearancePacking& s, std::size_t at) { return c_string(quad(s, at)); }
// A 16-byte move that carries unwritten bytes as unwritten (00390940's shifts copy whatever lies past the terminator).
void move_quad(AppearancePacking& s, std::size_t to, std::size_t from) {
    if (to + 16 > capacity || from + 16 > capacity) fail("pair table grows past 0x440 bytes");
    for (std::size_t i = 0; i < 16; ++i) { s.bytes[to + i] = s.bytes[from + i]; s.initialized[to + i] = s.initialized[from + i]; }
}
// 004068b0(packing, key): the first pair whose first name matches `key` at bytes 1..5; at the
// terminator its texture name is tested the same way (index = pair count); else -1.
std::int32_t find_part(const AppearancePacking& s, const char* key) {
    std::size_t k = 0;
    for (;; ++k) {
        const auto f = quad(s, first_of(k));
        if (!nonzero(f)) break;
        if (prefix_equal(f.data() + 1, key, 5)) return static_cast<std::int32_t>(k);
    }
    if (prefix_equal(quad(s, second_of(k)).data() + 1, key, 5)) return static_cast<std::int32_t>(k);
    return -1;
}
// 129218 strcmp == 0 over two 16-byte slots.
bool same_string(const Quad& a, const Quad& b) {
    for (std::size_t i = 0; i < 16; ++i) { if (a[i] != b[i]) return false; if (!a[i]) return true; }
    fail("16-byte name has no terminator");
}
}

AppearanceMeshRequest appearance_mesh_request(std::string_view name) { return mesh_request(to_quad(name)); }
AppearanceTextureRequest appearance_texture_request(std::string_view name) { return texture_request(to_quad(name)); }

std::uint8_t appearance_shape_table(const AppearancePacking& packing) { return static_cast<std::uint8_t>(body_kind(packing)); }
std::array<std::uint8_t, 16> appearance_shape_key(const AppearancePacking& packing) { return quad(packing, 0x10); }
std::optional<std::uint8_t> actor_gender_3260(std::string_view skeleton) {
    if (skeleton == "MALE.SKEL") return std::uint8_t{2};      // 1b34a8 (strcmp 0x49e830)
    if (skeleton == "FEMALE.SKEL") return std::uint8_t{1};    // 1b34cc (strcmp 0x49e840)
    return std::nullopt;
}

namespace { void resolve_parts(CharacterAppearance& out); }

std::string appearance_registry_key(std::string_view member_name) {
    const auto end = std::min(member_name.find('.'), member_name.find('\0'));
    const auto stem = member_name.substr(0, end);
    if (stem.size() >= 0x1f) fail("registry key of 31 or more characters is unterminated");
    return std::string(stem);
}

CharacterAppearance resolve_character_appearance(std::string_view name, std::int32_t serial,
                                                 std::span<const AppearanceRegistryEntry> registry) {
    CharacterAppearance out;
    out.name = appearance_registry_key(name);
    for (auto& c : out.name) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
    const auto base = std::find_if(registry.begin(), registry.end(),
                                   [&](const AppearanceRegistryEntry& e) { return e.key == out.name; });
    if (base == registry.end()) fail("manifest is not in the mission registry (COMMON/SKL.HOG fallback not supported)");
    auto& s = out.state;
    s = base->packing;
    out.tone_digit = tone_digit(s);                                          // +2c0d
    if (serial < 0) remove_blank_pairs(s);
    else {
        const auto count = pair_count(s);
        std::string upper(name);
        for (auto& c : upper) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - ('a' - 'A'));
        if (upper == "PC_MALE.SKL") {                                        // 128e80 stricmp
            const auto a = quad(s, first_of(count)), b = quad(s, second_of(count));
            const auto c = quad(s, first_of(count + 1)), d = quad(s, second_of(count + 1));
            put(s, first_of(0), a); put(s, second_of(0), b);
            put(s, first_of(1), c); put(s, second_of(1), d);
        } else out.variant = merge_variant(s, serial, count, out.name, registry);
    }
    resolve_parts(out);
    return out;
}

CharacterAppearance resolve_character_appearance_custom(const AppearancePacking& packing) {
    CharacterAppearance out;
    out.name = "CUSTOM";                                                     // D_004bc398 (String_Copy)
    out.state = packing;                                                     // 001b7150: model +40
    out.tone_digit = tone_digit(out.state);                                  // +2c0d
    remove_blank_pairs(out.state);                                           // serial < 0
    resolve_parts(out);
    return out;
}

namespace {
void resolve_parts(CharacterAppearance& out) {
    auto& s = out.state;
    // 0038dc90: textures for every pair with a nonzero texture slot.
    std::vector<AppearanceTextureRequest> textures;
    for (std::size_t k = 0; second_of(k) + 16 <= capacity && nonzero(quad(s, second_of(k))); ++k)
        textures.push_back(texture_request(quad(s, second_of(k))));
    // 00405940(state, 1, name): slot 2 must be a HARD part, then every pair's mesh.
    auto hard = quad(s, 0x20);
    if (!prefix_equal(hard.data() + 1, "HARD", 4)) {
        hard = to_quad("_HARD_DEF_DEFA0");
        hard[0] = body_kind(s) ? '1' : '2';
        put(s, 0x20, hard);
    }
    out.hard_mesh = mesh_request(hard);
    const auto meshes = pair_count(s);
    if (textures.size() < meshes) fail("a part has no texture slot");
    for (std::size_t k = 0; k < meshes; ++k)
        out.parts.push_back({mesh_request(quad(s, first_of(k))), textures[k]});
    if (textures.size() > meshes + 1) fail("textures continue past the terminator pair");
    if (textures.size() > meshes) out.extra_texture = textures[meshes];
    out.bone_scale_preset = slot_string(s, 0x00);
    out.shape = slot_string(s, 0x10);
    out.skeleton = slot_string(s, 0x30);
    out.gender_3260 = actor_gender_3260(out.skeleton);
}
}

namespace {
void copy_manifest(AppearancePacking& to, const AppearancePacking& from) {   // the 0x44 quadword copy loops
    to.bytes = from.bytes;
    to.initialized = from.initialized;
}
}

BodySwitch switch_body_packing(AppearancePacking& model, AppearancePacking& costume,
                               std::optional<AppearancePacking>& backup) {
    BodySwitch out;
    if (backup) copy_manifest(model, *backup);                                // 003909b0
    else { backup = model; out.backup_saved = true; }                         // 003909e0 Mem_Alloc(0, 0x440)
    const int model_kind = body_kind(model);                                  // +2c0c
    std::size_t count = pair_count(model);                                    // +23c0
    if (body_kind(costume) != model_kind) {                                   // 00390ab0
        const std::uint8_t from = model_kind == 1 ? '2' : '1', to = model_kind == 1 ? '1' : '2';
        const auto convert = [&](std::size_t at) { if (byte_at(costume, at) == from) costume.bytes[at] = to; };   // mask +30
        for (std::size_t k = 0; nonzero(quad(costume, first_of(k))); ++k) { convert(first_of(k)); convert(second_of(k)); }
        convert(0x20);
    }
    if (const auto hair = find_part(model, "HAIR_"); hair != -1) {            // 004bc3c8
        const auto model_hair = first_of(static_cast<std::size_t>(hair));
        if (const auto v = find_part(costume, "HAIR_"); v != -1)
            put(model, model_hair, quad(costume, first_of(static_cast<std::size_t>(v))));
        else if (const auto costume_hat = find_part(costume, "HAT-_"); costume_hat != -1) {   // 004bc3d0
            auto model_hat = find_part(model, "HAT-_");
            const bool hat_blank = prefix_equal(quad(costume, first_of(static_cast<std::size_t>(costume_hat))).data() + 6, "BLA", 3);
            auto q = quad(model, model_hair);
            if (model_hat != -1 && prefix_equal(quad(model, first_of(static_cast<std::size_t>(model_hat))).data() + 0xa, "HAIR", 4))
                model_hat = -1;
            const char* rename = model_hat == -1 && !hat_blank ? "HAT-" : model_hat != -1 && hat_blank ? "HAIR" : nullptr;
            if (rename) {                                                     // String_Copy(q + 0xa, ...): 4 bytes and NUL
                std::copy(rename, rename + 5, q.begin() + 0xa);
                out.cache_lookups.push_back(c_string(q));
                put(model, model_hair, q);
            }
        }
    }
    if (const auto hard = quad(costume, 0x20); !same_string(quad(model, 0x20), hard)) {   // 00390e18
        out.cache_lookups.push_back(c_string(hard));
        put(model, 0x20, hard);
    }
    std::size_t j = 0;                                                        // 00390e78
    for (; nonzero(quad(costume, first_of(j))); ++j) {
        const auto cf = quad(costume, first_of(j));
        std::size_t t = 0;
        for (; t < count; ++t) {
            const auto f = quad(model, first_of(t));
            bool same = true;
            for (std::size_t i = 0; i < 16; ++i) if (in_type_slot(i) && f[i] != cf[i]) same = false;
            if (same) break;
        }
        if (t == count) {
            move_quad(model, first_of(count + 2), first_of(count + 1));
            move_quad(model, second_of(count + 2), second_of(count + 1));
            move_quad(model, first_of(count + 1), first_of(count));
            move_quad(model, second_of(count + 1), second_of(count));
            ++count;
        }
        put(model, second_of(t), quad(costume, second_of(j)));
        put(model, first_of(t), cf);
    }
    if (const auto tail = quad(costume, second_of(j)); nonzero(tail)) put(model, second_of(count), tail);
    remove_blank_pairs(model);                                                // 00390fc0: 00406e00, 00406dc0
    pair_count(model);
    return out;
}

bool restore_body_packing(AppearancePacking& model, std::optional<AppearancePacking>& backup) {
    if (!backup) return false;                                                // 00391210: +2c14 == 0
    copy_manifest(model, *backup);
    backup.reset();
    return true;
}

CharacterAppearance appearance_of_packing(std::string name, const AppearancePacking& s) {
    CharacterAppearance out;
    out.name = std::move(name);
    out.state = s;
    std::vector<AppearanceTextureRequest> textures;                           // 0038e030
    for (std::size_t k = 0; second_of(k) + 16 <= capacity && nonzero(quad(s, second_of(k))); ++k)
        textures.push_back(texture_request(quad(s, second_of(k))));
    out.hard_mesh = mesh_request(quad(s, 0x20));
    const auto meshes = pair_count(s);                                        // 0038c920
    if (textures.size() < meshes) fail("a part has no texture slot");
    for (std::size_t k = 0; k < meshes; ++k) out.parts.push_back({mesh_request(quad(s, first_of(k))), textures[k]});
    if (textures.size() > meshes + 1) fail("textures continue past the terminator pair");
    if (textures.size() > meshes) out.extra_texture = textures[meshes];
    out.bone_scale_preset = slot_string(s, 0x00);
    out.shape = slot_string(s, 0x10);
    out.skeleton = slot_string(s, 0x30);
    out.gender_3260 = actor_gender_3260(out.skeleton);
    return out;
}

std::optional<std::size_t> appearance_preregistered_slot(const AppearanceMeshRequest& request,
                                                         std::string_view member_name) {
    const auto dot = member_name.rfind('.');
    if (dot == std::string_view::npos || member_name.substr(dot) != ".SKM") return std::nullopt;
    const auto end = std::min(member_name.find('.'), member_name.find('\0'));
    const auto stem = member_name.substr(0, std::min<std::size_t>(end, 15));
    Quad key{}, name = to_quad(request.name);
    std::copy(stem.begin(), stem.end(), key.begin());
    for (std::size_t i = 0; i < 16; ++i) if (in_identity(i) && key[i] != name[i]) return std::nullopt;
    const int slot = key[14] - '0';
    return static_cast<std::size_t>(slot == 1 ? 1 : 0);
}

std::optional<std::size_t> appearance_preregistered_texture_slot(const AppearanceTextureRequest& request,
                                                                 std::string_view member_name) {
    const auto end = std::min(member_name.find('.'), member_name.find('\0'));
    const auto stem = member_name.substr(0, std::min<std::size_t>(end, 15));
    Quad key{}, name = to_quad(request.name);
    std::copy(stem.begin(), stem.end(), key.begin());
    key[14] = '0';
    for (std::size_t i = 0; i < 16; ++i) if (in_identity(i) && key[i] != name[i]) return std::nullopt;
    if (member_name.size() < 14) fail("00403d20 reads byte 14 past a short member name");
    return static_cast<std::size_t>(member_name.size() > 14 && member_name[14] == '0' ? 0 : 1);
}

bool appearance_mouth_mesh(std::string_view mesh_name) {
    // 004068b0 compares 5 bytes from byte 1 (strncmp(name + 1, "MOUT_", 5)).
    return mesh_name.size() >= 6 && mesh_name.substr(1, 5) == "MOUT_";
}

bool appearance_strap_mesh(std::string_view mesh_name) {
    // 004068b0 compares 5 bytes from byte 1 (strncmp(name + 1, "STRA_", 5)).
    return mesh_name.size() >= 6 && mesh_name.substr(1, 5) == "STRA_";
}

ActorDrawLod actor_draw_lod(const std::array<float, 3>& view_root, float half_fov, std::int32_t gore_counter, bool forced) {
    if (forced) return {std::nullopt, 1};
    const float x2 = view_root[0] * view_root[0], y2 = view_root[1] * view_root[1], z2 = view_root[2] * view_root[2];
    const float sum = x2 + y2;
    const float distance = vu::sqrt(sum + z2);
    const float scaled = vu::div(distance * half_fov, 12.0F);
    std::int32_t lod = scaled >= 2147483647.0F ? std::numeric_limits<std::int32_t>::max()
                     : scaled <= -2147483648.0F ? std::numeric_limits<std::int32_t>::min()
                                                : static_cast<std::int32_t>(scaled);
    if (lod >= 2) lod = 1;
    if (gore_counter != 0) lod = 0;
    return {distance, lod};
}
}
