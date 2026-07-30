#include "hedge_pdf.h"

#include <chrono>
#include <cmath>
#include <ctime>
#include <format>
#include <sstream>
#include <iomanip>

namespace hyperion {
namespace {

std::string pdf_escape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (c == '(' || c == ')' || c == '\\') {
      out.push_back('\\');
    }
    if (static_cast<unsigned char>(c) < 32 ||
        static_cast<unsigned char>(c) > 126) {
      // Drop non-latin1 controls; keep printable ASCII for simplicity.
      if (c == '\n' || c == '\r' || c == '\t') {
        out.push_back(' ');
      }
      continue;
    }
    out.push_back(c);
  }
  return out;
}

std::string iso_now() {
  const auto now = std::chrono::system_clock::now();
  const std::time_t t = std::chrono::system_clock::to_time_t(now);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buf[64];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
  return buf;
}

std::string fmt_money(double v) {
  return std::format("{:.2f}", v);
}

std::string fmt_qty(long long q) {
  return std::format("{}", q);
}

// Build a simple multi-line text PDF.
std::string lines_to_pdf(const std::vector<std::string>& lines) {
  // Content stream: begin text, set font, show lines top-down.
  std::ostringstream content;
  content << "BT\n/F1 10 Tf\n14 TL\n50 780 Td\n";
  bool first = true;
  for (const auto& line : lines) {
    if (!first) {
      content << "T*\n";
    }
    first = false;
    content << "(" << pdf_escape(line) << ") Tj\n";
  }
  content << "ET\n";
  const std::string stream = content.str();

  std::ostringstream pdf;
  // Object layout with xref.
  std::vector<std::size_t> offsets;
  auto mark = [&] {
    offsets.push_back(static_cast<std::size_t>(pdf.tellp()));
  };

  pdf << "%PDF-1.4\n";

  mark();  // 1 catalog
  pdf << "1 0 obj<< /Type /Catalog /Pages 2 0 R >>endobj\n";

  mark();  // 2 pages
  pdf << "2 0 obj<< /Type /Pages /Kids [3 0 R] /Count 1 >>endobj\n";

  mark();  // 3 page
  pdf << "3 0 obj<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] "
         "/Contents 4 0 R /Resources << /Font << /F1 5 0 R >> >> >>endobj\n";

  mark();  // 4 contents
  pdf << "4 0 obj<< /Length " << stream.size() << " >>stream\n"
      << stream << "endstream\nendobj\n";

  mark();  // 5 font
  pdf << "5 0 obj<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>endobj\n";

  const auto xref_pos = static_cast<std::size_t>(pdf.tellp());
  pdf << "xref\n0 " << (offsets.size() + 1) << "\n";
  pdf << "0000000000 65535 f \n";
  for (auto off : offsets) {
    pdf << std::setw(10) << std::setfill('0') << off << " 00000 n \n";
  }
  pdf << "trailer<< /Size " << (offsets.size() + 1)
      << " /Root 1 0 R >>\nstartxref\n"
      << xref_pos << "\n%%EOF\n";
  return pdf.str();
}

void append_server_lines(std::vector<std::string>& lines,
                         const ServerSnapshot& snap) {
  lines.push_back(std::format("=== {} ({}) ===", snap.label, snap.server_id));
  lines.push_back(std::format(
      "cash={}  buying_power={}  equity_marked={}  equity_broker={}",
      fmt_money(snap.cash), fmt_money(snap.buying_power),
      fmt_money(snap.equity_marked), fmt_money(snap.equity_reported)));
  lines.push_back(std::format("uncertain={}  known={}  http_ok={}  redis_ok={}",
                              snap.uncertain_count, snap.known_count,
                              snap.http_ok ? "yes" : "no",
                              snap.redis_ok ? "yes" : "no"));
  lines.push_back("");

  lines.push_back("-- UNCERTAIN (hedge these first) --");
  bool any_u = false;
  for (const auto& p : snap.positions) {
    if (p.certainty != Certainty::Uncertain) {
      continue;
    }
    any_u = true;
    const double px = p.last_price.value_or(0.0);
    const double notional = static_cast<double>(p.qty) * px;
    lines.push_back(std::format(
        "  {:<8} qty={:<8} intended={:<8} px={} notional={} side={} inflight={} | {}",
        p.symbol, fmt_qty(p.qty), fmt_qty(p.intended),
        p.last_price ? fmt_money(*p.last_price) : "n/a",
        fmt_money(notional), p.side.empty() ? "-" : p.side,
        fmt_qty(p.inflight_qty), p.reason));
  }
  if (!any_u) {
    lines.push_back("  (none)");
  }
  lines.push_back("");

  lines.push_back("-- KNOWN --");
  bool any_k = false;
  for (const auto& p : snap.positions) {
    if (p.certainty != Certainty::Known) {
      continue;
    }
    any_k = true;
    const double px = p.last_price.value_or(0.0);
    const double notional = static_cast<double>(p.qty) * px;
    lines.push_back(std::format(
        "  {:<8} qty={:<8} intended={:<8} px={} notional={}",
        p.symbol, fmt_qty(p.qty), fmt_qty(p.intended),
        p.last_price ? fmt_money(*p.last_price) : "n/a", fmt_money(notional)));
  }
  if (!any_k) {
    lines.push_back("  (none)");
  }
  lines.push_back("");
}

}  // namespace

std::string build_hedge_pdf(const ServerSnapshot& snap) {
  std::vector<ServerSnapshot> v{snap};
  return build_hedge_pdf(v);
}

std::string build_hedge_pdf(const std::vector<ServerSnapshot>& snaps) {
  std::vector<std::string> lines;
  lines.push_back("HYPERION HEDGE REPORT");
  lines.push_back("Alpaca ToS: be ready to hedge if primary broker fails.");
  lines.push_back("Uncertain rows first (open orders / unconfirmed intents).");
  lines.push_back("Generated: " + iso_now());
  lines.push_back(std::string(72, '='));
  lines.push_back("");

  if (snaps.empty()) {
    lines.push_back("(no servers)");
  } else {
    for (const auto& s : snaps) {
      append_server_lines(lines, s);
    }
  }

  // PDF page holds ~50 lines at 14pt leading from y=780.
  // Truncate gracefully if enormous.
  constexpr std::size_t kMaxLines = 52;
  if (lines.size() > kMaxLines) {
    lines.resize(kMaxLines - 1);
    lines.push_back("... truncated — reduce positions or export per-tab");
  }
  return lines_to_pdf(lines);
}

}  // namespace hyperion
