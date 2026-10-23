// Checkpoint 132on2g: see online_social.hpp.
#include "online_social.hpp"

#include "online_files.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace omega::online {

namespace {

const std::set<std::uint32_t> kNoContacts;
const std::vector<SocialStore::Invitation> kNoInvitations;
const std::vector<SocialStore::Recent> kNoRecent;

std::vector<std::string> split_tabs(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    for (;;) {
        const auto tab = line.find('\t', start);
        out.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
        if (tab == std::string::npos) break;
        start = tab + 1;
    }
    return out;
}

std::uint32_t to_u32(const std::string& s) {
    std::size_t used = 0;
    const unsigned long v = std::stoul(s, &used);
    if (used != s.size() || v == 0 || v > 0xffffffffUL) throw std::invalid_argument("id");
    return static_cast<std::uint32_t>(v);
}

std::uint64_t to_u64(const std::string& s) {
    std::size_t used = 0;
    const unsigned long long v = std::stoull(s, &used);
    if (used != s.size()) throw std::invalid_argument("number");
    return v;
}

}  // namespace

void SocialStore::load() {
    contacts_.clear();
    invitations_.clear();
    recent_.clear();
    if (file_.empty() || !std::filesystem::exists(file_)) return;
    std::ifstream in(file_, std::ios::binary);
    if (!in) throw std::runtime_error("social: cannot read " + file_.string());
    std::string line;
    int number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        const auto f = split_tabs(line);
        try {
            if (f[0] == "contact" && f.size() == 3) {
                contacts_[to_u32(f[1])].insert(to_u32(f[2]));
            } else if (f[0] == "invite" && f.size() == 4) {
                invitations_[to_u32(f[2])].push_back({to_u32(f[1]), to_u64(f[3])});
            } else if (f[0] == "recent" && f.size() == 5) {
                recent_[to_u32(f[1])].push_back({to_u32(f[2]), to_u64(f[3]), static_cast<std::uint32_t>(to_u64(f[4]))});
            } else {
                throw std::invalid_argument("kind");
            }
        } catch (const std::exception&) {
            throw std::runtime_error("social: malformed line " + std::to_string(number));
        }
    }
}

void SocialStore::save() const {
    if (file_.empty()) return;
    std::ostringstream out;
    out << "# omega_server contacts, contact invitations and recent players (132on2g).\n";
    out << "# contact\towner\tother | invite\tfrom\tto\twhen | recent\towner\tother\twhen\tworld\n";
    for (const auto& [owner, set] : contacts_)
        for (const auto other : set) out << "contact\t" << owner << '\t' << other << '\n';
    for (const auto& [to, list] : invitations_)
        for (const auto& i : list) out << "invite\t" << i.from << '\t' << to << '\t' << i.when << '\n';
    for (const auto& [owner, list] : recent_)
        for (const auto& r : list)
            out << "recent\t" << owner << '\t' << r.account << '\t' << r.when << '\t' << r.world << '\n';
    write_file_atomic(file_, out.str(), backups_, backup_interval_);
}

const std::set<std::uint32_t>& SocialStore::contacts(std::uint32_t account) const {
    const auto it = contacts_.find(account);
    return it == contacts_.end() ? kNoContacts : it->second;
}

bool SocialStore::is_contact(std::uint32_t owner, std::uint32_t other) const {
    return contacts(owner).count(other) != 0;
}

bool SocialStore::add_contacts(std::uint32_t a, std::uint32_t b) {
    if (a == b) return false;
    auto& la = contacts_[a];
    auto& lb = contacts_[b];
    if ((!la.count(b) && la.size() >= kMaxContacts) || (!lb.count(a) && lb.size() >= kMaxContacts)) return false;
    la.insert(b);
    lb.insert(a);
    return true;
}

bool SocialStore::remove_contact(std::uint32_t owner, std::uint32_t other) {
    const auto it = contacts_.find(owner);
    if (it == contacts_.end() || it->second.erase(other) == 0) return false;
    if (it->second.empty()) contacts_.erase(it);
    return true;
}

bool SocialStore::invite(std::uint32_t from, std::uint32_t to, std::uint64_t now) {
    if (from == to) return false;
    auto& list = invitations_[to];
    for (const auto& i : list)
        if (i.from == from) return false;
    if (list.size() >= kMaxInvitations) return false;
    list.push_back({from, now});
    return true;
}

