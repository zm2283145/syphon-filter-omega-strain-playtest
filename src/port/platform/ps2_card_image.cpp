#include "ps2_card_image.hpp"

#include "agent_card.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <stdexcept>

namespace omega {
namespace {

constexpr char card_magic[] = "Sony PS2 Memory Card Format ";
constexpr std::size_t entry_size = 512;

std::uint32_t u32(std::span<const std::uint8_t> b, std::size_t at) {
    if (at + 4 > b.size()) throw std::runtime_error("PS2 card: read past a block");
    return static_cast<std::uint32_t>(b[at]) | static_cast<std::uint32_t>(b[at + 1]) << 8 |
           static_cast<std::uint32_t>(b[at + 2]) << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24;
}
std::uint16_t u16(std::span<const std::uint8_t> b, std::size_t at) {
    if (at + 2 > b.size()) throw std::runtime_error("PS2 card: read past a block");
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}

// 132win13: the writer's little-endian stores.
void put32(std::span<std::uint8_t> b, std::size_t at, std::uint32_t v) {
    if (at + 4 > b.size()) throw std::runtime_error("PS2 card: write past a block");
    for (int i = 0; i < 4; ++i) b[at + i] = static_cast<std::uint8_t>(v >> (8 * i));
}
void put16(std::span<std::uint8_t> b, std::size_t at, std::uint16_t v) {
    if (at + 2 > b.size()) throw std::runtime_error("PS2 card: write past a block");
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}

constexpr std::uint16_t mode_directory = 0x8427;   // exists, 0x400, directory, read / write / execute
constexpr std::uint16_t mode_root_parent = 0xa426; // the root's ".."
constexpr std::uint16_t mode_file = 0x8497;        // exists, 0x400, 0x80, file, read / write / execute
constexpr std::uint32_t fat_allocated = 0x80000000u;
constexpr std::uint32_t chain_end = 0xffffffffu;

void put_time(std::span<std::uint8_t> e, std::size_t at, const Ps2CardTime& t) {
    e[at] = 0;
    e[at + 1] = t.second;
    e[at + 2] = t.minute;
    e[at + 3] = t.hour;
    e[at + 4] = t.day;
    e[at + 5] = t.month;
    put16(e, at + 6, t.year);
}
Ps2CardTime get_time(std::span<const std::uint8_t> e, std::size_t at) {
    Ps2CardTime t;
    t.second = e[at + 1];
    t.minute = e[at + 2];
    t.hour = e[at + 3];
    t.day = e[at + 4];
    t.month = e[at + 5];
    t.year = u16(e, at + 6);
    return t;
}

std::array<std::uint8_t, entry_size> make_entry(std::uint16_t mode, std::uint32_t length, const Ps2CardTime& created,
                                                std::uint32_t cluster, std::uint32_t dir_entry,
                                                const Ps2CardTime& modified, const std::string& name) {
    if (name.size() >= 32) throw std::runtime_error("PS2 card: a name of 32 bytes or more: " + name);
    std::array<std::uint8_t, entry_size> e{};
    put16(e, 0, mode);
    put32(e, 4, length);
    put_time(e, 8, created);
    put32(e, 0x10, cluster);
    put32(e, 0x14, dir_entry);
    put_time(e, 0x18, modified);
    std::memcpy(e.data() + 0x40, name.data(), name.size());
    return e;
}

}  // namespace

std::array<std::uint8_t, 3> ps2_card_ecc(std::span<const std::uint8_t, 128> chunk) {
    // The column parity masks (bit 3 is never set: its mask is 0) and each byte's parity.
    static const auto tables = [] {
        std::array<std::uint8_t, 256> parity{}, column{};
        constexpr std::uint8_t masks[7] = {0x55, 0x33, 0x0f, 0x00, 0xaa, 0xcc, 0xf0};
        for (int b = 0; b < 256; ++b) {
            int x = b, p = 0;
            for (; x; x >>= 1) p ^= x & 1;
            parity[b] = static_cast<std::uint8_t>(p);
        }
        for (int b = 0; b < 256; ++b) {
            int c = 0;
            for (int i = 0; i < 7; ++i) c |= parity[b & masks[i]] << i;
            column[b] = static_cast<std::uint8_t>(c);
        }
        return std::pair{parity, column};
    }();
    std::uint8_t c = 0x77, l0 = 0x7f, l1 = 0x7f;
    for (std::size_t i = 0; i < 128; ++i) {
        const std::uint8_t b = chunk[i];
        c ^= tables.second[b];
        if (tables.first[b]) {
            l0 ^= static_cast<std::uint8_t>(~i);
            l1 ^= static_cast<std::uint8_t>(i);
        }
    }
    return {c, static_cast<std::uint8_t>(l0 & 0x7f), static_cast<std::uint8_t>(l1 & 0x7f)};
}

Ps2CardTime ps2_card_time_now() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now()) + 9 * 3600;
    std::tm t{};
