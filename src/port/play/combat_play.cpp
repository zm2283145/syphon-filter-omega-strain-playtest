#include "combat_play.hpp"
#include "actor_effect_slots.hpp"
#include "actor_object_action.hpp"
#include "weapon_field_of_view.hpp"
#include "bullet_hit.hpp"
#include "gobj_damage.hpp"
#include "morph_resources.hpp"
#include "npc_alert.hpp"
#include "npc_world.hpp"
#include "object_dynamic_id.hpp"
#include "pickup_menu_play.hpp"
#include "player_weapon_switch.hpp"
#include "port_cheats.hpp"
#include "skeleton_pose.hpp"
#include "water_effects.hpp"
#include "world_los.hpp"
#include <algorithm>
#include <bit>
#include <cstdio>
#include <map>
#include <source_location>
#include <stdexcept>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Combat play: "+what);}
std::string hex64(std::uint64_t v){char b[24];std::snprintf(b,sizeof b,"%llx",static_cast<unsigned long long>(v));return b;}
std::string hex_word(std::uint32_t v){
    static constexpr char digits[]="0123456789abcdef";
    std::string s(8,'0');
    for(int i=7;i>=0;--i){s[static_cast<std::size_t>(i)]=digits[v&0xfU];v>>=4;}
    return s;
}
TargetingMatrix rows_of(const std::array<float,16>& m){
    TargetingMatrix f{};
    for(std::size_t r=0;r<4;++r)f[r]={m[4*r],m[4*r+1],m[4*r+2],m[4*r+3]};
    return f;
}
// 00188d30 (actor virtual +cc): the target size factor by subtarget & 3.
float target_size_cc(std::int32_t subtarget){
    switch(static_cast<std::uint32_t>(subtarget)&3U){
    case 0:return std::bit_cast<float>(0x3e99999aU);     // 0048a2a8 (0.3)
    case 1:case 2:return std::bit_cast<float>(0x3f000000U);   // 0048a2b0 (0.5)
    default:return 1.0F;
    }
}
}

CombatActorModel make_combat_actor_model(const SkeletonDefinition& skeleton,std::vector<HitVolumePart> parts,
    const SkinnedMesh* hard_mesh,std::span<const SkinMatrix> bone_scales){
    CombatActorModel m;
    m.parts=std::move(parts);
    for(const auto& bone:skeleton_bind_pose(skeleton)){
        ShotFrame f{};
        for(std::size_t r=0;r<4;++r)f[r]={bone.inverse_bind[4*r],bone.inverse_bind[4*r+1],bone.inverse_bind[4*r+2],bone.inverse_bind[4*r+3]};
        m.inverse_bind.push_back(f);
    }
    if(hard_mesh){
        std::vector<ShotFrame> scales;
        for(const auto& s:bone_scales){
            ShotFrame f{};
            for(std::size_t r=0;r<4;++r)f[r]={s[4*r],s[4*r+1],s[4*r+2],s[4*r+3]};
            scales.push_back(f);
        }
        ModelAttachSlots slots{};
        std::optional<std::span<const ShotFrame>> scale_span;
        if(!scales.empty())scale_span=std::span<const ShotFrame>(scales);
        hard_mesh_attach_points(*hard_mesh,m.inverse_bind,scale_span,slots);
        m.attach=slots;
    }
    return m;
}

struct CombatPlaySession::Impl {
    ActorWorld& world;
    const WeaponDatabase& db;
    CombatPlayConfig config;
    std::unique_ptr<ActorWorldCombat> actors;
    std::unique_ptr<CombatWorld> combat;
    TargetVisibility visibility;
    std::vector<TargetHandle> registered;          // 004f77c0 +11c
    // The node objects (CombatPerceptionWorld::nodes): a script's SetTarget(NPC,
    // Node) registers one with the target manager (0016ddb0 -> 0022c650).
    const NpcNodeGraph* nodes{};
    std::function<std::optional<CombatHostObject>(std::uint64_t)> host_objects;
    HudTargets hud;
    PadWeaponState pad;
    PlayerTargetingState state;
    std::vector<TargetHandle> recent;              // input +ec
    std::vector<float> shake;                      // actor +352c
    std::int32_t fp{};
    std::vector<CombatBackpack> backpacks;
    CombatPlayInput input;
    bool in_frame{};
    CombatPlayFrame frame;
    // The actor actions sent with transport 0 that the session handles, in
    // send order, for the next pump (001a92a0): fire requests (0x3b kind 0xb,
    // the pad block and the NPC gate) and the NPCs' aim / look actions.
    struct PendingAction {
        std::size_t actor{};
        // fire: the pad block's request (its target a resolved handle);
        // ai_fire: the NPC gate's, whose +30 word (target_id; -1 the none
        // word) is resolved at the pump (001aa000 -> 0016f7e0 / 0040c540);
        // reload: action 0x3c (001aa048) from the AI or a discharge.
        enum class Kind : std::uint8_t {fire,ai_fire,aim,reload} kind{};
        ActorFireRequest fire;
        ActorAimMessage aim;
    };
    std::deque<PendingAction> pending;
    // The AI controllers' fire notices +cc (0014f460) by actor.
    std::vector<std::function<void(std::uint64_t,bool)>> fire_notices;
    std::unique_ptr<CombatThrows> throws;          // CombatPlayConfig::throws
    // CombatPlayConfig::backpack_world: the list *004f7f30, the clock *004f7f70
    // (0 at the session start) and, per actor, whether a backpack destroy
    // 00254770 wrote its +3464 = 0 since its last drop.
    BackpackObjectList backpack_list;
    std::uint32_t backpack_clock{};
    std::vector<BackpackCrateSnapshot> crate_snapshots;   // *004f7f48 (00256b40)
    std::map<std::size_t,bool> owner_cleared_3464;
    // The flashlight objects +33ec (roles 1 / 3) by actor, their lit spots'
    // IDs (0040c240 category 0xe7 over session-local counters), the local
    // player's pad state (input +a6 / +104), the Flash* variables and the
    // perception world (the think list and the Sight delivery).
    std::vector<std::optional<Flashlight>> flashlights;
    std::vector<std::int32_t> spot_ids;
    ObjectIdCounters spot_counters{};
    FlashlightPadState flash_pad;
    PlayerWeaponSwitchState weapon_switch;   // the local player's input +a4 / +108 / +10c
    FlashlightTuning flash_tuning;
    std::shared_ptr<CombatPerceptionWorld> perception;
    // Checkpoint 132t: the pickup menu 00253070 (component +178, input
    // +128) that the backpack receiver 001762c0 built, its backpack (an
    // index into `backpacks`) and its 0036b180 token; the panel globals
    // 004f7e90 / 004f7e98 and the screen host's previews and records.
    struct OpenPickupMenu {PickupMenu menu;std::size_t backpack{};std::uint32_t token{};};
    std::optional<OpenPickupMenu> pickup_menu;
    PickupMenuPanels pickup_panels;
    PickupMenuPlayState pickup_state;

    Impl(ActorWorld& w,const WeaponDatabase& d,CombatPlayConfig c):world(w),db(d),config(std::move(c)){}

    // 001b6678..001b6724 (single player): the record's item words, then 00143c50.
    void add_record_items(std::size_t a){
        if(!config.record_items.at(a))return;
        std::vector<WeaponInventoryEvent> ev;
        auto ops=combat->inventory_ops(a,*config.increased_ammo,ev);
        for(const auto word:*config.record_items[a])
            if(word!=0xffffffffU)ops.add(static_cast<std::int32_t>(word),true,false);
        ops.equip_first();
        combat->check_events(ev);
    }
    void hook(std::size_t a){
        world.actor(a).set_presence_hook([this,a](bool active){presence(a,active);});
        world.actor(a).set_inventory_update([this,a](float dt){inventory_stage(a,dt);});
        // The death action's 001977f0(actor, 0) with a held item, then 001cb120.
        world.actor(a).set_death_secondary_hooks([this,a]{return mode_input(a,0);},[this,a](std::uint8_t mode){
            auto& st=actors->secondary(a).states;apply_actor_secondary_mode(st.request_312,st.request_313,mode);});
        // Actions 0x40 / 0x41 (the flashbang hide) on the actor's +3314.
        world.actor(a).set_secondary_component([this,a]{return &actors->secondary(a);});
        // Checkpoint 132fx3: 00199f30's armed test and pose type (solo_actor_simulation.hpp set_equipment_hook).
        world.actor(a).set_equipment_hook([this,a]()->std::pair<bool,std::uint8_t>{
            const auto& inv=inventory(a);
            bool armed=false;
            if(world.actor(a).player_controller()){                                   // 001434d0
                for(const auto& slot:inv.slots)if(slot.item!=-1){armed=true;break;}
            }else armed=inv.current_84!=weapon_no_slot;                               // InvArmed: +84 != 6
            return {armed,item_kind(a)};                                              // 00143510
        });
        // 001b5560 on activation: the new +3314, +3524 +a0 = 0, 001977f0(actor, 0).
        world.actor(a).set_activation_hook([this,a]{
            actors->recreate_secondary(a);
            combat->inventory(a).reloading_a0=false;
            return actors->apply_secondary_mode(a,mode_input(a,0)).flag_31b2;
        });
        // 001b5190: 00185d40(actor, k, off, 1.0) for the three effect slots.
        // Without the throw flow no slot can hold an effect (the slots are
        // CombatThrows'), so only kind 0's unconditional part remains: +326a = 0
        // and the G_TASER_BODY sound stop (recorded).
        world.actor(a).set_deactivation_hook([this,a]{
            if(throws){for(std::uint32_t k=0;k<3;++k)throws->effect_off(a,k);return;}
            if(world.actor(a).health().burning_326a)world.actor(a).set_burning_326a(false);
            frame.notes.push_back("00185d40 actor "+std::to_string(a)+": slots off, sound stop "+std::string(actor_effect_stop_sound));
        });
    }

