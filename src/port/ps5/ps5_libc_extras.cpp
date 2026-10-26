// Port-only (132ps5c, research/PS5.md "Build"): the C library functions the SDK's libc++ and the shared code need that
// the PS5's libc does not export: utimensat over utimes, for paths only (std::filesystem's last_write_time setter), and
// the POSIX localtime_r / gmtime_r (card_backup.cpp, ps2_card_image.cpp, the crash reporter) over localtime / gmtime
// under one lock, and __cxa_thread_atexit_impl (libc++abi's thread_local destructors) over a pthread key.
// Also the functions the SDK's stubs import from libScePosixForWebKit, which a native title's loader leaves null
// (132ps5c hw5): isatty (no terminals: 0, ENOTTY), arc4random (the kernel's RNG through sysctl kern.arandom, as
// FreeBSD's own arc4random), and getaddrinfo / freeaddrinfo for numeric IPv4 hosts (a name: EAI_FAIL, no resolver).
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ctime>
#include <mutex>
#include <new>
#include <pthread.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>

extern "C" int utimensat(int fd, const char* path, const struct timespec times[2], int flag) {
    (void)flag;
    if (fd != AT_FDCWD) {
        errno = ENOSYS;
        return -1;
    }
    if (!times) return utimes(path, nullptr);
    const struct timeval tv[2]{{times[0].tv_sec, times[0].tv_nsec / 1000}, {times[1].tv_sec, times[1].tv_nsec / 1000}};
    return utimes(path, tv);
}
namespace {
std::mutex g_time_mutex;
}

extern "C" struct tm* localtime_r(const time_t* time, struct tm* result) {
    std::lock_guard lock(g_time_mutex);
    const struct tm* t = std::localtime(time);
    if (!t) return nullptr;
    *result = *t;
    return result;
}

extern "C" struct tm* gmtime_r(const time_t* time, struct tm* result) {
    std::lock_guard lock(g_time_mutex);
    const struct tm* t = std::gmtime(time);
    if (!t) return nullptr;
    *result = *t;
    return result;
}
// The destructors of a thread's thread_local objects, run in reverse order of registration when the thread exits.
namespace {
struct ThreadExit {
    void (*destructor)(void*);
    void* object;
    ThreadExit* next;
};
pthread_key_t g_exit_key;
pthread_once_t g_exit_once = PTHREAD_ONCE_INIT;
void run_thread_exits(void* head) {
    for (auto* e = static_cast<ThreadExit*>(head); e;) {
        ThreadExit* next = e->next;
        e->destructor(e->object);
        delete e;
        e = next;
    }
}
}  // namespace

// 132ps5c hw10: at the play's teardown every draw worker faulted inside these destructors (run_thread_exits ->
// free -> libSceLibcInternal, SIGBUS / SIGSEGV on all of them at once): the PS5 runs pthread key destructors after the
// exiting thread's TLS block is gone, so a thread_local object is destroyed from freed memory. Thread-local objects
// are therefore not destroyed when a thread exits (nothing is registered; the process's end returns the memory).
constexpr bool run_thread_local_destructors = false;

extern "C" int __cxa_thread_atexit_impl(void (*destructor)(void*), void* object, void*) {
    if (!run_thread_local_destructors) return 0;
    pthread_once(&g_exit_once, [] { pthread_key_create(&g_exit_key, run_thread_exits); });
    auto* e = new (std::nothrow) ThreadExit{destructor, object, static_cast<ThreadExit*>(pthread_getspecific(g_exit_key))};
    if (!e) return -1;
    return pthread_setspecific(g_exit_key, e) == 0 ? 0 : -1;
}

extern "C" int isatty(int fd) {
    if (fcntl(fd, F_GETFD) < 0) return 0;  // errno EBADF from fcntl
    errno = ENOTTY;
    return 0;
}

// FreeBSD's sysctl (libkernel); the SDK's <sys/sysctl.h> needs the BSD types this build does not expose.
extern "C" int sysctl(const int* name, unsigned name_length, void* old_value, std::size_t* old_length, const void* new_value,
                      std::size_t new_length);

namespace {
constexpr int ctl_kern = 1;   // CTL_KERN
constexpr int kern_arnd = 37; // KERN_ARND: bytes from arc4rand()
std::mutex g_random_mutex;
std::uint64_t g_fallback_state = 0;

// Bytes from the kernel's RNG (sysctl {CTL_KERN, KERN_ARND}, at most 256 a call); if the kernel refuses, a SplitMix64
// stream seeded from the clock (never expected: logged by nobody, but arc4random cannot fail).
void random_bytes(void* out, std::size_t size) {
    auto* bytes = static_cast<unsigned char*>(out);
    while (size) {
        int mib[2]{ctl_kern, kern_arnd};
        std::size_t chunk = size < 256 ? size : 256;
        std::size_t got = chunk;
        if (sysctl(mib, 2, bytes, &got, nullptr, 0) == 0 && got > 0) {
            bytes += got;
            size -= got;
            continue;
        }
        std::lock_guard lock(g_random_mutex);
        if (g_fallback_state == 0) {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            g_fallback_state = (std::uint64_t(now.tv_sec) << 32) ^ std::uint64_t(now.tv_nsec) ^ 0x6A09E667F3BCC909ull;
        }
        for (std::size_t i = 0; i < chunk; ++i) {
            std::uint64_t z = (g_fallback_state += 0x9E3779B97F4A7C15ull);
            z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
            z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
            bytes[i] = static_cast<unsigned char>(z ^ (z >> 31));
        }
        bytes += chunk;
        size -= chunk;
    }
}
}  // namespace

