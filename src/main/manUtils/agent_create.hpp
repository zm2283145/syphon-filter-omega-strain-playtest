#pragma once
// The new-agent appearance editor (GuiAgentCreate, class factory 0041f960, ctor 0028f760, vtable
// 004dd1b0, AGENTNEW.GUI; research/FRONTEND.md "Checkpoint 132cc5"): its four front-end models, the
// category table D_0048b500 and the select / step / randomize functions it runs over the part cache
// (agent_modify.hpp). The widget is gui_agent_create.cpp.
//
// The original's models are 0x3030-byte actor models (00393ff0); only the fields these functions read
// and write are kept: the packing (model +a0), the pair count (+2420), the sex (+2c6c), the tone
// (+2c6d), the bone-scale preset (+2c70) and the morph target (+2c20 / +2c22). The bone scales,
// morph weights (+2450 / +2454), part records and textures are draw state, not modelled.
#include "agent_camera.hpp"
#include "agent_modify.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace omega {

struct AgentCreateModel {
    AgentPacking packing{};          // +a0
    std::size_t pair_count{};        // +2420
    std::int32_t sex{};              // +2c6c (004067e0)
    std::int8_t tone{};              // +2c6d (00406750)
    std::int8_t preset{};            // +2c70 (ApplyScalePreset 003913d0)
    std::int32_t morph_index{};      // +2c22 (0038df00)
    bool morph_on{};                 // +2c20 (the target is not "SHAPE_BASE")
    AgentPartOrder order{};          // +2e10 / +2e30 (00390570)
};

// D_0048b500: 24 x {u8 type, s8 pair, u16 position}; [sex * 11 + category] for the 11 categories of
// D_0048b560 (entries 0..10 sex 0 / female, 11..21 sex 1 / male; 22, 23 type 0, pair -1).
struct AgentCreateEntry {
    std::uint8_t type{};
    std::int8_t pair{-1};
    std::uint16_t position{};
};
using AgentCreateTable = std::array<AgentCreateEntry, 24>;
AgentCreateTable agent_create_table_initial();   // the boot image's D_0048b500

// D_0048b560 (20-byte names), the event / select names of AGENTNEW's buttons.
inline constexpr std::array<const char*, 11> agent_create_categories{
    "gender", "randomize", "bodyshape", "skintone", "face", "faceshape", "hairstyle", "haircolor", "eyecolor",
    "makeup", "beard"};

// GuiAgentCreate's state: +60..+6c the models (0, 2 from the MALE.SKL packing 0055d510; 1, 3 from the
// FEMALE.SKL packing 0055d950), +70 the current one, +c0 the camera transition, +c4 count, +c8
// position, +d4 the gender toggle, +d8 sex, +dc tone, +e0 / +e4 the mesh / texture cache indices, +e8
// the morph target, +ec the head pair, +ed pair, +ee type, +ef category (11: none), +f0 the previous.
struct AgentCreate {
    std::array<AgentCreateModel, 4> models{};
    std::int32_t cur = 0;
    AgentCamera camera{};             // +80..+ac (Checkpoint 132cc6, agent_camera.hpp)
    std::int32_t transition = 0;      // +c0
    std::int32_t count = 0;           // +c4
    std::int32_t position = 0;        // +c8
    std::int32_t gender = 0;          // +d4
    std::int32_t sex = 0;             // +d8
    std::int8_t tone = 0;             // +dc
    std::int32_t mesh_index = 0;      // +e0
    std::int32_t texture_index = 0;   // +e4
    std::int32_t morph = 0;           // +e8
    std::int8_t head_pair = 0;        // +ec
    std::int8_t pair = 0;             // +ed
    std::uint8_t type = 0;            // +ee
    std::int8_t category = 11;        // +ef
    std::int8_t previous = 0;         // +f0
    AgentCreateTable table{};         // D_0048b500 (static in the original)
    std::string slot_text;            // "value_slot": "%d/%d" (D_004a9a30), "0/0" (D_004a9a38) or ""
    bool slider_shown = false;        // "selectindicator" flags bit 2
    std::int32_t slider_value = 0;    // 0041e0f0(this, +58, 0xa, value)
    std::uint32_t changes = 0;        // the step sound 0041e0f0(this, canvas, 0x10, 6)
    std::uint64_t random = 0;         // 00127dd0's generator state (D_00489738 +a8)
    // PORT-ONLY: stands in for T0_COUNT (0x10000000), the EE timer 0028cde0 seeds the generator with
    // (srand 00127db8). The widget advances it every frame.
    std::uint32_t timer = 0;

