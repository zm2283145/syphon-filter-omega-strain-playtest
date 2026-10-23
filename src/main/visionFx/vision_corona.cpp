#include "vision_corona.hpp"

#include <sstream>
#include <stdexcept>
#include <string>

namespace omega {
namespace {

std::string hex(std::uint32_t v){std::ostringstream o;o<<std::hex<<v;return o.str();}

bool plain_light(std::uint32_t vtable){
    return vtable==level_light_vtable_base||vtable==level_light_vtable_spot||vtable==level_light_vtable_directional||
           vtable==level_light_vtable_point||vtable==level_light_vtable_ambient;
}

}

std::vector<LevelLightRef> light_table_select_00372b40(const LevelLights& lights, std::span<const LevelLightTableRow> table,
                                                       std::uint8_t type_mask, std::uint8_t aux_mask, std::uint8_t class_mask){
    std::vector<LevelLightRef> out;
    for(const auto& row:table){
        const auto& pool=lights.pool(row.light.pool);
        if(row.light.index>=pool.size())throw std::runtime_error("00372b40: light table row past its pool");
        const LevelLight& l=pool[row.light.index];
        if((l.type&type_mask)==0)continue;
        if(aux_mask!=0xff&&(l.aux&aux_mask)==0)continue;
        if((row.cls&class_mask)==0)continue;
        out.push_back(row.light);
    }
    return out;
}

std::vector<VisionCoronaCall> vision_corona_0025f4c0(std::int32_t vision_mode, const LevelLights& lights,
                                                     std::span<const LevelLightTableRow> table,
                                                     std::optional<std::size_t> point_194,
                                                     const std::function<float(const LevelLightRef&)>& flashlight_glare_40){
    std::uint8_t type_mask=0,aux_mask=0,class_mask=0;
    float alpha=1.0F;
    if(vision_mode==0){type_mask=0x54;aux_mask=0x18;class_mask=1;}
    else if(vision_mode==1||vision_mode==2){type_mask=0x44;aux_mask=0xff;class_mask=1;alpha=0.7F;}
    std::vector<VisionCoronaCall> out;
    for(const auto& ref:light_table_select_00372b40(lights,table,type_mask,aux_mask,class_mask)){
        if(point_194&&ref.pool==LevelLightPool::point&&ref.index==*point_194)continue;
        const LevelLight& l=lights.pool(ref.pool)[ref.index];
        const bool flashlight=l.vtable==level_light_vtable_flashlight;
        if(!flashlight&&!plain_light(l.vtable))
            throw std::runtime_error("0025f4c0: corona light with an unported vtable "+hex(l.vtable)+" (vcalls +20 / +24 / +34)");
        VisionCoronaCall c;
        c.light=ref;
        c.position={l.position[0],l.position[1],l.position[2],l.position[3]};
        // vcall +0x34: 0045bc00 FlashDazzleSize (00497a20) or 001479f0 (light +0x14).
        c.half=0.5F*(flashlight?lights.flash_dazzle_size:l.outer);
        c.count=(l.aux&8)?9U:1U;
        for(std::uint32_t k=0;k<c.count;++k){
            const auto& row=vision_corona_rows_004a7de0[k];
            c.colours[k]={l.colour[0]*row[0],l.colour[1]*row[1],l.colour[2]*row[2],alpha*row[3]};
        }
        c.sprite=flashlight?0U:1U;                   // vcall +0x24: 0045bbf0 returns 0, 001479c0 returns 1
        c.glare=(l.aux&0x10)!=0;
        if(c.glare){
            if(flashlight){                          // vcall +0x20: 0045bbe0 (light +0x40)
                if(!flashlight_glare_40)throw std::runtime_error("0025f4c0: flashlight glare light without its +0x40");
                c.half=c.half*flashlight_glare_40(ref);
            }
            else c.half=c.half*1.0F;                 // 001479b0 returns 1.0
        }
        out.push_back(c);
    }
    return out;
}

}