#ifdef _WIN32
    gmtime_s(&t, &now);
#else
    gmtime_r(&now, &t);
#endif
    Ps2CardTime r;
    r.second = static_cast<std::uint8_t>(t.tm_sec);
    r.minute = static_cast<std::uint8_t>(t.tm_min);
    r.hour = static_cast<std::uint8_t>(t.tm_hour);
    r.day = static_cast<std::uint8_t>(t.tm_mday);
    r.month = static_cast<std::uint8_t>(t.tm_mon + 1);
    r.year = static_cast<std::uint16_t>(t.tm_year + 1900);
    return r;
}

Ps2CardImage::Ps2CardImage(std::vector<std::uint8_t> image) : image_(std::move(image)) {
    if (image_.size() < 0x200 || std::memcmp(image_.data(), card_magic, sizeof card_magic - 1) != 0)
        throw std::runtime_error("PS2 card: no superblock (not a formatted card image)");
    page_len_ = u16(image_, 0x28);
    pages_per_cluster_ = u16(image_, 0x2a);
    clusters_ = u32(image_, 0x30);
    alloc_offset_ = u32(image_, 0x34);
    alloc_end_ = u32(image_, 0x38);
    root_cluster_ = u32(image_, 0x3c);
    if (page_len_ == 0 || pages_per_cluster_ == 0 || clusters_ == 0)
        throw std::runtime_error("PS2 card: invalid superblock geometry");
    const std::uint64_t pages = static_cast<std::uint64_t>(clusters_) * pages_per_cluster_;
    if (image_.size() == pages * (page_len_ + 16)) raw_page_ = page_len_ + 16;
    else if (image_.size() == pages * page_len_) raw_page_ = page_len_;
    else throw std::runtime_error("PS2 card: the image size matches no page layout");
    for (std::size_t i = 0; i < 32; ++i) ifc_.push_back(u32(image_, 0x50 + 4 * i));
}

Ps2CardImage Ps2CardImage::read(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("PS2 card: cannot open " + path.string());
    return Ps2CardImage(std::vector<std::uint8_t>(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()));
}

std::vector<std::uint8_t> Ps2CardImage::cluster(std::uint32_t absolute) const {
    if (absolute >= clusters_) throw std::runtime_error("PS2 card: cluster past the card");
    std::vector<std::uint8_t> out;
    out.reserve(cluster_size());
    for (std::uint32_t p = 0; p < pages_per_cluster_; ++p) {
        const std::uint64_t at = (static_cast<std::uint64_t>(absolute) * pages_per_cluster_ + p) * raw_page_;
        out.insert(out.end(), image_.begin() + static_cast<std::ptrdiff_t>(at),
                   image_.begin() + static_cast<std::ptrdiff_t>(at + page_len_));
    }
    return out;
}

std::uint32_t Ps2CardImage::fat(std::uint32_t relative) const {
    const std::uint32_t per = cluster_size() / 4;
    const std::uint32_t fat_index = relative / per;
    const std::uint32_t indirect = fat_index / per;
    if (indirect >= ifc_.size()) throw std::runtime_error("PS2 card: FAT index past the indirect list");
    const auto ind = cluster(ifc_[indirect]);
    const auto fat_cluster = cluster(u32(ind, 4 * (fat_index % per)));
    return u32(fat_cluster, 4 * (relative % per));
}

