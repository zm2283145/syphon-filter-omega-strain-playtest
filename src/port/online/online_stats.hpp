#pragma once
// Checkpoint 132on2d (research/ONLINE.md §3.6): the agent statistics the original client reports to the lobby server,
// and omega_server's PORT-ONLY store of them.
//
// The ORIGINAL side (decoded; each address in ONLINE.md §3.6): the client keeps one 0xfc-byte statistics record per
// agent (the "stats blob"), built by 002cbd40 from the selected agent (Agent_GetSelected 0x4ffb50) and tagged with
// version byte 0xb (00450880 keeps only records whose byte 0 is 0xb). It sends it in three messages:
//  * AccountUpdateStats (17 -> 18, 004505e0): after the login (0044ce60), at each game entry (0044e340 / 0044e430 /
//    0044e960), after the agent edit (0033fa80) and every SFOLOBBY_UPDATE_PLAYERSTATS_INTERVAL in the lobby (00451b30);
//  * PlayerReport (1, 004446e0, no response): every 15 seconds while in a network game, with the world id;
//  * WorldReport (0, 00444944, no response): the host's report of its world (Medius' standard layout).
// It reads records back with GetAccountStats (49 -> 50, 00451200 / 0044df20) into a cache (session +fc, 00450880 /
// 00450cf0), which JOINMIS' list draws the rank column from (002ab0f8) and the agent info screen reads (0033e460).
// The original Medius servers kept each account's record; nothing in the executable asks for rankings or ladders
// (no caller of the MCL ladder calls), so the record is all the original "stats" are.
//
// The PORT-ONLY side: StatsStore, omega_server's durable copy (one record per account, the reports counted) and the
// results of finished games (each player's record at the game's end and its change since the game's entry: the
// "mission results"). Text file, written atomically with rotated backups (online_files.hpp).

#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::online {

// ---- The record (002cbd40) -------------------------------------------------------------------------------------
constexpr std::size_t kStatsBlobSize = 0xfc;
constexpr std::uint8_t kStatsBlobVersion = 0x0b;  // byte 0 (002cbd40 writes it; 00450880 requires it)
using StatsBlob = std::array<std::uint8_t, kStatsBlobSize>;

// The decoded fields (offsets in ONLINE.md §3.6). The statistics are the agent record's (00333ee0 index): 0 kills,
// 8 deaths (PlayerDeaths), 9 shots, 0xa hits, 0xb aim-view shots, 0xc aim-view hits; 1 / 2 / 4 / 6 / 7 / 0xd are
// further kill and hit counters (npc_death_notice.cpp, combat_play.cpp).
struct StatsFields {
    std::uint8_t version = 0;
    std::uint8_t rank = 0;                // +1: the rank row (agent +b50 +4; JOINMIS' rank column, 002ab0f8)
    std::uint8_t aim_accuracy = 0;        // +2: 100 * stat 0xc / stat 0xb
    std::uint8_t accuracy = 0;            // +3: 100 * stat 0xa / stat 9
    std::uint16_t stat13 = 0, stat1 = 0, stat4 = 0, stat6 = 0, stat7 = 0;  // +4 / +6 / +8 / +a / +c
    std::uint32_t shots = 0, hits = 0, stat2 = 0, kills = 0, deaths = 0;   // +10 / +14 / +18 / +1c / +20
    std::uint32_t ratings = 0;            // +24: bits 0..23 the rating rows (00333600), 24..31 the special ones (003334c0)
    // The tail the upload sites write after 002cbd40 (ONLINE.md §3.6): the login's tail 0044ce60 and the join's end
    // 0044e340 write +e8 / +f4 and zero +ec / +f0 / +f9 with +f8 = 5; the join 0044e430 and the create 0044e960 write
    // +ec / +f0 / +f8 / +f9 over the record as it stands.
    // +e8: session +2484, the agent's clan (agency cell) id, -1 none: GetMyClans 00449a00's callback 00448b70 (132on2i;
    // the 132on2d name "lobby world" was wrong). AGNTINFO's 00360610 reads it back to name the cell (00448ff0).
    std::int32_t clan_id = -1;
    std::uint32_t host_account = 0;       // +ec: the game's host account (the join: game row +c; the create: own id)
    std::uint32_t game_world = 0;         // +f0: the world joined (the create: *004976d8)
    std::uint32_t client_ip = 0;          // +f4: 00443020(session +2429), the client's own IPv4 "%d.%d.%d.%d"
    std::uint8_t filter = 0;              // +f8: the game's filter (5 outside a game)
    std::uint8_t mission = 0;             // +f9: the game's mission
};
constexpr std::uint8_t kStatsFilterNone = 5;  // +f8 outside a game (0044ce60 / 0044e340)
StatsFields decode_stats(std::span<const std::uint8_t> blob);
// "rank 3, accuracy 41% (120 shots, 49 hits), aim 0%, kills 12, deaths 2, ...".
std::string describe_stats(std::span<const std::uint8_t> blob);
// "+5 shots, +2 hits, +1 kills" between two records (only the counters that changed; "no change").
std::string describe_stats_change(std::span<const std::uint8_t> before, std::span<const std::uint8_t> after);
bool valid_stats(std::span<const std::uint8_t> blob);  // kStatsBlobSize bytes or more, byte 0 kStatsBlobVersion

// ---- PORT-ONLY: omega_server's store -----------------------------------------------------------------------------
struct AccountStatsRecord {
    std::uint32_t account = 0;
    StatsBlob blob{};
    std::uint64_t updated = 0;   // Unix seconds
    std::uint32_t reports = 0;   // records received (AccountUpdateStats and PlayerReport)
    std::uint32_t games = 0;     // finished games
};

struct GameResultPlayer {
    std::uint32_t account = 0;
    std::string name;
    StatsBlob entry{};   // the record at the game's entry (zero: none received)
    StatsBlob final{};   // the last record received in the game
};

struct GameResult {
    std::uint64_t ended = 0;
    std::uint32_t world = 0;
    std::uint64_t seconds = 0;   // from the create to the end
    std::uint32_t host = 0;
    std::string game_name;
    std::string why;             // "host left", "closed by an admin", ...
    std::vector<GameResultPlayer> players;
};

// File format (UTF-8 text, tab separated, '#' comments; the text fields escape \\, tab, newline and CR as \\ \t \n \r,
// and "-" is an empty one):
//   stats  <account> <updated> <reports> <games> <blob hex>
//   result <ended> <world> <seconds> <host> <why> <game name> <n> then n x (<account> <name> <entry hex> <final hex>)
class StatsStore {
public:
    explicit StatsStore(std::filesystem::path file = {}, int backups = 3, std::size_t max_results = 200,
                        std::uint64_t backup_interval = 3600);
    void load();  // a missing file is empty; a malformed line throws std::runtime_error
    void save() const;

    // Keeps a valid record (false: not version 0xb, ignored).
    bool update(std::uint32_t account, std::span<const std::uint8_t> blob, std::uint64_t now);
    const AccountStatsRecord* find(std::uint32_t account) const;
    void add_result(GameResult result);  // counts a game for each player; keeps the newest max_results
    const std::vector<GameResult>& results() const { return results_; }
    const std::map<std::uint32_t, AccountStatsRecord>& records() const { return records_; }

private:
    std::filesystem::path file_;
    int backups_;
    std::size_t max_results_;
    std::uint64_t backup_interval_;
    std::map<std::uint32_t, AccountStatsRecord> records_;
    std::vector<GameResult> results_;
};

}  // namespace omega::online
