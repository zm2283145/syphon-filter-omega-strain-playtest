// PORT-ONLY (checkpoint 132on1): see online_accounts.hpp.
#include "online_accounts.hpp"

#include "online_files.hpp"
#include "online_hash.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace omega::online {
namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> split_tabs(const std::string& line, std::size_t max_fields) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (out.size() + 1 < max_fields) {
        const std::size_t tab = line.find('\t', start);
        if (tab == std::string::npos) break;
        out.push_back(line.substr(start, tab - start));
        start = tab + 1;
    }
    out.push_back(line.substr(start));
    return out;
}

std::string one_line(std::string_view s) {
    std::string out(s);
    for (char& c : out)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return out;
}

}  // namespace

AccountStore::AccountStore(std::filesystem::path file, std::uint32_t iterations)
    : file_(std::move(file)), iterations_(iterations ? iterations : 1) {}

bool AccountStore::valid_name(std::string_view name) {
    if (name.empty() || name.size() > kMaxAccountName) return false;
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7f || c == '\t') return false;
    }
    return name.front() != ' ' && name.back() != ' ';
}

bool AccountStore::valid_password(std::string_view password) {
    return !password.empty() && password.size() <= kMaxPassword;
}

void AccountStore::load() {
    accounts_.clear();
    next_id_ = 1;
    if (file_.empty() || !std::filesystem::exists(file_)) return;
    std::ifstream in(file_, std::ios::binary);
    if (!in) throw std::runtime_error("accounts: cannot read " + file_.string());
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto f = split_tabs(line, 11);
        if (f.size() != 11 || f[0] != "account")
            throw std::runtime_error("accounts: malformed line " + std::to_string(number));
        Account a;
        try {
            a.id = static_cast<std::uint32_t>(std::stoul(f[1]));
            a.name = f[2];
            a.salt_hex = f[3];
            a.digest_hex = f[4];
            a.iterations = static_cast<std::uint32_t>(std::stoul(f[5]));
            a.admin = f[6] == "1";
            a.ban_until = std::stoull(f[7]);
            a.created = std::stoull(f[8]);
            a.accepted_policy = static_cast<std::uint32_t>(std::stoul(f[9]));
            a.ban_reason = f[10];
            (void)from_hex(a.salt_hex);
            (void)from_hex(a.digest_hex);
        } catch (const std::exception&) {
            throw std::runtime_error("accounts: malformed line " + std::to_string(number));
        }
        if (!valid_name(a.name) || a.iterations == 0)
            throw std::runtime_error("accounts: malformed line " + std::to_string(number));
        next_id_ = std::max(next_id_, a.id + 1);
        accounts_.push_back(std::move(a));
    }
}

void AccountStore::save() const {
    if (file_.empty()) return;
    std::ostringstream out;
    out << "# omega_server accounts (PORT-ONLY). Passwords are salted PBKDF2-HMAC-SHA-256 digests.\n";
    out << "# account\tid\tname\tsalt\tdigest\titerations\tadmin\tban_until\tcreated\taccepted_policy\tban_reason\n";
    for (const auto& a : accounts_)
        out << "account\t" << a.id << '\t' << a.name << '\t' << a.salt_hex << '\t' << a.digest_hex << '\t'
            << a.iterations << '\t' << (a.admin ? 1 : 0) << '\t' << a.ban_until << '\t' << a.created << '\t'
            << a.accepted_policy << '\t' << one_line(a.ban_reason) << '\n';
    write_file_atomic(file_, out.str(), backups_, backup_interval_);
}

void AccountStore::hash_into(Account& a, std::string_view password) const {
    const auto salt = random_bytes(16);
    a.salt_hex = to_hex(salt);
    a.iterations = iterations_;
    a.digest_hex = to_hex(pbkdf2_sha256(password, salt, a.iterations, 32));
}

CreateResult AccountStore::create(std::string_view name, std::string_view password, std::uint64_t now,
                                  std::uint32_t* id_out) {
    if (!valid_name(name)) return CreateResult::BadName;
    if (!valid_password(password)) return CreateResult::BadPassword;
    if (find(name)) return CreateResult::NameTaken;
    const HashParams params = new_hash_params();
    return create_with_digest(name, params, pbkdf2_sha256(password, params.salt, params.iterations, 32), now, id_out);
}

