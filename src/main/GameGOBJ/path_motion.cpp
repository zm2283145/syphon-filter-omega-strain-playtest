#include "path_motion.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace omega {
namespace {
void nonnegative(float value, const char* field) {
    if (!std::isfinite(value) || value < 0) throw std::runtime_error(std::string("Unsupported motion ") + field);
}
float ftz(float value) {
    if (value != 0.F && std::fabs(value) < std::numeric_limits<float>::min()) return std::copysign(0.F, value);
    return value;
}
// div.s as tools/ee/ee.py runs it: a zero divisor gives the largest float
// signed as S * T (positive for a zero S). A one-point path's only length is
// 0.0, so its segment progress is this value, clamped to 1.
float ee_div(float s, float t) {
    if (t != 0.F) return ftz(s / t);
    const double sign = s != 0.F ? static_cast<double>(s) * static_cast<double>(t) : 1.0;
    return std::copysign(std::numeric_limits<float>::max(), static_cast<float>(sign));
}
}

std::array<float,4> path_nlerp(std::array<float,4> q, std::array<float,4> other, float t) {
    float dot=0;
    for (int i=0;i<4;++i) dot+=q[i]*other[i];
    if (dot<0) for (auto& value:other) value=-value;
    float norm=0;
    for (int i=0;i<4;++i) { q[i]=(q[i]+other[i]*t)-q[i]*t; norm+=q[i]*q[i]; }
    if (!std::isfinite(norm) || norm<=0) throw std::runtime_error("Invalid interpolated quaternion");
    const float inverse=1.F/std::sqrt(norm);
    for (auto& value:q) value*=inverse;
    return q;
}
std::array<float,9> quaternion_basis(const std::array<float,4>& q) {
    const float x=q[0], y=q[1], z=q[2], w=q[3];
    const float xx=x*(x+x), xy=x*(y+y), xz=x*(z+z), xw=x*(w+w);
    const float yy=y*(y+y), yz=y*(z+z), yw=y*(w+w);
    const float zz=z*(z+z), zw=z*(w+w);
    return {(1-yy)-zz, xy+zw, xz-yw,
            xy-zw, (1-xx)-zz, yz+xw,
            xz+yw, yz-xw, (1-xx)-yy};
}

PathMotion::PathMotion(const PopulationObject& object) : pose_(object) {
    const auto& motion = object.motion;
    if (!std::isfinite(object.position.x) || !std::isfinite(object.position.y) || !std::isfinite(object.position.z))
        throw std::runtime_error("Nonfinite stored object position");
    for (const float value:object.basis)
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite stored object basis");
    // Modes 1 and 3 of 00216c60 are ported (mode 2 at 00217928 and mode 4,
    // the lift's, at 00217d38 are not, for a PathedGOBJ); both start stopped
    // with progress 1.0 and delay 0 (00219c34..00219c68).
    if (object.kind != 2 || (motion.mode != 1 && motion.mode != 3)
        || motion.points.size() != motion.rotations.size() || motion.points.size() > 65535)
        throw std::runtime_error("Only kind-2 paths in mode 1 or 3 (00216c60) with matching point/rotation arrays are supported");
    mode_ = motion.mode;
    // +c4: the record word after the speed, a float (002198ec..002198f4).
    pause_ = std::bit_cast<float>(motion.parameter);
    if (!std::isfinite(pause_)) throw std::runtime_error("Nonfinite path delay word (+c4)");
    state_.speed = motion.speed * 2.F;
    nonnegative(state_.speed, "file speed");
    for (const auto& q : motion.rotations) {
        float norm = 0;
        for (const float value : q) {
            if (!std::isfinite(value)) throw std::runtime_error("Nonfinite path quaternion");
            norm += value * value;
        }
        if (std::abs(norm-1.F)>0.001F) throw std::runtime_error("Non-unit endpoint quaternion is unsupported");
    }
    for (const auto& point : motion.points)
        if (!std::isfinite(point.x) || !std::isfinite(point.y) || !std::isfinite(point.z))
            throw std::runtime_error("Nonfinite path point");
    for (std::size_t i=1; i<motion.points.size(); ++i) {
        const auto a=motion.points[i-1], b=motion.points[i];
        const float x=b.x-a.x, y=b.y-a.y, z=b.z-a.z;
        float distance = std::sqrt((x*x+y*y)+z*z);
        if (!std::isfinite(distance)) throw std::runtime_error("Nonfinite path distance");
        // Float distance is promoted to double for the original soft-float threshold comparison.
        if (double(distance) <= 0.01) distance=1.F;
        lengths_.push_back(distance);
    }
    // 00219820 sizes +b4 by the point count; its last entry is 0.0 (*0048a9b8).
    if (!motion.points.empty()) lengths_.push_back(0.F);
}

