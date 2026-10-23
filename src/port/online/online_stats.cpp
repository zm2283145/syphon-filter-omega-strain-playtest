// Checkpoint 132on2d: see online_stats.hpp (the record's layout from 002cbd40; the store is PORT-ONLY).
#include "online_stats.hpp"

#include "online_files.hpp"
#include "online_hash.hpp"

#include <algorithm>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace omega::online {
namespace {

std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t o) {
    return static_cast<std::uint16_t>(b[o] | (b[o + 1] << 8));
}
std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t o) {
    return static_cast<std::uint32_t>(b[o]) | (static_cast<std::uint32_t>(b[o + 1]) << 8) |
           (static_cast<std::uint32_t>(b[o + 2]) << 16) | (static_cast<std::uint32_t>(b[o + 3]) << 24);
}

// Text fields are escaped (\\ \t \n \r; the game name holds tabs, 0044e960's "%s\t%d\t%d\t%d\t%d"); "-" is empty.
std::string one_field(std::string_view s) {
    if (s.empty()) return "-";
    std::string out;
    for (char c : s) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

std::string field_text(const std::string& s) {
    if (s == "-") return {};
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '\\' || i + 1 == s.size()) {
            out += s[i];
            continue;
        }
        const char e = s[++i];
        out += e == 't' ? '\t' : e == 'n' ? '\n' : e == 'r' ? '\r' : e;
    }
    return out;
}

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const std::size_t tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return out;
}

StatsBlob blob_from_hex(const std::string& hex) {
    const auto bytes = from_hex(hex);
    if (bytes.size() != kStatsBlobSize) throw std::runtime_error("statistics record size");
    StatsBlob b{};
    std::copy(bytes.begin(), bytes.end(), b.begin());
    return b;
}

}  // namespace

bool valid_stats(std::span<const std::uint8_t> blob) {
    return blob.size() >= kStatsBlobSize && blob[0] == kStatsBlobVersion;
}

// 002cbd40's writes (ONLINE.md §3.6) and 0044e340's game-entry fields.
StatsFields decode_stats(std::span<const std::uint8_t> b) {
    StatsFields f;
    if (b.size() < kStatsBlobSize) return f;
    f.version = b[0];
    f.rank = b[1];
    f.aim_accuracy = b[2];
    f.accuracy = b[3];
    f.stat13 = u16(b, 0x4);
    f.stat1 = u16(b, 0x6);
    f.stat4 = u16(b, 0x8);
    f.stat6 = u16(b, 0xa);
    f.stat7 = u16(b, 0xc);
    f.shots = u32(b, 0x10);
    f.hits = u32(b, 0x14);
    f.stat2 = u32(b, 0x18);
    f.kills = u32(b, 0x1c);
    f.deaths = u32(b, 0x20);
    f.ratings = u32(b, 0x24);
    f.clan_id = static_cast<std::int32_t>(u32(b, 0xe8));
    f.host_account = u32(b, 0xec);
    f.game_world = u32(b, 0xf0);
    f.client_ip = u32(b, 0xf4);
    f.filter = b[0xf8];
    f.mission = b[0xf9];
    return f;
}

std::string describe_stats(std::span<const std::uint8_t> blob) {
    if (!valid_stats(blob)) return "no record";
    const StatsFields f = decode_stats(blob);
    std::ostringstream s;
    s << "rank " << int(f.rank) << ", accuracy " << int(f.accuracy) << "% (" << f.shots << " shots, " << f.hits
      << " hits), aim-view accuracy " << int(f.aim_accuracy) << "%, kills " << f.kills << ", deaths " << f.deaths
      << ", stat1 " << f.stat1 << ", stat2 " << f.stat2 << ", stat4 " << f.stat4 << ", stat6 " << f.stat6
      << ", friendly kills " << f.stat7 << ", stat13 " << f.stat13 << ", ratings 0x" << std::hex << f.ratings << std::dec;
    if (f.filter == kStatsFilterNone) s << ", in the lobby";
    else if (f.host_account != 0)
        s << ", game entry: host account " << f.host_account << ", world " << f.game_world << ", mission "
          << int(f.mission) << ", filter " << int(f.filter);
    return s.str();
}

std::string describe_stats_change(std::span<const std::uint8_t> before, std::span<const std::uint8_t> after) {
    const StatsFields a = decode_stats(before), b = decode_stats(after);
    std::ostringstream s;
    bool any = false;
    auto item = [&](const char* name, std::int64_t from, std::int64_t to) {
        if (from == to) return;
        s << (any ? ", " : "") << (to > from ? "+" : "") << (to - from) << ' ' << name;
        any = true;
    };
    item("shots", a.shots, b.shots);
    item("hits", a.hits, b.hits);
    item("kills", a.kills, b.kills);
    item("deaths", a.deaths, b.deaths);
    item("stat1", a.stat1, b.stat1);
    item("stat2", a.stat2, b.stat2);
    item("stat4", a.stat4, b.stat4);
    item("stat6", a.stat6, b.stat6);
    item("friendly kills", a.stat7, b.stat7);
    item("stat13", a.stat13, b.stat13);
    item("rank", a.rank, b.rank);
    if (a.ratings != b.ratings) {
        s << (any ? ", " : "") << "ratings 0x" << std::hex << a.ratings << " -> 0x" << b.ratings << std::dec;
        any = true;
    }
    return any ? s.str() : "no change";
}

