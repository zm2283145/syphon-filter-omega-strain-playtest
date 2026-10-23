#include "player_pad_control.hpp"
#include "game_libm.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace omega {
namespace {
float bits(std::uint32_t value){return std::bit_cast<float>(value);}
// The EE FPU and VU have no denormals: tiny results become signed zero. The
// retained direction X (+0xa0) and turn decay geometrically into that range.
float check(float v){
    if(!std::isfinite(v))throw std::runtime_error("Player pad control: nonfinite arithmetic");
    if(std::fpclassify(v)==FP_SUBNORMAL)return std::signbit(v)?-0.0F:0.0F;
    return v;
}
float add(float a,float b){return check(check(a)+check(b));}
float sub(float a,float b){return check(check(a)-check(b));}
float mul(float a,float b){return check(check(a)*check(b));}
float divide(float a,float b){
    if(check(b)==0)throw std::runtime_error("Player pad control: division by zero");
    return check(check(a)/b);
}
// EE max.s/min.s.
float maximum(float a,float b){return a<b?b:a;}
float minimum(float a,float b){return b<a?b:a;}
float clamp_unit(float v){return minimum(maximum(v,-1.0F),1.0F);}
using Vector=std::array<float,4>;
// vmul.xyz, vmulax.w/vmadday.w/vmaddz.w: (x*x + y*y) + z*z.
float length_squared(const Vector& v){return add(add(mul(v[0],v[0]),mul(v[1],v[1])),mul(v[2],v[2]));}
float length(const Vector& v){return check(std::sqrt(length_squared(v)));}
bool zero_xyzw(const Vector& v){return v[0]==0&&v[1]==0&&v[2]==0&&v[3]==0;}
bool zero_xyz(const Vector& v){return v[0]==0&&v[1]==0&&v[2]==0;}
// vrsqrt Q then vmulq.xyz (W unchanged).
Vector normalize_xyz(Vector v){
    const float q=divide(1.0F,check(std::sqrt(length_squared(v))));
    for(int i=0;i<3;++i)v[i]=mul(v[i],q);
    return v;
}
// 001e5920: all four lanes times the scalar.
Vector scale(Vector v,float s){for(float& x:v)x=mul(x,s);return v;}
float angle(float y,float x){return check(std::atan2(y,x));} // 00121ab8 -> 0011d7a0
// 0019d1a0 / 0019d190 rows, 00132250 returns +8; 0019d180 / 0019d170.
float transform_yaw(const Vector& row0,const Vector& row1){
    return row0[2]<row1[2]?game_atan2f(row0[1],row0[0]):game_atan2f(-row1[0],row1[1]);
}

const float half_pi=bits(0x3fc90fdbU);
const float pi=bits(0x40490fdbU);
const float three_quarter_pi=bits(0x4016cbe4U);
const float twenty_degrees=bits(0x3eb2b8c2U);      // f26
const float window_low=bits(0x3f5f66f3U);          // 004a7560 (50 degrees, set by 004cfaf0)
const float window_high=bits(0x401c61aaU);         // 004a7568 (140 degrees)
const float forward_epsilon=bits(0x3a83126fU);     // 0.001
const float smoothing_rate=bits(0x41500000U);      // 13
const float smoothing_epsilon=bits(0x358637beU);   // 1e-6
const float speed_offset=bits(0x3eaaa64cU);        // 0.3333
const float speed_range=bits(0x3f2aacdaU);         // 0.6667
const float turn_gain=bits(0x40101829U);           // f24, 2.2514746
}

float player_pad_axis(float centered,const PlayerPadAxisTuning& tuning){
    const float limit=bits(0x42ff0000U); // 127.5
    const float dead=check(tuning.dead_zone);
    if(!(dead>=0&&dead<limit))throw std::runtime_error("Player pad dead zone outside (0, 127.5)");
    const float gain=divide(limit,sub(limit,dead)); // 0036a1d8
    float out=0;
    check(centered);
    if(centered<0){if(centered<-dead)out=mul(gain,add(centered,dead));}
    else if(dead<centered)out=mul(gain,sub(centered,dead));
    out=maximum(out,-limit);           // conditional moves: -127.5 when <= -127.5
    return minimum(out,limit);
}

