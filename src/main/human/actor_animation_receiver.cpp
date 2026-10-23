#include "float_bits.hpp"
#include "actor_animation_receiver.hpp"
#include "vu_float.hpp"
#include <bit>
#include <cmath>
#include <stdexcept>

namespace omega {
namespace {
void action(ActorAnimationReceiverState& state,unsigned index,bool enabled) {
    const auto bit=std::uint32_t{1}<<index;
    const bool prior=(state.changes_3218&bit)!=0;
    if(enabled)state.active_actions|=bit;else state.active_actions&=~bit;
    state.highest_action=state.active_actions?31-static_cast<std::int32_t>(std::countl_zero(state.active_actions)):-1;
    // Original reads +3218 before setting +3210 and ORs this same +3218 word
    // when its saved bit differs. It does not replace this word with the mask.
    if(prior!=enabled)state.changes_3218|=bit;
}
void unsupported(){throw std::runtime_error("Actor animation receiver branch requires an unimplemented gameplay dependency");}
void physical_mode(ActorAnimationReceiverState& state,unsigned index,bool enabled) {
    if(!state.physical)return;
    auto& physical=*state.physical;
    const auto bit=std::uint32_t{1}<<index;
    if(enabled)physical.modes_2c|=bit;else physical.modes_2c&=~bit;
    const auto scanned=physical.modes_2c&0x1fffU;
    physical.highest_mode_28=scanned?31-static_cast<std::int32_t>(std::countl_zero(scanned)):-1;
}
void sound(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& config,
    std::int32_t id,const MotionDeliveryView& view,const ActorAnimationReceiverHost& host) {
    const auto key=config.sound_keys.find(id);const auto sink=host.sound_immediate;
    if(key==config.sound_keys.end()||!config.actor_generation||!sink)
        throw std::runtime_error("Actor animation sound identity or delivery host is unresolved");
    const auto kind=id==0x57||id==0x59?ActorAnimationSoundRequest::Kind::stop:ActorAnimationSoundRequest::Kind::start;
    const ActorAnimationSoundRequest request{kind,id==0xb||id==0x73?1:0,
        key->second,config.receiver,*config.actor_generation,1};
    sink(request,s,view);
}
void queue_sound(const ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& config,
    const ActorAnimationSoundPreset& preset,const MotionDeliveryView& view,const ActorAnimationReceiverHost& host) {
    if(!config.actor_generation||!host.enqueue_sound)
        throw std::runtime_error("Actor queued sound identity or delivery host is unresolved");
    host.enqueue_sound({ActorAnimationSoundRequest::Kind::start,preset.bank,preset.key,
        config.receiver,*config.actor_generation,1,preset.word_180,preset.byte_186},s,view);
}
ActorAnimationInventoryState& inventory(ActorAnimationReceiverState& s) {
    if(!s.inventory||!s.inventory->owner||s.inventory->mode>6||s.inventory->saved_mode>6)
        throw std::runtime_error("Actor inventory state is unresolved");
    return *s.inventory;
}
bool inventory_idle(const ActorAnimationReceiverState& s) {
    const auto& item=*s.inventory;
    if(item.byte_ef)return false;
    if(!s.attachment_byte_a0)throw std::runtime_error("Actor inventory activity byte is unresolved");
    return !*s.attachment_byte_a0&&!item.pending_bc&&!item.pending_c0&&!item.owner_control_active;
}
void require_inventory_slots(const ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host) {
    if(!cfg.inventory||!host.model_slot_count||!host.swap_model_slots)
        throw std::runtime_error("Actor inventory model slot hosting is unresolved");
    const auto count=host.model_slot_count(s.inventory->owner);
    for(auto slot:cfg.inventory->slots_70)if(slot>=count)throw std::runtime_error("Inventory slot mapping outside model");
    for(auto slot:cfg.inventory->slots_78)if(slot>=count)throw std::runtime_error("Inventory slot mapping outside model");
}
void require_inventory_selection_host(const ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host) {
    require_inventory_slots(s,cfg,host);
    if(!host.model_slot_present||!host.inventory_owner||!host.enqueue_inventory_selection_sound)
        throw std::runtime_error("Actor inventory event hosting is unresolved");
}
void require_inventory_host(const ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host) {
    require_inventory_selection_host(s,cfg,host);
    if(!host.enqueue_inventory_empty)throw std::runtime_error("Actor empty inventory event hosting is unresolved");
}
void hide_inventory_slot(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host,std::uint8_t mode) {
    auto& item=*s.inventory;
    if(mode>5)throw std::runtime_error("Inventory hide mode outside table");
    if(mode==0&&item.entries[1].item!=-1)hide_inventory_slot(s,cfg,host,1);
    host.swap_model_slots(item.owner,cfg.inventory->slots_78[mode],cfg.inventory->slots_70[mode],s);
    item.mode=6;item.saved_mode=mode;item.selection_a4=-1;
}
void hide_inventory(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host) {
    if(!inventory_idle(s))return;
    if(s.inventory->mode==6)s.inventory->saved_mode=6;
    else hide_inventory_slot(s,cfg,host,s.inventory->mode);
}
void select_inventory(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host,std::uint8_t mode) {
    auto& item=*s.inventory;
    if(mode>5)throw std::runtime_error("Inventory selection outside table");
    if(item.mode==mode)return;
    hide_inventory(s,cfg,host);
    if(mode==0&&item.entries[1].item!=-1)select_inventory(s,cfg,host,1);
    const auto owner=host.inventory_owner(item.owner);
    if(owner.controller&&owner.player&&owner.local_active&&mode!=1)
        host.enqueue_inventory_selection_sound(item.owner,s);
    item.mode=mode;
    if(host.model_slot_present(item.owner,cfg.inventory->slots_70[mode]))
        host.swap_model_slots(item.owner,cfg.inventory->slots_70[mode],cfg.inventory->slots_78[mode],s);
    item.selection_a4=-1;
}
void cycle_inventory_forward(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host) {
    if(!inventory_idle(s))return;
    auto& item=*s.inventory;
    if(item.mode==6) {
        for(std::uint8_t next=0;next<6;++next)if(item.entries[next].item!=-1){select_inventory(s,cfg,host,next);break;}
        return;
    }
    auto next=static_cast<std::uint8_t>(item.mode+1);
    while(next!=6&&(next==1||item.entries[next].item==-1))++next;
    if(next==6)hide_inventory_slot(s,cfg,host,item.mode);
    else select_inventory(s,cfg,host,next);
}
void release_inventory(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host) {
    auto& item=inventory(s);const auto mode=item.mode;
    if(mode==4||mode==5) {
        if(item.entries[mode].count<1) {
            // Preflight all reachable host operations before the first queue
            // mutation. External model/event state belongs to the owner host.
            require_inventory_host(s,cfg,host);
            if(!s.attachment_byte_a0)throw std::runtime_error("Actor inventory activity byte is unresolved");
        } else if(!cfg.inventory||cfg.inventory->definitions.empty()||!host.set_model_slot)
            throw std::runtime_error("Actor inventory definition/model hosting is unresolved");
    }
    item.byte_ef=0;
    if(mode!=4&&mode!=5)return;
    auto& entry=item.entries[mode];
    if(entry.count<1) {
        const auto owner=host.inventory_owner(item.owner);
        if(owner.controller&&owner.player)host.enqueue_inventory_empty(owner.controller,entry.item,s);
        entry.item=-1;
        cycle_inventory_forward(s,cfg,host);
    } else {
        auto index=entry.item;
        if(index<0||static_cast<std::size_t>(index)>=cfg.inventory->definitions.size())index=0;
        const auto& part=cfg.inventory->definitions[static_cast<std::size_t>(index)];
        if(!part||part->type<8)host.set_model_slot(item.owner,0,part,s);
    }
}
void prepare_inventory_hide(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host) {
    auto& item=inventory(s);
    if(inventory_idle(s)&&item.mode!=6)require_inventory_slots(s,cfg,host);
}
void prepare_inventory_restore(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host,bool callback_may_change=false) {
    inventory(s);
    if(s.inventory->saved_mode!=6||callback_may_change) {
        require_inventory_selection_host(s,cfg,host);
        if(!s.attachment_byte_a0)throw std::runtime_error("Actor inventory activity byte is unresolved");
    }
}
void restore_inventory(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const ActorAnimationReceiverHost& host) {
    const auto saved=inventory(s).saved_mode;
    if(saved!=6) {
        require_inventory_selection_host(s,cfg,host);
        select_inventory(s,cfg,host,saved);
    }
}
void token(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& config,
    std::int32_t id,const MotionDeliveryView& view,const ActorAnimationReceiverHost& host) {
    const bool detaches=id==0x75||id==0x76||id==0x7d||id==0x7f||id==0x80;
    if(detaches&&(config.detach_roots.find(id)==config.detach_roots.end()||!host.clear_root_children))
        throw std::runtime_error("Actor root-child clearing key or host is unresolved");
    const auto clear_children=[&]{host.clear_root_children(config.detach_roots.at(id),s,view);};
    switch(id) {
    case 0:case 0xd:case 0x17:case 0x1b:case 0x1f:case 0x23:case 0x47:
    case 0x4e:case 0x51:case 0x65:case 0x69:case 0x6f:case 0xa2:return;
    case 1:case 2:
        action(s,2,true);
        if((s.flags_3204&6)==0)s.flags_3248|=4;else s.flags_3248&=~4U;
        s.flags_3248&=~8U;if(s.physical)s.physical->flags_38|=2;return;
    case 3: {
        if(!s.physical||!s.physical->byte_dd)return;
        const auto finite=[](float value){return !::omega::float_bits::nonfinite_or_subnormal(value);};
        if(!config.contact_height_sentinel||!finite(*config.contact_height_sentinel)||
           !finite(s.physical->value_e4)||!config.contact_sound_keys||
           !config.actor_generation||!host.enqueue_sound)
            throw std::runtime_error("Actor contact sound configuration is unresolved");
        const bool height=s.physical->value_e4!=*config.contact_height_sentinel;
        if(height) {
            if(!s.physical->model||!config.contact_root||!host.physical_root_position)
                throw std::runtime_error("Actor contact model-root query is unresolved");
            //001e96a0 refreshes/reads this root before returning a height the
            // caller discards. Preserve the query, without inventing motion.
            const auto position=host.physical_root_position(s.physical->model,*config.contact_root,s,view);
            if(!finite(position[2]))throw std::runtime_error("Actor contact root height is nonfinite");
        }
        const auto key=height?2U:((s.flags_3248&4U)?1U:0U);
        queue_sound(s,config,{height?0:1,(*config.contact_sound_keys)[key],0,0},view,host);
        return;
    }
    case 4:s.flags_3248|=8;return;
    case 5:if(s.physical)s.physical->flags_38|=4;return;
    case 6:case 7:
        action(s,2,false);s.flags_3248&=~12U;if(s.physical)s.physical->flags_38&=~6U;return;
    case 9:case 10:action(s,4,true);return;
    case 0xb:case 0x53:case 0x56:case 0x58:case 0x5c:case 0x73:sound(s,config,id,view,host);return;
    case 0xc:s.flags_3248|=0x4000;return;
    case 0xe:action(s,4,false);return;
    case 0xf:
        s.flags_3248&=~0x4000U;if(s.physical)s.physical->byte_9c0=0;action(s,4,false);return;
    case 0x10:action(s,5,true);return;
    case 0x11:if(s.physical)s.physical->byte_9c0=0;return;
    case 0x12:case 0x13:
        if(s.physical) {
            if(!config.physical_restore_374||::omega::float_bits::nonfinite_or_subnormal(*config.physical_restore_374))
                throw std::runtime_error("Actor receiver physical tuning is unresolved");
            if(id==0x13)s.physical->byte_9c0=0;
        }
        action(s,5,false);
        if(s.physical)s.physical->value_374=*config.physical_restore_374;
        return;
    case 0x15:
        if(!config.roots)throw std::runtime_error("Actor root bindings are unresolved");
        view.copy_anchor_world_orientation(config.roots->flor,config.roots->edge);return;
    case 0x14: {
        const auto sink=host.enqueue_controller;
        if(!config.controller_receiver||!sink)
            throw std::runtime_error("Actor animation controller queue is unresolved");
        action(s,7,true);action(s,6,true);physical_mode(s,5,true);physical_mode(s,1,true);
        sink({config.controller_receiver,6,-1,0,5},s,view);return;
    }
    case 0x16:case 0x18:
        action(s,7,false);action(s,6,false);
        physical_mode(s,5,false);physical_mode(s,1,false);return;
    case 0x19:
        action(s,8,true);action(s,6,true);
        physical_mode(s,5,true);physical_mode(s,1,true);return;
    case 0x1a:case 0x1c:
        action(s,8,false);action(s,6,false);
        physical_mode(s,5,false);physical_mode(s,1,false);return;
    case 0x1d:action(s,9,true);return;
    case 0x1e:case 0x20:action(s,9,false);return;
    case 0x21:action(s,10,true);return;
    case 0x22:case 0x24:action(s,10,false);return;
    case 0x25:action(s,11,true);if(s.physical)s.physical->byte_9c0=0;return;
    case 0x26:action(s,11,false);s.flags_3248|=0x8000;return;
    case 0x28:action(s,11,false);s.flags_3248&=~0x8000U;return;
    case 0x27:s.flags_3248&=~0x8000U;return;
    case 0x29:case 0x2e:case 0x33:case 0x38:
        if((id==0x29||id==0x2e)&&s.byte_14&&config.controller_receiver&&!host.controller_is_player)
            throw std::runtime_error("Actor climb entry player-role query is unresolved");
        action(s,1,true);
        if(id==0x33)s.flags_3248|=0x40U;
        else if(id==0x38)s.flags_3248|=0x80U;
        else {
            if(id==0x2e)s.flags_3248|=0x20U;
            if(s.byte_14&&config.controller_receiver&&host.controller_is_player(config.controller_receiver)&&s.physical)
                s.physical->byte_3d3=1;
        }
        return;
    case 0x2b:case 0x2d:case 0x30:case 0x32:case 0x35:case 0x37:
    case 0x3a:case 0x3c:case 0x3f:case 0x41:case 0x43:case 0x45: {
        const bool restores=id==0x3f||id==0x41||id==0x43||id==0x45;
        const bool completes=id==0x2d||id==0x32||id==0x37||id==0x3c||id==0x41||id==0x45;
        if(s.controller_3474&&!host.enqueue_controller)
            throw std::runtime_error("Actor climb controller queue is unresolved");
        if(s.byte_326b&&config.controller_receiver&&!host.controller_is_player)
            throw std::runtime_error("Actor climb player-role query is unresolved");
        const bool may_pop=s.byte_326b&&config.controller_receiver&&s.byte_14;
        if(may_pop&&!host.pop_world_object)throw std::runtime_error("Actor climb world pop is unresolved");
        if(restores)prepare_inventory_restore(s,config,host,may_pop&&!config.world_pop_keeps_inventory);
        action(s,restores?19:1,false);s.flags_3248|=0x10U;
        if(id==0x3a||id==0x3c)s.flags_3248&=~0x80U;
        else if(id==0x30||id==0x32)s.flags_3248&=~0x20U;
        else if(id==0x35||id==0x37)s.flags_3248&=~0x40U;
        if(id==0x2b||id==0x2d||id==0x30||id==0x32)physical_mode(s,5,false);
        else if(id==0x3a||id==0x3c||id==0x3f||id==0x41) {
            physical_mode(s,2,false);physical_mode(s,11,false);
        } else {
            physical_mode(s,5,false);physical_mode(s,2,false);physical_mode(s,11,false);
        }
        if(s.controller_3474) {
            host.enqueue_controller({s.controller_3474,completes?13:15,-1,config.receiver,1},s,view);
            s.controller_3474=0;
        }
        if(s.byte_326b&&config.controller_receiver&&host.controller_is_player(config.controller_receiver)&&s.byte_14) {
            host.pop_world_object(s,view);s.byte_326b=0;
        }
        if(restores)restore_inventory(s,config,host);
        if(completes){s.flags_3248&=~0x10U;physical_mode(s,3,false);}
        return;
    }
    case 0x2c:case 0x31:case 0x36:case 0x3b:case 0x40:case 0x44:
        s.flags_3248&=~0x10U;physical_mode(s,3,false);return;
    case 0x2a:case 0x2f:case 0x34:case 0x39:case 0x3e:case 0x66:case 0x6d: {
        if(!config.roots)throw std::runtime_error("Actor orientation roots are unresolved");
        if(id!=0x66&&id!=0x6d)physical_mode(s,3,false);
        RootVector offset{0,0,0,1};
        if(id==0x34||id==0x6d) {
            const float half=std::bit_cast<float>(std::uint32_t{0x3fc90fdb});
            offset={0,0,std::sin(half),std::cos(half)}; // Portable host trig, not PS2 bit equivalence.
        }
        view.copy_anchor_world_orientation(config.roots->flor,config.roots->edge,offset);return;
    }
    case 0x42: {
        if(!config.roots||!config.climb_finish_motion||!host.motion_end_placement)
            throw std::runtime_error("Actor climb completion motion resource is unresolved");
        physical_mode(s,3,false);
        const auto placement=host.motion_end_placement(*config.climb_finish_motion);
        view.copy_anchor_world_heading(config.roots->flor,config.roots->edge,
            placement?placement->orientation:RootVector{0,0,0,1});return;
    }
    case 0x46:
        if(!config.controller_receiver||!host.controller_immediate||!view.can_set_channel_target(21))
            throw std::runtime_error("Actor ladder entry controller/target hosting is unresolved");
        prepare_inventory_hide(s,config,host);
        action(s,16,true);action(s,12,true);physical_mode(s,9,true);
        host.controller_immediate({config.controller_receiver,10,-1,0,5},s,view);
        // The immediate controller may replace inventory; reread it before
        // hide and retain callback flag writes until the following masks.
        prepare_inventory_hide(s,config,host);hide_inventory(s,config,host);
        s.flags_3248&=~0x6000000U;
        view.set_channel_target(21,1,.5F,0,false);return;
    case 0x48:case 0x4a:
        if(s.active_actions&0x10000U) {
            const auto sink=host.controller_immediate;
            if(!config.controller_receiver||!sink)
                throw std::runtime_error("Actor immediate controller hosting is unresolved");
            s.flags_3248=(s.flags_3248|0x10000U|0x800000U)&~(0x20000U|0x1000000U);
            sink({config.controller_receiver,11,-1,0,5},s,view);
        }
        action(s,16,false);action(s,12,false);s.flags_3248|=0x200000U;physical_mode(s,9,false);
        if(id==0x4a)s.flags_3248&=~0x200000U;
        return;
    case 0x49:s.flags_3248&=~0x200000U;return;
    case 0x4b:
        action(s,16,true);action(s,13,true);s.flags_3248|=0x400000U;physical_mode(s,9,true);return;
    case 0x4c:s.flags_3248&=~0x400000U;return;
    case 0x4d:case 0x4f:
        action(s,16,false);action(s,13,false);physical_mode(s,9,false);return;
    case 0x50:case 0x55:
        action(s,16,true);action(s,14,true);physical_mode(s,9,true);physical_mode(s,8,true);return;
    case 0x52:case 0x54:case 0x57:case 0x59:
        if(s.active_actions&0x10000U)s.flags_3248&=~0x1c0000U;
        action(s,16,false);action(s,14,false);physical_mode(s,9,false);physical_mode(s,8,false);
        if(id==0x57||id==0x59)sound(s,config,id,view,host);
        return;
    case 0x5a:s.flags_3248|=0x1000000U;return;
    case 0x5b:s.flags_3248|=0x800000U;return;
    case 0x5d:case 0x5e:case 0x62:
        action(s,16,true);action(s,15,true);
        if(id==0x62)s.flags_3248|=0x20000000U;
        physical_mode(s,9,true);
        if(id!=0x62)physical_mode(s,6,id==0x5d);
        return;
    case 0x5f:case 0x61:
        action(s,16,false);action(s,15,false);
        s.flags_3248=(s.flags_3248&~0x20000000U)|0x10000000U;
        if(id==0x5f){physical_mode(s,7,true);return;}
        physical_mode(s,6,false);[[fallthrough]];
    case 0x60:
        s.flags_3248&=~0x10000000U;physical_mode(s,9,false);physical_mode(s,7,false);return;
    case 0x3d:
        prepare_inventory_hide(s,config,host);action(s,19,true);
        physical_mode(s,5,true);physical_mode(s,3,true);physical_mode(s,2,true);physical_mode(s,11,true);
        hide_inventory(s,config,host);return;
    case 0x63:case 0x64:case 0x6b:case 0x6c:
        prepare_inventory_hide(s,config,host);
        action(s,id==0x6b?17:(id==0x6c?18:20),true);physical_mode(s,11,true);
        if(id==0x64&&s.physical)s.physical->flags_38|=8;
        hide_inventory(s,config,host);return;
    case 0x67:
        if(s.controller_3474) {
            if(!host.enqueue_controller)throw std::runtime_error("Actor linked controller queue is unresolved");
            host.enqueue_controller({s.controller_3474,14,-1,0,1},s,view);
        }
        return;
    case 0x68:case 0x6a:case 0x6e:case 0x70: {
        if(config.controller_receiver&&!host.controller_is_player)
            throw std::runtime_error("Actor controller role query is unresolved");
        if((id==0x68||id==0x6a)&&s.controller_3474&&!host.enqueue_controller)
            throw std::runtime_error("Actor linked controller queue is unresolved");
        // Conservative host preflight allows a synchronous world callback to
        // change saved selection before the original inventory reread.
        prepare_inventory_restore(s,config,host,config.controller_receiver&&s.byte_14&&s.byte_326b&&!config.world_pop_keeps_inventory);
        if(config.controller_receiver&&s.byte_14&&s.byte_326b&&!host.pop_world_object)
            throw std::runtime_error("Actor active-world pop hosting is unresolved");
        if(id==0x6e||id==0x70) {
            action(s,17,false);action(s,18,false);physical_mode(s,5,false);
        } else {
            action(s,20,false);s.flags_324c&=~1U;
            if(s.physical)s.physical->flags_38&=~8U;
        }
        physical_mode(s,11,false);
        if((id==0x68||id==0x6a)&&s.controller_3474) {
            host.enqueue_controller({s.controller_3474,id==0x68?15:13,-1,config.receiver,1},s,view);
            s.controller_3474=0;
        }
        const bool player=config.controller_receiver&&host.controller_is_player(config.controller_receiver);
        if(player&&s.byte_14&&s.byte_326b) {
            host.pop_world_object(s,view);s.byte_326b=0;
        }
        // No inventory reference crosses the synchronous world callback.
        restore_inventory(s,config,host);
        return;
    }
    case 0x71:
        action(s,21,true);s.flags_324c|=2;physical_mode(s,10,true);return;
    case 0x72: {
        if(!config.roots||!view.can_set_channel_value(14))
            throw std::runtime_error("Actor placement/channel callback hosting is unresolved");
        const auto generation=host.object_generation;const auto setter=host.set_other_model_channel;
        if(s.linked_model.identity&&(!generation||!setter))
            throw std::runtime_error("Linked actor model hosting is unresolved");
        action(s,21,true);
        view.copy_anchor_world_placement(config.roots->flor,config.roots->edge);
        view.set_channel_value(14,.5F);
        if(s.linked_model.identity) {
            const auto first=generation(s.linked_model.identity);
            if(first&&*first==s.linked_model.generation) {
                const auto second=generation(s.linked_model.identity);
                if(!second||*second!=s.linked_model.generation)
                    throw std::runtime_error("Linked actor invalidated between original reference checks");
                if(s.linked_model.identity==config.receiver)view.set_channel_value(14,.5F);
                else setter(s.linked_model.identity,14,.5F);
            }
        }
        return;
    }
    case 0x74:case 0x76:
        action(s,21,false);s.flags_324c|=4;physical_mode(s,10,false);
        if(id==0x74)return;
        [[fallthrough]];
    case 0x75:s.flags_324c&=~6U;clear_children();return;
    case 0x77:
        if(!config.related_root||!host.attach_related_roots)
            throw std::runtime_error("Actor related-root attachment key or host is unresolved");
        action(s,21,true);s.flags_324c|=8;
        host.attach_related_roots(*config.related_root,s,view);return;
    case 0x78:action(s,21,true);return;
    case 0x79:action(s,21,false);return;
    case 0x7b:action(s,21,false);[[fallthrough]];
    case 0x7a:s.flags_324c&=~8U;return;
    case 0x7c:action(s,21,true);s.flags_324c|=0x10;physical_mode(s,10,true);return;
    case 0x7d:action(s,21,true);clear_children();return;
    case 0x7e:case 0x80:
        physical_mode(s,10,false);if(id==0x7e)return;
        [[fallthrough]];
    case 0x7f:action(s,21,false);s.flags_324c&=~0x10U;clear_children();return;
    case 0x81:action(s,22,true);s.flags_324c|=0x20;physical_mode(s,10,true);return;
    case 0x82:case 0x87:case 0x8c:action(s,22,true);return;
    case 0x83:case 0x89:case 0x8d:action(s,22,false);return;
    case 0x85:action(s,22,false);[[fallthrough]];
    case 0x84:s.flags_324c&=~0x20U;physical_mode(s,10,false);return;
    case 0x86:action(s,22,true);s.flags_324c|=0x40;physical_mode(s,12,true);return;
    case 0x8a:action(s,22,false);[[fallthrough]];
    case 0x88:s.flags_324c&=~0x40U;physical_mode(s,12,false);return;
    case 0x8b:
        action(s,22,true);s.flags_324c|=0x80;physical_mode(s,10,true);
        if(s.physical)s.physical->byte_14=3;
        return;
    case 0x8e:case 0x8f: {
        if(s.physical&&config.controller_receiver&&!host.controller_is_player)
            throw std::runtime_error("Actor physical completion player-role query is unresolved");
        if(id==0x8f)action(s,22,false);
        s.flags_324c&=~0x80U;
        if(s.physical) {
            physical_mode(s,10,false);
            const bool player=config.controller_receiver&&host.controller_is_player(config.controller_receiver);
            if(!player||!s.byte_14)s.physical->byte_14=1;
        }
        return;
    }
    case 0x90:action(s,28,false);action(s,30,true);return;
    case 0x91:action(s,28,false);action(s,29,true);return;
    case 0x92:case 0x93: {
        if(!(s.active_actions&0x80000000U)) {
            // Require both role branches before any synchronous mutation: the
            // cancellation host can change the later live role query result.
            if(!s.inventory||!config.controller_receiver||!host.controller_is_player||
               !host.create_world_inventory||!host.reset_history_template||!host.enqueue_controller)
                throw std::runtime_error("Actor reset inventory/controller dependencies are unresolved");
            if(s.inventory->owner_control_active&&
               (!s.inventory->control_component||!host.cancel_inventory_component))
                throw std::runtime_error("Actor active inventory component teardown is unresolved");
        }
        action(s,27,false);
        if(!(s.active_actions&0x80000000U)) {
            action(s,31,true);
            const auto component=s.inventory->control_component;
            if(component&&s.inventory->owner_control_active)
                host.cancel_inventory_component(component,s,view);
            if(!host.controller_is_player(config.controller_receiver))
                host.create_world_inventory(s,view);
            s.history_31c0=host.reset_history_template();
            s.history_31d0=0;s.history_31d4=0;s.byte_31b7=0;
            host.enqueue_controller({config.controller_receiver,16,-1,0,5},s,view);
            if(s.physical) {
                const bool player=host.controller_is_player(config.controller_receiver);
                if(!player||!s.byte_14)s.physical->byte_14=1;
            }
        }
        if(id==0x93)action(s,31,false);
        return;
    }
    case 0x94:action(s,23,true);return;
    case 0x95:action(s,23,false);return;
    case 0x96:case 0x97:
        if(!s.physical||!host.finish_world_pending)
            throw std::runtime_error("Actor physical/world pending completion is unresolved");
        if(id==0x97)action(s,23,false);
        s.physical->byte_3d0=1;
        host.finish_world_pending(s,view);
        s.byte_33e8=0;return;
    case 0x98: {
        if(!host.resolve_named_target)throw std::runtime_error("Actor sound target lookup is unresolved");
        const auto target=host.resolve_named_target(s.target_33cc);
        if(!target)return;
        if(!host.effect_descriptor||!host.target_transform||!host.emit_directional_effect||
           !config.target_transform_selector||!host.sound_preset||!host.enqueue_sound||!config.actor_generation)
            throw std::runtime_error("Actor target effect/audio dependencies are unresolved");
        const auto descriptor=host.effect_descriptor(0x19);
        if(!descriptor)throw std::runtime_error("Actor target effect descriptor is absent");
        const auto transform=host.target_transform(target,*config.target_transform_selector);
        if(*descriptor>=0) {
            RootVector position{},direction{};
            for(unsigned index=0;index<4;++index) {
                position[index]=transform[12+index];direction[index]=transform[4+index];
                if(!::omega::float_bits::finite(position[index])||!::omega::float_bits::finite(direction[index])||
                   ::omega::float_bits::subnormal(position[index])||::omega::float_bits::subnormal(direction[index]))
                    throw std::runtime_error("Actor directional effect transform outside finite domain");
            }
            host.emit_directional_effect(*descriptor,position,direction,s,view);
        }
        queue_sound(s,config,host.sound_preset(id),view,host);return;
    }
    case 0x99:
        if(!host.sound_preset||!host.enqueue_sound||!config.actor_generation)
            throw std::runtime_error("Actor queued sound preset is unresolved");
        queue_sound(s,config,host.sound_preset(id),view,host);return;
    case 0x9e:case 0x9f:action(s,24,true);s.flags_324c|=0x200;return;
    case 0xa0:case 0xa1:case 0xa3: {
        const bool launches=id==0xa0||(s.flags_324c&0x200U);
        inventory(s);
        if(launches) {
            if(!host.launch_inventory_projectile||!host.null_target_key)
                throw std::runtime_error("Actor projectile factory dependency is unresolved");
            if(::omega::float_bits::nonfinite_or_subnormal(s.launch_33e0))
                throw std::runtime_error("Actor projectile launch input outside finite domain");
            for(auto value:s.aim_33d0)if(::omega::float_bits::nonfinite_or_subnormal(value))
                throw std::runtime_error("Actor projectile aim outside finite domain");
            if(id!=0xa0) {
                // Launch callbacks may change selected mode/count/owner.
                // Preflight every release branch before the first factory call.
                require_inventory_host(s,config,host);
                if(!s.attachment_byte_a0||!config.inventory||config.inventory->definitions.empty()||!host.set_model_slot)
                    throw std::runtime_error("Actor post-projectile inventory release hosting is unresolved");
            }
        }
        if(id!=0xa0)action(s,24,false);
        if(launches) {
            s.flags_324c&=~0x200U;
            host.launch_inventory_projectile(s.launch_33e0,s,view);
            s.target_33cc=host.null_target_key();s.byte_33e4=2;
        }
        if(id!=0xa0){release_inventory(s,config,host);s.byte_33e4=0;}
        return;
    }
    default:unsupported();
    }
}
void embedded(ActorAnimationReceiverState& s,const ActorAnimationReceiverConfig& cfg,
    const MotionEffect& effect,const ActorAnimationReceiverHost& host) {
    if(effect.id==-1)return;
    if(effect.id<0||effect.id>4)unsupported();
    if(effect.action!=4&&effect.action!=5)return;
    switch(effect.id) {
    case 0:action(s,3,false);return;
    case 1:
        if(!s.attachment_byte_a0)throw std::runtime_error("Actor animation attachment is unresolved");
        *s.attachment_byte_a0=0;return;
    case 2:release_inventory(s,cfg,host);s.byte_33e4=0;return;
    case 3:s.byte_33e5=0;return;
    case 4:s.byte_33e8=0;return;
    }
}
}
std::optional<RootParentTransform> resolve_receiver_motion_placement(const AnimationGraph& graph,
    const AnimationClipBank& clips,std::string_view name,const RootVector& initial_translation,
    MotionPlacementEndpoint endpoint) {
    const bool start=endpoint==MotionPlacementEndpoint::start;  // 001901c0
    const AnimationGraphNode* selected=nullptr;
    for(const auto& node:graph.nodes())if(node.name==name) {
        if(selected)throw std::runtime_error("Ambiguous actor motion registry key");
        selected=&node;
    }
    if(!selected||selected->kind!=AnimationGraphKind::Leaf)return std::nullopt;
    const auto& policy=selected->leaf;
    const auto at=clips.find(policy.clip_key);
    if(at==clips.end()||at->second.version!=3)throw std::runtime_error("Actor motion clip is unresolved");
    const auto checked=[](float value){
        if(::omega::float_bits::nonfinite_or_subnormal(value))
            throw std::runtime_error("Actor motion placement outside finite normal-or-zero domain");
        return value;
    };
    if(policy.phase_offset!=0||policy.range_start!=0||policy.range_end!=1) {
        const auto& clip=at->second;
        const std::uint64_t extent=std::uint64_t(clip.frame_intervals)+(clip.looping?0:1);
        if(!clip.bone_count||clip.bone_count>32||!clip.frame_intervals||extent>10000||clip.frames.size()!=extent)
            throw std::runtime_error("Actor motion endpoint clip extent is malformed");
        const auto phase=checked(policy.phase_offset!=0?policy.phase_offset:
            (policy.reverse!=start?policy.range_start:policy.range_end));
        if(phase<0)throw std::runtime_error("Actor motion endpoint has negative frame coordinate");
        const auto intervals=static_cast<float>(clip.frame_intervals);
        auto coordinate=checked(phase*intervals);
        if(clip.looping) {
            unsigned wraps=0;
            while(coordinate>=intervals) {
                if(++wraps>4096)throw std::runtime_error("Actor motion endpoint wrap limit");
                coordinate=checked(coordinate-intervals);
            }
        } else if(coordinate>=intervals)coordinate=intervals;
        const auto first=static_cast<std::uint32_t>(coordinate);
        const auto second=clip.looping?(first+1<clip.frame_intervals?first+1:0):
            (first<clip.frame_intervals?first+1:first);
        const auto fraction=checked(coordinate-static_cast<float>(first));
        const auto& a=clip.frames.at(first);const auto& b=clip.frames.at(second);
        if(a.bones.size()!=clip.bone_count||b.bones.size()!=clip.bone_count)
            throw std::runtime_error("Actor motion endpoint frame bones are malformed");
        const auto word=[&](std::size_t offset){std::uint32_t bits=0;for(unsigned i=0;i<4;++i)bits|=std::uint32_t(clip.header_bytes[offset+i])<<(8*i);return bits;};
        const bool translation=(clip.translation_mask&1U)!=0,rotation=(clip.rotation_mask&1U)!=0;
        const bool yaw=rotation&&word(0x18)!=0;
        if(a.bones[0].translation.has_value()!=translation||b.bones[0].translation.has_value()!=translation||
           a.bones[0].rotation.has_value()!=rotation||b.bones[0].rotation.has_value()!=rotation||
           a.root_yaw_words.has_value()!=yaw||b.root_yaw_words.has_value()!=yaw)
            throw std::runtime_error("Actor motion endpoint root channels disagree with masks");
        const auto interpolate=[&](float x,float y){
            checked(x);checked(y);
            const auto incoming=checked(y*fraction);
            const auto accumulated=checked(x+incoming);
            const auto outgoing=checked(x*fraction);
            return checked(accumulated-outgoing);
        };
        RootParentTransform result;
        result.position=initial_translation;
        if(translation) {
            for(unsigned i=0;i<3;++i)result.position[i]=interpolate((*a.bones[0].translation)[i],(*b.bones[0].translation)[i]);
            //190700 is only a copy. Caller returns poseT, not motionT: the
            // separate XY motion vector is discarded when header14 is nonzero.
            if(word(0x14)!=0){result.position[0]=0;result.position[1]=0;}
        }
        for(unsigned i=0;i<3;++i)checked(result.position[i]);
        result.position[3]=1;
        if(yaw) {
            // The original also computes a separate yaw quaternion, but the
            // caller returns poseQ at+30, not that motionQ at+40.
            const auto angle_a=checked(std::bit_cast<float>((*a.root_yaw_words)[0]));
            const auto angle_b=checked(std::bit_cast<float>((*b.root_yaw_words)[0]));
            checked(checked(checked(1-fraction)*angle_a)+checked(fraction*angle_b));
        }
        if(rotation) {
            RootVector squares{};
            for(unsigned i=0;i<4;++i){result.orientation[i]=interpolate((*a.bones[0].rotation)[i],(*b.bones[0].rotation)[i]);squares[i]=checked(result.orientation[i]*result.orientation[i]);}
            const auto norm=checked(checked(checked(squares[0]+squares[1])+squares[2])+squares[3]);
            if(norm<=0)throw std::runtime_error("Actor motion endpoint quaternion cancels to zero");
            const auto inverse=checked(vu::rsqrt(norm)); // 00190750 vrsqrt: root, then 1/root (VU_ROUNDING.md)
            for(auto& value:result.orientation)value=checked(value*inverse);
        }
        return result;
    }
    const auto& bytes=at->second.header_bytes;RootParentTransform result;
    for(unsigned index=0;index<8;++index) {
        const auto offset=(start?0x30U:0x50U)+index*4;std::uint32_t bits=0;
        for(unsigned byte=0;byte<4;++byte)bits|=std::uint32_t(bytes[offset+byte])<<(8*byte);
        const auto value=std::bit_cast<float>(bits);
        if(::omega::float_bits::nonfinite_or_subnormal(value))
            throw std::runtime_error("Actor motion header transform outside finite domain");
        (index<4?result.orientation[index]:result.position[index-4])=value;
    }
    return result;
}
void receive_actor_animation(ActorAnimationReceiverState& state,
    const ActorAnimationReceiverConfig& config,const MotionEffect& effect,const MotionDeliveryView& view,
    const ActorAnimationReceiverHost& host) {
    if(!config.receiver||effect.receiver!=config.receiver)throw std::runtime_error("Actor animation receiver identity mismatch");
    auto next=state;const auto callbacks=host;
    switch(effect.kind) {
    case MotionEffect::Kind::token:token(next,config,effect.id,view,callbacks);break;
    case MotionEffect::Kind::embedded:embedded(next,config,effect,callbacks);break;
    default:throw std::runtime_error("Unknown actor animation event kind");
    }
    state=next;
}
void set_actor_action(ActorAnimationReceiverState& state,unsigned index,bool enabled) {
    if(index>31)throw std::runtime_error("Actor action index outside the priority mask");
    action(state,index,enabled);
}
void restore_actor_inventory(ActorAnimationReceiverState& state,const ActorAnimationReceiverConfig& config,
    const ActorAnimationReceiverHost& host) {
    restore_inventory(state,config,host);
}
void hide_actor_inventory(ActorAnimationReceiverState& state,const ActorAnimationReceiverConfig& config,
    const ActorAnimationReceiverHost& host) {
    prepare_inventory_hide(state,config,host);hide_inventory(state,config,host);
}
}