std::vector<std::uint8_t> Ps2CardImage::chain(std::uint32_t relative, std::size_t bytes) const {
    std::vector<std::uint8_t> out;
    std::uint32_t c = relative;
    for (std::size_t guard = 0; out.size() < bytes; ++guard) {
        if (guard > clusters_) throw std::runtime_error("PS2 card: FAT chain loop");
        const auto data = cluster(alloc_offset_ + c);
        out.insert(out.end(), data.begin(), data.end());
        if (out.size() >= bytes) break;
        const std::uint32_t next = fat(c);
        if (next == 0xffffffffu || !(next & 0x80000000u)) throw std::runtime_error("PS2 card: FAT chain ends early");
        c = next & 0x7fffffffu;
    }
    out.resize(bytes);
    return out;
}

std::vector<Ps2CardEntry> Ps2CardImage::entries(std::uint32_t relative, std::uint32_t count) const {
    const auto data = chain(relative, static_cast<std::size_t>(count) * entry_size);
    std::vector<Ps2CardEntry> out;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::span<const std::uint8_t> e(data.data() + i * entry_size, entry_size);
        Ps2CardEntry x;
        x.mode = u16(e, 0);
        x.length = u32(e, 4);
        x.cluster = u32(e, 0x10);
        const char* name = reinterpret_cast<const char*>(e.data() + 0x40);
        x.name.assign(name, strnlen(name, 32));
        out.push_back(std::move(x));
    }
    return out;
}

std::vector<Ps2CardEntry> Ps2CardImage::root() const {
    const auto dot = entries(root_cluster_, 1);
    Ps2CardEntry dir = dot.at(0);
    dir.cluster = root_cluster_;
    return list(dir);
}

std::vector<Ps2CardEntry> Ps2CardImage::list(const Ps2CardEntry& directory) const {
    // The directory's own length is its entry count; its "." entry repeats it.
    std::uint32_t count = directory.length;
    if (count == 0) count = entries(directory.cluster, 1).at(0).length;
    std::vector<Ps2CardEntry> out;
    for (auto& e : entries(directory.cluster, count))
        if (e.exists() && e.name != "." && e.name != "..") out.push_back(std::move(e));
    return out;
}

std::vector<std::uint8_t> Ps2CardImage::read(const Ps2CardEntry& file) const {
    if (!file.file()) throw std::runtime_error("PS2 card: " + file.name + " is not a file");
    if (file.length == 0) return {};
    return chain(file.cluster, file.length);
}

// ---- Checkpoint 132win13 (port-only): the writer ----

std::vector<std::uint32_t> Ps2CardImage::chain_clusters(std::uint32_t relative) const {
    std::vector<std::uint32_t> out;
    if (relative == chain_end) return out;
    for (std::uint32_t c = relative;;) {
        if (out.size() > clusters_) throw std::runtime_error("PS2 card: FAT chain loop");
        if (c >= alloc_end_) throw std::runtime_error("PS2 card: FAT chain past the allocatable clusters");
        out.push_back(c);
        const std::uint32_t next = fat(c);
        if (next == chain_end) break;
        if (!(next & fat_allocated)) throw std::runtime_error("PS2 card: FAT chain runs into a free cluster");
        c = next & ~fat_allocated;
    }
    return out;
}

void Ps2CardImage::write_cluster(std::uint32_t absolute, std::span<const std::uint8_t> data) {
    if (absolute >= clusters_) throw std::runtime_error("PS2 card: cluster past the card");
    if (data.size() > cluster_size()) throw std::runtime_error("PS2 card: a cluster's data is too long");
    for (std::uint32_t p = 0; p < pages_per_cluster_; ++p) {
        const std::uint64_t at = (static_cast<std::uint64_t>(absolute) * pages_per_cluster_ + p) * raw_page_;
        std::uint8_t* page = image_.data() + at;
        const std::size_t from = static_cast<std::size_t>(p) * page_len_;
        for (std::size_t i = 0; i < page_len_; ++i) page[i] = from + i < data.size() ? data[from + i] : 0;
        if (!has_ecc()) continue;
        std::uint8_t* spare = page + page_len_;
        std::fill(spare, spare + 16, std::uint8_t{0xff});
        for (std::uint32_t k = 0; k < page_len_ / 128 && 3 * k + 3 <= 12; ++k) {
            const auto ecc = ps2_card_ecc(std::span<const std::uint8_t, 128>(page + 128 * k, 128));
            std::copy(ecc.begin(), ecc.end(), spare + 3 * k);
        }
    }
}