std::array<float,2> player_pad_stick(float h,float v,const PlayerPadAxisTuning& tuning){
    const float reciprocal=divide(1.0F,bits(0x42ff0000U)); // f20 = 1 / [004bbe10]
    const float x=player_pad_axis(h,tuning),y=player_pad_axis(v,tuning);
    return {mul(x,reciprocal),mul(-y,reciprocal)};
}

std::array<float,4> smooth_player_vector(float rate_a,float rate_b,float epsilon_difference,
    float epsilon_previous,float elapsed,const std::array<float,4>& target,
    const std::array<float,4>& previous){
    Vector d{};for(int i=0;i<4;++i)d[i]=sub(target[i],previous[i]);
    if(length_squared(d)<check(epsilon_difference))return target;
    Vector u{};
    if(!(length_squared(previous)<check(epsilon_previous)))u=normalize_xyz(previous);
    // vmul.xyzw then the xyz sum into W ((x + y) + z), moved to a scalar.
    const float dot=add(add(mul(d[0],u[0]),mul(d[1],u[1])),mul(d[2],u[2]));
    Vector along{};if(dot<0)along=scale(u,dot);
    Vector across{};for(int i=0;i<4;++i)across[i]=sub(d[i],along[i]);
    const float ka=minimum(1.0F,mul(check(rate_a),check(elapsed)));
    const float kb=minimum(1.0F,mul(check(rate_b),elapsed));
    const Vector a=scale(across,ka),b=scale(along,kb);
    Vector out{};for(int i=0;i<4;++i)out[i]=add(add(previous[i],a[i]),b[i]);
    return out;
}

PlayerMoveCommand prepare_player_move(PlayerMoveState& state,const PlayerMoveInput& in){
    if(!(check(in.elapsed)>0))throw std::runtime_error("Player move elapsed time must be positive");
    PlayerMoveState next=state;
    const float dt=in.elapsed;
    // 0024c1b0: strafe f23 = clamp(((-p11) + p10) * 5).
    const float strafe=clamp_unit(mul(5.0F,add(-check(in.pressure_11),check(in.pressure_10))));
    const Vector left{check(in.left[0]),check(in.left[1]),0,0};
    const bool stick_zero=zero_xyzw(left);
    // sp+0x11c: angle from the stick's vertical axis, 0..pi/2.
    const float axis_angle=stick_zero?0.0F:angle(std::fabs(left[0]),std::fabs(left[1]));
    // f22: angle from forward, 0..pi.
    float forward_angle=stick_zero?0.0F:angle(std::fabs(left[0]),left[1]);
    // s5: turn in place. The other contributions at sp+0xce0/0xcf0/0xd00 are
    // zeroed at case entry, so only the stick window and strafe remain.
    bool in_place=false;
    if(in.actor_flag3204_bit0){
        const bool window=window_low<=forward_angle&&forward_angle<=window_high;
        in_place=window&&strafe==0;
    }
    const float magnitude=length(left); // f30
    // Backward hysteresis +0x2c (0024d658..d6e4).
    if(in_place)next.backward_2c=false;
    else if(!next.backward_2c){
        if(!(forward_angle<=three_quarter_pi))next.backward_2c=true;
        else if(!(forward_angle<=half_pi)&&in.actor_flag3204_bit0)next.backward_2c=true;
    }else if(forward_angle<half_pi)next.backward_2c=false;
    // f28: forward component.
    float forward=0;
    if(!in_place&&magnitude!=0)
        forward=next.backward_2c?minimum(-forward_epsilon,left[1]):maximum(forward_epsilon,left[1]);
    if(next.backward_2c)forward_angle=sub(pi,forward_angle);
    // f3: turn magnitude.
    float turn_amount;
    if(in_place)turn_amount=magnitude;
    else{
        const float slope=divide(1.0F,sub(half_pi,twenty_degrees)); // f25
        const float past=maximum(0.0F,sub(forward_angle,twenty_degrees));
        const float factor=strafe==0?1.0F:magnitude;
        turn_amount=mul(mul(slope,past),factor);
    }
    // Sign from the stick X (the subtracted sp+0xcf0 term is zero here).
    float turn=left[0]<0?clamp_unit(turn_amount):clamp_unit(sub(-0.0F,turn_amount));
    if(!in_place){
        const auto smoothed=smooth_player_vector(smoothing_rate,smoothing_rate,smoothing_epsilon,
            smoothing_epsilon,dt,{0,0,turn,0},{0,0,check(state.turn_80),0});
        turn=smoothed[2];
    }
    PlayerMoveCommand command;
    float speed_magnitude=0;
    Vector direction{};
    if(!in_place){
        speed_magnitude=minimum(1.0F,maximum(maximum(maximum(std::fabs(strafe),0.0F),magnitude),0.0F));
        direction={add(add(strafe,0.0F),0.0F),add(add(forward,0.0F),0.0F),0,0};
        if(!zero_xyz(direction))direction=normalize_xyz(direction);
        direction=scale(direction,speed_magnitude);
        if(strafe==0&&forward!=0){
            const float blend=minimum(maximum(add(0.1F,mul(mul(30.0F,sub(half_pi,axis_angle)),dt)),
                bits(0x3dcccccdU)),1.0F);
            if(blend!=1.0F){
                direction[0]=add(check(state.strafe_x_a0),mul(blend,sub(direction[0],state.strafe_x_a0)));
                if(!zero_xyz(direction))direction=normalize_xyz(direction);
                direction=scale(direction,speed_magnitude);
            }
        }
        next.strafe_x_a0=direction[0];   // 0024dc08
    }else{
        next.backward_2c=false;
        next.strafe_x_a0=0;              // sp+0x1a0 stays clear: 0024fae8
    }
    command.direction=direction;
    command.speed=speed_magnitude==0?0.0F:divide(sub(speed_magnitude,speed_offset),speed_range);
    command.turn=mul(turn,turn_gain);
    command.turn_in_place=in_place;
    next.turn_80=turn;                   // 0024fa64
    state=next;
    return command;
}

