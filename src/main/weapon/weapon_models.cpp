#include "weapon_models.hpp"
#include "texture.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {

[[noreturn]] void fail(const std::string& message){throw std::runtime_error(message);}

// 00128e80's ctype folding (a..z -> A..Z); bytes >= 0x80 are outside its table.
char fold_upper(char c,const char* where){
    const auto u=static_cast<unsigned char>(c);
    if(u>=0x80)fail(std::string(where)+": name byte >= 0x80 (outside the ctype table)");
    return (u>='a'&&u<='z')?static_cast<char>(u-0x20):c;
}

std::string folded(std::string_view name,const char* where){
    std::string out;
    for(const char c:name)out+=fold_upper(c,where);
    return out;
}

std::uint32_t le32(std::span<const std::uint8_t> b,std::size_t at,const char* where){
    if(at+4>b.size())fail(std::string(where)+": read past the data");
    return std::uint32_t(b[at])|(std::uint32_t(b[at+1])<<8)|(std::uint32_t(b[at+2])<<16)|(std::uint32_t(b[at+3])<<24);
}

struct HogMember { std::string name; std::size_t data{},size{}; };

// The HOG reader of 0036d200 / 0036d380 / 0036d4e0: count +4, offset table +8, names +12
// (consecutive NUL strings), data +16; member i spans table[i]..table[i+1] from the data base.
std::vector<HogMember> hog_members(std::span<const std::uint8_t> hog){
    constexpr const char* where="0036d2a0 (HOG member table)";
    const auto count=le32(hog,4,where);
    const auto table=le32(hog,8,where),names=le32(hog,12,where),data=le32(hog,16,where);
    std::vector<HogMember> out;
    std::size_t name=names;
    for(std::uint32_t i=0;i<count;++i){
        HogMember m;
        while(true){
            if(name>=hog.size())fail(std::string(where)+": member name runs past the HOG");
            const auto c=hog[name++];
            if(c==0)break;
            m.name+=static_cast<char>(c);
        }
        const auto a=le32(hog,table+4*i,where),b=le32(hog,table+4*i+4,where);
        if(b<a)fail(std::string(where)+": member offsets decrease");
        m.data=std::size_t(data)+a;m.size=b-a;
        if(m.data+m.size>hog.size())fail(std::string(where)+": member runs past the HOG");
        out.push_back(std::move(m));
    }
    return out;
}

// 00147880's table 00489ce0 (ammo type -> shell record; < 0 none).
constexpr std::array<std::int8_t,32> shell_table_489ce0{0,0,-1,0,1,0,0,0,6,-1,1,4,4,4,2,-1,5,1,1,2,2,2,3,3,5,1,-1,-1,-1,0,0,0};

// 001457c0's table 00489cc0 (db+b8+4k): the names it upper-cases in place (k 1 and 3 point at
// the same string).
constexpr std::array<const char*,7> table_names_489cc0{"gearpack.skl","icon_ammo.skl","icon_vest.skl",
    "icon_ammo.skl","icon_bodycarry.skl","00shells.skl","icon_costume.skl"};
std::string table_name(std::size_t k){return folded(table_names_489cc0.at(k),"001457c0");}

// The record's NUL-terminated name at `offset` (the original copies it with 00129360).
std::string record_name(const WeaponRecord& r,std::size_t offset,const char* where){
    std::string s;
    for(std::size_t o=offset;;++o){
        if(o>=r.bytes.size())fail(std::string(where)+": record name not NUL-terminated within the record");
        if(r.bytes[o]==0)return s;
        s+=static_cast<char>(r.bytes[o]);
    }
}

void put16(GunModel& m,std::size_t o,std::uint16_t v){m.bytes[o]=std::uint8_t(v);m.bytes[o+1]=std::uint8_t(v>>8);m.defined[o]=m.defined[o+1]=true;}
void put8(GunModel& m,std::size_t o,std::uint8_t v){m.bytes[o]=v;m.defined[o]=true;}
void put32(GunModel& m,std::size_t o,std::uint32_t v){for(std::size_t k=0;k<4;++k){m.bytes[o+k]=std::uint8_t(v>>(8*k));m.defined[o+k]=true;}}

}

