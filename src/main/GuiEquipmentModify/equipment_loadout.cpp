#include "equipment_loadout.hpp"
#include <stdexcept>
#include <utility>

namespace omega {

void apply_attach_events(ModelAttachTable& table,std::span<const WeaponInventoryEvent> events,
    const AttachedModelLookup& model){
    using K=WeaponInventoryEvent::Kind;
    for(const auto& e:events){
        switch(e.kind){
        case K::attach:
            if(e.model!=0&&model(e.model).byte_40c>=8)break;
            table.at(static_cast<std::size_t>(e.point))=e.model;
            break;
        case K::detach:
            table.at(static_cast<std::size_t>(e.point))=0;
            break;
        case K::move_attach:
            std::swap(table.at(static_cast<std::size_t>(e.point)),table.at(static_cast<std::size_t>(e.to)));
            break;
        default:
            break;
        }
    }
}

std::array<EquipmentAcceptStep,4> accept_equipment_modify(WeaponInventory& inventory,const WeaponDatabase& database,
    WeaponInventoryHost host,ModelAttachTable& player,EquipmentModifyScreen& screen,const AttachedModelLookup& model,
    std::vector<WeaponInventoryEvent>& events,bool multiplayer){
    if(multiplayer)throw std::runtime_error("Equipment accept: the multiplayer path (00414790 / 00418a80) is not hosted");
    const auto fc=screen.hand_point_fc;
    if(fc>=model_attach_point_count)throw std::runtime_error("Equipment accept: hand point +fc outside the attach table");
    auto& preview=screen.preview;
    if(preview[0]!=0&&fc!=0){
        preview[fc]=preview[0];
        preview[0]=0;
        if(preview[1]!=0){preview[4]=preview[1];preview[1]=0;}
    }
    host.attached=[&player](std::uint8_t point){return player.at(point)!=0;};
    WeaponInventoryOps ops(inventory,database,host,events);
    auto sync=[&](std::size_t from){
        apply_attach_events(player,std::span(events).subspan(from),model);
    };
    std::size_t n=events.size();
    ops.holster_current();
    sync(n);
    for(std::size_t point=2;point<model_attach_point_count;++point){
        if(player[point]==preview[point])continue;
        if(player[point]!=0){
            n=events.size();
            ops.remove(weapon_slot_of_class(static_cast<std::uint8_t>(point)),false);
            sync(n);
        }
        if(preview[point]!=0){
            n=events.size();
            ops.add(static_cast<std::int16_t>(model(preview[point]).item_40a),true,true);
            sync(n);
        }
    }
    if(fc!=0&&preview[fc]!=0){
        n=events.size();
        ops.equip(ops.slot_of_item(static_cast<std::int16_t>(model(preview[fc]).item_40a)));
        sync(n);
    }
    return {EquipmentAcceptStep::clear_loadout_pending,EquipmentAcceptStep::pad_update,EquipmentAcceptStep::pop_screen,
        EquipmentAcceptStep::resume};
}

}
