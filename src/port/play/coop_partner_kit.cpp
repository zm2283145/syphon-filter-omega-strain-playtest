#include "coop_partner_kit.hpp"
#include "bullet_hit.hpp"

#include <algorithm>
#include <cmath>
#include <sstream>
#include <stdexcept>
#include <tuple>

namespace omega {
namespace {

bool firearm_category(std::uint8_t point){return point==2||point==3||point==5;}

std::optional<std::uint8_t> slot_of(const WeaponRecord& record){
    const auto c=record.weapon_class_30();
    if(c>=10)return std::nullopt;                       // 00489d60 has 10 rows
    return weapon_slot_of_class(c);
}

// The rounds an inventory holds for `item` (the slot of its class when that slot holds it).
std::optional<std::int32_t> held_rounds(const WeaponDatabase& db,const WeaponInventory& inv,std::int32_t item){
    const auto s=slot_of(db.record(item));
    if(!s||*s>=inv.slots.size()||inv.slots[*s].item!=item)return std::nullopt;
    return inv.slots[*s].clip+inv.slots[*s].spare;
}

// A better (score, rating, lower ID) order.
bool better_score(const WeaponDatabase& db,std::int32_t a,std::int32_t b){
    const auto& ra=db.record(a);const auto& rb=db.record(b);
    const auto ka=std::make_tuple(partner_loadout_score(ra),partner_weapon_rating(ra),-a);
    const auto kb=std::make_tuple(partner_loadout_score(rb),partner_weapon_rating(rb),-b);
    return ka>kb;
}

}

std::optional<PartnerLoadoutMode> parse_partner_loadout_mode(std::string_view text){
    if(text=="auto")return PartnerLoadoutMode::automatic;
    if(text=="copy")return PartnerLoadoutMode::copy;
    if(text=="record")return PartnerLoadoutMode::record;
    return std::nullopt;
}

const char* partner_loadout_mode_name(PartnerLoadoutMode mode){
    switch(mode){
    case PartnerLoadoutMode::automatic:return "auto";
    case PartnerLoadoutMode::copy:return "copy";
    case PartnerLoadoutMode::record:return "record";
    }
    return "?";
}

std::int32_t partner_weapon_rating(const WeaponRecord& record){return record.damage_mid_64();}

std::int32_t partner_loadout_score(const WeaponRecord& record){
    return record.damage_mid_64()*std::min(record.word(0x60),record.clip_70());
}

bool partner_usable_firearm(const WeaponRecord& record){
    const auto kind=record.item_kind_48();
    const auto type=record.damage_type_bc();
    return record.hold_class_f0()==0&&kind>=1&&kind<=4&&!(record.explosion_radius_c4()>0.0F)&&record.projectile_d4()==0
        &&(type==0||type==1||type==13)&&partner_weapon_rating(record)>0&&record.weapon_class_30()<10
        &&firearm_category(record.weapon_class_30())&&record.display_name_80()!="base";
}

bool partner_melee(const WeaponRecord& record){
    return record.weapon_class_30()==9&&(record.hold_class_f0()==3||record.hold_class_f0()==4);
}

bool partner_unsafe_item(const WeaponRecord& record){
    const auto type=record.damage_type_bc();
    const auto kind=record.item_kind_48();
    const auto c=record.weapon_class_30();
    return record.explosion_radius_c4()>0.0F||type==2||type==4||type==5||type==6||kind==5||kind==6||c==6||c==7||c>=10;
}

EquipmentLoadoutSelection choose_partner_loadout(const WeaponDatabase& database,const std::vector<std::int32_t>& unlocked){
    EquipmentLoadoutSelection s;
    std::optional<std::int32_t> hand;
    for(std::size_t k=0;k<loadout_category_points.size();++k){
        const auto point=loadout_category_points[k];
        std::optional<std::int32_t> best;
        for(const auto item:unlocked){
            if(item<0||static_cast<std::size_t>(item)>=database.size())continue;
            const auto& r=database.record(item);
            if(r.weapon_class_30()!=point)continue;
            if(firearm_category(point)){
                if(!partner_usable_firearm(r))continue;
                if(!best||better_score(database,item,*best))best=item;
            }else if(point==9){
                if(!partner_melee(r))continue;
                if(!best||std::make_tuple(partner_weapon_rating(r),-item)>
                         std::make_tuple(partner_weapon_rating(database.record(*best)),-*best))best=item;
            }
            // point 8 (grenades): the partner takes none (a thrown item near the player).
        }
        if(best)s.items[k]=*best;
        if(best&&firearm_category(point)&&(!hand||better_score(database,*best,*hand))){hand=*best;s.hand_point=point;}
    }
    if(!hand)for(std::size_t k=0;k<loadout_category_points.size();++k)
        if(s.items[k]>=0){s.hand_point=loadout_category_points[k];break;}
    return s;
}

std::vector<std::int32_t> partner_loadout_items(const EquipmentLoadoutSelection& selection){
    std::vector<std::int32_t> out;
    for(const auto item:selection.items)if(item>=0)out.push_back(item);
    return out;
}

std::optional<std::int32_t> partner_loadout_hand(const EquipmentLoadoutSelection& selection){
    for(std::size_t k=0;k<loadout_category_points.size();++k)
        if(loadout_category_points[k]==selection.hand_point&&selection.items[k]>=0)return selection.items[k];
    return std::nullopt;
}

std::string describe_partner_loadout(const WeaponDatabase& database,const EquipmentLoadoutSelection& selection){
    static constexpr const char* names[]{"back","hip","leg","grenade","melee"};
    std::ostringstream o;
    for(std::size_t k=0;k<selection.items.size();++k){
        o<<(k?", ":"")<<names[k]<<' ';
        if(selection.items[k]<0){o<<"none";continue;}
        const auto& r=database.record(selection.items[k]);
        o<<selection.items[k]<<' '<<r.display_name_80()<<" (rating "<<partner_weapon_rating(r)<<", score "<<partner_loadout_score(r)<<')';
    }
    const auto h=partner_loadout_hand(selection);
    o<<"; hand "<<(h?std::to_string(*h):std::string("none"));
    return o.str();
}

std::vector<std::int32_t> partner_armed_items(const WeaponDatabase& database,const WeaponInventory& inventory){
    std::vector<std::int32_t> out;
    for(std::size_t s=0;s<6;++s){
        const auto& slot=inventory.slots[s];
        if(slot.item<0||!partner_usable_firearm(database.record(slot.item)))continue;
        if(slot.clip+slot.spare<=0)continue;
        if(std::find(out.begin(),out.end(),slot.item)==out.end())out.push_back(slot.item);
    }
    return out;
}

float partner_damage_rate(const WeaponRecord& record,float distance){
    if(distance>static_cast<float>(record.far_3c()))return 0.0F;
    // Equal band limits only throw for a distance inside an empty band, which the far check above excludes for the
    // last band; the first band holds every distance up to near.
    if(record.mid_38()==record.near_34()&&distance>static_cast<float>(record.near_34())&&distance<=static_cast<float>(record.mid_38()))
        return 0.0F;
    const auto damage=weapon_band_damage(record,distance);
    return static_cast<float>(std::max(damage,0))*record.rate_a4();
}

PartnerWeaponPreference partner_weapon_preference(const WeaponDatabase& database,const WeaponInventory& inventory){
    PartnerWeaponPreference p;
    const auto armed=partner_armed_items(database,inventory);
    if(armed.empty())return p;
    const auto best_at=[&](float d){
        std::int32_t best=-1;float value=-1.0F;
        for(const auto item:armed){
            const float v=partner_damage_rate(database.record(item),d);
            if(v>value||(v==value&&better_score(database,item,best))){best=item;value=v;}
        }
        return std::pair<std::int32_t,float>{best,value};
    };
    p.primary_b0=best_at(partner_near_distance).first;
    for(float d=partner_near_distance+1.0F;d<=partner_far_distance;d+=1.0F){
        const auto [item,value]=best_at(d);
        if(value<=0.0F)break;
        if(item!=p.primary_b0){
            // The original switches to +b4 beyond +b8 - 10 and back nearer than +b8 (00156d00 / 001572f8): the band is
            // centred on the crossover.
            p.secondary_b4=item;p.distance_b8=d+5.0F;
            return p;
        }
    }
    for(const auto item:armed)
        if(item!=p.primary_b0&&(p.secondary_b4<0||better_score(database,item,p.secondary_b4)))p.secondary_b4=item;
    return p;
}

std::optional<std::int32_t> partner_out_of_ammo_switch(const WeaponDatabase& database,const WeaponInventory& inventory){
    if(inventory.current_84>=6)return std::nullopt;
    const auto& held=inventory.slots[inventory.current_84];
    if(held.item<0||!partner_usable_firearm(database.record(held.item)))return std::nullopt;
    if(held.clip>0||held.spare>0)return std::nullopt;
    std::optional<std::int32_t> best;
    for(const auto item:partner_armed_items(database,inventory))
        if(item!=held.item&&(!best||better_score(database,item,*best)))best=item;
    return best;
}

PartnerPickupChoice partner_pickup_choice(const WeaponDatabase& database,const PartnerBackpackView& backpack,
                                          const PartnerAgentView& partner,const PartnerAgentView& player){
    PartnerPickupChoice none;
    if(!partner.inventory||!player.inventory||!partner.capacity||!player.capacity)
        throw std::runtime_error("partner_pickup_choice: an agent view without its inventory or capacity");
    if(backpack.stash_2f3){none.reason="the player's stolen weapons (StealPlayerWeapons +2f3)";return none;}
    const auto name=[&](std::int32_t item){return std::to_string(item)+" "+database.record(item).display_name_80();};
    const auto player_needs=[&](std::int32_t item){
        const auto r=held_rounds(database,*player.inventory,item);
        return r&&*r<player.capacity(item);
    };
    // 00254da0 and 00255290 take anything only with +2f0 set; an INV: crate's is 0 (00257f30), so its contents come
    // only through the menu's Cross.
    const bool walk=backpack.enabled_2f0;
    std::int32_t gains=0;std::string what;
    const auto gain=[&](const std::string& s){++gains;what+=(what.empty()?"":", ")+s;};
    if(walk){
        for(const auto& e:backpack.entries){
            if(!e.listed||e.item<0)continue;
            const auto& r=database.record(e.item);
            const auto s=slot_of(r);
            if(!s){none.reason="the walk-over would read item "+name(e.item)+" of class >= 10";return none;}
            const auto held=partner.inventory->slots[*s].item;
            if(held<0){
                if(partner_unsafe_item(r)){none.reason="the walk-over would hand the partner "+name(e.item);return none;}
                if(e.rounds>0&&player_needs(e.item)){none.reason="the player needs the rounds of "+name(e.item);return none;}
                if(partner_usable_firearm(r)||partner_melee(r))gain(name(e.item)+" into its empty slot "+std::to_string(*s));
            }else if(held==e.item&&e.rounds>0){
                const auto have=held_rounds(database,*partner.inventory,e.item).value_or(0);
                if(have<partner.capacity(e.item)){
                    if(player_needs(e.item)){none.reason="the player needs the rounds of "+name(e.item)+" (the player has priority)";return none;}
                    gain("rounds for "+name(e.item)+" ("+std::to_string(have)+" of "+std::to_string(partner.capacity(e.item))+")");
                }
            }
        }
        if(backpack.armor>partner.armor){
            if(player.armor<backpack.armor){none.reason="armour "+std::to_string(backpack.armor)+" left for the player ("+std::to_string(player.armor)+")";return none;}
            gain("armour "+std::to_string(backpack.armor));
        }
    }
    PartnerPickupChoice best;
    std::string declined;
    for(const auto& e:backpack.entries){
        if(e.item<0)continue;
        const auto& r=database.record(e.item);
        const bool firearm=partner_usable_firearm(r);
        if(!firearm&&!partner_melee(r))continue;
        const auto s=slot_of(r);
        if(!s)continue;
        const auto held=partner.inventory->slots[*s].item;
        std::int32_t value=0;std::string why;
        if(held<0){
            if(walk&&e.listed)continue;                                        // the walk-over adds it
            value=std::max(partner_weapon_rating(r),1);
            why=name(e.item)+" for its empty slot "+std::to_string(*s);
        }else if(held==e.item){
            if(walk||!firearm||e.rounds<=0)continue;                           // the walk-over merges the rounds
            const auto have=held_rounds(database,*partner.inventory,e.item).value_or(0);
            if(have>=partner.capacity(e.item))continue;
            value=1;
            why="rounds for "+name(e.item)+" ("+std::to_string(have)+" of "+std::to_string(partner.capacity(e.item))+")";
        }else{
            if(!firearm)continue;
            const auto& h=database.record(held);
            value=partner_weapon_rating(r)-(partner_usable_firearm(h)?partner_weapon_rating(h):0);
            if(value<=0){
                declined+=(declined.empty()?"":", ")+name(e.item)+" rated "+std::to_string(partner_weapon_rating(r))+" against its "+
                          name(held)+" ("+std::to_string(partner_weapon_rating(h))+")";
                continue;
            }
            why=name(e.item)+" rated "+std::to_string(partner_weapon_rating(r))+" over its "+name(held)+" ("+
                std::to_string(partner_weapon_rating(h))+", 00142b50)";
        }
        if(e.rounds>0&&player_needs(e.item))continue;                          // the player has priority
        if(best.kind==PartnerPickupKind::none||value>best.gain||(value==best.gain&&e.item<best.item)){
            best.kind=PartnerPickupKind::cross;best.item=e.item;best.gain=value;best.reason=why;
        }
    }
    if(best.kind==PartnerPickupKind::cross){
        best.reason+=" (the menu's Cross 00251920)";
        if(!what.empty())best.reason+="; the walk-over also: "+what;
        return best;
    }
    if(gains>0){best.kind=PartnerPickupKind::walk_over;best.gain=1;best.reason=what;return best;}
    none.reason=walk?"nothing better for the partner":"nothing better for the partner in the crate";
    if(!declined.empty())none.reason+=" ("+declined+", 00142b50)";
    return none;
}

namespace {
float flat(const RootVector& a,const RootVector& b){
    const float dx=a[0]-b[0],dy=a[1]-b[1];
    return std::sqrt(dx*dx+dy*dy);
}
}

std::optional<std::size_t> coop_partner_pickup_pick(const std::vector<CoopPickupCandidate>& candidates,const CoopPickupState& state,
                                                    const CoopPickupConfig& config,const RootVector& partner,
                                                    const RootVector& player,std::uint32_t frame){
    std::optional<std::size_t> best;
    const auto key=[&](const CoopPickupCandidate& c){
        return std::make_tuple(c.choice.kind==PartnerPickupKind::cross?1:0,c.choice.gain,-flat(c.position,partner));
    };
    for(std::size_t i=0;i<candidates.size();++i){
        const auto& c=candidates[i];
        if(c.choice.kind==PartnerPickupKind::none)continue;
        if(std::any_of(state.skipped.begin(),state.skipped.end(),[&](const auto& s){return s.first==c.backpack&&frame<s.second;}))continue;
        if(flat(c.position,partner)>config.reach||std::fabs(c.position[2]-partner[2])>config.max_dz)continue;
        if(flat(c.position,player)>config.leash||std::fabs(c.position[2]-player[2])>config.max_dz)continue;
        if(!best||key(c)>key(candidates[*best]))best=i;
    }
    return best;
}

bool coop_partner_pickup_arrived(const CoopPickupConfig& config,const RootVector& goal,const RootVector& partner){
    return flat(goal,partner)<=config.take_distance&&std::fabs(goal[2]-partner[2])<=config.max_dz;
}

std::vector<RootVector> coop_partner_pickup_points(const RootVector& backpack,const RootVector& partner){
    std::vector<RootVector> out;
    for(const float r:{0.7F,1.2F,1.8F,2.5F,3.2F}){
        std::vector<RootVector> ring;
        for(int k=0;k<8;++k){
            const float a=static_cast<float>(k)*0.785398163F;
            ring.push_back({backpack[0]+r*std::cos(a),backpack[1]+r*std::sin(a),backpack[2],1.0F});
        }
        std::stable_sort(ring.begin(),ring.end(),[&](const RootVector& a,const RootVector& b){return flat(a,partner)<flat(b,partner);});
        out.insert(out.end(),ring.begin(),ring.end());
    }
    return out;
}

void coop_partner_pickup_start(CoopPickupState& state,const CoopPickupCandidate& candidate,std::uint32_t frame){
    state.backpack=candidate.backpack;
    state.swap=candidate.choice.kind==PartnerPickupKind::cross?std::optional<std::int32_t>(candidate.choice.item):std::nullopt;
    state.goal=candidate.position;
    state.started=frame;state.phase=0;state.phase_at=frame;
    state.points.clear();state.next_point=0;state.route_ended=false;
    ++state.trips;
}

void coop_partner_pickup_end(CoopPickupState& state,const CoopPickupConfig& config,std::uint32_t frame){
    if(!state.backpack)return;
    const auto b=*state.backpack;
    std::erase_if(state.skipped,[&](const auto& s){return s.first==b||frame>=s.second;});
    state.skipped.emplace_back(b,frame+config.retry_frames);
    state.backpack.reset();state.swap.reset();state.phase=0;state.points.clear();state.next_point=0;state.route_ended=false;
}

}
