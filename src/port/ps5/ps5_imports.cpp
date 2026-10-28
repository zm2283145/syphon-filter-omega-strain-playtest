// Port-only (132ps5c, research/PS5.md "Imports"): GENERATED from the link's imported symbols (llvm-nm -u of
// llvm-pie.elf, the wrapped ones through their __real_ names). The PS5 loader leaves an import that no system
// module exports as a null pointer, and calling one faults with rip 0 (132ps5c hw1: getcwd; hw4: mkstemp in the
// GL driver's shader cache). log_missing_imports() logs every import that is null at run time, at the start.
// No headers on purpose: the symbols are declared as bytes, only their addresses are read.

#include "ps5_imports.hpp"

#include "ps5_log.hpp"

extern "C" {
extern char _Exit;
extern char __cxa_atexit;
extern char __error;
extern char __inet_pton;
extern char __isfinite;
extern char __isfinitef;
extern char __isinff;
extern char __isnormal;
extern char __isnormalf;
extern char __isthreaded;
extern char __mb_cur_max;
extern char __srget;
extern char __stderrp;
extern char __stdinp;
extern char __stdoutp;
extern char _fstatfs;
extern char _init_env;
extern char _umtx_op;
extern char abort;
extern char accept;
extern char __real_access;
extern char acos;
extern char acosf;
extern char aligned_alloc;
extern char asinf;
extern char asprintf;
extern char atan2;
extern char atan2f;
extern char atanf;
extern char atexit;
extern char atof;
extern char atoi;
extern char bind;
extern char bsearch;
extern char __real_calloc;
extern char cbrt;
extern char chmod;
extern char clearerr;
extern char clock_gettime;
extern char close;
extern char connect;
extern char cos;
extern char cosf;
extern char exit;
extern char exp;
extern char exp2;
extern char exp2f;
extern char expf;
extern char fchmod;
extern char fclose;
extern char fcntl;
extern char fdopen;
extern char ferror;
extern char fflush;
extern char fgetc;
extern char fgets;
extern char fgetwc;
extern char fileno;
extern char fmodf;
extern char __real_fopen;
extern char fprintf;
extern char fputc;
extern char fputs;
extern char fputwc;
extern char fread;
extern char __real_free;
extern char freeifaddrs;
extern char __real_freopen;
extern char frexp;
extern char frexpf;
extern char fseek;
extern char fseeko;
extern char fstat;
extern char fsync;
extern char ftell;
extern char ftello;
extern char ftruncate;
extern char fwrite;
extern char getc;
extern char getdents;
extern char getegid;
extern char getenv;
extern char geteuid;
extern char getgid;
extern char getifaddrs;
extern char getpid;
extern char getprogname;
extern char getrlimit;
extern char getrusage;
extern char getsockname;
extern char getsockopt;
extern char getuid;
extern char gmtime;
extern char hypotf;
extern char isalnum;
extern char isspace;
extern char isxdigit;
extern char ldexp;
extern char ldexpf;
extern char listen;
extern char llround;
extern char localtime;
extern char log;
extern char log10;
extern char log2;
extern char log2f;
extern char logf;
extern char longjmp;
extern char lround;
extern char lroundf;
extern char __real_lstat;
extern char __real_malloc;
extern char __real_malloc_usable_size;
extern char mbrlen;
extern char mbrtowc;
extern char mbsrtowcs;
extern char mbtowc;
extern char memchr;
extern char memcmp;
extern char memcpy;
extern char memmove;
extern char memset;
extern char __real_mkdir;
extern char mmap;
extern char munmap;
extern char nanosleep;
extern char nextafterf;
extern char __real_open;
extern char poll;
extern char __real_posix_memalign;
extern char pow;
extern char powf;
extern char printf;
extern char pthread_attr_destroy;
extern char pthread_attr_init;
extern char pthread_attr_setstacksize;
extern char pthread_barrier_destroy;
extern char pthread_barrier_init;
extern char pthread_barrier_wait;
extern char pthread_cond_broadcast;
extern char pthread_cond_destroy;
extern char pthread_cond_init;
extern char pthread_cond_signal;
extern char pthread_cond_timedwait;
extern char pthread_cond_wait;
extern char pthread_create;
extern char pthread_detach;
extern char pthread_equal;
extern char pthread_exit;
extern char pthread_getcpuclockid;
extern char pthread_getspecific;
extern char pthread_join;
extern char pthread_key_create;
extern char pthread_key_delete;
extern char pthread_mutex_destroy;
extern char pthread_mutex_init;
extern char pthread_mutex_lock;
extern char pthread_mutex_timedlock;
extern char pthread_mutex_trylock;
extern char pthread_mutex_unlock;
extern char pthread_mutexattr_destroy;
extern char pthread_mutexattr_init;
extern char pthread_mutexattr_settype;
extern char pthread_once;
extern char pthread_rwlock_rdlock;
extern char pthread_rwlock_unlock;
extern char pthread_rwlock_wrlock;
extern char pthread_self;
extern char pthread_set_name_np;
extern char pthread_setspecific;
extern char pthread_sigmask;
extern char putchar;
extern char puts;
extern char qsort;
extern char raise;
extern char rand;
extern char read;
extern char __real_realloc;
extern char recv;
extern char recvfrom;
extern char remainderf;
extern char __real_remove;
extern char __real_rename;
extern char rmdir;
extern char sceAgcCbDispatch;
extern char sceAgcCbReleaseMem;
extern char sceAgcCbSetShRegisterRangeDirect;
extern char sceAgcCreateShader;
extern char sceAgcDcbAcquireMem;
extern char sceAgcDcbDrawIndex;
extern char sceAgcDcbDrawIndexAuto;
extern char sceAgcDcbSetCxRegistersIndirect;
extern char sceAgcDcbSetFlip;
extern char sceAgcDcbSetIndexBuffer;
extern char sceAgcDcbSetIndexCount;
extern char sceAgcDcbSetIndexSize;
extern char sceAgcDcbSetNumInstances;
extern char sceAgcDcbSetShRegistersIndirect;
extern char sceAgcDcbSetUcRegistersIndirect;
extern char sceAgcDriverGetTFRing;
extern char sceAgcDriverGetWaitRenderingPacketSizeInDwords;
extern char sceAgcDriverSetTFRing;
extern char sceAgcDriverSubmitDcb;
extern char sceAgcDriverWaitUntilSafeForRendering;
extern char sceAgcGetRegisterDefaults;
extern char sceAgcInit;
extern char sceAgcLinkShaders;
extern char sceAgcSuspendPoint;
extern char sceAudioOutClose;
extern char sceAudioOutInit;
extern char sceAudioOutOpen;
extern char sceAudioOutOutput;
extern char sceKernelAllocateDirectMemory;
extern char sceKernelAvailableFlexibleMemorySize;
extern char sceKernelGetDirectMemorySize;
extern char sceKernelGetModuleInfoFromAddr;
extern char sceKernelMapDirectMemory;
extern char sceKernelReleaseDirectMemory;
extern char sceKernelUsleep;
extern char sceLibcMspaceCalloc;
extern char sceLibcMspaceCreate;
extern char sceLibcMspaceFree;
extern char sceLibcMspaceMalloc;
extern char sceLibcMspaceMallocUsableSize;
extern char sceLibcMspacePosixMemalign;
extern char sceLibcMspaceRealloc;
extern char scePadClose;
extern char scePadInit;
extern char scePadOpen;
extern char scePadReadState;
extern char scePadSetVibration;
extern char scePthreadSetprio;
extern char sceSystemServiceHideSplashScreen;
extern char sceUserServiceGetLoginUserIdList;
extern char sceUserServiceInitialize;
extern char sceVideoOutClose;
extern char sceVideoOutConfigureOutput;
extern char sceVideoOutGetFlipStatus;
extern char sceVideoOutGetOutputStatus;
extern char sceVideoOutGetResolutionStatus;
extern char sceVideoOutIsFlipPending;
extern char sceVideoOutIsOutputSupported;
extern char sceVideoOutOpen;
extern char sceVideoOutRegisterBuffers2;
extern char sceVideoOutSetBufferAttribute2;
extern char sceVideoOutSetFlipRate;
extern char sceVideoOutSubmitFlip;
extern char sceVideoOutUnregisterBuffers;
extern char sceVideoOutWaitVblank;
extern char sched_yield;
extern char select;
extern char send;
extern char sendto;
extern char setenv;
extern char setjmp;
extern char setlocale;
extern char setsockopt;
extern char setvbuf;
extern char sigaction;
extern char sigdelset;
extern char sigfillset;
extern char signal;
extern char sin;
extern char sincos;
extern char sincosf;
extern char sinf;
extern char sleep;
extern char snprintf;
extern char socket;
extern char sprintf;
extern char sscanf;
extern char __real_stat;
extern char strcasecmp;
extern char strcat;
extern char strchr;
extern char strcmp;
extern char strcoll;
extern char strcpy;
extern char strcspn;
extern char strdup;
extern char strerror;
extern char strerror_r;
extern char strftime;
extern char strlen;
extern char strncat;
extern char strncmp;
extern char strncpy;
extern char strndup;
extern char strnlen;
extern char strpbrk;
extern char strrchr;
extern char strstr;
extern char strtod;
extern char strtof;
extern char strtok;
extern char strtok_r;
extern char strtol;
extern char strtold;
extern char strtoll;
extern char strtoul;
extern char strtoull;
extern char strxfrm;
extern char swprintf;
extern char sysconf;
extern char sysctl;
extern char sysctlbyname;
extern char syslog;
extern char system;
extern char tan;
extern char tanf;
extern char time;
extern char tolower;
extern char toupper;
extern char towlower;
extern char __real_truncate;
extern char ungetc;
extern char ungetwc;
extern char __real_unlink;
extern char unsetenv;
extern char usleep;
extern char __real_utimes;
extern char vasprintf;
extern char vfprintf;
extern char vsnprintf;
extern char vsprintf;
extern char vsscanf;
extern char wcrtomb;
extern char wcscoll;
extern char wcslen;
extern char wcstod;
extern char wcstof;
extern char wcstol;
extern char wcstold;
extern char wcstoll;
extern char wcstoul;
extern char wcstoull;
extern char wcsxfrm;
extern char wmemchr;
extern char wmemcmp;
extern char write;
}