void Ps2CardImage::set_fat(std::uint32_t relative, std::uint32_t value) {
    const std::uint32_t per = cluster_size() / 4;
    const std::uint32_t fat_index = relative / per;
    const std::uint32_t indirect = fat_index / per;
    if (indirect >= ifc_.size()) throw std::runtime_error("PS2 card: FAT index past the indirect list");
    const std::uint32_t at = u32(cluster(ifc_[indirect]), 4 * (fat_index % per));
    auto data = cluster(at);
    put32(data, 4 * (relative % per), value);
    write_cluster(at, data);
}

void Ps2CardImage::write_chain(const std::vector<std::uint32_t>& clusters, std::span<const std::uint8_t> data) {
    const std::size_t size = cluster_size();
    for (std::size_t k = 0; k < clusters.size(); ++k) {
        set_fat(clusters[k], k + 1 < clusters.size() ? fat_allocated | clusters[k + 1] : chain_end);
        const std::size_t from = std::min(data.size(), k * size);
        write_cluster(alloc_offset_ + clusters[k], data.subspan(from, std::min(size, data.size() - from)));
    }
}

std::uint32_t Ps2CardImage::free_clusters() const {
    std::uint32_t n = 0;
    for (std::uint32_t r = 0; r < alloc_end_; ++r)
        if (!(fat(r) & fat_allocated)) ++n;
    return n;
}

std::size_t Ps2CardImage::bad_ecc_pages() const {
    if (!has_ecc()) return 0;
    std::size_t bad = 0;
    for (std::uint64_t at = 0; at + raw_page_ <= image_.size(); at += raw_page_) {
        const std::uint8_t* page = image_.data() + at;
        if (std::all_of(page, page + raw_page_, [](std::uint8_t b) { return b == 0xff; })) continue;
        for (std::uint32_t k = 0; k < page_len_ / 128 && 3 * k + 3 <= 12; ++k) {
            const auto ecc = ps2_card_ecc(std::span<const std::uint8_t, 128>(page + 128 * k, 128));
            if (!std::equal(ecc.begin(), ecc.end(), page + page_len_ + 3 * k)) {
                ++bad;
                break;
            }
        }
    }
    return bad;
}

void Ps2CardImage::write(const std::filesystem::path& path) const {
    auto temp = path;
    temp += ".tmp~";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(image_.data()), static_cast<std::streamsize>(image_.size()));
        if (!out) throw std::runtime_error("PS2 card: cannot write " + temp.string());
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
        throw std::runtime_error("PS2 card: cannot replace " + path.string());
    }
}