bool update_player_quick_turn(PlayerQuickTurnState& state,const PlayerQuickTurnInput& input){
    const auto reset=[&]{state.timer_114=0;state.armed_118=false;return false;};
    check(input.left[0]);check(input.left[1]);check(input.elapsed);check(state.timer_114);
    for(const float v:input.previous_left_60)check(v);
    if(input.selector_38!=0&&input.selector_38!=4)return reset();
    if(input.physical_unsupported_dc)return reset();
    const float y=input.left[1];
    if(y<0){
        // Timing starts only from a neutral previous stick (vaddx flags on +0x60).
        if(state.timer_114<=0&&!zero_xyzw(input.previous_left_60))return reset();
        state.timer_114=add(state.timer_114,input.elapsed);
        if(y<bits(0xbf400000U))state.armed_118=true;
        return false;
    }
    const bool fire=y<0.5F&&!(state.timer_114<=0)&&state.timer_114<=bits(0x3e4ccccdU)
        &&state.armed_118&&input.actor_mode_320c!=3;
    reset();
    return fire;
}

PlayerLadderMove prepare_player_ladder_move(PlayerMoveState& state,const PlayerMoveInput& input,float cross){
    const auto larger=[](float a,float b){return std::fabs(check(a))<=std::fabs(b)?b:a;};   // 0x0024e864
    const float x=check(input.left[0]);
    const float y=larger(input.left[1],0.0F);                      // sp+0xf28
    // min.s / max.s chains (0x0024e89c..0x0024e8fc).
    const float left=std::fabs(minimum(minimum(minimum(x,0.0F),-check(input.pressure_11)),0.0F));
    const float right=std::fabs(maximum(maximum(maximum(x,0.0F),check(input.pressure_10)),0.0F));
    PlayerLadderMove out;
    const float half=bits(0x3f000000U);
    if(check(cross)!=0.0F)out.exit_action=31;                      // 0x0024e914
    else if(left<half&&right<half){
        if(y<0.0F)out.exit_action=31;
        else if(!(y<=0.0F))out.exit_action=32;
    }else out.exit_action=left<=right?34:33;                       // 0x0024e978
    const float magnitude=std::fabs(y);                            // f30
    out.command.direction={0.0F,0.0F,y,0.0F};                      // sp+0xf20 (cleared at 0x0024c224)
    out.command.speed=magnitude==0.0F?0.0F:divide(sub(magnitude,speed_offset),speed_range); // 0x0024eac4
    out.command.turn=mul(0.0F,turn_gain);                          // f22 * f24
    out.command.turn_in_place=false;
    state.backward_2c=false;state.turn_80=0.0F;state.strafe_x_a0=0.0F;
    return out;
}
PlayerHangMove prepare_player_hang_move(PlayerMoveState& state,const std::array<float,2>& left){
    // c.ole.s |a| <= |b| keeps b (0x0024e62c, 0x0024e658, 0x0024e690).
    const auto larger=[](float a,float b){return std::fabs(check(a))<=std::fabs(b)?b:a;};
    const float x=larger(larger(left[0],0.0F),0.0F);
    const float y=larger(left[1],0.0F);
    PlayerHangMove out;
    if(!(y<=bits(0x3f666666U)))out.queued_action=0x11;            // 0x0024e6d4
    else if(y<bits(0xbf666666U))out.queued_action=0x13;           // 0x0024e758
    const float magnitude=std::fabs(x);                            // f30 (0x0024e6e0)
    out.command.direction={x,0.0F,0.0F,0.0F};                      // sp+0xf20
    out.command.speed=magnitude==0.0F?0.0F:divide(sub(magnitude,speed_offset),speed_range); // 0x0024eac4
    out.command.turn=mul(0.0F,turn_gain);                          // f22 * f24
    out.command.turn_in_place=false;
    state.backward_2c=false;state.turn_80=0.0F;state.strafe_x_a0=0.0F;
    return out;
}