    std::size_t index(std::uint64_t object,std::source_location at=std::source_location::current()) const{
        for(std::size_t a=0;a<world.size();++a)if(world.actor(a).handle()==object)return a;
        fail("object 0x"+hex64(object)+" is not an actor of the world (combat_play.cpp:"+std::to_string(at.line())+")");
    }
    std::optional<std::size_t> find_index(std::uint64_t object) const{
        for(std::size_t a=0;a<world.size();++a)if(world.actor(a).handle()==object)return a;
        return std::nullopt;
    }
    const NpcNode* node_at(std::uint64_t object) const{
        if(!nodes)return nullptr;
        const auto i=nodes->node_of_object(object);
        return i?&nodes->nodes.at(static_cast<std::size_t>(*i)):nullptr;
    }
    // A flashlight's lit spot (0045bad0: a 0x60-byte base world object,
    // vtable 004dff50) has the session handle 0xf0000000 + its owner's index.
    static constexpr std::uint64_t spot_tag=0xf0000000ULL;
    static std::uint64_t spot_handle(std::size_t a){return spot_tag+a;}
    std::optional<std::size_t> spot_owner(std::uint64_t object) const{
        if((object&~0x0fffffffULL)!=spot_tag)return std::nullopt;
        const auto a=static_cast<std::size_t>(object&0x0fffffffULL);
        if(a>=flashlights.size()||!flashlights[a])return std::nullopt;
        return a;
    }
    // 003cd260 (+e4, also the spot's +bc 00153860 and +c0 0014d340): the
    // translation of its +50 transform.
    RootVector spot_position(std::size_t a) const{return flashlights.at(a)->spot_position;}
    std::int32_t object_id(std::uint64_t object) const{
        if(const auto s=spot_owner(object))return spot_ids.at(*s);
        if(const auto* n=node_at(object))return static_cast<std::int32_t>(n->id);   // +c
        if(!find_index(object)&&host_objects)if(const auto g=host_objects(object))return g->id;   // a GOB's +c
        return config.object_ids.at(index(object));
    }
    // The play diagnostic: the box block (00170e20) is recorded as skipped.
    static const WeaponBackpackHost& backpack_host(){
        static const WeaponBackpackHost h=[]{WeaponBackpackHost x;x.box_not_simulated=true;return x;}();
        return h;
    }
    CombatBackpack& backpack(std::uint64_t handle){
        for(auto& b:backpacks)if(b.handle==handle)return b;
        fail("not a backpack of the session");
    }
    // CombatPlayConfig::backpack_world addresses: a backpack 0xb0000000 + n
    // (= its handle), its transform 0xb8000000 + n, an actor 0xa0000000 | its
    // index, a box 0xc0000000 | the player's hotbox index.
    static constexpr std::uint32_t actor_address_tag=0xa0000000U;
    static constexpr std::uint32_t box_address_tag=0xc0000000U;
    std::size_t actor_at(std::uint32_t address) const{
        if((address&0xf0000000U)!=actor_address_tag||(address&0x0fffffffU)>=world.size())
            fail("an owner +300 that is not an actor address of the session");
        return address&0x0fffffffU;
    }
    CombatBackpack& backpack_at(std::uint32_t address){
        for(auto& b:backpacks)if(address!=0&&b.address==address)return b;
        fail("not a backpack address of the session");
    }
    std::size_t box_index(std::uint32_t box) const{
        if((box&0xf0000000U)!=box_address_tag)fail("not a backpack box of the session");
        return box&0x0fffffffU;
    }
    // An actor as the backpack reads it (+c, root +2c..+2e, +3468, +3469,
    // 004704f0(+3528)). +3468 is the actor's SetPickupable byte (0014a890,
    // checkpoint 132zf7); +3469 its disguise byte (the AI virtual +1d8
    // 0014ac90: SetHasDisguise and the kind-4 take, checkpoint 132sx10). +3528 is
    // the actor's item list (CombatPlayConfig::item_list); unset, the empty
    // list 001b6a60 builds (001b6084..001b60d0: +4 count 0), so 004704f0
    // returns 0.
    // A backpack keeps its owner +300 after 0040bf50 deleted that actor; the
    // readers test +304 against the freed block's +c first. The root
    // destructor 003cb330 / 003cad70 left +c = f0fffff0 and the free
    // 0036bc10 keeps the user words (freed_ai_resume goldens), so the owner
    // is never valid and nothing else of it is read. The port assumes the
    // block was not handed to a later allocation (Checkpoint 132zf5).
    static constexpr std::uint32_t freed_actor_c=0xf0fffff0U;
    WeaponBackpackOwner owner_view(std::size_t a) const{
        WeaponBackpackOwner o;
        if(world.destroyed(a)){o.key_c=static_cast<std::int32_t>(freed_actor_c);return o;}
        o.key_c=config.object_ids.at(a);o.root=world.actor(a).root_bytes();o.pickupable_3468=world.actor(a).pickupable_3468();
        o.byte_3469=world.actor(a).disguise_3469();
        if(config.item_list)o.items_3528=config.item_list(o.key_c).any();
        return o;
    }
    // 001a32a0's actor: the floor root placement as +f4. The inactive branch
    // 001a3448 reads +3598, then the +58 controller's virtual +5c: every
    // controller vtable of the play (the AI 004d9380, the player 004d9cf0 and
    // 004d9890 / 004dba20 / 004de530 / 004e06c0) has +5c = 001535d0, which
    // returns 0, so +3584 is never read and the row stays the +f4 row 3
    // whatever +3598 holds (left clear).
    BackpackDropActor drop_actor(std::size_t a) const{
        BackpackDropActor d;
        d.address=actor_address_tag|static_cast<std::uint32_t>(a);
        d.view=owner_view(a);
        d.matrix_f4=root_placement_matrix(world.actor(a).floor_root_placement());
        d.controller_5c=false;
        return d;
    }
    // 00170e20(*004ffbd0, bp, 0, 1) for the play: 0040c240(&id, 0xf4), the box
    // type 0x12 (001734f0), the bounds 001710a0 (the transform's virtual +58
    // = 00132900 clears its dirty byte +50; translation = the matrix row 3
    // at T +40; the gearpack model's +3e0 box via 00253fb0, halved on every
    // lane), the list append 00171020, 0040c9d0 and the link 00171c40 to the
    // backpack, in the player's hotbox world.
    std::uint32_t create_backpack_box(CombatBackpack& b){
        const auto& w=*config.backpack_world;
        if(!w.allocate_id)fail("00170e20: the object ID allocator 0040c240 is not supplied");
        const std::uint32_t id=w.allocate_id(0xf4);
        b.transform.set_byte(0x50,0);
        const RootVector t{std::bit_cast<float>(b.transform.word(0x40)),std::bit_cast<float>(b.transform.word(0x44)),
            std::bit_cast<float>(b.transform.word(0x48)),std::bit_cast<float>(b.transform.word(0x4c))};
        const auto bounds=npc_interactable_bounds(t,w.model_minimum,w.model_maximum,false);
        const auto index=world.actor(config.player).add_object_hotbox(id,b.object.word(0xc),bounds);
        b.box=index;b.box_bounds=bounds;
        return box_address_tag|static_cast<std::uint32_t>(index);
    }
    BackpackObjectHost object_host(){
        const auto& w=*config.backpack_world;
        BackpackObjectHost h;
        h.object=[this](std::uint32_t address)->WeaponBackpack&{return backpack_at(address).object;};
        h.owner=[this](std::uint32_t address){return owner_view(actor_at(address));};
        h.containing_region=w.containing_region;h.nearest_region=w.nearest_region;h.line_of_sight=w.line_of_sight;
        h.game_class_5468=w.game_class_5468;
        h.create_box=[this](std::uint32_t address){return create_backpack_box(backpack_at(address));};
        h.attach_rider=w.attach_rider;
        h.model_bounds=[&w]{return std::array<RootVector,2>{w.model_minimum,w.model_maximum};};
        h.set_box_bounds=[this](std::uint32_t box,const RootVector& lo,const RootVector& hi){
            const auto index=box_index(box);
            world.actor(config.player).set_hotbox_bounds(index,lo,hi);
            for(auto& o:backpacks)if(o.box&&*o.box==index)o.box_bounds=NpcInteractableBounds{lo,hi};
        };
        return h;
    }
    // taker (checkpoint 132pw1): the PORT-ONLY CPU partner at the backpack: its virtual +c0, and no DualWield unlock
    // (00332f90(.., 1) of its own agent: the partner has no agent record).
    WeaponBackpackHost weapon_host(std::uint32_t address,WeaponInventoryOps* trigger_inventory=nullptr,
                                   std::optional<std::size_t> taker=std::nullopt){
        WeaponBackpackHost h;
        h.create_box=[this,address]{return create_backpack_box(backpack_at(address));};
        h.owner=[this](std::uint32_t a){return owner_view(actor_at(a));};
        const auto who=taker.value_or(config.player);
        h.position_c0=[this,who]{return actors->eye_c0(who,-1);};
        if(taker)h.dual_unlock=[]{return false;};
        else h.dual_unlock=[this]{
            if(!config.agent_unlock_1)fail("00254da0: the agent-data unlock 1 (00332f90) is not supplied");
            return *config.agent_unlock_1;};
        // 001762c0's Triangle confirm object (only the player's trigger reaches it).
        if(trigger_inventory)h.open_menu=[this,address,trigger_inventory](std::uint32_t owner_check){
            return open_pickup_menu(address,*trigger_inventory,owner_check);};
        return h;
    }
    // The life-cycle events of one backpack from index `from`, applied to the
    // play's world: +2d / 0040c9d0 / 0040c9a0, the box words and the box
    // removal 00170db0 in the player's hotbox world, the owner's +3464.
    void apply_object_events(CombatBackpack& b,std::size_t from){
        for(std::size_t i=from;i<b.object_events.size();++i){
            const auto e=b.object_events[i];
            using K=BackpackObjectEvent::Kind;
            switch(e.kind){
            case K::register_object:if(e.a==b.address)b.registered=true;break;
            case K::box_word:world.actor(config.player).set_hotbox_word(box_index(e.a),e.b,e.c);break;
            case K::remove_box:   // also another backpack's: the constructor's eviction 00257b7c
                world.actor(config.player).remove_object_hotbox(box_index(e.a));
                for(auto& o:backpacks)if(o.box&&*o.box==box_index(e.a))o.box.reset();
                break;
            case K::unregister:if(e.a==b.address)b.registered=false;break;
            case K::clear_owner_3464:owner_cleared_3464[actor_at(e.a)]=true;break;
            default:break;
            }
        }
    }
    void destroy_backpack(CombatBackpack& b){
        const auto from=b.object_events.size();
        backpack_object_destroy(b.address,backpack_list,object_host(),b.object_events);   // 00254770
        b.destroyed=true;
        apply_object_events(b,from);
        // 00254974: 003cb230(bp, 0.1), the deferred delete (Checkpoint 132el6: hosted when the play supplies it).
        if(const auto& release=config.backpack_world->release_3cb230)
            release(b.address,[this,address=b.address]{backpack_at(address).deleted=true;});
    }
    // The contents' events from index `from`: 003ce980 (+2d = 1), 0040c9d0,
    // the box words and removal, and the destroy 00254770 at check_empty.
    void apply_weapon_events(CombatBackpack& b,std::size_t from){
        for(std::size_t i=from;i<b.events.size();++i){
            const auto e=b.events[i];
            using K=WeaponBackpackEvent::Kind;
            switch(e.kind){
            case K::activate_5c:                         // 003ce980(bp, 1)
                b.object.set_byte(0x2d,1);
                if(b.object.word(0x58)!=0&&b.object.byte(0x2c)!=0)fail("003ce980 with a +58 component (its virtual +48) is not ported");
                break;
            case K::register_40c9d0:b.registered=true;break;
            case K::box_word:world.actor(config.player).set_hotbox_word(box_index(e.a),e.b,e.c);break;
            case K::remove_box:
                world.actor(config.player).remove_object_hotbox(box_index(e.a));
                if(b.box&&*b.box==box_index(e.a))b.box.reset();
                break;
            case K::destroy:destroy_backpack(b);break;
            default:break;
            }
        }
    }
    // 00257640(actor, 1, 0) (backpack_object.hpp) for a dead NPC's drop
    // (001a3520): the backpack is appended to the session first so the hosts
    // find it by address; the drop matrix is the actor's virtual +f4
    // (00132280, the floor root placement as elsewhere in the play).
    std::uint64_t construct_backpack(std::size_t a){
        const auto& w=*config.backpack_world;
        if(!w.allocate_id)fail("00257640: the object ID allocator 0040c240 is not supplied");
        const auto n=backpacks.size();
        if(n>=0x08000000U)fail("too many backpacks for the session's addresses");
        backpacks.emplace_back();
        auto& b=backpacks.back();
        b.actor=a;b.address=0xb0000000U+static_cast<std::uint32_t>(n);b.handle=b.address;
        BackpackObjectCreate c;
        c.object=&b.object;c.address=b.address;c.transform=&b.transform;
        c.transform_address=0xb8000000U+static_cast<std::uint32_t>(n);
        c.id_40c240=w.allocate_id(0xf5);
        c.clock_4f7f70=backpack_clock;
        BackpackDropActor d=drop_actor(a);
        backpack_object_construct(c,d,backpack_list,object_host(),b.object_events,db);
        owner_cleared_3464[a]=false;
        apply_object_events(b,0);
        frame.backpacks.push_back(n);
        return b.handle;
    }
    // The actor's +3464: its last dropped backpack until a destroy 00254770
    // cleared the word (owner_cleared_3464).
    CombatBackpack* carried_backpack(std::size_t a){
        const auto it=owner_cleared_3464.find(a);
        if(it==owner_cleared_3464.end()||it->second)return nullptr;
        for(auto b=backpacks.rbegin();b!=backpacks.rend();++b)
            if(!b->crate&&b->actor==a&&!b->destroyed)return &*b;
        return nullptr;
    }
    // Loader_LoadInventoryDefs 001c2a10 ("INV:"): per record 00257480,
    // Mem_Alloc(0, 0x318), cBackpack_ctor 00257f30 and 00257620(*004f7f30,
    // &bp), the append to the backpack list (single player: nothing else).
    // The constructor allocates no ID (its +c is the record's); its slot adds
    // 00255af0 and hotbox 00170e20 (0040c240(&id, 0xf4)) are the session's.
    std::vector<std::size_t> construct_crates(const std::vector<BackpackInvRecord>& records,
                                              const std::function<std::uint32_t(std::uint32_t,std::uint32_t)>& find_collision){
        if(!config.backpack_world)fail("001c2a10: the INV: crates need CombatPlayConfig::backpack_world");
        std::vector<std::size_t> out;
        for(const auto& r:records){
            const auto n=backpacks.size();
            if(n>=0x08000000U)fail("too many backpacks for the session's addresses");
            backpacks.emplace_back();
            auto& b=backpacks.back();
            b.crate=true;b.address=0xb0000000U+static_cast<std::uint32_t>(n);b.handle=b.address;
            BackpackObjectCreate c;
            c.object=&b.object;c.address=b.address;c.transform=&b.transform;
            c.transform_address=0xb8000000U+static_cast<std::uint32_t>(n);
            c.clock_4f7f70=backpack_clock;
            BackpackInvHost h;
            h.add_weapon=[this,address=b.address](WeaponBackpack&,std::int32_t item,std::int32_t spare,std::int32_t clip,std::uint8_t flag){
                auto& x=backpack_at(address);
                const auto from=x.events.size();
                const auto host=weapon_host(address);
                WeaponBackpackOps(x.object,db,host,x.events).add_weapon(item,spare,clip,flag);
                x.weapons.push_back({item,spare,clip});
                apply_weapon_events(x,from);
            };
            h.find_collision=find_collision;
            h.create_box=[this](std::uint32_t address){return create_backpack_box(backpack_at(address));};
            backpack_object_construct_inv(c,r,h,b.object_events,db);
            auto& done=backpacks.back();
            if(done.object.value(0x2e4)!=0)done.armor=std::array<std::int32_t,2>{done.object.value(0x2e8),done.object.value(0x2ec)};
            apply_object_events(done,0);
            backpack_list.data.push_back(done.address);   // 00257620
            out.push_back(n);
        }
        return out;
    }
    // A crate as a zone member (00224330 / 00224180): virtual +5c 003ce980 /
    // +60 003ce790 set / clear +2d (+58 is 0: nothing else).
    bool set_crate_present(std::uint32_t key,bool present){
        for(auto& b:backpacks){
            if(!b.crate||b.object.word(0xc)!=key)continue;
            if(b.object.word(0x58)!=0)fail("003ce980 / 003ce790 with a +58 component (its virtual +48 / +4c) is not ported");
            b.object.set_byte(0x2d,present?1:0);
            return true;
        }
        return false;
    }
    // The HUD message texts of a pickup's events (00254da0 / 00255290): the
    // record name 003fdb50(+80), the string 003fdb50(004a7b20 / 30 / 40), the
    // format 00128bd0(buf, "%s %s", name, string) and 00242b00(*004ffc2c,
    // text), whose text is the formatted buffer, or the bare string for the
    // armor (no format). An unresolved string throws (a NULL %s / message).
    std::vector<std::string> pickup_notices(const std::vector<WeaponBackpackEvent>& events) const{
        std::vector<std::string> out;
        std::optional<std::string> name,text,formatted;
        const auto resolve=[this](std::string_view key,const char* address)->std::string{
            if(!config.localization)fail(std::string(address)+": the backpack pickup's strings need the localization");
            const auto r=config.localization->resolve(key);
            if(!r)fail(std::string(address)+": the pickup string \""+std::string(key)+"\" is not resolved (a NULL text)");
            return std::string(*r);
        };
        for(const auto& e:events){
            using K=WeaponBackpackEvent::Kind;
            if(e.kind==K::display_name){name=resolve(db.records().at(e.a).display_name_80(),"003fdb50");}
            else if(e.kind==K::string){
                const char* key=e.a==backpack_string_pickup_wpn?"PickupWpn":e.a==backpack_string_pickup_armor?"PickupArmor":
                    e.a==backpack_string_pickup_ammo?"PickupAmmo":nullptr;
                if(!key)fail("a backpack string that is not a pickup string");
                text=resolve(key,"003fdb50");formatted.reset();
            }else if(e.kind==K::format){
                if(!name||!text||e.a!=backpack_format_pickup)fail("00128bd0: a pickup format without its name and string");
                formatted=*name+" "+*text;
            }else if(e.kind==K::hud_message){
                if(!formatted&&!text)fail("00242b00: a pickup message without its text");
                out.push_back(formatted?*formatted:*text);
            }
        }
        return out;
    }
    // The player's object-zone trigger hooks (00177380 code 0x12 -> 001774c8).
    std::optional<std::size_t> backpack_of_key(std::uint32_t key) const{
        for(std::size_t i=0;i<backpacks.size();++i){
            const auto& b=backpacks[i];
            if(b.address!=0&&b.registered&&!b.destroyed&&b.object.word(0xc)==key)return i;
        }
        return std::nullopt;
    }
    // 001774c8's 0021b790(msg, 4, event +2c, comp +30, 0) and 003c9420(msg,
    // bp, 1): the trigger *004f5440 delivered to the backpack's receiver
    // 00254600 with the player's controller as the picker (present; virtual
    // +54 true, +14 = 1, virtual +68 = 0, +178 = 0: CombatPlayConfig::
    // backpack_world), then the picker's +1bc / +17c written back.
    void trigger_backpack(std::uint32_t key,PlayerButtonState& buttons){
        const auto i=backpack_of_key(key);
        if(!i)fail("001774c8: a trigger to an object that is not a backpack of the session");
        if(!in_frame)fail("a backpack pickup outside begin_frame / end_frame");
        auto& b=backpacks[*i];
        const auto a=config.player;
        actors->collect_inventory_view(a,*combat);
        std::vector<WeaponInventoryEvent> inventory_events;
        auto ops=combat->inventory_ops(a,*config.increased_ammo,inventory_events);
        const auto& health=world.actor(a).health();
        WeaponBackpackActor actor{&ops,health.armor_33a0,health.max_armor_339c};
        WeaponBackpackPicker picker;
        picker.present=true;picker.predicate_54=true;picker.byte_14=1;picker.index_68=0;
        picker.pickup_1bc=buttons.pickup_16c?1:0;picker.confirm_17c=buttons.interaction_12c?1:0;
        picker.confirm_178=pickup_menu?pickup_menu->token:0U;      // component +178 (input +128)
        picker.word_184=buttons.zone_count_134;
        const auto from=b.events.size();
        const auto host=weapon_host(b.address,&ops);
        WeaponBackpackOps(b.object,db,host,b.events).receive(true,picker,actor);
        buttons.pickup_16c=picker.pickup_1bc!=0;buttons.interaction_12c=picker.confirm_17c!=0;
        combat->check_events(inventory_events);
        actors->publish_inventory_view(a,*combat);
        world.actor(a).set_armor(actor.armor_33a0,actor.max_armor_339c);
        CombatPickup p;
        p.backpack=*i;
        p.events.assign(b.events.begin()+static_cast<std::ptrdiff_t>(from),b.events.end());
        p.inventory_events=inventory_events;
        p.notices=pickup_notices(p.events);
        p.armor_33a0=actor.armor_33a0;p.max_armor_339c=actor.max_armor_339c;
        apply_weapon_events(b,from);
        frame.pickups.push_back(std::move(p));
        take_pickup_menu_state_notes();
    }
    // Checkpoint 132t: the game half of the pickup menu's hosts
    // (pickup_menu.hpp) over the session: the player's inventory *(actor +
    // 3524) through the ops of the current call (the token 1), the menu's
    // backpack +0 through its WeaponBackpackOps, the player's armor
    // (written back by the caller) and the HUD messages 00242b00 (in order
    // with the backpack's own pickup messages: `at` is its event count).
    struct MenuMessage {std::size_t at{};std::string text;};
    class MenuHost final : public PickupMenuScreenHost {
    public:
        MenuHost(Impl& s,CombatBackpack& b,WeaponInventoryOps& inv,WeaponBackpackOps& bp,std::int32_t& armor,
                 std::int32_t& max_armor,std::vector<MenuMessage>& messages)
            :PickupMenuScreenHost(*s.config.pickup_menu_screen,s.config.pickup_text_colour,s.menu_strings(),
                                  s.config.player_name,s.pickup_state),
             s_(s),b_(b),inv_(inv),bp_(bp),armor_(armor),max_armor_(max_armor),messages_(messages){}
        // PickupMenuHost.
        std::vector<std::uint32_t> list_items(std::uint32_t backpack,std::uint32_t inventory,std::int32_t max) override{
            check(backpack,"002553e0");
            if(inventory!=1)fail("002553e0: an inventory other than the player's");
            const auto items=bp_.list(inv_.inventory());
            if(items.size()>static_cast<std::size_t>(max))fail("002553e0: more items than the menu's bound");
            return {items.begin(),items.end()};
        }
        std::uint32_t owner_state_e50(std::uint32_t backpack) override{check(backpack,"00255e50");return bp_.owner_state(false)?1U:0U;}
        std::uint32_t owner_state_d90(std::uint32_t backpack) override{check(backpack,"00255d90");return bp_.owner_state(true)?1U:0U;}
        void close_backpack(std::uint32_t backpack) override{check(backpack,"00255f10");bp_.finish();}
        // PickupMenuFrameHost.
        std::uint8_t held_84() override{return inv_.inventory().current_84;}
        std::int32_t slot_item(std::uint32_t slot) override{
            if(slot>=inv_.inventory().slots.size())fail("pickup menu: an inventory slot outside the 8 slots");
            return inv_.inventory().slots[slot].item;
        }
        std::int32_t backpack_ammunition() override{return b_.object.word(0x2e4)!=0?b_.object.value(0x2e8):0;}
        std::uint8_t backpack_60() override{return b_.object.byte(0x60);}
        bool owner_valid() override{
            const auto owner=b_.object.word(0x300);
            return owner!=0&&s_.owner_view(s_.actor_at(owner)).key_c==b_.object.value(0x304);
        }
        std::int32_t armor_33a0() override{return armor_;}
        // 0025220c: the action message 0x2b (+28 = 5, +2c = the owner's +c) queued to the player actor.
        void owner_pickup_message() override{
            const auto body=s_.actor_at(b_.object.word(0x300));
            s_.world.actor(s_.config.player).queue_carry_pickup(s_.world.actor(body).handle(),5);
            s_.frame.notes.push_back("0025220c: carry pickup of actor "+std::to_string(body));
        }
        void owner_disguise_taken() override{
            const auto body=s_.actor_at(b_.object.word(0x300));
            if(body==s_.config.player)fail("002522c4: a kind-4 line on the player's own backpack (the player component's +1d8 00140e90 is not hosted)");
            auto& owner=s_.world.actor(body);
            // 002522c4..002522e0: the owner's AI component +1d8 = 0014ac90(c, 0, 1).
            owner.set_disguise_3469(npc_disguise_store(owner.eligible(),owner.disguise_3469(),0));
            // 002522e8..00252344: AIDisguiseTakenMsg (004ff2b8), +24 the taker, to the owner (transport 0).
            ActorAiNotification n;n.key=actor_disguise_taken_message_key;n.object=s_.world.actor(s_.config.player).handle();
            owner.queue_ai_notification(n);
            s_.frame.notes.push_back("002522c4: disguise taken from actor "+std::to_string(body));
        }
        std::int32_t max_armor_339c() override{return max_armor_;}
        void set_armor(std::int32_t armor,std::int32_t max_armor) override{armor_=armor;max_armor_=max_armor;}
        void clear_selection_a4() override{s_.combat->inventory(s_.config.player).selection_a4=-1;}
        bool ammo_label_48af08() override{return true;}         // *0048af08 = 1 (the ELF's data)
        bool multiplayer_5721c8() override{return false;}       // single player
        std::uint32_t agent_unlock(std::uint32_t player,std::int32_t unlock) override{
            if(player!=player_token||unlock!=1)fail("00332f90: an unlock other than the local player's 1");
            if(!s_.config.agent_unlock_1)fail("00332f90: the agent-data unlock 1 is not supplied");
            return *s_.config.agent_unlock_1?1U:0U;
        }
        std::int32_t slot_of_item(std::int32_t item) override{return inv_.slot_of_item(item);}
        std::int32_t cycle(std::uint32_t backpack,std::int32_t item,std::int32_t dir) override{
            check(backpack,"00254ca0");return bp_.cycle(item,dir!=0);}
        std::uint32_t take(std::uint32_t backpack,std::int32_t item,std::int32_t test) override{
            check(backpack,"002558a0");return bp_.take(item,test!=0)?1U:0U;}
        // 00143710(inv, item): -1 is the held slot's item (none: 0); else the record's clip +70.
        std::uint32_t has_clip(std::int32_t item) override{
            const auto& inv=inv_.inventory();
            if(item==-1)item=inv.current_84!=weapon_no_slot?inv.slots.at(inv.current_84).item:-1;
            if(item==-1)return 0;
            return s_.db.record(item).clip_70()!=0?1U:0U;
        }
        std::uint32_t slot_flag(std::uint32_t backpack,std::int32_t item) override{check(backpack,"002559d0");return bp_.slot_flag(item);}
        std::optional<PickupMenuAmmo> ammunition(std::uint32_t backpack,std::int32_t item) override{
            check(backpack,"00255500");
            const auto a=bp_.ammunition(item);
            if(!a)return std::nullopt;
            return PickupMenuAmmo{a->spare,a->clip,a->total};
        }
        PickupMenuRecord record(std::int32_t item) override{
            const auto& r=s_.db.record(item);
            PickupMenuRecord out;
            out.class_30=r.weapon_class_30();out.clip_70=r.clip_70();out.clips_74=r.clips_74();
            out.name_80=r.display_name_80();out.hold_f0=r.hold_class_f0();
            out.icon_11c=static_cast<std::uint32_t>(r.hud_icon_11c());out.model_13c=static_cast<std::uint32_t>(r.model_13c());
            return out;
        }
        std::int32_t spare(std::int32_t slot) override{return inv_.spare(slot_byte(slot));}
        std::int32_t clip(std::int32_t slot) override{return inv_.clip(slot_byte(slot));}
        std::int32_t clips_for_actor(std::int32_t clips) override{return inv_.clips_for_actor(clips);}
        RootVector actor_position() override{return s_.actors->eye_c0(s_.config.player,-1);}
        std::int32_t add_spare(std::int32_t slot,std::int32_t rounds,std::int32_t no_limit) override{
            return inv_.add_spare(slot_byte(slot),rounds,no_limit!=0);}
        std::int32_t remove_ammunition(std::uint32_t backpack,std::int32_t item,std::int32_t rounds) override{
            check(backpack,"00255650");return bp_.remove_ammunition(item,rounds);}
        void remove(std::int32_t slot) override{inv_.remove(slot_byte(slot),false);}
        void add(std::int32_t item) override{inv_.add(item,false,false);}
        void set_clip(std::int32_t slot,std::int32_t rounds) override{inv_.set_clip(slot_byte(slot),rounds);}
        void equip(std::int32_t slot) override{inv_.equip(slot_byte(slot));}
        void hud_message(std::string_view text) override{messages_.push_back({b_.events.size(),std::string(text)});}
        void mark_30c(std::uint32_t backpack) override{check(backpack,"00255a40");bp_.mark_30c();}
        void add_weapon(std::uint32_t backpack,std::int32_t item,std::int32_t spare,std::int32_t clip,std::int32_t flag) override{
            check(backpack,"00255af0");bp_.add_weapon(item,spare,clip,static_cast<std::uint8_t>(flag));}
        std::pair<std::int32_t,std::int32_t> take_armor(std::uint32_t backpack) override{
            check(backpack,"00255860");
            std::int32_t armor{},max_armor{};
            bp_.take_armor(armor,max_armor);
            return {armor,max_armor};
        }
        void add_armor(std::uint32_t backpack,std::int32_t armor,std::int32_t max_armor) override{
            check(backpack,"00255a50");bp_.add_armor(armor,max_armor);}
        void pickup_ammunition(std::uint32_t backpack) override{check(backpack,"00255290");bp_.pickup_ammunition(inv_);}
        void share_dual() override{inv_.share_dual();}
        // The CrateInteractMsg (key 004f7ea0, vtable 004dc480) Event_Send
        // 003c9420(msg, bp, 0) queues for the backpack: pumped next frame.
        void crate_interact(std::uint32_t backpack,std::int32_t taken,std::int32_t given,std::uint8_t flags_20) override{
            check(backpack,"003c9420");
            CombatCrateMessage m;
            m.backpack=static_cast<std::size_t>(&b_-s_.backpacks.data());m.address=backpack;
            m.taken=taken;m.given=given;m.flags_20=flags_20;
            s_.frame.crate_messages.push_back(m);
        }
        // Checkpoint 132zf5: the kind-5 line / take over the item lists
        // (CombatPlayConfig::item_list): the owner is bp +300's actor, the
        // actor the player.
        std::uint32_t item_icon(std::int32_t item) override{
            if(!s_.config.item_icon)fail("002526f0: the item table's +28 handle (004703b0) is not supplied");
            return s_.config.item_icon(item);
        }
        std::int32_t owner_item_count(std::int32_t item) override{return owner_items("002526f0").count(item);}
        std::int32_t actor_item_count(std::int32_t item) override{return actor_items("002526f0").count(item);}
        std::int32_t owner_remove_item(std::int32_t item,std::int32_t count) override{
            return owner_items("002523b0").remove(item,count);}
        void actor_add_item(std::int32_t item,std::int32_t count) override{actor_items("002523b0").add(item,count);}
        std::optional<std::string> item_name(std::int32_t item) override{
            if(!s_.config.item_name)fail("002523b0: the item name 00470270 is not supplied");
            return s_.config.item_name(item);
        }
        // The InventoryMsg (key 004f7ee0, vtable 004dc460) Event_Send
        // 003c9420(msg, bp, 0) queues for the backpack: pumped next frame.
        void inventory_message(std::uint32_t backpack,std::int32_t item,std::int32_t count) override{
            check(backpack,"003c9420");
            CombatInventoryMessage m;
            m.backpack=static_cast<std::size_t>(&b_-s_.backpacks.data());m.address=backpack;
            m.item=item;m.count=count;
            s_.frame.inventory_messages.push_back(m);
        }
    private:
        void check(std::uint32_t backpack,const char* address) const{
            if(backpack!=b_.address)fail(std::string(address)+": a backpack other than the pickup menu's");
        }
        static std::uint8_t slot_byte(std::int32_t slot){
            if(slot<0||slot>0xff)fail("pickup menu: an inventory slot outside a byte");
            return static_cast<std::uint8_t>(slot);
        }
        ItemInventory& owner_items(const char* address) const{
            if(!s_.config.item_list)fail(std::string(address)+": the item lists +3528 are not supplied");
            const auto owner=b_.object.word(0x300);
            if(owner==0)fail(std::string(address)+": the backpack has no owner +300");
            return s_.config.item_list(s_.config.object_ids.at(s_.actor_at(owner)));
        }
        ItemInventory& actor_items(const char* address) const{
            if(!s_.config.item_list)fail(std::string(address)+": the item lists +3528 are not supplied");
            return s_.config.item_list(s_.config.object_ids.at(s_.config.player));
        }
        Impl& s_;
        CombatBackpack& b_;
        WeaponInventoryOps& inv_;
        WeaponBackpackOps& bp_;
        std::int32_t& armor_;
        std::int32_t& max_armor_;
        std::vector<MenuMessage>& messages_;
    };
    // PORT-ONLY (checkpoint 132pw1, coop_partner_kit.hpp): the Cross 00251920's game half for the CPU partner, the
    // second agent at a backpack. In the original a second agent's menu runs on its own machine (its HUD screen, menu
    // sounds and ticker), so its GUI calls are not made here: the Cross makes only the GS_CONFIRM sound (noted) and
    // the ticker message (the CombatPickup's notices, for the partner's own ticker). The inventory is the partner's
    // *(actor + 3524), the position its virtual +c0, the agent 001314f0(004ffb50, -1) its own (no agent record: no
    // DualWield, 00332f90(.., 1) = 0) and Who of a CrateInteractMsg the partner. The kind-5 line (mission items) is
    // never chosen for the partner.
    static constexpr std::uint32_t partner_token=2;
    class PartnerMenuHost final : public PickupMenuFrameHost {
    public:
        PartnerMenuHost(Impl& s,CombatBackpack& b,std::size_t actor,WeaponInventoryOps& inv,WeaponBackpackOps& bp,std::int32_t& armor,
                        std::int32_t& max_armor,std::vector<MenuMessage>& messages)
            :s_(s),b_(b),a_(actor),inv_(inv),bp_(bp),armor_(armor),max_armor_(max_armor),messages_(messages){}
        std::uint8_t held_84() override{return inv_.inventory().current_84;}
        std::int32_t slot_item(std::uint32_t slot) override{
            if(slot>=inv_.inventory().slots.size())fail("partner pickup menu: an inventory slot outside the 8 slots");
            return inv_.inventory().slots[slot].item;
        }
        std::int32_t backpack_ammunition() override{return b_.object.word(0x2e4)!=0?b_.object.value(0x2e8):0;}
        std::uint8_t backpack_60() override{return b_.object.byte(0x60);}
        bool owner_valid() override{
            const auto owner=b_.object.word(0x300);
            return owner!=0&&s_.owner_view(s_.actor_at(owner)).key_c==b_.object.value(0x304);
        }
        std::int32_t armor_33a0() override{return armor_;}
        std::int32_t max_armor_339c() override{return max_armor_;}
        void set_armor(std::int32_t armor,std::int32_t max_armor) override{armor_=armor;max_armor_=max_armor;}
        void clear_selection_a4() override{s_.combat->inventory(a_).selection_a4=-1;}
        bool ammo_label_48af08() override{return true;}         // *0048af08 = 1 (the ELF's data)
        bool multiplayer_5721c8() override{return false;}       // the partner is local: no network send
        std::string get_string(std::string_view key) override{
            if(!s_.config.localization)fail("00251920: the partner's pickup strings (003fdb50) need the localization");
            const auto r=s_.config.localization->resolve(key);
            if(!r)fail("00251920: the pickup string \""+std::string(key)+"\" is not resolved (a NULL text)");
            return std::string(*r);
        }
        std::uint32_t player(std::uint32_t list,std::int32_t index) override{
            if(list!=kPickupMenuPlayerList||index!=-1)fail("001314f0: a player other than the agent's own");
            return partner_token;
        }
        std::uint32_t agent_unlock(std::uint32_t player,std::int32_t unlock) override{
            if(player!=partner_token||unlock!=1)fail("00332f90: an unlock other than the partner's own 1");
            return 0;
        }
        std::int32_t slot_of_item(std::int32_t item) override{return inv_.slot_of_item(item);}
        std::int32_t cycle(std::uint32_t backpack,std::int32_t item,std::int32_t dir) override{
            check(backpack,"00254ca0");return bp_.cycle(item,dir!=0);}
        std::uint32_t take(std::uint32_t backpack,std::int32_t item,std::int32_t test) override{
            check(backpack,"002558a0");return bp_.take(item,test!=0)?1U:0U;}
        std::uint32_t has_clip(std::int32_t item) override{
            const auto& inv=inv_.inventory();
            if(item==-1)item=inv.current_84!=weapon_no_slot?inv.slots.at(inv.current_84).item:-1;
            if(item==-1)return 0;
            return s_.db.record(item).clip_70()!=0?1U:0U;
        }
        std::uint32_t slot_flag(std::uint32_t backpack,std::int32_t item) override{check(backpack,"002559d0");return bp_.slot_flag(item);}
        std::optional<PickupMenuAmmo> ammunition(std::uint32_t backpack,std::int32_t item) override{
            check(backpack,"00255500");
            const auto a=bp_.ammunition(item);
            if(!a)return std::nullopt;
            return PickupMenuAmmo{a->spare,a->clip,a->total};
        }
        PickupMenuRecord record(std::int32_t item) override{
            const auto& r=s_.db.record(item);
            PickupMenuRecord out;
            out.class_30=r.weapon_class_30();out.clip_70=r.clip_70();out.clips_74=r.clips_74();
            out.name_80=r.display_name_80();out.hold_f0=r.hold_class_f0();
            out.icon_11c=static_cast<std::uint32_t>(r.hud_icon_11c());out.model_13c=static_cast<std::uint32_t>(r.model_13c());
            return out;
        }
        std::int32_t spare(std::int32_t slot) override{return inv_.spare(slot_byte(slot));}
        std::int32_t clip(std::int32_t slot) override{return inv_.clip(slot_byte(slot));}
        std::int32_t clips_for_actor(std::int32_t clips) override{return inv_.clips_for_actor(clips);}
        void set_preview(std::uint32_t,std::uint32_t,std::uint8_t) override{gui("00259420");}
        void set_element_data(std::uint32_t,std::uint32_t) override{gui("003e7eb0");}
        void set_text(std::uint32_t,std::string_view) override{gui("003e7ec0");}
        void anim_range(std::uint32_t,float,float,std::int32_t) override{gui("003f4510");}
        void fade(std::uint32_t,float) override{gui("003f44e0");}
        void post_sound(std::string_view name,std::uint32_t,std::int32_t) override{
            s_.frame.notes.push_back("partner pickup menu: "+std::string(name)+" (the second agent's own menu sound, not played)");
        }
        RootVector actor_position() override{return s_.actors->eye_c0(a_,-1);}
        void positional_sound(std::uint32_t descriptor,const RootVector& position) override{
            PickupMenuSound m;m.descriptor=descriptor;m.position=position;   // 0020f860 to 004ffb50: a world sound
            s_.frame.menu_sounds.push_back(std::move(m));
        }
        std::int32_t add_spare(std::int32_t slot,std::int32_t rounds,std::int32_t no_limit) override{
            return inv_.add_spare(slot_byte(slot),rounds,no_limit!=0);}
        std::int32_t remove_ammunition(std::uint32_t backpack,std::int32_t item,std::int32_t rounds) override{
            check(backpack,"00255650");return bp_.remove_ammunition(item,rounds);}
        void remove(std::int32_t slot) override{inv_.remove(slot_byte(slot),false);}
        void add(std::int32_t item) override{inv_.add(item,false,false);}
        void set_clip(std::int32_t slot,std::int32_t rounds) override{inv_.set_clip(slot_byte(slot),rounds);}
        void equip(std::int32_t slot) override{inv_.equip(slot_byte(slot));}
        void hud_message(std::string_view text) override{messages_.push_back({b_.events.size(),std::string(text)});}
        void mark_30c(std::uint32_t backpack) override{check(backpack,"00255a40");bp_.mark_30c();}
        void add_weapon(std::uint32_t backpack,std::int32_t item,std::int32_t spare,std::int32_t clip,std::int32_t flag) override{
            check(backpack,"00255af0");bp_.add_weapon(item,spare,clip,static_cast<std::uint8_t>(flag));}
        std::pair<std::int32_t,std::int32_t> take_armor(std::uint32_t backpack) override{
            check(backpack,"00255860");
            std::int32_t armor{},max_armor{};
            bp_.take_armor(armor,max_armor);
            return {armor,max_armor};
        }
        void add_armor(std::uint32_t backpack,std::int32_t armor,std::int32_t max_armor) override{
            check(backpack,"00255a50");bp_.add_armor(armor,max_armor);}
        void pickup_ammunition(std::uint32_t backpack) override{check(backpack,"00255290");bp_.pickup_ammunition(inv_);}
        void share_dual() override{inv_.share_dual();}
        void crate_interact(std::uint32_t backpack,std::int32_t taken,std::int32_t given,std::uint8_t flags_20) override{
            check(backpack,"003c9420");
            CombatCrateMessage m;
            m.backpack=static_cast<std::size_t>(&b_-s_.backpacks.data());m.address=backpack;
            m.taken=taken;m.given=given;m.flags_20=flags_20;m.who=a_;
            s_.frame.crate_messages.push_back(m);
        }
        std::uint32_t item_icon(std::int32_t) override{kind5();}
        std::int32_t owner_item_count(std::int32_t) override{kind5();}
        std::int32_t actor_item_count(std::int32_t) override{kind5();}
        std::int32_t owner_remove_item(std::int32_t,std::int32_t) override{kind5();}
        void actor_add_item(std::int32_t,std::int32_t) override{kind5();}
        std::optional<std::string> item_name(std::int32_t) override{kind5();}
        void inventory_message(std::uint32_t,std::int32_t,std::int32_t) override{kind5();}
    private:
        void check(std::uint32_t backpack,const char* address) const{
            if(backpack!=b_.address)fail(std::string(address)+": a backpack other than the partner's");
        }
        [[noreturn]] static void gui(const char* address){
            fail(std::string(address)+": the partner's menu draws on the second agent's own screen (not made here)");
        }
        [[noreturn]] static void kind5(){fail("002526f0 / 002523b0: the partner takes no mission items (kind 5)");}
        static std::uint8_t slot_byte(std::int32_t slot){
            if(slot<0||slot>0xff)fail("partner pickup menu: an inventory slot outside a byte");
            return static_cast<std::uint8_t>(slot);
        }
        Impl& s_;
        CombatBackpack& b_;
        std::size_t a_;
        WeaponInventoryOps& inv_;
        WeaponBackpackOps& bp_;
        std::int32_t& armor_;
        std::int32_t& max_armor_;
        std::vector<MenuMessage>& messages_;
    };
    CombatPickup partner_pickup(std::size_t i,std::size_t a,std::optional<std::int32_t> swap){
        if(!in_frame)fail("a partner pickup outside begin_frame / end_frame");
        if(a==config.player)fail("partner_pickup: the taker is the player");
        if(!config.backpack_world)fail("partner_pickup: the backpacks are not simulated (CombatPlayConfig::backpack_world)");
        if(i>=backpacks.size()||backpacks[i].destroyed||backpacks[i].address==0)fail("partner_pickup: not a live simulated backpack");
        const auto address=backpacks[i].address;
        actors->collect_inventory_view(a,*combat);
        std::vector<WeaponInventoryEvent> inventory_events;
        auto ops=combat->inventory_ops(a,false,inventory_events);   // 001861f0: no IncreasedAmmo (no agent record)
        const auto& health=world.actor(a).health();
        WeaponBackpackActor actor{&ops,health.armor_33a0,health.max_armor_339c};
        // The second agent's zone enter (001774c8: +1bc = 1) delivers *004f5440 to 00254600 with its own component:
        // a player component (+54 answers 1, +14 = 1). Its index +68 is the backpack's authority +308, which stands in
        // for the authority serving the request 005061b0 (PORT-ONLY: the request's handler is not ported).
        WeaponBackpackPicker picker;
        picker.present=true;picker.predicate_54=true;picker.byte_14=1;
        picker.index_68=backpacks[i].object.value(0x308);
        picker.pickup_1bc=1;
        const auto host=weapon_host(address,nullptr,a);
        auto& b=backpack_at(address);
        const auto from=b.events.size();
        WeaponBackpackOps(b.object,db,host,b.events).receive(true,picker,actor);
        apply_weapon_events(b,from);
        std::vector<MenuMessage> messages;
        if(swap&&!b.destroyed){
            // Triangle, then Cross on the entry (00251920) and the menu's close 00252fb0 -> 00255f10.
            const auto from_menu=b.events.size();
            WeaponBackpackOps bp(b.object,db,host,b.events);
            std::int32_t armor=actor.armor_33a0,max_armor=actor.max_armor_339c;
            PartnerMenuHost h(*this,b,a,ops,bp,armor,max_armor,messages);
            PickupMenu menu;
            menu.backpack=address;menu.count=1;menu.cursor=0;
            menu.entries[0].kind=0;menu.entries[0].item=static_cast<std::uint32_t>(*swap);
            (void)pickup_menu_confirm(menu,h);
            bp.finish();
            actor.armor_33a0=armor;actor.max_armor_339c=max_armor;
            apply_weapon_events(b,from_menu);
        }
        combat->check_events(inventory_events);
        actors->publish_inventory_view(a,*combat);
        world.actor(a).set_armor(actor.armor_33a0,actor.max_armor_339c);
        CombatPickup p;
        p.backpack=i;p.via=swap?"00251920":"001762c0";p.taker=a;
        p.events.assign(b.events.begin()+static_cast<std::ptrdiff_t>(from),b.events.end());
        p.inventory_events=inventory_events;
        p.notices=pickup_notices(p.events);
        for(auto& m:messages)p.notices.push_back(std::move(m.text));
        p.armor_33a0=actor.armor_33a0;p.max_armor_339c=actor.max_armor_339c;
        frame.pickups.push_back(p);
        return p;
    }
    // 003fdb50 through the mission's string pools (unresolved: nullopt).
    PickupMenuScreenHost::Strings menu_strings() const{
        if(!config.localization)fail("00253070: the pickup menu's strings (003fdb50) need the localization");
        const Localization* l=config.localization;
        return [l](std::string_view key)->std::optional<std::string>{
            const auto r=l->resolve(key);
            if(!r)return std::nullopt;
            return std::string(*r);
        };
    }
    // The menu's records go to the frame's notes.
    void take_pickup_menu_state_notes(){
        for(auto& n:pickup_state.notes)frame.notes.push_back("pickup menu: "+std::move(n));
        pickup_state.notes.clear();
        for(auto& s:pickup_state.sounds)frame.menu_sounds.push_back(std::move(s));
        pickup_state.sounds.clear();
    }
    // 001762c0 (+17c set, +178 null): 0036b180(0xb8, "pickup_menu.cc",
    // 0x27a) and 00253070(menu, *(*004ffc2c + 0x54), bp, comp + 30,
    // owner_check, 0) on the HUD screen (node handle 1, the root); the
    // returned token is the component's +178.
    std::uint32_t open_pickup_menu(std::uint32_t address,WeaponInventoryOps& inventory,std::uint32_t owner_check){
        if(pickup_menu)fail("001762c0: a pickup menu while +178 is set");
        if(!config.pickup_menu_screen)
            fail("001762c0 creates the Triangle confirm menu 00253070 (HUD *004ffc2c screen +54) and the session has no HUD screen");
        auto& b=backpack_at(address);
        const auto index=static_cast<std::size_t>(&b-backpacks.data());
        const auto token=pickup_menu_block_base+pickup_state.blocks++;
        pickup_state.notes.push_back("0036b180(0xb8, \"pickup_menu.cc\", 0x27a) -> the menu "+hex_word(token));
        const auto host=weapon_host(address);
        WeaponBackpackOps bp(b.object,db,host,b.events);
        const auto& health=world.actor(config.player).health();
        std::int32_t armor=health.armor_33a0,max_armor=health.max_armor_339c;
        std::vector<MenuMessage> messages;
        MenuHost h(*this,b,inventory,bp,armor,max_armor,messages);
        OpenPickupMenu m;m.backpack=index;m.token=token;
        PickupMenuBackpack v;
        v.address=address;v.word_2e4=b.object.word(0x2e4);v.word_2e8=b.object.value(0x2e8);
        v.owner_300=b.object.word(0x300);v.key_304=b.object.word(0x304);
        if(v.owner_300!=0)v.owner_key_0c=static_cast<std::uint32_t>(owner_view(actor_at(v.owner_300)).key_c);
        // owner_pairs: the owner's +3528 pairs {item, count} (the empty list
        // 001b6a60 builds without CombatPlayConfig::item_list).
        // (00253070 reads them only for a valid owner; a deleted one is not.)
        if(v.owner_300!=0&&config.item_list&&!world.destroyed(actor_at(v.owner_300)))
            for(const auto& st:config.item_list(config.object_ids.at(actor_at(v.owner_300))).stacks())
                v.owner_pairs.emplace_back(static_cast<std::uint32_t>(st.index),static_cast<std::uint32_t>(st.count));
        pickup_menu_build(m.menu,pickup_panels,h,1,v,1,owner_check,0);
        if(!messages.empty())fail("00253070 sent a HUD message");
        pickup_menu=std::move(m);
        return token;
    }
    // One menu operation `via` (see CombatPickup::via) on the open menu: the
    // player's inventory and armor around it, the backpack's events applied
    // and a CombatPickup when anything changed.
    void menu_call(const char* via,const std::function<void(MenuHost&)>& f){
        if(!pickup_menu)fail(std::string(via)+": no pickup menu is open");
        if(!in_frame)fail(std::string(via)+": a pickup menu call outside begin_frame / end_frame");
        auto& b=backpacks.at(pickup_menu->backpack);
        const auto a=config.player;
        actors->collect_inventory_view(a,*combat);
        std::vector<WeaponInventoryEvent> inventory_events;
        auto ops=combat->inventory_ops(a,*config.increased_ammo,inventory_events);
        const auto& health=world.actor(a).health();
        const std::int32_t armor_before=health.armor_33a0,max_before=health.max_armor_339c;
        std::int32_t armor=armor_before,max_armor=max_before;
        const auto from=b.events.size();
        const auto host=weapon_host(b.address);
        WeaponBackpackOps bp(b.object,db,host,b.events);
        std::vector<MenuMessage> messages;
        {
            MenuHost h(*this,b,ops,bp,armor,max_armor,messages);
            f(h);
        }
        combat->check_events(inventory_events);
        actors->publish_inventory_view(a,*combat);
        if(armor!=armor_before||max_armor!=max_before)world.actor(a).set_armor(armor,max_armor);
        // The per-frame 002526f0 only reads (00332f90, 003fdb50, 00128bd0,
        // +c0, the 001861f0 unlock 5 query): a pickup is reported when
        // something changed.
        using EK=WeaponBackpackEvent::Kind;
        const bool changed=std::any_of(b.events.begin()+static_cast<std::ptrdiff_t>(from),b.events.end(),[](const auto& e){
            return e.kind!=EK::unlock_query&&e.kind!=EK::display_name&&e.kind!=EK::string&&e.kind!=EK::format&&e.kind!=EK::position_c0;});
        const bool inventory_changed=std::any_of(inventory_events.begin(),inventory_events.end(),[](const auto& e){
            return e.kind!=WeaponInventoryEvent::Kind::agent_unlock_query;});
        if(changed||inventory_changed||!messages.empty()||armor!=armor_before||max_armor!=max_before){
            CombatPickup p;
            p.via=via;
            p.backpack=pickup_menu->backpack;
            p.events.assign(b.events.begin()+static_cast<std::ptrdiff_t>(from),b.events.end());
            p.inventory_events=inventory_events;
            std::size_t at=from;
            for(const auto& m:messages){
                const std::vector<WeaponBackpackEvent> part(b.events.begin()+static_cast<std::ptrdiff_t>(at),
                    b.events.begin()+static_cast<std::ptrdiff_t>(m.at));
                for(auto& n:pickup_notices(part))p.notices.push_back(std::move(n));
                p.notices.push_back(m.text);
                at=m.at;
            }
            const std::vector<WeaponBackpackEvent> rest(b.events.begin()+static_cast<std::ptrdiff_t>(at),b.events.end());
            for(auto& n:pickup_notices(rest))p.notices.push_back(std::move(n));
            p.armor_33a0=armor;p.max_armor_339c=max_armor;
            frame.pickups.push_back(std::move(p));
        }
        apply_weapon_events(b,from);
        take_pickup_menu_state_notes();
    }
    // 00247320: 00252fb0(menu, -1) (its 00255f10 on the backpack), the free
    // 00100740 and +128 = 0.
    void close_pickup_menu(){
        if(!pickup_menu)return;
        menu_call("00252fb0",[this](MenuHost& h){pickup_menu_destroy(pickup_menu->menu,h);});
        pickup_state.notes.push_back("00100740 frees the menu "+hex_word(pickup_menu->token)+"; +128 = 0");
        pickup_menu.reset();
        take_pickup_menu_state_notes();
    }
    // 002568f0(dt) once per frame: the destroys it makes are split per
    // backpack (each ends with 003cb230(bp, 0.1)) and applied.
    void tick_backpacks(){
        std::vector<BackpackObjectEvent> events;
        backpack_object_tick(input.dt,backpack_clock,backpack_list,object_host(),events);
        std::size_t start=0;
        for(std::size_t i=0;i<events.size();++i){
            const auto& e=events[i];
            if(e.kind!=BackpackObjectEvent::Kind::release||(e.a&0xf8000000U)!=0xb0000000U)continue;
            auto& b=backpack_at(e.a);
            if(pickup_menu&&&backpacks[pickup_menu->backpack]==&b)
                fail("002568f0 destroys the backpack of the open pickup menu (+314 is negative while it is open; not expected)");
            const auto from=b.object_events.size();
            b.object_events.insert(b.object_events.end(),events.begin()+static_cast<std::ptrdiff_t>(start),
                events.begin()+static_cast<std::ptrdiff_t>(i+1));
            b.destroyed=true;
            apply_object_events(b,from);
            start=i+1;
        }
        if(start!=events.size())fail("002568f0: events outside a destroy");
    }
    void add_backpack_contents(std::uint64_t handle,const std::function<void(WeaponBackpackOps&)>& add){
        auto& b=backpack(handle);
        if(!config.backpack_world){
            WeaponBackpackOps ops(b.object,db,backpack_host(),b.events);
            add(ops);
            return;
        }
        const auto from=b.events.size();
        const auto host=weapon_host(b.address);
        WeaponBackpackOps ops(b.object,db,host,b.events);
        add(ops);
        apply_weapon_events(b,from);
    }
    std::uint64_t find(std::int32_t id) const{   // 0040c540: the traversal tree (a flushed removal is not found)
        for(std::size_t a=0;a<world.size();++a)if(config.object_ids[a]==id&&world.traversed(a))return world.actor(a).handle();
        if(nodes)for(std::size_t k=0;k<nodes->nodes.size();++k)
            if(static_cast<std::int32_t>(nodes->nodes[k].id)==id)return npc_node_object(static_cast<std::int32_t>(k));
        return 0;
    }
    // A node: +2c 1, +2d 0, +2e 0 (003cf0e0; nothing activates a node). The
    // lit spot keeps the same bytes (003cf0e0; 0045bad0 writes none of them).
    std::array<std::uint8_t,3> root(std::uint64_t object) const{
        if(node_at(object)||spot_owner(object))return {1,0,0};
        if(const auto g=gob(object))return g->root;
        return world.actor(index(object)).root_bytes();
    }
    // A population GOB (vtable 004db4c0) that is not an actor of the world.
    std::optional<CombatHostObject> gob(std::uint64_t object) const{
        if(!host_objects||find_index(object)||spot_owner(object))return std::nullopt;
        return host_objects(object);
    }
    // Actor virtual +c4 (00184b90); a node's and the lit spot's (vtable
    // 004dff50) is 0 (0015c020); a GOB's
    // (002152d0) is 0 when its +108 (00141850) is 3, else 1.
    std::uint8_t kind_c4(std::uint64_t object) const{
        if(node_at(object)||spot_owner(object))return 0;
        if(const auto g=gob(object))return g->kind_c4?*g->kind_c4:static_cast<std::uint8_t>(object_life_state(g->health_40)==3?0:1);
        const auto life=world.actor(index(object)).life_state();
        return (life==2||life==3)?0:2;
    }
    // Actor virtual +bc (00184af0): node 0049d020[3] = 9 of a present actor.
    // A GOB's (00153860) jumps to +e4 (003cd260), as does the lit spot's.
    RootVector eye_bc(std::uint64_t object) const{
        if(const auto s=spot_owner(object))return spot_position(*s);
        if(const auto g=gob(object))return g->eye;
        const auto& a=world.actor(index(object));
        if(!a.eligible())fail("the eye +bc of an actor that is not present (+e0, 00133e00) is not hosted");
        return a.skeleton_node_world_position(9);
    }
    // 003cc8e0(a, b): a's controller +6c (IsEnemyTeam 0022f440) of b's team (+68).
    bool enemy(std::uint64_t a,std::uint64_t b) const{
        if(!config.team||!config.relations)fail("teams and the team matrix (*004ee620) are not supplied");
        return config.relations->enemy(config.team(index(a)),config.team(index(b)));
    }
    const WeaponInventory& inventory(std::size_t a) const{return combat->inventory(a);}
    std::optional<std::int32_t> held_item(std::size_t a) const{
        const auto& inv=inventory(a);
        if(inv.current_84==weapon_no_slot)return std::nullopt;
        return inv.slots.at(inv.current_84).item;
    }
    // 00143510: the held record's +48 (signed byte), 0 with nothing held.
    std::uint8_t item_kind(std::size_t a) const{
        const auto item=held_item(a);
        return item?db.record(*item).item_kind_48():std::uint8_t{0};
    }
    // 0022f510 (controller +74): physical +310 of an actor with a physical
    // component (present), else the zero vector 004e1d90.
    RootVector velocity_74(std::size_t a) const{
        const auto& s=world.actor(a);
        return s.eligible()?s.physical().integrator.velocity:RootVector{};
    }
    // 00184bf0 (actor virtual +d4).
    bool ready_d4(std::size_t a){
        if(!world.actor(a).eligible())return false;
        const auto& st=actors->secondary(a).states;
        return secondary_state_allows_fire(st.current_310)&&secondary_state_allows_fire(st.next_311);
    }
    // Actor +33a8 (blind time): written only by 00186830 (the throw flow's flash).
    float blind_33a8(std::size_t a) const{return throws?throws->senses(a).blind_33a8:0.0F;}
    // 001b33d0.
    float accuracy_multiplier(std::size_t a) const{
        const float f2=shake[a]/5.0F;
        const float f0=(std::bit_cast<float>(0x3e4ccccdU)-1.0F)*f2;
        return 1.0F+f0;
    }