Ps2CardImage Ps2CardImage::format(const Ps2CardTime& time) {
    // The geometry and the superblock bytes of a card the PS2 formatted (PCSX2's Mcd001.ps2): 8192 clusters of
    // two 512-byte pages, 16 pages per erase block, the indirect FAT cluster 8, the FAT clusters 9..40, data from
    // cluster 41 for 8135 clusters, the backup erase blocks 1023 and 1022 (clusters 8176..8191).
    constexpr std::uint32_t page = 512, raw = 528, per_cluster = 2, clusters = 8192, alloc_offset = 41,
                            alloc_end = 8135, ifc = 8, fat_first = 9, fat_count = 32;
    std::vector<std::uint8_t> image(static_cast<std::size_t>(clusters) * per_cluster * raw, 0xff);
    std::array<std::uint8_t, page> sb;
    sb.fill(0xff);
    std::fill(sb.begin(), sb.begin() + 0x150, std::uint8_t{0});
    const char magic[] = "Sony PS2 Memory Card Format 1.2.0.0";
    std::memcpy(sb.data(), magic, sizeof magic - 1);
    put16(sb, 0x28, page);
    put16(sb, 0x2a, per_cluster);
    put16(sb, 0x2c, 16);                 // pages per erase block
    put16(sb, 0x2e, 0xff00);
    put32(sb, 0x30, clusters);
    put32(sb, 0x34, alloc_offset);
    put32(sb, 0x38, alloc_end);
    put32(sb, 0x3c, 0);                  // the root directory's cluster
    put32(sb, 0x40, 1023);               // backup block 1
    put32(sb, 0x44, 1022);               // backup block 2
    put32(sb, 0x50, ifc);                // the indirect FAT cluster list (the rest 0)
    for (std::size_t i = 0; i < 32; ++i) put32(sb, 0xd0 + 4 * i, 0xffffffffu);  // no bad blocks
    sb[0x150] = 2;                       // card type
    sb[0x151] = 0x2b;                    // card flags
    constexpr std::uint8_t tail[] = {0xff, 0xff, 0x00, 0x04, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00,
                                     0xff, 0xff, 0xff, 0xff};
    std::memcpy(sb.data() + 0x152, tail, sizeof tail);
    std::fill(sb.begin() + 0x152 + sizeof tail, sb.begin() + 0x170, std::uint8_t{0});
    put32(sb, 0x170, 8001);              // max allocatable clusters
    std::memcpy(image.data(), sb.data(), page);
    Ps2CardImage card(std::move(image));
    card.write_cluster(0, std::span<const std::uint8_t>(sb.data(), page));  // page 0 with its ECC
    std::fill(card.image_.begin() + raw, card.image_.begin() + 2 * raw, std::uint8_t{0xff});  // page 1 stays erased
    // The indirect cluster names the FAT clusters; the FAT: every data cluster free, the root's one cluster
    // allocated (chain end), the entries past the allocatable count FFFFFFFF.
    std::vector<std::uint8_t> data(card.cluster_size(), 0xff);
    for (std::uint32_t i = 0; i < fat_count; ++i) put32(data, 4 * i, fat_first + i);
    card.write_cluster(ifc, data);
    const std::uint32_t per = card.cluster_size() / 4;
    for (std::uint32_t i = 0; i < fat_count; ++i) {
        for (std::uint32_t k = 0; k < per; ++k) {
            const std::uint32_t r = i * per + k;
            put32(data, 4 * k, r == 0 || r >= alloc_end ? chain_end : 0x7fffffffu);
        }
        card.write_cluster(fat_first + i, data);
    }
    std::vector<std::uint8_t> root(card.cluster_size(), 0);
    const auto dot = make_entry(mode_directory, 2, time, 0, 0, time, ".");
    const auto parent = make_entry(mode_root_parent, 0, time, 0, 0, time, "..");
    std::copy(dot.begin(), dot.end(), root.begin());
    std::copy(parent.begin(), parent.end(), root.begin() + entry_size);
    card.write_cluster(alloc_offset, root);
    return card;
}

std::optional<std::vector<Ps2CardFile>> Ps2CardImage::directory_files(const std::string& name) const {
    for (const auto& d : root()) {
        if (!d.directory() || d.name != name) continue;
        std::vector<Ps2CardFile> files;
        for (const auto& f : list(d))
            if (f.file()) files.push_back({f.name, read(f)});
        return files;
    }
    return std::nullopt;
}

