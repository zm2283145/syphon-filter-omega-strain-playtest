#include "float_environment.hpp"
#include <cstdint>
#if defined(_M_X64)||defined(_M_IX86)||defined(__x86_64__)||defined(__i386__)
#include <xmmintrin.h>
#endif

namespace omega {
namespace {
// The control register and the bits of it that are the mode (the rest are sticky exception flags).
#if defined(_M_X64)||defined(_M_IX86)||defined(__x86_64__)||defined(__i386__)
constexpr std::uint64_t mode_bits=0xffc0U;   // MXCSR: DAZ, the exception masks, rounding, FTZ
std::uint64_t read_controls(){return _mm_getcsr();}
void write_controls(std::uint64_t v){_mm_setcsr(static_cast<unsigned>(v));}
#elif defined(__aarch64__)
constexpr std::uint64_t mode_bits=~std::uint64_t{0};   // FPCR holds no flags
std::uint64_t read_controls(){std::uint64_t v;asm volatile("mrs %0, fpcr":"=r"(v));return v;}
void write_controls(std::uint64_t v){asm volatile("msr fpcr, %0"::"r"(v));}
#elif defined(__arm__)
constexpr std::uint64_t mode_bits=0x07ff9f00U;   // FPSCR: AHP, DN, FZ, RMode, Stride, Len, the trap enables
std::uint64_t read_controls(){std::uint32_t v;asm volatile("vmrs %0, fpscr":"=r"(v));return v;}
void write_controls(std::uint64_t v){asm volatile("vmsr fpscr, %0"::"r"(static_cast<std::uint32_t>(v)));}
#else
#error "No floating-point control access for this architecture"
#endif
}

void enable_ps2_denormal_flush(){
#if defined(_M_X64)||defined(_M_IX86)||defined(__x86_64__)||defined(__i386__)
    write_controls(read_controls()|0x8040U); // FTZ (bit 15) and DAZ (bit 6)
#else
    write_controls(read_controls()|(std::uint64_t{1}<<24)); // FZ
#endif
}

std::uint64_t float_controls(){return read_controls()&mode_bits;}

FloatControlsScope::FloatControlsScope(std::uint64_t controls):saved_(read_controls()),changed_((saved_&mode_bits)!=(controls&mode_bits)){
    if(changed_)write_controls((saved_&~mode_bits)|(controls&mode_bits));
}
FloatControlsScope::~FloatControlsScope(){
    if(changed_)write_controls((read_controls()&~mode_bits)|(saved_&mode_bits));
}
}