    VisibilityHost visibility_host(){
        VisibilityHost h;
        // The lit spot has no controller (+58 0 from 003cf0e0). Its +14 is
        // never read: 0022cb10 tests it only for a player-controlled object.
        h.player_controlled=[this](std::uint64_t o){return (spot_owner(o)||gob(o))?false:world.actor(index(o)).player_controller();};
        h.byte_14=[this](std::uint64_t o){
            if(spot_owner(o))fail("0022cb10 reading the lit spot's +14");
            return gob(o)?true:world.actor(index(o)).byte_14()!=0;};   // a GOB: +14 = 1 (003cb090)
        h.eye_bc=[this](std::uint64_t o){return eye_bc(o);};
        // A GOB's +4c is *004f5468; the lit spot is a base world object (not the actor class *0049d010).
        h.actor_class=[this](std::uint64_t o){if(spot_owner(o)||gob(o))return false;(void)index(o);return true;};
        // The native world LOS host gathers from the whole static world and keeps
        // no provider region cache (world_los.hpp), so the hint is not computed.
        h.actor_region=[](std::uint64_t)->std::optional<std::uint64_t>{return std::nullopt;};
        h.seed_los_region=[](std::uint64_t){fail("the LOS provider region seed is not hosted");};
        // 002594a0: the smoke volumes of 004f8040; play creates no area effects.
        h.smoke_blocker=[](const RootVector&,const RootVector&)->std::uint64_t{return 0;};
        h.los=[this](const LosServiceRequest& r){return answer_los_request(world.collision(),r);};
        return h;
    }
    std::uint64_t blocker(std::uint64_t a,std::uint64_t b,std::int32_t max_age,bool force){
        auto h=visibility_host();
        return visibility.test(a,b,max_age,force,fp,h);
    }

