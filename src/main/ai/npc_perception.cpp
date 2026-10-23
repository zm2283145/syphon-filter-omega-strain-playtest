#include "npc_perception.hpp"
#include <bit>

namespace omega {

void perception_add_awareness(NpcPerception& p,const AwarenessObjectView& o,std::uint8_t cause,float value,
    std::vector<PerceptionSend>& sends){
    // 0016db40: the first record whose handle resolves to the object.
    AwarenessRecord* rec=nullptr;
    for(auto& r:p.records)if(r.object!=0&&r.object==o.object&&r.id_4==o.id){rec=&r;break;}
    bool dead_actor=false;                                                     // s1
    if(o.life_108==3){                                                         // virtual +108
        if(o.actor_class){
            const bool present=o.root[1]!=0&&o.root[0]!=0&&o.root[2]==0;
            dead_actor=present&&o.flag_3468!=0;
        }
        if(!dead_actor)return;
    }
    if(rec){
        if(rec->byte_39!=0)rec->byte_3a=1;
        if(rec->level_24<0.0F)return;                                          // 0016cb14 c.olt.s
    }else{
        p.records.insert(p.records.begin(),AwarenessRecord{});                 // 0016cee0: at the front
        rec=&p.records.front();
    }
    rec->level_24=0.0F;
    switch(cause){
    case 6: rec->byte_38=1;rec->byte_39=1;rec->byte_3a=1;break;
    case 4:
        rec->score_34=std::bit_cast<float>(0x3e4ccccdU);                     // 0.2
        if(!(rec->byte_39!=0&&rec->byte_38!=0)&&!(value<=25.0F))rec->level_24=-value/500.0F;
        break;
    case 8: rec->score_34=1.0F;break;
    case 7: rec->score_34=5.0F;rec->level_24=-1.0F;break;
    default: break;
    }
    rec->object=o.object;
    rec->id_4=o.object!=0?o.id:-1;
    rec->id_8=o.id;
    rec->position_10=o.position;
    rec->cause_20=cause;
    if(dead_actor&&rec->dead_notice_3b==0){
        sends.push_back({o.object});
        rec->dead_notice_3b=1;
    }
}

void perception_clear(NpcPerception& p){p.records.clear();}

}