StatsStore::StatsStore(std::filesystem::path file, int backups, std::size_t max_results, std::uint64_t backup_interval)
    : file_(std::move(file)), backups_(backups), max_results_(max_results ? max_results : 1),
      backup_interval_(backup_interval) {}

bool StatsStore::update(std::uint32_t account, std::span<const std::uint8_t> blob, std::uint64_t now) {
    if (!account || !valid_stats(blob)) return false;
    AccountStatsRecord& r = records_[account];
    r.account = account;
    std::copy(blob.begin(), blob.begin() + kStatsBlobSize, r.blob.begin());
    r.updated = now;
    ++r.reports;
    return true;
}

const AccountStatsRecord* StatsStore::find(std::uint32_t account) const {
    const auto it = records_.find(account);
    return it == records_.end() ? nullptr : &it->second;
}

void StatsStore::add_result(GameResult result) {
    for (const auto& p : result.players)
        if (auto it = records_.find(p.account); it != records_.end()) ++it->second.games;
    results_.push_back(std::move(result));
    if (results_.size() > max_results_)
        results_.erase(results_.begin(), results_.begin() + static_cast<std::ptrdiff_t>(results_.size() - max_results_));
}

void StatsStore::load() {
    records_.clear();
    results_.clear();
    if (file_.empty() || !std::filesystem::exists(file_)) return;
    std::ifstream in(file_, std::ios::binary);
    if (!in) throw std::runtime_error("statistics: cannot read " + file_.string());
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto f = split_tabs(line);
        try {
            if (f[0] == "stats" && f.size() == 6) {
                AccountStatsRecord r;
                r.account = static_cast<std::uint32_t>(std::stoul(f[1]));
                r.updated = std::stoull(f[2]);
                r.reports = static_cast<std::uint32_t>(std::stoul(f[3]));
                r.games = static_cast<std::uint32_t>(std::stoul(f[4]));
                r.blob = blob_from_hex(f[5]);
                records_[r.account] = r;
                continue;
            }
            if (f[0] == "result" && f.size() >= 8) {
                GameResult g;
                g.ended = std::stoull(f[1]);
                g.world = static_cast<std::uint32_t>(std::stoul(f[2]));
                g.seconds = std::stoull(f[3]);
                g.host = static_cast<std::uint32_t>(std::stoul(f[4]));
                g.why = field_text(f[5]);
                g.game_name = field_text(f[6]);
                const std::size_t n = std::stoul(f[7]);
                if (f.size() != 8 + n * 4) throw std::runtime_error("count");
                for (std::size_t i = 0; i < n; ++i) {
                    GameResultPlayer p;
                    p.account = static_cast<std::uint32_t>(std::stoul(f[8 + i * 4]));
                    p.name = field_text(f[9 + i * 4]);
                    p.entry = blob_from_hex(f[10 + i * 4]);
                    p.final = blob_from_hex(f[11 + i * 4]);
                    g.players.push_back(std::move(p));
                }
                results_.push_back(std::move(g));
                continue;
            }
        } catch (const std::exception&) {
        }
        throw std::runtime_error("statistics: malformed line " + std::to_string(number));
    }
}

void StatsStore::save() const {
    if (file_.empty()) return;
    std::ostringstream out;
    out << "# omega_server statistics (PORT-ONLY): each account's last agent statistics record (the 0xfc bytes the\n"
           "# game sends in AccountUpdateStats / PlayerReport, research/ONLINE.md section 3.6) and the finished games.\n"
           "# stats\taccount\tupdated\treports\tgames\trecord hex\n"
           "# result\tended\tworld\tseconds\thost\twhy\tgame name\tn\tn x (account\tname\tentry hex\tfinal hex)\n";
    for (const auto& [id, r] : records_)
        out << "stats\t" << r.account << '\t' << r.updated << '\t' << r.reports << '\t' << r.games << '\t' << to_hex(r.blob)
            << '\n';
    for (const auto& g : results_) {
        out << "result\t" << g.ended << '\t' << g.world << '\t' << g.seconds << '\t' << g.host << '\t' << one_field(g.why)
            << '\t' << one_field(g.game_name) << '\t' << g.players.size();
        for (const auto& p : g.players)
            out << '\t' << p.account << '\t' << one_field(p.name) << '\t' << to_hex(p.entry) << '\t' << to_hex(p.final);
        out << '\n';
    }
    write_file_atomic(file_, out.str(), backups_, backup_interval_);
}

}  // namespace omega::online
