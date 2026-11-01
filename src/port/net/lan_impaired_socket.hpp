#pragma once
// PORT-ONLY (not in the original; LAN resilience testing, 133lt3): a LanSocket decorator that drops, duplicates
// and delays (reorders) outgoing datagrams of a real socket, so two or more real game processes can be soaked
// under packet loss, duplication and reordering. research/LAN_PLAY.md "LAN resilience (133lt3)".
//
// The original's RT UDP layer (RtUdp_SendPump 0x3091B8, the ACK / duplicate check 0x30ac40 / 0x30acd8 and the
// ordered read 0x307dd8) ran over a real network that could lose, copy and reorder datagrams; LanTransport stands in
// for that layer (lan_transport.hpp). This decorator is the test network, not game behaviour: it is only installed
// by the explicit --lan-impair option and never by default. The fates are drawn with the same seeded splitmix64
// stream as LanLoopbackNetwork (lan_loopback.hpp), one stream per socket (seed + the socket's creation index), so the
// decisions repeat for a seed; delivery timing on a real network still varies from run to run.
//
// A delayed or duplicated datagram is held in the decorator and handed to the inner socket once its time has come,
// on the next send_to or receive_from of the same socket (the transport pumps both every frame).

#include "lan_socket.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace omega::lan {

struct LanImpairment {
    double loss = 0.0;              // probability an outgoing datagram is dropped
    double duplicate = 0.0;         // probability a kept datagram is sent a second time, 1..reorder_ms ms later
    double reorder = 0.0;           // probability a kept datagram is held back by 1..reorder_ms ms
    std::uint64_t reorder_ms = 40;  // the maximum extra delay
    std::uint64_t seed = 1;         // the base seed (each socket adds its creation index)
    bool any() const { return loss > 0.0 || duplicate > 0.0 || reorder > 0.0; }
};

// Totals over every impaired socket of the process (for the run's log).
struct LanImpairmentStats {
    std::uint64_t sent = 0, dropped = 0, duplicated = 0, delayed = 0, forwarded = 0;
};

// "loss=0.05,dup=0.02,reorder=0.1,reorder_ms=60,seed=7" (any subset, any order; probabilities in [0, 0.9],
// reorder_ms in [1, 2000]). nullopt with *error set when the text is malformed.
std::optional<LanImpairment> lan_parse_impairment(const std::string& text, std::string* error = nullptr);
std::string lan_impairment_text(const LanImpairment& impairment);

std::unique_ptr<LanSocket> lan_impaired_socket(std::unique_ptr<LanSocket> inner, const LanImpairment& impairment,
                                               std::uint64_t stream, LanClock clock,
                                               std::shared_ptr<LanImpairmentStats> stats);

// Wraps every socket the inner factory makes. The stats object is shared by all of them.
LanSocketFactory lan_impaired_socket_factory(LanSocketFactory inner, const LanImpairment& impairment,
                                             std::shared_ptr<LanImpairmentStats> stats,
                                             LanClock clock = lan_steady_clock());

}  // namespace omega::lan
