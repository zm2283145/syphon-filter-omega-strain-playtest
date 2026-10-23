#include "attachment_transforms.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <utility>

namespace omega {
namespace {
void check(bool condition,const char* message){if(!condition)throw std::runtime_error(message);}
float finite(float x){check(std::isfinite(x)&&(!x||std::isnormal(x)),"Invalid attachment arithmetic");return x;}
float mul(float a,float b){return finite(a*b);}float add(float a,float b){return finite(a+b);}
void validate(const SkinMatrix& matrix){for(float x:matrix)finite(x);check(matrix[3]==0&&matrix[7]==0&&matrix[11]==0&&matrix[15]==1,"Attachment matrix must be affine");}
SkinMatrix compose(const SkinMatrix& parent,const SkinMatrix& local){
    SkinMatrix result{};
    for(unsigned column=0;column<4;++column)for(unsigned row=0;row<4;++row){
        float value=mul(parent[row],local[column*4]);
        for(unsigned k=1;k<4;++k)value=add(value,mul(parent[k*4+row],local[column*4+k]));
        result[column*4+row]=value;
    }
    return result;
}
SkinMatrix inverse(const SkinMatrix& matrix){
    SkinMatrix result=matrix;
    for(unsigned c=0;c<3;++c)for(unsigned r=0;r<3;++r)result[c*4+r]=matrix[r*4+c];
    if(matrix[12]!=0||matrix[13]!=0||matrix[14]!=0){
        for(unsigned r=0;r<3;++r){float value=mul(result[r],finite(0.0F-matrix[12]));
            value=add(value,mul(result[4+r],finite(0.0F-matrix[13])));
            result[12+r]=add(value,mul(result[8+r],finite(0.0F-matrix[14])));}
    }
    return result;
}
}
AttachmentNodeId AttachmentTransforms::add(const SkinMatrix& local){
    validate(local);check(nodes_.size()<4096,"Attachment node limit");
    const auto id=nodes_.size();nodes_.push_back({local,local,std::nullopt,{},true});return id;
}
const AttachmentTransformNode& AttachmentTransforms::node(AttachmentNodeId id)const{
    check(id<nodes_.size(),"Attachment node index");return nodes_[id];
}
void AttachmentTransforms::dirty(AttachmentNodeId id){
    std::vector<AttachmentNodeId> pending{id};
    while(!pending.empty()){const auto next=pending.back();pending.pop_back();auto& n=nodes_[next];
        if(n.dirty)continue;n.dirty=true;pending.insert(pending.end(),n.children.begin(),n.children.end());}
}
void AttachmentTransforms::update(AttachmentNodeId id){
    (void)node(id);std::vector<AttachmentNodeId> chain;
    auto current=std::optional<AttachmentNodeId>{id};
    while(current&&nodes_[*current].dirty){chain.push_back(*current);current=nodes_[*current].parent;}
    for(auto it=chain.rbegin();it!=chain.rend();++it){auto& n=nodes_[*it];
        n.world=n.parent?compose(nodes_[*n.parent].world,n.local):n.local;n.dirty=false;}
}
SkinMatrix AttachmentTransforms::world(AttachmentNodeId id){
    auto next=*this;next.update(id);const auto result=next.nodes_[id].world;*this=std::move(next);return result;
}
void AttachmentTransforms::set_local(AttachmentNodeId id,const SkinMatrix& local){
    (void)node(id);validate(local);auto next=*this;next.nodes_[id].local=local;next.dirty(id);*this=std::move(next);
}
void AttachmentTransforms::write_world(AttachmentNodeId id,const SkinMatrix& matrix){
    auto& n=nodes_[id];
    if(n.parent){update(*n.parent);n.local=compose(inverse(nodes_[*n.parent].world),matrix);}
    else n.local=matrix;
    dirty(id);
}
void AttachmentTransforms::set_world(AttachmentNodeId id,const SkinMatrix& matrix){
    (void)node(id);validate(matrix);auto next=*this;next.write_world(id,matrix);*this=std::move(next);
}
void AttachmentTransforms::unlink(AttachmentNodeId id,bool preserve_world){
    auto& n=nodes_[id];if(!n.parent)return;
    update(id);const auto saved=n.world;auto& siblings=nodes_[*n.parent].children;
    const auto found=std::find(siblings.begin(),siblings.end(),id);
    check(found!=siblings.end(),"Attachment parent/child invariant");*found=siblings.back();siblings.pop_back();n.parent.reset();
    if(preserve_world)write_world(id,saved);else dirty(id);
}
void AttachmentTransforms::detach(AttachmentNodeId child,bool preserve_world){
    (void)node(child);auto next=*this;next.unlink(child,preserve_world);*this=std::move(next);
}
void AttachmentTransforms::attach(AttachmentNodeId child,std::optional<AttachmentNodeId> parent,bool preserve_world){
    (void)node(child);if(parent)(void)node(*parent);if(nodes_[child].parent==parent)return;
    for(auto ancestor=parent;ancestor;ancestor=nodes_[*ancestor].parent)check(*ancestor!=child,"Attachment parent cycle");
    auto next=*this;next.unlink(child,true);
    if(parent){
        next.update(child);const auto prior=next.nodes_[child].world;
        next.nodes_[child].parent=parent;next.nodes_[*parent].children.push_back(child);
        if(preserve_world)next.write_world(child,prior);else next.dirty(child);
    }
    *this=std::move(next);
}
}