    void register_actor(std::size_t a){
        const auto handle=world.actor(a).handle();
        if(visibility.register_object(handle)){
            registered.push_back({handle,config.object_ids[a]});
            frame.notes.push_back("0022c650 registered actor "+std::to_string(a));
        }else if(!visibility.slot(handle))frame.notes.push_back("0022c650: the 30 slots are full, actor "+std::to_string(a)+" not registered");
    }
    void presence(std::size_t a,bool active){
        if(active)register_actor(a);
        else{
            // 0022c720: with a slot (0022d380), 0022d740 removes the list
            // entries whose weak handle resolves to the same object.
            const auto handle=world.actor(a).handle();
            if(visibility.slot(handle)){
                const auto id=config.object_ids[a];
                std::erase_if(registered,[&](const TargetHandle& t){return t.object==handle&&t.id==id;});
            }
            visibility.unregister_object(handle);
            frame.notes.push_back("0022c720 unregistered actor "+std::to_string(a));
        }
    }

    // 001b3400 for the shakes of the events from `from` on.
    void apply_shakes(const std::vector<CombatEvent>& events,std::size_t from){
        for(std::size_t k=from;k<events.size();++k){
            const auto& e=events[k];
            if(!e.discharge||e.discharge->kind!=WeaponDischargeEvent::Kind::shake)continue;
            float v=shake.at(e.actor)+e.discharge->a;
            if(!(v<=5.0F))v=5.0F;                                    // 0048a208
            shake[e.actor]=v;
            if(world.actor(e.actor).eligible())                      // +3314 exists while activated
                request_secondary_aim_recoil(actors->secondary_aim(e.actor),e.discharge->a,e.discharge->b);
        }
    }

    SecondaryModeInput mode_input(std::size_t a,std::uint8_t raise){
        const auto& s=world.actor(a);
        const auto& inv=inventory(a);
        SecondaryModeInput in;
        in.raise=raise!=0;
        in.item_kind=item_kind(a);
        in.hold_class=static_cast<std::uint8_t>(weapon_held_class(inv,db));
        in.inventory_18=inv.slots[1].item;                           // +3524 +18: slot 1's item (+4 + 16k, +4)
        in.inventory_84=inv.current_84;
        if(s.actions().highest_action<0)fail("001977f0 with no priority action (+320c)");
        in.state_320c=s.actions().highest_action;
        if(in.state_320c==8)in.component_810=s.physical_flags810();   // 00197868: +3310 (always present in play) +810
        in.flags_3204=s.state_words().current_3204;
        in.flags_3248=s.actions().flags_3248;
        if(config.with_object.at(a))in.byte_33ec=static_cast<std::uint8_t>(flashlight_is_on(flashlight_of(a))?1:0);   // the flashlight's on byte
        in.value_31c4=s.controls().desired[1];
        in.value_31d0=s.controls().speed;
        return in;
    }
    ActorAimHost aim_host(std::size_t a){
        ActorAimHost h;
        h.secondary=world.actor(a).eligible();
        h.secondary_mode=[this,a](std::uint8_t raise){(void)actors->apply_secondary_mode(a,mode_input(a,raise));};
        h.find_object=[this](std::int32_t id){return find(id);};
        h.object_id=[this](std::uint64_t o){return object_id(o);};
        h.eye_c0=[this](std::uint64_t o,std::int32_t sub){
            if(const auto g=gob(o))return g->eye;                                 // 0014d340: +e4
            return actors->eye_c0(index(o),sub);};
        // Virtual +e0: an actor's 00133e00 (SoloActorSimulation::position_e0). A node's (004d95b0) is
        // 0015bec0 cNode_v36: the translation +80 as a point (w 1). The lit spot's (004dff50) and a
        // GOB's (base 004df740) are 003cd420: the +50 transform's world row 3 (+40 of its cached
        // matrix +10, refreshed through +50 when dirty), VF0 (0,0,0,1) without a transform; the
        // same row 003cd260 (+e4) and 003cd1c0 (+e8) read.
        h.point_e0=[this](std::uint64_t o)->RootVector{
            if(const auto* n=node_at(o))return RootVector{n->position[0],n->position[1],n->position[2],1.0F};
            if(const auto s=spot_owner(o))return spot_position(*s);
            if(const auto g=gob(o))return g->position;                             // +e8 row 3
            return world.actor(index(o)).position_e0();};
        return h;
    }
    void deliver(std::size_t a,const TargetingSend& send){
        ActorAimMessage m;
        m.action=send.action;m.kind=send.kind;
        if(send.action==0x3e)m.word_2c=static_cast<std::uint32_t>(send.object_id);
        if(send.action==0x39&&send.kind==0xb){m.object_30=send.object_id;m.subtarget_34=send.subtarget;}
        if(send.action==0x39&&send.kind==1)m.word_2c=send.byte_2c;
        if(send.action==0x39&&send.kind==4)m.point_30=send.point;
        if(!handle_actor_aim_action(actors->aim_state(a),m,aim_host(a)))fail("an aim action the handler does not take");
    }

    TargetPickHost pick_host(std::size_t a){
        TargetPickHost h;
        h.actor=world.actor(a).handle();
        h.object_id=[this](std::uint64_t o){return object_id(o);};
        h.root_bytes=[this](std::uint64_t o){return root(o);};
        h.position_e4=[this](std::uint64_t o){return world.actor(index(o)).published_position();};
        h.eye_c0=[this](std::uint64_t o){return actors->eye_c0(index(o),-1);};
        h.kind_c4=[this](std::uint64_t o){return kind_c4(o);};
        h.stealth_44=[this](std::uint64_t o)->std::int32_t{(void)index(o);return 0;};
        h.actor_flag_346c=[this](std::uint64_t o){(void)index(o);return false;};
        h.controller=[this](std::uint64_t o)->std::optional<TargetPickThreat>{
            const auto& aim=combat->aim(index(o));
            TargetPickThreat t;
            if(aim.target_28){
                const auto v=find_index(aim.target_28);
                if(v&&config.object_ids[*v]==aim.target_id_2c)t.aim_target=aim.target_28;
            }
            t.progress=aim.progress_20;
            return t;
        };
        h.enemy=[this,a](std::uint64_t o){return enemy(world.actor(a).handle(),o);};
        h.blocker=[this](std::uint64_t t,std::int32_t age,bool force){return blocker(world.actor(config.player).handle(),t,age,force);};
        h.actor_matrix_f4=[this,a]{return rows_of(root_placement_matrix(world.actor(a).floor_root_placement()));};
        h.actor_frame_d8=[this,a]{return rows_of(root_placement_matrix(world.actor(a).component_placement()));};
        // 004ffc14's object (the camera's tracked object) is the local player.
        h.camera_frame_d8=[this]{return rows_of(root_placement_matrix(world.actor(config.player).component_placement()));};
        h.held_record=[this,a]()->std::optional<std::array<std::int32_t,2>>{
            const auto item=held_item(a);
            if(!item)return std::nullopt;
            const auto& r=db.record(*item);
            return std::array<std::int32_t,2>{static_cast<std::int8_t>(r.hold_class_f0()),r.far_3c()};
        };
        h.actor_33a8=blind_33a8(a);h.actor_3470=world.actor(a).item_actions().vision_3470;   // 001b31b0 (actor_goggles.hpp)
        h.candidates=hud.enemies_18;
        return h;
    }

    std::array<float,2> aim_update(std::size_t a,std::uint64_t target,std::int32_t item,std::int32_t subtarget){
        auto& s=world.actor(a);
        WeaponAimInput in;
        in.now_ms=input.now_ms;
        if(item>=0)in.record=&db.record(item);
        in.subtarget=subtarget;
        in.settings=config.to_hit_live?config.to_hit_live():config.to_hit;   // 0022FAE8: *D_0048ACB8 each call (132fx2)
        if(config.player_aim_time_pct)in.player_aim_time_pct=config.player_aim_time_pct();
        if(config.global_npc_skill)in.global_npc_skill=config.global_npc_skill();
        if(target){
            WeaponAimTarget wt;
            wt.object=target;
            if(const auto g=gob(target)){
                // A population GOB (vtable 004db4c0): +c0 0014d340 -> +e4, +c8 00140e90 (0),
                // +cc 0015c030 (1.0), +44 0 and no controller (+58 0; 003cf0e0, kept by 0021add0).
                wt.id_c=g->id;wt.eye_c0=g->eye;wt.stance_c8=0;wt.size_cc=1.0F;wt.stealth_44=0;
            }else{
                const auto t=index(target);
                wt.id_c=config.object_ids[t];
                wt.eye_c0=actors->eye_c0(t,-1);
                wt.stance_c8=actor_stance_c8(world.actor(t).state_words().current_3204);
                wt.size_cc=target_size_cc(subtarget);
                wt.stealth_44=config.stealth_44?config.stealth_44(t):0;           // 0022FA8C: target +44 (132fx2)
                wt.controller=WeaponAimTargetController{world.actor(t).player_controller(),world.actor(t).byte_14()!=0,velocity_74(t)};
            }
            in.target=wt;
            in.target_manager_22cb10=blocker(s.handle(),target,0,false)!=0;   // called first whenever a target is given (0022f63c)
        }
        in.shooter.actor=true;
        in.shooter.blinded_33a8=blind_33a8(a);
        in.shooter.player_54=s.player_controller();
        in.shooter.accuracy_multiplier=accuracy_multiplier(a);
        in.shooter.ignore_size_3470=world.actor(a).item_actions().vision_3470!=0;   // 0022f820: sltu (+3470 != 0)
        in.shooter.ready_d4=ready_d4(a);
        in.shooter.eye_c0=actors->eye_c0(a,-1);
        in.shooter.stance_c8=actor_stance_c8(s.state_words().current_3204);
        in.shooter.velocity_74=velocity_74(a);
        auto& aim=combat->aim(a);
        if(const auto notice=update_weapon_aim(aim,in))frame.aim_notices.push_back(*notice);
        return {aim.progress_20,aim.accuracy_24};
    }

    // The followed camera's +50 transform (+da0 is its copy after the camera
    // update): the aim view's shot frame (00176400 a3 != 0), aim point
    // (00249670) and thrown point (002497c0) read it.
    ShotFrame aim_camera() const{
        if(!config.aim_view_camera)fail("the camera +50 transform of the aim view (00176400, 00249b10, 002497c0) is not supplied");
        return config.aim_view_camera();
    }
    // 001437e0(inventory): the held record (00147840 selects record 0 for an
    // id outside the table), null with nothing held (+84 == 6).
    const WeaponRecord* held_record(std::size_t a) const{
        const auto item=held_item(a);
        return item?&db.record(*item):nullptr;
    }
    // 0022a8c0(actor, request, a2) with the native world LOS and candidate
    // scan (shared by the aim-view lock and the flashlight update).
    FlashlightProbeResult beam_probe(std::size_t a,const FlashlightProbeRequest& q,std::vector<BulletEvent>& scan_out){
        FlashlightProbeHost ph;
        ph.los=[this](const FlashlightLosRequest& r)->std::optional<RootVector>{
            WorldLosQuery w;w.origin=r.origin;w.displacement=r.displacement;
            w.excluded=static_cast<std::uint8_t>(r.mask&0xffU);       // 0013a480: the low byte of +58
            const auto hit=trace_world_los(world.collision(),w);
            if(!hit)return std::nullopt;
            return hit->point;};
        std::vector<CombatEvent> hits;
        const auto pool=combat->scan_host(hits);
        std::vector<BulletEvent> scan;
        ph.scan=flashlight_candidate_scan(pool,scan);
        ph.held_record=held_record(a);
        const auto r=flashlight_beam_probe_result(q,ph);
        for(auto& e:scan)scan_out.push_back(std::move(e));
        for(auto& e:hits)frame.events.push_back(std::move(e));
        return r;
    }
    // 00249b10(input, camera, actor): the aim-view lock. Writes inventory +f4.
    std::uint64_t aim_view_pick(std::size_t a){
        AimViewLockHost h;
        h.actor=world.actor(a).handle();
        h.shot_frame=[this,a]{return actors->shot_frame(a,false,true);};   // controller +64 (a2 = 0, a3 = 1)
        h.range=[this,a]{                                                  // 0013faf0
            const auto item=held_item(a);
            if(!item||*item==-1)return 0.0F;
            return static_cast<float>(db.record(*item).far_3c());};
        h.probe=[this,a](const FlashlightProbeRequest& q){                 // 0022a8c0(actor, request, 0x10)
            return beam_probe(a,q,frame.aim_view_scan);};
        h.enemy=[this,a](std::uint64_t o){return enemy(world.actor(a).handle(),o);};
        h.life_108=[this](std::uint64_t o){return world.actor(index(o)).life_state();};   // 001872f0
        h.object_id=[this](std::uint64_t o){return object_id(o);};
        auto r=aim_view_lock(h);
        combat->inventory(a).aim_view_range_f4=r.range_f4;                  // 00249ccc
        for(const auto& e:r.hud){frame.aim_view_hud.push_back(e);if(config.aim_view_hud)config.aim_view_hud(e);}
        return r.object;
    }
    // 002497c0(input + d0, camera, actor): the hold-class-1 aim point.
    void aim_view_thrown(std::size_t a){
        AimViewThrownHost h;
        h.actor=world.actor(a).handle();
        h.camera=[this]{return aim_camera();};
        h.record_3c=[this,a]{                                              // 001437e0 +3c
            const auto* r=held_record(a);
            if(!r)fail("002497c0 reads +3c of a null held record (nothing held)");
            return r->far_3c();};
        h.los=[this](const FlashlightLosRequest& r)->std::optional<AimViewLosHit>{
            WorldLosQuery w;w.origin=r.origin;w.displacement=r.displacement;
            w.excluded=static_cast<std::uint8_t>(r.mask&0xffU);
            const auto hit=trace_world_los(world.collision(),w);
            if(!hit)return std::nullopt;
            return AimViewLosHit{hit->point,hit->normal};};
        auto r=aim_view_thrown_point(h);
        pad.point_d0=r.point;
        for(const auto& e:r.hud){frame.aim_view_hud.push_back(e);if(config.aim_view_hud)config.aim_view_hud(e);}
    }

    PlayerTargetingHost targeting_host(std::size_t a){
        PlayerTargetingHost h;
        h.object_id=[this](std::uint64_t o){return object_id(o);};
        h.root_bytes=[this](std::uint64_t o){return root(o);};
        h.position_e4=[this](std::uint64_t o){return world.actor(index(o)).published_position();};
        h.subtarget_count_d0=[this](std::uint64_t o)->std::int32_t{(void)index(o);return 1;};   // 00184be0
        h.actor=world.actor(a).handle();
        h.actor_matrix_f4=[this,a]{return rows_of(root_placement_matrix(world.actor(a).floor_root_placement()));};
        h.current_item=[this,a]{return held_item(a).value_or(-1);};
        h.hold_class=[this,a]{return static_cast<std::uint8_t>(weapon_held_class(inventory(a),db));};
        h.item_kind=[this,a]{return item_kind(a);};
        h.pointer_33ec=[this,a]{return flashlight_is_on(flashlight_of(a));};   // the flashlight's on byte
        h.agent_unlock_2=[this]{
            if(!config.agent_unlock_2)fail("the agent-data unlock 2 (00332f90) is not supplied");
            return *config.agent_unlock_2;};
        h.lock_pick=[this,a](std::uint64_t current){return lock_target_pick(pick_host(a),current,recent);};
        h.auto_pick=[this,a](std::uint64_t previous){return auto_target_pick(pick_host(a),previous);};
        h.blocker=[this,a](std::uint64_t t,std::int32_t age,bool force){return blocker(world.actor(a).handle(),t,age,force);};
        h.aim_view_pick=[this,a]{return aim_view_pick(a);};
        h.camera_da0=[this]()->TargetingMatrix{
            if(!config.aim_view_camera_da0)fail("the camera +da0 of the aim point (00249670) is not supplied");
            return config.aim_view_camera_da0();};
        h.hud_target_changed=[]{return false;};                      // the camera tracks the local player
        h.aim_update=[this,a](std::uint64_t t,std::int32_t item,std::int32_t sub){return aim_update(a,t,item,sub);};
        h.multiplayer=static_cast<bool>(config.net_record);                 // 005721c8 (132ln5h)
        h.record=config.net_record;
        return h;
    }
    HudTargetsHost hud_host(){
        HudTargetsHost h;
        h.find=[this](std::int32_t id){return find(id);};
        h.object_id=[this](std::uint64_t o){return object_id(o);};
        // A node, a GOB and the lit spot have no controller (+58 0: 003cf0e0 at 003cf164, kept by 0021add0).
        h.has_controller=[this](std::uint64_t o){if(node_at(o)||gob(o)||spot_owner(o))return false;(void)index(o);return true;};
        h.player_controlled=[this](std::uint64_t o){
            if(node_at(o)||gob(o)||spot_owner(o))return false;
            const auto a=index(o);
            return world.actor(a).player_controller()||(config.coop_hud&&world.in_agent_slot(a));};   // port-only co-op HUD
        h.root_bytes=[this](std::uint64_t o){return root(o);};
        h.kind_c4=[this](std::uint64_t o){return kind_c4(o);};
        h.enemy=[this](std::uint64_t x,std::uint64_t y){return enemy(x,y);};
        h.local_actor=world.actor(config.player).handle();
        h.blocker=[this](std::uint64_t t){return blocker(world.actor(config.player).handle(),t,0,false);};
        h.threat_progress=[this](std::uint64_t o)->std::optional<float>{
            const auto& aim=combat->aim(index(o));
            if(!aim.target_28)return std::nullopt;
            const auto v=find_index(aim.target_28);
            if(!v||config.object_ids[*v]!=aim.target_id_2c)return std::nullopt;
            const auto& t=world.actor(*v);
            if(!t.player_controller()||t.byte_14()==0)return std::nullopt;
            return aim.progress_20;
        };
        if(config.hud_marker_added)h.added=[this](const HudTargetMarker& m){config.hud_marker_added(m.id,m.state_10);};
        return h;
    }

