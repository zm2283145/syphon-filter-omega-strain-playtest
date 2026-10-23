#include "animation_root.hpp"
#include "vu_float.hpp"
#include <cmath>
#include <iomanip>
#include <limits>
#include <ostream>
#include <stdexcept>

namespace omega {
namespace {
float finite(float value) {
    if (!std::isfinite(value)) throw std::runtime_error("Root animation: nonfinite arithmetic or input");
    return value;
}
void finite_vector(const RootVector& value) {
    for (float component : value) finite(component);
}
float add(float a, float b) { return finite(a + b); }
float sub(float a, float b) { return finite(a - b); }
float mul(float a, float b) { return finite(a * b); }
float norm_squared(const RootVector& value, unsigned count) {
    float result = mul(value[0], value[0]);
    for (unsigned i = 1; i < count; ++i) result = add(result, mul(value[i], value[i]));
    return result;
}
float reciprocal_length(float norm) {
    if (!(norm > 0)) throw std::runtime_error("Root animation: zero or underflowed normalization");
    // 003af400 at 003af604, 003af784 and 003af950: VU RSQRT
    // Q = vf0.w/sqrt(norm), then vmulq.
    return finite(vu::rsqrt(norm));
}
RootVector normalized(RootVector value, unsigned count) {
    const float reciprocal = reciprocal_length(norm_squared(value, count));
    for (unsigned i = 0; i < count; ++i) value[i] = mul(value[i], reciprocal);
    return value;
}
RootVector multiply_quaternions(const RootVector& a, const RootVector& b) {
    RootVector result;
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned j = (i + 1) % 3, k = (i + 2) % 3;
        // VOPMULA, two VMADDAW, then VOPMSUB; preserve this order.
        result[i] = sub(add(add(mul(a[j], b[k]), mul(a[i], b[3])),
                            mul(b[i], a[3])), mul(b[j], a[k]));
    }
    result[3] = sub(sub(sub(mul(a[3], b[3]), mul(a[2], b[2])),
                        mul(a[1], b[1])), mul(a[0], b[0]));
    return result;
}
RootVector rotate_vector(const RootVector& q, const RootVector& value) {
    RootVector first, conjugate{ sub(0, q[0]), sub(0, q[1]), sub(0, q[2]), q[3] };
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned j = (i + 1) % 3, k = (i + 2) % 3;
        // The original omits q.xyz * vector.w after replacing vector.w with 0.
        first[i] = sub(add(mul(q[j], value[k]), mul(value[i], q[3])), mul(value[j], q[k]));
    }
    first[3] = sub(sub(sub(mul(q[3], 0), mul(q[2], value[2])),
                       mul(q[1], value[1])), mul(q[0], value[0]));
    RootVector result;
    for (unsigned i = 0; i < 3; ++i) {
        const unsigned j = (i + 1) % 3, k = (i + 2) % 3;
        result[i] = sub(add(add(mul(first[j], conjugate[k]), mul(first[i], conjugate[3])),
                            mul(conjugate[i], first[3])), mul(conjugate[j], first[k]));
    }
    result[3] = value[3];
    return result;
}
RootVector incremental_orientation(float angle, float inverse_weight) {
    const float half = mul(0.5f, mul(angle, inverse_weight));
    // Original six-call Euler construction. Host double trig followed by a
    // binary32 conversion is an explicit approximation, not original libm.
    const float c0 = static_cast<float>(std::cos(0.0));
    const float c1 = static_cast<float>(std::cos(0.0));
    const float c2 = finite(static_cast<float>(std::cos(static_cast<double>(half))));
    const float s0 = static_cast<float>(std::sin(0.0));
    const float s1 = static_cast<float>(std::sin(0.0));
    const float s2 = finite(static_cast<float>(std::sin(static_cast<double>(half))));
    const float p1 = mul(c1, c2), p5 = mul(s1, s2), p3 = mul(c1, s2), p4 = mul(s1, c2);
    return {sub(mul(s0, p1), mul(c0, p5)), add(mul(c0, p4), mul(s0, p3)),
            add(mul(c0, p3), mul(s0, p4)), sub(mul(c0, p1), mul(s0, p5))};
}
RootVector constrained(RootVector motion, const RootMotionConstraint& constraint) {
    if (constraint.kind == RootConstraintKind::none) return motion;
    if (constraint.kind != RootConstraintKind::reject_axis && constraint.kind != RootConstraintKind::keep_axis)
        throw std::runtime_error("Root animation: unsupported physical callback");
    finite_vector(constraint.axis);
    const auto& axis = constraint.axis;
    const float dot = add(add(mul(motion[0], axis[0]), mul(motion[1], axis[1])), mul(motion[2], axis[2]));
    for (unsigned i = 0; i < 3; ++i) {
        const float along = mul(axis[i], dot);
        motion[i] = constraint.kind == RootConstraintKind::reject_axis ? sub(motion[i], along) : along;
    }
    return motion;
}
void vector_json(std::ostream& out, const RootVector& value) {
    out << '[';
    for (unsigned i = 0; i < 4; ++i) { if (i) out << ','; out << value[i]; }
    out << ']';
}
void optional_json(std::ostream& out, const std::optional<RootVector>& value) {
    if (value) vector_json(out, *value); else out << "null";
}
}

