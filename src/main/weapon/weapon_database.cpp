#include "weapon_database.hpp"
#include "binary.hpp"
#include <bit>
#include <cstring>
#include <stdexcept>

namespace omega {
namespace {
void check(std::size_t offset,std::size_t size){
    if(offset>weapon_record_size||size>weapon_record_size-offset)throw std::runtime_error("Weapon record field outside the record");
}
// 00147400 (00147530..001475a8): mult by 0x66666667, mfhi, sra 1, plus the sign
// bit: signed division by 5 toward zero.
std::int32_t divide_by_5(std::int32_t value){return value/5;}
}

std::int32_t WeaponRecord::word(std::size_t offset) const{
    check(offset,4);
    std::uint32_t v{};std::memcpy(&v,bytes.data()+offset,4);
    return static_cast<std::int32_t>(v);
}
std::uint8_t WeaponRecord::byte(std::size_t offset) const{check(offset,1);return bytes[offset];}
float WeaponRecord::real(std::size_t offset) const{return std::bit_cast<float>(static_cast<std::uint32_t>(word(offset)));}
std::string WeaponRecord::text(std::size_t offset,std::size_t limit) const{
    check(offset,limit);
    std::string out;
    for(std::size_t i=0;i<limit&&bytes[offset+i]!=0;++i)out.push_back(static_cast<char>(bytes[offset+i]));
    return out;
}
void WeaponRecord::set_word(std::size_t offset,std::int32_t value){
    check(offset,4);const auto v=static_cast<std::uint32_t>(value);std::memcpy(bytes.data()+offset,&v,4);
}
void WeaponRecord::set_byte(std::size_t offset,std::uint8_t value){check(offset,1);bytes[offset]=value;}
void WeaponRecord::set_real(std::size_t offset,float value){set_word(offset,static_cast<std::int32_t>(std::bit_cast<std::uint32_t>(value)));}
void WeaponRecord::set_text(std::size_t offset,std::size_t limit,const std::string& value){
    check(offset,limit);
    if(value.size()>=limit)throw std::runtime_error("Weapon record text does not fit its field");
    std::memset(bytes.data()+offset,0,limit);std::memcpy(bytes.data()+offset,value.data(),value.size());
}

WeaponDatabase::WeaponDatabase(std::vector<WeaponRecord> records):records_(std::move(records)){}

WeaponDatabase WeaponDatabase::load(std::span<const std::uint8_t> file){
    if(file.size()<0x10)throw std::runtime_error("Weapon database: file shorter than its header");
    if(read_u32(file,0)!=weapon_file_version)throw std::runtime_error("Weapon database: header word is not 4 (00147400 rejects the file)");
    const auto count=static_cast<std::int32_t>(read_u32(file,4));
    std::vector<WeaponRecord> records;
    if(count>0){
        const auto n=static_cast<std::size_t>(count);
        if(n>(file.size()-0x10)/weapon_record_size)throw std::runtime_error("Weapon database: record count exceeds the file");
        records.resize(n);
        for(std::size_t i=0;i<n;++i){
            auto& r=records[i];
            std::memcpy(r.bytes.data(),file.data()+0x10+i*weapon_record_size,weapon_record_size);
            r.set_byte(0x138,0);r.set_word(0x130,-1);r.set_word(0x134,-1);r.set_word(0x13c,0);r.set_word(0x11c,0);
            if(r.damage_type_bc()==1){
                r.set_word(0x64,divide_by_5(r.word(0x64)));
                r.set_word(0x68,divide_by_5(r.word(0x68)));
                r.set_word(0x6c,divide_by_5(r.word(0x6c)));
            }
        }
    }
    WeaponDatabase db(std::move(records));
    db.header_words={read_u32(file,8),read_u32(file,12)};
    return db;
}

std::size_t WeaponDatabase::index(std::int32_t id) const{
    if(records_.empty())throw std::runtime_error("Weapon database: lookup in an empty database");
    if(id<0||static_cast<std::size_t>(id)>=records_.size())return 0;
    return static_cast<std::size_t>(id);
}

namespace {
// 00128e80's fold: the ctype bit 1 (upper case) adds 0x20.
unsigned char fold_reticle(unsigned char c){
    if(c>=0x80)throw std::runtime_error("Weapon reticles 00128e80: a byte >= 0x80 indexes the ctype table 0x499c71 below its start");
    return c>='A'&&c<='Z'?static_cast<unsigned char>(c+0x20):c;
}
bool same_reticle(const std::string& a,const std::string& b){   // 00128e80(a, b) == 0
    std::size_t i=0;
    for(;i<a.size();++i){
        const auto x=fold_reticle(static_cast<unsigned char>(a[i]));
        const auto y=i<b.size()?fold_reticle(static_cast<unsigned char>(b[i])):0;
        if(x!=y)return false;
    }
    return i==b.size();
}
// 00139200 growing a vector to hold `needed`.
std::uint32_t grown_capacity(std::uint32_t capacity,std::uint32_t needed){
    if(needed>0x1800000U)throw std::runtime_error("Weapon reticles: the list grows past 0x1800000 (00139200 aborts)");
    if(capacity>=needed)return capacity;
    std::uint32_t n=capacity?capacity:1;
    while(n<needed)n=n<0xc00000U?n*2:0x1800000U;
    return n;
}
}

std::string weapon_reticle_name(const WeaponRecord& record){
    std::string out;
    for(std::size_t i=0x90;i<weapon_record_size;++i){
        if(record.bytes[i]==0)return out;
        out.push_back(static_cast<char>(record.bytes[i]));
    }
    throw std::runtime_error("Weapon reticles: the name at record +90 runs past the record");
}

WeaponReticleList build_weapon_reticles(WeaponDatabase& db){
    WeaponReticleList list;
    std::vector<std::string> names;
    for(std::size_t r=0;r<db.size();++r){
        auto& rec=db.mutable_record(static_cast<std::int32_t>(r));
        if(static_cast<std::int8_t>(rec.byte(0x90))==0){rec.set_word(0x130,-1);continue;}
        const auto name=weapon_reticle_name(rec);
        std::size_t s=0;
        while(s<names.size()&&!same_reticle(name,names[s]))++s;
        rec.set_word(0x130,static_cast<std::int32_t>(s));
        if(s<names.size())continue;
        list.capacity=grown_capacity(list.capacity,static_cast<std::uint32_t>(names.size()+1));
        names.push_back(name);
        list.records.push_back(r);
    }
    return list;
}

std::uint8_t weapon_slot_of_class(std::uint8_t weapon_class){
    static constexpr std::array<std::uint8_t,10> table{0,1,2,0,1,3,6,7,4,5};   // 00489d60
    if(weapon_class>=table.size())throw std::runtime_error("Weapon class outside the 00489d60 slot table");
    return table[weapon_class];
}

}