    // The pad producer's stages read and write the input component fields.
    void load_fields(PadCombatFields& f){
        state.selector_38=f.selector_38;
        pad.selector_mask_3c=f.bits_3c;pad.group_40=f.top_40;pad.group_mask_44=f.bits_44;
        state.recheck_failed_bc=f.buttons.byte_bc?1:0;
        // The Triangle interaction drops the pick (+c8 = 0, +cc = -1).
        if(f.buttons.word_c8==0&&state.pick_c8.object!=0)state.pick_c8={0,f.buttons.word_cc};
    }
    void store_fields(PadCombatFields& f){
        f.selector_38=state.selector_38;
        f.bits_3c=pad.selector_mask_3c;f.top_40=pad.group_40;f.bits_44=pad.group_mask_44;
        f.buttons.byte_bc=state.recheck_failed_bc!=0;
        // +c8 is a pointer in the original; the button state keeps only whether it is set.
        f.buttons.word_c8=state.pick_c8.object!=0?1:0;f.buttons.word_cc=state.pick_c8.id;
    }
    void targeting_stage(PadCombatFields& f){
        const auto a=config.player;
        actors->collect_inventory_view(a,*combat);
        load_fields(f);
        flashlight_pad_stage(f);                                                                            // 0024c980..0024ca44
        weapon_switch_stage(f);                                                                             // 0x0024ca48..0x0024cd30
        manual_reload_stage();                                                                              // 0x0024cdb0..0x0024cde4
        auto hh=hud_host();
        hh.sprites=[this](const HudSpriteEvent& e){frame.hud_sprites_pad.push_back(e);};
        const auto released=update_target_markers(hud,registered,world.actor(a).handle(),pad,state,hh);   // 0022c210
        frame.hud_none.push_back(released);
        if(config.lock_hud)for(std::size_t i=0;i<released;++i){TargetingEvent n;n.type=TargetingEvent::Type::hud_none_280;config.lock_hud(n);}
        const auto events=run_player_targeting(pad,state,input.r1,input.l1,input.dt,targeting_host(a));         // 00247d60
        for(const auto& e:events){
            frame.targeting.push_back(e);
            using T=TargetingEvent::Type;
            if(config.lock_hud&&(e.type==T::hud_changed_240||e.type==T::hud_target_380||e.type==T::hud_none_280||e.type==T::hud_aim_2e0))
                config.lock_hud(e);
            if(e.type==TargetingEvent::Type::send)deliver(a,e.send);
            if(e.type==TargetingEvent::Type::clear_recent)recent.clear();
            if(e.type==TargetingEvent::Type::thrown_point)aim_view_thrown(a);
            if(e.type==TargetingEvent::Type::hud_clear_960&&config.aim_view_hud){   // 00247e84
                AimViewHudEvent m;m.kind=AimViewHudEvent::Kind::meter_960;m.value=e.value;
                config.aim_view_hud(m);
            }
        }
        store_fields(f);
        actors->publish_inventory_view(a,*combat);
    }
    void weapon_stage(PadCombatFields& f){
        const auto a=config.player;
        actors->collect_inventory_view(a,*combat);
        load_fields(f);
        PadWeaponFrame wf;
        wf.square=input.square;wf.aim_selector_1e0=f.aim_1e0;wf.near_camera_1f0=f.near_1f0;wf.dt=input.dt;
        // sp+1e0 (0x0024be2c) is the previous frame's selector 8 and sp+1f0
        // the camera distance test 0x0024bdc0 / 0017a610 (Checkpoint 121).
        PadWeaponHost h;
        h.hold_class=[this,a]{return weapon_held_class(inventory(a),db);};
        if(world.actor(a).eligible()){
            const auto& st=actors->secondary(a).states;
            h.secondary=std::array<std::uint8_t,2>{st.current_310,st.next_311};
        }
        h.spare_6=[this,a]{std::vector<WeaponInventoryEvent> ev;return combat->inventory_ops(a,false,ev).spare(6);};
        h.clip_6=[this,a]{std::vector<WeaponInventoryEvent> ev;return combat->inventory_ops(a,false,ev).clip(6);};
        h.full_auto=[this,a]{std::vector<WeaponInventoryEvent> ev;return combat->inventory_ops(a,false,ev).full_auto();};
        h.item_notice=[this,a]{                                      // 00143770: if(*inv) 00263080(*inv)
            const auto& inv=inventory(a);
            if(!inv.object_0)return;
            if(combat->air_taser().release){combat->air_taser().release(a);return;}
            if(inv.object_0->state_8!=0)fail("00263080 on the inventory object (+8 set) is not hosted");};
        h.lock_valid=[this](std::uint64_t o,std::int32_t id){const auto v=find_index(o);return v&&config.object_ids[*v]==id;};
        h.send=[this,a](const ActorFireRequest& r){
            frame.fire_requests.push_back(r);
            PendingAction q;q.actor=a;q.kind=PendingAction::Kind::fire;q.fire=r;pending.push_back(q);};
        h.shot_counter=[this]{++frame.shot_counter;if(config.net_shot)config.net_shot();};   // 00249530
        h.aim_shot_frame=[this,a]{return actors->shot_frame(a,false,true);};   // controller +64 (a3 = 1)
        h.aim_range_f4=[this,a]{
            const auto& range=inventory(a).aim_view_range_f4;
            if(!range)fail("the inventory's aim-view range +f4 is not set");
            return *range;};
        h.actor_matrix_f4=[this,a]{return rows_of(root_placement_matrix(world.actor(a).floor_root_placement()));};
        (void)run_pad_weapon_block(pad,wf,h);
        store_fields(f);
        actors->publish_inventory_view(a,*combat);
    }
    // 0x0024f928..0x0024f964: +346e = 00241ec0(004ffc2c, sp+1e0, sp+200,
    // 001437e0(inventory), +346f ? 0013fb50 : 0) (pad_crosshair_346e); t0's
    // word +130 is the goggles reticle (Checkpoint 132cp4). +346e reaches
    // +3470 through 001b31b0 (vision_stage).
    void crosshair_stage(PadCombatFields& f){
        auto& bytes=world.actor(config.player).item_actions();
        actors->collect_inventory_view(config.player,*combat);
        std::optional<std::int32_t> goggles_130;
        if(bytes.byte_346f!=0){                                            // 0024f930..0024f944
            const auto item=combat->inventory(config.player).slots.at(6).item;   // 0013fb50: inventory +68
            if(item!=-1)goggles_130=db.record(item).word(0x130);           // 00241f04 lw +130(t0)
        }
        const auto* record=held_record(config.player);
        const auto v=config.crosshair_hud?config.crosshair_hud(f.aim_1e0,f.far_200,crosshair_held(record),goggles_130)
                                         :pad_crosshair_346e(f.aim_1e0,f.far_200,record,goggles_130);
        bytes.byte_346e=static_cast<std::uint8_t>(v);                      // 0024f964 sb +346e
    }
    // The fire notice +cc of an AI shooter for its discharges among
    // events[from..] (0013ff40 step 11, synchronous in the original).
    void ai_fire_notices(const std::vector<CombatEvent>& events,std::size_t from){
        for(std::size_t k=from;k<events.size();++k){
            const auto& e=events[k];
            if(!e.discharge||e.discharge->kind!=WeaponDischargeEvent::Kind::fire_notice)continue;
            if(e.actor>=fire_notices.size()||!fire_notices[e.actor])
                fail("the fire notice +cc (0014f460) of an AI shooter without a linked controller");
            fire_notices[e.actor](e.discharge->object,e.discharge->flag);
        }
    }
    // A discharge's reload_action (0013ff40 / 001960c0: 003c9420 of action 0x3c
    // kind 0 with transport 0) is queued for the next pump, and its
    // out_of_ammo (00140c74..00140cc4: cOutOfAmmoMsg 004ea150 to the player
    // controller, transport 0; single player, 005721c8 clear) and the
    // inventory update's weapon_value_message (00144270..00144314:
    // cWeaponVisChangeMsg 00583968 to the player controller, transport 0) go
    // to the actor's queue.
    void queue_reloads(const std::vector<CombatEvent>& events,std::size_t from){
        for(std::size_t k=from;k<events.size();++k){
            const auto& e=events[k];
            if(e.discharge&&e.discharge->kind==WeaponDischargeEvent::Kind::out_of_ammo){
                world.actor(e.actor).queue_out_of_ammo(e.discharge->value);
                continue;
            }
            if(e.discharge&&e.discharge->kind==WeaponDischargeEvent::Kind::weapon_value_message){
                world.actor(e.actor).queue_weapon_vis_change(e.discharge->value);
                continue;
            }
            if(!e.discharge||e.discharge->kind!=WeaponDischargeEvent::Kind::reload_action)continue;
            PendingAction q;q.actor=e.actor;q.kind=PendingAction::Kind::reload;pending.push_back(q);
        }
    }
    // Checkpoint 132f12: the player statistics into the local agent record
    // (00333f10(001314f0(004ffb50, -1), id, 1)), in the frame's event order.
    // Only the debrief and the scores screens read them (00333ee0 at 002cbdb4..,
    // 0035c1f4.., 00360a9c.., 0046251c..; play reads only 0x19, which these never
    // add), so adding them at the frame's end gives the same record.
    //   0013fe80(inventory, aim view), after each successful discharge (0013ff40):
    //   with the owner's (+88) controller +58 answering +54 (player) and its byte
    //   +14: statistic 9 (shots) and, in the aim view, 0xb.
    //   00188780 (the bullet hit): the statistic events 0xa / 0xc / 6 it decided.
    //   00176540 (the player component's +7c, the kill notice): statistic 8
    //   (PlayerDeaths), the solo actor's counter.
    void agent_statistics_of_frame(){
        auto add=[this](std::uint32_t id,const char* site){
            if(!config.agent_statistics)fail(std::string("the player statistic 00333f10 (")+site+") has no agent statistics (CombatPlayConfig::agent_statistics)");
            add_agent_statistic(*config.agent_statistics,id,1);
        };
        for(const auto& e:frame.events){
            if(e.discharge&&e.discharge->kind==WeaponDischargeEvent::Kind::shot_statistics){
                if(e.actor>=world.size())fail("the shot statistics 0013fe80 of an unknown actor");
                const auto& owner=world.actor(e.actor);
                if(!owner.player_controller()||owner.byte_14()==0)continue;   // 0013fea0..0013fee0
                add(9,"0013fefc");
                if(e.discharge->flag)add(0xb,"0013ff20");
            }else if(e.hit&&e.hit->kind==BulletHitEvent::Kind::statistic){
                add(static_cast<std::uint32_t>(e.hit->value),"00188bb8 / 00188be0 / 00188c14");
            }else if(e.bullet&&e.bullet->kind==BulletEvent::Kind::statistic){
                add(static_cast<std::uint32_t>(e.bullet->value),"002776fc");   // the hold-9 block (132uk4)
            }
        }
        const auto deaths=world.actor(config.player).player_deaths();
        if(deaths<player_deaths_seen)fail("the player's PlayerDeaths counter (00176540) went back");
        for(;player_deaths_seen<deaths;++player_deaths_seen)add(8,"00176568");
    }
    int player_deaths_seen{0};
    // 0013fdb8 (hud_script_state.hpp): the text the HUD ticker would show.
    std::string transfer_ammo_notice(const WeaponReloadEvent& e) const{
        if(!config.localization)fail("0013fd20's TransferAmmo HUD notice (003fdb50 / 00128bd0 / 00242b00) needs the localization");
        return omega::transfer_ammo_notice(*config.localization,db.record(e.item).display_name_80());
    }
    // 001a92a0 action 0x3c (001aa048) on actor a: +3524 is the combat
    // inventory, +3314 the session's secondary component (present with the
    // actor), +88 the actor (00144dc0) whose byte +14 enables 0013fd20's notice.
    void reload(std::size_t a){
        auto& actor=world.actor(a);
        ActorReloadInput in;
        in.mode_320c=actor.actions().highest_action;in.flags_3210=actor.actions().active_actions;
        in.secondary_3314=actor.eligible();in.actor=actor.handle();
        CombatReload rec;rec.actor=a;
        ActorReloadHost h;
        h.reload_allowed=[this,a]{
            std::vector<WeaponInventoryEvent> ev;
            const bool ok=combat->inventory_ops(a,false,ev).reload_allowed();   // 00143560
            combat->check_events(ev);
            return ok;};
        h.item_kind=[this,a]{return item_kind(a);};                           // 00143510
        h.one_shot=[this,a](const std::array<std::uint8_t,48>& name,std::uint8_t kind,const SecondaryOneShot& e){
            if(!throws)fail("the reload's one-shot (001cae20) completes only through the secondary playback 001cb160 (CombatPlayConfig::throws)");
            return request_secondary_one_shot(actors->secondary(a),name,kind,e);};
        h.start=[this,a,&actor,&rec]{
            auto& inv=combat->inventory(a);
            inv.reloading_a0=true;                                             // 001aa2d8
            PortAmmoGuard ammo(inv,actor.player_controller()&&port_cheat(PortCheat::infinite_ammo));   // port-only (132f5)
            reload_weapon(inv,db,actor.byte_14()!=0,rec.events);               // 0013fb90
            for(const auto& e:rec.events)
                if(e.kind==WeaponReloadEvent::Kind::transfer_notice)
                    rec.notices.push_back(transfer_ammo_notice(e));};
        actors->collect_inventory_view(a,*combat);
        rec.result=handle_actor_reload(in,h);
        actors->publish_inventory_view(a,*combat);
        frame.reloads.push_back(std::move(rec));
    }
    // 001b2598: 001b31b0 (actor_goggles.hpp) with +346e from crosshair_stage. The local player's HUD calls (002609e0,
    // 00241e30, 0053b540) go to CombatPlayConfig::vision_hud as they happen (Checkpoint 132cp4); without it they throw.
    void vision_stage(std::size_t a){
        auto& actor=world.actor(a);
        GogglesActorView v;
        v.controller_player=actor.player_controller();v.byte_14=actor.byte_14()!=0;
        v.crosshair_346e=actor.item_actions().byte_346e;                   // 001b31c8..001b31ec
        std::vector<GogglesEvent> ev;
        update_actor_vision(actor.item_actions(),v,ev);
        for(const auto& e:ev){
            using K=GogglesEvent::Kind;
            if(e.kind==K::hud_mode||e.kind==K::hud_vision||e.kind==K::vision_flag){
                if(!config.vision_hud)fail("the vision HUD calls 002609e0 / 00241e30 and 0053b540 of 001b31b0 need CombatPlayConfig::vision_hud");
                config.vision_hud(a,e);
            }
            frame.goggles.emplace_back(a,e);
        }
    }
    Flashlight* flashlight_of(std::size_t a){return a<flashlights.size()&&flashlights[a]?&*flashlights[a]:nullptr;}
    const Flashlight* flashlight_of(std::size_t a) const{return a<flashlights.size()&&flashlights[a]?&*flashlights[a]:nullptr;}
    LevelLights& lights(){
        if(!config.lights)fail("the flashlight's point lights (0045b610 / 0045ae70) need the level's light pools (CombatPlayConfig::lights)");
        return *config.lights;
    }
    // 0045bad0(fl, actor) at 001b5fe0 for roles 1 / 3: the spot's ID from
    // 0040c240(&id, 0xe7). The counters are this session's: the original's
    // shared 00571780[0xe7] also counts the category's other objects created
    // before (its index may differ; the ID is only compared with itself).
    void create_flashlight(std::size_t a){
        if(flashlights.size()<=a){flashlights.resize(a+1);spot_ids.resize(a+1,-1);}
        if(!config.with_object.at(a))return;
        const auto id=object_dynamic_id(spot_counters,0xe7,[this](std::uint32_t key){
            for(const auto s:spot_ids)if(static_cast<std::uint32_t>(s)==key)return true;
            return false;});
        spot_ids[a]=static_cast<std::int32_t>(id);
        flashlights[a]=make_flashlight(world.actor(a).handle(),spot_handle(a));
    }
    // 0022c650(004f77c0, spot) / 0022c720 (the spot keeps its +11c entries, as an actor does).
    void register_spot(std::size_t a){
        const auto spot=spot_handle(a);
        if(visibility.register_object(spot)){
            registered.push_back({spot,spot_ids.at(a)});
            frame.notes.push_back("0022c650 registered the lit spot of actor "+std::to_string(a));
        }else if(!visibility.slot(spot))frame.notes.push_back("0022c650: the 30 slots are full, the lit spot of actor "+std::to_string(a)+" not registered");
    }
    void apply_flashlight_events(std::size_t a,const std::vector<FlashlightEvent>& events){
        using K=FlashlightEvent::Kind;
        for(const auto& e:events){
            frame.flashlight.emplace_back(a,e);
            switch(e.kind){
            case K::spot_registered:register_spot(a);break;
            case K::spot_unregistered:
                visibility.unregister_object(spot_handle(a));
                frame.notes.push_back("0022c720 unregistered the lit spot of actor "+std::to_string(a));
                break;
            case K::goggle_mode:{
                // 001992b0(actor, value, 0) as the EquipGoggles host runs it (actor_goggles.hpp).
                auto& actor=world.actor(a);
                const bool record=combat->inventory(a).slots.at(6).item!=-1;
                SecondaryComponent* secondary=actor.eligible()?&actors->secondary(a):nullptr;
                const auto r=switch_actor_goggles(actor.item_actions(),static_cast<std::uint8_t>(e.value),false,record,secondary,actor.handle(),false);
                frame.notes.push_back("001992b0 (flashlight toggle) actor "+std::to_string(a)+" value "+std::to_string(e.value)
                    +(r.requested?(r.accepted?": switch_on_ir accepted":": switch_on_ir refused"):""));
                break;}
            case K::spot_deleted:fail("0045b990 (the flashlight's destructor) is not reached in play");
            case K::light_byte:combat->write_slot_render_byte(a,7,static_cast<std::uint8_t>(e.value));break;   // 00142e00(inv, 7, on)
            default:break;   // click sound (the play's gameSound post), set-state call and HUD calls: recorded
            }
        }
    }
    // 00199770 (tap) / 00199860 (hold) on actor a's +33ec.
    void toggle_flashlight_of(std::size_t a,bool goggles){
        auto* fl=flashlight_of(a);
        if(!fl)fail("the flashlight toggle of an actor without the object +33ec (roles 1 / 3)");
        auto& actor=world.actor(a);
        auto& bytes=actor.item_actions();
        FlashlightGoggles g{bytes.byte_33e5!=0,bytes.byte_33e6,bytes.byte_33e7!=0};
        const auto view=flashlight_toggle_actor(combat->inventory(a),db,actors->position_e4(a),actor.player_controller(),actor.byte_14()!=0);
        std::vector<FlashlightEvent> ev;
        if(goggles)toggle_goggles(*fl,g,lights(),view,ev);
        else toggle_flashlight(*fl,g,lights(),view,ev);
        bytes.byte_33e7=g.on_33e7?1:0;
        apply_flashlight_events(a,ev);
    }
    // 0024c980..0024ca44 of the pad producer: the HUD bytes *004ffc2c +6a8 /
    // +6a9 / +6aa are 0 in play (no HUD menu is open) and 00250350(input +38,
    // 7) is bit 7 of the current priority set +3c.
    void flashlight_pad_stage(const PadCombatFields& f){
        FlashlightPadGates g;
        g.priority_bit7=((f.bits_3c>>7)&1U)!=0;
        const auto r=update_flashlight_pad(flash_pad,input.dpad_right,input.dt,g);
        if(r!=FlashlightPadRequest::none)toggle_flashlight_of(config.player,r==FlashlightPadRequest::goggles);
    }
    // 0x0024ca48..0x0024cd30 of the pad producer (player_weapon_switch): the
    // clock is the play's millisecond clock (the aim accumulator's), the
    // GuiGameScreen +d4 and the hold-Select menu 0045ca20 come from the
    // host's GUI (checkpoint 132hud; without the GAMEGUI overlay +d4 is 0 and
    // a held Select throws naming 0045ca20) and byte 005721c8 (multiplayer)
    // is 0 in solo.
    void weapon_switch_stage(const PadCombatFields& f){
        const auto a=config.player;
        PlayerWeaponSwitchInput in;
        in.select_f80=input.select;in.clock_ms=input.now_ms;
        in.priority_bit11=((f.bits_3c>>11)&1U)!=0;
        in.selector_38=f.selector_38;
        in.mode_320c=world.actor(a).actions().highest_action;
        in.menu_128=pickup_menu.has_value();
        in.menu_start_98=pickup_menu&&pickup_menu->menu.start!=0;
        in.game_screen_d4_one=input.game_screen_d4_one;
        PlayerWeaponSwitchHost h;
        h.choose_menu_45ca20=input.choose_menu_45ca20;
        h.switch_allowed=[this,a]{std::vector<WeaponInventoryEvent> ev;return combat->inventory_ops(a,false,ev).switch_allowed();};
        h.cycle=[this,a](bool holster){
            const auto& inv=combat->inventory(a);
            const auto before=inv.current_84;
            std::vector<WeaponInventoryEvent> ev;
            combat->inventory_ops(a,*config.increased_ammo,ev).cycle(holster);   // 00143a60
            combat->check_events(ev);
            std::string note="weapon switch (0x0024cd0c 00143a60(+3524, "+std::to_string(holster?1:0)+")): slot "
                +std::to_string(int(before))+" -> "+std::to_string(int(inv.current_84));
            if(inv.current_84<inv.slots.size())note+=" item "+std::to_string(inv.slots[inv.current_84].item);
            frame.notes.push_back(note);};
        if(config.net_weapon){                                              // 132ln5h: 005721c8 set
            in.multiplayer_5721c8=true;
            h.multiplayer_2493c0=[this,a]{
                const auto& inv=combat->inventory(a);
                config.net_weapon(inv.current_84<inv.slots.size()?inv.slots[inv.current_84].item:-1);};
        }
        run_player_weapon_switch(weapon_switch,in,h);
    }
    // 0x0024cdb0..0x0024cde4 of the pad producer: 0024aa70(input, R3 sp+fd8 != 0 && !input +ba, actor). Input +ba
    // has no writer (the constructor's 0), so R3 held asks every frame; with 00143560 allowing a reload it sends
    // action 0x3c kind 0 with transport 0 (003c9420), which the actor's pump takes as a discharge's reload.
    void manual_reload_stage(){
        if(input.r3==0.0F)return;
        const auto a=config.player;
        std::vector<WeaponInventoryEvent> ev;
        const bool ok=combat->inventory_ops(a,false,ev).reload_allowed();   // 0024aa84 00143560(+3524)
        combat->check_events(ev);
        if(!ok)return;
        PendingAction q;q.actor=a;q.kind=PendingAction::Kind::reload;pending.push_back(q);
        frame.notes.push_back("manual reload (R3, 0x0024cde4 0024aa70): action 0x3c queued");
    }
    // 001b1d74: 0045ae70(fl, M, dt) while the flashlight is on (001b1710).
    void flashlight_update_stage(std::size_t a,float dt){
        auto* fl=flashlight_of(a);
        if(!flashlight_is_on(fl))return;
        auto& actor=world.actor(a);
        const auto* slots=actors->attach_slots(a);
        if(!slots)fail("the flashlight's attach point 7 (00385240) of a model without its attach slots");
        FlashlightFrameInput in;
        in.attach=actors->attach_point_frame(a,flashlight_attach_index,flashlight_attach_point(*slots));
        in.dt=dt;
        if(config.aim_view_camera)in.camera_position=config.aim_view_camera()[3];   // camera +50 transform +40
        in.short_range=(actor.state_words().current_3204&0x10U)!=0&&actor.eligible()&&actor.physical().auxiliary.first_hit!=0;
        in.probe=[this,a](const FlashlightProbeRequest& q){return beam_probe(a,q,frame.flashlight_scan).step;};
        if(perception){
            for(const auto& k:perception->system->think_list){                         // 004ea190
                bool found=false;
                for(std::size_t b=0;b<world.size()&&!found;++b){
                    const NpcAiController* c=world.actor(b).player_controller()?nullptr:perception->controller(b);
                    if(c&&&c->component()==k.component){
                        in.think_list.push_back({world.actor(b).handle(),actors->position_e4(b),true,true});
                        found=true;
                    }
                }
                if(!found)fail("a think-list entry whose actor the session does not know");
            }
        }
        const auto r=update_flashlight(*fl,lights(),flash_tuning,in);
        for(const auto& s:r.sight){
            frame.flashlight_sight.emplace_back(a,s);
            if(perception&&perception->flashlight_sight)perception->flashlight_sight(index(s.object),s.source);
        }
    }
    void inventory_stage(std::size_t a,float dt){
        actors->collect_inventory_view(a,*combat);
        const auto from=frame.events.size();
        combat->update_inventory(a,dt,frame.events);                  // 00144080 (001b1d6c)
        apply_shakes(frame.events,from);
        actors->publish_inventory_view(a,*combat);
        ai_fire_notices(frame.events,from);
        queue_reloads(frame.events,from);
        flashlight_update_stage(a,dt);
        // 001b2560: +352c decays by 5 per second (0048a200) to 0.
        if(!(shake[a]<=0.0F)){
            const float v=shake[a]-dt*5.0F;
            shake[a]=v<0.0F?0.0F:v;
        }
        vision_stage(a);
        actors->publish_inventory_view(a,*combat);
    }
};

