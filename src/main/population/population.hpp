#pragma once

#include "geometry.hpp"
#include <span>

namespace omega {
struct PopulationSection {
    std::string tag;
    std::size_t offset{}, end{};
    std::uint32_t count{};
    bool decoded{};
};
struct TerrainDefinition { std::uint32_t id{}; std::string name; };
struct TerrainLink { std::uint32_t id{}, adjacency{}; Bounds bounds; };
struct PopulationPlacement {
    std::uint32_t id{};
    std::array<std::uint32_t, 2> parent{};
    std::string name;
    Vec3 position;
    std::array<float, 9> basis{}; // Retained in file order; not yet applied to actors.
};
struct PopulationMotion {
    std::uint32_t mode{}, parameter{};
    float speed{}; // Raw file value; no runtime scaling or interpolation applied.
    std::vector<Vec3> points;
    std::vector<std::array<float, 4>> rotations;
};
struct PopulationObject {
    std::uint32_t kind{}, id{};
    std::string asset, script;
    Vec3 position;
    std::array<float, 9> basis{};
    // Keep the two stored words, including any original sentinel values.
    std::vector<std::array<std::uint32_t, 2>> associations, references, extra_references;
    PopulationMotion motion; // Present only for kinds 2 and 3.
};
struct PopulationNpc {
    std::uint32_t id{};
    std::string model, extra_name;
    std::array<std::uint32_t, 2> parameters{}, trailing_parameters{};
    Vec3 position;
    std::array<float, 9> basis{};
    std::int32_t node_index{-1};
    // PORT-ONLY (checkpoint 132md16, the mods' data_overrides.hpp): where parameters[0] lies in the file (the health
    // word; armor follows), so the mod layer can patch the two words. Not read by the game.
    std::size_t parameters_offset{};
};
struct PopulationNode {
    std::uint32_t id{}, flags{}, parameter{};
    std::int32_t parent_index{-1};
    Vec3 position;
    std::array<float, 9> basis{};
    std::vector<std::array<std::uint32_t, 2>> links;
};
// One BOX: record (a hotbox; CROUCH_ZONES_AND_RUNNING_JUMP.md). 001c0c00 reads
// the section (count, then per record the type word for version >= 0x36) and
// 00172ae0 the rest. Words are kept as stored.
struct PopulationHotbox {
    std::uint32_t type{};                 // +0x90, the message code (read after the associations below 0x36)
    std::uint32_t id_category{},id_index{};
    std::uint32_t id{};                   // +0x0c/+0x10: (category << 16) | index (32-bit, as 00172ae0)
    // (a, b) pairs: a > 0 (signed) associates the box with object (a, b)
    // through 00173d60; (-1, -1) starts the box disabled (+2c = 0); other a <= 0
    // are ignored.
    std::vector<std::array<std::uint32_t, 2>> associations;
    std::uint32_t parameter{};            // +0x94
    std::array<float, 3> minimum{}, maximum{}; // +0x70/+0x80 (w = 1); local when transformed
    // Version >= 0x3d: basis rows X, Y, Z then the translation. When the rows
    // are exactly the identity (c.eq.s, so -0 counts; the translation is not
    // tested) the box has no transform component and min/max are world.
    std::array<float, 12> basis{1,0,0, 0,1,0, 0,0,1, 0,0,0};
    bool transformed{};                   // +0x50 created by virtual +0x90 (003cdff0)
    std::vector<std::array<std::uint32_t, 2>> links; // resolved by 0040c480 into the +0x60 link list
    bool starts_disabled() const;         // a (-1, -1) association
};
// Decodes the records of a BOX: section body (after the tag and count words)
// for the given POP version. Every count is bounded by the bytes, the
// association count by 00172ae0's 256-entry stack buffers, the link word count
// must be even, coordinates must be finite and the records must end exactly at
// `bytes.size()`.
std::vector<PopulationHotbox> decode_population_hotboxes(std::span<const std::uint8_t> bytes,
                                                         std::uint32_t version, std::uint32_t count);
// One PNT: record as 00179620 reads it (LEVEL_LIGHTS.md section 2.2): 88 bytes
// from version 0x3b, 80 below. Each byte field is followed by padding to a
// 4-byte boundary, which the reader skips unread. Words the reader skips are
// kept as stored.
struct PopulationPointLight {
    std::uint32_t id_category{},id_index{};      // object +0xc / light +0x8 = (category << 16) | index (32-bit)
    std::array<std::uint32_t,2> skipped{};       // bytes 8..15, not read
    std::uint32_t unused{};                      // bytes 16..19 (a float slot copied but never used)
    std::array<float,3> position{},colour{};     // light +0x30 (w = 1), +0x20 (w = 0)
    float inner{},outer{};                       // light +0x10, +0x14
    std::array<float,3> direction{};             // light +0x40 (w = 0)
    float cone_angle_outer{},cone_angle_inner{}; // degrees: light +0x50/+0x54 = cosf(k*(0.5*a)), k = pi/180
    std::uint8_t enable{};                       // nonzero: intensity +0xc = the file value, else 0
    std::uint8_t aux_08{},aux_10{};              // version >= 0x3b: nonzero sets aux (+0x1d) bit 0x08 / 0x10
    float intensity{};
    std::uint32_t id() const { return (id_category << 16) | id_index; }
};
// One DIR: record as 00179240 reads it (44 bytes).
struct PopulationDirectionalLight {
    std::uint32_t id_category{},id_index{};      // object +0xc (the light keeps its pool id)
    std::uint32_t unused{};                      // bytes 8..11, not used
    std::array<float,3> direction{},colour{};    // light +0x40 (w = 0), +0x20 (w = 0)
    std::uint8_t enable{};                       // nonzero: intensity +0xc = the file value, else 0
    float intensity{};
    std::uint32_t id() const { return (id_category << 16) | id_index; }
};
// One ENV: record as 00178630 reads it: 76 bytes from version 0x3a, 72 from
// 0x37, 68 below.
struct PopulationEnvironment {
    std::uint32_t id_category{},id_index{};      // object +0xc
    // Environment +0x70, +0x74, +0x78, +0x68, +0x6c, +0x60, +0x64, +0x84, +0x88,
    // +0x8c in file order. +0x64 is the camera far term (00132ff0..0013300c),
    // +0x84..+0x8c the ambient light colour.
    std::array<float,10> values{};
    // Object indices resolved by 0040c480(id_category, word) into +0x90..+0x9c;
    // 001781a0 activates them (vcall +0x5c) when the environment becomes current.
    std::array<std::uint32_t,4> references{};
    std::uint8_t fog_mode{};                     // +0x7c (00132eb4: mode bit 0x100 when set, else 0x80)
    float value_80{};                            // +0x80: from the file from version 0x37, else 0.45 (0x3ee66666)
    std::uint8_t search{};                       // version >= 0x3a: the byte; below 0x3a the reader uses 1
    std::uint32_t id() const { return (id_category << 16) | id_index; }
    float far_term() const { return values[6]; } // +0x64
};
// Decoders of the PNT:/DIR:/ENV: section bodies (after the tag and count
// words) for the given POP version: counts are bounded by the bytes (a
// negative count throws), floats the loaders use must be
// finite and the records must end exactly at `bytes.size()`.
std::vector<PopulationPointLight> decode_population_point_lights(std::span<const std::uint8_t> bytes,
                                                                 std::uint32_t version, std::uint32_t count);
std::vector<PopulationDirectionalLight> decode_population_directional_lights(std::span<const std::uint8_t> bytes,
                                                                             std::uint32_t version, std::uint32_t count);
std::vector<PopulationEnvironment> decode_population_environments(std::span<const std::uint8_t> bytes,
                                                                  std::uint32_t version, std::uint32_t count);
// One GEN: record (a cGenerator) as 00221d20 reads it (NPC_ACTIVATION_AND_RESPAWN.md).
// Byte fields are followed by padding to 4 bytes; meanings marked inferred
// there are kept as stored words here.
struct PopulationGenerator {
    std::uint32_t id{};                          // (category << 16) | index
    std::string manifest;                        // +0x64, appearance of the spawned NPCs
    std::uint32_t health{};                      // +0x68 (inferred)
    Vec3 position;
    std::array<float, 9> basis{};
    std::array<std::uint32_t, 2> items{};        // +0x6c, +0x70 (inferred)
    std::vector<std::uint32_t> spawn_nodes;      // node IDs resolved into +0x7c
    std::uint32_t alive_cap{}, total{};          // +0x88, +0x8c
    float interval{};                            // +0x84, seconds
    std::uint8_t flag_d2{};                      // +0xd2 (version >= 0x3e)
    std::uint8_t enabled{};                      // +0x2c at load
    std::int32_t parent_region{-1};              // index in the same category, or -1 (found from the position)
    std::string name;                            // +0xa8
};
// One INV: record (a Backpack) as 00257f30 reads it: 84 bytes from version
// 0x39. Each slot is (item, copies, amount, byte) and is added through
// 00255af0 when the item is >= 0; the byte is padded to 4 bytes. Slot meanings
// beyond that are not claimed.
struct PopulationBackpack {
    std::uint32_t id{};                          // +0x0c: (category << 16) | index
    Vec3 position;
    std::array<std::array<std::uint32_t, 4>, 3> slots{};
    std::array<std::uint32_t, 2> pair_2e4{};     // second word > 0 and first nonzero: +0x2e4..+0x2ec
    std::array<std::uint32_t, 2> owner{};        // (category, index) resolved by 0040c480 and associated by 00173d60; -1 none
};
// Decoders of the GEN:/INV: section bodies for the given POP version; the
// records must end exactly at `bytes.size()`.
std::vector<PopulationGenerator> decode_population_generators(std::span<const std::uint8_t> bytes,
                                                              std::uint32_t version, std::uint32_t count);
std::vector<PopulationBackpack> decode_population_backpacks(std::span<const std::uint8_t> bytes,
                                                            std::uint32_t version, std::uint32_t count);
// One FIR: record (a FireObj, 0xa0 bytes) as 002625b0 reads it. The loader
// 001c0b00 has no version branches. The object stores the centre (+0x70) and
// half extent (+0x80) of the two points, and int(value * [0048b0e0] / 100.0)
// at +0x60 ([0048b0e0] is initialised to 1000). Each link with neither word
// -1 is resolved by 0040c480 and associated by 00173d60(object, fire, 1, 0).
struct PopulationFire {
    std::uint32_t id{};                          // +0x0c: (category << 16) | index
    Vec3 first, second;
    float value{};
    std::vector<std::array<std::uint32_t, 2>> links;
};
// Decoder of the FIR: section body; the records must end at `bytes.size()`.
std::vector<PopulationFire> decode_population_fires(std::span<const std::uint8_t> bytes, std::uint32_t count);
// Checkpoint 132ms2: CAM: records (loader 001c0990: per record 0036b180(0xcf0), the stream
// constructor 0017e3f0, 0040c9d0). 0017e990..0017eac4 read 14 words: the key (w0 << 16) | w1
// (+c), the position, then rows 0..2; 00131e90 / 00131e70 build the matrix (row 3 = the
// position) that 003cdff0 sets as the camera's transform; +60 = 0.5 * 0x3f91361e (the 65
// degree field of view's half angle). GameCamera in the scripts (CAMERA.md).
struct PopulationCamera {
    std::uint32_t id{};
    Vec3 position;
    std::array<Vec3, 3> rows{};
};
std::vector<PopulationCamera> decode_population_cameras(std::span<const std::uint8_t> bytes, std::uint32_t count);
// A keyed track as the NIL:/NIE: constructors read it: a key count, then (when
// nonzero) a float stored as int(v - 1.0), then `width` floats per key.
struct PopulationTrack {
    std::size_t width{};
    float length{};              // only read when there are keys
    std::vector<float> values;   // keys * width, in stream order
    std::size_t keys() const { return width ? values.size() / width : 0; }
};
// One NIL: record (an NIEventLight, 0x1b0 bytes) as 00237f80 reads it for
// version >= 0x46. Field names give the object offsets; meanings are not claimed.
struct PopulationEventLight {
    std::uint32_t id{};                          // +0x0c
    std::array<PopulationTrack, 2> tracks;       // +0x170, +0x190 via 00239df0 (4 floats per key)
    std::array<float, 3> values_144{};           // +0x144..+0x14c
    std::uint8_t byte_151{}, byte_150{};         // version >= 0x42, else 0
    std::array<float, 5> values_160{};           // +0x160, +0x164, +0x168, +0x154, +0x158 (version >= 0x45, else 0)
    PopulationTrack keyframes;                   // +0xac, 10 floats per key (+0x1c of each set to 1.0)
    PopulationTrack switches;                    // +0xc4, (time, value) per key; the flag is value != 0
    std::vector<std::array<std::uint32_t, 2>> links; // each resolved by 0040c480, associated by 00173d60
};
// One NIE: event record (004a6fc0 dispatch in 00238a50): a float type, then
// 0/3 -> 1 float, 1 -> 5 floats, 2 -> 3 floats (002397f0), 4 -> 3 floats
// (00239610); other types read nothing.
struct PopulationEvent { std::int32_t type{}; std::vector<float> values; };
// One NIE: record (an NIEventOBJ, 0x1c0 bytes) as 0023c610 reads it.
struct PopulationEventObject {
    std::string group;                           // +0x64 (version >= 0x3f); shared by one event sequence
    std::uint32_t id{};                          // +0x0c
    std::uint32_t kind{};                        // +0x60; kinds 1..6 read `asset` (1/3/6: 0023c3c0, 2: 0023c1c0)
    std::string asset;
    PopulationTrack keyframes;                   // +0xac, 10 floats per key
    std::array<PopulationTrack, 3> tracks;       // +0x144, +0x164, +0x184 via 00239df0
    PopulationTrack switches;                    // +0xc4
    std::vector<PopulationEvent> events;         // +0x1a4 via 00238a50 (version >= 0x38)
    std::vector<std::array<std::uint32_t, 2>> links;    // 0040c480 + 00173d60
    std::vector<std::array<std::uint32_t, 2>> children; // NIEventOBJ/NIEventLight children get +0x94 = this
    std::uint8_t byte_130{};                     // version >= 0x44, else 0
};
std::vector<PopulationEventLight> decode_population_event_lights(std::span<const std::uint8_t> bytes,
                                                                 std::uint32_t version, std::uint32_t count);
std::vector<PopulationEventObject> decode_population_event_objects(std::span<const std::uint8_t> bytes,
                                                                   std::uint32_t version, std::uint32_t count);
// BUG: records are (category, index, name) with the name padded to 4 bytes.
// The retail executable never references the BUG: tag, so these names are for
// diagnostics only.
struct PopulationDebugName { std::uint32_t id{}; std::string name; };
// NIS: as 002390a0 reads it: per record a null-terminated name realigned to 4
// bytes, then (version >= 0x43) a category word. The first record's category
// is kept in 0048ae28 and every record of that category increments the base
// 004f7c80 (the index offset 002397f0 adds for owners of another category).
struct PopulationSoundNames {
    std::vector<std::string> names;
    std::vector<std::uint32_t> categories;
    std::int32_t first_category{-1};
    std::uint32_t base{};
};
PopulationSoundNames decode_population_sound_names(std::span<const std::uint8_t> bytes, std::uint32_t version,
                                                   std::uint32_t count);
std::vector<PopulationDebugName> decode_population_debug_names(std::span<const std::uint8_t> bytes, std::uint32_t count);
// One SND: record (a SoundGOBJ, class 0xb, vtable 004daf30) as the loader
// 001c3390 and the constructor 00212ee0 read it: the loader's flag word
// (+1f1 = flag != 1, the zone-driven autostart), the key words, the sound name
// and a script name (each null-terminated and realigned to 4 bytes), the
// position and 9 basis floats (Mtx_SetBasis / Mtx_SetTranslation, then
// virtual +90 = 003cdff0), the association pairs (copied raw into two
// 0x400-byte stack arrays), the byte +1f2 (realigned), and from version 0x35
// the sound type word (else 0), all passed to 001ca010(+60, type, name, obj, 0).
struct PopulationSoundObject {
    std::uint32_t flag{};                        // the loader's word: +1f1 = (flag != 1)
    std::uint32_t id{};                          // +0x0c
    std::string name;                            // +0x70 via 001ca010 (may be empty)
    std::string script;                          // non-empty: +18 script object, 003d9d30
    Vec3 position;
    std::array<float, 9> basis{};
    std::vector<std::array<std::uint32_t, 2>> associations; // 0040c480(a, b) -> 00173d60(found, obj, 1, found)
    std::uint8_t owned_1f2{};                    // 1: the object owns its sounds (positional)
    std::int32_t type{};                         // +0x60 (version >= 0x35, else 0)
    bool autostart() const { return flag != 1; }
};
std::vector<PopulationSoundObject> decode_population_sound_objects(std::span<const std::uint8_t> bytes,
                                                                   std::uint32_t version, std::uint32_t count);
struct Population {
    std::vector<PopulationSoundObject> sound_objects; // SND: (empty when absent)
    std::vector<PopulationFire> fires;           // FIR: (empty when absent)
    std::vector<PopulationCamera> cameras;       // CAM: (empty when absent)
    std::vector<PopulationEventLight> event_lights;   // NIL: (empty when absent)
    std::vector<PopulationEventObject> event_objects; // NIE: (empty when absent)
    std::vector<PopulationDebugName> debug_names; // BUG: (not read by the retail runtime)
    PopulationSoundNames sound_names;             // NIS: (empty when absent)
    std::vector<PopulationHotbox> hotboxes; // BOX: section, empty when the section is absent
    // PNT:/DIR:/ENV: sections (empty when absent; `sections` tells absent from empty).
    std::vector<PopulationPointLight> point_lights;
    std::vector<PopulationDirectionalLight> directional_lights;
    std::vector<PopulationEnvironment> environments;
    std::vector<PopulationGenerator> generators; // GEN: (empty when absent)
    std::vector<PopulationBackpack> backpacks;   // INV: (empty when absent)
    std::vector<TerrainDefinition> terrain;
    std::vector<std::vector<std::uint32_t>> adjacency;
    std::vector<TerrainLink> links;
    std::vector<PopulationPlacement> placements; // PLR particle effects, not player starts.
    std::vector<PopulationObject> objects;
    std::vector<PopulationNpc> npcs; // Loader treats the first record specially for the local player.
    std::vector<PopulationNode> nodes;
    std::vector<PopulationSection> sections;
};

Population decode_population(std::span<const std::uint8_t> bytes);
struct SpawnPose {
    std::uint32_t id{};
    Vec3 position;
    std::array<float, 9> basis{};
    bool node_override{};
};
SpawnPose npc_spawn_pose(const Population& population, std::size_t index = 0);
// Node +8c, the yaw the node loader 0015c390 derives from the record's basis:
// row 1 (basis[3..5]) with z = 0 is normalized (vmul, the ACC sum
// (x^2 + y^2) + 0, vrsqrt Q = 1/sqrt, a zero sum giving the interpreter's
// +FLT_MAX), a = acosf(y') (00121a88; the dot with (0, 1, 0) is y'), then
// a = pi + (pi - a) with pi = 0x40490fdb unless a is 0 or pi or x' < 0, and
// a = 0 when (double)a >= 6.2 (0011bd70 / 00100230). The checkpoint message
// (001b3b60) turns it into a z rotation. Throws where acosf does.
float population_node_yaw(const PopulationNode& node);
void write_population(std::ostream& output, const Population& population);
// Terrain vertices already use the model's world-space coordinates. No actor transform is inferred.
void append_terrain(Mesh& destination, CollisionMesh& collision,
                    const Mesh& source, const CollisionMesh& source_collision);
// Diagnostic stored pose only. Collision coefficients remain source metadata, not a physics contract.
// Nonsingular affine positions; inverse-transpose normals for diagnostic lighting only.
// Original winding and collision metadata are retained, including reflected placements.
void apply_stored_pose(Mesh& mesh, CollisionMesh& collision, const PopulationObject& object);
}
