#pragma once

#include "book_state.h"

#include <string>
#include <vector>

namespace hyperion {

// Minimal PDF (single page, Helvetica) listing uncertain positions first,
// then known holdings — for Alpaca ToS hedge readiness.
std::string build_hedge_pdf(const ServerSnapshot& snap);

// Multi-server combined report.
std::string build_hedge_pdf(const std::vector<ServerSnapshot>& snaps);

}  // namespace hyperion