const std::vector<SocialStore::Invitation>& SocialStore::invitations(std::uint32_t to) const {
    const auto it = invitations_.find(to);
    return it == invitations_.end() ? kNoInvitations : it->second;
}

bool SocialStore::take_invitation(std::uint32_t from, std::uint32_t to) {
    const auto it = invitations_.find(to);
    if (it == invitations_.end()) return false;
    auto& list = it->second;
    const auto i = std::find_if(list.begin(), list.end(), [&](const Invitation& v) { return v.from == from; });
    if (i == list.end()) return false;
    list.erase(i);
    if (list.empty()) invitations_.erase(it);
    return true;
}

void SocialStore::played_with(std::uint32_t owner, std::uint32_t other, std::uint32_t world, std::uint64_t now) {
    if (owner == other || owner == 0 || other == 0) return;
    auto& list = recent_[owner];
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Recent& r) { return r.account == other; }), list.end());
    list.insert(list.begin(), {other, now, world});
    if (list.size() > kMaxRecent) list.resize(kMaxRecent);
}

const std::vector<SocialStore::Recent>& SocialStore::recent(std::uint32_t owner) const {
    const auto it = recent_.find(owner);
    return it == recent_.end() ? kNoRecent : it->second;
}

void SocialStore::forget(std::uint32_t account) {
    contacts_.erase(account);
    for (auto& [owner, set] : contacts_) set.erase(account);
    invitations_.erase(account);
    for (auto& [to, list] : invitations_)
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Invitation& i) { return i.from == account; }),
                   list.end());
    recent_.erase(account);
    for (auto& [owner, list] : recent_)
        list.erase(std::remove_if(list.begin(), list.end(), [&](const Recent& r) { return r.account == account; }),
                   list.end());
}

// ---- ChatFilter -------------------------------------------------------------------------------------------------

namespace {
// The built-in list: the common English profanity stems (a word STARTING with one is masked, so "fucking" goes but
// "Scunthorpe" or "class" do not); a word written "=word" is masked only whole (so "cockpit" stays). An operator's
// chat_filter_words file replaces or extends it.
const char* const kBuiltinWords[] = {"fuck", "shit", "cunt", "bitch", "bastard", "asshole", "=dick", "=dicks",
                                     "=cock", "=cocks", "pussy", "whore", "slut", "=fag", "=fags", "faggot",
                                     "nigger", "nigga", "retard", "wank", "twat", "motherfuck", "bollock", "piss"};

bool word_char(unsigned char c) { return std::isalnum(c) != 0; }
}  // namespace

ChatFilter::ChatFilter() {
    for (const char* w : kBuiltinWords) words_.emplace_back(w);
}

void ChatFilter::load_words(std::string_view text) {
    std::vector<std::string> words;
    bool keep_builtin = false;
    bool first = true;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        std::string w(text.substr(start, end - start));
        start = end + 1;
        while (!w.empty() && std::isspace(static_cast<unsigned char>(w.back()))) w.pop_back();
        std::size_t lead = 0;
        while (lead < w.size() && std::isspace(static_cast<unsigned char>(w[lead]))) ++lead;
        w.erase(0, lead);
        if (w.empty() || w[0] == '#') continue;
        if (first && w == "+") {
            keep_builtin = true;
            first = false;
            continue;
        }
        first = false;
        for (auto& ch : w) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        words.push_back(std::move(w));
        if (end == text.size()) break;
    }
    if (keep_builtin)
        for (const char* w : kBuiltinWords) words.emplace_back(w);
    words_ = std::move(words);
}

std::string ChatFilter::apply(std::string_view text) const {
    std::string out(text);
    std::size_t i = 0;
    while (i < out.size()) {
        if (!word_char(static_cast<unsigned char>(out[i]))) {
            ++i;
            continue;
        }
        std::size_t j = i;
        std::string lower;
        while (j < out.size() && word_char(static_cast<unsigned char>(out[j]))) {
            lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(out[j]))));
            ++j;
        }
        for (const auto& w : words_)
            if (w.size() > 1 && w[0] == '=' ? lower == std::string_view(w).substr(1)
                                            : !w.empty() && lower.compare(0, w.size(), w) == 0) {
                std::fill(out.begin() + static_cast<std::ptrdiff_t>(i), out.begin() + static_cast<std::ptrdiff_t>(j), '*');
                break;
            }
        i = j;
    }
    return out;
}

}  // namespace omega::online