AccountStore::HashParams AccountStore::new_hash_params() const { return HashParams{random_bytes(16), iterations_}; }

AccountStore::HashParams AccountStore::hash_params(std::string_view name) const {
    // 132on2d review: an unknown name costs the same PBKDF2 work as a real check, so the time does not tell which
    // names exist (the status still does: the original client shows ACCOUNT_NOT_FOUND, 0x004475e0; the login
    // throttle bounds it).
    if (const Account* a = find(name)) return HashParams{from_hex(a->salt_hex), a->iterations};
    return HashParams{std::vector<std::uint8_t>(16, 0x5a), iterations_};
}

CreateResult AccountStore::create_with_digest(std::string_view name, const HashParams& params,
                                              std::span<const std::uint8_t> digest, std::uint64_t now,
                                              std::uint32_t* id_out) {
    if (!valid_name(name)) return CreateResult::BadName;
    if (find(name)) return CreateResult::NameTaken;  // (taken while the digest was computed)
    Account a;
    a.id = next_id_++;
    a.name = std::string(name);
    a.created = now;
    a.salt_hex = to_hex(params.salt);
    a.iterations = params.iterations;
    a.digest_hex = to_hex(digest);
    if (id_out) *id_out = a.id;
    accounts_.push_back(std::move(a));
    return CreateResult::Created;
}

CheckResult AccountStore::check(std::string_view name, std::string_view password, std::uint64_t now,
                                const Account** out) const {
    const HashParams params = hash_params(name);
    return check_digest(name, pbkdf2_sha256(password, params.salt, params.iterations, 32), now, out);
}

CheckResult AccountStore::check_digest(std::string_view name, std::span<const std::uint8_t> digest, std::uint64_t now,
                                       const Account** out) const {
    const Account* a = find(name);
    if (out) *out = a;
    if (!a) return CheckResult::NotFound;
    if (!equal_digest(digest, from_hex(a->digest_hex))) return CheckResult::WrongPassword;
    if (a->banned_at(now)) return CheckResult::Banned;
    return CheckResult::Ok;
}

bool AccountStore::upgrade_hash(std::uint32_t id, std::string_view password) {
    Account* a = const_cast<Account*>(find(id));
    if (!a || a->iterations >= iterations_ || !valid_password(password)) return false;
    hash_into(*a, password);
    return true;
}

const Account* AccountStore::find(std::string_view name) const {
    // 132on2h: compared in place, without a lowered copy of every account name per lookup.
    auto same = [&](const std::string& other) {
        if (other.size() != name.size()) return false;
        for (std::size_t i = 0; i < name.size(); ++i)
            if (std::tolower(static_cast<unsigned char>(other[i])) != std::tolower(static_cast<unsigned char>(name[i])))
                return false;
        return true;
    };
    for (const auto& a : accounts_)
        if (same(a.name)) return &a;
    return nullptr;
}

const Account* AccountStore::find(std::uint32_t id) const {
    for (const auto& a : accounts_)
        if (a.id == id) return &a;
    return nullptr;
}

Account* AccountStore::find_mutable(std::string_view name) { return const_cast<Account*>(find(name)); }

bool AccountStore::set_admin(std::string_view name, bool admin) {
    Account* a = find_mutable(name);
    if (!a) return false;
    a->admin = admin;
    return true;
}

bool AccountStore::ban(std::string_view name, std::uint64_t until, std::string_view reason) {
    Account* a = find_mutable(name);
    if (!a || until == 0) return false;
    a->ban_until = until;
    a->ban_reason = one_line(reason);
    return true;
}

bool AccountStore::unban(std::string_view name) {
    Account* a = find_mutable(name);
    if (!a) return false;
    a->ban_until = 0;
    a->ban_reason.clear();
    return true;
}

bool AccountStore::set_accepted_policy(std::uint32_t id, std::uint32_t version) {
    Account* a = const_cast<Account*>(find(id));
    if (!a) return false;
    a->accepted_policy = version;
    return true;
}

bool AccountStore::set_password(std::string_view name, std::string_view password) {
    Account* a = find_mutable(name);
    if (!a || !valid_password(password)) return false;
    hash_into(*a, password);
    return true;
}

}  // namespace omega::online
