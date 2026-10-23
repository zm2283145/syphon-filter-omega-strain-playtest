#include "play_loadout.hpp"
#include <algorithm>
#include <charconv>
#include <optional>
#include <stdexcept>

namespace omega {
namespace {

constexpr std::int32_t stand_in_base=0x10000;
constexpr std::array<std::string_view,5> category_names{"back","hip","leg","grenade","melee"};

[[noreturn]] void bad(const std::string& why){throw std::runtime_error("--loadout: "+why);}

std::size_t category_of(std::string_view name){
    for(std::size_t k=0;k<category_names.size();++k)if(category_names[k]==name)return k;
    bad("unknown category '"+std::string(name)+"' (back, hip, leg, grenade, melee, hand)");
}

}

EquipmentLoadoutSelection default_equipment_loadout(){
    EquipmentLoadoutSelection s;
    s.items[1]=106;     // Baretta_92F
    s.items[4]=129;     // StunBaton
    s.hand_point=3;
    return s;
}

EquipmentLoadoutSelection parse_equipment_loadout(std::string_view spec){
    if(spec=="default")return default_equipment_loadout();
    EquipmentLoadoutSelection s;
    std::optional<std::uint8_t> hand;
    while(!spec.empty()){
        const auto comma=spec.find(',');
        const auto entry=spec.substr(0,comma);
        spec=comma==std::string_view::npos?std::string_view{}:spec.substr(comma+1);
        const auto eq=entry.find('=');
        if(eq==std::string_view::npos)bad("entry '"+std::string(entry)+"' is not name=value");
        const auto name=entry.substr(0,eq),value=entry.substr(eq+1);
        if(name=="hand"){
            hand=value=="none"?std::uint8_t{0}:loadout_category_points[category_of(value)];
            continue;
        }
        auto& item=s.items[category_of(name)];
        if(value=="none"){item=-1;continue;}
        std::int32_t id{};
        const auto [end,ec]=std::from_chars(value.data(),value.data()+value.size(),id);
        if(ec!=std::errc{}||end!=value.data()+value.size()||id<0)bad("'"+std::string(value)+"' is not an item ID or none");
        item=id;
    }
    s.hand_point=hand?*hand:std::uint8_t{3};
    return s;
}

bool fresh_profile_loadout(const EquipmentLoadoutSelection& s){
    return s.items[0]==-1&&(s.items[1]==-1||s.items[1]==106)&&s.items[2]==-1&&s.items[3]==-1&&(s.items[4]==-1||s.items[4]==129);
}

std::int32_t loadout_stand_in_model(std::int32_t item){return stand_in_base+item;}

bool loadout_stand_in(const WeaponInventoryEvent& e){
    using K=WeaponInventoryEvent::Kind;
    return e.kind==K::attach&&e.model>=stand_in_base;
}

std::array<EquipmentAcceptStep,4> apply_equipment_loadout(WeaponInventory& inventory,const WeaponDatabase& database,
    const WeaponInventoryHost& host,const EquipmentLoadoutSelection& selection,std::vector<WeaponInventoryEvent>& events){
    auto in_database=[&](std::int32_t item){
        if(item<0||static_cast<std::size_t>(item)>=database.size())
            bad("item "+std::to_string(item)+" is outside the weapon database (00147840 would select record 0)");
    };
    for(std::size_t k=0;k<selection.items.size();++k){
        const auto item=selection.items[k];
        if(item<0)continue;
        in_database(item);
        const auto cls=database.record(item).weapon_class_30();
        if(cls!=loadout_category_points[k])
            bad("item "+std::to_string(item)+" has class "+std::to_string(cls)+", not the "+std::string(category_names[k])
                +" point "+std::to_string(loadout_category_points[k])+" (002bf090 lists items by class)");
    }
    const auto& hp=selection.hand_point;
    if(hp!=0&&std::find(loadout_category_points.begin(),loadout_category_points.end(),hp)==loadout_category_points.end())
        bad("the hand point must be a category's point");
    WeaponDatabase stand_in=database;
    auto model_of=[&](std::int32_t item){
        in_database(item);
        stand_in.mutable_record(item).set_word(0x13c,loadout_stand_in_model(item));
        return loadout_stand_in_model(item);
    };
    ModelAttachTable player{},preview{};
    for(std::uint8_t k=0;k<inventory.slots.size();++k){
        const auto item=inventory.slots[k].item;
        if(item==-1)continue;
        std::uint8_t point=k==inventory.current_84?weapon_hand_point(k):weapon_holster_point(k);
        if(k==1&&inventory.current_84==0)point=1;
        player.at(point)=model_of(item);
        preview.at(weapon_holster_point(k))=model_of(item);
    }
    for(std::size_t k=0;k<selection.items.size();++k){
        const auto item=selection.items[k];
        preview[loadout_category_points[k]]=item<0?0:model_of(item);
    }
    if(selection.point_4)preview[4]=*selection.point_4<0?0:model_of(*selection.point_4);
    EquipmentModifyScreen screen{preview,hp};
    const AttachedModelLookup lookup=[](std::int32_t model){
        if(model<stand_in_base)throw std::runtime_error("--loadout: a model that is not a stand-in");
        return AttachedModel{model-stand_in_base,0};
    };
    return accept_equipment_modify(inventory,stand_in,host,player,screen,lookup,events);
}

std::string describe_inventory(const WeaponInventory& inv){
    std::string s;
    for(std::size_t k=0;k<inv.slots.size();++k){
        const auto& slot=inv.slots[k];
        if(slot.item==-1)continue;
        if(!s.empty())s+=", ";
        s+="slot "+std::to_string(k)+" item "+std::to_string(slot.item)+" "+std::to_string(slot.clip)+"/"+std::to_string(slot.spare);
    }
    s+=inv.current_84==weapon_no_slot?"; nothing held":"; holding slot "+std::to_string(inv.current_84);
    return s;
}

}