extern "C" std::uint32_t arc4random(void) {
    std::uint32_t value;
    random_bytes(&value, sizeof value);
    return value;
}

extern "C" void arc4random_buf(void* buffer, std::size_t size) { random_bytes(buffer, size); }

extern "C" int getaddrinfo(const char* host, const char* service, const struct addrinfo* hints,
                           struct addrinfo** result) {
    if (!result || (!host && !service)) return EAI_NONAME;
    *result = nullptr;
    const int family = hints ? hints->ai_family : AF_UNSPEC;
    if (family != AF_UNSPEC && family != AF_INET) return EAI_FAMILY;
    const int flags = hints ? hints->ai_flags : 0;
    in_addr address{};
    if (host) {
        if (inet_pton(AF_INET, host, &address) != 1) return (flags & AI_NUMERICHOST) ? EAI_NONAME : EAI_FAIL;
    } else {
        address.s_addr = htonl((flags & AI_PASSIVE) ? INADDR_ANY : INADDR_LOOPBACK);
    }
    unsigned long port = 0;
    if (service && *service) {
        char* end = nullptr;
        port = std::strtoul(service, &end, 10);
        if (*end || port > 65535) return EAI_SERVICE;  // numeric services only (no services database)
    }
    auto* info = static_cast<struct addrinfo*>(std::calloc(1, sizeof(struct addrinfo) + sizeof(sockaddr_in)));
    if (!info) return EAI_MEMORY;
    auto* sa = reinterpret_cast<sockaddr_in*>(info + 1);
    sa->sin_len = sizeof(sockaddr_in);
    sa->sin_family = AF_INET;
    sa->sin_port = htons(static_cast<std::uint16_t>(port));
    sa->sin_addr = address;
    info->ai_family = AF_INET;
    info->ai_socktype = hints ? hints->ai_socktype : 0;
    info->ai_protocol = hints ? hints->ai_protocol : 0;
    info->ai_addrlen = sizeof(sockaddr_in);
    info->ai_addr = reinterpret_cast<sockaddr*>(sa);
    *result = info;
    return 0;
}

extern "C" void freeaddrinfo(struct addrinfo* info) {
    while (info) {
        struct addrinfo* next = info->ai_next;
        std::free(info);  // one block: the addrinfo and its sockaddr_in
        info = next;
    }
}

// 132ps5c hw11: every C++ throw is logged (the type and what(), the throwing code's address), up to 64 and then
// every 1000th: the hw10 run ended in the play's teardown after an exception whose message the faulting teardown never
// let the top-level catch print. The link wraps __cxa_throw (OMEGA_PS5_PATH_WRAPS).
#include <filesystem>
#include <ios>
#include <stdexcept>
#include <system_error>
#include <typeinfo>

#include "ps5_log.hpp"

extern "C" [[noreturn]] void __real___cxa_throw(void* object, std::type_info* type, void (*destroy)(void*));

namespace {
const char* exception_text(void* object, const std::type_info& type) {
#define OMEGA_PS5_EXCEPTION(T) \
    if (type == typeid(T)) return static_cast<const T*>(object)->what();
    OMEGA_PS5_EXCEPTION(std::runtime_error)
    OMEGA_PS5_EXCEPTION(std::logic_error)
    OMEGA_PS5_EXCEPTION(std::out_of_range)
    OMEGA_PS5_EXCEPTION(std::invalid_argument)
    OMEGA_PS5_EXCEPTION(std::length_error)
    OMEGA_PS5_EXCEPTION(std::domain_error)
    OMEGA_PS5_EXCEPTION(std::range_error)
    OMEGA_PS5_EXCEPTION(std::overflow_error)
    OMEGA_PS5_EXCEPTION(std::underflow_error)
    OMEGA_PS5_EXCEPTION(std::system_error)
    OMEGA_PS5_EXCEPTION(std::filesystem::filesystem_error)
    OMEGA_PS5_EXCEPTION(std::ios_base::failure)
    OMEGA_PS5_EXCEPTION(std::bad_alloc)
    OMEGA_PS5_EXCEPTION(std::bad_array_new_length)
    OMEGA_PS5_EXCEPTION(std::exception)
#undef OMEGA_PS5_EXCEPTION
    return nullptr;
}
std::atomic<unsigned long long> g_throws{0};
}  // namespace

extern "C" [[noreturn]] void __wrap___cxa_throw(void* object, std::type_info* type, void (*destroy)(void*)) {
    const unsigned long long n = ++g_throws;
    if (n <= 64 || n % 1000 == 0) {
        const char* text = type ? exception_text(object, *type) : nullptr;
        omega::ps5::log("throw %llu: %s from %p: %s", n, type ? type->name() : "?", __builtin_return_address(0),
                        text ? text : "(not a standard exception)");
    }
    __real___cxa_throw(object, type, destroy);
}
