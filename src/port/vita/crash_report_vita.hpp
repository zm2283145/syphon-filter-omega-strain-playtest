#pragma once

// Port-only (checkpoint 132vt2): the PS Vita crash reporter's extra (crash_report_vita.cpp).

#include <string>

namespace omega::crash {

// The last show_message text (play_main's top-level catch), for the Vita app's error screen; empty when none.
std::string vita_last_message();

// Port-only (132vs1): called before the shutdown watchdog ends the process (std::_Exit), so the Vita app's log writer
// can write the lines it still queues; null when unset.
inline void (*vita_before_exit)() = nullptr;

}  // namespace omega::crash
