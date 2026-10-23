#pragma once
// Native MPEG-2 (ISO/IEC 13818-2) video decoder for the ZMOVIES.HOG .PSS movies
// (research/FRONTEND.md, "Checkpoint 114 native movie decoder").
//
// Scope is exactly the feature set measured over all 54 movies: MP@ML 4:2:0
// frame pictures (I/P/B), frame and field DCT, frame and field motion
// compensation, intra_vlc_format 0/1, alternate_scan 0/1, q_scale_type 0/1,
// intra_dc_precision 0..3, custom quantiser matrices and the MPEG-2 escape.
// Field pictures, dual-prime, concealment vectors and slice extra information
// throw. The decode order of operations mirrors the Python prototype
// local/frontend_movies/mpeg2_ref.py; the inverse DCT is the deterministic
// integer separable transform below so the output is bit-reproducible.
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <span>
#include <vector>

namespace omega::mpeg2 {

// K[u][x] = round(2^16 * c(u)/2 * cos((2x+1)u*pi/16)), c(0) = 1/sqrt(2), else 1.
extern const std::array<std::array<std::int32_t, 8>, 8> idct_basis;

// Integer IDCT of one 8x8 block (natural order, F[v*8+u] with v vertical):
// R = F*K (rows), S = K^T*R (columns) in 64-bit, out = (S + 2^31) >> 32
// clamped to [-256, 255].
void idct_8x8(const std::int32_t* coefficients, std::int32_t* out);
// Port-only (132vt2a, research/VITA.md "Movies"): the same output as idct_8x8 for every input (exact integer
// regrouping, 32-bit row pass); the Vita decoder's IDCT (the reference's 64 x 64-bit multiplies are slow on its
// Cortex-A9). tests/movie_tests.cpp compares the two.
void idct_8x8_fast(const std::int32_t* coefficients, std::int32_t* out);
#if defined(__ARM_NEON)
// Port-only (132vt2a2, research/VITA.md "Movies"): idct_8x8's output on NEON for the decoder's coefficients
// (|F| <= 2048: the dequantiser's clamp), 16-bit in and out. The Vita decoder's IDCT; the app's boot check and
// tests compare it with idct_8x8.
void idct_8x8_neon(const std::int16_t* coefficients, std::int16_t* out);
#endif

// Scan tables (7.3.1): scan[k] = natural position of the k-th coefficient.
extern const std::array<std::uint8_t, 64> zigzag_scan;
extern const std::array<std::uint8_t, 64> alternate_scan;
extern const std::array<std::uint8_t, 64> default_intra_matrix;   // natural order (6.3.11)
extern const std::array<std::uint8_t, 32> non_linear_quantiser_scale;   // table 7-6

// Variable-length code tables of Annex B as (code, length, value) entries;
// exposed so tests can check the transcription (prefix-free, complete).
struct VlcEntry {
    std::uint32_t code;
    std::uint8_t length;
    std::int16_t value;   // table-specific meaning (see mpeg2_decoder.cpp)
};
struct DctEntry {
    std::uint32_t code;    // without the sign bit
    std::uint8_t length;
    std::uint8_t run;
    std::uint8_t level;
};
std::span<const VlcEntry> table_b1_address_increment();   // value 1..33, 34 = escape, 35 = stuffing
std::span<const VlcEntry> table_b9_coded_block_pattern();   // value = cbp 0..63
std::span<const VlcEntry> table_b10_motion_code();   // value = |motion_code| 0..16 (sign bit excluded)
std::span<const VlcEntry> table_b12_dc_size_luma();
std::span<const VlcEntry> table_b13_dc_size_chroma();
std::span<const DctEntry> table_b14_dct_zero();   // (0,1) as "11"; the first-coefficient "1" form is handled by the decoder
std::span<const DctEntry> table_b15_dct_one();
// End-of-block and escape codes of B.14/B.15 (6 bits "000001" for the escape).
struct DctSpecialCodes {
    std::uint32_t eob_code;
    std::uint8_t eob_length;
    std::uint32_t escape_code;
    std::uint8_t escape_length;
};
DctSpecialCodes table_b14_special();
DctSpecialCodes table_b15_special();

struct PictureStats {
    std::uint32_t macroblocks{};     // coded (non-skipped) macroblocks
    std::uint32_t skipped{};
    std::uint32_t intra{};
    std::uint32_t field_mc{};        // frame_motion_type 1 (field prediction)
    std::uint32_t frame_mc{};        // frame_motion_type 2
    std::uint32_t field_dct{};
    std::uint32_t frame_dct{};
    std::uint32_t quant{};
    std::uint32_t escapes{};
    std::uint32_t p_no_mc{};
    std::uint32_t mismatch_toggles{};
    std::uint32_t slices{};
};

struct DecoderStats {
    std::uint64_t pictures_i{}, pictures_p{}, pictures_b{};
    std::uint64_t skipped_leading_b{};
    std::uint64_t sequence_headers{};
    std::uint64_t gops{};
    std::uint64_t quant_matrix_extensions{};
    PictureStats totals{};
};

// One decoded picture in display order, cropped to the sequence size, I420.
struct Frame {
    int width{};
    int height{};
    std::vector<std::uint8_t> y, u, v;
    char type{};                      // 'I', 'P' or 'B'
    int temporal_reference{};
    std::uint64_t decode_index{};     // decoded picture count (dropped leading Bs excluded)
    PictureStats stats{};
};

class Decoder {
public:
    Decoder();
    // Units before the n-th (0-based) sequence header are ignored (random access
    // at a sequence header; the following GOP decides whether leading Bs drop).
    void start_at_sequence_header(int index) {
        start_sequence_ = index;
        active_ = false;
    }
    // Feed elementary-stream bytes; complete start-code units are decoded at once.
    void push(std::span<const std::uint8_t> bytes);
    // End of stream: decode the last unit and flush the reorder buffer.
    void finish();
    // Display-order frames produced so far.
    bool pop(Frame& frame);
    std::size_t ready() const { return output_.size(); }
    bool finished() const { return finished_; }
    int width() const { return width_; }
    int height() const { return height_; }
    const DecoderStats& stats() const { return stats_; }

