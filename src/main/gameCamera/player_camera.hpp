#pragma once
// Player camera (vtable 004dcde0, constructor 002810a0): preupdate 00280f60,
// update 00280690 and follow logic 0027d620 over the base camera rig 0017a640.
//
// Supported: the ordinary on-foot state and the aim view. Camera flags
// (+cf0/+cf4/+cf8, built by 00247800 from the input helper) may only carry
// bit 2 (the aim view, Checkpoint 121b), bit 3 (aiming a held class other
// than 1, Checkpoint 121c) and bit 1 (the R1 pick camera 0027fa40 outside the
// aim view, Checkpoint 121d); the followed actor must be a character in mode
// +320c == 0 with flags +3248 == 0, and the target-frame query (actor vtable
// +ec = 001b0f30) then takes its default branch: the `flor` root placement
// (with a3 = s2 = bit 2 and not bit 3: X/Y of skeleton node *0049d024 = 14),
// the bit-3 frame point of node 14 (0027dab0), the look-at of a locked
// target (00280070) and the bit-3 rotation of the +50 transform by +d90
// (00280d90). Crouch, ladders, ledges, vehicles, the free camera (+cf4 bit
// 0), scripted cameras and zoom are rejected rather than approximated.
#include "camera_rig.hpp"
#include <array>
#include <cstdint>
#include <functional>
#include <optional>