std::optional<std::size_t> weapon_archive_member(const std::vector<std::string>& names,std::string_view name){
    constexpr const char* where="0036d2a0";
    const auto key=folded(name,where);
    for(std::size_t i=0;i<names.size();++i)if(folded(names[i],where)==key)return i;
    return std::nullopt;
}

bool weapon_model_name_equal(std::string_view a,std::string_view b){
    // 00146b80 / 001468e0: compare byte by byte; '.' or NUL ends a name; a..z (signed) fold.
    std::size_t i=0;
    const auto at=[](std::string_view s,std::size_t k)->char{
        if(k>=s.size())return 0;
        const char c=s[k];
        return c=='.'?0:c;
    };
    while(true){
        char x=at(a,i),y=at(b,i);
        const auto up=[](char c){const auto s=static_cast<signed char>(c);return (s>='a'&&s<='z')?static_cast<char>(s-0x20):c;};
        x=up(x);y=up(y);
        if(x!=y)return false;
        if(x==0)return true;
        ++i;
    }
}

std::vector<std::int32_t> weapon_level_preload_list(std::span<const std::uint8_t> population){
    constexpr const char* where="001457c0 (POP WPN: section)";
    constexpr std::uint32_t tag=0x3a4e5057;   // "WPN:"
    std::vector<std::int32_t> list{0x67};
    std::optional<std::size_t> found;
    for(std::size_t o=0;o+4<=population.size();o+=4){
        if(le32(population,o,where)!=tag)continue;
        // 00178bb0 first tests the reader's cursor, then scans from the base: with several tags the
        // result would depend on the cursor the population reader left.
        if(found)fail(std::string(where)+": more than one WPN: tag (00178bb0's result depends on the reader cursor)");
        found=o;
    }
    if(!found)return list;
    std::size_t at=*found+4;
    const auto read=[&]{const auto v=static_cast<std::int32_t>(le32(population,at,where));at+=4;return v;};
    const auto groups=read();
    for(std::int32_t g=0;g<groups;++g){
        const auto n=read();
        for(std::int32_t k=0;k<n;++k){
            const auto item=read();
            if(std::find(list.begin(),list.end(),item)!=list.end())continue;
            if(list.size()>=128)fail(std::string(where)+": more than 128 items (the stack list 0x70..0x270 overflows)");
            list.push_back(item);
        }
    }
    return list;
}

namespace {
// The texture services 0038cd90 calls, answered from the gun HOG's members: 00380d40 finds
// the image "<name>.TDX" of the context 0036e1b0 opened on the HOG (-1 when absent);
// 00381000 is the default image (ERROR.TDX).
struct LibraryTextures final:GunTextureHost {
    std::function<std::int16_t(const std::string&)> find;
    std::function<std::int16_t()> error;
    void tinit() override{}
    void tload(const std::string&) override{}
    std::int16_t tex(const std::array<std::uint8_t,16>& name,const std::array<bool,16>& defined) override{
        std::string n;
        for(std::size_t k=0;k<16;++k){
            if(!defined[k])fail("00380d40: undefined texture name byte");
            if(name[k]==0)return find(n);
            n+=static_cast<char>(name[k]);
        }
        fail("00380d40: texture name not NUL-terminated within 16 bytes");
    }
    std::int16_t texd() override{return error();}
    void tfree() override{}
};
}

WeaponModelLibrary::WeaponModelLibrary(WeaponDatabase& database,WeaponModelArchive archive,std::vector<std::uint8_t> error_tdx)
    :db_(database),archive_(std::move(archive)),error_tdx_(std::move(error_tdx)){}

WeaponRecord& WeaponModelLibrary::record_at(std::int32_t item){
    if(item<0||static_cast<std::size_t>(item)>=db_.size())
        fail("00145710 / 00145630: item "+std::to_string(item)+" outside the weapon table (the original reads past it)");
    return db_.mutable_record(item);
}