    // Port-only (132vt2a2, research/VITA.md "Movies"): with a runner set (process-wide; none by default), a
    // picture's slices are collected and decoded together when the picture ends, through runner(count, job), which
    // must call job(0) .. job(count - 1) once each (on any threads) and return when all are done. Slices are
    // independent (each resets its quantiser, DC and motion vector predictors) and write their own macroblocks, so
    // the pictures are the same bytes; the frames come out at the same push calls as without a runner. Set it while
    // no decoder decodes.
    using SliceRunner = std::function<void(int count, const std::function<void(int)>& job)>;
    static void set_slice_runner(SliceRunner runner);

private:
    struct Planes {
        std::vector<std::uint8_t> y, u, v;
    };
    struct Prev {
        bool valid{};
        bool dirs[2]{};
        int motion_type{};
        int mv[2][2][2]{};     // [s][r][t]
        int field_select[2][2]{};   // [r][s]
    };
    class Bits;
    // The state a slice starts afresh (the quantiser, the DC and motion vector predictors, the last macroblock's
    // prediction for skipped ones) and its statistics.
    struct Slice {
        int quantiser{};
        int dc_pred[3]{};
        int pmv[2][2][2]{};   // [r][s][t]
        Prev prev{};
        PictureStats stats{};
    };
    struct PendingSlice {
        std::size_t offset{}, size{};
        int row{};
    };

    void unit(std::uint8_t code, const std::uint8_t* body, std::size_t size);
    void sequence_header(Bits& b);
    void extension(Bits& b);
    void picture_header(Bits& b);
    void start_picture();
    void finish_picture();
    void run_slices();
    void slice(Bits& b, int row, Slice& s);
    void skipped_macroblock(Slice& s, int address);
    void macroblock(Bits& b, Slice& s, int address);
    void motion_vectors(Bits& b, Slice& sl, int s, int motion_type, int (&mv)[2][2], int (&field_select)[2][2]);
    void motion_vector(Bits& b, Slice& sl, int r, int s, bool field, int (&vec)[2]);
#if defined(__ARM_NEON)
    void block(Bits& b, Slice& s, int index, bool intra, std::uint8_t* dst, int stride);
#else
    void block(Bits& b, Slice& s, int index, bool intra, std::int32_t* out);
#endif
    void predict(int address, const bool (&dirs)[2], int motion_type, const int (&mv)[2][2][2],
                 const int (&field_select)[2][2]);
    static void reset_pmv(Slice& s);
    int quantiser_scale(int code) const;

    // Sequence / picture state.
    int width_{}, height_{}, mb_width_{}, mb_height_{};
    std::array<std::uint8_t, 64> intra_matrix_{}, non_intra_matrix_{};
    int f_code_[2][2]{};
    int dc_precision_{}, picture_structure_{3}, frame_pred_frame_dct_{}, concealment_{};
    int q_scale_type_{}, intra_vlc_{}, alternate_{};
    int temporal_reference_{}, picture_type_{};
    bool have_sequence_{}, have_picture_extension_{};
    bool closed_gop_{true}, skip_picture_{}, in_picture_{};
    int start_sequence_{}, sequence_index_{};
    bool active_{true};
    // Slice / macroblock state: Slice, per slice; the picture's statistics.
    PictureStats picture_stats_{};
    // The picture's slices collected for the slice runner (bodies with their 16 zero bytes, back to back).
    std::vector<std::uint8_t> slice_bytes_;
    std::vector<PendingSlice> slices_;
    // Pictures.
    Planes current_, forward_, backward_;
    bool have_forward_{}, have_backward_{};
    bool have_pending_{};
    Frame pending_{};
    std::deque<Frame> output_;
    std::uint64_t decode_index_{};
    DecoderStats stats_{};
    // Start-code framing of the pushed bytes.
    std::vector<std::uint8_t> buffer_;
    std::size_t unit_start_{static_cast<std::size_t>(-1)};
    std::size_t scan_{};
    std::vector<std::uint8_t> scratch_;
    bool finished_{};
};

}  // namespace omega::mpeg2