CombatPlaySession::CombatPlaySession(ActorWorld& world,const WeaponDatabase& database,CombatPlayConfig config)
    :impl_(std::make_unique<Impl>(world,database,std::move(config))){
    auto& s=*impl_;
    const auto n=world.size();
    if(s.config.player>=n||!world.actor(s.config.player).player_controller())fail("the player index does not name a player actor");
    if(s.config.object_ids.size()!=n||s.config.models.size()!=n||s.config.with_object.size()!=n||s.config.record_items.size()!=n)
        fail("one object ID, model, inventory flag and record entry per actor");
    if(s.config.frame_0055a818<50)fail("the start value of the frame counter 0055a818 must be at least 50");
    if(!s.config.increased_ammo)fail("IncreasedAmmo (agent-data unlock 5) is not supplied");
    s.fp=s.config.frame_0055a818;
    ActorWorldCombatConfig ac;
    ac.object_ids=s.config.object_ids;ac.models=s.config.models;ac.agent_unlock_0=s.config.agent_unlock_0;
    ac.effects_not_drawn=s.config.effects_not_drawn;
    ac.effect_table=s.config.effect_table;
    ac.scene_cull_38=s.config.scene_cull_38;
    ac.frame_rate_1e8=s.config.frame_rate_1e8;
    ac.lights=s.config.lights;
    Impl* self=impl_.get();
    // The body carry's backpack operations on the body's +3464 (its last
    // drop while no destroy 00254770 cleared it).
    {
        SoloActorSimulation::CarryHooks h;
        h.backpack=[self](std::uint64_t body){return self->carried_backpack(self->index(body))!=nullptr;};
        h.hide_backpack=[self](std::uint64_t body){
            auto* b=self->carried_backpack(self->index(body));
            if(!b)fail("00256120: the body has no backpack +3464");
            const auto from=b->object_events.size();
            backpack_object_hide(b->object,b->object_events);
            self->apply_object_events(*b,from);
        };
        h.turn_on_weapon_pickup=[self](std::uint64_t body){
            const auto a=self->index(body);
            auto* b=self->carried_backpack(a);
            if(!b)return;   // 001a3250: +3464 == 0 does nothing
            const BackpackDropActor d=self->drop_actor(a);
            const auto from=b->object_events.size();
            backpack_object_turn_on(b->object,b->address,b->transform,d,self->object_host(),b->object_events);
            self->apply_object_events(*b,from);
        };
        world.set_carry_hooks(std::move(h));
    }
    if(s.config.weapon_models)
        ac.attached_muzzle=[self](std::size_t a,std::uint8_t point)->std::optional<RootVector>{
            const auto model=self->combat->attach_table(a).at(point);
            if(model==0)return std::nullopt;
            return self->config.weapon_models->muzzle_130(model);};
    if(s.config.materials)
        ac.static_material=[&world,self](const BulletStaticHit& hit)->const MaterialRow*{
            // hit.source: the hit triangle's (geometry, triangle, object) (world_los gather).
            const CollisionTriangleSource source{hit.source[0],hit.source[1],hit.source[2]};
            const auto bytes=world.collision().geometry(source.geometry);
            // 001e1dc0's static default surface before COL version 3 is not hosted.
            if(bytes.size()<4||bytes[3]<3)fail("a bullet static hit on a pre-version-3 COL (its default surface) is not hosted");
            const auto& table=*self->config.materials;
            return &table.row(material_surface_index(table,world.collision().refresh(source).surface));};
    ac.static_hit=[&world,self](const RootVector& start,const RootVector& d,std::uint64_t){
        return bullet_static_los(world.collision(),start,d,[self](const RootVector& point,const CollisionTriangleSource& source){
            CombatWaterHit w{point,std::bit_cast<float>(0x3fa00000U),source.geometry,source.triangle};
            if(const auto b=self->combat->bullet_event_cursor())w.at={*b,true};else w.at={self->frame.events.size(),false};
            self->frame.water_hits.push_back(w);});};
    // The player controller's +64 (00176400, a3 != 0) reads the camera at +1a0.
    ac.aim_view_camera=[self](std::size_t a){
        if(a!=self->config.player)fail("the aim-view shot frame of an actor that is not the local player is not hosted");
        return self->aim_camera();};
    ac.static_object_id=[self](std::uint64_t o)->std::int32_t{
        for(const auto& t:self->config.terrain)if(t.handle==o)return t.id;
        if(self->config.object_id)if(const auto id=self->config.object_id(o))return *id;
        if(const auto* n=self->node_at(o))return static_cast<std::int32_t>(n->id);   // +c: a SetTarget node's weak handle
        fail("a static hit object that is neither a terrain, a population object nor a node (its class is not hosted)");};
    // 003ccb00 on a terrain object (vtable 004d9be0) in single player: +40 is 0, so nothing changes.
    // A population object (GOB vtables 004db4c0 / ...) takes the same 003ccb00 on its own +40.
    ac.static_damage=[self](std::uint64_t o,std::uint64_t shooter,std::uint8_t type,float amount){
        for(const auto& t:self->config.terrain)if(t.handle==o)return;
        if(self->config.object_id&&self->config.object_damage&&self->config.object_id(o)){
            self->config.object_damage(o,shooter,type,amount);return;}
        fail("static damage on an object that is neither a terrain nor a population object (its +100 is not hosted)");};
    s.actors=std::make_unique<ActorWorldCombat>(world,std::move(ac));
    s.combat=std::make_unique<CombatWorld>(database,*s.actors,world.random());
    s.actors->set_effect_cursor([self]{
        if(const auto b=self->combat->bullet_event_cursor())return ActorHitEffect::Cursor{*b,true};
        return ActorHitEffect::Cursor{self->frame.events.size(),false};});
    s.combat->set_weapon_models(s.config.weapon_models);
    if(s.config.npc_dmg_aim_thresh)s.combat->set_npc_dmg_aim_thresh(s.config.npc_dmg_aim_thresh);
    s.combat->construct_inventories(s.config.with_object);
    s.shake.assign(n,0.0F);
    for(std::size_t a=0;a<n;++a)s.create_flashlight(a);                  // 001b5fe0 -> 0045bad0
    for(std::size_t a=0;a<n;++a)s.add_record_items(a);
    // Actors present at load: 0022c650 at 001b5410 (001b53d0) or 001b50d0 (001b4f40).
    for(std::size_t a=0;a<n;++a)if(world.actor(a).eligible())s.register_actor(a);
    // 0022ba30 walks the target manager's list 004f78dc: only registered
    // objects are candidates of the bullet scan and the aim-view probe (an
    // actor that never became present is not one).
    {
        CombatWorld::ObjectList ol;
        ol.candidates=[self]{
            std::vector<BulletListEntry> l;l.reserve(self->registered.size());
            for(const auto& t:self->registered)l.push_back({t.object,t.id});
            BulletListHost h;
            h.id_c=[self](std::uint64_t o){return self->object_id(o);};
            h.bounds_88=[self](std::uint64_t o)->std::optional<std::array<RootVector,2>>{
                if(self->node_at(o)||self->spot_owner(o))return std::nullopt;   // 0013ee60: null (the lit spot's vtable 004dff50 too)
                if(const auto a=self->find_index(o))return self->actors->bounds_88(*a);   // 00184380: +2e80
                const auto g=self->gob(o);
                if(!g)fail("0022ba30: a listed object that is neither an actor, a node nor a hosted GOB");
                if(!g->bounds_hosted)fail("0022ba30: the listed object's virtual +88 (GOB 00213ac0 / hotbox 00173660) is not hosted");
                return g->bounds_88;};
            h.position_e4=[self](std::uint64_t o){
                if(const auto a=self->find_index(o))return self->actors->position_e4(*a);
                const auto g=self->gob(o);
                if(!g)fail("0022ba30: the +e4 of a listed object that is not hosted");
                return g->eye;};                                            // +c0 0014d340 jumps to +e4
            return target_list_candidates(l,h);};
        ol.base_object=[self](std::uint64_t o){return !self->find_index(o)&&(self->node_at(o)||self->gob(o));};
        ol.base_eye_c0=[self](std::uint64_t o){
            if(const auto g=self->gob(o))return g->eye;
            const auto* n=self->node_at(o);
            if(!n)fail("0022ba30: a designated listed object that is neither a node nor a hosted GOB");
            return RootVector{n->position[0],n->position[1],n->position[2],1.0F};};   // +c0 0014d340 -> +e4 0015bef0: +80, w 1
        s.combat->set_object_list(std::move(ol));
    }
    for(std::size_t a=0;a<n;++a)s.hook(a);
    s.actors->attach_secondary_updates();
    if(s.config.throws){
        CombatThrowSession ts;
        ts.world=&world;ts.actors=s.actors.get();ts.combat=s.combat.get();ts.database=&database;ts.registered=&s.registered;
        ts.object_ids=s.config.object_ids;
        ts.statistics=s.config.agent_statistics?&*s.config.agent_statistics:nullptr;
        ts.find=[self](std::int32_t id){return self->find(id);};
        // Checkpoint 132pc1: a node's +40 is the base constructor's 0 (003cf0e0 at 003cf144; 0015c390 does not write
        // it; nor does the lit spot's 0045bad0), a GOB's is its hosted +40. All three +108 are 00141850 (004d95b0 / 004dff50 / GOB).
        ts.base_object=[self](std::uint64_t o)->std::optional<CombatThrowSession::BaseObject>{
            if(self->find_index(o))return std::nullopt;
            if(const auto* n=self->node_at(o))return CombatThrowSession::BaseObject{static_cast<std::int32_t>(n->id),object_life_state(0.0F)};
            if(const auto sp=self->spot_owner(o))return CombatThrowSession::BaseObject{self->spot_ids.at(*sp),object_life_state(0.0F)};   // vtable 004dff50 +108 00141850
            if(self->host_objects)if(const auto g=self->host_objects(o))return CombatThrowSession::BaseObject{g->id,object_life_state(g->health_40)};
            return std::nullopt;
        };
        ts.fire_notice=[self](std::size_t a,std::uint64_t target,bool flag){
            if(a>=self->fire_notices.size()||!self->fire_notices[a])fail("the fire notice +cc (0014f460) of an AI thrower without a linked controller");
            self->fire_notices[a](target,flag);};
        s.throws=std::make_unique<CombatThrows>(std::move(ts),*s.config.throws);
        // Checkpoint 132uk4: the bullet update's hold-8/9 area and blast go to the throw flow's world.
        s.combat->set_bullet_effects({[self](const BulletAreaCall& c){self->throws->bullet_area(c);},
                                      [self](const BulletBlastCall& c,const BulletObjectShooter* o){self->throws->bullet_blast(c,o);},
                                      // Checkpoint 132uk6: the discharge's launched projectile joins the throw world.
                                      [self](const ObjectWeakHandle& owner,const ShotRequest& r,const RootVector& point,float arc){
                                          self->throws->launch_discharge(owner,r.frame,r.record_dc,point,arc);}});
        // Checkpoint 132ia1c: the air taser (hold class 5) on the inventory object +0.
        s.combat->set_air_taser({[self](std::size_t a,std::uint64_t t,bool aim,std::int32_t sub){return self->throws->air_taser_fire(a,t,aim,sub);},
                                 [self](std::size_t a,float dt){self->throws->air_taser_update(a,dt);},
                                 [self](std::size_t a){self->throws->air_taser_release(a);}});
    }
    if(s.config.innocent_hit_chance)s.combat->set_innocent_hit_chance(s.config.innocent_hit_chance);
    PadCombatHooks hooks;
    hooks.targeting=[self](PadCombatFields& f){self->targeting_stage(f);};
    hooks.weapon=[self](PadCombatFields& f){self->weapon_stage(f);};
    hooks.crosshair=[self](PadCombatFields& f){self->crosshair_stage(f);};
    // 0x002478ac..0x002478c8: the lock counts while +c0's +c id equals +c4.
    hooks.camera_lock=[self]{
        PadCombatHooks::CameraLock l;
        const auto& p=self->pad;
        if(p.lock_c0!=0){
            const auto v=self->find_index(p.lock_c0);
            if(v&&self->config.object_ids[*v]==p.lock_id_c4)l.lock=std::pair{p.lock_c0,p.lock_id_c4};
        }
        l.subtarget_e4=p.subtarget_e4;
        return l;};
    // 0x0024dc5c: 0014d350(+c8); the target's +50 (its transform) is
    // assumed present for every registered actor.
    hooks.pick_resolves=[self]{
        const auto& c=self->state.pick_c8;
        if(c.object==0)return false;
        const auto v=self->find_index(c.object);
        return v&&self->config.object_ids[*v]==c.id;};
    // 0x00247b40: 001437a0(+3524).
    hooks.hold_class=[self]{
        const auto a=self->config.player;
        return static_cast<std::uint8_t>(weapon_held_class(self->inventory(a),self->db));};
    // 0x00247cd8: every combat actor holds the secondary component +3314.
    hooks.secondary_250=[self]()->std::optional<RootVector>{
        return self->actors->secondary_aim(self->config.player).curve_230.current;};
    // 0x0024f9bc: 00142f20 on the player's inventory. The byte 004ea140 it
    // reads at 0x001430bc is 0 in the booted image and no writer of it is
    // known (assumed 0).
    if(self->config.camera_field_of_view)hooks.field_of_view=[self](const WeaponFieldOfViewInput& in){
        auto q=in;q.byte_4ea140=false;
        q.zoom_sound=[self](bool start){
            self->frame.zoom_sounds.push_back({self->config.player,start,self->combat->inventory(self->config.player).value_e8});};
        return weapon_field_of_view(self->combat->inventory(self->config.player),self->db,q);};
    // Checkpoint 132t: input +128, the pickup menu of the backpack receiver 001762c0.
    hooks.pickup_menu.open=[self]{return self->pickup_menu.has_value();};
    hooks.pickup_menu.previous=[self]{self->menu_call("00251690",[self](auto& h){pickup_menu_previous(self->pickup_menu->menu,h);});};
    hooks.pickup_menu.next=[self]{self->menu_call("002517d0",[self](auto& h){pickup_menu_next(self->pickup_menu->menu,h);});};
    hooks.pickup_menu.left=[self]{self->menu_call("00251370",[self](auto& h){pickup_menu_left(self->pickup_menu->menu,h);});};
    hooks.pickup_menu.right=[self]{self->menu_call("00251500",[self](auto& h){pickup_menu_right(self->pickup_menu->menu,h);});};
    hooks.pickup_menu.confirm=[self]{
        bool close=false;
        self->menu_call("00251920",[self,&close](auto& h){close=pickup_menu_confirm(self->pickup_menu->menu,h);});
        return close;};
    hooks.pickup_menu.close=[self]{self->close_pickup_menu();};
    // 0x0024c4f8..0x0024c5d0: 001ca010(msg, 3, "GS_CONFIRM" 004a77b8, 0, 0),
    // 0020faa0, 003c9420(msg, 004ffb50, 1).
    hooks.pickup_menu.confirm_sound=[self]{
        self->frame.menu_sounds.push_back({"GS_CONFIRM",1,0,{}});
        self->frame.notes.push_back("pickup menu: sound GS_CONFIRM (0x0024c570 001ca010(msg, 3, name) -> 003c9420(004ffb50, 1))");};
    // 002473e0(input, object): +128 set and its +0 (the backpack) is the object.
    hooks.pickup_menu.close_for=[self](std::uint32_t key){
        if(!self->pickup_menu)return;
        const auto& b=self->backpacks.at(self->pickup_menu->backpack);
        if(b.object.word(0xc)==key)self->close_pickup_menu();};
    world.actor(s.config.player).set_pad_combat_hooks(std::move(hooks));
    if(s.config.backpack_world){
        if(s.config.backpacks_not_simulated)fail("backpack_world and backpacks_not_simulated together");
        world.actor(s.config.player).set_object_trigger_hooks(
            [self](std::uint32_t key){return self->backpack_of_key(key).has_value();},   // 0040c540(event +2c)
            [self](std::uint32_t key,PlayerButtonState& state){self->trigger_backpack(key,state);});
    }
    s.actors->publish_inventory_views(*s.combat);
}

void CombatPlaySession::append_actor(std::int32_t object_id,std::optional<CombatActorModel> model,bool with_object,
    std::optional<std::array<std::uint32_t,2>> record_items,std::optional<SecondaryMotionRegistry> registry){
    auto& s=*impl_;
    const auto a=s.config.object_ids.size();
    if(s.world.size()!=a+1)fail("append one actor right after the world appends it");
    if(s.world.actor(a).player_controller())fail("an appended actor is an AI actor");
    if(s.throws&&!registry)fail("an appended actor needs its motion registry for the throw flow");
    s.config.object_ids.push_back(object_id);s.config.models.push_back(model);
    s.config.with_object.push_back(with_object);s.config.record_items.push_back(record_items);
    s.actors->append_actor(object_id,std::move(model));
    s.combat->append_inventory(with_object);                              // 00144dc0
    s.shake.push_back(0.0F);
    s.create_flashlight(a);
    s.add_record_items(a);
    s.hook(a);
    if(s.throws)s.throws->append_actor(object_id,std::move(registry));
    s.actors->publish_inventory_view(a,*s.combat);
}
void CombatPlaySession::reset_actor(std::size_t a,std::optional<CombatActorModel> model,
    std::optional<std::array<std::uint32_t,2>> record_items,std::optional<SecondaryMotionRegistry> registry){
    auto& s=*impl_;
    if(a>=s.config.object_ids.size())fail("reset of an unknown actor");
    if(s.world.actor(a).player_controller())fail("a reset actor is an AI actor");
    if(s.config.with_object.at(a))fail("ResetNpc's rebuild 001b8130 is role 2 (no 0xd0 object)");
    if(s.throws&&!registry)fail("a reset actor needs its motion registry for the throw flow");
    s.config.models[a]=model;s.config.record_items[a]=record_items;
    s.actors->reset_actor(a,std::move(model));
    s.combat->reset_inventory(a,false);                                    // 00144dc0
    s.shake.at(a)=0.0F;
    s.add_record_items(a);
    s.hook(a);
    if(s.throws)s.throws->reset_actor(a,std::move(registry));
    s.actors->publish_inventory_view(a,*s.combat);
}
std::vector<WeaponDischargeEvent> CombatPlaySession::stop_weapon_loop_sound(std::size_t actor,std::uint8_t hand){
    auto& s=*impl_;
    if(actor>=s.config.object_ids.size())fail("00144ba0: no inventory of actor "+std::to_string(actor));
    std::vector<WeaponDischargeEvent> events;
    omega::stop_weapon_loop_sound(s.combat->inventory(actor),s.db,hand,events);
    return events;
}

CombatPlaySession::~CombatPlaySession(){
    auto& s=*impl_;
    s.throws.reset();
    for(std::size_t a=0;a<s.world.size();++a){
        s.world.actor(a).set_presence_hook({});
        s.world.actor(a).set_inventory_update({});
        s.world.actor(a).set_secondary_update({});
        s.world.actor(a).set_death_secondary_hooks({},{});
        s.world.actor(a).set_secondary_component({});
        s.world.actor(a).set_equipment_hook({});
    }
    s.world.actor(s.config.player).set_pad_combat_hooks({});
    if(s.config.backpack_world)s.world.actor(s.config.player).set_object_trigger_hooks({},{});
}

void CombatPlaySession::set_lights(LevelLights* lights){impl_->config.lights=lights;impl_->actors->set_torch_lights(lights);}

void CombatPlaySession::begin_frame(const CombatPlayInput& input){
    auto& s=*impl_;
    if(s.in_frame)fail("begin_frame twice");
    if(!(input.dt>0.0F))fail("dt must be positive");
    s.in_frame=true;s.input=input;
    auto notes=std::move(s.frame.notes);   // registrations and drops made between frames
    auto dropped=std::move(s.frame.backpacks);
    s.frame=CombatPlayFrame{};s.frame.notes=std::move(notes);s.frame.backpacks=std::move(dropped);
    s.actors->collect_inventory_views(*s.combat);
    // The pump (001a92a0) in send order: 001aa000 -> 001960c0 for a fire
    // request, the aim / look handlers for the NPCs' actions.
    auto pending=std::move(s.pending);s.pending.clear();
    for(auto& q:pending){
        if(q.kind==Impl::PendingAction::Kind::reload){
            s.reload(q.actor);
            s.actors->collect_inventory_views(*s.combat);
        }else if(q.kind!=Impl::PendingAction::Kind::aim){
            ActorFireRequest r=q.fire;
            if(q.kind==Impl::PendingAction::Kind::ai_fire){
                r.target=r.target_id==-1?0:s.find(r.target_id);
                if(r.target==0)r.target_id=-1;
            }
            const auto from=s.frame.events.size();
            s.combat->queue_fire(q.actor,r);
            s.combat->pump(s.frame.events);
            s.actors->publish_inventory_views(*s.combat);
            s.ai_fire_notices(s.frame.events,from);
            s.queue_reloads(s.frame.events,from);
            s.actors->collect_inventory_views(*s.combat);
        }else if(!handle_actor_aim_action(s.actors->aim_state(q.actor),q.aim,s.aim_host(q.actor)))
            fail("an AI action the aim handler does not take");
    }
    s.apply_shakes(s.frame.events,0);
    s.actors->publish_inventory_views(*s.combat);
}

CombatPlayFrame CombatPlaySession::end_frame(){
    auto& s=*impl_;
    if(!s.in_frame)fail("end_frame without begin_frame");
    s.actors->collect_inventory_views(*s.combat);
    const auto bullet_base=s.frame.events.size();
    s.combat->update_bullets(s.input.dt,s.frame.events);              // 00275670
    const auto bullet_hits=s.combat->last_bullet_hit_events();
    if(s.throws){                                                     // the thrown items after the bullet manager
        s.throws->update(s.input.dt);
        s.frame.throws=s.throws->take_frame();
    }
    s.actors->publish_inventory_views(*s.combat);
    auto hh=s.hud_host();
    hh.sprites=[&s](const HudSpriteEvent& e){s.frame.hud_sprites_update.push_back(e);};
    s.frame.hud_meter=update_hud_targets(s.hud,hh);                    // 00273b40
    if(s.config.backpack_world)s.tick_backpacks();                    // 002568f0
    s.frame.damage=s.actors->take_damage();
    s.frame.effects=s.actors->take_effects();
    // A request inside the bullet update came before the bullet event it
    // counted; that update listed its hit events ahead of its bullet events.
    for(auto& e:s.frame.effects)if(e.at.bullet)e.at={bullet_base+bullet_hits+e.at.event,false};
    for(auto& w:s.frame.water_hits)if(w.at.bullet)w.at={bullet_base+bullet_hits+w.at.event,false};
    s.agent_statistics_of_frame();
    s.frame.secondary=s.actors->take_secondary_steps();
    s.frame.torch=s.actors->take_torch_calls();
    ++s.fp;                                                           // 00165610
    s.in_frame=false;
    auto out=std::move(s.frame);
    s.frame=CombatPlayFrame{};
    return out;
}