bool WeaponModelLibrary::contains(std::int32_t handle)const{return handle>0&&static_cast<std::size_t>(handle)<=models_.size()&&!models_[static_cast<std::size_t>(handle)-1].freed;}
WeaponModelLibrary::Entry& WeaponModelLibrary::entry(std::int32_t handle){
    if(!contains(handle))fail("weapon model handle "+std::to_string(handle)+" not loaded");
    return models_[static_cast<std::size_t>(handle)-1];
}
const WeaponModelLibrary::Entry& WeaponModelLibrary::entry(std::int32_t handle)const{
    if(!contains(handle))fail("weapon model handle "+std::to_string(handle)+" not loaded");
    return models_[static_cast<std::size_t>(handle)-1];
}
const GunModel& WeaponModelLibrary::model(std::int32_t handle)const{return entry(handle).model;}
std::span<const std::uint8_t> WeaponModelLibrary::hog(std::int32_t handle)const{
    const auto& e=entry(handle);
    if(!e.hog)return {};
    return *e.hog;
}
AttachedModel WeaponModelLibrary::attached(std::int32_t handle)const{
    const auto& m=model(handle);
    AttachedModel a;
    a.item_40a=static_cast<std::int16_t>(m.bytes[0x40a]|(m.bytes[0x40b]<<8));
    if(!m.all_defined(0x40a,3))fail("0038c7d0: model +40a / +40c undefined");
    a.byte_40c=m.bytes[0x40c];
    return a;
}
bool WeaponModelLibrary::flare_411(std::int32_t handle)const{
    const auto& m=model(handle);
    if(!m.defined[0x411])fail("0013ff40: model +411 undefined");
    return m.bytes[0x411]!=0xff;
}
RootVector WeaponModelLibrary::muzzle_130(std::int32_t handle)const{
    const auto& m=model(handle);
    return {m.real(0x130),m.real(0x134),m.real(0x138),m.real(0x13c)};
}

std::int32_t WeaponModelLibrary::allocate(const GunModel& model,const std::shared_ptr<const std::vector<std::uint8_t>>& hog){
    if(models_.size()+1>=0x10000)fail("weapon model handles exhausted (the loadout stand-ins start at 0x10000)");
    models_.push_back({model,hog});
    ++generation_;
    return static_cast<std::int32_t>(models_.size());
}
std::int32_t WeaponModelLibrary::word_41c(std::int32_t handle)const{return static_cast<std::int32_t>(model(handle).word(0x41c));}
void WeaponModelLibrary::set_word_41c(std::int32_t handle,std::int32_t value){put32(entry(handle).model,0x41c,static_cast<std::uint32_t>(value));}

std::int32_t WeaponModelLibrary::table_model(std::size_t k)const{
    if(k>=table_.size())fail("db+b8: table index outside the 7 entries");
    return table_[k];
}

std::optional<GunShellRecord> WeaponModelLibrary::shell_147880(const WeaponRecord& record){
    constexpr const char* where="00147880";
    const auto a1=record.ammo_type_10c();
    if(a1<0)return std::nullopt;
    if(a1>=static_cast<std::int32_t>(shell_table_489ce0.size()))fail(std::string(where)+": ammo type outside the 00489ce0 table");
    const auto n=shell_table_489ce0[static_cast<std::size_t>(a1)];
    if(n<0)return std::nullopt;
    const auto h=table_[5];   // db+cc
    if(h==0)return std::nullopt;
    const auto& m=model(h);
    if(!m.defined[0x40c])fail(std::string(where)+": shells model +40c undefined");
    if(n>=m.bytes[0x40c])return std::nullopt;
    if(!m.all_defined(0x3c0,2))fail(std::string(where)+": shells model +3c0 undefined");
    shell_texture_=static_cast<std::int16_t>(m.bytes[0x3c0]|(m.bytes[0x3c1]<<8));   // 0055de90
    const auto rec=0x2c0+16*static_cast<std::size_t>(n);
    if(!m.all_defined(rec+4,2))fail(std::string(where)+": shell record +4 undefined");
    ++shell_records_;
    return GunShellRecord{m.word(rec),static_cast<std::uint16_t>(m.bytes[rec+4]|(m.bytes[rec+5]<<8))};
}