PlayerMoveCommand prepare_player_idle_move(PlayerMoveState& state){
    PlayerMoveCommand out;                                         // 0x0024e560 -> 0x0024ea08
    out.turn=mul(0.0F,turn_gain);                                  // f22 * f24
    state.backward_2c=false;state.turn_80=0.0F;state.strafe_x_a0=0.0F;
    return out;
}

std::array<float,4> smooth_player_aim_vector(const std::array<float,4>& target,
    const std::array<float,4>& previous,float rate,float epsilon,float elapsed){
    Vector d{};
    for(int i=0;i<4;++i)d[i]=sub(target[i],previous[i]);           // vsub.xyzw (0x00250d90)
    if(length_squared(d)<check(epsilon))return target;              // 0x00250dd4
    const float k=minimum(1.0F,mul(rate,elapsed));                  // min.s (0x00250df0)
    Vector out{};
    for(int i=0;i<4;++i)out[i]=add(previous[i],mul(d[i],k));        // vmulx, vadd (0x00250e30)
    return out;
}

PlayerAimViewMove prepare_player_aim_view_move(PlayerMoveState& state,const PlayerAimViewMoveInput& input){
    for(const auto* v:{&input.row0,&input.row1,&input.previous_90})for(float x:*v)check(x);
    check(input.half_fov_60);check(input.elapsed);
    PlayerAimViewMove out;
    out.yaw=transform_yaw(input.row0,input.row1);
    Vector s{add(input.left[0],input.right[0]),add(input.left[1],input.right[1]),0.0F,
        add(0.0F,0.0F)};                                            // 00173d00, Z cleared at 0x0024e2bc
    float len=0.0F;                                                 // sp+105c (0x0024e2d0)
    if(!zero_xyzw(s)){                                              // vaddx + MAC zero (0x0024e2f8)
        len=add(0.0F,check(std::sqrt(length_squared(s))));         // vsqrt, vaddq.x
        const float q=divide(1.0F,len);                             // vdiv (0x0024e338)
        for(int i=0;i<3;++i)s[i]=mul(s[i],q);
    }
    if(len<1.0F){                                                   // 0x0024e360
        float f0=sub(mul(2.0F,len),1.0F);
        f0=sub(1.0F,mul(f0,f0));
        const float len2=mul(len,len);
        const float k=add(mul(len2,len2),mul(bits(0x3e99999aU),f0)); // mula.s / madd.s
        s=scale(s,k);                                               // 001e5920
    }
    out.stick_c40=s;out.length_105c=len;
    Vector look{input.invert_124?s[1]:-s[1],0.0F,-s[0],0.0F};       // 0x0024e3a8..0x0024e3f0
    const float gain14=bits(0x3fb33333U);                           // 0048ae58
    look[0]=mul(look[0],gain14);look[2]=mul(look[2],gain14);
    const float f2=mul(bits(0x3f61a840U),mul(2.0F,input.half_fov_60)); // 004a7548, camera +60
    look[0]=mul(look[0],f2);look[2]=mul(look[2],f2);
    const float rate=zero_xyzw(look)?bits(0x41a00000U):bits(0x40000000U); // 0048ae68 / 0048ae60
    const float epsilon=mul(0.0F,mul(f2,f2));
    out.look_ee0=smooth_player_aim_vector(look,input.previous_90,rate,epsilon,input.elapsed);
    out.rates_f30=input.bits_3c_bit1?Vector{}:out.look_ee0;         // 00250350(+38, 1)
    out.command.turn=mul(0.0F,turn_gain);                           // f22 = 0, f30 = 0, sp+f20 = 0
    state.backward_2c=false;state.turn_80=0.0F;state.strafe_x_a0=0.0F;
    return out;
}

