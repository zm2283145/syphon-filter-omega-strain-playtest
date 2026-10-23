#include "float_environment.hpp"
#include <cstdint>
#if defined(_M_X64)||defined(_M_IX86)||defined(__x86_64__)||defined(__i386__)
#include <xmmintrin.h>
#endif

namespace omega {
void enable_ps2_denormal_flush(){
#if defined(_M_X64)||defined(_M_IX86)||defined(__x86_64__)||defined(__i386__)
    _mm_setcsr(_mm_getcsr()|0x8040U); // FTZ (bit 15) and DAZ (bit 6)
#elif defined(__aarch64__)
    std::uint64_t fpcr;
    asm volatile("mrs %0, fpcr":"=r"(fpcr));
    fpcr|=std::uint64_t{1}<<24; // FZ
    asm volatile("msr fpcr, %0"::"r"(fpcr));
#elif defined(__arm__)
    std::uint32_t fpscr;
    asm volatile("vmrs %0, fpscr":"=r"(fpscr));
    fpscr|=std::uint32_t{1}<<24; // FZ
    asm volatile("vmsr fpscr, %0"::"r"(fpscr));
#else
#error "No denormal flush control for this architecture"
#endif
}
}
