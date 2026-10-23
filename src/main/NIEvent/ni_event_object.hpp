#pragma once
// The NIE:/NIL: event objects' playback (COMBAT.md "Checkpoint 120f"):
// NIEventOBJ (vtable 004dc050) and NIEventLight (004dbed0) after the
// script's Play(NIEventOBJ) 00237960 -> virtual +114 = 0023e820.
//   tracks    the position (0023d290), switch (0023d6d0) and rotation
//             (00239a90) tracks and the event list (00238990);
//   matrices  the axis-angle rotation 00132130, the row rotation 0023b450 and
//             the parent product 0023e0a0 (world 0023de10);
//   messages  MSGGAME_NI_EVENT_SHUTTLE (key 004f7c28: Play 0 / Stop 1 /
//             Reset 2) queued to the object (003c9420 a2 = 0) and delivered
//             at the next pump (003c8de0 snapshots its queue); the receivers
//             0023b7d0 (NIE) / 00237eb0 (NIL) and the base 0023e890;
//             MSGGAME_NI_EVENT_NOTICE (004f7c30, the scripts' NINotice) is
//             offered to the scripts and otherwise ignored;
//   update    the root wrappers 0023a560 / 00237b50 (objects with a parent
//             +94 are updated by it), 0023a5a0 (NIE) / 00237b90 (NIL); the
//             steps 0023b1c0 / 0023d1d0;
//   events    type 0 Stop + notice, 1 blast 00239960 -> 0021a200, 2 sound
//             00239780 (an immediate message to the sound manager 004ffb50,
//             delivered through the host's sound hook, Checkpoint 132vo2),
//             3 notice, 4 sound stop 002395a0 (the type-2 twin with
//             0020eba0, Checkpoint 132uk2);
//   lights    an NIL that is present with its switch on holds a point light
//             of the light manager 00535d20 (00373e70 / 00179540, vtable
//             004d9200), released when absent or switched off.
// Rendering: the kind-2 particle asset's virtuals +48 / +5c / +60 go to the
// host hooks (particle_update / particle_present, Checkpoint 122d; the
// SHUTTLE reset's +60 / +5c pair to particle_reset, Checkpoint 132zf9) and its
// draw world to particle_world(); the kind-1 model asset's draw state to
// model_draw() (Checkpoint 132p), the kind-3 billboard model with its
// camera-facing world 0023e180 too (Checkpoint 132o), the kind-6 beam model
// the same way as kind 1 with its beam (0023a6f4 -> 00239ff0, the player
// box test and the BeamMsg, Checkpoint 132sx11); the submission 003ce650 is
// not drawn; asset bounds (0023a250) come from the
// host's asset_bounds hook for the kind-1 / kind-3 model assets (Checkpoint
// 132zf9) and throw for other kinds when a position needs them.
#include "animation_root.hpp"
#include "gobj_damage.hpp"
#include "level_lights.hpp"
#include "population.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace omega {

using NiMatrix = std::array<RootVector, 4>;