void Ps2CardImage::replace_directory(const std::string& name, const std::vector<Ps2CardFile>& files,
                                     const Ps2CardTime& time) {
    if (name.empty() || name == "." || name == ".." || name.size() >= 32)
        throw std::runtime_error("PS2 card: invalid directory name " + name);
    const std::size_t size = cluster_size();
    const auto clusters_for = [size](std::size_t bytes) { return static_cast<std::uint32_t>((bytes + size - 1) / size); };
    // The root: its "." entry's length is the entry count.
    const auto root_chain = chain_clusters(root_cluster_);
    auto root_bytes = chain(root_cluster_, root_chain.size() * size);
    const std::uint32_t count = u32(root_bytes, 4);
    std::optional<std::uint32_t> index, free_index;
    Ps2CardTime created = time;
    std::vector<std::uint32_t> freed;
    for (std::uint32_t i = 2; i < count; ++i) {
        const std::span<const std::uint8_t> e(root_bytes.data() + i * entry_size, entry_size);
        const std::uint16_t mode = u16(e, 0);
        if (!(mode & 0x8000)) {
            if (!free_index) free_index = i;
            continue;
        }
        const char* n = reinterpret_cast<const char*>(e.data() + 0x40);
        if (std::string(n, strnlen(n, 32)) != name) continue;
        if (!(mode & 0x20)) throw std::runtime_error("PS2 card: " + name + " is a file");
        index = i;
        created = get_time(e, 8);
        Ps2CardEntry dir;
        dir.mode = mode;
        dir.length = u32(e, 4);
        dir.cluster = u32(e, 0x10);
        for (const auto& f : list(dir))
            if (f.file() && f.length) {
                const auto c = chain_clusters(f.cluster);
                freed.insert(freed.end(), c.begin(), c.end());
            }
        const auto c = chain_clusters(dir.cluster);
        freed.insert(freed.end(), c.begin(), c.end());
    }
    const std::uint32_t at = index ? *index : free_index ? *free_index : count;
    const std::uint32_t new_count = std::max(count, at + 1);
    const std::uint32_t root_extra = clusters_for(std::size_t{new_count} * entry_size) -
                                     std::min<std::uint32_t>(clusters_for(std::size_t{new_count} * entry_size),
                                                             static_cast<std::uint32_t>(root_chain.size()));
    // The space check before anything is changed.
    std::vector<std::uint32_t> table(alloc_end_);
    for (std::uint32_t r = 0; r < alloc_end_; ++r) table[r] = fat(r);
    for (const auto c : freed) table[c] &= ~fat_allocated;
    std::uint32_t needed = root_extra + clusters_for((files.size() + 2) * entry_size);
    for (const auto& f : files) {
        if (f.name.empty() || f.name == "." || f.name == ".." || f.name.size() >= 32)
            throw std::runtime_error("PS2 card: invalid file name " + f.name);
        needed += clusters_for(f.bytes.size());
    }
    const auto available = static_cast<std::uint32_t>(
        std::count_if(table.begin(), table.end(), [](std::uint32_t v) { return !(v & fat_allocated); }));
    if (needed > available)
        throw std::runtime_error("PS2 card: " + std::to_string(needed) + " clusters needed, " +
                                 std::to_string(available) + " free");
    // Free the old directory, then take clusters first-fit.
    for (const auto c : freed) set_fat(c, fat(c) & ~fat_allocated);
    std::uint32_t next_free = 0;
    const auto take = [&](std::uint32_t n) {
        std::vector<std::uint32_t> out;
        while (out.size() < n) {
            while (table[next_free] & fat_allocated) ++next_free;
            table[next_free] = chain_end;
            out.push_back(next_free++);
        }
        return out;
    };
    if (root_extra) {
        const auto more = take(root_extra);
        auto all = root_chain;
        all.insert(all.end(), more.begin(), more.end());
        root_bytes.resize(all.size() * size, 0);
        set_fat(root_chain.back(), fat_allocated | more.front());
        for (std::size_t k = 0; k < more.size(); ++k)
            set_fat(more[k], k + 1 < more.size() ? fat_allocated | more[k + 1] : chain_end);
    }
    const auto dir_clusters = take(clusters_for((files.size() + 2) * entry_size));
    std::vector<std::uint8_t> dir_bytes(dir_clusters.size() * size, 0);
    const auto put_entry = [](std::vector<std::uint8_t>& to, std::size_t i, const std::array<std::uint8_t, entry_size>& e) {
        std::copy(e.begin(), e.end(), to.begin() + static_cast<std::ptrdiff_t>(i * entry_size));
    };
    put_entry(dir_bytes, 0, make_entry(mode_directory, 0, created, 0, at, time, "."));
    put_entry(dir_bytes, 1, make_entry(mode_directory, 0, created, 0, 0, time, ".."));
    for (std::size_t i = 0; i < files.size(); ++i) {
        const auto& f = files[i];
        const auto c = take(clusters_for(f.bytes.size()));
        if (!c.empty()) write_chain(c, f.bytes);
        put_entry(dir_bytes, i + 2,
                  make_entry(mode_file, static_cast<std::uint32_t>(f.bytes.size()), time, c.empty() ? chain_end : c.front(),
                             0, time, f.name));
    }
    write_chain(dir_clusters, dir_bytes);
    put_entry(root_bytes, at,
              make_entry(mode_directory, static_cast<std::uint32_t>(files.size() + 2), created, dir_clusters.front(), 0,
                         time, name));
    put32(root_bytes, 4, new_count);
    const auto root_all = chain_clusters(root_cluster_);
    for (std::size_t k = 0; k < root_all.size(); ++k)
        write_cluster(alloc_offset_ + root_all[k],
                      std::span<const std::uint8_t>(root_bytes.data() + k * size, size));
}

