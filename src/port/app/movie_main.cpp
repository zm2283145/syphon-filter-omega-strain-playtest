// omega_movie: decode a ZMOVIES.HOG movie (or a standalone .PSS file) with the
// native PSS demuxer and MPEG-2 decoder (research/FRONTEND.md, Checkpoint 114).
//
//   omega_movie [--iso ISO] --movie NAME | --pss FILE
//               [--frames N] [--start-seq K] [--out DIR] [--every K] [--ppm]
//               [--yuv FILE] [--wav FILE] [--crc] [--subs [--lang L]] [--list] [--dump-vlc]
//
// Frames go to DIR/NAME_<display index>.png (or .ppm) every K frames; --yuv
// writes raw I420 for all decoded frames; --wav writes the whole PCM track;
// --crc prints a CRC-32 of each frame's Y, U, V planes; --subs prints the
// subtitle cues of the movie's .SUB feed (language byte L, default 0).
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "disc.hpp"
#include "movie_pss.hpp"
#include "movie_subtitles.hpp"

namespace {

using namespace omega;
using namespace omega::movie;

void put32be(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int i = 3; i >= 0; --i) out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
}

void png_chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    put32be(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32be(out, crc32(std::span(out).subspan(start)));
}

// Uncompressed (stored-deflate) PNG: no zlib dependency.
std::vector<std::uint8_t> encode_png(int w, int h, const std::vector<std::uint8_t>& rgb) {
    std::vector<std::uint8_t> raw;
    raw.reserve(std::size_t(h) * (w * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgb.begin() + std::ptrdiff_t(y) * w * 3, rgb.begin() + std::ptrdiff_t(y + 1) * w * 3);
    }
    std::vector<std::uint8_t> z{0x78, 0x01};
    for (std::size_t p = 0; p < raw.size() || raw.empty(); p += 65535) {
        const std::size_t n = std::min<std::size_t>(65535, raw.size() - p);
        z.push_back(p + n >= raw.size() ? 1 : 0);
        z.push_back(static_cast<std::uint8_t>(n));
        z.push_back(static_cast<std::uint8_t>(n >> 8));
        z.push_back(static_cast<std::uint8_t>(~n));
        z.push_back(static_cast<std::uint8_t>(~n >> 8));
        z.insert(z.end(), raw.begin() + std::ptrdiff_t(p), raw.begin() + std::ptrdiff_t(p + n));
        if (raw.empty()) break;
    }
    std::uint32_t a = 1, b = 0;
    for (const auto c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    put32be(z, (b << 16) | a);
    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::vector<std::uint8_t> ihdr;
    put32be(ihdr, static_cast<std::uint32_t>(w));
    put32be(ihdr, static_cast<std::uint32_t>(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});
    png_chunk(png, "IHDR", ihdr);
    png_chunk(png, "IDAT", z);
    png_chunk(png, "IEND", {});
    return png;
}

void write_file(const std::filesystem::path& path, std::span<const std::uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

int usage() {
    std::cerr << "usage: omega_movie [--iso ISO] --movie NAME | --pss FILE [--frames N] [--start-seq K]\n"
                 "                   [--out DIR] [--every K] [--ppm] [--yuv FILE] [--wav FILE] [--crc]\n"
                 "                   [--subs [--lang L]] [--list]\n";
    return 2;
}

}  // namespace

int main(int argc, char** argv) {
    try {
        std::optional<std::string> iso, movie_name, pss_file, out_dir, yuv_path, wav_path;
        long long max_frames = -1;
        int start_seq = 0, every = 1, language = 0;
        bool ppm = false, print_crc = false, subs = false, list = false, dump_vlc = false;
        for (int i = 1; i < argc; ++i) {
            const std::string a = argv[i];
            const auto value = [&]() -> std::string {
                if (i + 1 >= argc) throw std::runtime_error("missing value for " + a);
                return argv[++i];
            };
            if (a == "--iso") iso = value();
            else if (a == "--movie") movie_name = value();
            else if (a == "--pss") pss_file = value();
            else if (a == "--frames") max_frames = std::stoll(value());
            else if (a == "--start-seq") start_seq = std::stoi(value());
            else if (a == "--out") out_dir = value();
            else if (a == "--every") every = std::max(1, std::stoi(value()));
            else if (a == "--ppm") ppm = true;
            else if (a == "--yuv") yuv_path = value();
            else if (a == "--wav") wav_path = value();
            else if (a == "--crc") print_crc = true;
            else if (a == "--subs") subs = true;
            else if (a == "--lang") language = std::stoi(value());
            else if (a == "--list") list = true;
            else if (a == "--dump-vlc") dump_vlc = true;
            else return usage();
        }
        if (dump_vlc) {
            // Annex B tables as transcribed, for cross-checking: "table code length value [level]".
            const auto vlc = [](const char* t, std::span<const mpeg2::VlcEntry> entries) {
                for (const auto& e : entries) std::printf("%s %u %u %d\n", t, e.code, e.length, e.value);
            };
            const auto dct = [](const char* t, std::span<const mpeg2::DctEntry> entries, mpeg2::DctSpecialCodes s) {
                for (const auto& e : entries) std::printf("%s %u %u %u %u\n", t, e.code, e.length, e.run, e.level);
                std::printf("%s_eob %u %u\n%s_esc %u %u\n", t, s.eob_code, s.eob_length, t, s.escape_code,
                            s.escape_length);
            };
            vlc("b1", mpeg2::table_b1_address_increment());
            vlc("b9", mpeg2::table_b9_coded_block_pattern());
            vlc("b10", mpeg2::table_b10_motion_code());
            vlc("b12", mpeg2::table_b12_dc_size_luma());
            vlc("b13", mpeg2::table_b13_dc_size_chroma());
            dct("b14", mpeg2::table_b14_dct_zero(), mpeg2::table_b14_special());
            dct("b15", mpeg2::table_b15_dct_one(), mpeg2::table_b15_special());
            return 0;
        }
        if (!pss_file && !iso) iso = locate_game_iso();
        std::ifstream stream;
        std::optional<DiscImage> disc;
        if (iso) {
            stream.open(*iso, std::ios::binary);
            if (!stream) throw std::runtime_error("cannot open ISO " + *iso);
            disc.emplace(stream, std::filesystem::file_size(*iso));
        }
        if (list) {
            if (!disc) throw std::runtime_error("--list needs the ISO");
            for (const auto& n : disc->archive_member_names(movies_hog())) {
                const auto e = disc->archive_member_extent(movies_hog(), n);
                std::cout << n << " " << e.size << "\n";
            }
            return 0;
        }
        if (!movie_name && !pss_file) return usage();
        const std::string name = movie_name ? *movie_name : std::filesystem::path(*pss_file).stem().string();

        if (subs) {
            if (!disc) throw std::runtime_error("--subs needs the ISO");
            auto bytes = disc->archive_member(movies_hog(), name + ".SUB");
            const auto file = parse_subtitles(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
            const int feed_id = subtitle_feed_for_language(static_cast<std::uint8_t>(language));
            const SubtitleFeed* feed = file.feed(feed_id);
            std::cout << name << ".SUB: " << file.feeds.size() << " feeds; language " << language << " -> feed "
                      << feed_id << (feed ? " " + feed->language : std::string(" (none)")) << "\n";
            if (feed)
                for (const auto& c : feed->cues) {
                    std::printf("  %8.3f - %8.3f  flags %u  %s %s  %zu line(s)\n", c.start, c.end, c.flags,
                                c.method.c_str(), c.position.c_str(), c.lines.size());
                }
            return 0;
        }

        std::ifstream pss_stream;
        PssMovie::Source source;
        std::uint64_t container_size = 0;
        if (pss_file) {
            pss_stream.open(*pss_file, std::ios::binary);
            if (!pss_stream) throw std::runtime_error("cannot open " + *pss_file);
            container_size = std::filesystem::file_size(*pss_file);
            source = stream_source(pss_stream, 0, container_size);
        } else {
            if (!disc) throw std::runtime_error("no ISO found (pass --iso or set OMEGA_ISO)");
            const auto extent = disc->archive_member_extent(movies_hog(), name + ".PSS");
            container_size = extent.size;
            source = stream_source(stream, extent.offset, extent.size);
        }
        std::cout << name << ".PSS: " << container_size << " bytes\n";

        if (out_dir) std::filesystem::create_directories(*out_dir);
        std::ofstream yuv;
        if (yuv_path) {
            yuv.open(*yuv_path, std::ios::binary);
            if (!yuv) throw std::runtime_error("cannot write " + *yuv_path);
        }

        PssMovie movie(std::move(source), 256 * 1024);
        movie.decoder().start_at_sequence_header(start_seq);
        mpeg2::Frame frame;
        long long count = 0;
        double decode_seconds = 0;
        const auto begin = std::chrono::steady_clock::now();
        while (max_frames < 0 || count < max_frames) {
            const auto t0 = std::chrono::steady_clock::now();
            const bool got = movie.next_frame(frame);
            decode_seconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
            if (!got) break;
            if (print_crc)
                std::printf("frame %lld %c tref %d crc %08x field_mc %u\n", count, frame.type,
                            frame.temporal_reference, frame_crc32(frame), frame.stats.field_mc);
            if (yuv) {
                yuv.write(reinterpret_cast<const char*>(frame.y.data()), std::streamsize(frame.y.size()));
                yuv.write(reinterpret_cast<const char*>(frame.u.data()), std::streamsize(frame.u.size()));
                yuv.write(reinterpret_cast<const char*>(frame.v.data()), std::streamsize(frame.v.size()));
            }
            if (out_dir && count % every == 0) {
                const auto rgb = frame_to_rgb(frame);
                char file[64];
                std::snprintf(file, sizeof file, "_%05lld.%s", count, ppm ? "ppm" : "png");
                const auto path = std::filesystem::path(*out_dir) / (name + file);
                if (ppm) {
                    const std::string header =
                        "P6\n" + std::to_string(frame.width) + " " + std::to_string(frame.height) + "\n255\n";
                    std::vector<std::uint8_t> bytes(header.begin(), header.end());
                    bytes.insert(bytes.end(), rgb.begin(), rgb.end());
                    write_file(path, bytes);
                } else {
                    write_file(path, encode_png(frame.width, frame.height, rgb));
                }
            }
            ++count;
        }
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - begin).count();
        const auto& s = movie.decoder().stats();
        std::printf("decoded %lld frames (%dx%d) in %.3f s decode (%.1f fps), %.3f s wall\n", count,
                    movie.decoder().width(), movie.decoder().height(), decode_seconds,
                    decode_seconds > 0 ? count / decode_seconds : 0.0, wall);
        std::printf("pictures I %llu P %llu B %llu (leading B dropped %llu); field MC MBs %u, field DCT MBs %u\n",
                    static_cast<unsigned long long>(s.pictures_i), static_cast<unsigned long long>(s.pictures_p),
                    static_cast<unsigned long long>(s.pictures_b),
                    static_cast<unsigned long long>(s.skipped_leading_b), s.totals.field_mc, s.totals.field_dct);

        if (wav_path) {
            movie.drain_audio();
            auto& audio = movie.audio();
            const auto& h = audio.header();
            const auto& samples = audio.samples();
            const auto data_bytes = static_cast<std::uint32_t>(samples.size() * 2);
            auto bytes = wav_header(h.rate, h.channels, data_bytes);
            const auto* p = reinterpret_cast<const std::uint8_t*>(samples.data());
            bytes.insert(bytes.end(), p, p + data_bytes);
            write_file(*wav_path, bytes);
            std::printf("audio: %d Hz, %d ch, interleave %d, SSbd %u bytes, %zu sample frames (%.3f s) -> %s\n",
                        h.rate, h.channels, h.interleave, h.body_size, samples.size() / h.channels,
                        double(samples.size() / h.channels) / h.rate, wav_path->c_str());
            const auto& d = movie.demuxer().stats();
            std::printf("demux: %llu packs, %llu video PES, %llu audio PES, %llu padding PES, end code %d\n",
                        static_cast<unsigned long long>(d.packs), static_cast<unsigned long long>(d.video_pes),
                        static_cast<unsigned long long>(d.audio_pes), static_cast<unsigned long long>(d.padding_pes),
                        d.end_code ? 1 : 0);
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "omega_movie: " << e.what() << "\n";
        return 1;
    }
}