void PathMotion::notice(const char* kind, int waypoint) { events_.push_back({kind,waypoint}); }
void PathMotion::trigger() {
    events_.clear(); state_.stopped=false; notice("moving"); state_.progress=0;
}
void PathMotion::prepare_trigger() {
    events_.clear(); state_.progress=0;
}
void PathMotion::reset_path() {
    if (pose_.motion.points.empty()) throw std::runtime_error("Reset of an empty path is unsupported");
    events_.clear(); publish(0,0,0,true);
    state_.direction=1; state_.index=0; state_.progress=0;
}
void PathMotion::stop() {
    events_.clear();
    if (!state_.stopped) state_.stop_target=state_.replicated_stop_target=state_.index+state_.direction;
}
void PathMotion::seek(int index) {
    events_.clear();
    if (index<0 || index>=static_cast<int>(pose_.motion.points.size()) || index==state_.index) return;
    state_.progress=0; state_.direction=index>state_.index ? 1 : -1;
    state_.stop_target=state_.replicated_stop_target=index; state_.stopped=false;
    notice("moving",index);
}
void PathMotion::set_speed(float speed) {
    nonnegative(speed,"script speed"); events_.clear(); state_.speed=speed;
}
void PathMotion::restore_state(const MotionState& state) {
    if (!std::isfinite(state.speed) || !std::isfinite(state.progress) || !std::isfinite(state.delay))
        throw std::runtime_error("Nonfinite restored motion state");
    events_.clear(); state_=state;
}

// 00218ea0: unparented, the point itself (00218fd0); parented (+6c), the point
// through the parent's rotation rows (vt+2c, 00171b30 with w = 1 against a
// zero fourth row) plus its translation (+40), each VU step flushed.
Vec3 PathMotion::waypoint(int index) const {
    const auto p=pose_.motion.points[static_cast<std::size_t>(index)];
    if (!parent_) return p;
    const auto m=parent_();
    std::array<float,3> out{};
    const float v[3]={p.x,p.y,p.z};
    for (std::size_t c=0;c<3;++c) {
        float acc=ftz(v[0]*m[c]);
        acc=ftz(acc+ftz(v[1]*m[4+c]));
        acc=ftz(acc+ftz(v[2]*m[8+c]));
        acc=ftz(acc+0.F);
        out[c]=ftz(acc+m[12+c]);
    }
    return {out[0],out[1],out[2]};
}

void PathMotion::carry(const std::array<float,16>& m) {
    for (const float value:m)
        if (!std::isfinite(value)) throw std::runtime_error("Nonfinite carried object root");
    events_.clear();
    pose_.basis={m[0],m[1],m[2],m[4],m[5],m[6],m[8],m[9],m[10]};
    pose_.position={m[12],m[13],m[14]};
    notice("pose");
}

void PathMotion::publish(int from, int to, float t, bool endpoint) {
    const auto& motion=pose_.motion;
    const auto a=waypoint(from), b=waypoint(to);
    auto q=motion.rotations[static_cast<std::size_t>(from)];
    if (endpoint) pose_.position=a;
    else {
        pose_.position={a.x+(b.x-a.x)*t, a.y+(b.y-a.y)*t, a.z+(b.z-a.z)*t};
        q=path_nlerp(q,motion.rotations[static_cast<std::size_t>(to)],t);
    }
    pose_.basis=quaternion_basis(q);
    notice("pose");
}
void PathMotion::publish_endpoint(int point, int rotation) {
    pose_.position=waypoint(point);
    pose_.basis=quaternion_basis(pose_.motion.rotations[static_cast<std::size_t>(rotation)]);
    notice("pose");
}

