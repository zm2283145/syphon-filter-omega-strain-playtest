#pragma once
#include "spawned_actor_ctor.hpp"

namespace omega {
// Original 002300d0..002301d4: the AI component binding body. Only virtual
// references exist (the words at 004d940c, 004d991c, 004d9d7c, 004dbaac,
// 004de32c and 004de5bc, i.e. slot +8c of 004d9380, 004d9890 and 004dba20
// among others); 003cce30 calls it with (component, actor). It always
// stores the actor at component +30; for a non-null actor whose +4c equals
// the live word at 0049d010 and whose entity +3264 is nonzero it lazily
// allocates the +18 slot (0xc bytes, system.hh line 0x49, initialised
// 0/0/-1 and stored even when null) and calls 003d9d30 with the reread slot
// and entity. Locks, allocator and 003d9d30 stay explicit services; a
// callback failure leaves earlier writes committed. v0 is not modelled (the
// original leaves 003d9d30's v0 or the caller's; 003cce30 ignores it).
void run_ai_component_bind(SpawnedActorMemory&, SpawnedActorHost&,
                           std::uint32_t component, std::uint32_t actor);
}