void WeaponModelLibrary::load_member(GunModel& model,const std::shared_ptr<const std::vector<std::uint8_t>>& hog,
    const std::function<std::vector<std::uint8_t>(std::size_t)>& following){
    LibraryTextures host;
    std::vector<HogMember> members;
    host.find=[&](const std::string& n)->std::int16_t{
        if(members.empty())members=hog_members(*hog);
        const auto key=folded(n+".TDX","00380d40");
        for(const auto& m:members){
            if(folded(m.name,"00380d40")!=key)continue;
            // 0038cd90 opens the gun HOG in memory (0036d540) and the TDX is read in place: a member cut short
            // (ICON_*.HOG's _BODY_* icons) uploads the following .GUN bytes, which 0038cd90 only copies out.
            if(following&&m.size>=64){
                // Checkpoint 132wp1: a gun HOG of a mission WPN.HOG lies in the archive's memory image (0036d4e0: +158 + the
                // header size + the member offset), so an upload past the gun HOG's end (YEMEN1 FADHILPHOTO.HOG's
                // _BODY_PH8_FADH0.TDX, 0x20 bytes) reads the next member's first bytes: its HOG header, which 0036d540
                // only reads. Further than that header is not hosted (0038cd90 may have written the next member).
                const std::span<const std::uint8_t> tdx(hog->data()+m.data,m.size);
                const auto upload=64+std::uint64_t(tdx[34]|tdx[35]<<8)*le32(tdx,56,"003bba10");
                if(upload>m.size&&upload>hog->size()-m.data){
                    const auto need=static_cast<std::size_t>(upload-(hog->size()-m.data));
                    auto image=*hog;
                    const auto after=following(need);
                    if(after.size()<need||after.size()<20||le32(after,0,"003bba10")!=0x40465c42||need>le32(after,16,"003bba10"))
                        fail("003bba10: "+m.name+" uploads "+std::to_string(need)+" bytes past its gun HOG, beyond the next member's header (not hosted)");
                    image.insert(image.end(),after.begin(),after.end());
                    return texture(m.name,tdx_in_place(image,m.data,m.size));
                }
            }
            return texture(m.name,tdx_in_place(*hog,m.data,m.size));
        }
        return -1;
    };
    host.error=[this]{return error_texture();};
    gun_model_load(model,GunHog{*hog,{}},&host);
}

void WeaponModelLibrary::load_mission_archive_146040(const WeaponModelArchive& mission){
    for(std::size_t i=0;i<mission.names.size();++i){
        const auto& name=mission.names[i];
        const auto hog=std::make_shared<const std::vector<std::uint8_t>>(mission.read(i));
        std::function<std::vector<std::uint8_t>(std::size_t)> following;
        if(mission.following)following=[&mission,i](std::size_t count){return mission.following(i,count);};
        bool table=false;
        for(std::size_t k=0;k<table_.size();++k){
            const auto n=table_name(k);
            if(!weapon_model_name_equal(n,name))continue;
            GunModel m;
            gun_model_construct(m,n,static_cast<std::int16_t>(k),0,0,0,0);
            load_member(m,hog,following);
            table_[k]=allocate(m,hog);
            table=true;
        }
        if(table)continue;
        // the temporary model 00146b80 copies (freed by 0036b130 afterwards)
        GunModel temp;
        gun_model_construct(temp,name,0,0,0,0,0);
        load_member(temp,hog,following);
        clone_146b80(temp,hog);
    }
}

void WeaponModelLibrary::preload(std::int32_t item,std::uint8_t keep_139){
    auto& r=record_at(item);
    const auto count=static_cast<std::uint8_t>(r.byte(0x138)+1);
    r.set_byte(0x138,count);
    if(const auto h=r.model_13c())set_word_41c(h,word_41c(h)+1);
    if(count>=2)return;
    r.set_byte(0x139,keep_139);
    if(r.model_13c()!=0)return;
    load_147150(record_name(r,0,"00145710"),0x13c,item,0,r.byte(0xcc),r.byte(0x108));
    if(const auto h=db_.record(item).model_13c())set_word_41c(h,word_41c(h)+1);
}

