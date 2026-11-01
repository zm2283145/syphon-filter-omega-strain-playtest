// PORT-ONLY (checkpoint 134on5): see online_dns.hpp (research/ONLINE.md §8.7).
#include "online_dns.hpp"

#include <algorithm>
#include <cctype>

namespace omega::online {

namespace {
std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    while (!s.empty() && s.back() == '.') s.pop_back();
    return s;
}
std::uint16_t be16(std::span<const std::uint8_t> b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] << 8 | b[o + 1]);
}
void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x));
}
// The question's name at offset 12 (no compression in a question); the offset after QTYPE/QCLASS in *end.
std::optional<std::string> question_name(std::span<const std::uint8_t> q, std::size_t* end) {
    std::size_t at = 12;
    std::string name;
    for (int labels = 0; labels < 128; ++labels) {
        if (at >= q.size()) return std::nullopt;
        const std::uint8_t len = q[at++];
        if (len == 0) {
            if (at + 4 > q.size()) return std::nullopt;
            *end = at + 4;
            return name;
        }
        if (len & 0xC0 || at + len > q.size()) return std::nullopt;
        if (!name.empty()) name.push_back('.');
        name.append(reinterpret_cast<const char*>(q.data() + at), len);
        at += len;
        if (name.size() > 253) return std::nullopt;
    }
    return std::nullopt;
}
}  // namespace

void DnsResponder::add(const std::string& name, std::uint32_t ip) {
    const std::string n = lower(name);
    if (n.rfind("*.", 0) == 0) wildcard_[n.substr(2)] = ip;
    else exact_[n] = ip;
}

std::optional<std::uint32_t> DnsResponder::lookup(const std::string& name) const {
    const std::string n = lower(name);
    if (auto it = exact_.find(n); it != exact_.end()) return it->second;
    for (const auto& [suffix, ip] : wildcard_)
        if (n.size() > suffix.size() && n.compare(n.size() - suffix.size(), suffix.size(), suffix) == 0 &&
            n[n.size() - suffix.size() - 1] == '.')
            return ip;
    return std::nullopt;
}

DnsResponder::Result DnsResponder::handle(std::span<const std::uint8_t> q, bool can_forward) const {
    Result r;
    if (q.size() < 12) return r;
    const std::uint16_t flags = be16(q, 2);
    if (flags & 0x8000) return r;  // a response, not a query
    if (be16(q, 4) != 1) return r;  // exactly one question (what every stub resolver sends)
    std::size_t end = 0;
    const auto name = question_name(q, &end);
    if (!name) return r;
    r.name = *name;
    r.qtype = be16(q, end - 4);
    const std::uint16_t qclass = be16(q, end - 2);
    const unsigned opcode = (flags >> 11) & 0xF;
    const auto ip = (opcode == 0 && qclass == 1) ? lookup(*name) : std::nullopt;
    if (!ip && can_forward && opcode == 0) {
        r.kind = Kind::Forward;
        return r;
    }
    // The reply: the id, QR, the opcode and RD copied, AA for our names, RA when we forward; the question copied.
    std::uint16_t rflags = static_cast<std::uint16_t>(0x8000 | (flags & 0x7900) | (can_forward ? 0x0080 : 0));
    const bool answer = ip.has_value() && r.qtype == 1;
    if (ip) rflags |= 0x0400;
    else rflags |= 5;  // REFUSED
    r.reply.assign(q.begin(), q.begin() + 2);
    put16(r.reply, rflags);
    put16(r.reply, 1);
    put16(r.reply, answer ? 1 : 0);
    put16(r.reply, 0);
    put16(r.reply, 0);
    r.reply.insert(r.reply.end(), q.begin() + 12, q.begin() + static_cast<std::ptrdiff_t>(end));
    if (answer) {
        put16(r.reply, 0xC00C);  // the question's name
        put16(r.reply, 1);       // A
        put16(r.reply, 1);       // IN
        put16(r.reply, 0);
        put16(r.reply, 60);      // TTL 60 s
        put16(r.reply, 4);
        for (int s = 24; s >= 0; s -= 8) r.reply.push_back(static_cast<std::uint8_t>(*ip >> s));
    }
    r.kind = ip ? Kind::Answer : Kind::Refuse;
    return r;
}

