#pragma once

// Port-only (132ps5c): the PS5 crash reporter's extra (crash_report_ps5.cpp).

#include <string>

namespace omega::crash {

// The last show_message text (play_main's top-level catch), for the PS5 app's error screen; empty when none.
std::string ps5_last_message();

// Called before the shutdown watchdog ends the process (std::_Exit), so the log can be flushed; null when unset.
inline void (*ps5_before_exit)() = nullptr;

}  // namespace omega::crash