void WeaponModelLibrary::unload(std::int32_t item,std::uint8_t free){
    auto& r=record_at(item);
    const auto count=static_cast<std::uint8_t>(r.byte(0x138)-1);
    r.set_byte(0x138,count);
    if(count!=0)return;
    const auto h=r.model_13c();
    if(h==0)return;
    set_word_41c(h,word_41c(h)-1);
    if(free!=0&&word_41c(h)<=0){
        std::string name;   // 001456e4: a1 = the model itself, whose +0 is its name ("<record +0>.HOG")
        const auto& m=model(h);
        for(std::size_t o=0;;++o){
            if(o>=0x20)fail("00146460: model name not NUL-terminated within +0..+1f");
            if(!m.defined[o])fail("00146460: model name byte undefined");
            if(m.bytes[o]==0)break;
            name+=static_cast<char>(m.bytes[o]);
        }
        free_146460(name);
    }
}

void WeaponModelLibrary::release_38cc70(std::int32_t handle){
    auto& m=entry(handle).model;
    if(!m.defined[0x418]||!m.defined[0x417])fail("0038cc70: model +417 / +418 undefined");
    // +418 set: the "%s.HOG" archive request is cancelled (0036e140, 003eba50, 003eaa60, 0036e220). The
    // port's 00147150 load completes at once (weapon_models.hpp), so no request is outstanding.
    if(m.bytes[0x418]!=0)put8(m,0x418,0);
    if(static_cast<std::int8_t>(m.bytes[0x417])<0){put8(m,0x417,static_cast<std::uint8_t>(-2));return;}
    // 003813a0(*00539248, tex, 0) per part: +3d0[k] when it differs from +3c0[k], then +3c0[k].
    if(!m.defined[0x40c])fail("0038cc70: model +40c undefined");
    for(std::size_t k=0;k<m.bytes[0x40c];++k){
        if(!m.all_defined(0x3c0+2*k,2)||!m.all_defined(0x3d0+2*k,2))fail("0038cc70: part texture id undefined");
        const auto lod0=static_cast<std::int16_t>(m.bytes[0x3c0+2*k]|(m.bytes[0x3c1+2*k]<<8));
        const auto lod1=static_cast<std::int16_t>(m.bytes[0x3d0+2*k]|(m.bytes[0x3d1+2*k]<<8));
        if(lod1!=lod0)release_texture_3813a0(lod1);
        release_texture_3813a0(lod0);
    }
    put8(m,0x417,static_cast<std::uint8_t>(-3));
}

void WeaponModelLibrary::free_146460(const std::string& name){
    // Pass 1: every record naming the model (+0 -> +13c, +120 -> +11c) adds its model's +41c; a model whose
    // +418 is 0 ends it (nothing freed); a positive sum frees nothing.
    std::int32_t sum=0;
    for(std::size_t j=0;j<db_.size();++j){
        const auto& r=db_.record(static_cast<std::int32_t>(j));
        for(const auto [at,slot]:{std::pair<std::size_t,std::size_t>{0,0x13c},{0x120,0x11c}}){
            if(!weapon_model_name_equal(record_name(r,at,"00146460"),name))continue;
            const auto h=static_cast<std::int32_t>(r.word(slot));
            if(!contains(h))fail("00146460: record "+std::to_string(j)+" names the model with no model at its slot (reads +41c of 0)");
            const auto& m=model(h);
            if(!m.defined[0x418])fail("00146460: model +418 undefined");
            sum+=word_41c(h);
            if(m.bytes[0x418]==0)return;
        }
    }
    if(sum>0)return;
    // Pass 2: the first match is released (0038cc70); each match's model is freed (0036b130(0, model,
    // "weapon.cc", 0x270 / 0x279)) and its slot cleared.
    std::int32_t freed=0;
    for(std::size_t j=0;j<db_.size();++j){
        auto& r=db_.mutable_record(static_cast<std::int32_t>(j));
        for(const auto [at,slot]:{std::pair<std::size_t,std::size_t>{0,0x13c},{0x120,0x11c}}){
            if(!weapon_model_name_equal(record_name(r,at,"00146460"),name))continue;
            const auto h=static_cast<std::int32_t>(r.word(slot));
            if(!contains(h))fail("00146460: record "+std::to_string(j)+" names the model with no model at its slot");
            if(freed==0)release_38cc70(h);
            auto& e=models_[static_cast<std::size_t>(h)-1];
            e.freed=true;e.hog.reset();
            r.set_word(slot,0);
            ++freed;++models_freed_;
            ++generation_;
        }
    }
}

