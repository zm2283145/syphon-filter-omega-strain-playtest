#pragma once

// Port-only (checkpoint 132vt2): the PS Vita crash reporter's extra (crash_report_vita.cpp).

#include <string>

namespace omega::crash {

// The last show_message text (play_main's top-level catch), for the Vita app's error screen; empty when none.
std::string vita_last_message();

}  // namespace omega::crash