void DnsForwarder::expire(std::uint64_t now_ms) {
    for (auto it = pending_.begin(); it != pending_.end();)
        it = now_ms - it->second.at > timeout_ms_ ? pending_.erase(it) : std::next(it);
}

std::vector<std::uint8_t> DnsForwarder::outgoing(std::span<const std::uint8_t> query, std::uint32_t ip,
                                                 std::uint16_t port, std::uint64_t now_ms) {
    expire(now_ms);
    if (query.size() < 12 || pending_.size() >= 4096) return {};
    std::uint16_t id = next_;
    for (int tries = 0; pending_.count(id) && tries < 65536; ++tries) id = static_cast<std::uint16_t>(id + 1);
    next_ = static_cast<std::uint16_t>(id + 1);
    pending_[id] = Entry{ip, port, be16(query, 0), now_ms};
    std::vector<std::uint8_t> out(query.begin(), query.end());
    out[0] = static_cast<std::uint8_t>(id >> 8);
    out[1] = static_cast<std::uint8_t>(id);
    return out;
}

std::optional<DnsForwarder::Back> DnsForwarder::incoming(std::span<const std::uint8_t> reply, std::uint64_t now_ms) {
    expire(now_ms);
    if (reply.size() < 12 || !(reply[2] & 0x80)) return std::nullopt;
    const auto it = pending_.find(be16(reply, 0));
    if (it == pending_.end()) return std::nullopt;
    Back b{it->second.ip, it->second.port, {reply.begin(), reply.end()}};
    b.reply[0] = static_cast<std::uint8_t>(it->second.id >> 8);
    b.reply[1] = static_cast<std::uint8_t>(it->second.id);
    pending_.erase(it);
    return b;
}

std::vector<std::uint8_t> dns_query(std::uint16_t id, const std::string& name, std::uint16_t qtype) {
    std::vector<std::uint8_t> q;
    put16(q, id);
    put16(q, 0x0100);  // RD
    put16(q, 1);
    put16(q, 0);
    put16(q, 0);
    put16(q, 0);
    std::size_t start = 0;
    while (start <= name.size()) {
        const std::size_t dot = std::min(name.find('.', start), name.size());
        if (dot > start) {
            q.push_back(static_cast<std::uint8_t>(dot - start));
            q.insert(q.end(), name.begin() + static_cast<std::ptrdiff_t>(start), name.begin() + static_cast<std::ptrdiff_t>(dot));
        }
        start = dot + 1;
    }
    q.push_back(0);
    put16(q, qtype);
    put16(q, 1);
    return q;
}

std::optional<std::uint32_t> dns_first_a(std::span<const std::uint8_t> reply, int* rcode) {
    if (reply.size() < 12) return std::nullopt;
    if (rcode) *rcode = reply[3] & 0xF;
    std::size_t end = 0;
    if (be16(reply, 4) != 1 || !question_name(reply, &end)) return std::nullopt;
    std::size_t at = end;
    for (unsigned n = be16(reply, 6); n > 0; --n) {
        // the owner name: a pointer or labels
        while (at < reply.size()) {
            const std::uint8_t len = reply[at];
            if ((len & 0xC0) == 0xC0) {
                at += 2;
                break;
            }
            at += 1 + len;
            if (len == 0) break;
        }
        if (at + 10 > reply.size()) return std::nullopt;
        const std::uint16_t type = be16(reply, at), rdlen = be16(reply, at + 8);
        at += 10;
        if (at + rdlen > reply.size()) return std::nullopt;
        if (type == 1 && rdlen == 4)
            return static_cast<std::uint32_t>(reply[at]) << 24 | static_cast<std::uint32_t>(reply[at + 1]) << 16 |
                   static_cast<std::uint32_t>(reply[at + 2]) << 8 | reply[at + 3];
        at += rdlen;
    }
    return std::nullopt;
}

}  // namespace omega::online
