#pragma once
// Checkpoint 132on2g (research/ONLINE.md §3.7 / §6.15): omega_server's social store and its chat filter.
//
//  * The contact (buddy) lists and the pending contact invitations: the server side of the original's
//    SFOLobby_Contact.cc flows (AddToBuddyListConfirmation 258 -> 68 and 259 / 260, GetBuddyInvitations 261 -> 262,
//    GetBuddyList_ExtraInfo 214 -> 215, RemoveFromBuddyList 65 -> 66). The wire is original; how the server keeps
//    them is PORT-ONLY (Medius' own database is unknown): an accepted invitation makes both agents contacts of each
//    other, a removal is one-sided (the other keeps its contact until it removes it too).
//  * The recent players (PORT-ONLY: the original has no such list): the agents each account last played a game with,
//    newest first, kept to kMaxRecent.
//  * The chat filter (the original's TextFilter 152 -> 153 is a server-side filter; its word list is the server's):
//    a word that starts with a listed word is masked with '*' (PORT-ONLY: the original's list is unknown).
//
// Kept in one text file ("<file>.tmp" + rename, online_files.hpp), separate from the accounts.

#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace omega::online {

class SocialStore {
public:
    static constexpr std::size_t kMaxRecent = 20;
    static constexpr std::size_t kMaxContacts = 100;
    static constexpr std::size_t kMaxInvitations = 50;  // pending, per invitee

    struct Invitation {
        std::uint32_t from = 0;
        std::uint64_t when = 0;
    };
    struct Recent {
        std::uint32_t account = 0;
        std::uint64_t when = 0;
        std::uint32_t world = 0;
    };

    explicit SocialStore(std::filesystem::path file = {}, int backups = 0, std::uint64_t backup_interval = 0)
        : file_(std::move(file)), backups_(backups), backup_interval_(backup_interval) {}

    void load();        // throws std::runtime_error on a malformed file
    void save() const;  // throws std::runtime_error when the file cannot be written
    const std::filesystem::path& file() const { return file_; }

    const std::set<std::uint32_t>& contacts(std::uint32_t account) const;
    bool is_contact(std::uint32_t owner, std::uint32_t other) const;
    // False when either list is full.
    bool add_contacts(std::uint32_t a, std::uint32_t b);
    bool remove_contact(std::uint32_t owner, std::uint32_t other);  // one-sided

    // False when it is already pending or the invitee's list is full.
    bool invite(std::uint32_t from, std::uint32_t to, std::uint64_t now);
    const std::vector<Invitation>& invitations(std::uint32_t to) const;
    bool take_invitation(std::uint32_t from, std::uint32_t to);  // false when there was none

    void played_with(std::uint32_t owner, std::uint32_t other, std::uint32_t world, std::uint64_t now);
    const std::vector<Recent>& recent(std::uint32_t owner) const;

    // An account was deleted: every trace of it goes.
    void forget(std::uint32_t account);

private:
    std::filesystem::path file_;
    int backups_ = 0;
    std::uint64_t backup_interval_ = 0;
    std::map<std::uint32_t, std::set<std::uint32_t>> contacts_;
    std::map<std::uint32_t, std::vector<Invitation>> invitations_;  // by invitee, oldest first
    std::map<std::uint32_t, std::vector<Recent>> recent_;          // newest first
};

class ChatFilter {
public:
    ChatFilter();  // the built-in word list
    // Replaces the list: one word per line, '#' comments; a "+" first line keeps the built-in words too.
    void load_words(std::string_view text);
    // Each word (a run of letters / digits) whose lowercase form starts with a listed word (or, for a listed "=word",
    // equals it) becomes '*'s.
    std::string apply(std::string_view text) const;
    bool changes(std::string_view text) const { return apply(text) != text; }
    std::size_t size() const { return words_.size(); }

private:
    std::vector<std::string> words_;
};

}  // namespace omega::online
