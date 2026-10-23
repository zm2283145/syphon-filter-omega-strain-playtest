#include "morph_resources.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <string>

namespace omega {
namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
float value(float x){check(std::isfinite(x)&&(!x||std::isnormal(x)),"Invalid morph resource arithmetic");return x;}
float product(float a,float b){return value(a*b);}
float sum(float a,float b){return value(a+b);}
void validate(const MorphGripMap& map){
    for(unsigned hand=0;hand<2;++hand){
        check(map.shape_counts[hand]>0&&map.shape_counts[hand]<=128,"Grip shape count outside signed-index domain");
        for(auto index:map.shapes[hand])check(index<map.shape_counts[hand],"Grip map shape outside resource");
    }
}
std::uint8_t mapped(const MorphGripMap& map,unsigned hand,unsigned index){
    check(index<24,"Equipment grip lookup outside recovered table");return map.shapes[hand][index];
}
void matrix(const SkinMatrix& m){
    for(auto x:m)value(x);
    check(m[3]==0&&m[7]==0&&m[11]==0&&m[15]==1,"Gaze transform must be affine");
}
SkinMatrix compose(const SkinMatrix& a,const SkinMatrix& b){
    SkinMatrix result{};
    for(unsigned c=0;c<4;++c)for(unsigned r=0;r<4;++r){
        float x=product(a[r],b[c*4]);
        for(unsigned k=1;k<4;++k)x=sum(x,product(a[k*4+r],b[c*4+k]));
        result[c*4+r]=x;
    }
    return result;
}
}
MorphGripMap read_morph_grip_map(std::istream& input){
    auto expect=[&](const char* expected){std::string token;check(bool(input>>token)&&token==expected,"Invalid grip map token");};
    auto integer=[&](unsigned maximum){std::string token;check(bool(input>>token)&&!token.empty()&&token.size()<=10,"Missing grip map integer");
        unsigned n=0;for(char c:token){check(c>='0'&&c<='9',"Invalid grip map integer");const unsigned digit=c-'0';
            check(n<=maximum/10&&(n!=maximum/10||digit<=maximum%10),"Grip map integer out of range");n=n*10+digit;}return n;};
    expect("OMEGA_MORPH_GRIP_MAP");check(integer(1)==1,"Unsupported grip map version");MorphGripMap result;
    for(unsigned hand=0;hand<2;++hand){expect("HAND");check(integer(1)==hand,"Grip hands out of order");result.shape_counts[hand]=static_cast<std::uint16_t>(integer(128));
        for(auto& index:result.shapes[hand])index=static_cast<std::uint8_t>(integer(127));}
    expect("END");std::string extra;check(!(input>>extra),"Trailing grip map input");validate(result);return result;
}
std::uint8_t morph_shape_index(const ShapeNames& names,std::string_view name){
    check(names.size()<=256,"Morph shape table larger than the byte index domain");
    check(name.find('\0')==std::string_view::npos,"Morph shape lookup name with an embedded NUL");
    for(std::size_t i=0;i<names.size();++i){
        const auto& entry=names[i];std::size_t length=0;
        while(length<entry.size()&&entry[length]!=0)++length;
        check(length<entry.size(),"Morph shape name without a NUL in its 16 bytes (003ffa70 strcmp)");
        if(length==name.size()&&std::equal(name.begin(),name.end(),entry.begin(),[](char a,std::uint8_t b){return std::uint8_t(a)==b;}))
            return static_cast<std::uint8_t>(i);
    }
    return 0;
}
MorphGripMap morph_grip_map(std::span<const std::string> keys,const std::array<const ShapeNames*,2>& hands){
    check(keys.size()==24,"Grip key table is not the 24 names at 00493af0");
    MorphGripMap result;
    for(unsigned hand=0;hand<2;++hand){
        check(hands[hand]!=nullptr,"Grip map hand table missing (00404060 loads both ?HAND?.BIN members)");
        const auto& names=*hands[hand];
        check(!names.empty()&&names.size()<=128,"Grip hand table count outside signed-index domain");
        result.shape_counts[hand]=static_cast<std::uint16_t>(names.size());
        for(std::size_t k=0;k<24;++k)result.shapes[hand][k]=morph_shape_index(names,keys[k]);
    }
    validate(result);return result;
}
MorphCurve decode_morph_curve(std::span<const std::uint8_t> bytes){
    check(!bytes.empty()&&bytes.size()%8==0,"Morph curve is not whole 8-byte keys");
    MorphCurve curve;
    for(std::size_t o=0;o<bytes.size();o+=8){
        std::uint32_t frame=0,bits=0;
        for(unsigned i=0;i<4;++i){frame|=std::uint32_t(bytes[o+i])<<(8*i);bits|=std::uint32_t(bytes[o+4+i])<<(8*i);}
        const float v=std::bit_cast<float>(bits);check(std::isfinite(v),"Morph curve value is not finite");
        curve.push_back({std::bit_cast<std::int32_t>(frame),v});
    }
    return curve;
}
void initialize_equipment_grips(MorphHostState& state,const MorphGripMap& map,const MorphEquipment& equipment){
    validate(map);auto grips=state.grips;
    for(unsigned hand:{1U,0U}){
        auto& grip=grips[hand];
        if(!equipment[hand]){grip.primary_weight=grip.secondary_weight=0;continue;}
        const auto index=equipment[hand]->grip;
        grip.primary_shape=mapped(map,hand,index);grip.secondary_shape=mapped(map,hand,unsigned(index)+1);
        grip.primary_weight=1;grip.secondary_weight=0;
    }
    if(equipment[0]){
        // LBU compared with full-width -1 never skips this original override.
        const auto index=mapped(map,1,equipment[0]->other_hand_grip);
        grips[1].primary_shape=grips[1].secondary_shape=index;
        grips[1].primary_weight=1;grips[1].secondary_weight=0;
    }
    state.grips=grips;
}
std::array<std::optional<std::uint8_t>,2> equipment_secondary_grips(const MorphHostState& state,
    const MorphGripMap& map,const MorphEquipment& equipment){
    validate(map);std::array<std::optional<std::uint8_t>,2> result;
    for(unsigned hand=0;hand<2;++hand)if(equipment[hand]){
        // Only the counter-one branch dereferences the mapping again.
        result[hand]=state.grips[hand].counter==1?mapped(map,hand,unsigned(equipment[hand]->grip)+1):state.grips[hand].secondary_shape;
    }
    return result;
}
SkinMatrix root_placement_matrix(const RootParentTransform& placement){
    const auto& q=placement.orientation;for(float x:q)value(x);for(float x:placement.position)value(x);
    RootVector doubled{},x{},y{},z{};
    for(unsigned i=0;i<4;++i)doubled[i]=sum(q[i],q[i]);
    for(unsigned i=0;i<4;++i){x[i]=product(q[i],doubled[0]);y[i]=product(q[i],doubled[1]);z[i]=product(q[i],doubled[2]);}
    auto diagonal=[](float a,float b){return value(value(1.0F-a)-b);};
    return {diagonal(y[1],z[2]),sum(x[1],z[3]),value(x[2]-y[3]),0,
        value(y[0]-z[3]),diagonal(x[0],z[2]),sum(y[2],x[3]),0,
        sum(z[0],y[3]),value(z[1]-x[3]),diagonal(x[0],y[1]),0,
        placement.position[0],placement.position[1],placement.position[2],placement.position[3]};
}
std::optional<std::array<float,3>> resolve_morph_gaze(const SkinMatrix& root_world,
    const std::optional<SkinMatrix>& node14_model,const std::optional<RootVector>& live_target,const RootVector& fallback){
    // The actor's outer gate bypasses the helper entirely without either input.
    if(!live_target&&fallback[3]==0)return std::nullopt;
    const auto target=live_target?*live_target:fallback;for(auto x:target)value(x);
    matrix(root_world);SkinMatrix head=root_world;
    if(node14_model){matrix(*node14_model);head=compose(root_world,*node14_model);}
    // Original 0013eca0 is a basis transpose and transformed negative position,
    // not a generic affine inverse; preserve that graph without renormalizing.
    SkinMatrix inverse=head;
    for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)inverse[c*4+r]=head[r*4+c];
    if(head[12]!=0||head[13]!=0||head[14]!=0){
        for(unsigned r=0;r<3;++r){float x=product(inverse[r],value(0.0F-head[12]));
            x=sum(x,product(inverse[4+r],value(0.0F-head[13])));
            inverse[12+r]=sum(x,product(inverse[8+r],value(0.0F-head[14])));}
    }
    std::array<float,3> result{};
    for(unsigned r=0;r<3;++r){float x=product(inverse[r],target[0]);
        for(unsigned k=1;k<4;++k)x=sum(x,product(inverse[k*4+r],target[k]));
        result[r]=x;}
    return result;
}
RootVector component_placed_vector(const SkinMatrix& component,const RootVector& local){
    RootVector result{};
    for(unsigned r=0;r<4;++r){
        float x=product(component[r],value(local[0]));
        for(unsigned k=1;k<4;++k)x=sum(x,product(component[k*4+r],value(local[k])));
        result[r]=x;
    }
    return result;
}
RootVector skeleton_node_world_position(const SkinMatrix& component,const SkinMatrix& node){
    RootVector result{};
    for(unsigned r=0;r<4;++r){
        float x=product(component[r],value(node[12]));
        for(unsigned k=1;k<4;++k)x=sum(x,product(component[k*4+r],value(node[12+k])));
        result[r]=x;
    }
    return result;
}
std::array<RootVector,4> skeleton_node_world_matrix(const SkinMatrix& component,const SkinMatrix& node){
    std::array<RootVector,4> rows{};
    for(unsigned row=0;row<4;++row)
        for(unsigned r=0;r<4;++r){
            float x=product(component[r],value(node[row*4]));
            for(unsigned k=1;k<4;++k)x=sum(x,product(component[k*4+r],value(node[row*4+k])));
            rows[row][r]=x;
        }
    return rows;
}
std::array<RootVector,4> published_skeleton_node_matrix(const RootParentTransform& published,
    std::span<const SkinMatrix> nodes,int node){
    check(node>=0&&static_cast<std::size_t>(node)<nodes.size(),"Skeleton node outside the model's node collection");
    return skeleton_node_world_matrix(root_placement_matrix(published),nodes[static_cast<std::size_t>(node)]);
}
RootVector published_skeleton_node_position(const RootParentTransform& published,
    std::span<const SkinMatrix> nodes,int node){
    check(node>=0&&static_cast<std::size_t>(node)<nodes.size(),"Skeleton node outside the model's node collection");
    return skeleton_node_world_position(root_placement_matrix(published),nodes[static_cast<std::size_t>(node)]);
}
}
