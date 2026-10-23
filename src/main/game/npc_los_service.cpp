#include "npc_los_service.hpp"
#include <stdexcept>

namespace omega {
bool npc_los_request(NpcLosService& service,const NpcLosRequest& request){
    if(request.mode_5c==0)throw std::runtime_error("NPC LOS: a request with +5c = 0 is answered at once (answer_los_request), not queued");
    std::size_t k=0;                                                            // 0013a930 .. 0013a840: the first inactive entry
    while(k<service.pool.size()&&service.pool[k].active_20!=0)++k;
    if(k==service.pool.size())return false;                                    // 0013a738: dropped
    auto& entry=service.pool[k];
    std::uint32_t serial=0;
    if(request.serial_28){                                                      // 0013a8d0: +48 += 1, 0 -> 1
        ++service.serial_48;
        if(service.serial_48==0)service.serial_48=1;
        *request.serial_28=service.serial_48;
        serial=*request.serial_28;
    }
    // 0013a870: +0 origin, +10 displacement, +20 = 1, +24 receiver, +28 the
    // storage (the entry's own +30 for a null +2c), +d0 serial, +d4 / +d8 the
    // objects, +dc the mask.
    entry.origin_00=request.origin_30;entry.displacement_10=request.displacement_40;entry.active_20=1;
    entry.receiver_24=request.receiver_24;entry.serial_d0=serial;
    entry.object_d4=request.object_50;entry.object_d8=request.object_54;entry.mask_dc=request.mask_58;
    if(request.mode_5c==1){service.list_30.push_back(k);return true;}           // 0013a7f0(+30)
    if(request.mode_5c==2){service.list_3c.push_back(k);return true;}           // 0013a7f0(+3c)
    entry.active_20=0;                                                          // 0013a7a0
    return false;
}

namespace {
// 0013ae20(manager, entry).
void execute(NpcLosEntry& entry,const NpcLosTrace& trace){
    NpcLosQuery q;q.origin=entry.origin_00;q.displacement=entry.displacement_10;
    q.excluded_mask=static_cast<std::uint8_t>(entry.mask_dc&0xffU);
    for(const auto o:{entry.object_d4,entry.object_d8})                         // 0013ab10-like adds at +18 (count +14 < 2)
        if(o!=0&&q.object_count<2)q.objects[q.object_count++]=o;
    if(!trace)throw std::runtime_error("NPC LOS: the LOS manager's query (003c26d0 through the provider *004ea0b8) is not hosted");
    const auto hit=trace(q);
    NpcLosResult r;r.hit_30=hit?1:0;r.serial_c0=entry.serial_d0;
    if(hit)r.point_40=*hit;
    if(entry.receiver_24)entry.receiver_24(r);                                 // 005435a8, transport 1
}
}

void update_npc_los_service(NpcLosService& service,const NpcLosTrace& trace){
    // List +30: every entry up to its end sentinel (entries a receiver appends
    // meanwhile are reached too), then the list is emptied (00138c60).
    for(std::size_t i=0;i<service.list_30.size();++i){
        const auto k=service.list_30[i];
        execute(service.pool[k],trace);
        service.pool[k].active_20=0;                                            // 0013ae00
    }
    service.list_30.clear();
    // List +3c: its head only (the receiver may append new requests behind it).
    if(!service.list_3c.empty()){
        const auto k=service.list_3c.front();
        execute(service.pool[k],trace);
        service.pool[k].active_20=0;
        service.list_3c.pop_front();                                            // 00138da0 on the head read after the execution
    }
}
}