// 0023d290: +b0 count, +b4 keys (0x40 bytes: t, pos +10, in +20, out +30),
// +bc loop, +c0 time, +e0 the output.
struct NiPositionKey { float t{}; RootVector pos{}, in{}, out{}; };
struct NiPositionTrack {
    std::vector<NiPositionKey> keys;
    std::int32_t loop{-1};
    float time{};
    RootVector out{};
};
bool ni_position_track_step(NiPositionTrack& track, float dt);
// 0023d6d0: +c8 count, +cc keys (t, on byte), +d4 loop, +d8 time, +dc on.
struct NiSwitchKey { float t{}; bool on{}; };
struct NiSwitchTrack {
    std::vector<NiSwitchKey> keys;
    std::int32_t loop{-1};
    float time{};
    bool on{};
};
bool ni_switch_track_step(NiSwitchTrack& track, float dt);
// 00239a90: +0 value, +8 count, +c keys (t, v, in, out), +14 loop, +18 time,
// +1c active.
struct NiRotationKey { float t{}, v{}, in{}, out{}; };
struct NiRotationTrack {
    float value{};
    std::vector<NiRotationKey> keys;
    std::int32_t loop{-1};
    float time{};
    bool active{};
};
bool ni_rotation_track_step(NiRotationTrack& track, float dt);
// 00238990: +4 active, +8 time, +10 count, +14 the events (their +4 time).
// Returns the indices of the events fired (each t <= time + dt and not
// t <= time, in list order); an inactive list neither fires nor advances.
struct NiEventList { bool active{}; float time{}; std::vector<float> times; };
// Checkpoint 132sx12: the track part of the NIE end state 0023b4c0 (SetEndState(NIEventOBJ) 00237870 ->
// virtual +128): vt+12c = 0023d290 by dt = keys[+b0 - 1].t - +c0, vt+130 = 0023d6d0 by keys[+c8 - 1].t - +d8,
// then for each rotation track (+144, +164, +184) +1c = 1 and, only while its loop +14 is -1, 00239a90 by
// keys[+8 - 1].t - +18. Tracks without keys throw naming 0023b4c0 (the EE reads key -1).
void ni_end_state_tracks(NiPositionTrack& position, NiSwitchTrack& sw, std::array<NiRotationTrack, 3>& rotation);
std::vector<std::size_t> ni_event_list_step(NiEventList& list, float dt);
// 00132130(axis x, y, z, angle) with sinf 00121928 / cosf 00121688; the w
// lanes of rows 0..2 are 0 and row 3 is (0, 0, 0, 1).
NiMatrix ni_axis_angle(float x, float y, float z, float angle);
// 0023b450(A, B): rows 0..3 of A, xyz = x * B0 + y * B1 + z * B2 (w kept).
void ni_rotate_rows(NiMatrix& a, const NiMatrix& b);
// 0023e0a0(out, A, B): out[i] = A0 * B[i].x + A1 * B[i].y + A2 * B[i].z + A3 * B[i].w.
NiMatrix ni_matrix_product(const NiMatrix& a, const NiMatrix& b);
// 0023b1c0's local matrix: X(+144), Y(+164), Z(+184) degrees (each times
// 0x3c8efa35), X rotated by Y then by Z, row 3 the position.
NiMatrix ni_rotation_compose(float x, float y, float z, const RootVector& position);
// 00394850's test: the 16 floats against the identity (c.eq.s).
bool ni_identity(const NiMatrix& m);
// 0023c180 (an NIE's virtual +124): the asset +140 for kinds 1 / 3 / 6, else 0.
inline bool ni_kind_has_part(std::uint32_t kind) { return kind == 1 || kind == 3 || kind == 6; }
// 0023e22c: a point through a world (vmulax / vmadday / vmaddaz / vmaddw:
// W0 * x + W1 * y + W2 * z + W3 * w).
RootVector ni_point_product(const NiMatrix& world, const RootVector& point);
// 0023e180's rows once the camera and the position are known (Checkpoint
// 132o): dir = camera - position (vsub.xyzw) normalised as vmul.xyz, the
// 1 * x^2 + 1 * y^2 + 1 * z^2 chain, vrsqrt and vmulq.xyz (w kept); then
// 00131f00 with rows dir x (0, 0, 1, 0), dir, (dir x (0, 0, 1, 0)) x dir
// (vopmula / vopmsub, w 0) and the position. A camera at the position
// (vrsqrt of 0) throws.
NiMatrix ni_billboard_world(const RootVector& camera, const RootVector& position);
// Checkpoint 132sx11: the kind-6 beam (0023a5a0 at 0023a6f4; interpreter
// goldens, tools/ee/gen_ni_beam.py).
// 00239ff0: the asset bounds (min, max: 0023a250 -> the asset's vt +88) and
// the NIE's node world: the centre min + (max - min) / 2.0 (sub.s, div.s,
// add.s; w 1) with the dominant axis (abs.s, c.olt.s; x unless shorter than
// y or z, then y unless shorter than x or z, else z) running min -> max,
// each end through the world (vmulax / vmadday / vmaddaz / vmaddw).
struct NiBeam { RootVector start{}, end{}; };
NiBeam ni_beam_segment(const std::array<RootVector, 2>& bounds, const NiMatrix& world);
// 0023a914..0023aeb8: the segment p0 -> p1 (in the box's space) against the
// box (min, max): the vsub sign-flag outcodes (both ends beyond one plane:
// no hit; both inside: a hit), then for x, y, z (skipped where d = p1 - p0
// is 0: MAC zero flag) the crossing of each plane the ends straddle, p0 +
// (d * (plane - p0).axis) * Q with Q = 1 / d.axis (vdiv, vmulx, vmaddq),
// tested against the other two axes (c.ole.s). True: a hit.
bool ni_beam_box_hit(const std::array<RootVector, 2>& box, const RootVector& p0, const RootVector& p1);

