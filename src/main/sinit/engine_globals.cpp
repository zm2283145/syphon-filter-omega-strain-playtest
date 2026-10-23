#include "engine_globals.hpp"

#include <map>

namespace omega {

std::vector<EngineStruct::Field> to_hit_settings_defaults() {
    constexpr Word float_type = 2;
    const std::pair<const char*, Word> fields[] = {
        {"runAccFactor", 0x3f333333},        {"runAimFactor", 0x3f333333},
        {"kneelAccFactor", 0x3fa66666},      {"kneelAimFactor", 0x40000000},
        {"movingTargAccFactor", 0x3f333333}, {"crouchTargAccFactor", 0x3f4ccccd},
        {"rollTargAccFactor", 0x3f000000},   {"maxVelocity", 0x41a00000},
        {"npcExtraAimTime", 0x3fc00000},     {"npcMovingTargVal", 0x3cf5c28f},
        {"npcRollingTargVal", 0x3ca3d70a},   {"reloadAimLoss", 0x3dcccccd},
        {"npcNearDist", 0x41400000},         {"npcNearMul", 0x40c00000},
    };
    std::vector<EngineStruct::Field> result;
    for (const auto& [name, bits] : fields) result.push_back({name, float_type, Word{bits}});
    return result;
}

const std::vector<Word>* engine_enum(const std::string& name) {
    // Registration callsite per table; GameSound_SndBank's site is not decoded,
    // its descriptors were read directly.
    static const std::map<std::string, std::vector<Word>> tables{
        {"BOOL", {1, 0}}, // 004d1130
        {"CAS_Action", {37, 38, 39}}, // 004d63ec
        {"EffectName", {0, 1, 2, 3, 4, 5, 6, 26, 10, 21, 22, 0xffffffffU}}, // 004cf324
        {"ElevatorGOBJ_ElevatorStates", {0, 1, 2, 3}}, // 004cd32c
        {"ElevatorNotice_ActionTypes", {0, 1, 2, 3, 4}}, // 004cd514
        {"FontType", {0, 1}}, // 004c5998
        {"GOBJTriggerEventMsg_ActionTypes", {0, 1, 2, 4, 5, 6, 3}}, // 004cd458
        {"GameSound_SndBank", {0, 1, 2, 3, 5, 4, 0xffffffffU}}, // descriptors at 0048a340
        {"HitLocation", {0, 1, 2, 3, 4}}, // 004d7868
        {"HotboxMsg_ActionTypes", {0, 1, 2}}, // 004c5fd8
        {"JoystickButton", {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}}, // 004d7e38
        {"MaterialProperties_DecalBlendMode", {0, 1, 2}}, // 004d6f98
        {"MaterialProperties_MatPropName", {0, 1, 2, 3, 4}}, // 004d6f78
        {"NINotice_ActionTypes", {0, 1}}, // 004cf868
        {"NPC_Action", {1, 9, 2, 4, 10}}, // 004c3de0
        {"NPC_AimMode", {0, 1, 2}}, // 004c3d80
        {"NPC_AimSubtarget", {0xffffffffU, 0}}, // 004c3da0
        {"NPC_CauseForAwareness", {0, 1, 2, 3, 4, 5, 6, 7, 8}}, // 004c5a4c
        {"NPC_Mode", {0, 2, 3, 4, 5, 6, 7, 8, 10}}, // 004c3d60
        {"NPC_TravelSpeed", {0, 1, 2, 3}}, // 004c3dc0
        {"NPC_WeaponPreference", {0, 1}}, // 004c3e00
        {"Node_TYPES", {1, 2, 4, 8, 16, 32, 64, 128, 256, 512}}, // 004c5798
        {"Objective_MultiTypes", {0, 1, 2, 3}}, // 004ce240
        {"Objective_NotifyTypes", {0, 1, 2}}, // 004ce260
        {"Objective_States", {0, 1, 2}}, // 004ce280
        {"Objective_Types", {0, 1, 2, 3, 4, 5}}, // 004ce220
        {"PathedGOBJ_ObjectStates", {0, 1}}, // 004ccd70
        {"PathedNotice_ActionTypes", {0, 1, 2}}, // 004cd5d0
        {"VisionFxMan_Flair", {0, 1, 2, 3}}, // 004d0514
        {"VisionFxMan_Mode", {0, 1, 2, 3, 4}}, // 004d04f4
        {"Weapon_WpnSlot", {0, 1, 2, 3, 7, 4, 5, 6}}, // 004c3b10
        {"WpnDamageType", {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15}}, // 004d7848
        {"testEnum", {12, 13, 5}}, // 004d1110
    };
    const auto found = tables.find(name);
    return found == tables.end() ? nullptr : &found->second;
}

}