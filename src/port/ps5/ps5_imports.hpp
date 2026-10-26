#pragma once
// Port-only (132ps5c): the start-up check of the title's imports (ps5_imports.cpp).

namespace omega::ps5 {

// Logs every imported symbol the loader left null; returns their count.
unsigned log_missing_imports();

}  // namespace omega::ps5