namespace omega::ps5 {
namespace {

struct Import {
    const char* name;
    const void* address;
};

}  // namespace

unsigned log_missing_imports() {
    // Filled at run time (each address loaded through the GOT, as the calls do), not a static table.
    const Import imports[]{
        {"_Exit", &::_Exit},
        {"__cxa_atexit", &::__cxa_atexit},
        {"__error", &::__error},
        {"__inet_pton", &::__inet_pton},
        {"__isfinite", &::__isfinite},
        {"__isfinitef", &::__isfinitef},
        {"__isinff", &::__isinff},
        {"__isnormal", &::__isnormal},
        {"__isnormalf", &::__isnormalf},
        {"__isthreaded", &::__isthreaded},
        {"__mb_cur_max", &::__mb_cur_max},
        {"__srget", &::__srget},
        {"__stderrp", &::__stderrp},
        {"__stdinp", &::__stdinp},
        {"__stdoutp", &::__stdoutp},
        {"_fstatfs", &::_fstatfs},
        {"_init_env", &::_init_env},
        {"_umtx_op", &::_umtx_op},
        {"abort", &::abort},
        {"accept", &::accept},
        {"access", &::__real_access},
        {"acos", &::acos},
        {"acosf", &::acosf},
        {"aligned_alloc", &::aligned_alloc},
        {"asinf", &::asinf},
        {"asprintf", &::asprintf},
        {"atan2", &::atan2},
        {"atan2f", &::atan2f},
        {"atanf", &::atanf},
        {"atexit", &::atexit},
        {"atof", &::atof},
        {"atoi", &::atoi},
        {"bind", &::bind},
        {"bsearch", &::bsearch},
        {"calloc", &::__real_calloc},
        {"cbrt", &::cbrt},
        {"chmod", &::chmod},
        {"clearerr", &::clearerr},
        {"clock_gettime", &::clock_gettime},
        {"close", &::close},
        {"connect", &::connect},
        {"cos", &::cos},
        {"cosf", &::cosf},
        {"exit", &::exit},
        {"exp", &::exp},
        {"exp2", &::exp2},
        {"exp2f", &::exp2f},
        {"expf", &::expf},
        {"fchmod", &::fchmod},
        {"fclose", &::fclose},
        {"fcntl", &::fcntl},
        {"fdopen", &::fdopen},
        {"ferror", &::ferror},
        {"fflush", &::fflush},
        {"fgetc", &::fgetc},
        {"fgets", &::fgets},
        {"fgetwc", &::fgetwc},
        {"fileno", &::fileno},
        {"fmodf", &::fmodf},
        {"fopen", &::__real_fopen},
        {"fprintf", &::fprintf},
        {"fputc", &::fputc},
        {"fputs", &::fputs},
        {"fputwc", &::fputwc},
        {"fread", &::fread},
        {"free", &::__real_free},
        {"freeifaddrs", &::freeifaddrs},
        {"freopen", &::__real_freopen},
        {"frexp", &::frexp},
        {"frexpf", &::frexpf},
        {"fseek", &::fseek},
        {"fseeko", &::fseeko},
        {"fstat", &::fstat},
        {"fsync", &::fsync},
        {"ftell", &::ftell},
        {"ftello", &::ftello},
        {"ftruncate", &::ftruncate},
        {"fwrite", &::fwrite},
        {"getc", &::getc},
        {"getdents", &::getdents},
        {"getegid", &::getegid},
        {"getenv", &::getenv},
        {"geteuid", &::geteuid},
        {"getgid", &::getgid},
        {"getifaddrs", &::getifaddrs},
        {"getpid", &::getpid},
        {"getprogname", &::getprogname},
        {"getrlimit", &::getrlimit},
        {"getrusage", &::getrusage},
        {"getsockname", &::getsockname},
        {"getsockopt", &::getsockopt},
        {"getuid", &::getuid},
        {"gmtime", &::gmtime},
        {"hypotf", &::hypotf},
        {"isalnum", &::isalnum},
        {"isspace", &::isspace},
        {"isxdigit", &::isxdigit},
        {"ldexp", &::ldexp},
        {"ldexpf", &::ldexpf},
        {"listen", &::listen},
        {"llround", &::llround},
        {"localtime", &::localtime},
        {"log", &::log},
        {"log10", &::log10},
        {"log2", &::log2},
        {"log2f", &::log2f},
        {"logf", &::logf},
        {"longjmp", &::longjmp},
        {"lround", &::lround},
        {"lroundf", &::lroundf},
        {"lstat", &::__real_lstat},
        {"malloc", &::__real_malloc},
        {"malloc_usable_size", &::__real_malloc_usable_size},
        {"mbrlen", &::mbrlen},
        {"mbrtowc", &::mbrtowc},
        {"mbsrtowcs", &::mbsrtowcs},
        {"mbtowc", &::mbtowc},
        {"memchr", &::memchr},
        {"memcmp", &::memcmp},
        {"memcpy", &::memcpy},
        {"memmove", &::memmove},
        {"memset", &::memset},
        {"mkdir", &::__real_mkdir},
        {"mmap", &::mmap},
        {"munmap", &::munmap},
        {"nanosleep", &::nanosleep},
        {"nextafterf", &::nextafterf},
        {"open", &::__real_open},
        {"poll", &::poll},
        {"posix_memalign", &::__real_posix_memalign},
        {"pow", &::pow},
        {"powf", &::powf},
        {"printf", &::printf},
        {"pthread_attr_destroy", &::pthread_attr_destroy},
        {"pthread_attr_init", &::pthread_attr_init},
        {"pthread_attr_setstacksize", &::pthread_attr_setstacksize},
        {"pthread_barrier_destroy", &::pthread_barrier_destroy},
        {"pthread_barrier_init", &::pthread_barrier_init},
        {"pthread_barrier_wait", &::pthread_barrier_wait},
        {"pthread_cond_broadcast", &::pthread_cond_broadcast},
        {"pthread_cond_destroy", &::pthread_cond_destroy},
        {"pthread_cond_init", &::pthread_cond_init},
        {"pthread_cond_signal", &::pthread_cond_signal},
        {"pthread_cond_timedwait", &::pthread_cond_timedwait},
        {"pthread_cond_wait", &::pthread_cond_wait},
        {"pthread_create", &::pthread_create},
        {"pthread_detach", &::pthread_detach},
        {"pthread_equal", &::pthread_equal},
        {"pthread_exit", &::pthread_exit},
        {"pthread_getcpuclockid", &::pthread_getcpuclockid},
        {"pthread_getspecific", &::pthread_getspecific},
        {"pthread_join", &::pthread_join},
        {"pthread_key_create", &::pthread_key_create},
        {"pthread_key_delete", &::pthread_key_delete},
        {"pthread_mutex_destroy", &::pthread_mutex_destroy},
        {"pthread_mutex_init", &::pthread_mutex_init},
        {"pthread_mutex_lock", &::pthread_mutex_lock},
        {"pthread_mutex_timedlock", &::pthread_mutex_timedlock},
        {"pthread_mutex_trylock", &::pthread_mutex_trylock},
        {"pthread_mutex_unlock", &::pthread_mutex_unlock},
        {"pthread_mutexattr_destroy", &::pthread_mutexattr_destroy},
        {"pthread_mutexattr_init", &::pthread_mutexattr_init},
        {"pthread_mutexattr_settype", &::pthread_mutexattr_settype},
        {"pthread_once", &::pthread_once},
        {"pthread_rwlock_rdlock", &::pthread_rwlock_rdlock},
        {"pthread_rwlock_unlock", &::pthread_rwlock_unlock},
        {"pthread_rwlock_wrlock", &::pthread_rwlock_wrlock},
        {"pthread_self", &::pthread_self},
        {"pthread_set_name_np", &::pthread_set_name_np},
        {"pthread_setspecific", &::pthread_setspecific},
        {"pthread_sigmask", &::pthread_sigmask},
        {"putchar", &::putchar},
        {"puts", &::puts},
        {"qsort", &::qsort},
        {"raise", &::raise},
        {"rand", &::rand},
        {"read", &::read},
        {"realloc", &::__real_realloc},
        {"recv", &::recv},
        {"recvfrom", &::recvfrom},
        {"remainderf", &::remainderf},
        {"remove", &::__real_remove},
        {"rename", &::__real_rename},
        {"rmdir", &::rmdir},
        {"sceAgcCbDispatch", &::sceAgcCbDispatch},
        {"sceAgcCbReleaseMem", &::sceAgcCbReleaseMem},
        {"sceAgcCbSetShRegisterRangeDirect", &::sceAgcCbSetShRegisterRangeDirect},
        {"sceAgcCreateShader", &::sceAgcCreateShader},
        {"sceAgcDcbAcquireMem", &::sceAgcDcbAcquireMem},
        {"sceAgcDcbDrawIndex", &::sceAgcDcbDrawIndex},
        {"sceAgcDcbDrawIndexAuto", &::sceAgcDcbDrawIndexAuto},
        {"sceAgcDcbSetCxRegistersIndirect", &::sceAgcDcbSetCxRegistersIndirect},
        {"sceAgcDcbSetFlip", &::sceAgcDcbSetFlip},
        {"sceAgcDcbSetIndexBuffer", &::sceAgcDcbSetIndexBuffer},
        {"sceAgcDcbSetIndexCount", &::sceAgcDcbSetIndexCount},
        {"sceAgcDcbSetIndexSize", &::sceAgcDcbSetIndexSize},
        {"sceAgcDcbSetNumInstances", &::sceAgcDcbSetNumInstances},
        {"sceAgcDcbSetShRegistersIndirect", &::sceAgcDcbSetShRegistersIndirect},
        {"sceAgcDcbSetUcRegistersIndirect", &::sceAgcDcbSetUcRegistersIndirect},
        {"sceAgcDriverGetTFRing", &::sceAgcDriverGetTFRing},
        {"sceAgcDriverGetWaitRenderingPacketSizeInDwords", &::sceAgcDriverGetWaitRenderingPacketSizeInDwords},
        {"sceAgcDriverSetTFRing", &::sceAgcDriverSetTFRing},
        {"sceAgcDriverSubmitDcb", &::sceAgcDriverSubmitDcb},
        {"sceAgcDriverWaitUntilSafeForRendering", &::sceAgcDriverWaitUntilSafeForRendering},
        {"sceAgcGetRegisterDefaults", &::sceAgcGetRegisterDefaults},
        {"sceAgcInit", &::sceAgcInit},
        {"sceAgcLinkShaders", &::sceAgcLinkShaders},
        {"sceAgcSuspendPoint", &::sceAgcSuspendPoint},
        {"sceAudioOutClose", &::sceAudioOutClose},
        {"sceAudioOutInit", &::sceAudioOutInit},
        {"sceAudioOutOpen", &::sceAudioOutOpen},
        {"sceAudioOutOutput", &::sceAudioOutOutput},
        {"sceKernelAllocateDirectMemory", &::sceKernelAllocateDirectMemory},
        {"sceKernelAvailableFlexibleMemorySize", &::sceKernelAvailableFlexibleMemorySize},
        {"sceKernelGetDirectMemorySize", &::sceKernelGetDirectMemorySize},
        {"sceKernelGetModuleInfoFromAddr", &::sceKernelGetModuleInfoFromAddr},
        {"sceKernelMapDirectMemory", &::sceKernelMapDirectMemory},
        {"sceKernelReleaseDirectMemory", &::sceKernelReleaseDirectMemory},
        {"sceKernelUsleep", &::sceKernelUsleep},
        {"sceLibcMspaceCalloc", &::sceLibcMspaceCalloc},
        {"sceLibcMspaceCreate", &::sceLibcMspaceCreate},
        {"sceLibcMspaceFree", &::sceLibcMspaceFree},
        {"sceLibcMspaceMalloc", &::sceLibcMspaceMalloc},
        {"sceLibcMspaceMallocUsableSize", &::sceLibcMspaceMallocUsableSize},
        {"sceLibcMspacePosixMemalign", &::sceLibcMspacePosixMemalign},
        {"sceLibcMspaceRealloc", &::sceLibcMspaceRealloc},
        {"scePadClose", &::scePadClose},
        {"scePadInit", &::scePadInit},
        {"scePadOpen", &::scePadOpen},
        {"scePadReadState", &::scePadReadState},
        {"scePadSetVibration", &::scePadSetVibration},
        {"scePthreadSetprio", &::scePthreadSetprio},
        {"sceSystemServiceHideSplashScreen", &::sceSystemServiceHideSplashScreen},
        {"sceUserServiceGetLoginUserIdList", &::sceUserServiceGetLoginUserIdList},
        {"sceUserServiceInitialize", &::sceUserServiceInitialize},
        {"sceVideoOutClose", &::sceVideoOutClose},
        {"sceVideoOutConfigureOutput", &::sceVideoOutConfigureOutput},
        {"sceVideoOutGetFlipStatus", &::sceVideoOutGetFlipStatus},
        {"sceVideoOutGetOutputStatus", &::sceVideoOutGetOutputStatus},
        {"sceVideoOutGetResolutionStatus", &::sceVideoOutGetResolutionStatus},
        {"sceVideoOutIsFlipPending", &::sceVideoOutIsFlipPending},
        {"sceVideoOutIsOutputSupported", &::sceVideoOutIsOutputSupported},
        {"sceVideoOutOpen", &::sceVideoOutOpen},
        {"sceVideoOutRegisterBuffers2", &::sceVideoOutRegisterBuffers2},
        {"sceVideoOutSetBufferAttribute2", &::sceVideoOutSetBufferAttribute2},
        {"sceVideoOutSetFlipRate", &::sceVideoOutSetFlipRate},
        {"sceVideoOutSubmitFlip", &::sceVideoOutSubmitFlip},
        {"sceVideoOutUnregisterBuffers", &::sceVideoOutUnregisterBuffers},
        {"sceVideoOutWaitVblank", &::sceVideoOutWaitVblank},
        {"sched_yield", &::sched_yield},
        {"select", &::select},
        {"send", &::send},
        {"sendto", &::sendto},
        {"setenv", &::setenv},
        {"setjmp", &::setjmp},
        {"setlocale", &::setlocale},
        {"setsockopt", &::setsockopt},
        {"setvbuf", &::setvbuf},
        {"sigaction", &::sigaction},
        {"sigdelset", &::sigdelset},
        {"sigfillset", &::sigfillset},
        {"signal", &::signal},
        {"sin", &::sin},
        {"sincos", &::sincos},
        {"sincosf", &::sincosf},
        {"sinf", &::sinf},
        {"sleep", &::sleep},
        {"snprintf", &::snprintf},
        {"socket", &::socket},
        {"sprintf", &::sprintf},
        {"sscanf", &::sscanf},
        {"stat", &::__real_stat},
        {"strcasecmp", &::strcasecmp},
        {"strcat", &::strcat},
        {"strchr", &::strchr},
        {"strcmp", &::strcmp},
        {"strcoll", &::strcoll},
        {"strcpy", &::strcpy},
        {"strcspn", &::strcspn},
        {"strdup", &::strdup},
        {"strerror", &::strerror},
        {"strerror_r", &::strerror_r},
        {"strftime", &::strftime},
        {"strlen", &::strlen},
        {"strncat", &::strncat},
        {"strncmp", &::strncmp},
        {"strncpy", &::strncpy},
        {"strndup", &::strndup},
        {"strnlen", &::strnlen},
        {"strpbrk", &::strpbrk},
        {"strrchr", &::strrchr},
        {"strstr", &::strstr},
        {"strtod", &::strtod},
        {"strtof", &::strtof},
        {"strtok", &::strtok},
        {"strtok_r", &::strtok_r},
        {"strtol", &::strtol},
        {"strtold", &::strtold},
        {"strtoll", &::strtoll},
        {"strtoul", &::strtoul},
        {"strtoull", &::strtoull},
        {"strxfrm", &::strxfrm},
        {"swprintf", &::swprintf},
        {"sysconf", &::sysconf},
        {"sysctl", &::sysctl},
        {"sysctlbyname", &::sysctlbyname},
        {"syslog", &::syslog},
        {"system", &::system},
        {"tan", &::tan},
        {"tanf", &::tanf},
        {"time", &::time},
        {"tolower", &::tolower},
        {"toupper", &::toupper},
        {"towlower", &::towlower},
        {"truncate", &::__real_truncate},
        {"ungetc", &::ungetc},
        {"ungetwc", &::ungetwc},
        {"unlink", &::__real_unlink},
        {"unsetenv", &::unsetenv},
        {"usleep", &::usleep},
        {"utimes", &::__real_utimes},
        {"vasprintf", &::vasprintf},
        {"vfprintf", &::vfprintf},
        {"vsnprintf", &::vsnprintf},
        {"vsprintf", &::vsprintf},
        {"vsscanf", &::vsscanf},
        {"wcrtomb", &::wcrtomb},
        {"wcscoll", &::wcscoll},
        {"wcslen", &::wcslen},
        {"wcstod", &::wcstod},
        {"wcstof", &::wcstof},
        {"wcstol", &::wcstol},
        {"wcstold", &::wcstold},
        {"wcstoll", &::wcstoll},
        {"wcstoul", &::wcstoul},
        {"wcstoull", &::wcstoull},
        {"wcsxfrm", &::wcsxfrm},
        {"wmemchr", &::wmemchr},
        {"wmemcmp", &::wmemcmp},
        {"write", &::write},
    };
    unsigned missing = 0;
    for (const Import& i : imports) {
        // Read through a volatile pointer: the compiler may assume a symbol's address is never null.
        const void* const volatile* slot = &i.address;
        if (*slot == nullptr) {
            ++missing;
            log("import %s: NOT RESOLVED (null; a call faults with rip 0)", i.name);
        }
    }
    log("imports: %u checked, %u not resolved", unsigned(sizeof imports / sizeof imports[0]), missing);
    return missing;
}

}  // namespace omega::ps5
