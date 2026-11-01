#pragma once
// PORT-ONLY (checkpoint 134on5, research/ONLINE.md §8.7, ONLINE_DEPLOY.md): an optional DNS responder in omega_server
// (off unless dns_port= / --dns is given), so a real PS2 whose network setting names this host as its DNS server
// reaches omega_server with no patched disc and no separate DNS server:
//  * the disc's MAS host (SFO_GAME.INI MAS_DNS=syphonfilter-prod.pdonline.scea.com, ONLINE.md §8.3) answers with
//    the server's address;
//  * the DNAS hosts (gate1.us / eu / jp.dnas.playstation.org, used by the disc's DNAS step, SFOLobby_ConfigVerifyDNAS
//    0x0044b7a0 via cdrom0:\IRX\DNAS280.IMG) answer with the DNAS replacement's address (dns_dnas_ip=, a separately
//    run DNASrep / clank-dnas instance; omega_server has no DNAS of its own);
//  * any other name is forwarded to dns_upstream= when one is set, else refused (REFUSED), so the PS2 never gets
//    a wrong answer for a name we do not own.
// RFC 1035 message format; A records only (another type for one of our names gets an empty NOERROR answer).

#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace omega::online {

constexpr const char* kMasHostName = "syphonfilter-prod.pdonline.scea.com";
// The DNAS gateway names (DNASrep's dnsmasq list: gate1.<region>.dnas.playstation.org; the misspelt
// "playstaion" aliases appear in its Apache config too).
inline const char* const kDnasHostNames[] = {
    "gate1.us.dnas.playstation.org", "gate1.eu.dnas.playstation.org", "gate1.jp.dnas.playstation.org",
    "gate1.us.dnas.playstaion.org",  "gate1.eu.dnas.playstaion.org",  "gate1.jp.dnas.playstaion.org",
};

class DnsResponder {
public:
    // name: a host name, or "*.suffix" for every name under suffix; case-insensitive. ip: host order.
    void add(const std::string& name, std::uint32_t ip);
    std::optional<std::uint32_t> lookup(const std::string& name) const;
    std::size_t size() const { return exact_.size() + wildcard_.size(); }

    enum class Kind { Answer, Forward, Refuse, Ignore };
    struct Result {
        Kind kind = Kind::Ignore;
        std::vector<std::uint8_t> reply;  // Answer / Refuse: the datagram to send back
        std::string name;                 // the question's name (for the log)
        std::uint16_t qtype = 0;
    };
    // `can_forward`: an upstream is configured (an unknown name is then Forward, else Refuse).
    Result handle(std::span<const std::uint8_t> query, bool can_forward) const;

private:
    std::map<std::string, std::uint32_t> exact_, wildcard_;
};

// Relays the queries DnsResponder does not own to the upstream resolver: each gets a fresh id; the answer goes back
// to its asker with the asker's id. Entries expire after timeout_ms.
class DnsForwarder {
public:
    explicit DnsForwarder(std::uint64_t timeout_ms = 5000) : timeout_ms_(timeout_ms) {}
    std::vector<std::uint8_t> outgoing(std::span<const std::uint8_t> query, std::uint32_t ip, std::uint16_t port,
                                       std::uint64_t now_ms);
    struct Back {
        std::uint32_t ip = 0;
        std::uint16_t port = 0;
        std::vector<std::uint8_t> reply;
    };
    std::optional<Back> incoming(std::span<const std::uint8_t> reply, std::uint64_t now_ms);
    std::size_t pending() const { return pending_.size(); }

private:
    struct Entry {
        std::uint32_t ip;
        std::uint16_t port, id;
        std::uint64_t at;
    };
    void expire(std::uint64_t now_ms);
    std::map<std::uint16_t, Entry> pending_;
    std::uint16_t next_ = 1;
    std::uint64_t timeout_ms_;
};

// A query datagram (tests and the self-check).
std::vector<std::uint8_t> dns_query(std::uint16_t id, const std::string& name, std::uint16_t qtype = 1);
// The first A record's address in a reply (host order), the reply's rcode in *rcode.
std::optional<std::uint32_t> dns_first_a(std::span<const std::uint8_t> reply, int* rcode = nullptr);

}  // namespace omega::online
