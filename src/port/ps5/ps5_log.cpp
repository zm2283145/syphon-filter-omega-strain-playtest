// Port-only (132ps5c): the PS5 log (ps5_log.hpp). The network part follows the Melee PS5 port's ps5_log.c (a ring
// buffer drained by one sender thread, three failed connects and it stops trying).
#include "ps5_log.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>

namespace omega::ps5 {
namespace {

constexpr unsigned short log_port = 18200;
constexpr std::size_t ring_size = 1u << 20;

char g_ring[ring_size];
std::size_t g_head = 0, g_tail = 0;  // bytes queued / sent (under g_net_mutex)
pthread_mutex_t g_net_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_net_cond = PTHREAD_COND_INITIALIZER;
bool g_net_started = false, g_net_off = false;
char g_host[64] = {};

std::mutex g_mutex;  // the file and the line assembly
FILE* g_file = nullptr;
FILE* g_mirror = nullptr;
timespec g_start{};
std::string g_partial;

bool read_host(const char* path) {
    FILE* f = std::fopen(path, "r");
    if (!f) return false;
    char text[64] = {};
    const bool ok = std::fgets(text, sizeof text, f) != nullptr;
    std::fclose(f);
    if (!ok) return false;
    std::size_t n = std::strlen(text);
    while (n && (text[n - 1] == '\n' || text[n - 1] == '\r' || text[n - 1] == ' ')) text[--n] = 0;
    in_addr probe{};
    if (!n || inet_pton(AF_INET, text, &probe) != 1) return false;
    std::memcpy(g_host, text, n + 1);
    return true;
}

int connect_host() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    timeval timeout{2, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof timeout);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(log_port);
    inet_pton(AF_INET, g_host, &address.sin_addr);
    if (connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

void* net_thread(void*) {
    int fd = -1;
    unsigned failures = 0;
    for (;;) {
        pthread_mutex_lock(&g_net_mutex);
        while (g_head == g_tail) pthread_cond_wait(&g_net_cond, &g_net_mutex);
        const std::size_t head = g_head;
        std::size_t tail = g_tail;
        pthread_mutex_unlock(&g_net_mutex);
        if (fd < 0) {
            if (failures >= 3) {  // nobody listens: drop the queue and stop trying
                pthread_mutex_lock(&g_net_mutex);
                g_tail = g_head;
                g_net_off = true;
                pthread_mutex_unlock(&g_net_mutex);
                continue;
            }
            fd = connect_host();
            if (fd < 0) {
                ++failures;
                sleep(1);
                continue;
            }
        }
        while (tail != head) {
            const std::size_t at = tail % ring_size;
            std::size_t chunk = head - tail;
            if (chunk > ring_size - at) chunk = ring_size - at;
            const ssize_t sent = send(fd, g_ring + at, chunk, 0);
            if (sent <= 0) {
                close(fd);
                fd = -1;
                ++failures;
                break;
            }
            tail += std::size_t(sent);
            pthread_mutex_lock(&g_net_mutex);
            g_tail = tail;
            pthread_mutex_unlock(&g_net_mutex);
        }
    }
    return nullptr;
}

void send_line(const char* text, std::size_t size) {
    static bool host_checked = false;
    if (!host_checked) {
        host_checked = true;
        if (!read_host("/download0/log_host.txt") && !read_host("/app0/log_host.txt")) {
#ifdef OMEGA_PS5_LOG_HOST
            std::snprintf(g_host, sizeof g_host, "%s", OMEGA_PS5_LOG_HOST);
#endif
        }
    }
    if (!g_host[0]) return;
    pthread_mutex_lock(&g_net_mutex);
    if (!g_net_off) {
        if (!g_net_started) {
            g_net_started = true;
            pthread_t thread;
            if (pthread_create(&thread, nullptr, net_thread, nullptr) == 0) pthread_detach(thread);
        }
        if (g_head - g_tail + size <= ring_size) {
            for (std::size_t k = 0; k < size; ++k) g_ring[(g_head + k) % ring_size] = text[k];
            g_head += size;
            pthread_cond_signal(&g_net_cond);
        }
    }
    pthread_mutex_unlock(&g_net_mutex);
}

// 132ps5c hw10: the files are flushed at most every 0.25 s (and by log_flush: the fault handler, the exit paths), not
// after every line: the game logs tens of lines a second from its own threads.
double g_last_flush = 0.0;

// One complete line (without its newline), with g_mutex held.
void write_line(const char* text, std::size_t size) {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (g_start.tv_sec == 0 && g_start.tv_nsec == 0) g_start = now;
    const double t = double(now.tv_sec - g_start.tv_sec) + double(now.tv_nsec - g_start.tv_nsec) * 1e-9;
    char stamp[32];
    const int n = std::snprintf(stamp, sizeof stamp, "[%10.3f] ", t);
    if (!g_file) g_file = std::fopen(log_file, "w");
    if (g_file) {
        std::fwrite(stamp, 1, std::size_t(n), g_file);
        std::fwrite(text, 1, size, g_file);
        std::fputc('\n', g_file);
    }
    if (g_mirror) {
        std::fwrite(stamp, 1, std::size_t(n), g_mirror);
        std::fwrite(text, 1, size, g_mirror);
        std::fputc('\n', g_mirror);
    }
    if (t - g_last_flush >= 0.25) {
        g_last_flush = t;
        if (g_file) std::fflush(g_file);
        if (g_mirror) std::fflush(g_mirror);
    }
    std::string line(stamp, std::size_t(n));
    line.append(text, size);
    line += '\n';
    send_line(line.data(), line.size());
}

class LogBuf final : public std::streambuf {
protected:
    int overflow(int c) override {
        if (c == traits_type::eof()) return traits_type::not_eof(c);
        const char ch = char(c);
        log_text(&ch, 1);
        return c;
    }
    std::streamsize xsputn(const char* s, std::streamsize n) override {
        log_text(s, std::size_t(n));
        return n;
    }
};

}  // namespace

void log(const char* format, ...) {
    char text[2048];
    va_list args;
    va_start(args, format);
    int n = std::vsnprintf(text, sizeof text, format, args);
    va_end(args);
    if (n < 0) return;
    if (n >= int(sizeof text)) n = int(sizeof text) - 1;
    std::lock_guard lock(g_mutex);
    // A text with newlines is several lines.
    const char* at = text;
    const char* end = text + n;
    while (at <= end) {
        const char* nl = static_cast<const char*>(std::memchr(at, '\n', std::size_t(end - at)));
        const char* stop = nl ? nl : end;
        if (stop > at || !nl) write_line(at, std::size_t(stop - at));
        if (!nl) break;
        at = nl + 1;
        if (at == end) break;
    }
}

void log_text(const char* text, std::size_t size) {
    std::lock_guard lock(g_mutex);
    for (std::size_t k = 0; k < size; ++k) {
        if (text[k] == '\n') {
            write_line(g_partial.data(), g_partial.size());
            g_partial.clear();
        } else if (text[k] != '\r') {
            g_partial += text[k];
            if (g_partial.size() >= 4000) {
                write_line(g_partial.data(), g_partial.size());
                g_partial.clear();
            }
        }
    }
}

void log_flush() {
    std::lock_guard lock(g_mutex);
    if (!g_partial.empty()) {
        write_line(g_partial.data(), g_partial.size());
        g_partial.clear();
    }
    if (g_file) std::fflush(g_file);
    if (g_mirror) std::fflush(g_mirror);
}

bool log_mirror(const char* path) {
    std::lock_guard lock(g_mutex);
    if (g_mirror) std::fclose(g_mirror);
    g_mirror = std::fopen(path, "w");
    return g_mirror != nullptr;
}

void log_drain() {
    for (int k = 0; k < 50; ++k) {
        pthread_mutex_lock(&g_net_mutex);
        const bool empty = g_head == g_tail || g_net_off;
        pthread_mutex_unlock(&g_net_mutex);
        if (empty) return;
        usleep(20000);
    }
}

void redirect_streams() {
    static LogBuf* buf = new LogBuf;  // never destroyed: static destructors may still log
    std::cout.rdbuf(buf);
    std::cerr.rdbuf(buf);
    std::clog.rdbuf(buf);
}

}  // namespace omega::ps5