Ps2CardImport import_ps2_card_directory(const Ps2CardImage& card, const std::string& directory,
                                        FolderAgentCard& folder, bool replace) {
    Ps2CardImport r;
    for (const auto& d : card.root()) {
        if (!d.directory() || d.name != directory) continue;
        r.found = true;
        if (!folder.create_save_directory())
            throw std::runtime_error("PS2 card: cannot create " + folder.directory().string());
        const auto present = folder.list();
        const auto on_card = card.list(d);
        if (replace)  // 132win13: the folder's files the card lacks go first (room for the card's)
            for (const auto& name : present)
                if (std::none_of(on_card.begin(), on_card.end(), [&](const Ps2CardEntry& f) { return f.file() && f.name == name; }))
                    if (!folder.remove(name)) throw std::runtime_error("PS2 card: cannot remove " + name);
        for (const auto& f : on_card) {
            if (!f.file()) continue;
            const auto bytes = card.read(f);
            if (std::find(present.begin(), present.end(), f.name) != present.end()) {
                bool same = !replace;
                if (replace && folder.size(f.name) == bytes.size()) {
                    std::vector<std::uint8_t> now(bytes.size());
                    same = folder.read(f.name, now) && now == bytes;
                }
                if (same) {
                    r.kept.push_back(f.name);
                    continue;
                }
            }
            if (!folder.write(f.name, bytes)) throw std::runtime_error("PS2 card: cannot write " + f.name);
            r.copied.push_back(f.name);
        }
    }
    return r;
}

Ps2CardExport export_ps2_card_directory(const FolderAgentCard& folder, const std::string& directory, Ps2CardImage& card,
                                        const Ps2CardTime& time, const std::vector<Ps2CardFile>& icons) {
    Ps2CardExport r;
    std::vector<Ps2CardFile> pc;
    for (const auto& name : folder.list()) {
        std::vector<std::uint8_t> bytes(folder.size(name));
        if (!folder.read(name, bytes)) throw std::runtime_error("PS2 card: cannot read the PC card's " + name);
        pc.push_back({name, std::move(bytes)});
    }
    const auto existing = card.directory_files(directory);
    const auto in_pc = [&](const std::string& name) {
        return std::find_if(pc.begin(), pc.end(), [&](const Ps2CardFile& f) { return f.name == name; });
    };
    std::vector<Ps2CardFile> files;
    if (existing)
        for (const auto& f : *existing) {
            if (const auto it = in_pc(f.name); it != pc.end()) files.push_back(*it);
            else if (f.name == "icon.sys" || f.name == "MEMCARD.ICN" || f.name == directory) {
                files.push_back(f);
                r.kept_icons.push_back(f.name);
            }
        }
    for (const auto& f : pc)
        if (std::none_of(files.begin(), files.end(), [&](const Ps2CardFile& x) { return x.name == f.name; })) files.push_back(f);
    std::vector<Ps2CardFile> added;
    for (const auto& f : icons)
        if (std::none_of(files.begin(), files.end(), [&](const Ps2CardFile& x) { return x.name == f.name; })) {
            added.push_back(f);
            r.added_icons.push_back(f.name);
        }
    files.insert(files.begin(), added.begin(), added.end());
    for (const auto& f : pc) r.written.push_back(f.name);
    if (existing && *existing == files) {
        r.unchanged = true;
        return r;
    }
    card.replace_directory(directory, files, time);
    return r;
}

}  // namespace omega