bool CombatPlaySession::set_radar_blip(std::int32_t id,bool on){
    auto& s=*impl_;
    auto hh=s.hud_host();
    hh.sprites=[&s](const HudSpriteEvent& e){s.frame.hud_sprites_pad.push_back(e);};
    return on?add_hud_target(s.hud,id,true,hh):remove_hud_target(s.hud,id,false,hh.sprites);
}
bool CombatPlaySession::set_player_weapons(std::int32_t first,std::int32_t second){
    auto& s=*impl_;
    const auto a=s.config.player;
    s.actors->collect_inventory_view(a,*s.combat);
    std::vector<WeaponInventoryEvent> ev;
    s.combat->inventory_ops(a,*s.config.increased_ammo,ev).set_player_weapons(first,second);
    s.combat->check_events(ev);
    s.actors->publish_inventory_view(a,*s.combat);
    s.frame.notes.push_back("SetPlayerWeapons("+std::to_string(first)+", "+std::to_string(second)+")");
    return true;
}
bool CombatPlaySession::player_add_weapon(std::int32_t item){
    auto& s=*impl_;
    if(!s.config.weapon_models)return false;
    const auto record=item<0||static_cast<std::size_t>(item)>=s.db.size()?0:item;   // 00147840: record 0 outside
    if(s.db.record(record).model_13c()==0){                                         // 00175c38
        s.frame.notes.push_back("AddWeapon(Player, "+std::to_string(item)+"): record +13c is 0, nothing");
        return true;
    }
    const auto a=s.config.player;
    s.actors->collect_inventory_view(a,*s.combat);
    std::vector<WeaponInventoryEvent> ev;
    s.combat->inventory_ops(a,*s.config.increased_ammo,ev).add(item,true,false);   // 00143df0(+3524, item, 1, 1, 0, 0)
    s.combat->check_events(ev);
    s.actors->publish_inventory_view(a,*s.combat);
    s.frame.notes.push_back("AddWeapon(Player, "+std::to_string(item)+")");
    return true;
}
bool CombatPlaySession::remove_all_backpacks(){
    auto& s=*impl_;
    if(!s.config.backpack_world)return false;
    std::vector<BackpackObjectEvent> events;
    backpack_remove_drops(s.backpack_list,s.object_host(),events);   // 002570d8..0025711c
    std::size_t start=0;
    for(std::size_t i=0;i<events.size();++i){
        const auto& e=events[i];
        if(e.kind!=BackpackObjectEvent::Kind::release||(e.a&0xf8000000U)!=0xb0000000U)continue;
        auto& b=s.backpack_at(e.a);
        if(s.pickup_menu&&&s.backpacks[s.pickup_menu->backpack]==&b)
            fail("002570c0 destroys the backpack of the open pickup menu (ResetLevel closes it first; RemoveAllBackpacks does not)");
        const auto from=b.object_events.size();
        b.object_events.insert(b.object_events.end(),events.begin()+static_cast<std::ptrdiff_t>(start),
            events.begin()+static_cast<std::ptrdiff_t>(i+1));
        b.destroyed=true;
        s.apply_object_events(b,from);
        start=i+1;
    }
    if(start!=events.size())fail("002570c0: events outside a destroy");
    events.clear();
    backpack_crate_restore(s.backpack_list,s.object_host(),s.crate_snapshots,events);   // 00256d90
    for(const auto& e:events){
        if(e.kind!=BackpackObjectEvent::Kind::box_word)continue;   // create_box: create_backpack_box set b.box
        CombatBackpack* owner=nullptr;
        for(auto& o:s.backpacks)if(!o.destroyed&&o.box&&*o.box==s.box_index(e.a))owner=&o;
        if(!owner)fail("00256d90: a restored box without its backpack");
        const auto from=owner->object_events.size();
        owner->object_events.push_back(e);
        s.apply_object_events(*owner,from);
    }
    s.world.actor(s.config.player).clear_runtime_hotboxes();   // 00170570
    s.frame.notes.push_back("002570c0: "+std::to_string(s.backpack_list.data.size())+" backpacks kept, "+
        std::to_string(s.crate_snapshots.size())+" crate records restored");
    return true;
}
bool CombatPlaySession::player_remove_weapon(std::int32_t item){
    auto& s=*impl_;
    if(!s.config.weapon_models)return false;
    const auto record=item<0||static_cast<std::size_t>(item)>=s.db.size()?0:item;   // 00147840
    if(s.db.record(record).model_13c()==0){                                         // 00176098
        s.frame.notes.push_back("RemoveWeapon(Player, "+std::to_string(item)+"): record +13c is 0, nothing");
        return true;
    }
    const auto a=s.config.player;
    s.actors->collect_inventory_view(a,*s.combat);
    std::vector<WeaponInventoryEvent> ev;
    auto ops=s.combat->inventory_ops(a,*s.config.increased_ammo,ev);
    const auto& inv=ops.inventory();
    std::uint8_t slot=ops.slot_of_item(item);                                       // 00143d20
    if(slot>=inv.slots.size())fail("00176060: slot "+std::to_string(slot)+" of item "+std::to_string(item)+" past the slot array");
    if(inv.slots[slot].item!=item){                                                 // 001760c0
        s.frame.notes.push_back("RemoveWeapon(Player, "+std::to_string(item)+"): not in its slot, nothing");
        return true;
    }
    if(slot==0&&inv.slots[1].item==item)slot=1;                                     // 001760c8..001760dc
    ops.remove(slot,true);                                                          // 00142450(+3524, slot, 1, 0, 0)
    s.combat->check_events(ev);
    s.actors->publish_inventory_view(a,*s.combat);
    s.frame.notes.push_back("RemoveWeapon(Player, "+std::to_string(item)+"): slot "+std::to_string(slot));
    return true;
}

const WeaponInventory& CombatPlaySession::player_inventory() const{return impl_->combat->inventory(impl_->config.player);}

bool CombatPlaySession::player_equip_item(std::int32_t item){
    auto& s=*impl_;
    const auto a=s.config.player;
    s.actors->collect_inventory_view(a,*s.combat);
    std::vector<WeaponInventoryEvent> ev;
    const bool ok=s.combat->inventory_ops(a,*s.config.increased_ammo,ev).equip_item(item);   // 001439b0
    s.combat->check_events(ev);
    s.actors->publish_inventory_view(a,*s.combat);
    s.frame.notes.push_back("00175ff0(player, "+std::to_string(item)+")"+(ok?"":": refused"));
    return ok;
}

void CombatPlaySession::player_goggles_toggle(){
    auto& s=*impl_;
    s.toggle_flashlight_of(s.config.player,true);   // 00199860
    s.frame.notes.push_back("00199860(player): +33e7 = "+std::to_string(int(s.world.actor(s.config.player).item_actions().byte_33e7)));
}

std::uint8_t CombatPlaySession::player_goggles_33e7() const{
    return impl_->world.actor(impl_->config.player).item_actions().byte_33e7;
}

std::int32_t CombatPlaySession::player_clips_for(std::int32_t clips_74){
    auto& s=*impl_;
    std::vector<WeaponInventoryEvent> ev;
    const auto n=s.combat->inventory_ops(s.config.player,*s.config.increased_ammo,ev).clips_for_actor(clips_74);   // 001861f0
    // The player's 00332f90(agent data, 5) query (the increased-ammo unlock, read through the host) is the
    // only event it makes.
    for(const auto& e:ev)
        if(e.kind!=WeaponInventoryEvent::Kind::agent_unlock_query)fail("001861f0: the clip count made inventory events");
    return n;
}

void CombatPlaySession::set_hardpoint_vis(std::size_t actor,std::uint8_t slot,bool visible){
    impl_->combat->set_hardpoint_vis(actor,slot,visible);
}

void CombatPlaySession::player_holster_current(){
    auto& s=*impl_;
    const auto a=s.config.player;
    s.actors->collect_inventory_view(a,*s.combat);
    std::vector<WeaponInventoryEvent> ev;
    s.combat->inventory_ops(a,*s.config.increased_ammo,ev).holster_current();   // 00143b60
    s.combat->check_events(ev);
    s.actors->publish_inventory_view(a,*s.combat);
    s.frame.notes.push_back("00143b60(player +3524)");
}

bool CombatPlaySession::steal_player_weapons(std::uint32_t backpack,const std::array<std::uint8_t,3>& slots){
    auto& s=*impl_;
    if(!s.config.backpack_world)return false;
    CombatBackpack* found=nullptr;
    for(auto& b:s.backpacks)if(!b.destroyed&&b.object.word(0xc)==backpack)found=&b;
    if(!found)fail("StealPlayerWeapons 00253dd0: no constructed backpack with +c "+std::to_string(backpack));
    const auto handle=found->handle;
    const auto a=s.config.player;
    s.actors->collect_inventory_view(a,*s.combat);
    std::vector<WeaponInventoryEvent> ev;
    auto inv=s.combat->inventory_ops(a,*s.config.increased_ammo,ev);              // *004ffc0c +30 -> +3524
    s.add_backpack_contents(handle,[&](WeaponBackpackOps& ops){ops.reset(true,inv.inventory());});   // 00258330(bp, 1)
    s.backpack(handle).object.set_byte(0x2f3,1);                                    // 00253e24
    s.backpack(handle).object.set_byte(0x2f0,0);                                    // 00253e30
    for(const auto slot:slots){
        if(slot==8)continue;                                                       // 00253e2c
        if(slot>=inv.inventory().slots.size())fail("00253dd0 reads a player inventory slot outside the 8 slots");
        const std::int32_t item=inv.inventory().slots[slot].item;                  // slot +8
        if(item<0)continue;
        const std::int32_t spare=inv.spare(slot),clip=inv.clip(slot);             // 00143870, 00143940
        s.backpack(handle).weapons.push_back({item,spare,clip});
        s.add_backpack_contents(handle,[&](WeaponBackpackOps& ops){ops.add_weapon(item,spare,clip,0);});   // 00255af0
        inv.remove(slot,false);                                                    // 00142450(inv, slot, 0, 0, 0)
    }
    s.add_backpack_contents(handle,[&](WeaponBackpackOps& ops){ops.check_empty();});   // 00254990
    s.combat->check_events(ev);
    s.actors->publish_inventory_view(a,*s.combat);
    s.frame.notes.push_back("StealPlayerWeapons("+std::to_string(backpack)+")");
    return true;
}

std::vector<CombatPlaySession::PartnerBackpack> CombatPlaySession::partner_backpacks(std::size_t partner){
    auto& s=*impl_;
    std::vector<PartnerBackpack> out;
    if(!s.config.backpack_world)return out;
    const auto& inv=s.combat->inventory(partner);
    for(std::size_t i=0;i<s.backpacks.size();++i){
        auto& b=s.backpacks[i];
        if(b.destroyed||b.address==0||!b.registered||!b.box)continue;
        PartnerBackpack v;
        v.index=i;v.crate=b.crate;v.bounds=b.box_bounds;
        v.position={std::bit_cast<float>(b.transform.word(0x40)),std::bit_cast<float>(b.transform.word(0x44)),
                    std::bit_cast<float>(b.transform.word(0x48)),1.0F};
        const auto host=s.weapon_host(b.address);
        std::vector<WeaponBackpackEvent> scratch;
        WeaponBackpackOps ops(b.object,s.db,host,scratch);
        const auto listed=ops.list(inv);                                       // 002553e0
        for(std::size_t k=0;k<3;++k){
            const std::size_t o=WeaponBackpack::slot(k);
            for(std::int32_t j=0;j<b.object.value(o+0xcc);++j){
                PartnerBackpackEntry e;
                e.item=b.object.value(WeaponBackpack::entry(k,j)+0x20);
                if(const auto a=ops.ammunition(e.item))e.rounds=a->total;     // 00255500
                e.listed=std::find(listed.begin(),listed.end(),e.item)!=listed.end();
                v.view.entries.push_back(e);
            }
        }
        v.view.armor=b.object.word(0x2e4)!=0?b.object.value(0x2e8):0;
        v.view.enabled_2f0=b.object.byte(0x2f0)!=0;
        v.view.stash_2f3=b.object.byte(0x2f3)!=0;
        if(!scratch.empty())fail("partner_backpacks: the backpack reads made events");
        out.push_back(std::move(v));
    }
    return out;
}

PartnerAgentView CombatPlaySession::partner_agent_view(std::size_t actor){
    auto& s=*impl_;
    PartnerAgentView v;
    v.inventory=&s.combat->inventory(actor);
    const auto& h=s.world.actor(actor).health();
    v.armor=h.armor_33a0;v.max_armor=h.max_armor_339c;
    const bool increased=actor==s.config.player&&*s.config.increased_ammo;
    v.capacity=[&s,actor,increased](std::int32_t item){
        std::vector<WeaponInventoryEvent> ev;
        const auto& r=s.db.record(item);
        return r.clip_70()*s.combat->inventory_ops(actor,increased,ev).clips_for_actor(r.clips_74());   // 001861f0
    };
    return v;
}

CombatPickup CombatPlaySession::partner_pickup(std::size_t backpack,std::size_t partner,std::optional<std::int32_t> swap_item){
    return impl_->partner_pickup(backpack,partner,swap_item);
}

std::int32_t CombatPlaySession::player_spare(std::uint8_t slot) const{
    auto& s=*impl_;
    std::vector<WeaponInventoryEvent> ev;
    return s.combat->inventory_ops(s.config.player,*s.config.increased_ammo,ev).spare(slot);   // 00143870
}

std::int32_t CombatPlaySession::player_clip(std::uint8_t slot) const{
    auto& s=*impl_;
    std::vector<WeaponInventoryEvent> ev;
    return s.combat->inventory_ops(s.config.player,*s.config.increased_ammo,ev).clip(slot);    // 00143940
}

// 00143710(inv, item): -1 is the held slot's item (none: false); else the record's clip +70 != 0.
bool CombatPlaySession::player_has_clip(std::int32_t item) const{
    const auto& s=*impl_;
    const auto& inv=s.combat->inventory(s.config.player);
    if(item==-1)item=inv.current_84!=weapon_no_slot?inv.slots.at(inv.current_84).item:-1;
    if(item==-1)return false;
    return s.db.record(item).clip_70()!=0;
}

std::string CombatPlaySession::apply_equipment_loadout(const EquipmentLoadoutSelection& selection){
    auto& s=*impl_;
    const auto a=s.config.player;
    s.actors->collect_inventory_view(a,*s.combat);
    std::vector<WeaponInventoryEvent> ev;
    auto host=s.combat->inventory_host(a,*s.config.increased_ammo);
    if(s.config.weapon_models){
        // The screen's tables hold stand-ins; the player's own table takes the
        // record's model, which the attach's 00145710 (the preload event just
        // before it) has loaded. A record without a model makes no attach.
        host.apply=[self=&s,a,apply=host.apply](const WeaponInventoryEvent& e){
            if(!loadout_stand_in(e)){apply(e);return;}
            auto t=e;
            t.model=self->db.record(e.model-loadout_stand_in_model(0)).model_13c();
            if(t.model!=0)apply(t);};
    }
    omega::apply_equipment_loadout(s.combat->inventory(a),s.db,host,selection,ev);
    // The stand-in models' attach events stay in the loadout's tables; any other attach is refused.
    std::erase_if(ev,[](const WeaponInventoryEvent& e){return loadout_stand_in(e);});
    s.combat->check_events(ev);
    s.actors->publish_inventory_view(a,*s.combat);
    const auto text=describe_inventory(s.combat->inventory(a));
    s.frame.notes.push_back("Loadout accept (002bcb50): "+text);
    return text;
}

std::string CombatPlaySession::add_player_item(std::int32_t item){
    auto& s=*impl_;
    const auto a=s.config.player;
    s.actors->collect_inventory_view(a,*s.combat);
    std::vector<WeaponInventoryEvent> ev;
    s.combat->inventory_ops(a,*s.config.increased_ammo,ev).add(item,true,true);
    s.combat->check_events(ev);
    s.actors->publish_inventory_view(a,*s.combat);
    const auto text=describe_inventory(s.combat->inventory(a));
    s.frame.notes.push_back("Add item "+std::to_string(item)+" (00143df0): "+text);
    return text;
}

NpcCombatLink CombatPlaySession::npc_combat_link(std::size_t actor){
    Impl* self=impl_.get();
    if(actor>=self->world.size()||self->world.actor(actor).player_controller())fail("the combat link is for an AI actor");
    NpcCombatLink link;
    link.object_view=[self](std::uint64_t o)->std::optional<AwarenessObjectView>{
        if(const auto* n=self->node_at(o)){
            // 132pw5: a Node GOBJ (vtable 004d95b0) as the attacker: a script CreateBlast on a node (STREETS
            // OnDamageMsg #800 at node 136626) names it in its damage's +78 hit notice to an actor that survives
            // the blast. The same view as the perception link's node (+108 = 3, not the actor class), so 0016c9d0
            // takes no record.
            const auto s=npc_node_sense_object(static_cast<std::int32_t>(n->id),n->position);
            AwarenessObjectView view;
            view.object=o;view.id=s.id;view.life_108=s.life_108;view.actor_class=s.actor_class;
            view.root=s.root;view.flag_3468=s.flag_3468;view.position=s.position;
            return view;
        }
        const auto v=self->find_index(o);
        if(!v){
            if(self->throws)if(auto t=self->throws->object_view(o))return t;     // the effect objects / fire zones
            // A GOB (e.g. a script's CreateBlast source, 0021a35c: its damage's +78 hit notice names the GOB): the
            // awareness add 0016c9d0 reads only +c, +108, the root bytes, +4c, +3468 and +e8 row 3 (as the perception link's GOB sense object).
            const auto g=self->host_objects?self->host_objects(o):std::nullopt;
            if(!g)return std::nullopt;
            AwarenessObjectView view;
            view.object=o;view.id=g->id;view.life_108=object_life_state(g->health_40);view.actor_class=false;
            view.root=g->root;view.flag_3468=0;view.position=g->position;
            return view;
        }
        const auto& a=self->world.actor(*v);
        AwarenessObjectView view;
        view.object=o;view.id=self->config.object_ids[*v];view.life_108=a.life_state();view.actor_class=true;
        view.root=a.root_bytes();view.flag_3468=a.pickupable_3468();
        view.position=rows_of(root_placement_matrix(self->world.actor(*v).floor_root_placement()))[3];   // +e8 = 00184a80
        return view;
    };
    // The actor's view is collected before the controller reads or changes the
    // inventory and published after a change (the drop happens inside the
    // actor's own step, the death notice between steps).
    link.inventory=[self,actor]{self->actors->collect_inventory_view(actor,*self->combat);return &self->combat->inventory(actor);};
    link.inventory_changed=[self,actor]{self->actors->publish_inventory_view(actor,*self->combat);};
    link.database=&self->db;
    link.killer=[self](std::uint64_t k)->std::optional<NpcKillerView>{
        const auto v=self->find_index(k);
        if(!v)return std::nullopt;
        return NpcKillerView{self->world.actor(*v).player_controller(),self->world.actor(*v).byte_14()!=0};
    };
    link.killer_hostile=[self,actor](std::uint64_t k){return self->enemy(k,self->world.actor(actor).handle());};
    link.statistics=self->config.agent_statistics?&*self->config.agent_statistics:nullptr;
    link.voice_line=[self](std::uint64_t k,std::int32_t line){
        const auto v=self->find_index(k);
        if(!v)throw std::runtime_error("Combat play: the friendly-kill voice line (0018c770) of a killer outside the world");
        self->world.actor(*v).voice_line(line);};
    link.secondary=[self,actor]()->std::optional<std::array<std::uint8_t,2>>{
        if(!self->world.actor(actor).eligible())return std::nullopt;
        const auto& st=self->actors->secondary(actor).states;
        return std::array<std::uint8_t,2>{st.current_310,st.next_311};
    };
    link.random=[self]{return next_game_random(self->world.random());};
    link.eye_c0=[self,actor]{return self->actors->eye_c0(actor,-1);};
    link.word_38=[self,actor]{return self->actors->word_38(actor);};
    link.port_ignored=[self](std::uint64_t o){                                    // port-only (port_cheats.hpp)
        const auto a=self->find_index(o);
        return a&&port_perception_ignores(self->world.actor(*a));};
    // actor +60 +2410: models are attached only from record +13c, which stays 0
    // natively without the weapon model library.
    if(self->config.weapon_models){
        link.attached=[self,actor](std::uint8_t point){return self->combat->attach_table(actor).at(point)!=0;};
        link.apply=[self,actor](const WeaponInventoryEvent& e){self->combat->apply_event(actor,e);};
    }else link.attached=[](std::uint8_t){return false;};
    // 001a3520 reads +3528 only for an NPC holding no weapon and no armor.
    if(self->config.item_list)
        link.items_3528=[self,actor]{return self->config.item_list(self->config.object_ids.at(actor)).any();};   // 004704f0
    else link.items_3528=[]()->bool{fail("an NPC's item list +3528 (004704f0) is not hosted");};
    link.create_backpack=[self,actor]()->std::uint64_t{
        if(self->config.backpack_world)return self->construct_backpack(actor);
        if(!self->config.backpacks_not_simulated)fail("the backpack object 00257640 (vtable 004dc350) is not hosted");
        CombatBackpack b;b.actor=actor;b.handle=0xb0000000ULL+self->backpacks.size();
        // The contents: 00258330(bp, 0) natively (keep 0 reads no inventory);
        // 003cf0e0, the vtable +90 (003cdff0) and +310 are not run.
        WeaponBackpackOps(b.object,self->db,self->backpack_host(),b.events).reset(false,WeaponInventory{});
        b.object.set_byte(0x2f0,1);
        self->frame.backpacks.push_back(self->backpacks.size());
        self->backpacks.push_back(b);
        return b.handle;
    };
    link.add_backpack_weapon=[self](std::uint64_t h,std::int32_t item,std::int32_t spare,std::int32_t clip){
        self->backpack(h).weapons.push_back({item,spare,clip});
        self->add_backpack_contents(h,[&](WeaponBackpackOps& ops){ops.add_weapon(item,spare,clip,0);});};
    link.add_backpack_armor=[self](std::uint64_t h,std::int32_t armor,std::int32_t max_armor){
        self->backpack(h).armor=std::array<std::int32_t,2>{armor,max_armor};
        self->add_backpack_contents(h,[&](WeaponBackpackOps& ops){ops.add_armor(armor,max_armor);});};
    link.backpack_cleared_3464=[self,actor](std::uint64_t h){
        (void)self->backpack(h);
        const auto it=self->owner_cleared_3464.find(actor);
        return it!=self->owner_cleared_3464.end()&&it->second;};
    // N5b: the AI's aim block (virtual +60 = 0022f5b0, the component's skill
    // +38), its transport-0 combat actions, the gate's direct discharge and
    // the fire notice +cc of the discharges the session runs.
    link.aim=[self,actor](std::uint64_t target,std::int32_t item,std::int32_t subtarget,float skill){
        self->combat->aim(actor).skill_38=skill;
        (void)self->aim_update(actor,target,item,subtarget);
    };
    link.aim_block=[self,actor]{const auto& b=self->combat->aim(actor);return std::array<float,2>{b.progress_20,b.accuracy_24};};
    link.actor_action=[self,actor](const NpcAiSend& m){
        Impl::PendingAction q;q.actor=actor;
        if(m.action==0x3b){
            if(m.action_kind!=0xb)fail("an AI fire action of a kind other than 0xb");
            q.kind=Impl::PendingAction::Kind::ai_fire;
            q.fire.target_id=m.word_30;q.fire.subtarget=m.word_34;q.fire.aim_view=m.byte_38!=0;q.fire.point=m.point;q.fire.power=m.power_50;
        }else if(m.action==0x3c){
            if(m.action_kind!=0)fail("an AI reload action of a kind other than 0");
            q.kind=Impl::PendingAction::Kind::reload;
        }
        else{
            q.kind=Impl::PendingAction::Kind::aim;
            q.aim.action=static_cast<std::uint8_t>(m.action);q.aim.kind=m.action_kind;
            q.aim.word_2c=static_cast<std::uint32_t>(m.word_2c);q.aim.point_30=m.point;
            if(m.action_kind==0xb){q.aim.object_30=m.word_30;q.aim.subtarget_34=m.word_34;}   // 0x39 kind 0xb (132ln5h)
        }
        self->pending.push_back(q);
    };
    link.discharge=[self,actor]{
        self->actors->collect_inventory_view(actor,*self->combat);
        const auto from=self->frame.events.size();
        self->combat->discharge(actor,WeaponDischargeInput{},self->frame.events);   // 0013ff40(inv, 0, -1, 0, 0)
        self->apply_shakes(self->frame.events,from);
        self->actors->publish_inventory_view(actor,*self->combat);
        self->queue_reloads(self->frame.events,from);
        std::vector<NpcFireNoticeCall> notices;
        for(std::size_t k=from;k<self->frame.events.size();++k){
            const auto& e=self->frame.events[k];
            if(e.discharge&&e.discharge->kind==WeaponDischargeEvent::Kind::fire_notice)notices.push_back({e.discharge->object,e.discharge->flag});
        }
        return notices;
    };
    link.fire_notice_hook=[self,actor](std::function<void(std::uint64_t,bool)> f){
        if(self->fire_notices.size()<self->world.size())self->fire_notices.resize(self->world.size());
        self->fire_notices[actor]=std::move(f);
    };
    link.eye_c0_of=[self](std::uint64_t o,std::int32_t sub){
        if(!self->find_index(o)&&self->host_objects)if(const auto g=self->host_objects(o))return g->eye;   // 0014d340: +e4
        return self->actors->eye_c0(self->index(o),sub);};
    link.position_f4_of=[self](std::uint64_t o){
        // A GOB's +f4 (003ccff0) is the same code as its +e8 (003cd1c0): +50's matrix, else the identity 0055c9f0.
        if(!self->find_index(o)&&self->host_objects)if(const auto g=self->host_objects(o))return g->position;
        return rows_of(root_placement_matrix(self->world.actor(self->index(o)).floor_root_placement()))[3];};
    link.byte_33e4=[self,actor]{return self->world.actor(actor).actions().byte_33e4;};
    link.secondary_slot1_415=[self,actor]{return self->actors->aim_state(actor).head_active_415!=0;};
    link.weapon_rating_of=[self](std::uint64_t o){
        const auto i=self->index(o);
        self->actors->collect_inventory_view(i,*self->combat);
        return npc_weapon_rating(self->combat->inventory(i),self->db);
    };
    // Checkpoint 132zf12, mode 9 Follow (00153870): an actor's +f4 (00132280, the 'flor' placement), its
    // +30e0 (the navigation block; +3584 is the network object, 0 in solo) and an AI actor's path follower
    // +4c. A GOB target's +f4 matrix and every +58 virtual +74 (0022f510, physical +310) are not hosted.
    link.world_f4_of=[self](std::uint64_t o){
        if(!self->find_index(o))fail("mode 9: the +f4 matrix of an object that is not an actor (003ccff0) is not hosted");
        return rows_of(root_placement_matrix(self->world.actor(self->index(o)).floor_root_placement()));};
    link.speed_of=[self](std::uint64_t o){
        if(!self->find_index(o))fail("mode 9: +30e0 of an object that is not an actor");
        return self->world.actor(self->index(o)).navigation_target().speed_30e0;};
    link.velocity_74_of=[](std::uint64_t)->std::optional<RootVector>{
        fail("mode 9: +58 virtual +74 (0022f510, the physical +310 velocity) of a non-actor target is not hosted");};
    link.hold_4c_of=[self](std::uint64_t o){
        const auto a=self->find_index(o);
        if(!a||!self->perception)fail("mode 9: an AI target's path follower +4c without the perception world");
        const NpcAiController* c=self->perception->controller(*a);
        if(!c)fail("mode 9: an AI target without its controller");
        return c->component().path.hold_4c;};
    // Checkpoint 132uk5, mode 8 CarryBody (00153150): the body's skeleton node *0049d024 (14) world matrix.
    link.node_14_of=[self](std::uint64_t o){
        if(!self->find_index(o))fail("mode 8: 00133ed0 of an object that is not an actor is not hosted");
        return self->world.actor(self->index(o)).skeleton_node_world_matrix(14);};
    return link;
}