// 00216c60 for modes 1 and 3. The point (00218ea0), rotation (00218c80 /
// 2181c0) and length (2181e0) reads clamp their index to the array.
void PathMotion::tick(float timestep) {
    nonnegative(timestep,"timestep"); events_.clear();
    auto& s=state_;
    // 00216c84: stopped (+dc) or no points (+98): only the base update 003ce650.
    if (s.stopped || pose_.motion.points.empty()) return;
    // 00216cb0: a positive delay +c8 counts down; while it stays positive the
    // update returns before anything else (not even 003ce650).
    if (!(s.delay <= 0.F)) {
        s.delay=ftz(s.delay-timestep);
        if (!(s.delay <= 0.F)) return;
        // 00216cd8..00216dfc: the +e4 sound (0020f990, sent to 004ffb50 with
        // transport 1) and a GOBJTriggerEventMsg to each class-0xb +7c
        // reference; the host answers this event. Then the motion continues.
        notice("delay_end");
    }
    const int n=static_cast<int>(pose_.motion.points.size());
    const auto clamp=[n](int i) { return std::clamp(i,0,n-1); };
    float length;
    if (s.direction==1) length=lengths_[static_cast<std::size_t>(clamp(s.index))];
    else {
        // 00216e70..00216f58.
        if (s.index==-1) s.index=0;
        if (s.direction==-1 && s.index==0) s.direction=0;
        if (s.index==n && s.direction==1) s.direction=0;
        if (s.index>=n) s.index=n-1;
        length=lengths_[static_cast<std::size_t>(clamp(s.index+s.direction))];
    }
    float progress=ftz(s.progress+ee_div(ftz(s.speed*timestep),length));
    if (!(progress <= 1.F)) progress=1.F;
    s.progress=progress;
    if (progress!=1.F) {
        // 00217fd8: the lerp of the two points, 00218c80(to, from, progress), vt+94.
        publish(clamp(s.index),clamp(s.index+s.direction),progress,false);
        return;
    }
    s.index+=s.direction; notice("waypoint",s.index);
    if (s.index==s.stop_target) {
        // 002170a8: the +cc point and rotation through vt+ac.
        const int c=clamp(s.stop_target);
        publish_endpoint(c,c); s.stopped=true; notice("stopped",s.index);
        s.stop_target=-1; s.progress=0;
        return;
    }
    s.progress=0;
    const int rotation=clamp(s.index<n ? s.index : n-1);   // 00217354..00217370 (s0)
    if (mode_==1) {
        if (s.index>=n-1) {
            // 002173cc: stopped, PathedNotice (0, index), +c8 = 0, direction -1,
            // index n - 1, then the endpoint through vt+ac.
            s.stopped=true; notice("stopped",s.index);
            s.delay=0; s.direction=-1; s.index=n-1;
            publish_endpoint(s.index,rotation);
        } else if (s.index<=0) {
            // 00217688: point and rotation 0 through vt+ac, stopped, PathedNotice
            // (0, index), +c8 = 0, direction 1, index 0.
            publish_endpoint(0,0); s.stopped=true; notice("stopped",s.index);
            s.delay=0; s.direction=1; s.index=0;
        }
        // An intermediate arrival publishes nothing (00217680).
        return;
    }
    // Mode 3 (00217b88).
    if (s.index>=n-1) {
        // 00217ba4: the far end waits: +c8 = the delay word +c4, direction -1,
        // index n - 1; no pose, no notice, not stopped.
        s.delay=pause_; s.direction=-1; s.index=n-1;
    } else if (s.index<=0) {
        // 00217bd8: point and rotation 0 through vt+ac (no rider pass 00218200),
        // stopped, PathedNotice (0, index), +e0 = 1.0, +c8 = 0, direction 1, index 0.
        publish_endpoint(0,0); s.stopped=true; notice("stopped",s.index);
        s.progress=1.F; s.delay=0; s.direction=1; s.index=0;
    }
}

void write_motion_state(std::ostream& out, const PathMotion& motion) {
    const auto& s=motion.state(); const auto& p=motion.pose();
    out<<std::setprecision(9)<<"{\"index\":"<<s.index<<",\"direction\":"<<s.direction
       <<",\"stop_target\":"<<s.stop_target<<",\"replicated_stop_target\":"<<s.replicated_stop_target
       <<",\"progress\":"<<s.progress<<",\"delay\":"<<s.delay<<",\"speed\":"<<s.speed
       <<",\"stopped\":"<<(s.stopped?"true":"false")
       <<",\"position\":["<<p.position.x<<','<<p.position.y<<','<<p.position.z<<"],\"basis\":[";
    for (std::size_t i=0;i<9;++i) { if(i)out<<','; out<<p.basis[i]; }
    out<<"],\"events\":[";
    for(std::size_t i=0;i<motion.events().size();++i) {
        if(i)out<<',';
        const auto& event=motion.events()[i];
        out<<"{\"kind\":\""<<event.kind<<"\",\"waypoint\":"<<event.waypoint<<'}';
    }
    out<<"]}";
}
}
