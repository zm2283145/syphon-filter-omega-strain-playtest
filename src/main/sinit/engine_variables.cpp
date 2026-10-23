#include "engine_variables.hpp"
#include "script_context.hpp"
#include <array>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
constexpr std::array<EngineVariableDefault, 83> table{{
    {"DarknessDecreaser", 0x00489bb0U, 1, 0x00000041U},   // registered at 004c3720
    {"NpcDarknessDecreaser", 0x00489bb8U, 1, 0x0000005aU},   // registered at 004c3744
    {"SendStatsTime", 0x00489bc0U, 1, 0x00000003U},   // registered at 004c3768
    {"dmgAdjust", 0x00489c10U, 2, 0x3f800000U},   // registered at 004c3a00
    {"MinNpcAmmoPct", 0x00489c18U, 2, 0x3d4ccccdU},   // registered at 004c3a24
    {"MaxNpcAmmoPct", 0x00489c20U, 2, 0x3e99999aU},   // registered at 004c3a48
    {"doKnifeDmg", 0x00489da0U, 1, 0x00000001U},   // registered at 004c3b34
    {"WorthShootingThreshold", 0x004ea388U, 2, 0x00000000U},   // registered at 004c3c54
    {"IncreasedArmorMul", 0x0048a1c8U, 2, 0x3fc00000U},   // registered at 004c6f78
    {"IncreasedHealthMul", 0x0048a1d0U, 2, 0x3fc00000U},   // registered at 004c6f9c
    {"HumanFireIntensity", 0x0048a1d8U, 2, 0x41200000U},   // registered at 004c6fc0
    {"HumanFireRad", 0x0048a1e0U, 2, 0x40a00000U},   // registered at 004c6fe4
    {"FireReliefPerSecond", 0x0048a1e8U, 2, 0x41700000U},   // registered at 004c7008
    {"OnFireDmgPerSecond", 0x0048a1f0U, 2, 0x42200000U},   // registered at 004c702c
    {"FireDmgToIgnite", 0x0048a1f8U, 1, 0x00000014U},   // registered at 004c7050
    {"ArmorPenetrationResistance", 0x0048a218U, 1, 0x0000000aU},   // registered at 004c7074
    {"BodyPenetrationResistance", 0x0048a220U, 1, 0x00000004U},   // registered at 004c7098
    {"HeadPenetrationResistance", 0x0048a228U, 1, 0x00000002U},   // registered at 004c70bc
    {"StartingArmor", 0x0048a238U, 1, 0x000000faU},   // registered at 004c7140
    {"humanHeadAim", 0x0048a2a8U, 2, 0x3e99999aU},   // registered at 004c7214
    {"humanKneeAim", 0x0048a2b0U, 2, 0x3f000000U},   // registered at 004c7238
    {"HitInnocentNpcChance", 0x0048a2b8U, 2, 0x3e99999aU},   // registered at 004c725c
    {"BlindRampUp", 0x0048a2c8U, 2, 0x3e99999aU},   // registered at 004c72ac
    {"IncreasedAmmoMul", 0x0048a2d0U, 2, 0x3f99999aU},   // registered at 004c72d0
    {"RadarLosLag", 0x0048ac68U, 1, 0x000000c8U},   // registered at 004cebfc
    {"immediateLos", 0x0048ac70U, 1, 0x00000001U},   // registered at 004cece8
    {"globalNpcSkill", 0x0048acc0U, 2, 0x3f800000U},   // registered at 004cefd4
    {"PlayerAimTimePct", 0x0048acc8U, 2, 0x3f800000U},   // registered at 004ceff8
    {"LaserRad", 0x0048ae20U, 2, 0x3e800000U},   // registered at 004cf4b8
    {"NightVisionMaskFrames", 0x0048ae48U, 1, 0x0000000aU},   // registered at 004cf8dc
    {"ManualAimTurnTopSpeed", 0x0048ae58U, 2, 0x3fb33333U},   // registered at 004cfa10
    {"ManualAimTurnAcceleration", 0x0048ae60U, 2, 0x40000000U},   // registered at 004cfa34
    {"ManualAimStopTurnDeceleration", 0x0048ae68U, 2, 0x41a00000U},   // registered at 004cfa58
    {"QuickLookMaxAngleX_Degrees", 0x0048ae70U, 2, 0x42700000U},   // registered at 004cfa7c
    {"QuickLookMaxAngleZ_Degrees", 0x0048ae78U, 2, 0x43200000U},   // registered at 004cfaa0
    {"QuickLookFractionForRun", 0x0048ae80U, 2, 0x3ef0a3d7U},   // registered at 004cfac4
    {"stickyLOSFrames", 0x0048aed0U, 1, 0x00000032U},   // registered at 004cfb5c
    {"SubTargetMaxDist", 0x0048aed8U, 2, 0x42000000U},   // registered at 004cfb80
    {"FlashDownOff", 0x0048aee0U, 2, 0x443b8000U},   // registered at 004cfba4
    {"MaxPacks", 0x0048af10U, 1, 0x0000000cU},   // registered at 004cff6c
    {"PackFadeTime", 0x0048af18U, 2, 0x40400000U},   // registered at 004cff90
    {"BlindDuration", 0x0048aff0U, 2, 0x40000000U},   // registered at 004d01f8
    {"BlindNVFactor", 0x0048aff8U, 2, 0x40400000U},   // registered at 004d021c
    {"lockAlpha", 0x0048b0a0U, 2, 0x3f666666U},   // registered at 004d063c
    {"NVlockAlpha", 0x0048b0a8U, 2, 0x3f666666U},   // registered at 004d0660
    {"dangerFirstFrame", 0x004f8260U, 1, 0x00000000U},   // registered at 004d072c
    {"dangerLastFrame", 0x0048b0b0U, 1, 0x00000016U},   // registered at 004d0750
    {"dangerDimFrame", 0x0048b0b8U, 1, 0x0000001cU},   // registered at 004d0774
    {"RadarPixWid", 0x0048b0c0U, 1, 0x00000037U},   // registered at 004d0798
    {"RadarMaxDist", 0x0048b0c8U, 2, 0x43160000U},   // registered at 004d07bc
    {"RadarMaxScale", 0x0048b0d0U, 2, 0x40800000U},   // registered at 004d07e0
    {"TremorCamFactor", 0x0048b2d0U, 2, 0x3e99999aU},   // registered at 004d1e3c
    {"TremorMinRumble", 0x0048b2d8U, 1, 0x00000028U},   // registered at 004d1e60
    {"TremorMaxRumble", 0x0048b2e0U, 1, 0x000000ffU},   // registered at 004d1e84
    {"TremorRandPct", 0x0048b2e8U, 2, 0x3f000000U},   // registered at 004d1ea8
    {"DangerMag", 0x0048b310U, 2, 0x3fa00000U},   // registered at 004d1f2c
    {"BloodDecalDist", 0x0048b318U, 2, 0x40e00000U},   // registered at 004d2358
    {"BloodDecalRad", 0x0048b320U, 2, 0x40000000U},   // registered at 004d237c
    {"MinFTScale", 0x0048b330U, 2, 0x3ecccccdU},   // registered at 004d23a0
    {"MaxFTScale", 0x0048b338U, 2, 0x40400000U},   // registered at 004d23c4
    {"FTPower", 0x0048b340U, 1, 0x00000002U},   // registered at 004d23e8
    {"BulletOffY", 0x0048b348U, 2, 0xc0000000U},   // registered at 004d240c
    {"scriptInterpreter", 0x004938e8U, 1, 0x00555070U},   // registered at 004d6010
    {"PerformActionOnceThrough", 0x00493938U, 2, 0xbf800000U},   // registered at 004d6410
    {"PerformActionForever", 0x00493940U, 2, 0xc0000000U},   // registered at 004d6434
    {"cursorFlashRate", 0x00493960U, 1, 0x0000000fU},   // registered at 004d67a0
    {"cursorFlashDiv", 0x00493968U, 1, 0x00000003U},   // registered at 004d67c4
    {"cursorOff", 0x00493978U, 1, 0xfffffffeU},   // registered at 004d67e8
    {"NpcDmgAimThresh", 0x00493988U, 2, 0x3f666666U},   // registered at 004d698c
    {"FlashDist", 0x004979b8U, 2, 0x42c80000U},   // registered at 004d7c2c
    {"FlashShortDist", 0x004979c0U, 2, 0x40200000U},   // registered at 004d7c50
    {"FlashHitRad", 0x004979c8U, 2, 0x3fc00000U},   // registered at 004d7c74
    {"FlashAdjusty", 0x004979d0U, 2, 0x3f800000U},   // registered at 004d7c98
    {"FlashMinOuter", 0x004979d8U, 2, 0x41100000U},   // registered at 004d7cbc
    {"FlashMinInner", 0x005860c0U, 2, 0x00000000U},   // registered at 004d7ce0
    {"FlashMaxOuter", 0x004979e0U, 2, 0x41c80000U},   // registered at 004d7d04
    {"FlashMaxInner", 0x004979e8U, 2, 0x40a00000U},   // registered at 004d7d28
    {"FlashMinRG", 0x004979f0U, 2, 0x3dcccccdU},   // registered at 004d7d4c
    {"FlashMinB", 0x004979f8U, 2, 0x3d4ccccdU},   // registered at 004d7d70
    {"FlashVel", 0x00497a10U, 2, 0x3f800000U},   // registered at 004d7d94
    {"FlashAcc", 0x00497a18U, 2, 0x43480000U},   // registered at 004d7db8
    {"FlashDazzleSize", 0x00497a20U, 2, 0x41000000U},   // registered at 004d7ddc
    {"FlashSqueeze", 0x005860c8U, 2, 0x00000000U},   // registered at 004d7e00
}};
}

std::span<const EngineVariableDefault> engine_variable_defaults() { return table; }

void define_engine_variable_defaults(ScriptContext& context) {
    for (const auto& v : table) {
        const std::string name = std::string("game::") + v.name;
        bool defined = true;
        try { (void)context.engine_variable(name); } catch (const std::runtime_error&) { defined = false; }
        if (!defined) context.define_engine_variable(name, Value(Word{v.bits}));
    }
}
}