PlayerLockMove prepare_player_lock_move(PlayerMoveState& state,const PlayerLockMoveInput& input){
    for(const auto* v:{&input.row0,&input.row1})for(float x:*v)check(x);
    check(input.left[0]);check(input.left[1]);
    PlayerLockMove out;
    out.yaw=transform_yaw(input.row0,input.row1);                   // f21 (0x0024dc28..0x0024dcb4)
    // 0024fde0: while v > pi subtract 2pi, then while v < -pi add 2pi (rechecking).
    const auto wrap=[](float v){
        for(;;){
            if(!(check(v)<=pi)){v=add(v,bits(0xc0c90fdbU));continue;}
            if(v<bits(0xc0490fdbU)){v=add(v,bits(0x40c90fdbU));continue;}
            return v;
        }};
    Vector ca0{};                                                   // 0x0024dcc0..0x0024dccc
    if(input.actor_rows){                                           // 0014d350(+c8), +50 (0x0024dcc8..0x0024dce4)
        for(const auto& r:*input.actor_rows)for(float x:r)check(x);
        const float actor_yaw=transform_yaw((*input.actor_rows)[0],(*input.actor_rows)[1]);
        const float d=sub(out.yaw,actor_yaw);                       // 0x0024ddc4
        ca0={wrap(0.0F),wrap(0.0F),wrap(d),0.0F};
    }
    out.difference_ca0=ca0;
    // 0x0024c1b0: f23 = clamp(5 ((-p11) + p10)).
    const float strafe=clamp_unit(mul(5.0F,add(-check(input.pressure_11),check(input.pressure_10))));
    const Vector stick{input.left[0],input.left[1],0.0F,0.0F};     // sp+c80 = sp+f70
    const float f26=length(stick);                                  // VU length (0x0024df54)
    const float f25=0.0F;                                           // |004e1d90| (the zero vector)
    const bool turn_in_place=input.actor_flag3204_bit0&&f26==0.0F&&f25==0.0F&&strafe==0.0F
        &&!(std::fabs(ca0[2])<=0.0F);                               // 0x0024dfa8..0x0024e0b0
    Vector direction{};
    float f30=0.0F;
    if(!turn_in_place&&!input.bits_3c_bit1){                        // 0x0024e0b8, 0x0024e0c0
        f30=minimum(1.0F,maximum(maximum(std::fabs(strafe),f26),f25)); // max.s, 0024ffd0 (min.s)
        direction={add(add(strafe,0.0F),stick[0]),add(0.0F,stick[1]),0.0F,0.0F};
        if(!zero_xyz(direction))direction=normalize_xyz(direction);
        direction=scale(direction,f30);                             // 001e5920
    }
    out.command.direction=direction;
    out.magnitude_f30=f30;
    out.command.speed=f30==0.0F?0.0F:divide(sub(f30,speed_offset),speed_range); // 0x0024eac4
    out.command.turn=mul(0.0F,turn_gain);                           // f22 = 0 (0x0024e0b4)
    out.command.turn_in_place=turn_in_place;                        // s5
    state.backward_2c=false;state.turn_80=0.0F;state.strafe_x_a0=direction[0]; // sp+1a0 = 1
    return out;
}

