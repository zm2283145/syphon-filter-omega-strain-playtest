// Port-only (checkpoint 132md10): see mod_archive.hpp.
#include "mod_archive.hpp"

#include "image_files.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <stdexcept>

namespace omega::mods {
namespace {

namespace fs = std::filesystem;

constexpr std::uint32_t local_signature = 0x04034b50, central_signature = 0x02014b50, end_signature = 0x06054b50;
constexpr std::uint16_t dos_date_1980 = (0 << 9) | (1 << 5) | 1;   // 1980-01-01
constexpr std::uint32_t member_size_limit = 1u << 31;

std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t at) {
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}
std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return std::uint32_t(b[at]) | std::uint32_t(b[at + 1]) << 8 | std::uint32_t(b[at + 2]) << 16 | std::uint32_t(b[at + 3]) << 24;
}
void put16(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}
void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

std::string lower_ascii(std::string_view text) {
    std::string result(text);
    for (auto& c : result) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

// The member name with '\' -> '/', or an error text when it is empty, absolute or has an empty, "." or ".." component.
std::string checked_name(std::string name, std::string& problem) {
    std::replace(name.begin(), name.end(), '\\', '/');
    if (name.empty() || name.front() == '/' || name.find(':') != std::string::npos) {
        problem = "an absolute or empty member name";
        return name;
    }
    std::size_t begin = 0;
    const auto end = name.ends_with('/') ? name.size() - 1 : name.size();
    while (begin <= end) {
        auto slash = name.find('/', begin);
        if (slash == std::string::npos || slash > end) slash = end;
        const auto part = std::string_view(name).substr(begin, slash - begin);
        if (part.empty() || part == "." || part == "..") {
            problem = "an empty, '.' or '..' path component";
            return name;
        }
        begin = slash + 1;
    }
    return name;
}

[[noreturn]] void fail(const fs::path& path, const std::string& what) {
    throw std::runtime_error("Mod archive " + path.filename().string() + ": " + what);
}

// Fixed-Huffman deflate output (RFC 1951 3.2.6): bits are packed from the least significant end; Huffman codes go
// most significant bit first.
class BitWriter {
public:
    std::vector<std::uint8_t> out;
    void put(std::uint32_t bits, unsigned count) {
        buffer_ |= std::uint64_t(bits) << count_;
        count_ += count;
        while (count_ >= 8) {
            out.push_back(static_cast<std::uint8_t>(buffer_));
            buffer_ >>= 8;
            count_ -= 8;
        }
    }
    void code(std::uint32_t code, unsigned length) {
        std::uint32_t reversed = 0;
        for (unsigned i = 0; i < length; ++i) reversed |= ((code >> i) & 1u) << (length - 1 - i);
        put(reversed, length);
    }
    void flush() {
        if (count_) out.push_back(static_cast<std::uint8_t>(buffer_));
        buffer_ = 0;
        count_ = 0;
    }

private:
    std::uint64_t buffer_ = 0;
    unsigned count_ = 0;
};

void literal(BitWriter& w, unsigned symbol) {
    if (symbol < 144) w.code(0x30 + symbol, 8);
    else if (symbol < 256) w.code(0x190 + symbol - 144, 9);
    else if (symbol < 280) w.code(symbol - 256, 7);
    else w.code(0xC0 + symbol - 280, 8);
}

constexpr std::uint16_t length_base[29]{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                        31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::uint8_t length_extra[29]{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::uint16_t distance_base[30]{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                          193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::uint8_t distance_extra[30]{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

void match(BitWriter& w, unsigned length, unsigned distance) {
    unsigned l = 28;
    while (length_base[l] > length) --l;
    literal(w, 257 + l);
    w.put(length - length_base[l], length_extra[l]);
    unsigned d = 29;
    while (distance_base[d] > distance) --d;
    w.code(d, 5);
    w.put(distance - distance_base[d], distance_extra[d]);
}

struct Registered {
    std::string key;   // the absolute normalized path, generic, lower case
    std::shared_ptr<const ZipArchive> archive;
};

struct Registry {
    std::mutex mutex;
    std::vector<Registered> archives;
};
Registry& registry() {
    static Registry r;
    return r;
}
std::atomic<bool> any_registered{false};

std::string utf8(const std::u8string& text) { return std::string(text.begin(), text.end()); }
fs::path from_utf8(std::string_view text) { return fs::path(std::u8string(text.begin(), text.end())); }

std::string path_key(const fs::path& path) { return lower_ascii(utf8(path.lexically_normal().generic_u8string())); }

// The registered archive at or above `path` (equal allowed when `or_equal`), the rest of the path in `member`.
std::shared_ptr<const ZipArchive> locate(const fs::path& path, std::string& member, bool or_equal) {
    if (!any_registered.load(std::memory_order_relaxed)) return nullptr;
    const auto normal = utf8(path.lexically_normal().generic_u8string());
    const auto key = lower_ascii(normal);
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    for (const auto& entry : r.archives) {
        if (!key.starts_with(entry.key)) continue;
        if (key.size() == entry.key.size()) {
            if (!or_equal) continue;
            member.clear();
            return entry.archive;
        }
        if (key[entry.key.size()] != '/') continue;
        member = normal.substr(entry.key.size() + 1);
        while (member.ends_with('/')) member.pop_back();
        return entry.archive;
    }
    return nullptr;
}

}  // namespace

std::shared_ptr<const ZipArchive> ZipArchive::open(const fs::path& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) fail(path, "cannot open");
    const auto file_size = static_cast<std::uint64_t>(in.tellg());
    if (file_size < 22) fail(path, "too short for a zip archive");
    if (file_size >= 0xFFFFFFFFull) fail(path, "4 GiB or larger (zip64 is not read)");
    const auto tail_size = static_cast<std::size_t>(std::min<std::uint64_t>(file_size, 22 + 0xFFFF));
    std::vector<std::uint8_t> tail(tail_size);
    in.seekg(static_cast<std::streamoff>(file_size - tail_size));
    if (!in.read(reinterpret_cast<char*>(tail.data()), static_cast<std::streamsize>(tail.size()))) fail(path, "cannot read");
    std::size_t eocd = std::string::npos;
    for (std::size_t at = tail.size() - 22 + 1; at-- > 0;)
        if (le32(tail, at) == end_signature && at + 22 + le16(tail, at + 20) <= tail.size()) {
            eocd = at;
            break;
        }
    if (eocd == std::string::npos) fail(path, "not a zip archive (no end of central directory record)");
    const auto disk = le16(tail, eocd + 4), cd_disk = le16(tail, eocd + 6);
    const auto entries_here = le16(tail, eocd + 8), entries = le16(tail, eocd + 10);
    const auto cd_size = le32(tail, eocd + 12), cd_offset = le32(tail, eocd + 16);
    if (entries == 0xFFFF || cd_size == 0xFFFFFFFFu || cd_offset == 0xFFFFFFFFu) fail(path, "zip64 archives are not read");
    if (disk != 0 || cd_disk != 0 || entries_here != entries) fail(path, "multi-part archives are not read");
    if (std::uint64_t(cd_offset) + cd_size > file_size) fail(path, "central directory past the end of the file");
    std::vector<std::uint8_t> cd(cd_size);
    in.seekg(cd_offset);
    if (cd_size && !in.read(reinterpret_cast<char*>(cd.data()), static_cast<std::streamsize>(cd.size())))
        fail(path, "cannot read the central directory");

    auto archive = std::shared_ptr<ZipArchive>(new ZipArchive());
    archive->path_ = path;
    std::map<std::string, std::string> seen;   // lower-case name -> name
    std::size_t at = 0;
    for (unsigned i = 0; i < entries; ++i) {
        if (at + 46 > cd.size() || le32(cd, at) != central_signature) fail(path, "damaged central directory");
        const auto flags = le16(cd, at + 8);
        ZipMember m;
        m.method = le16(cd, at + 10);
        m.crc = le32(cd, at + 16);
        m.compressed = le32(cd, at + 20);
        m.size = le32(cd, at + 24);
        const auto name_length = le16(cd, at + 28), extra_length = le16(cd, at + 30), comment_length = le16(cd, at + 32);
        m.local_offset = le32(cd, at + 42);
        if (at + 46 + std::size_t(name_length) + extra_length + comment_length > cd.size()) fail(path, "damaged central directory");
        std::string name(reinterpret_cast<const char*>(cd.data() + at + 46), name_length);
        at += 46 + std::size_t(name_length) + extra_length + comment_length;
        std::string problem;
        name = checked_name(std::move(name), problem);
        if (!problem.empty()) fail(path, "member \"" + name + "\": " + problem);
        if (name.ends_with('/')) continue;   // a directory entry
        if (flags & 1u) fail(path, "member " + name + " is encrypted (not read)");
        if (m.method != 0 && m.method != 8)
            fail(path, "member " + name + " uses compression method " + std::to_string(m.method) + " (only stored and deflate are read)");
        if (m.size == 0xFFFFFFFFu || m.compressed == 0xFFFFFFFFu || m.local_offset == 0xFFFFFFFFu)
            fail(path, "zip64 archives are not read");
        if (m.size > member_size_limit) fail(path, "member " + name + " is larger than 2 GiB");
        if (m.method == 0 && m.compressed != m.size) fail(path, "member " + name + ": stored sizes differ");
        if (std::uint64_t(m.local_offset) + 30 + m.compressed > file_size) fail(path, "member " + name + " past the end of the file");
        const auto [it, inserted] = seen.emplace(lower_ascii(name), name);
        if (!inserted) fail(path, "members " + it->second + " and " + name + " differ only in case (or repeat)");
        m.name = std::move(name);
        archive->members_.push_back(std::move(m));
    }
    std::sort(archive->members_.begin(), archive->members_.end(),
              [](const ZipMember& a, const ZipMember& b) { return a.name < b.name; });
    return archive;
}

const ZipMember* ZipArchive::find(std::string_view name) const {
    std::string wanted(name);
    std::replace(wanted.begin(), wanted.end(), '\\', '/');
    const auto it = std::lower_bound(members_.begin(), members_.end(), wanted,
                                     [](const ZipMember& m, const std::string& n) { return m.name < n; });
    if (it != members_.end() && it->name == wanted) return &*it;
    const auto folded = lower_ascii(wanted);
    for (const auto& m : members_)
        if (m.name.size() == folded.size() && lower_ascii(m.name) == folded) return &m;
    return nullptr;
}

std::vector<std::uint8_t> ZipArchive::read_raw(const ZipMember& member, std::size_t count) const {
    std::ifstream in(path_, std::ios::binary);
    if (!in) fail(path_, "cannot open");
    std::uint8_t header[30];
    in.seekg(member.local_offset);
    if (!in.read(reinterpret_cast<char*>(header), sizeof header) || le32(header, 0) != local_signature)
        fail(path_, "member " + member.name + ": damaged local header");
    in.seekg(std::streamoff(member.local_offset) + 30 + le16(header, 26) + le16(header, 28));
    std::vector<std::uint8_t> data(std::min<std::size_t>(count, member.compressed));
    if (!data.empty() && !in.read(reinterpret_cast<char*>(data.data()), static_cast<std::streamsize>(data.size())))
        fail(path_, "member " + member.name + ": truncated data");
    return data;
}

std::vector<std::uint8_t> ZipArchive::read(const ZipMember& member) const {
    auto data = read_raw(member, member.compressed);
    if (member.method == 8) {
        try {
            data = inflate_raw(data, member.size);
        } catch (const std::exception& e) {
            fail(path_, "member " + member.name + ": " + e.what());
        }
        if (data.size() != member.size) fail(path_, "member " + member.name + ": inflated size differs");
    }
    if (crc32(data) != member.crc) fail(path_, "member " + member.name + ": CRC-32 mismatch (damaged archive)");
    return data;
}

std::vector<std::uint8_t> ZipArchive::read_prefix(const ZipMember& member, std::size_t count) const {
    if (member.method == 0) return read_raw(member, count);
    auto data = read(member);
    if (data.size() > count) data.resize(count);
    return data;
}

std::vector<std::uint8_t> deflate_raw(std::span<const std::uint8_t> data) {
    constexpr std::size_t window = 32768, hash_size = 1 << 15, max_chain = 64, max_length = 258;
    BitWriter w;
    w.put(1, 1);   // BFINAL
    w.put(1, 2);   // BTYPE 01: fixed Huffman
    std::vector<std::int64_t> head(hash_size, -1), prev(window, -1);
    const auto n = data.size();
    const auto hash = [&](std::size_t i) {
        return ((std::uint32_t(data[i]) << 10) ^ (std::uint32_t(data[i + 1]) << 5) ^ data[i + 2]) & (hash_size - 1);
    };
    const auto insert = [&](std::size_t i) {
        const auto h = hash(i);
        prev[i & (window - 1)] = head[h];
        head[h] = static_cast<std::int64_t>(i);
    };
    for (std::size_t i = 0; i < n;) {
        std::size_t best_length = 0, best_distance = 0;
        if (i + 3 <= n) {
            auto candidate = head[hash(i)];
            const auto limit = std::min(max_length, n - i);
            for (std::size_t chain = 0; candidate >= 0 && chain < max_chain; ++chain) {
                const auto c = static_cast<std::size_t>(candidate);
                if (i - c > window) break;
                std::size_t length = 0;
                while (length < limit && data[c + length] == data[i + length]) ++length;
                if (length > best_length) {
                    best_length = length;
                    best_distance = i - c;
                    if (length == limit) break;
                }
                const auto next = prev[c & (window - 1)];
                if (next >= candidate) break;   // a slot reused by a newer position
                candidate = next;
            }
            insert(i);
        }
        if (best_length >= 3) {
            match(w, static_cast<unsigned>(best_length), static_cast<unsigned>(best_distance));
            for (std::size_t k = 1; k < best_length; ++k)
                if (i + k + 3 <= n) insert(i + k);
            i += best_length;
        } else {
            literal(w, data[i]);
            ++i;
        }
    }
    literal(w, 256);
    w.flush();
    return std::move(w.out);
}

namespace {

// A member ready to write: its data as stored.
struct Prepared {
    std::string name;
    std::uint16_t method{};
    std::uint32_t crc{}, size{};
    std::vector<std::uint8_t> data;
};

// Checkpoint 132md15: a movie (.mp4 / .m4v / .mov / .m2v / .pss) is always stored, so the player reads it in place
// (a deflated member would be read whole into memory first; movie data barely deflates anyway).
bool movie_member(std::string_view name) {
    const auto dot = name.rfind('.');
    if (dot == std::string_view::npos) return false;
    const auto ext = lower_ascii(std::string(name.substr(dot)));
    return ext == ".mp4" || ext == ".m4v" || ext == ".mov" || ext == ".m2v" || ext == ".pss";
}

Prepared prepare(std::string name, const std::vector<std::uint8_t>& bytes) {
    const bool store = movie_member(name);
    Prepared p{std::move(name), 0, crc32(bytes), static_cast<std::uint32_t>(bytes.size()), store ? bytes : deflate_raw(bytes)};
    if (!store && p.data.size() < bytes.size()) p.method = 8;
    else p.data = bytes;
    return p;
}

void check_names(const std::vector<std::string*>& names) {
    std::map<std::string, std::string> seen;
    for (auto* name : names) {
        std::string problem;
        *name = checked_name(std::move(*name), problem);
        if (problem.empty() && name->ends_with('/')) problem = "a folder name";
        if (!problem.empty()) throw std::runtime_error("Cannot pack \"" + *name + "\": " + problem);
        const auto [it, inserted] = seen.emplace(lower_ascii(*name), *name);
        if (!inserted) throw std::runtime_error("Cannot pack " + *name + " beside " + it->second + ": names differ only in case");
    }
}

std::vector<std::uint8_t> assemble(std::vector<Prepared> members) {
    std::sort(members.begin(), members.end(), [](const Prepared& a, const Prepared& b) { return a.name < b.name; });
    std::vector<std::uint8_t> out, central;
    for (const auto& m : members) {
        const std::uint16_t flags =
            std::any_of(m.name.begin(), m.name.end(), [](char c) { return static_cast<unsigned char>(c) >= 0x80; }) ? 0x0800 : 0;
        if (out.size() + 30 + m.name.size() + m.data.size() >= 0xFFFFFFFFull)
            throw std::runtime_error("Cannot pack: the archive would reach 4 GiB (zip64 is not written)");
        const auto offset = static_cast<std::uint32_t>(out.size());
        const auto header = [&](std::vector<std::uint8_t>& to, bool central_entry) {
            put32(to, central_entry ? central_signature : local_signature);
            if (central_entry) put16(to, 20);   // made by: MS-DOS, 2.0
            put16(to, 20);                      // needed: 2.0
            put16(to, flags);
            put16(to, m.method);
            put16(to, 0);
            put16(to, dos_date_1980);
            put32(to, m.crc);
            put32(to, static_cast<std::uint32_t>(m.data.size()));
            put32(to, m.size);
            put16(to, static_cast<std::uint32_t>(m.name.size()));
            put16(to, 0);
            if (central_entry) {
                put16(to, 0);
                put16(to, 0);
                put16(to, 0);
                put32(to, 0);
                put32(to, offset);
            }
            to.insert(to.end(), m.name.begin(), m.name.end());
        };
        header(out, false);
        out.insert(out.end(), m.data.begin(), m.data.end());
        header(central, true);
    }
    if (members.size() >= 0xFFFF || out.size() + central.size() >= 0xFFFFFFFFull)
        throw std::runtime_error("Cannot pack: too many members or too large for a classic zip");
    const auto cd_offset = static_cast<std::uint32_t>(out.size());
    out.insert(out.end(), central.begin(), central.end());
    put32(out, end_signature);
    put16(out, 0);
    put16(out, 0);
    put16(out, static_cast<std::uint32_t>(members.size()));
    put16(out, static_cast<std::uint32_t>(members.size()));
    put32(out, static_cast<std::uint32_t>(central.size()));
    put32(out, cd_offset);
    put16(out, 0);
    return out;
}

void write_file_replacing(const fs::path& path, const std::vector<std::uint8_t>& bytes) {
    auto temporary = path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) throw std::runtime_error("Cannot write " + temporary.string());
        out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        if (!out) throw std::runtime_error("Cannot write " + temporary.string());
    }
    std::error_code error;
    fs::rename(temporary, path, error);
    if (error) {
        fs::remove(temporary, error);
        throw std::runtime_error("Cannot replace " + path.string() + ": " + error.message());
    }
}

}  // namespace

std::vector<std::uint8_t> write_zip(std::vector<ZipInput> inputs) {
    std::vector<std::string*> names;
    for (auto& input : inputs) {
        names.push_back(&input.name);
        if (input.bytes.size() > member_size_limit) throw std::runtime_error("Cannot pack " + input.name + ": larger than 2 GiB");
    }
    check_names(names);
    std::vector<Prepared> members;
    members.reserve(inputs.size());
    for (auto& input : inputs) members.push_back(prepare(std::move(input.name), input.bytes));
    return assemble(std::move(members));
}

void write_zip_file(const fs::path& path, std::vector<ZipInput> inputs) { write_file_replacing(path, write_zip(std::move(inputs))); }

void replace_zip_member(const fs::path& path, std::string_view name, std::vector<std::uint8_t> bytes) {
    const auto archive = ZipArchive::open(path);
    const auto* replaced = archive->find(name);
    std::vector<Prepared> members;
    for (const auto& m : archive->members()) {
        if (&m == replaced) continue;
        members.push_back({m.name, m.method, m.crc, m.size, archive->read_raw(m, m.compressed)});
    }
    members.push_back(prepare(replaced ? replaced->name : std::string(name), bytes));
    std::vector<std::string*> names;
    for (auto& m : members) names.push_back(&m.name);
    check_names(names);
    write_file_replacing(path, assemble(std::move(members)));
    register_archive(path);
}

bool is_zip_name(const fs::path& path) { return lower_ascii(path.extension().string()) == ".zip"; }

std::shared_ptr<const ZipArchive> register_archive(const fs::path& zip) {
    const auto absolute = fs::absolute(zip).lexically_normal();
    auto archive = ZipArchive::open(absolute);
    const auto key = path_key(absolute);
    auto& r = registry();
    std::lock_guard lock(r.mutex);
    const auto it = std::find_if(r.archives.begin(), r.archives.end(), [&](const Registered& e) { return e.key == key; });
    if (it != r.archives.end()) it->archive = archive;
    else r.archives.push_back({key, archive});
    any_registered.store(true, std::memory_order_relaxed);
    return archive;
}

std::shared_ptr<const ZipArchive> archive_holding(const fs::path& path, std::string& member) {
    return locate(path, member, false);
}

std::optional<std::vector<std::uint8_t>> read_archived(const fs::path& path) {
    std::string name;
    const auto archive = locate(path, name, false);
    if (!archive) return std::nullopt;
    const auto* member = archive->find(name);
    if (!member) throw std::runtime_error("Cannot open " + path.string() + " (no such member in " + archive->path().filename().string() + ")");
    return archive->read(*member);
}

std::vector<fs::path> mod_files_below(const fs::path& folder) {
    std::vector<fs::path> result;
    std::string prefix;
    if (const auto archive = locate(folder, prefix, true)) {
        if (!prefix.empty()) prefix += '/';
        const auto folded = lower_ascii(prefix);
        for (const auto& m : archive->members())
            if (m.name.size() > prefix.size() && lower_ascii(m.name.substr(0, prefix.size())) == folded)
                result.push_back((folder / from_utf8(m.name.substr(prefix.size()))).make_preferred());
        std::sort(result.begin(), result.end());
        return result;
    }
    std::error_code error;
    if (!fs::is_directory(folder, error)) return result;
    for (fs::recursive_directory_iterator it(folder, error), end; !error && it != end; it.increment(error))
        if (it->is_regular_file(error)) result.push_back(it->path());
    std::sort(result.begin(), result.end());
    return result;
}

bool mod_file_exists(const fs::path& path) {
    std::string name;
    if (const auto archive = locate(path, name, false)) return archive->find(name) != nullptr;
    std::error_code error;
    return fs::is_regular_file(path, error);
}

}  // namespace omega::mods
