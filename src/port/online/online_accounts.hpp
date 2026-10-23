#pragma once
// PORT-ONLY (not in the original; checkpoint 132on1, research/ONLINE.md §5.2): omega_server's account store. The
// original accounts lived on SCEA's Medius servers; the client only sends AccountRegistration (id 9, 0x00445b90) and
// AccountLogin (id 7, 0x00445420) with a name and a password (32 bytes each). This store keeps, per account, a salted
// PBKDF2-HMAC-SHA-256 digest of the password (never the text), the admin flag and the ban (until when, why).
//
// File format (UTF-8 text, one account per line, tab separated; '#' lines are comments):
//   account <id> <name> <salt hex> <digest hex> <iterations> <admin 0|1> <ban_until> <created> <accepted policy version> <ban reason>
// ban_until: 0 = not banned, kBanForever = permanent, else a Unix time in seconds. The reason is the rest of the line
// (tabs and newlines in a reason are written as spaces). Writes go to "<file>.tmp" and replace the file; checkpoint
// 132on2d keeps rotated backups ("<file>.bak.1" newest, online_files.hpp).
//
// Hashing (132on2d review, ONLINE.md §6.10): PBKDF2-HMAC-SHA-256, a 16-byte random salt per digest, 32-byte digests
// compared in constant time; the iteration count is stored per account, so raising the server's count (the default
// is kDefaultPbkdf2Iterations) upgrades each account's digest at its next successful login (upgrade_hash).

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace omega::online {

constexpr std::uint64_t kBanForever = ~std::uint64_t{0};
constexpr std::size_t kMaxAccountName = 31;  // the request's AccountName[32] with its NUL
constexpr std::size_t kMaxPassword = 31;     // Password[32]
constexpr std::uint32_t kDefaultPbkdf2Iterations = 120000;  // 132on2d (was 20000; ONLINE.md §6.10)

struct Account {
    std::uint32_t id = 0;
    std::string name;
    std::string salt_hex;
    std::string digest_hex;
    std::uint32_t iterations = 0;
    bool admin = false;
    std::uint64_t ban_until = 0;
    std::uint64_t created = 0;
    std::uint32_t accepted_policy = 0;  // the policy version accepted at the last login (0: never)
    std::string ban_reason;

    bool banned_at(std::uint64_t now) const { return ban_until != 0 && (ban_until == kBanForever || now < ban_until); }
};

enum class CreateResult { Created, NameTaken, BadName, BadPassword };
enum class CheckResult { Ok, NotFound, WrongPassword, Banned };

class AccountStore {
public:
    // file empty: memory only (tests). iterations: the PBKDF2 cost of new digests.
    explicit AccountStore(std::filesystem::path file = {}, std::uint32_t iterations = kDefaultPbkdf2Iterations);
    // Rotated backups kept by save() (0: none), at most one per interval (seconds; 0: at every save).
    void set_backups(int count, std::uint64_t interval_seconds) { backups_ = count, backup_interval_ = interval_seconds; }
    std::uint32_t iterations() const { return iterations_; }

    void load();  // a missing file is an empty store; a malformed line throws std::runtime_error
    void save() const;

    CreateResult create(std::string_view name, std::string_view password, std::uint64_t now, std::uint32_t* id_out = nullptr);
    CheckResult check(std::string_view name, std::string_view password, std::uint64_t now, const Account** out = nullptr) const;

    // 132on2h: the same two operations with the PBKDF2 work taken out, so a server can run it on worker threads
    // (pbkdf2_sha256(password, params.salt, params.iterations, 32) is pure): create() == the checks, new_hash_params,
    // the digest, create_with_digest; check() == hash_params, the digest, check_digest.
    struct HashParams {
        std::vector<std::uint8_t> salt;
        std::uint32_t iterations = 0;
    };
    HashParams new_hash_params() const;                     // a fresh random salt at the store's cost
    HashParams hash_params(std::string_view name) const;    // the account's; an unknown name gets a dummy of equal cost
    CreateResult create_with_digest(std::string_view name, const HashParams& params, std::span<const std::uint8_t> digest,
                                    std::uint64_t now, std::uint32_t* id_out = nullptr);
    CheckResult check_digest(std::string_view name, std::span<const std::uint8_t> digest, std::uint64_t now,
                             const Account** out = nullptr) const;
    // After a successful check: rehashes the password with the store's iteration count when the account's digest
    // used fewer (true: changed, save the store).
    bool upgrade_hash(std::uint32_t id, std::string_view password);

    const Account* find(std::string_view name) const;  // case-insensitive
    const Account* find(std::uint32_t id) const;
    bool set_admin(std::string_view name, bool admin);
    bool ban(std::string_view name, std::uint64_t until, std::string_view reason);
    bool unban(std::string_view name);
    bool set_password(std::string_view name, std::string_view password);
    bool set_accepted_policy(std::uint32_t id, std::uint32_t version);
    const std::vector<Account>& accounts() const { return accounts_; }

    static bool valid_name(std::string_view name);
    static bool valid_password(std::string_view password);

private:
    Account* find_mutable(std::string_view name);
    void hash_into(Account& a, std::string_view password) const;

    std::filesystem::path file_;
    std::uint32_t iterations_;
    int backups_ = 0;
    std::uint64_t backup_interval_ = 0;
    std::vector<Account> accounts_;
    std::uint32_t next_id_ = 1;
};

}  // namespace omega::online