namespace omega {

// Values the follow logic reads from the followed actor.
struct PlayerCameraTarget {
    // 001b0f30 default branch: world placement of root +3290 (`flor`) through
    // 00132400; its quaternion is 4D-normalised before use.
    RootParentTransform floor_root;
    // Actor +3310 (physical component). Without one the corresponding reads
    // below are skipped exactly as the original does.
    bool physical{};
    float offset_394{};          // subtracted from the frame height
    bool unsupported_dc{};       // +dc: set unless the floor response accepted a contact
    RootVector velocity_310{};   // +310
    bool plane_valid_e8{};       // +e8 (actor vtable +f8 = 0014d3d0)
    RootVector plane_f0{};       // +f0
    // Actor vtable +f0 (001b0f20 -> 00133e00): body centre, stored at +3e0.
    RootVector look_target{};
    std::uint32_t mode_320c{};   // must be 0
    std::uint32_t flags_3248{};  // must be 0
    // 001b13ec (001b0f30 with a3 = 1, the aim view): world position of the
    // skeleton node signed byte *0049d024 = 14 (00132a30 then 00132910); its
    // X/Y replace the frame position's (s2 set). With s2 clear (bit 3) the
    // look-at 00280070 (t0 = 0) reads it as its origin (00132910 at
    // 00280298). Required with +cf4 bit 2.
    std::optional<RootVector> aim_node_14;
    // 00133ed0 (00132a30(+cfc), node *0049d024 = 14): the node's world
    // matrix (rows x, y, z, t), read at 0027dac8 with +cf4 bit 3.
    std::optional<std::array<RootVector,4>> aim_node_14_matrix;
    // 00280070 from 0027fa40 (a3 = 10, t0 = 0): world position of skeleton
    // node signed byte *0049d02a = 15 (00132a30 then 00132910), the look-at
    // origin of the pick camera (+cf4 bit 1 without bit 2) with a lock.
    std::optional<RootVector> aim_node_15;
};

// 00247800 (0024794c..00247988): a valid pad lock +c0/+c4 (00247d60) and its
// object id from 00247d50.
struct PlayerCameraLock {
    std::uint64_t object{};
    std::int32_t id{-1};
};

// 00280070's view of the locked object +d04: its +c (the id compared with
// +d08), whether its +50 word is non-zero, the root bytes +2c/+2d/+2e and its
// virtual +c0(out, object, +d0c) (00188d80 for an actor).
struct PlayerCameraLookAtHost {
    std::function<std::int32_t(std::uint64_t object)> id_c;
    std::function<bool(std::uint64_t object)> transform_50;
    std::function<std::array<std::uint8_t,3>(std::uint64_t object)> root_bytes;
    std::function<RootVector(std::uint64_t object,std::int32_t subtarget)> eye_c0;
};

// 00247800 copies: input-state +10..+1c (constructor 002504e0 values) and the
// right-stick look offset 002503e0 (+d30). Rates +d60 are zero on foot.
struct PlayerCameraInput {
    std::uint32_t flags_cf8{};
    float height_d18{4.73F};       // 0x40975c29
    float distance_d14{13.0F};
    float side_d1c{0.5F};
    float pitch_d20{-0.0645771846F}; // 0xbd84410b (004a7570)
    RootVector look_offset_d30{};
    RootVector rates_d60{};
    // 0024f9fc: +c64 = +c68 = clamped 00142f20(...) (combat play passes it); unzoomed 65 degrees.
    float field_of_view{1.13446403F};
    // 00247800: with input +3c bit 8 clear and a valid pad lock, +d04 = lock,
    // +d08 = its id, +d10 = *004a9330 (0.24); +d0c = -1 with bit 8, else pad
    // +e4. The pad stage runs before the camera's preupdate.
    std::optional<PlayerCameraLock> lock;
    std::int32_t subtarget_d0c{-1};
    // Required when the aim view runs with +d04 != 0 (00280070 from 0027eb5c).
    const PlayerCameraLookAtHost* look_at{};
    // 00247cd8..00247d14: +d90, written by every pad pass (001ca840 of the
    // actor's secondary component +3314, zero without one). Read by the
    // camera only with +cf4 bit 3 (00280da4); nullopt there throws.
    std::optional<RootVector> rotation_d90;
};

struct PlayerCameraState {
    CameraRigState rig;
    std::uint32_t flags_cf0{}, flags_cf4{};
    std::uint64_t lock_d04{};        // 002810a0: 0
    std::int32_t lock_id_d08{-1};    // 002810a0: -1
    std::int32_t subtarget_d0c{-1};
    float lock_time_d10{};
    std::int32_t frame_kind_d84{};   // written by 001b0f30 (0 = floor root)
    float height_d88{};
    float pitch_d70{-0.0645771846F}, roll_d74{}, yaw_d78{};   // 004a9328 initial pitch
    RootVector rotation_d90{};       // 002811c0: 0; the pad's +d90 (00247d14)
    // 002811a0 / 002811a4: 0; the pick camera's hold bytes (0027fa40 only).
    std::uint8_t hold_d80{}, hold_d81{};
    // +da0: the copy of the +50 transform made right after the rig
    // (00280d40..00280d8c). The aim point 00249670 and the targeting stage
    // read it.
    std::array<RootVector,4> world_matrix{};
    // The +50 transform after the update: world_matrix, rotated by +d90 with
    // +cf4 bit 3 (00280d90..00280ee8). 00176400 (the shot frame and the lock
    // 00249b10), the thrown point 002497c0 and the pad's movement read it.
    std::array<RootVector,4> transform_50{};
    // +64: written by the preupdate's 0017e1b0 (00281014) from transform_50 row 1 before this
    // frame's update (atan2f(x, y)); read by the sound pan 001c7ce8.
    float heading_64{};
};

// 00247800 (pad 0024b5d0 at 0024ec14, after the targeting stage 00247d60 at
// 0024cec0) at 00247b20..00247b64: with input +3c bit 8, +cf8 bit 2 (4) and,
// when the held class 001437a0(+3524) & 0xff != 1 (00247b40; 11 with nothing
// held), bit 3 (8). hold_class is called only with bit 8; a missing hook
// then throws.
std::uint32_t player_camera_aim_flags_cf8(std::uint32_t input_bits_3c,
    const std::function<std::uint8_t()>& hold_class);
// 00247800 at 00247b68..00247b84: input +3c bit 5 (the R1 pick held,
// pending selector 5) sets +cf8 bit 1 (2), which selects 0027fa40.
std::uint32_t player_camera_pick_flag_cf8(std::uint32_t input_bits_3c);

// 001ca840(out, secondary): the +d90 rotation from the secondary component's
// recoil quaternion q (+250). q itself when |1 - q.w| <= 0x33d6bf95, else q
// times the 4D-normalised (vrsqrt) blend identity + 0.35 (q - identity)
// (ACC: identity + 0.35 q - 0.35 identity), with the VU product.
RootVector camera_recoil_rotation(const RootVector& q_250);

// 00280da4..00280ee8: the +50 transform M rotated by the quaternion +d90:
// rows i = 0..2 are R[i].x M0 + R[i].y M1 + R[i].z M2 (ACC order) with R the
// VU rotation of +d90; row 3 is M3.
std::array<RootVector,4> camera_rotate_transform(const std::array<RootVector,4>& m, const RootVector& d90);

// 0027dac0..0027db54 (+cf4 bit 3): node 14's world matrix applied to
// (0, 0, 0.25, 1) in ACC order x, y, z, w; the frame point sp+500 / sp+530.
RootVector camera_aim_node_point(const std::array<RootVector,4>& node_14);

// 00280c50: the pitch clamp of 00280690, *004a9320 (0x3fc49809, 88 degrees)
// with +cf4 bit 2, else *004a9318 (0x3f9c61aa, 70 degrees); static
// initialiser 004d2770.
float player_camera_pitch_limit(std::uint32_t flags_cf4);

// 0027db78..0027dbd8 (after 00280390; the bit-3 frame skips it): +cf4 bit
// 0xa (crawl zone), 0xb (crouched under a low ceiling) or 5 add 2.25 to +d88,
// otherwise the input-state height +d18.
float player_camera_frame_height(std::uint32_t flags_cf4, float height_d88, float height_d18);

// 002807f8..0028094c of 00280690 (+cf4 bit 0 clear): the boom +af0 (side)
// and +af4 (min(0, -distance)). The distance is 0 with bit 3 or 5, else 2
// with bit 0xb, 5 with bit 0xa, otherwise +d14; the side is 0 with any of
// bits 3, 0xb, 0xa, 5, 6, otherwise +d1c.
std::array<float,2> player_camera_boom_af0(std::uint32_t flags_cf4, float distance_d14, float side_d1c);

// 0027dd28..0027e058 of 0027d620: the retimes f24 (+210), f23 (+2e0/+3a0)
// and f21 (+910) outside the aim view (+cf4 bit 2 leaves them). A frame
// change (+d84 / +d88) raises f24/f23 to 0.67; +cf4 bit 0xb rising sets
// f23 = f21 = 0.4 (0.33 with actor +320c == 2 and bit 5 clear), falling
// raises both to 1, otherwise a bit 0xa change raises both to 1.5; bit 5
// sets f21 to 1.3 (actor +3248 bit 5) or 0.9; bit 1 raises f24 to 0.67,
// leaving it (+cf0 bit 1 only) to 1.
struct PlayerCameraRetimes { float orientation_210{-1}, pivot_2e0{-1}, boom_910{-1}; };
PlayerCameraRetimes player_camera_frame_retimes(std::uint32_t flags_cf0, std::uint32_t flags_cf4,
    std::int32_t old_kind, float old_height, std::int32_t kind, float height,
    std::int32_t mode_320c, std::uint32_t flags_3248, PlayerCameraRetimes in);

// 002810a0 with field of view 0 (the game's 00132c58 call); the input helper
// sets +c64/+c68 every frame.
PlayerCameraState make_player_camera();

// 00280f60 (flag shift) then 00280690 (0027d620 then 0017a640). The original
// dereferences the followed actor unconditionally, so a target is required.
void update_player_camera(PlayerCameraState&, const PlayerCameraInput&,
    const std::optional<PlayerCameraTarget>&, float elapsed, const CameraCollisionHost* host,
    const CameraZoomHost* zoom = nullptr, const std::function<std::uint32_t()>* random = nullptr);   // random: camera_rig.hpp

// The view the renderer draws from: 00133330 returns the camera's +50 node and
// 00133190 builds the view from it (00133300 copy, rigid inverse 00133200).
// 00280e60..00280ee4 writes the +d90-rotated matrix into that node (node vt
// +0xc) while +cf4 bit 3 is set, so this is transform_50, not the unrotated
// +da0 copy (world_matrix); the two differ only in the bit-3 aim view.
const std::array<RootVector,4>& player_camera_render_view(const PlayerCameraState&);

// 002503e0 for the on-foot cases: right stick (normalised, up positive) to the
// look offset (X = pitch, Z = yaw). invert_yaw/invert_pitch are the input
// state bytes +125/+126.
RootVector player_camera_look_offset(float stick_x, float stick_y, bool invert_yaw, bool invert_pitch);

// Helpers exposed for tests.
RootVector camera_matrix_to_quaternion(const std::array<RootVector,3>& rows);   // 0027d808 arithmetic, IEEE zero signs (actor callers)
RootVector camera_vu_matrix_to_quaternion(const std::array<RootVector,3>& rows); // 0027d808 / 0027ecb0 with the VU +0 lanes
RootVector camera_axis_quaternion(const RootVector& axis, float angle);       // cos/sin(angle/2)
RootVector camera_euler_quaternion(float x, float y, float z);                // 0027f444 block
// The same block with the game's cosf/sinf (00121688/00121928), as the aim
// view uses it (the normal path keeps the host trig of earlier checkpoints).
RootVector camera_game_euler_quaternion(float x, float y, float z);
// 00281220(out, matrix) with the flag byte 004a9340 = 1: X, Y, Z angles.
RootVector camera_matrix_euler(const std::array<RootVector,3>& rows);
// 0027eb80..0027ee70: the aim view's orientation from the frame quaternion
// (sp+520) and the yaw +d78: Euler X/Y of the frame, Z = +d78, rebuilt by
// 00140e20 and converted back to a quaternion.
RootVector camera_aim_orientation(const RootVector& q, float yaw_d78);
// 001bd590(out, rows) with the flag byte 0049e9a0 = 0: Z = atan2f(-M1.x,
// M1.y), X = atan2f(M1.z, sqrt(M1.y^2 + M1.x^2)), Y = atan2f(-q0.z, q0.x)
// of M (Rz(-Z) Rx(-X)) (game trig, ACC products).
RootVector camera_matrix_euler_1bd590(const std::array<RootVector,3>& rows);
// 0027fa40(camera, sp+520, actor): with a valid lock, look = 00280070(a3 =
// 10, t0 = 0; origin node 15), e = 001bd590 of the +50 transform's rows,
// d = look - e wrapped into [-pi, pi] (0027ffc0); +d80 / +d81 hold X / Z
// with hysteresis and zero d.x / d.z; the result is the game-trig Euler
// quaternion of e + d. Without a valid lock q is returned unchanged.
RootVector camera_pick_orientation(const RootVector& q, std::uint8_t& hold_d80, std::uint8_t& hold_d81,
    std::uint64_t lock_d04, std::int32_t lock_id_d08, std::int32_t subtarget_d0c,
    const std::optional<RootVector>& node_15, const std::array<RootVector,4>& transform_50,
    const PlayerCameraLookAtHost* host);
// 0027ee98..0027f214: Euler angles e of the frame quaternion (00281220);
// the frame rebuilt with X = 0 (sp+520), the X rotation of e.x (sp+510) and
// e.x (+d70, sp+54c).
struct PlayerCameraPick { RootVector orientation, pitch_quaternion; float pitch_d70; };
PlayerCameraPick camera_pick_frame(const RootVector& q);
// 0027ea98..0027eb10: rotation about X by +d70 (game trig).
RootVector camera_aim_pitch_quaternion(float pitch_d70);
// 00280070(camera, out, actor, a3, t0 != 0, origin): nullopt (v0 = 0) without
// a lock +d04, when the object's +c differs from +d08, its +50 is 0 or it is
// not active (+2d, +2c set, +2e clear); otherwise d = +c0(+d0c) - origin
// (vsub.xyzw), length = vsqrt(x*x + y*y + 0) and out = {atan2f(d.z, length),
// 0, atan2f(-d.x, d.y)} with the game's atan2f (00121ab8). The origin is read
// only on that path; nullopt there throws.
std::optional<RootVector> camera_lock_look_at(std::uint64_t lock_d04, std::int32_t lock_id_d08,
    std::int32_t subtarget_d0c, const std::optional<RootVector>& origin, const PlayerCameraLookAtHost* host);
// 001bd590 X (pitch) and Z (yaw) results from a rotation's forward row.
float camera_forward_pitch(const RootVector& forward);
float camera_forward_yaw(const RootVector& forward);
}
