#include "frontend_lights.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

namespace omega {

namespace {
// 00402f80: each of the four rows times vrsqrt(x^2 + y^2 + z^2).
void normalise_directions(FrontendLightSet& out){
    for(auto& d:out.directions){
        const float len2=d[0]*d[0]+d[1]*d[1]+d[2]*d[2];
        if(len2==0.0F)continue;   // vrsqrt of 0 gives the maximum; 0 times it stays 0
        const float r=1.0F/std::sqrt(len2);
        for(auto& v:d)v*=r;
    }
}
}  // namespace

FrontendLightSet parse_frontend_lights_4029b0(std::string_view text){
    std::string buffer(text);
    buffer.push_back('\n');
    FrontendLightSet out;
    std::size_t line=0;
    const auto next_line=[&buffer](std::size_t at)->std::size_t{
        const auto nl=buffer.find('\n',at);
        return nl==std::string::npos?buffer.size():nl+1;
    };
    while(line<buffer.size()&&std::strncmp(buffer.c_str()+line,"LIGHT",5)!=0)line=next_line(line);
    if(line>=buffer.size())return out;
    line=next_line(next_line(line));   // the "LIGHT :" header and its '{'
    while(line<buffer.size()){
        char name[256]{},type[256]{};
        int id=0;
        std::array<float,12> tm{};
        std::array<float,3> rgb{};
        float intensity=0.0F;
        const int read=std::sscanf(buffer.c_str()+line,
            "%255s %d, %*s %255s %f %f %f  %f %f %f  %f %f %f  %f %f %f,  %f %f %f, %f",
            name,&id,type,&tm[0],&tm[1],&tm[2],&tm[3],&tm[4],&tm[5],&tm[6],&tm[7],&tm[8],&tm[9],&tm[10],&tm[11],
            &rgb[0],&rgb[1],&rgb[2],&intensity);
        if(read!=19)break;
        if(std::strncmp(type,"DIRECT",6)==0&&out.count<4){
            out.directions[out.count]={tm[6],tm[7],tm[8]};
            for(std::size_t c=0;c<3;++c)out.colours[out.count][c]=128.0F*intensity*rgb[c];
            ++out.count;
        }
        line=next_line(line);
    }
    normalise_directions(out);
    return out;
}

FrontendLightSet frontend_default_lights_402d80(){
    FrontendLightSet out;
    out.directions={{{0.0F,0.5F,-1.5F},{1.5F,0.5F,0.5F},{-1.5F,0.5F,0.5F},{0.5F,0.5F,0.5F}}};   // sp+0x480..0x4b0
    const float c0=std::bit_cast<float>(0x3e4ccccdU),c1=std::bit_cast<float>(0x3e99999aU),c2=std::bit_cast<float>(0x3f666666U);
    out.colours={{{c0,c0,c0,0.0F},{c1,c1,c1,0.0F},{c2,c2,c2,0.0F},{0.0F,0.0F,0.0F,0.0F}}};   // a2 +0/+10/+20/+30
    out.count=4;   // port bookkeeping: every row is written
    normalise_directions(out);
    return out;
}

FrontendLightSet frontend_lights_4029b0(std::optional<std::string_view> text){
    return text?parse_frontend_lights_4029b0(*text):frontend_default_lights_402d80();
}

FrontendLights frontend_lights_4053b0(std::optional<std::string_view> manlight_text,std::optional<std::string_view> gunlight_text){
    FrontendLights out{frontend_lights_4029b0(manlight_text),frontend_lights_4029b0(gunlight_text)};
    for(auto& d:out.man.directions){d[0]=-d[0];d[1]=-d[1];}   // Rz(pi)
    return out;
}

std::array<std::int32_t,3> frontend_light_colour(const FrontendLightSet& lights,const std::array<float,3>& normal,
                                                 std::int32_t clamp){
    const float c=std::bit_cast<float>(0xd06e2000U);
    std::array<float,3> sum{};
    for(std::size_t i=0;i<4;++i){
        const auto& l=lights.directions[i];
        const float d=std::max(l[0]*normal[0]+l[1]*normal[1]+l[2]*normal[2],0.02F);
        const float t=static_cast<float>(std::bit_cast<std::int32_t>(d))*16.0F+c;   // itof0, mul, add
        const float bounded=std::clamp(t,-2147483648.0F,2147483520.0F);              // ftoi0 saturates
        const float p=std::bit_cast<float>(static_cast<std::int32_t>(bounded));
        const float e=d+p;
        for(std::size_t k=0;k<3;++k)sum[k]+=lights.colours[i][k]*e;
    }
    std::array<std::int32_t,3> out{};
    for(std::size_t k=0;k<3;++k){
        const float v=std::clamp(sum[k],0.0F,static_cast<float>(clamp));
        out[k]=static_cast<std::int32_t>(std::floor(v));
    }
    return out;
}

}  // namespace omega