PlayerAimViewDistance player_aim_view_distance(float camera_934,float camera_9c4){
    const float v=std::fabs(-camera_934);                           // 00250380, 001d3e10
    PlayerAimViewDistance out;
    out.far_200=!(v<bits(0x40800000U));                             // c.olt (0x0024bdc0): NaN is far
    out.near_1f0=!out.far_200&&!(camera_9c4!=0.0F);                 // 0017a610: c.eq (NaN is moving)
    return out;
}

std::array<float,4> player_camera_rates_d60(const std::array<float,4>& rates_f30){
    return scale(rates_f30,turn_gain);
}
PlayerPadGateResult apply_player_pad_gates(PlayerPadSlots& s,const PlayerPadGateBytes& b){
    PlayerPadGateResult r;
    // 0x0024be18..0x0024be58: input +0, 004ffc29, 00241ac0, or state 0x13.
    r.disabled=b.input_00!=0||b.loadout_pending_4ffc29||b.hud_busy_241ac0||b.state_4ffba0==0x13;
    if(r.disabled){
        // 0x0024be60..0x0024bf0c: the game screen's +d4 == 1 keeps Cross (as does +6a9) and both sticks.
        s.r2_fa8=0.0F;s.l2_fac=0.0F;s.r1_f90=0.0F;s.l1_f94=0.0F;
        if(!b.game_screen_d4_one&&!b.hud_6a9)s.cross_fa0=0.0F;
        s.triangle_fa4=0.0F;s.square_f9c=0.0F;s.circle_f98=0.0F;s.down_f8c=0.0F;s.left_f84=0.0F;s.right_f88=0.0F;
        s.select_f80=0.0F;s.start_fd4=0.0F;s.r3_fd8=0.0F;
        if(!b.game_screen_d4_one){s.left_f70={0.0F,0.0F};s.right_f60={0.0F,0.0F};}   // sq zero sp+f70 / sp+f60 (0x0024bf00 / 0x0024bf08)
    }
    // 0x0024bf30..0x0024bfe8: the time limit runs in state 15 without +6a9 / +6a8;
    // otherwise input +4 = 0.
    if(!b.hud_6a9&&!b.hud_6a8&&b.state_4ffba0==0x0f&&!(b.time_limit_48==0.0F))
        throw std::runtime_error("0024bf6c: the time limit G+48 (00369f90, input +4, 0012fa60 kind 2) is not hosted");
    r.hud_screen=b.hud_6a9||b.hud_6a8||b.hud_6aa;
    if(r.hud_screen){
        // 0x0024c010..0x0024c0dc.
        s.r2_fa8=0.0F;s.l2_fac=0.0F;s.r1_f90=0.0F;s.l1_f94=0.0F;s.select_f80=0.0F;s.down_f8c=0.0F;s.right_f88=0.0F;
        if(!(s.triangle_fa4==0.0F)&&!b.triangle_latch_ad&&b.hud_6a8)
            throw std::runtime_error("0024c050: Triangle closing the start menu from the player input (002261c0, 00131470(G, 0)) is not hosted");
        s.triangle_fa4=0.0F;s.square_f9c=0.0F;s.circle_f98=0.0F;
        // +6a9 outside selector 7 keeps the left stick's X (0x0024c0a4).
        if(b.hud_6a9&&b.selector_38!=7)s.left_f70[1]=0.0F;
        else s.left_f70={0.0F,0.0F};
        s.right_f60={0.0F,0.0F};s.r3_fd8=0.0F;
    }
    return r;
}
}
