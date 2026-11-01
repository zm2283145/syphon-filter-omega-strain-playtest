// Checkpoint 132on1: see online_protocol.hpp (research/ONLINE.md).
#include "online_protocol.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstring>

namespace omega::online {

Platform platform_from_byte(std::uint8_t value) {
    return value <= static_cast<std::uint8_t>(Platform::Ps2) ? static_cast<Platform>(value) : Platform::Unknown;
}

std::string_view platform_name(Platform p) {
    switch (p) {
    case Platform::Pc: return "pc";
    case Platform::Vita: return "vita";
    case Platform::Ps4: return "ps4";
    case Platform::Ps5: return "ps5";
    case Platform::Switch: return "switch";
    case Platform::Ps2: return "ps2";
    default: return "unknown";
    }
}

std::string_view platform_badge(Platform p) {
    switch (p) {
    case Platform::Pc: return "PC";
    case Platform::Vita: return "VITA";
    case Platform::Ps4: return "PS4";
    case Platform::Ps5: return "PS5";
    case Platform::Switch: return "NSW";
    case Platform::Ps2: return "PS2";
    default: return "?";
    }
}

std::string badged_name(std::string_view name, Platform p, bool show_badges) {
    if (!show_badges) return std::string(name);
    return "[" + std::string(platform_badge(p)) + "] " + std::string(name);
}

std::optional<Platform> parse_platform(std::string_view name) {
    for (std::uint8_t v = 0; v <= static_cast<std::uint8_t>(Platform::Ps2); ++v)
        if (platform_name(static_cast<Platform>(v)) == name) return static_cast<Platform>(v);
    return std::nullopt;
}

Platform this_platform() {
#if defined(OMEGA_PLATFORM_VITA)
    return Platform::Vita;
#elif defined(OMEGA_PLATFORM_PS4)
    return Platform::Ps4;
#elif defined(OMEGA_PLATFORM_PS5)
    return Platform::Ps5;
#elif defined(OMEGA_PLATFORM_SWITCH)
    return Platform::Switch;
#else
    return Platform::Pc;
#endif
}

std::vector<std::uint8_t> encode_frame(std::uint16_t type, std::span<const std::uint8_t> body) {
    const std::uint32_t length = static_cast<std::uint32_t>(body.size() + 4);
    std::vector<std::uint8_t> out;
    out.reserve(body.size() + 8);
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<std::uint8_t>(length >> (8 * i)));
    out.push_back(static_cast<std::uint8_t>(type));
    out.push_back(static_cast<std::uint8_t>(type >> 8));
    out.push_back(0);
    out.push_back(0);
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

void FrameReader::feed(std::span<const std::uint8_t> bytes) { buffer_.insert(buffer_.end(), bytes.begin(), bytes.end()); }

std::optional<Frame> FrameReader::next() {
    if (bad_ || buffer_.size() < 8) return std::nullopt;
    std::uint32_t length = 0;
    for (int i = 0; i < 4; ++i) length |= std::uint32_t{buffer_[i]} << (8 * i);
    if (length < 4 || length > kMaxFrameBody + 4) {
        bad_ = true;
        return std::nullopt;
    }
    if (buffer_.size() < 4 + std::size_t{length}) return std::nullopt;
    Frame f;
    f.type = static_cast<std::uint16_t>(buffer_[4] | (buffer_[5] << 8));
    f.body.assign(buffer_.begin() + 8, buffer_.begin() + 4 + length);
    buffer_.erase(buffer_.begin(), buffer_.begin() + 4 + length);
    return f;
}

std::int32_t get_i32(std::span<const std::uint8_t> s, std::size_t off) {
    if (off + 4 > s.size()) return 0;
    return static_cast<std::int32_t>(std::uint32_t{s[off]} | (std::uint32_t{s[off + 1]} << 8) |
                                     (std::uint32_t{s[off + 2]} << 16) | (std::uint32_t{s[off + 3]} << 24));
}

void put_i32(std::span<std::uint8_t> s, std::size_t off, std::int32_t v) {
    if (off + 4 > s.size()) return;
    const auto u = static_cast<std::uint32_t>(v);
    for (int i = 0; i < 4; ++i) s[off + i] = static_cast<std::uint8_t>(u >> (8 * i));
}

std::string get_str(std::span<const std::uint8_t> s, std::size_t off, std::size_t len) {
    std::string out;
    for (std::size_t i = 0; i < len && off + i < s.size() && s[off + i] != 0; ++i)
        out.push_back(static_cast<char>(s[off + i]));
    return out;
}

void put_str(std::span<std::uint8_t> s, std::size_t off, std::size_t len, std::string_view v) {
    if (len == 0 || off + len > s.size()) return;
    const std::size_t n = std::min(v.size(), len - 1);
    std::memcpy(s.data() + off, v.data(), n);
    std::memset(s.data() + off + n, 0, len - n);
}

ExtWriter& ExtWriter::u8(std::uint8_t v) {
    out_.push_back(v);
    return *this;
}
ExtWriter& ExtWriter::u16(std::uint16_t v) {
    out_.push_back(static_cast<std::uint8_t>(v));
    out_.push_back(static_cast<std::uint8_t>(v >> 8));
    return *this;
}
ExtWriter& ExtWriter::u32(std::uint32_t v) {
    for (int i = 0; i < 4; ++i) out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    return *this;
}
ExtWriter& ExtWriter::str(std::string_view v) {
    const std::size_t n = std::min<std::size_t>(v.size(), 0xffff);
    u16(static_cast<std::uint16_t>(n));
    out_.insert(out_.end(), v.begin(), v.begin() + static_cast<std::ptrdiff_t>(n));
    return *this;
}

std::uint8_t ExtReader::u8() {
    if (pos_ + 1 > in_.size()) {
        ok_ = false;
        return 0;
    }
    return in_[pos_++];
}
std::uint16_t ExtReader::u16() {
    const std::uint16_t lo = u8();
    const std::uint16_t hi = u8();
    return static_cast<std::uint16_t>(lo | (hi << 8));
}
std::uint32_t ExtReader::u32() {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t{u8()} << (8 * i);
    return v;
}
std::string ExtReader::str() {
    const std::size_t n = u16();
    if (!ok_ || pos_ + n > in_.size()) {
        ok_ = false;
        return {};
    }
    std::string out(reinterpret_cast<const char*>(in_.data() + pos_), n);
    pos_ += n;
    return out;
}

std::vector<std::uint8_t> encode_hello(const HelloInfo& h) {
    return ExtWriter().u32(kHelloMagic).u16(h.protocol).u8(static_cast<std::uint8_t>(h.platform)).u32(h.build).u32(h.caps).bytes();
}

std::optional<HelloInfo> decode_hello(std::span<const std::uint8_t> body) {
    ExtReader r(body);
    if (r.u32() != kHelloMagic) return std::nullopt;
    HelloInfo h;
    h.protocol = r.u16();
    h.platform = platform_from_byte(r.u8());
    h.build = r.u32();
    h.caps = r.u32();
    if (!r.ok()) return std::nullopt;
    return h;
}

std::vector<std::uint8_t> encode_roster(const std::vector<RosterEntry>& entries) {
    ExtWriter w;
    w.u16(static_cast<std::uint16_t>(entries.size()));
    for (const auto& e : entries) w.u32(e.account_id).u8(static_cast<std::uint8_t>(e.platform)).u32(e.world_id).str(e.name);
    return w.bytes();
}

std::optional<std::vector<RosterEntry>> decode_roster(std::span<const std::uint8_t> body) {
    ExtReader r(body);
    const std::size_t n = r.u16();
    std::vector<RosterEntry> out;
    for (std::size_t i = 0; i < n && r.ok(); ++i) {
        RosterEntry e;
        e.account_id = r.u32();
        e.platform = platform_from_byte(r.u8());
        e.world_id = r.u32();
        e.name = r.str();
        out.push_back(std::move(e));
    }
    if (!r.ok()) return std::nullopt;
    return out;
}

std::optional<GameNameFields> parse_game_name(std::string_view text) {
    // 0x00447790: the name runs to the first tab; sscanf "%32s\t%d\t%d\t%d\t%d" must convert 5 fields.
    const std::size_t tab = text.find('\t');
    if (tab == std::string_view::npos || tab == 0 || tab >= 32) return std::nullopt;
    GameNameFields f;
    f.name.assign(text.substr(0, tab));
    int* fields[4] = {&f.a, &f.b, &f.c, &f.d};
    std::string_view rest = text.substr(tab + 1);
    for (int i = 0; i < 4; ++i) {
        const std::size_t end = rest.find('\t');
        const std::string_view token = rest.substr(0, end);
        const auto [p, ec] = std::from_chars(token.data(), token.data() + token.size(), *fields[i]);
        if (ec != std::errc() || token.empty()) return std::nullopt;
        if (end == std::string_view::npos) {
            if (i != 3) return std::nullopt;
            rest = {};
        } else {
            rest = rest.substr(end + 1);
        }
    }
    return f;
}

// ---- Checkpoint 133on1: the RT system message and SFOLobby's event tags (ONLINE.md §3.7) ----------------------------

namespace sysmsg {
namespace {
struct TagRow {
    std::uint8_t id;
    const char* tag;
};
// 0x004976e0, in the table's order (ParseSystemMessage tries them in this order).
constexpr TagRow kTags[8] = {{ServerMessage, "<SFO_ServerMessage>"},     {ClanDisband, "<SFO_ClanDisband>"},
                             {ClanKick, "<SFO_ClanKick>"},               {ClanConfirmation, "<SFO_ClanConfirmation>"},
                             {BuddyConfirmation, "<SFO_BuddyConfirmation>"}, {BuddyRemove, "<SFO_BuddyRemove>"},
                             {CheatDetection, "<SFO_CheatDetection>"},   {ClanResign, "<SFO_ClanResign>"}};

std::string c_string(const char* p) { return std::string(p); }

bool equal_no_case(std::string_view a, std::string_view b) {  // String_CompareNoCase(a, b) == 0
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const auto x = static_cast<unsigned char>(a[i]), y = static_cast<unsigned char>(b[i]);
        if (std::tolower(x) != std::tolower(y)) return false;
    }
    return true;
}

// strncpy(dst[n], src, n) read back as a C string bounded by n (the next field supplies the tail when unterminated).
std::string bounded(const std::string& s, std::size_t n) { return s.substr(0, std::min(s.size(), n)); }
}  // namespace

const char* tag(std::uint8_t code) {
    for (const auto& row : kTags)
        if (row.id == code) return row.tag;
    return nullptr;
}

// ParseSystemMessage 0x00446340 over a NUL-terminated copy of the text (0x00445f90's buf[0x200]).
Parsed parse(std::string_view text) {
    std::vector<char> buf(0x200 + 1, 0);
    const std::size_t len = std::min<std::size_t>(text.size(), 0x200);
    std::memcpy(buf.data(), text.data(), len);
    char* str = buf.data();
    const int ilen = static_cast<int>(len);
    Parsed out;
    for (const auto& row : kTags) {
        const int n = static_cast<int>(std::strlen(row.tag));
        if (std::strncmp(str, row.tag, static_cast<std::size_t>(n)) != 0) continue;
        const int rem = ilen - n;
        char* p = str + n;
        char* const base = p;
        std::vector<char*> fields;
        if (rem >= 0) {
            do {
                fields.push_back(p);
                char* q = std::strchr(p, kSeparator);
                if (!q || q - base > rem || fields.size() == 4) break;
                *q = 0;
                p = q + 1;
            } while (p - base <= rem);
        }
        for (char*& s : fields) {
            while (*s && static_cast<unsigned char>(*s) <= ' ') ++s;
            for (int k = static_cast<int>(std::strlen(s)) - 1; k >= 0 && static_cast<unsigned char>(s[k]) <= ' '; --k)
                s[k] = 0;
            out.fields.push_back(c_string(s));
        }
        out.code = row.id;
        return out;
    }
    out.code = ServerMessage;
    out.fields.push_back(c_string(str));
    return out;
}

std::string compose(std::uint8_t code, const std::vector<std::string>& fields) {
    std::string out = tag(code) ? tag(code) : "";
    for (std::size_t i = 0; i < fields.size(); ++i) {
        if (i) out.push_back(kSeparator);
        std::string f = fields[i];
        std::replace(f.begin(), f.end(), kSeparator, ' ');
        out += f;
    }
    return out;
}

// SFOLobby_Callback_SystemMessage 0x00445f90.
Event event(std::uint8_t severity, std::string_view text) {
    Event e;
    if (text.size() >= 0x200) return e;
    const Parsed p = parse(text);
    const std::uint8_t code = p.code;
    if ((code == ServerMessage || code == CheatDetection) && (code != ServerMessage || severity == kSeverityBig)) {
        e.kind = Event::Kind::Big;
        e.type = code;
        if (code == CheatDetection) e.s1 = p.fields.size() == 1 ? bounded(p.fields[0], 0x200) : std::string();
        else e.s1 = std::string(text.substr(0, text.find('\0')));
        return e;
    }
    if (text.size() >= 0x80) return e;
    const std::size_t n = p.fields.size();
    auto field = [&](std::size_t i) { return i < n ? p.fields[i] : std::string(); };
    // s0[0x20] is followed by s1 in the record (+c, +2c): an unterminated 32-byte s0 reads on into s1.
    auto set_s0 = [&](const std::string& v) { e.s0 = v.size() >= 0x20 ? v.substr(0, 0x20) + e.s1 : v; };
    switch (code) {
    case ServerMessage:
        if (n > 0) {
            e.type = 10;
            e.s1 = bounded(field(0), 0x80);
            e.kind = Event::Kind::Lobby;
        }
        break;
    case ClanKick:
    case ClanDisband:
    case ClanResign:
        if (n == 2) {
            e.type = code == ClanKick ? 2 : code == ClanDisband ? 6 : 11;
            e.s1 = bounded(field(0), 0x20);
            set_s0(field(1));
            e.kind = Event::Kind::Lobby;
        }
        break;
    case ClanConfirmation:
    case 5:
        if (n == 3) {
            e.type = equal_no_case(field(2), "ACCEPT") ? 4 : 5;
            e.s1 = bounded(field(0), 0x20);
            set_s0(field(1));
            e.kind = Event::Kind::Lobby;
        }
        break;
    case BuddyConfirmation:
    case 7:
        if (n == 2) {
            e.type = equal_no_case(field(1), "ACCEPT") ? 7 : 8;
            set_s0(field(0));
            e.kind = Event::Kind::Lobby;
        }
        break;
    case BuddyRemove:
        if (n == 1) {
            e.type = 3;
            set_s0(field(0));
            e.kind = Event::Kind::Lobby;
        }
        break;
    default: break;
    }
    return e;
}
}  // namespace sysmsg

std::vector<std::uint8_t> encode_system_message(std::uint8_t severity, std::uint8_t encoding, std::uint8_t language,
                                                std::string_view text) {
    std::vector<std::uint8_t> out = {severity, encoding, language, 1};
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

std::optional<SystemMessageFields> decode_system_message(std::span<const std::uint8_t> body) {
    if (body.size() < 4) return std::nullopt;  // 0x00300450 reads four bytes, then len - 4 of text
    SystemMessageFields f;
    f.severity = body[0];
    f.encoding = body[1];
    f.language = body[2];
    f.end = body[3];
    f.text.assign(body.begin() + 4, body.end());
    return f;
}

std::string format_game_name(const GameNameFields& f) {
    return f.name.substr(0, 31) +  "\t" + std::to_string(f.a) + "\t" + std::to_string(f.b) + "\t" + std::to_string(f.c) + "\t" +
           std::to_string(f.d);
}

}  // namespace omega::online
