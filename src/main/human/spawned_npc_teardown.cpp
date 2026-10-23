#include "spawned_npc_teardown.hpp"
#include "spawned_npc_dtor.hpp"

#include <cstdio>
#include <cstring>
#include <set>
#include <stdexcept>

namespace omega {
namespace {

constexpr std::uint32_t A=spawned_npc_teardown_actor;
constexpr std::uint32_t INV=spawned_npc_teardown_inventory;
constexpr std::uint32_t C=spawned_npc_teardown_component;
constexpr std::uint32_t LIST_3528=0x01300000;     // the +3528 list object (allocated by the constructor)
constexpr std::uint32_t AI_1AC=0x01400000;        // AI component +1ac / +1b0 (allocated by 00158bc0)
constexpr std::uint32_t AI_1B0=0x01410000;
constexpr std::uint32_t ROUTE_MANAGER=0x004ed9b0; // 003caa50(004ffb50, 004ed9b0)'s answer: the manager object
constexpr std::uint32_t STACK=0x01f00000;

std::string hex(std::uint32_t v){char b[16];std::snprintf(b,sizeof b,"%x",v);return b;}

// Child calls whose effect is the release of memory, of list links inside the
// released objects, or of the object magic (003cb330 / 003cad70: +4 = 0, +c =
// f0fffff0): the native port holds none of that memory, so they are recorded.
// 0042df20 (NetObjectMgr erase of +c) is called because +3598 is set; its map
// only receives entries on the network paths (005721c8), so in solo it erases
// nothing (GENERATOR_RUNTIME.md "Checkpoint 119f").
const std::set<std::uint32_t> recorded{
    0x36b130,0x100740,0x161680,0x16dfd0,0x16f550,0x1be780,0x1a08d0,0x1a07f0,0x22eba0,0x136300,0x136350,
    0x1b5f90,0x1b5f40,0x17ed20,0x1b5ed0,0x1b5e70,0x1b5e20,0x392c10,0x42df20,0x36e220,0x3c9d60,0x20eba0};

class TeardownHost:public SpawnedActorHost{
public:
    TeardownHost(const SpawnedNpcTeardownServices& s,SpawnedNpcTeardownResult& r):s_(s),r_(r){}
    std::uint32_t call(SpawnedActorMemory& m,std::uint32_t address,std::initializer_list<std::uint32_t> args) override{
        std::string line=hex(address)+"(";
        bool first=true;
        for(const auto a:args){line+=(first?"":",")+hex(a);first=false;}
        line+=")";
        const auto* p=args.begin();
        const auto fail=[&](const char* why){throw std::runtime_error("Spawned NPC teardown: "+line+" "+why);};
        switch(address){
        // The tree's natively ported nodes and leaves (spawned_npc_dtor.hpp, SpawnedActorHost).
        case 0x144c70:case 0x3ccf30:case 0x158550:case 0x1a02c0:case 0x184cd0:case 0x12f370:
            r_.calls.push_back(line);
            return SpawnedActorHost::call(m,address,args);
        case 0x185d40:
            if(args.size()!=4||p[0]!=A||p[1]>2||p[2]!=0||p[3]!=0x3f800000U)fail("is not an effect slot turned off");
            if(!s_.effect_off)fail("needs the effect slots (00185d40)");
            r_.calls.push_back(line);s_.effect_off(p[1]);return 0;
        case 0x144ba0:
            if(args.size()!=2||p[0]!=INV||p[1]>1)fail("is not a hand of the actor's inventory");
            if(!s_.stop_loop_sound)fail("needs the inventory (00144ba0)");
            r_.calls.push_back(line);s_.stop_loop_sound(static_cast<std::uint8_t>(p[1]));return 0;
        case 0x3caa50:
            if(args.size()!=2||p[0]!=0x4ffb50||p[1]!=ROUTE_MANAGER)fail("is not the route manager lookup");
            r_.calls.push_back(line);return ROUTE_MANAGER;
        case 0x163c50:
            if(args.size()!=2||p[0]!=ROUTE_MANAGER||p[1]!=A)fail("is not the actor's route release");
            if(!s_.route_release)fail("needs the route manager (00163c50)");
            r_.calls.push_back(line);s_.route_release();return 0;
        case 0x3c9420:
            if(args.size()!=3||p[1]!=0x4ffb50||p[2]!=1)fail("is not the stun's sound stop");
            if(!s_.stop_stun_sound)fail("needs the stun sound stop (0020eba0 -> 003c9420)");
            r_.calls.push_back(line);s_.stop_stun_sound();return 0;
        default:
            break;
        }
        if(recorded.count(address)){
            if(address==0x20eba0&&(args.size()!=3||p[1]!=0x4f12d0||p[2]!=A))fail("is not the stun's sound message");
            r_.calls.push_back(line);
            return 0;
        }
        return SpawnedActorHost::call(m,address,args);
    }
private:
    const SpawnedNpcTeardownServices& s_;
    SpawnedNpcTeardownResult& r_;
};

}

SpawnedNpcTeardownResult run_spawned_npc_teardown(const SpawnedNpcTeardownInput& in,const SpawnedNpcTeardownServices& services){
    if(in.root[1]&&in.root[0]&&!in.root[2])
        throw std::runtime_error("Spawned NPC teardown: an active actor's physical component release 001b5ae4 is not hosted");
    SpawnedActorMemory m;
    const auto w8=[&m](std::uint32_t a,std::uint32_t v){m.write(a,1,v);};
    const auto w32=[&m](std::uint32_t a,std::uint32_t v){m.write(a,4,v);};
    // The ELF's table words the tree reads (asserted against the image by
    // tools/ee/gen_spawned_npc_dtor.py): 004d9380 +8 / +4c / +54, 004da690 +7c.
    w32(0x4d9388,0x158550);w32(0x4d93cc,0x157ad0);w32(0x4d93d4,0x12f370);w32(0x4da70c,0x184cd0);
    w8(0x533880,0);w32(0x533888,0);w8(0x5721c8,0);   // no profiler; solo
    w32(A,0x4da690);w32(A+0xc,in.id_c);
    w8(A+0x2c,in.root[0]);w8(A+0x2d,in.root[1]);w8(A+0x2e,in.root[2]);
    w32(A+0x58,C);                 // the AI component (001ae450 -> 003cce30)
    w32(A+0x3264,0);               // the template name: play refuses a named GEN record
    w32(A+0x3524,INV);             // the inventory (00144dc0)
    w8(A+0x31fc,1);                // the component attached (003cce30)
    w32(A+0x3584,0);               // no network object (solo)
    w32(A+0x33ec,0);               // no flashlight object (role 2 without the inventory object)
    w32(A+0x3310,0);w32(A+0x3314,0);   // 001b531c / 001b5320 (deactivated)
    w32(A+0x3528,LIST_3528);
    std::uint32_t stun;std::memcpy(&stun,&in.stun_33bc,4);w32(A+0x33bc,stun);
    w32(A+0x3478,0);               // no target link (no writer on the spawned path)
    w8(A+0x3598,1);                // the generator's register (0021e620)
    w32(INV,0);w32(INV+0xd4,0);w32(INV+0xd8,0);   // 00144dc0 without the object
    w32(C,0x4d9380);w32(C+0x30,A);w8(C+0x159,in.ai_inactive_159);
    w32(C+0x1ac,AI_1AC);w32(C+0x1b0,AI_1B0);
    SpawnedNpcTeardownResult r;
    TeardownHost host(services,r);
    run_spawned_npc_dtor(m,host,A,0xffffffffU,STACK);
    return r;
}

}