    AgentCreateModel& model() { return models[static_cast<std::size_t>(cur)]; }
    const AgentCreateModel& model() const { return models[static_cast<std::size_t>(cur)]; }
};

// 0028f760's model part, after Skl_Load (00402840) filled `male` / `female` (0055d510 / 0055d950):
// per model the pair count 00406dc0, the blank pairs 004061b0, sex / tone, the skeleton name
// ("FEMALE.SKEL" for sex 0, else "MALE.SKEL", strcpy to packing +30) and ApplyScalePreset(model, -1);
// then the table's pairs (the first pair 0..count whose texture bytes 1..4 equal D_0048b470's name;
// else the table's previous value), positions 0, the head pair, +ed / +ee from category 11 and the
// count 0028f220. Checkpoint 132cc6: 00390570 (the MANSORT2 ordering, agent_reorder_parts) runs after
// the blank pairs (0028fb28) and the camera preset is record 0 (002903fc).
AgentCreate agent_create_open(const AgentPartCache& c, const AgentPacking& male, const AgentPacking& female,
                              const AgentCreateTable& table);
void agent_create_count(const AgentPartCache& c, AgentCreate& a);                         // 0028f220
void agent_create_select(const AgentPartCache& c, AgentCreate& a, std::string_view name); // 0028d610
void agent_create_step(const AgentPartCache& c, AgentCreate& a, bool back, bool forward); // 0028c390
// 0028cb20(this, dir, step): with `step` (the "randomize" category's L1 / R1) the models +68 / +6c
// hold the previous / next look; without (the "randomize" button) both are generated afresh.
void agent_create_randomize(const AgentPartCache& c, AgentCreate& a, std::int32_t dir, bool step, std::uint32_t seed);
// 0028cde0(this, model, dir): model = current, then categories 2..10 stepped (Random_Next & 3) + 1 times.
void agent_create_random_look(const AgentPartCache& c, AgentCreate& a, std::size_t model, std::int32_t dir,
                              std::uint32_t seed);
void agent_create_copy(const AgentPartCache& c, AgentCreateModel& dst, const AgentCreateModel& src);  // 0028cd70

// 003913d0(model, preset): -1 finds the packing +0 key among the .BON presets; a preset not found or
// past the list returns 0 with nothing changed. Else the key's 15 bytes to packing +0 and +2c70 = p.
bool agent_apply_scale_preset(const AgentPartCache& c, AgentCreateModel& m, std::int32_t preset);
std::int32_t agent_morph_count(const AgentPartCache& c, std::int32_t sex);                // 0038de20
// 0038df00(model, dir, cur, sex): the next "SHAPE_" entry of morph list [sex] (wrapping) to packing
// +10, +2c22 / +2c20. The original loops forever without one: throws.
std::int32_t agent_morph_step(const AgentPartCache& c, AgentCreateModel& m, std::int32_t dir, std::int32_t cur,
                              std::int32_t sex);
// 004061b0(packing, count): the blank ("BLA") meshes and textures of the other sex' parts the
// packing lacks become pairs before the terminator. Returns the new count.
std::size_t agent_add_blank_pairs(const AgentPartCache& c, AgentPacking& p, std::size_t count);
// 00127dd0 over 00127db8's state.
std::uint32_t agent_random_next(std::uint64_t& state);

}  // namespace omega