RootAnimationState make_initial_root_animation() { return {}; }

void set_root_placement(RootAnimationState& state, const RootVector& orientation, const RootVector& position) {
    finite_vector(orientation);
    finite_vector(position);
    state.orientation = orientation;
    state.position = position;
    state.dirty_transform = true;
}

void set_root_world_orientation(RootAnimationState& state,const RootVector& orientation,
    const RootParentTransform* parent) {
    finite_vector(orientation);
    auto local=orientation;
    if(parent){
        finite_vector(parent->orientation);
        const auto& q=parent->orientation;
        local=multiply_quaternions({sub(0,q[0]),sub(0,q[1]),sub(0,q[2]),q[3]},orientation);
    }
    state.orientation=local;state.dirty_transform=true;
}

RootParentTransform invert_root_placement(const RootParentTransform& placement) {
    finite_vector(placement.orientation);finite_vector(placement.position);
    const auto& q=placement.orientation;const auto& p=placement.position;
    const RootVector conjugate{sub(0,q[0]),sub(0,q[1]),sub(0,q[2]),q[3]};
    return {conjugate,rotate_vector(conjugate,{sub(0,p[0]),sub(0,p[1]),sub(0,p[2]),p[3]})};
}
void set_root_world_placement(RootAnimationState& state,const RootParentTransform& placement,
    const RootParentTransform* parent) {
    finite_vector(placement.orientation);finite_vector(placement.position);
    auto local=placement;
    if(parent){
        const auto inverse=invert_root_placement(*parent);
        local.orientation=multiply_quaternions(inverse.orientation,placement.orientation);
        local.position=rotate_vector(inverse.orientation,placement.position);
        // Original rotates the two translations separately, then adds XYZ.
        // Its operation order differs from rotating their difference once.
        for(unsigned i=0;i<3;++i)local.position[i]=add(local.position[i],inverse.position[i]);
    }
    set_root_placement(state,local.orientation,local.position);
}

void set_root_world_position(RootAnimationState& state,const RootVector& position,
    const RootParentTransform* parent) {
    finite_vector(position);
    auto local=position;
    if(parent) {
        const auto inverse=invert_root_placement(*parent);
        local=rotate_vector(inverse.orientation,position);
        // 001b2750 treats an affine point separately: no translation-W
        // multiplication, and input W=1 survives even if inverse.position.W
        // is another value. The weighted branch multiplies all four lanes.
        if(local[3]==1.0F) {
            for(unsigned i=0;i<3;++i)local[i]=add(local[i],inverse.position[i]);
        } else {
            RootVector weighted;
            for(unsigned i=0;i<4;++i)weighted[i]=mul(inverse.position[i],local[3]);
            for(unsigned i=0;i<3;++i)local[i]=add(local[i],weighted[i]);
            local[3]=weighted[3];
        }
    }
    state.position=local;
    state.dirty_transform=true;
}