// A drawn kind-1 model object (Checkpoint 132p). `cull` is the NIE's own
// transform component (+50, matrix +10: its world), the culler 003c8990's
// placement at 0013e2e4; `matrix` the asset's component, which 00394ad0
// draws with (nullopt: none, the VUM in world space).
// `stale`: a kind-3 object whose world was last written while it was absent (at load, 0023b120): 0023de10
// then copies its uninitialised stack buffer (0023e060), which the EE draws with in the frame the zone
// tracker makes it present, before the next update's step refreshes it. The host does not draw that frame.
struct NiModelDraw { NiMatrix cull{}; std::optional<NiMatrix> matrix; bool stale{}; };

// `stop`: a type-4 event (002395a0: 0020eba0, the STOP message) rather than type 2's PLAY (00239780: 0020f990).
struct NiSoundRecord { std::uint32_t object{}; std::string name; bool named{}; std::int32_t param{}; bool stop{}; };
struct NiNoticeRecord { std::uint32_t object{}; std::int32_t value_24{}; bool handled{}; };
struct NiLightRecord {
    std::uint32_t light{};          // the NIL id
    bool on{};                      // allocated (true) or released (false)
    std::size_t index{};            // point pool entry
    RootVector position{};
    float value_10{}, value_14{};   // the light's +10 / +14 (the NIL's +170 / +190 values)
};
struct NiBlastRecord { std::uint32_t object{}; std::int32_t damage{}; float strength{}, inner{}, outer{}; RootVector position{}; };
// A player slot the beam tests (0023a7b8): `actor` the host's handle for the
// controller's actor (+30), `node` its +50 world (matrix +10), `box` its
// vt +88 bounds (min, max) in that space.
struct NiBeamPlayer { std::uint64_t actor{}; NiMatrix node{}; std::array<RootVector, 2> box{}; };
struct NiBeamRecord { std::uint32_t object{}; std::uint64_t actor{}; std::uint32_t flags{}; bool handled{}; };
struct NiEventWorldHost {
    // 0021a200(owner, damage, 1, null, 0, 0, strength, inner, outer); the
    // owner's position (003cd260) in `position`. Empty: a blast event throws.
    std::function<void(const NiBlastRecord&)> blast;
    // The scripts' NINotice filter (003c8de0) for the object; true: handled.
    std::function<bool(std::uint32_t object, std::int32_t value_24)> notice;
    // The light manager 00535d20 (point pool). Null: an NIL needing a light throws.
    LevelLights* lights{};
    // The kind-2 particle asset (vtable 004daa10, particle/particle_asset.hpp)
    // of the object: vt+48 (dt) from 0023a5a0 before the step while the
    // switch +dc is on, vt+5c / vt+60 from the present walks 0023c010 /
    // 0023bee0 (the asset before the children). Empty: not called.
    // 0023a5cc passes node+10 to vt+48 (00394590 -> 003a2400): `node` is
    // that world matrix, or null when its rows are not valid.
    std::function<void(std::uint32_t object, float dt, const NiMatrix* node)> particle_update;
    std::function<void(std::uint32_t object, bool present)> particle_present;
    // Checkpoint 132zf9: the SHUTTLE reset 0023b960 of a kind-2 object with
    // an asset (0023bb4c): asset +6c = 0, vt+60(1) (003941a0, absent), then
    // vt+5c(1) (00394270, present: a fresh instance). Empty: the reset throws.
    std::function<void(std::uint32_t object)> particle_reset;
    // Checkpoint 132zf9: the NIE's bounds 0023a250 (+dc on, +140 set, +2d,
    // +2c set, +2e clear) return the asset's vt+88. The kind-1 / kind-3 model
    // asset (00395e40, vtable 004df970) answers 003947a0: the VUM's declared
    // bounds (vum_declared_bounds; +68 is null, as for the GOBs' 00213ac0).
    // nullopt: no bounds (003cd260 then reads the node translation). Empty:
    // a position that needs them throws naming 0023a250.
    std::function<std::optional<VumBounds>(std::uint32_t object)> asset_bounds;
    // The kind-3 billboard world 0023e180 faces the camera object of list
    // 004ffc14 (00159570 / 00159560): the translation (+40) of its +50
    // transform, or nullopt when the list has no object (0023e180 then
    // writes no world). Empty: a present kind-3 world is unhosted (its rows
    // report it and throw where they are needed).
    std::function<std::optional<RootVector>()> camera_position;
    // 00239780: 0020f990(msg, event +10, owner = the NIE) with the descriptor
    // 002397f0 built at load (001ca010(type = param, name, NIE, 0)), +20 = 1,
    // then 003c9420(msg, 004ffb50, 1): delivered to the sound manager at
    // once. Type 4 (Checkpoint 132uk2; goldens tools/ee/gen_ni_sound_event.py):
    // 00239610 builds the same descriptor (vtable 004dc008), and its fire
    // 002395a0 sends 0020eba0 (STOP) the same way (record `stop`). Empty: a
    // sound event throws.
    std::function<void(const NiSoundRecord&)> sound;
    // Checkpoint 132sx11: the kind-6 beam of a present object (+2d; +2c set,
    // +2e clear) whose bounds 0023a250 exist (switch +dc on, asset +140).
    // beam_asset_bounds: the asset's vt +88 (003947a0: the VUM's declared bounds).
    // beam_player: 003fb8d0(slot) for slots 0..3, nullopt unless the object
    // has a controller (+58) whose vt +54 is nonzero and vt +88 & 0xff is not
    // 4 (0023a7c4..0023a80c). beam_hit: 00188140(actor, start, end - start,
    // mode 0, record +4c = *0048ae20 = 0.25), its flags. beam: the BeamMsg
    // (key 00506210, +24 the actor, byte +20 1 in single player) queued to
    // the object (003c9420 a2 = 0) when the flags are nonzero, offered at the
    // next pump to the scripts (003c8de0; the receiver 0023b7d0 passes any
    // key but SHUTTLE to the base 0023e890 -> 003cea10); true: handled.
    // Empty: such an object throws at its update.
    std::function<std::array<RootVector, 2>(std::uint32_t object)> beam_asset_bounds;
    std::function<std::optional<NiBeamPlayer>(int slot)> beam_player;
    std::function<std::uint32_t(std::uint64_t actor, const RootVector& origin, const RootVector& direction)> beam_hit;
    std::function<bool(std::uint32_t object, std::uint64_t actor)> beam;
};
class NiEventWorld {
public:
    // The NIL: records first (they are loaded before the NIE: records that
    // name them as children), then the NIE: records with their children.
    NiEventWorld(const Population& population, NiEventWorldHost host);
    // Script natives (0 returned): Play 00237960 (+114 = 0023e820, mode 0),
    // Stop 00237930 (+118 = 0023e790 with 1, mode 1), 00237900 (+11c =
    // 0023e720, Reset, mode 2). Unknown ids throw.
    void play(std::uint32_t id);
    void stop(std::uint32_t id);
    void reset(std::uint32_t id);
    // Checkpoint 132sx12: SetEndState(NIEventOBJ) 00237870 -> virtual +128 = 0023b4c0, at once (no message):
    // +130 = 1, ni_end_state_tracks, then (kind != 3) the local matrix of 0023b618..0023b754 (the
    // 0023b2d0 compose: X +144, Y +164, Z +184 degrees, row 3 +e0), virtual +120, and each child's
    // virtual +128 in +a4 order. An NIL child (00237a20) throws.
    void end_state(std::uint32_t id);
    // Checkpoint 132uk3: Disable / Enable(NIEventOBJ) 002378a0 / 002378d0 (`lw a0,(a0)`, virtual +68 / +64 with
    // a1 = -1, return 0) -> 0023bdc0 / 0023be50: virtual +2c / +28 (-1) = 0023bcc0 / 0023bd40 sets +2c = 0 / 1
    // and then calls each child's (+a4, +a0 of them, 0023ce68 / 0023ce90) virtual +2c / +28 (-1) in order: an
    // NIE child recurses, an NIL child's 0012ed40 / 0012ed30 return at once. The message that follows is built
    // and sent only when networked (005721c8), so single player stops there. Unknown ids throw.
    // +2c is read by the kind-3 step gate (0023a620), the beam (0023a704), the bounds 0023a250 (and so the
    // position 003cd260 and the model COL 0023fed0) and the draw walk 0013e270.
    void set_enabled(std::uint32_t id, bool enable);
    bool enabled(std::uint32_t id) const;
    // Snapshots the queued SHUTTLE / NOTICE messages (003c8de0); returns
    // their count. deliver() delivers exactly those; messages queued while
    // delivering wait for the next take_pending().
    std::size_t take_pending();
    void deliver();
    // 0023a560 / 00237b50 for every object in load order (roots only).
    void update(float dt);
    // 00224330 / 00224180 on a member: virtual +5c(1/0) -> 0023c100 walks to
    // the root and 0023c010 sets +2d on the whole tree. False: unknown id.
    bool set_present(std::uint32_t id, bool present);
    bool contains(std::uint32_t id) const;
    bool playing(std::uint32_t id) const;
    bool present(std::uint32_t id) const;
    bool switched_on(std::uint32_t id) const;
    float event_time(std::uint32_t id) const;
    // 003cd260 of the object: the node row 3 (throws where unhosted).
    RootVector position(std::uint32_t id) const;
    // The node matrix row 3 regardless of the asset (throws when invalid).
    RootVector node_position(std::uint32_t id) const;
    // Checkpoint 132cp6: the NIE's own transform component (+50, matrix +10) as last written, the owner
    // the collision gathers 001815e0 / 003c4f00 read (00183220) for its model COL. Null while a row is
    // not established; node_reason() then says why.
    std::optional<NiMatrix> node_world(std::uint32_t id) const;
    std::string node_reason(std::uint32_t id) const;
    // Checkpoint 132zf11 (the script tank, tank.hpp): +94, the parent's ID
    // (nullopt for a root); +144 / +164 / +184, the rotation track values
    // (axis 0..2); 0023e3d0(node, speed): +134 = speed on the node and every
    // descendant of its +9c child list (the function recurses); and the
    // tank's writes of the +f0 local rows 0..2 (x, y, z, w; row 3 kept)
    // followed by vt +120 (0023a2d0 -> 0023dda0: the node world, node_update).
    std::optional<std::uint32_t> parent_id(std::uint32_t id) const;
    float rotation_value(std::uint32_t id, int axis) const;
    void set_speed(std::uint32_t id, float speed);
    void set_local_rows(std::uint32_t id, const std::array<RootVector, 3>& rows);
    // The draw walk 0013e250 for a kind-2 object: drawn when present (+2d)
    // with +2c set, +2e clear and +2f set (003cf0e0 / 0023c1c0 leave them 1,
    // 0, 1) and the switch +dc on (00239fa0); the asset then draws with the
    // NIE's node world (0023a250 -> asset vt+94 / +ac). Null when not drawn;
    // throws when the world rows are not established.
    std::optional<NiMatrix> particle_world(std::uint32_t id) const;
    // The draw walk 0013e250 for a kind-1 model object (+49 0x10; virtual
    // +124 = 0023c180 returns the asset +140 for kinds 1 / 3 / 6): filed
    // when present (+2d; +2c, +2e, +2f as above) and its bounds 0023a250
    // exist (switch +dc on, asset loaded by 0023c3c0); 00239fa0 then draws
    // the asset (vt +58 = 00394ad0). The asset's transform component: the
    // constructor's asset vt +90 = 00394850 (0023d0ac) with the local matrix
    // keeps one only for a non-identity matrix (003cdff0; identity:
    // 003cdf20), and 0023a2d0 (vt +120) copies the NIE's world into it
    // (asset vt +94 / +ac). Null when not drawn; throws for other kinds and
    // when the world rows are not established. Kind 3 (0023c180 returns its
    // asset too) is drawn the same way with its billboard world (0023de10 ->
    // 0023e180, refreshed by the update's step). Kind 6 (0023c610 loads its
    // asset as kind 1's, 0023c3c0) draws as kind 1.
    std::optional<NiModelDraw> model_draw(std::uint32_t id) const;
    // Checkpoint 132uk6: the asset +140 of a kind-1 / kind-3 / kind-6 NIE (0023c180)
    // as the blood decals read it: whether it exists, and its +50 component (the
    // NIE's node world that 0023a2d0 copies in, when the constructor's 00394850
    // gave it one; null without one). Throws for other kinds, and for a
    // component whose rows are not established.
    bool has_asset(std::uint32_t id) const;
    std::optional<NiMatrix> asset_world(std::uint32_t id) const;
    std::uint32_t kind(std::uint32_t id) const;
    std::optional<std::size_t> light_index(std::uint32_t id) const;
    std::vector<std::uint32_t> ids() const;
    std::size_t objects() const { return nodes_.size(); }
    std::size_t autoplay() const { return autoplay_; }
    std::size_t pending() const { return queue_.size(); }
    std::vector<NiSoundRecord> take_sounds();
    std::vector<NiNoticeRecord> take_notices();
    std::vector<NiLightRecord> take_lights();
    std::vector<NiBlastRecord> take_blasts();
    std::vector<NiBeamRecord> take_beams();
    // The light manager, once the level's lights exist (after construction).
    void set_lights(LevelLights* lights) { host_.lights = lights; }
private:
    struct Event {
        std::int32_t type{};
        std::vector<float> values;
        std::string sound;
        bool named{};
        std::int32_t param{};
    };
    struct Node {
        bool light{};                                   // NIL (kind 7)
        std::uint32_t id{}, kind{};
        bool asset{};                                   // NIE +140 non-null
        bool asset_matrix{};                            // the asset's +50 component (0023d0ac: 00394850)
        int parent{-1};
        std::vector<int> children;
        NiPositionTrack position;
        NiSwitchTrack sw;
        std::array<NiRotationTrack, 3> rotation;        // NIE +144/+164/+184, NIL +170/+190
        NiEventList list;
        std::vector<Event> events;
        bool playing_130{}, present_2d{};
        bool enabled_2c{true};                          // 003cf0e0 sets +2c = 1; Enable / Disable (132uk3)
        float speed_134{1.0F};
        NiMatrix local{};
        std::array<bool, 4> local_rows{};               // false: uninitialised stack rows
        NiMatrix node{};
        std::array<bool, 4> node_rows{};
        std::string node_reason;                        // why the invalid rows are invalid
        std::optional<NiMatrix> node_written;           // the last world 0023de10 wrote (all rows established)
        std::array<float, 3> colour{};                  // NIL +144..+14c
        bool byte_150{}, byte_151{};
        std::optional<std::size_t> light_140;           // NIL +140: the point pool entry
    };
    struct Message { int node{}; bool notice{}; int mode{}; bool byte_20{}; std::int32_t value_24{}; bool beam{}; std::uint64_t actor{}; std::uint32_t flags{}; };
    int find(std::uint32_t id) const;
    int need(std::uint32_t id, const char* what) const;
    void queue_shuttle(int node, int mode, bool byte_20);
    void queue_notice(int node, std::int32_t value);
    void receive(const Message& message);
    void step(int n, float dt);
    void fire(int n, std::size_t event);
    void node_update(int n);                            // virtual +120 = 0023dda0
    void world(int n, NiMatrix& out, std::array<bool, 4>& rows, std::string& reason) const;
    void update_node(int n, float dt);
    void end_state_node(int n);
    void update_beam(int n);
    void update_light(int n);
    void release_light(int n);
    std::vector<Node> nodes_;
    std::unordered_map<std::uint32_t, int> index_;
    NiEventWorldHost host_;
    std::vector<Message> queue_, delivering_;
    std::vector<NiSoundRecord> sounds_;
    std::vector<NiNoticeRecord> notices_;
    std::vector<NiLightRecord> lights_;
    std::vector<NiBlastRecord> blasts_;
    std::vector<NiBeamRecord> beams_;
    std::size_t autoplay_{};
};

}