void WeaponModelLibrary::level_preload(std::span<const std::int32_t> items,const WeaponModelArchive* mission){
    table_.fill(0);
    if(mission)load_mission_archive_146040(*mission);
    for(const auto item:items)preload(item,1);
    // 00145c30: a table entry still 0 gets a model that is constructed but never loaded
    // (+417 stays 0xff: not drawn; 00147880 finds +40c 0).
    for(std::size_t k=0;k<table_.size();++k){
        if(table_[k]!=0)continue;
        GunModel m;
        gun_model_construct(m,table_name(k),static_cast<std::int16_t>(k),0,0,0,0xff);
        table_[k]=allocate(m,nullptr);
        set_word_41c(table_[k],word_41c(table_[k])+1);
    }
    for(std::size_t i=0;i<db_.size();++i){
        auto& r=db_.mutable_record(static_cast<std::int32_t>(i));
        const auto name=record_name(r,0x120,"001457c0");
        if(name.empty())continue;
        if(name.find(".skl")==std::string::npos&&name.find(".SKL")==std::string::npos)continue;
        if(r.word(0x11c)!=0)continue;
        load_147150(name,0x11c,static_cast<std::int32_t>(i),1,0xff,0xff);
    }
}

void WeaponModelLibrary::load_147150(const std::string& name,std::size_t slot,std::int32_t item,std::uint8_t t0,std::uint8_t t1,std::uint8_t t2){
    record_at(item).set_word(slot,0);
    const auto dot=name.find('.');
    const std::string buf=(dot==std::string::npos?name:name.substr(0,dot))+".HOG";
    const auto member=weapon_archive_member(archive_.names,buf);
    if(!member)return;   // 0036def0 fails: nothing loaded, the slot stays 0
    // 0x147218: 00147880 gives 0038d530 its a3 (+404 / +408).
    const auto shell=item>=0?shell_147880(record_at(item)):std::nullopt;
    GunModel source;
    gun_model_construct(source,buf,static_cast<std::int16_t>(item),t0,1,t1,t2,shell);
    // 0038d530's slot store, then 00146b80's clones replace it in every record naming the HOG.
    clone_146b80(source,nullptr);
    auto hog=std::make_shared<const std::vector<std::uint8_t>>(archive_.read(*member));
    // 003eae60's completion 00147110 -> 00146fc0: 0038cd90 on the source, then 001468e0.
    load_member(source,hog);
    propagate_1468e0(source,hog);
}

std::int32_t WeaponModelLibrary::load_item_147150(const std::string& resource){
    const auto dot=resource.find('.');
    const std::string buf=(dot==std::string::npos?resource:resource.substr(0,dot))+".HOG";
    const auto member=weapon_archive_member(archive_.names,buf);
    if(!member)return 0;
    GunModel source;
    gun_model_construct(source,buf,-1,0,1,0,0,std::nullopt);
    clone_146b80(source,nullptr);
    auto hog=std::make_shared<const std::vector<std::uint8_t>>(archive_.read(*member));
    load_member(source,hog);
    const auto handle=allocate(source,hog);
    propagate_1468e0(source,hog);
    return handle;
}

void WeaponModelLibrary::clone_146b80(const GunModel& source,const std::shared_ptr<const std::vector<std::uint8_t>>& hog){
    std::string name;
    for(std::size_t o=0;o<0x20&&source.bytes[o]!=0;++o)name+=static_cast<char>(source.bytes[o]);
    for(std::size_t j=0;j<db_.size();++j){
        auto& r=db_.mutable_record(static_cast<std::int32_t>(j));
        if(weapon_model_name_equal(record_name(r,0,"00146b80"),name)){
            const auto h=allocate(source,hog);
            r.set_word(0x13c,h);
            auto& m=entry(h).model;
            put16(m,0x40a,static_cast<std::uint16_t>(j));
            put8(m,0x40d,r.byte(0xcc));
            put8(m,0x40e,r.byte(0x108));
            if(const auto s=shell_147880(r)){put32(m,0x404,s->chunk_start);put16(m,0x408,s->qwords);}
        }
        if(weapon_model_name_equal(record_name(r,0x120,"00146b80"),name)){
            const auto h=allocate(source,hog);
            r.set_word(0x11c,h);
            auto& m=entry(h).model;
            put16(m,0x40a,static_cast<std::uint16_t>(j));
            put8(m,0x419,1);
        }
    }
}

