#include "pickup_menu_play.hpp"

#include <sstream>
#include <stdexcept>

namespace omega {
namespace {
[[noreturn]] void fail(const std::string& what){throw std::runtime_error("Pickup menu play: "+what);}
std::string hex(std::uint32_t v){std::ostringstream s;s<<std::hex<<v;return s.str();}
}

PickupMenuScreenHost::PickupMenuScreenHost(IeScreen& screen,const std::array<float,4>& pickup_colour,Strings strings,
                                           std::optional<std::string> player_name,PickupMenuPlayState& state)
    :screen_(screen),state_(state),colour_(pickup_colour),strings_(std::move(strings)),player_name_(std::move(player_name)){}

IeElement& PickupMenuScreenHost::element(std::uint32_t handle,const char* where){
    if(handle==0)fail(std::string(where)+": a null element (the original dereferences it)");
    return screen_.element_at(static_cast<int>(handle-1));
}
IeTextObject& PickupMenuScreenHost::text(std::uint32_t handle,const char* where){
    if(handle==0)fail(std::string(where)+": a null text object (the original dereferences it)");
    return screen_.text_object(static_cast<int>(handle-1));
}
PickupMenuPreview& PickupMenuScreenHost::preview(std::uint32_t token,const char* where){
    const auto it=state_.previews.find(token);
    if(it==state_.previews.end())fail(std::string(where)+": "+hex(token)+" is not a 00259420 preview of the menu");
    if(it->second.released)fail(std::string(where)+": the preview "+hex(token)+" was released (003cb230)");
    return it->second;
}

std::uint32_t PickupMenuScreenHost::find_child(std::uint32_t node,std::string_view name){
    if(node==0)fail("003eeb30: a null node");
    const int c=screen_.find_child(static_cast<int>(node-1),name);
    return c<0?0U:static_cast<std::uint32_t>(c+1);
}
std::uint32_t PickupMenuScreenHost::create_element(std::uint32_t node){
    if(node==0)fail("003ef010: a null node");
    return static_cast<std::uint32_t>(screen_.create_element(static_cast<int>(node-1))+1);
}
std::uint32_t PickupMenuScreenHost::element_node(std::uint32_t e){
    return static_cast<std::uint32_t>(element(e,"element +70").node+1);
}
std::uint32_t PickupMenuScreenHost::element_text(std::uint32_t e){
    const int t=element(e,"element +74").text;
    return t<0?0U:static_cast<std::uint32_t>(t+1);
}
void PickupMenuScreenHost::evaluate(std::uint32_t e,float time){
    element(e,"003f45a0");
    screen_.evaluate_element(static_cast<int>(e-1),time);
}
void PickupMenuScreenHost::fade(std::uint32_t e,float time){
    element(e,"003f44e0");
    screen_.fade_element(static_cast<int>(e-1),time);
}
void PickupMenuScreenHost::set_text(std::uint32_t e,std::string_view value){
    const int t=element(e,"003e7ec0").text;
    if(t<0)fail("003e7ec0: an element without a text object +74");
    screen_.set_text(t,std::string(value));
}
std::string PickupMenuScreenHost::get_string(std::string_view key){
    if(!strings_)fail("003fdb50: no string table");
    const auto r=strings_(key);
    if(!r)fail("003fdb50: \""+std::string(key)+"\" is not resolved (a NULL text)");
    return *r;
}
std::uint32_t PickupMenuScreenHost::player(std::uint32_t list,std::int32_t index){
    if(list!=kPickupMenuPlayerList||index!=-1)fail("001314f0: only the local player (004ffb50, -1) is hosted");
    return player_token;
}
std::string PickupMenuScreenHost::player_name(std::uint32_t p){
    if(p!=player_token)fail("00336170: not the local player");
    if(!player_name_)fail("00336170: the local agent record's name is not supplied");
    return *player_name_;
}
void PickupMenuScreenHost::text_byte(std::uint32_t t,std::uint32_t offset,std::uint8_t value){
    auto& o=text(t,"text byte");
    switch(offset){
    case 0x10:o.byte_10=value;break;
    case 0x11:o.byte_11=value;break;
    case 0x5b:o.byte_5b=value;break;
    default:fail("text byte +"+hex(offset)+" is not modelled");
    }
}
void PickupMenuScreenHost::set_colour(std::uint32_t t,std::uint32_t colour){
    if(colour!=kPickupMenuTextColour)fail("003d0810: a colour block other than 004f7e70");
    ie_text_colour(text(t,"003d0810"),colour_);
}
void PickupMenuScreenHost::set_visible(std::uint32_t node,std::int32_t a1,std::int32_t a2){
    if(node==0)fail("003eebe0: a null node");
    screen_.set_visible(static_cast<int>(node-1),static_cast<std::uint8_t>(a1),a2!=0);
}
std::uint32_t PickupMenuScreenHost::allocate(std::uint32_t size,std::string_view file,std::uint32_t line){
    if(size!=kPickupMenuPreviewSize)fail("0036b180: an allocation of "+hex(size)+" bytes");
    const auto block=pickup_menu_block_base+state_.blocks++;
    state_.notes.push_back("0036b180(0x68, \""+std::string(file)+"\", 0x"+hex(line)+") -> block "+hex(block));
    return block;
}
std::uint32_t PickupMenuScreenHost::construct_preview(std::uint32_t block){
    const auto token=pickup_menu_preview_base+state_.objects++;
    PickupMenuPreview p;p.block=block;
    state_.previews.emplace(token,p);
    state_.notes.push_back("00259420 preview "+hex(token)+" (vtable 004dc4a0, +60 = 0, +64 = 7; 0040c240(0xf5) and 003cf0e0 recorded)");
    return token;
}
void PickupMenuScreenHost::post_sound(std::string_view name,std::uint32_t target,std::int32_t flag){
    if(target!=kPickupMenuPlayerList)fail("003c9420: a sound message to "+hex(target)+" (only gameSound 004ffb50 is hosted)");
    state_.sounds.push_back({std::string(name),flag,0,{}});
    state_.notes.push_back("sound "+std::string(name)+" (001ca010(msg, 3, name) -> 003c9420("+hex(target)+", "+std::to_string(flag)+"))");
}
void PickupMenuScreenHost::release_preview(std::uint32_t token,float seconds){
    preview(token,"003cb230").released=true;
    std::ostringstream s;s<<"003cb230 preview "<<hex(token)<<" ("<<seconds<<")";
    state_.notes.push_back(s.str());
}
void PickupMenuScreenHost::remove_element(std::uint32_t e){
    element(e,"003e8eb0");
    screen_.remove_element(static_cast<int>(e-1));
}
void PickupMenuScreenHost::set_preview(std::uint32_t token,std::uint32_t word_60,std::uint8_t byte_64){
    auto& p=preview(token,"00259420 +60 / +64");
    p.model_60=word_60;p.byte_64=byte_64;
}
void PickupMenuScreenHost::set_element_data(std::uint32_t e,std::uint32_t data){
    element(e,"003e7eb0");
    if(data!=0&&!state_.previews.count(data))fail("003e7eb0: element data "+hex(data)+" is not a preview of the menu");
    screen_.set_element_data(static_cast<int>(e-1),data);
}
void PickupMenuScreenHost::anim_range(std::uint32_t e,float from,float to,std::int32_t loop){
    ie_anim_range(element(e,"003f4510").anim,from,to,static_cast<std::uint8_t>(loop));
}
void PickupMenuScreenHost::positional_sound(std::uint32_t descriptor,const RootVector& position){
    state_.sounds.push_back({std::string(),0,descriptor,position});
    std::ostringstream s;
    s<<"sound "<<hex(descriptor)<<" at ("<<position[0]<<", "<<position[1]<<", "<<position[2]
     <<") (0020f860, +20 = 1, 003c9420(004ffb50, 0))";
    state_.notes.push_back(s.str());
}

}
