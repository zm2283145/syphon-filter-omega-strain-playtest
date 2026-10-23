#include "actor_component_attach.hpp"
#include <stdexcept>

namespace omega {
void run_actor_component_attach(SpawnedActorMemory& m,SpawnedActorHost& h,
                                std::uint32_t actor,std::uint32_t replacement){
    if(!actor)throw std::runtime_error("Actor component attachment requires an actor");
    const auto old=m.read(actor+0x58);
    if(old){
        if(m.read(0x533880,1))h.call(m,0x10dc40,{9});
        m.write(0x533888,4,m.read(0x533888)+1);
        // The receiver is saved, but its vtable is read after the lock callback.
        h.call(m,m.read(m.read(old)+8),{old,0xffffffffU});
        h.call(m,0x36b130,{0,old,0x4bd228,0x291});
        if(m.read(0x533880,1))h.call(m,0x10dca8,{9});
        m.write(0x533888,4,m.read(0x533888)-1);
        m.write(actor+0x58,4,0);
    }
    m.write(actor+0x58,4,replacement);
    const auto live=m.read(actor+0x58);
    if(live)h.call(m,m.read(m.read(live)+0x8c),{live,actor});
}
}