void WeaponModelLibrary::propagate_1468e0(const GunModel& source,const std::shared_ptr<const std::vector<std::uint8_t>>& hog){
    std::string name;
    for(std::size_t o=0;o<0x20&&source.bytes[o]!=0;++o)name+=static_cast<char>(source.bytes[o]);
    const auto copy_into=[&](std::int32_t h){
        auto& e=entry(h);
        GunModel m=source;
        for(std::size_t k=0;k<4;++k){m.bytes[0x41c+k]=e.model.bytes[0x41c+k];m.defined[0x41c+k]=e.model.defined[0x41c+k];}
        e.model=m;e.hog=hog;
        ++generation_;
        return std::ref(e.model);
    };
    for(std::size_t j=0;j<db_.size();++j){
        auto& r=db_.mutable_record(static_cast<std::int32_t>(j));
        if(weapon_model_name_equal(record_name(r,0,"001468e0"),name)){
            const auto h=r.model_13c();
            if(h==0)fail("001468e0: a record naming the HOG has no model (00146b80 cloned one into every such record)");
            GunModel& m=copy_into(h);
            put16(m,0x40a,static_cast<std::uint16_t>(j));
            put8(m,0x40d,r.byte(0xcc));
            put8(m,0x40e,r.byte(0x108));
            if(const auto s=shell_147880(r)){put32(m,0x404,s->chunk_start);put16(m,0x408,s->qwords);}   // 0x146a2c
        }
        if(weapon_model_name_equal(record_name(r,0x120,"001468e0"),name)){
            const auto h=r.word(0x11c);
            if(h==0)fail("001468e0: a record naming the HOG at +120 has no +11c model");
            GunModel& m=copy_into(h);
            put16(m,0x40a,static_cast<std::uint16_t>(j));
            put8(m,0x419,1);
        }
    }
}

std::int16_t WeaponModelLibrary::texture(const std::string& member,const std::vector<std::uint8_t>& tdx){
    // 00380d40 -> 0037fea0 makes a new record (reference count 1) for every call. The port shares one live
    // record per member, adding a reference instead: each id is released once per 00380d40 call, so the
    // counts reach 0 together.
    const auto key=folded(member,"00380d40");
    for(std::size_t i=0;i<textures_.size();++i){
        auto& t=textures_[i];
        if(t.refs>0&&folded(t.member,"00380d40")==key){++t.refs;return static_cast<std::int16_t>(i);}
    }
    if(textures_.size()>=0x7fff)fail("00380d40: texture ids exhausted");
    textures_.push_back({member,tdx,1});
    ++generation_;
    return static_cast<std::int16_t>(textures_.size()-1);
}

std::int16_t WeaponModelLibrary::error_texture(){
    // 00381000 -> 00380a40: the live ERROR.TDX record by name gains a reference; else it is loaded (count 1).
    if(error_index_&&textures_[static_cast<std::size_t>(*error_index_)].refs>0){
        ++textures_[static_cast<std::size_t>(*error_index_)].refs;
        return *error_index_;
    }
    if(textures_.size()>=0x7fff)fail("00381000: texture ids exhausted");
    textures_.push_back({"ERROR.TDX",error_tdx_,1});
    ++generation_;
    error_index_=static_cast<std::int16_t>(textures_.size()-1);
    return *error_index_;
}

void WeaponModelLibrary::release_texture_3813a0(std::int16_t id){
    // 003813a0 indexes the record table +8 with the id unchecked; the port's ids are this list's.
    if(id<0||static_cast<std::size_t>(id)>=textures_.size())fail("003813a0: texture id "+std::to_string(id)+" outside the records (the original reads past the table)");
    ++texture_releases_;
    auto& t=textures_[static_cast<std::size_t>(id)];
    if(t.refs<=0)return;   // the slot's record is null (destroyed): nothing
    if(--t.refs>0)return;
    // 00380280(record, 0, 1, "texman.cc", 0x39e), the slot cleared and pushed on the free list (+434).
    t.tdx.clear();t.tdx.shrink_to_fit();
    ++textures_destroyed_;
    ++generation_;
}

}
