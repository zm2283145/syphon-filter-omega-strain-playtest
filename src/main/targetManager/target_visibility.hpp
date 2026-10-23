#pragma once
// The target manager's line-of-sight cache (004f77c0; research/COMBAT.md,
// "Checkpoint 54"), interpreter-verified:
//   0022c650  register an object in the 30 slots +98 (and its weak handle in
//             the registration list +11c);
//   0022c720  unregister: the object's row and column entries lose +0/+4/+d
//             (+8 and +c are kept) and its slot is freed;
//   0022cb10  the cached pair test: returns the blocking object (0: clear);
//   0022d1f0  the manager's handler for the LOS result (key 005435a8) that
//             0022cb10 posts to itself.
// The world LOS service (004ea0b0, virtual +0c = 00138a10 -> 0013a480) and the
// smoke volumes 002594a0 are host functions: 0022cb10 builds the request
// exactly (LosServiceRequest) and the host answers it. The service's region
// hint (the viewer's +2fe0 region into the provider *004ea0b8 +50/+54) is a
// host call too.
#include "animation_root.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

namespace omega {

// One pair entry (16 bytes).
struct VisibilityEntry {
    std::int32_t expiry_0{};          // frame the answer expires (fp + 40)
    std::int32_t visible_until_4{};   // last frame known clear
    std::uint64_t blocker_8{};        // the blocking object (0: none)
    std::uint8_t visible_c{}, pending_d{};
    bool operator==(const VisibilityEntry&) const=default;
};

// The synchronous LOS request 0022cb10 sends (class 005435a0, vtable 004d9180).
struct LosServiceRequest {
    RootVector origin_30{};           // the viewer's eye (virtual +bc)
    RootVector displacement_40{};     // 250 * the normalized direction to the target's eye
    std::uint64_t object_50{}, object_54{};   // viewer and target (excluded from direct geometry)
    std::uint32_t mask_58{};          // 4 (forwarded by 0013a480 as the excluded surface mask)
    std::uint8_t mode_5c{};           // 0: answered at once
    float value_3c{};                 // 1.0
    float value_4c{};                 // 0.0
    bool operator==(const LosServiceRequest&) const=default;
};
struct LosServiceResult {
    bool hit{};                       // result +0
    RootVector point{};               // result +10 (read only with a hit)
    std::uint64_t object{};           // *(result +84): the hit object
};

// 0022cb10's request (0022cf20..0022cf6c): viewer eye -> 250 * the unit direction to the target's eye,
// the two objects excluded, mask 4, answered at once. Coincident eyes give a zero direction (VRSQRT of 0 is
// +FLT_MAX in the interpreter, times 0; Checkpoint 132zf12).
LosServiceRequest pair_los_request(std::uint64_t viewer,const RootVector& viewer_eye,std::uint64_t target,const RootVector& target_eye);
// 0022d1f0's reading of the answer: a hit blocks unless it lies beyond the target's eye.
bool pair_los_blocks(const LosServiceResult& result,const RootVector& viewer_eye,const RootVector& target_eye);

struct VisibilityHost {
    std::function<bool(std::uint64_t)> player_controlled;         // +58 nonzero and its virtual +54
    std::function<bool(std::uint64_t)> byte_14;                   // +14 (locally owned)
    std::function<RootVector(std::uint64_t)> eye_bc;              // virtual +bc
    std::function<bool(std::uint64_t)> actor_class;               // +4c == *0049d010
    // 0014d410 on *(actor +2fe0): the actor's current region (nullopt: none);
    // then 001ae320(*004ea0b8, region) seeds the LOS provider's region cache.
    std::function<std::optional<std::uint64_t>(std::uint64_t)> actor_region;
    std::function<void(std::uint64_t region)> seed_los_region;
    std::function<std::uint64_t(const RootVector& viewer_eye,const RootVector& target_eye)> smoke_blocker;   // 002594a0
    std::function<LosServiceResult(const LosServiceRequest&)> los;   // 004ea0b0 virtual +0c (0013a480)
};

class TargetVisibility {
public:
    static constexpr std::size_t slot_count=30;
    // 0022c650: false when already registered or all 30 slots are taken.
    bool register_object(std::uint64_t object);
    // 0022c720 (the entries and the slot; the registration list is the host's).
    void unregister_object(std::uint64_t object);
    // 0022cb10(manager, a, b, max_age, force) with fp = *0055a818.
    std::uint64_t test(std::uint64_t a,std::uint64_t b,std::int32_t max_age,bool force,std::int32_t frame,VisibilityHost& host);
    std::optional<std::size_t> slot(std::uint64_t object) const;     // 0022d380
    const VisibilityEntry& entry(std::size_t lo,std::size_t hi) const;
    const std::array<std::uint64_t,slot_count>& slots() const{return slots_;}
    bool operator==(const TargetVisibility&) const=default;
private:
    std::array<std::uint64_t,slot_count> slots_{};
    std::array<VisibilityEntry,slot_count*(slot_count-1)/2> entries_{};
    VisibilityEntry& at(std::size_t lo,std::size_t hi);
};

}
