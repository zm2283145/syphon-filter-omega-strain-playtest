#pragma once
// Port-only (132ps5c, research/PS5.md "Paths"): the PS5 build's log. Every line goes to /download0/omega_ps5.log
// and to the mirror file (log_mirror: /data/omega_strain/omega_ps5.log, which FTP reaches after a crash; the title's
// /download0 is an image file then), flushed at once so a crash keeps it, with the seconds since the start, and to a TCP listener on port 18200
// (tools/ps5/log-listener.ps1) when a host is named: /download0/log_host.txt or /app0/log_host.txt (an IPv4 address),
// else the build's OMEGA_PS5_LOG_HOST. The network copy is sent by its own thread from a ring buffer, so a missing
// listener never stalls the game. std::cout / std::cerr (the shared code's log) are routed here by redirect_streams.

#include <cstddef>

namespace omega::ps5 {

void log(const char* format, ...) __attribute__((format(printf, 1, 2)));
// Text that may hold several lines or a partial one (the redirected streams); each complete line is logged.
void log_text(const char* text, std::size_t size);
void log_flush();
// A second log file (opened now; the lines so far are not copied). False when it cannot be opened.
bool log_mirror(const char* path);
// Waits up to a second for the network thread to send what is queued (the crash handler, before the process ends).
void log_drain();
// std::cout and std::cerr into the log (their lines keep their order with log()).
void redirect_streams();
inline constexpr const char* log_file = "/download0/omega_ps5.log";

}  // namespace omega::ps5