namespace {
// The perception's line test over the session's cache (0022cb10 with age 0,
// not forced) and its registration (0022c650 for an actor).
class SessionVisibility final:public NpcVisibilityQuery {
public:
    SessionVisibility(CombatPlaySession& s,std::function<void(std::uint64_t)> reg):session_(s),register_(std::move(reg)){}
    std::uint64_t blocker(std::uint64_t viewer,std::uint64_t target) override{return session_.visibility_blocker(viewer,target,0,false);}
    void register_target(std::uint64_t object) override{register_(object);}
private:
    CombatPlaySession& session_;
    std::function<void(std::uint64_t)> register_;
};
}

void CombatPlaySession::set_host_objects(std::function<std::optional<CombatHostObject>(std::uint64_t)> view){
    impl_->host_objects=std::move(view);
}

NpcPerceptionLink CombatPlaySession::npc_perception_link(CombatPerceptionWorld w){
    Impl* self=impl_.get();
    if(!w.controller||!w.system)fail("the perception link needs the controllers and the AI system");
    self->nodes=w.nodes;
    auto query=std::make_shared<SessionVisibility>(*this,[self](std::uint64_t o){
        if(const auto* n=self->node_at(o)){
            if(self->visibility.slot(o))return;
            if(self->visibility.register_object(o)){
                self->registered.push_back({o,static_cast<std::int32_t>(n->id)});
                self->frame.notes.push_back("0022c650 registered node "+std::to_string(n->id)+" (a script's SetTarget)");
            }else self->frame.notes.push_back("0022c650: the 30 slots are full, node "+std::to_string(n->id)+" not registered");
            return;
        }
        const auto a=self->find_index(o);
        if(!a){
            // 0022c650 only stores the object and its +c (any class): a GOB of a script's SetTarget.
            const auto g=self->host_objects?self->host_objects(o):std::nullopt;
            if(!g)fail("0022c650 for an object that is neither an actor nor a hosted GOB");
            if(self->visibility.slot(o))return;
            if(self->visibility.register_object(o)){
                self->registered.push_back({o,g->id});
                self->frame.notes.push_back("0022c650 registered GOB "+std::to_string(g->id)+" (a script's SetTarget)");
            }else self->frame.notes.push_back("0022c650: the 30 slots are full, GOB "+std::to_string(g->id)+" not registered");
            return;
        }
        if(!self->visibility.slot(o))self->register_actor(*a);});
    auto world=std::make_shared<CombatPerceptionWorld>(std::move(w));
    self->perception=world;
    NpcPerceptionLink link;
    link.object_by_id=[self](std::int32_t id){return self->find(id);};          // 0040c540
    link.sense=[self,query,world](SoloActorSimulation& observer)->NpcPerceptionHost{
        NpcPerceptionHost h;
        const auto o=self->index(observer.handle());
        h.self=observer.handle();h.self_root=observer.root_bytes();
        h.self_eye=self->actors->eye_c0(o,-1);
        const bool present=observer.eligible();
        if(present)h.self_head=observer.skeleton_node_world_matrix(14);             // 00133ed0, node *0049d024
        h.self_forward=rows_of(root_placement_matrix(observer.floor_root_placement()))[1];   // +e8 = 00184a80, row 1
        h.blind_33a8=self->blind_33a8(o);h.flag_3470=observer.item_actions().vision_3470;   // 0016bd60 / 0016be14 / 0016c220 / 0016d8e0: == 1
        // Port-only (speed, 132vs4): the world by pointer (the session keeps it, self->perception), so the
        // view fits std::function's small buffer on the Vita (no allocation per host).
        h.object=[self,world=world.get()](std::uint64_t object)->std::optional<NpcSenseObject>{
            if(const auto* n=self->node_at(object))return npc_node_sense_object(static_cast<std::int32_t>(n->id),n->position);
            const auto a=self->find_index(object);
            if(!a){
                // A GOB (a script's RequestFacial): only +c, +108, the root bytes, +4c, +e8 row 3 and +c0
                // matter to look entry 2 (0014dea0, 0014e5e8.., 0014c710..); the actor fields stay empty.
                const auto g=self->host_objects?self->host_objects(object):std::nullopt;
                if(!g)return std::nullopt;
                NpcSenseObject v;
                v.id=g->id;v.life_108=object_life_state(g->health_40);v.root=g->root;
                v.actor_class=false;v.class_4ea770=false;v.position=g->position;v.eye=g->eye;
                v.controller=NpcSenseObject::Controller::none;v.team=-1;
                return v;
            }
            auto& s=self->world.actor(*a);
            NpcSenseObject v;
            v.id=self->config.object_ids[*a];v.life_108=s.life_state();v.root=s.root_bytes();v.actor_class=true;
            v.flag_3468=s.pickupable_3468();v.byte_14=s.byte_14();v.flags_3034=s.control_history()[1];v.flags_3204=s.state_words().current_3204;
            if(!world->stealth_44)fail("the stealth +44 (00133ab0, model light gather 00373320) is not hosted");
            v.stealth_44=world->stealth_44(*a);
            v.position=rows_of(root_placement_matrix(s.floor_root_placement()))[3];     // +e8 row 3
            v.eye=self->actors->eye_c0(*a,-1);
            v.weapon_ready_d4=self->ready_d4(*a);
            const bool floor=s.eligible()&&s.physical().response.physical.e8!=0;         // 0014d3d0
            v.kind_5b0=floor;v.kind_580=floor;
            if(!self->config.team)fail("teams are not supplied");
            v.team=self->config.team(*a);                                               // 003cc950: controller +68
            if(s.player_controller()){
                v.controller=NpcSenseObject::Controller::player;
                const auto& aim=self->combat->aim(*a);                                  // 003ea550
                if(aim.target_28){
                    const auto t=self->find_index(aim.target_28);
                    if(t&&self->config.object_ids[*t]==aim.target_id_2c)v.aim_target=aim.target_28;
                }
            }else{
                v.controller=NpcSenseObject::Controller::ai;
                const NpcAiController* c=world->controller(*a);
                if(!c)fail("an AI actor without its controller");
                v.contact_158=c->component().contact_158;v.region_5c=c->component().path.destination_5c;
                const auto& p=c->perception();
                if(p.target_40){
                    const auto t=self->find_index(p.target_40);
                    if(t&&self->config.object_ids[*t]==p.target_id_44)v.ai_target=p.target_40;
                }
            }
            const auto* fl=self->flashlight_of(*a);                                      // 00199700
            v.lit_spot=flashlight_lit_spot(fl);
            if(v.lit_spot)v.lit_spot_position=fl->spot_position;                         // the spot's +e4 (003cd260)
            return v;
        };
        h.teams=self->config.relations;
        // Port-only (port_cheats.hpp): a marked observer sees no NPC unless it is engaged (132ob6), and no
        // observer sees a marked NPC.
        const bool observer_ignored=port_perception_observer_blind(observer);
        h.think_list.reserve(world->system->think_list.size());                      // port-only (speed, 132vs4)
        for(const auto& k:world->system->think_list){                                  // 004ea190 as {actor, ID}
            bool found=false;
            for(std::size_t a=0;a<self->world.size()&&!found;++a){
                const NpcAiController* c=self->world.actor(a).player_controller()?nullptr:world->controller(a);
                if(c&&&c->component()==k.component){
                    found=true;
                    if(observer_ignored||port_perception_ignores(self->world.actor(a)))continue;
                    h.think_list.push_back({self->world.actor(a).handle(),self->config.object_ids[a]});
                }
            }
            if(!found)fail("a think-list entry whose actor the session does not know");
        }
        h.local_player=self->world.actor(self->config.player).handle();
        h.port_ignored=[self](std::uint64_t o){                                   // port-only (port_cheats.hpp)
            const auto a=self->find_index(o);
            return a&&port_perception_ignores(self->world.actor(*a));};
        h.player_slots={h.local_player,0,0,0};                                         // 003fb8d0: slot 0 only in single player
        h.visibility=query.get();
        h.player_region=world->player_region;h.region_at=world->region_at;
        return h;
    };
    return link;
}

std::uint64_t CombatPlaySession::visibility_blocker(std::uint64_t a,std::uint64_t b,std::int32_t max_age,bool force){
    return impl_->blocker(a,b,max_age,force);
}
// 00280070's view of a locked actor: +c the object id, +50 the model
// component (00392768 -> 003a9f20; every actor of the world has one), the root
// bytes and +c0 = 00188d80. An object that is not an actor of the world throws.
PlayerCameraLookAtHost CombatPlaySession::camera_look_at_host(){
    Impl* self=impl_.get();
    PlayerCameraLookAtHost h;
    h.id_c=[self](std::uint64_t o){return self->object_id(o);};
    h.transform_50=[self](std::uint64_t o){self->index(o);return true;};
    h.root_bytes=[self](std::uint64_t o){return self->root(o);};
    h.eye_c0=[self](std::uint64_t o,std::int32_t sub){return self->actors->eye_c0(self->index(o),sub);};
    return h;
}
ActorWorldCombat& CombatPlaySession::actors(){return *impl_->actors;}
std::span<const CombatEvent> CombatPlaySession::frame_events() const{
    if(!impl_->in_frame)fail("frame_events outside begin_frame / end_frame");
    return impl_->frame.events;
}
std::optional<RootVector> CombatPlaySession::object_point_c0(std::int32_t id,std::int32_t sub){
    const auto o=impl_->find(id);
    if(o==0)return std::nullopt;
    return impl_->aim_host(impl_->config.player).eye_c0(o,sub);
}
CombatWorld& CombatPlaySession::combat(){return *impl_->combat;}
const HudTargets& CombatPlaySession::hud() const{return impl_->hud;}
bool CombatPlaySession::hud_found(std::int32_t id) const{return impl_->find(id)!=0;}
std::optional<HudBlipObject> CombatPlaySession::hud_blip_object(std::int32_t id){
    auto& s=*impl_;
    const auto o=s.find(id);
    if(!o)return std::nullopt;
    if(const auto* n=s.node_at(o)){
        // Checkpoint 132ms5 (BELARUS2's objective markers on nodes): the node vtable 004d95b0's +e4 is 0015bef0
        // (+80, w 1) and its +f4 0015bf40 jumps to 0015c170, the rotation about z by +8c with row 3 = +80.
        const RootVector p{n->position[0],n->position[1],n->position[2],1.0F};
        const auto r=actor_object_node_rows(n->yaw_8c);
        HudBlipObject b;
        b.position_e4=p;
        b.f4={r[0],r[1],r[2],p};
        b.f4_row1=r[1];
        return b;
    }
    auto& actor=s.world.actor(s.index(o));
    HudBlipObject b;
    b.position_e4=actor.position_e4();
    b.f4=rows_of(root_placement_matrix(actor.floor_root_placement()));
    b.f4_row1=b.f4[1];
    return b;
}
std::optional<HudCalloutObject> CombatPlaySession::hud_callout_object(std::int32_t id){
    auto& s=*impl_;
    const auto o=s.find(id);
    if(!o)return std::nullopt;
    HudCalloutObject c;
    c.root=s.root(o);
    // 00242330 reads the object's virtual +e4 (00242538) only after its root
    // test (+2d, +2c, not +2e) passed: an actor that is not present (never
    // activated, so never stepped) is hidden without its +e4 being read.
    const bool shown=c.root[1]!=0&&c.root[0]!=0&&c.root[2]==0;
    if(shown&&!s.node_at(o))c.position_e4=s.world.actor(s.index(o)).position_e4();
    // Checkpoint 132hud4: an hscallout_bottom record reads the actor's virtual +108 (001872f0) after that test.
    if(shown&&!s.node_at(o))c.life_108=s.world.actor(s.index(o)).life_state();
    return c;
}
const PadWeaponState& CombatPlaySession::pad() const{return impl_->pad;}
const PlayerTargetingState& CombatPlaySession::targeting() const{return impl_->state;}
const TargetVisibility& CombatPlaySession::visibility() const{return impl_->visibility;}
const std::vector<TargetHandle>& CombatPlaySession::registered() const{return impl_->registered;}

bool CombatPlaySession::register_host_object(std::uint64_t o){
    auto& s=*impl_;
    if(s.find_index(o)||s.node_at(o))fail("register_host_object for an actor or a node (their 0022c650 is the session's)");
    const auto g=s.host_objects?s.host_objects(o):std::nullopt;
    if(!g)fail("0022c650 for an object that is neither an actor nor a hosted object");
    if(s.visibility.slot(o))return false;
    if(!s.visibility.register_object(o)){
        s.frame.notes.push_back("0022c650: the 30 slots are full, object "+std::to_string(g->id)+" not registered");
        return false;
    }
    s.registered.push_back({o,g->id});
    s.frame.notes.push_back("0022c650 registered object "+std::to_string(g->id));
    return true;
}

std::vector<CombatPlaySession::TraversalActor> CombatPlaySession::traversal_actors() const{
    const auto& s=*impl_;
    std::vector<TraversalActor> r;
    for(std::size_t a=0;a<s.world.size();++a){
        if(!s.world.traversed(a))continue;
        const auto& actor=s.world.actor(a);
        r.push_back({actor.handle(),actor.root_bytes(),true,actor.player_controller()});
    }
    return r;
}

std::uint64_t CombatPlaySession::object_by_key(std::int32_t id) const{return impl_->find(id);}

std::int32_t CombatPlaySession::object_key(std::uint64_t o) const{return impl_->object_id(o);}

bool CombatPlaySession::object_resolves(std::uint64_t o,std::int32_t id) const{
    const auto& s=*impl_;
    if(o==0)return false;
    if(const auto a=s.find_index(o))return s.actors->alive_handle(*a)&&s.config.object_ids.at(*a)==id;
    return s.object_id(o)==id;
}

RootVector CombatPlaySession::object_eye_c0(std::uint64_t o) const{
    const auto& s=*impl_;
    if(const auto a=s.find_index(o))return s.actors->eye_c0(*a,-1);
    if(const auto g=s.gob(o))return g->eye;
    fail("the virtual +c0 of an object that is neither an actor nor a hosted object");
}

void CombatPlaySession::fire_object_bullet(const ShotRequest& r,const BulletObjectShooter& shooter){
    auto& s=*impl_;
    if(!s.in_frame)fail("fire_object_bullet outside begin_frame / end_frame");
    s.actors->collect_inventory_views(*s.combat);
    s.combat->request_bullet(r,shooter,s.frame.events);
    s.actors->publish_inventory_views(*s.combat);
}
float CombatPlaySession::shake_352c(std::size_t actor) const{return impl_->shake.at(actor);}
std::int32_t CombatPlaySession::frame_counter() const{return impl_->fp;}
const std::vector<CombatBackpack>& CombatPlaySession::backpacks() const{return impl_->backpacks;}
std::vector<std::size_t> CombatPlaySession::construct_crates(const std::vector<BackpackInvRecord>& records,
    const std::function<std::uint32_t(std::uint32_t,std::uint32_t)>& find_collision){
    return impl_->construct_crates(records,find_collision);
}
bool CombatPlaySession::set_crate_present(std::uint32_t key,bool present){return impl_->set_crate_present(key,present);}
const std::vector<BackpackCrateSnapshot>& CombatPlaySession::snapshot_crates(){
    if(impl_->backpack_list.data.empty()){impl_->crate_snapshots.clear();return impl_->crate_snapshots;}   // 002569c0, no entries
    if(!impl_->config.backpack_world)throw std::runtime_error("00256b40: the crate snapshot needs CombatPlayConfig::backpack_world");
    backpack_crate_snapshot(impl_->backpack_list,impl_->object_host(),impl_->crate_snapshots);
    return impl_->crate_snapshots;
}
std::optional<WeaponBackpackOwner> CombatPlaySession::backpack_owner(std::size_t i) const{
    const auto owner=impl_->backpacks.at(i).object.word(0x300);
    if(owner==0)return std::nullopt;
    return impl_->owner_view(impl_->actor_at(owner));
}
const std::optional<AgentStatistics>& CombatPlaySession::agent_statistics() const{return impl_->config.agent_statistics;}
bool CombatPlaySession::hosts_backpacks() const{return impl_->config.backpack_world.has_value();}
bool CombatPlaySession::backpack_rider_live(std::uint32_t address,std::uint32_t id){
    auto& b=impl_->backpack_at(address);
    if(b.destroyed&&!b.deleted&&!impl_->config.backpack_world->release_3cb230)
        fail("a lift rider handle to a destroyed backpack: its deferred release 003cb230 (the destructor's +c) is not hosted");
    return !b.deleted&&b.object.word(0xc)==id;
}
void CombatPlaySession::carry_backpack(std::uint32_t address,bool message,const std::array<float,16>& matrix){
    auto& b=impl_->backpack_at(address);
    if(b.deleted)fail("00218200 carries a deleted backpack");
    if(message){
        // 003c9420(msg, bp, 1) -> 00254600: not the pickup trigger *004f5440 -> 003cea10 (mode 3).
        (void)backpack_object_receive_transform(b.object,b.transform,3,matrix);
        return;
    }
    const auto from=b.object_events.size();
    backpack_object_move_a8(b.object,b.transform,{matrix[12],matrix[13],matrix[14],matrix[15]},impl_->object_host(),b.object_events);
    impl_->apply_object_events(b,from);
}
bool CombatPlaySession::reset_level_statistics(){
    if(!impl_->config.agent_statistics)return false;
    reset_agent_level_statistics(*impl_->config.agent_statistics);
    return true;
}
const std::optional<std::string>& CombatPlaySession::player_name() const{return impl_->config.player_name;}
const CombatThrows* CombatPlaySession::throws() const{return impl_->throws.get();}
CombatThrows* CombatPlaySession::throws(){return impl_->throws.get();}
const Flashlight* CombatPlaySession::flashlight(std::size_t actor) const{return impl_->flashlight_of(actor);}
bool CombatPlaySession::flashlight_on(std::size_t actor) const{return flashlight_is_on(impl_->flashlight_of(actor));}
void CombatPlaySession::set_flashlight_tuning(const FlashlightTuning& tuning){impl_->flash_tuning=tuning;}
const PickupMenu* CombatPlaySession::pickup_menu() const{return impl_->pickup_menu?&impl_->pickup_menu->menu:nullptr;}
bool CombatPlaySession::pickup_menu_update(){
    bool any=false;
    impl_->menu_call("002526f0",[this,&any](auto& h){any=::omega::pickup_menu_update(impl_->pickup_menu->menu,h);});
    return any;
}
void CombatPlaySession::pickup_menu_close(){impl_->close_pickup_menu();}
const PickupMenuPreview* CombatPlaySession::pickup_menu_preview(std::uint32_t token) const{
    const auto& p=impl_->pickup_state.previews;
    const auto it=p.find(token);
    return it==p.end()?nullptr:&it->second;
}

}