RootAnimationOutput finalize_root_animation(RootAnimationState& state,
    const RootFrame& frame, const RootMotionConstraint& constraint,const RootParentTransform* parent) {
    finite(frame.weight);
    if (frame.weight < 0) throw std::runtime_error("Root animation: negative aggregate weight unsupported");
    if (frame.weight == 0) {
        state.sign_reference = RootVector{0, 0, 0, 1};
        return {};
    }
    finite_vector(state.orientation);
    finite_vector(state.position);
    finite_vector(frame.pose_quaternion);
    finite_vector(frame.pose_translation);
    finite(frame.angle);
    for (const auto& bucket : frame.motion_buckets) finite_vector(bucket);
    auto next = state;
    const float inverse_weight = finite(1.0f / frame.weight);
    next.orientation = multiply_quaternions(state.orientation, incremental_orientation(frame.angle, inverse_weight));
    if (state.normalization_counter < 20) ++next.normalization_counter;
    else {
        next.orientation = normalized(next.orientation, 4);
        next.normalization_counter = 0;
    }
    std::array<float, 3> lengths;
    for (unsigned i = 0; i < 3; ++i)
        lengths[i] = finite(vu::sqrt(norm_squared(frame.motion_buckets[i + 1], 3))); // VU SQRT 003af644/674/6a4
    const float combined_length = add(lengths[2], add(lengths[0], lengths[1]));
    RootVector motion;
    for (unsigned i = 0; i < 4; ++i)
        motion[i] = add(add(frame.motion_buckets[1][i], frame.motion_buckets[2][i]), frame.motion_buckets[3][i]);
    if (motion[0] != 0 || motion[1] != 0 || motion[2] != 0) {
        motion = normalized(motion, 3);
        for (float& component : motion) component = mul(component, combined_length);
    }
    for (unsigned i = 0; i < 4; ++i) motion[i] = mul(add(motion[i], frame.motion_buckets[0][i]), inverse_weight);
    motion = constrained(rotate_vector(next.orientation, motion), constraint);
    for (unsigned i = 0; i < 3; ++i) next.position[i] = add(state.position[i], motion[i]);

    RootVector pose = frame.pose_quaternion;
    if (pose[3] < 0) for (float& component : pose) component = sub(0, component);
    const RootVector output_orientation = multiply_quaternions(next.orientation, pose);
    next.pose_orientation = normalized(output_orientation, 4);
    RootVector position = frame.pose_translation;
    position[3] = frame.weight;
    position = rotate_vector(next.orientation, position);
    if (position[3] == 1) {
        for (unsigned i = 0; i < 3; ++i) position[i] = add(position[i], next.position[i]);
    } else {
        RootVector weighted_position;
        for (unsigned i = 0; i < 4; ++i) weighted_position[i] = mul(next.position[i], position[3]);
        for (unsigned i = 0; i < 3; ++i) position[i] = add(position[i], weighted_position[i]);
        position[3] = weighted_position[3];
    }
    for (unsigned i = 0; i < 4; ++i) next.pose_position[i] = mul(position[i], inverse_weight);
    next.pose_position[3] = 1;
    next.motion_cache = motion;
    next.sign_reference = pose;
    next.dirty_transform = next.dirty_motion = next.dirty_pose = true;
    RootAnimationOutput output{true,output_orientation,position};
    if(parent) {
        finite_vector(parent->orientation);finite_vector(parent->position);
        output.weighted_orientation=multiply_quaternions(parent->orientation,output_orientation);
        auto world=rotate_vector(parent->orientation,position);
        if(world[3]==1) {
            for(unsigned i=0;i<3;++i)world[i]=add(world[i],parent->position[i]);
        } else {
            RootVector offset;
            for(unsigned i=0;i<4;++i)offset[i]=mul(parent->position[i],world[3]);
            for(unsigned i=0;i<3;++i)world[i]=add(world[i],offset[i]);
            world[3]=offset[3];
        }
        output.weighted_position=world;
    }
    state = next;
    return output;
}

RootParentTransform root_world_placement(const RootAnimationState& state,const RootParentTransform* parent) {
    finite_vector(state.orientation);finite_vector(state.position);
    if(!parent)return {state.orientation,state.position};
    finite_vector(parent->orientation);finite_vector(parent->position);
    RootParentTransform result;
    result.orientation=multiply_quaternions(parent->orientation,state.orientation);
    result.position=rotate_vector(parent->orientation,state.position);
    for(unsigned i=0;i<3;++i)result.position[i]=add(result.position[i],parent->position[i]);
    result.position[3]=1;
    return result;
}
RootVector transform_root_vector(const RootParentTransform& transform,const RootVector& value) {
    finite_vector(transform.orientation);finite_vector(transform.position);finite_vector(value);
    auto result=rotate_vector(transform.orientation,value);
    for(unsigned i=0;i<3;++i)result[i]=add(result[i],mul(transform.position[i],value[3]));
    result[3]=mul(transform.position[3],value[3]);
    return result;
}

void write_root_animation(std::ostream& out, const RootAnimationState& state, const RootAnimationOutput& result) {
    out << std::setprecision(std::numeric_limits<float>::max_digits10);
    out << "{\"state\":{\"orientation\":"; vector_json(out, state.orientation);
    out << ",\"position\":"; vector_json(out, state.position);
    out << ",\"motion_cache\":"; vector_json(out, state.motion_cache);
    out << ",\"pose_orientation\":"; vector_json(out, state.pose_orientation);
    out << ",\"pose_position\":"; vector_json(out, state.pose_position);
    out << ",\"sign_reference\":"; optional_json(out, state.sign_reference);
    out << ",\"normalization_counter\":" << state.normalization_counter;
    out << ",\"dirty_transform\":" << (state.dirty_transform ? "true" : "false");
    out << ",\"dirty_motion\":" << (state.dirty_motion ? "true" : "false");
    out << ",\"dirty_pose\":" << (state.dirty_pose ? "true" : "false");
    out << "},\"output\":{\"active\":" << (result.active ? "true" : "false");
    out << ",\"weighted_orientation\":"; optional_json(out, result.weighted_orientation);
    out << ",\"weighted_position\":"; optional_json(out, result.weighted_position);
    out << "}}";
}
}
