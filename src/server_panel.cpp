#include "server_panel.h"

#include "http_client.h"

#include <Wt/WColor.h>
#include <Wt/WLength.h>
#include <Wt/WLineEdit.h>
#include <Wt/WPainter.h>
#include <Wt/WPen.h>
#include <Wt/WPushButton.h>
#include <Wt/WTable.h>
#include <Wt/WTableCell.h>
#include <Wt/WTabWidget.h>
#include <Wt/WText.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <nlohmann/json.hpp>
#include <sstream>
#include <thread>

namespace hyperion {
namespace {

Wt::WText* plain(Wt::WContainerWidget* parent, std::string_view value,
                 std::string_view style = {}) {
  auto* w =
      parent->addNew<Wt::WText>(Wt::WString::fromUTF8(std::string(value)));
  w->setTextFormat(Wt::TextFormat::Plain);
  if (!style.empty()) {
    w->setStyleClass(std::string(style));
  }
  return w;
}

std::string money(double v) { return std::format("{:.2f}", v); }

std::string upper_sym(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  }
  // strip whitespace
  s.erase(std::remove_if(s.begin(), s.end(),
                         [](unsigned char c) { return std::isspace(c); }),
          s.end());
  return s;
}

std::string pos_sig(const ServerSnapshot& snap) {
  std::ostringstream oss;
  oss << snap.positions.size() << '|' << snap.uncertain_count << '|'
      << snap.locks.global_block << '|';
  for (const auto& p : snap.positions) {
    oss << p.symbol << ',' << p.qty << ',' << p.intended << ','
        << (p.certainty == Certainty::Uncertain ? 'U' : 'K') << ','
        << p.blocked << ',' << p.last_price.value_or(-1.0) << ',' << p.reason
        << ',' << p.inflight_qty << ',' << p.hyperion_qty << ';';
  }
  oss << snap.cash << '|' << snap.equity_marked;
  return oss.str();
}

std::string robot_sig(const ServerSnapshot& snap) {
  std::ostringstream oss;
  for (const auto& r : snap.robots) {
    oss << r.source << ',' << r.muted << ',' << r.alive << ',' << r.age_sec
        << ',' << r.positions.size() << ';';
  }
  return oss.str();
}

std::string lock_sig(const ServerSnapshot& snap) {
  std::ostringstream oss;
  oss << snap.locks.global_block << '|'
      << snap.locks.liquidator_lock_supported << '|';
  for (const auto& s : snap.locks.blocked_symbols) {
    oss << s << ',';
  }
  oss << '|';
  for (const auto& s : snap.locks.muted_sources) {
    oss << s << ',';
  }
  return oss.str();
}

}  // namespace

FullWidthChart::FullWidthChart() {
  setLayoutSizeAware(true);
  setInline(false);
  // Start wide enough that the first paint isn't a stub; layoutSizeChanged
  // will replace this with the real container width.
  resize(Wt::WLength(960, Wt::LengthUnit::Pixel),
         Wt::WLength(preferred_height_, Wt::LengthUnit::Pixel));
  setMinimumSize(Wt::WLength(200, Wt::LengthUnit::Pixel),
                 Wt::WLength(120, Wt::LengthUnit::Pixel));
  setMaximumSize(Wt::WLength::Auto, Wt::WLength::Auto);
}

void FullWidthChart::layoutSizeChanged(int width, int height) {
  if (width < 50) {
    width = 50;
  }
  // Prefer CSS height if the layout height is collapsed.
  if (height < preferred_height_) {
    height = preferred_height_;
  }
  resize(Wt::WLength(width, Wt::LengthUnit::Pixel),
         Wt::WLength(height, Wt::LengthUnit::Pixel));
  update();
}

EquityChart::EquityChart() {
  preferred_height_ = 260;
  setStyleClass("equity-chart chart-full");
  resize(Wt::WLength(960, Wt::LengthUnit::Pixel),
         Wt::WLength(preferred_height_, Wt::LengthUnit::Pixel));
}

void EquityChart::set_series(std::vector<EquityPoint> series) {
  series_ = std::move(series);
  update();
}

RateChart::RateChart() {
  preferred_height_ = 180;
  setStyleClass("rate-chart chart-full");
  resize(Wt::WLength(960, Wt::LengthUnit::Pixel),
         Wt::WLength(preferred_height_, Wt::LengthUnit::Pixel));
}

void RateChart::set_series(std::vector<RatePoint> series, int limit_qpm,
                           int current) {
  series_ = std::move(series);
  limit_qpm_ = limit_qpm > 0 ? limit_qpm : 200;
  current_ = current;
  update();
}

void RateChart::paintEvent(Wt::WPaintDevice* paintDevice) {
  Wt::WPainter painter(paintDevice);
  const double w = paintDevice->width().toPixels();
  const double h = paintDevice->height().toPixels();
  painter.setBrush(Wt::WBrush(Wt::WColor(14, 18, 24)));
  painter.setPen(Wt::WPen(Wt::WColor(50, 60, 72)));
  painter.drawRect(0, 0, w, h);

  const double left = 44;
  const double right = w - 10;
  const double top = 26;
  const double bottom = h - 14;
  const double span_x = right - left;
  const double span_y = bottom - top;
  // Prefer latest sample when current_ is stale/zero but series has data.
  int cur = current_;
  if (!series_.empty()) {
    cur = std::max(cur, series_.back().qpm);
  }
  const double max_y = std::max(
      {static_cast<double>(limit_qpm_) * 1.15, static_cast<double>(cur) * 1.25,
       50.0});
  auto y_of = [&](double qpm) {
    return bottom - span_y * (qpm / max_y);
  };

  // Zero baseline
  painter.setPen(Wt::WPen(Wt::WColor(40, 48, 58)));
  painter.drawLine(left, bottom, right, bottom);

  // Limit bar at 200 qpm
  const double y_lim = y_of(static_cast<double>(limit_qpm_));
  Wt::WPen lim(Wt::WColor(255, 80, 80));
  lim.setWidth(2);
  painter.setPen(lim);
  painter.drawLine(left, y_lim, right, y_lim);
  painter.setPen(Wt::WPen(Wt::WColor(255, 160, 160)));
  painter.drawText(left, y_lim - 14, span_x, 14,
                   Wt::AlignmentFlag::Right | Wt::AlignmentFlag::Top,
                   Wt::WString::fromUTF8(
                       std::format("{} qpm limit", limit_qpm_)));

  // Current trailing-minute rate (rQPM) — always drawn so the level is obvious
  // even when the history line sits near zero.
  const double y_cur = y_of(static_cast<double>(cur));
  Wt::WPen cur_pen(Wt::WColor(241, 196, 15));  // bright amber
  cur_pen.setWidth(3);
  painter.setPen(cur_pen);
  painter.drawLine(left, y_cur, right, y_cur);
  // Endpoint marker
  painter.setBrush(Wt::WBrush(Wt::WColor(241, 196, 15)));
  painter.drawEllipse(right - 6, y_cur - 5, 10, 10);
  painter.setPen(Wt::WPen(Wt::WColor(255, 230, 120)));
  // Place label above the bar unless it would collide with the title.
  const double cur_label_y =
      (y_cur - 16 < top) ? y_cur + 4 : y_cur - 16;
  painter.drawText(left, cur_label_y, span_x * 0.55, 16,
                   Wt::AlignmentFlag::Left | Wt::AlignmentFlag::Top,
                   Wt::WString::fromUTF8(
                       std::format("{} rQPM now", cur)));

  // History line (trailing-minute rate over time)
  if (series_.size() >= 2) {
    Wt::WPen line(Wt::WColor(126, 192, 255));
    line.setWidth(2);
    painter.setPen(line);
    const std::size_t n = series_.size();
    for (std::size_t i = 1; i < n; ++i) {
      const double x0 = left + span_x * static_cast<double>(i - 1) / (n - 1);
      const double x1 = left + span_x * static_cast<double>(i) / (n - 1);
      painter.drawLine(x0, y_of(static_cast<double>(series_[i - 1].qpm)), x1,
                       y_of(static_cast<double>(series_[i].qpm)));
    }
  } else {
    painter.setPen(Wt::WPen(Wt::WColor(140, 150, 165)));
    painter.drawText(left, bottom - 18, span_x, 14,
                     Wt::AlignmentFlag::Left | Wt::AlignmentFlag::Top,
                     "history building…");
  }

  // Title / readout
  painter.setPen(Wt::WPen(Wt::WColor(240, 245, 255)));
  painter.drawText(
      left, 2, span_x, 18, Wt::AlignmentFlag::Left | Wt::AlignmentFlag::Top,
      Wt::WString::fromUTF8(std::format(
          "Alpaca REST  {} / {} rQPM  (amber=now  red=limit  blue=history)",
          cur, limit_qpm_)));

  // Left-axis ticks: 0, current, limit
  painter.setPen(Wt::WPen(Wt::WColor(150, 160, 175)));
  painter.drawText(2, bottom - 8, left - 4, 14,
                   Wt::AlignmentFlag::Right | Wt::AlignmentFlag::Top, "0");
  if (cur > 0 && std::abs(y_cur - y_lim) > 12 && y_cur < bottom - 8) {
    painter.drawText(2, y_cur - 7, left - 4, 14,
                     Wt::AlignmentFlag::Right | Wt::AlignmentFlag::Top,
                     Wt::WString::fromUTF8(std::to_string(cur)));
  }
  painter.drawText(2, y_lim - 7, left - 4, 14,
                   Wt::AlignmentFlag::Right | Wt::AlignmentFlag::Top,
                   Wt::WString::fromUTF8(std::to_string(limit_qpm_)));
}

PriceViewer::PriceViewer() {
  preferred_height_ = 280;
  setStyleClass("price-viewer chart-full");
  resize(Wt::WLength(960, Wt::LengthUnit::Pixel),
         Wt::WLength(preferred_height_, Wt::LengthUnit::Pixel));
}

void PriceViewer::set_symbol_series(const std::string& symbol,
                                    std::vector<PricePoint> series,
                                    long long holdings) {
  mode_ = Mode::Symbol;
  symbol_ = symbol;
  price_series_ = std::move(series);
  holdings_ = holdings;
  update();
}

void PriceViewer::set_account_series(std::vector<EquityPoint> series) {
  mode_ = Mode::Account;
  symbol_.clear();
  account_series_ = std::move(series);
  update();
}

void PriceViewer::paintEvent(Wt::WPaintDevice* paintDevice) {
  Wt::WPainter painter(paintDevice);
  const double w = paintDevice->width().toPixels();
  const double h = paintDevice->height().toPixels();
  painter.setBrush(Wt::WBrush(Wt::WColor(14, 18, 24)));
  painter.setPen(Wt::WPen(Wt::WColor(50, 60, 72)));
  painter.drawRect(0, 0, w, h);

  const double left = 8;
  const double right = w - 8;
  const double top = 28;
  const double bottom = h - 10;
  const double span_x = right - left;
  const double span_y = bottom - top;

  if (mode_ == Mode::Account) {
    if (account_series_.size() < 2) {
      painter.setPen(Wt::WPen(Wt::WColor(180, 190, 200)));
      painter.drawText(0, 0, w, h,
                       Wt::AlignmentFlag::Center | Wt::AlignmentFlag::Middle,
                       "Entire account — waiting for samples…");
      return;
    }
    double min_v = account_series_.front().equity;
    double max_v = min_v;
    for (const auto& p : account_series_) {
      min_v = std::min({min_v, p.equity, p.cash});
      max_v = std::max({max_v, p.equity, p.cash});
    }
    if (std::abs(max_v - min_v) < 1e-9) {
      max_v = min_v + 1.0;
    }
    const double pad = 0.05 * (max_v - min_v);
    min_v -= pad;
    max_v += pad;
    const std::size_t n = account_series_.size();
    auto y_of = [&](double v) {
      return bottom - span_y * (v - min_v) / (max_v - min_v);
    };
    Wt::WPen eq(Wt::WColor(46, 204, 113));
    eq.setWidth(2);
    painter.setPen(eq);
    for (std::size_t i = 1; i < n; ++i) {
      const double x0 = left + span_x * static_cast<double>(i - 1) / (n - 1);
      const double x1 = left + span_x * static_cast<double>(i) / (n - 1);
      painter.drawLine(x0, y_of(account_series_[i - 1].equity), x1,
                       y_of(account_series_[i].equity));
    }
    Wt::WPen cash(Wt::WColor(126, 192, 255));
    cash.setWidth(2);
    painter.setPen(cash);
    for (std::size_t i = 1; i < n; ++i) {
      const double x0 = left + span_x * static_cast<double>(i - 1) / (n - 1);
      const double x1 = left + span_x * static_cast<double>(i) / (n - 1);
      painter.drawLine(x0, y_of(account_series_[i - 1].cash), x1,
                       y_of(account_series_[i].cash));
    }
    const auto& last = account_series_.back();
    painter.setPen(Wt::WPen(Wt::WColor(240, 245, 255)));
    painter.drawText(
        left, 2, span_x, 20, Wt::AlignmentFlag::Left | Wt::AlignmentFlag::Top,
        Wt::WString::fromUTF8(std::format(
            "Entire account   equity {:.2f}   cash {:.2f}   (green=equity blue=cash)",
            last.equity, last.cash)));
    return;
  }

  // Symbol mode
  if (price_series_.size() < 2) {
    painter.setPen(Wt::WPen(Wt::WColor(180, 190, 200)));
    painter.drawText(0, 0, w, h,
                     Wt::AlignmentFlag::Center | Wt::AlignmentFlag::Middle,
                     Wt::WString::fromUTF8(symbol_.empty()
                                               ? "Pick a symbol"
                                               : symbol_ + " — waiting for ticks…"));
    return;
  }
  double min_v = price_series_.front().price;
  double max_v = min_v;
  for (const auto& p : price_series_) {
    min_v = std::min(min_v, p.price);
    max_v = std::max(max_v, p.price);
  }
  if (std::abs(max_v - min_v) < 1e-9) {
    max_v = min_v + 1.0;
  }
  const double pad = 0.05 * (max_v - min_v);
  min_v -= pad;
  max_v += pad;
  const std::size_t n = price_series_.size();
  Wt::WPen line(Wt::WColor(241, 196, 15));
  line.setWidth(2);
  painter.setPen(line);
  for (std::size_t i = 1; i < n; ++i) {
    const double x0 = left + span_x * static_cast<double>(i - 1) / (n - 1);
    const double x1 = left + span_x * static_cast<double>(i) / (n - 1);
    const double y0 =
        bottom - span_y * (price_series_[i - 1].price - min_v) / (max_v - min_v);
    const double y1 =
        bottom - span_y * (price_series_[i].price - min_v) / (max_v - min_v);
    painter.drawLine(x0, y0, x1, y1);
  }
  const auto& last = price_series_.back();
  painter.setPen(Wt::WPen(Wt::WColor(240, 245, 255)));
  painter.drawText(
      left, 2, span_x, 20, Wt::AlignmentFlag::Left | Wt::AlignmentFlag::Top,
      Wt::WString::fromUTF8(std::format(
          "{}   last {:.4f}   holdings {}   events {}", symbol_, last.price,
          holdings_, n)));
}

void EquityChart::paintEvent(Wt::WPaintDevice* paintDevice) {
  Wt::WPainter painter(paintDevice);
  const double w = paintDevice->width().toPixels();
  const double h = paintDevice->height().toPixels();

  painter.setBrush(Wt::WBrush(Wt::WColor(14, 18, 24)));
  painter.setPen(Wt::WPen(Wt::WColor(50, 60, 72)));
  painter.drawRect(0, 0, w, h);

  if (series_.size() < 2) {
    painter.setPen(Wt::WPen(Wt::WColor(180, 190, 200)));
    painter.drawText(0, 0, w, h,
                     Wt::AlignmentFlag::Center | Wt::AlignmentFlag::Middle,
                     "Waiting for equity samples…");
    return;
  }

  // Dual series: equity (green) + free cash (blue). A stock buy moves cash
  // while marked equity stays ~flat — cash is what makes fills visible live.
  double min_v = series_.front().equity;
  double max_v = min_v;
  for (const auto& p : series_) {
    min_v = std::min({min_v, p.equity, p.cash});
    max_v = std::max({max_v, p.equity, p.cash});
  }
  if (std::abs(max_v - min_v) < 1e-9) {
    max_v = min_v + 1.0;
  }
  const double pad = 0.05 * (max_v - min_v);
  min_v -= pad;
  max_v += pad;

  const double left = 8;
  const double right = w - 8;
  const double top = 28;
  const double bottom = h - 10;
  const double span_x = right - left;
  const double span_y = bottom - top;
  auto y_of = [&](double v) {
    return bottom - span_y * (v - min_v) / (max_v - min_v);
  };

  painter.setPen(Wt::WPen(Wt::WColor(55, 65, 78)));
  painter.drawLine(left, top + span_y * 0.5, right, top + span_y * 0.5);

  const std::size_t n = series_.size();
  Wt::WPen eq(Wt::WColor(46, 204, 113));
  eq.setWidth(2);
  painter.setPen(eq);
  for (std::size_t i = 1; i < n; ++i) {
    const double x0 = left + span_x * static_cast<double>(i - 1) / (n - 1);
    const double x1 = left + span_x * static_cast<double>(i) / (n - 1);
    painter.drawLine(x0, y_of(series_[i - 1].equity), x1,
                     y_of(series_[i].equity));
  }
  Wt::WPen cash(Wt::WColor(126, 192, 255));
  cash.setWidth(2);
  painter.setPen(cash);
  for (std::size_t i = 1; i < n; ++i) {
    const double x0 = left + span_x * static_cast<double>(i - 1) / (n - 1);
    const double x1 = left + span_x * static_cast<double>(i) / (n - 1);
    painter.drawLine(x0, y_of(series_[i - 1].cash), x1, y_of(series_[i].cash));
  }

  const auto& last = series_.back();
  painter.setPen(Wt::WPen(Wt::WColor(230, 236, 245)));
  painter.drawText(
      left, 2, span_x, 22, Wt::AlignmentFlag::Left | Wt::AlignmentFlag::Top,
      Wt::WString::fromUTF8(std::format(
          "Equity {}   cash {}   (green=equity  blue=cash)", money(last.equity),
          money(last.cash))));
}

ServerPanel::ServerPanel(ServerConfig cfg, SessionFeed* feed)
    : cfg_(std::move(cfg)), feed_(feed) {
  setStyleClass("server-panel");
  build();
}

void ServerPanel::set_pdf_handler(std::function<void()> handler) {
  pdf_provider_ = std::move(handler);
}

void ServerPanel::post_action(const std::string& method, const std::string& path,
                              const std::string& body) {
  const std::string base = latest_.http_base.empty() ? cfg_.http_base
                                                     : latest_.http_base;
  const std::string url = base + path;
  action_note_->setText(
      Wt::WString::fromUTF8(std::format("Sending {} {} …", method, path)));
  action_note_->setStyleClass("action-note");

  std::thread([method, url, body] {
    if (method == "POST") {
      http_post(url, body, 5000);
    } else if (method == "DELETE") {
      http_delete(url, 5000);
    } else {
      http_get(url, 5000);
    }
  }).detach();
}

void ServerPanel::build() {
  auto* head = addNew<Wt::WContainerWidget>();
  head->setStyleClass("panel-head");

  auto* titles = head->addNew<Wt::WContainerWidget>();
  titles->setStyleClass("panel-titles");
  title_ = plain(titles, cfg_.label, "panel-title");
  status_ = plain(titles, "starting…", "panel-status");

  auto* metrics = head->addNew<Wt::WContainerWidget>();
  metrics->setStyleClass("panel-metrics");
  cash_ = plain(metrics, "Cash —", "metric cash-metric");
  equity_ = plain(metrics, "Equity —", "metric");
  alpaca_rate_ = plain(metrics, "Alpaca 0/200 qpm", "metric alpaca-metric");

  auto* actions = head->addNew<Wt::WContainerWidget>();
  actions->setStyleClass("panel-actions");
  pdf_btn_ = actions->addNew<Wt::WPushButton>("Hedge PDF");
  pdf_btn_->setStyleClass("hedge-btn");
  pdf_btn_->clicked().connect([this] {
    if (pdf_provider_) {
      pdf_provider_();
    }
  });
  auto* block_all = actions->addNew<Wt::WPushButton>("Block ALL trading");
  block_all->setStyleClass("warn-btn");
  block_all->clicked().connect([this] {
    post_action("POST", "/v1/block/all");
    action_note_->setText("Global block — freezes trading, no liquidation");
  });
  auto* liq_all = actions->addNew<Wt::WPushButton>("Liquidate ALL + block");
  liq_all->setStyleClass("danger-btn");
  liq_all->clicked().connect([this] {
    post_action("POST", "/v1/liquidate/all");
    action_note_->setText("Alpaca liquidate all + global block");
  });
  auto* unlock = actions->addNew<Wt::WPushButton>("Unblock all");
  unlock->setStyleClass("ok-btn");
  unlock->clicked().connect([this] {
    post_action("POST", "/v1/unblock/all");
    action_note_->setText("Unblocked global + symbol freezes");
  });

  action_note_ = plain(this, "", "action-note");

  screens_ = addNew<Wt::WTabWidget>();
  screens_->setStyleClass("screen-tabs");

  // --- Positions + live trade (same screen so fills are visible immediately) ---
  auto pos = std::make_unique<Wt::WContainerWidget>();
  pos->setStyleClass("screen positions-screen");

  trade_bar_ = pos->addNew<Wt::WContainerWidget>();
  trade_bar_->setStyleClass("trade-bar");
  plain(trade_bar_, "Live trade · robot hyperion", "section-label");
  plain(trade_bar_,
        "Set the position hyperion requests. Order → fill → qty/target update "
        "on this table in realtime (STATUS stream).",
        "section-hint");
  trade_live_hint_ =
      plain(trade_bar_, "Waiting for stream…", "trade-live-hint");

  trade_phase1_ = trade_bar_->addNew<Wt::WContainerWidget>();
  trade_phase1_->setStyleClass("trade-phase trade-phase-inline");
  auto* p1row = trade_phase1_->addNew<Wt::WContainerWidget>();
  p1row->setStyleClass("trade-inline-row");
  plain(p1row, "Symbol", "trade-field-label");
  trade_symbol_ = p1row->addNew<Wt::WLineEdit>();
  trade_symbol_->setPlaceholderText("IBM");
  trade_symbol_->setStyleClass("trade-input trade-input-sym");
  trade_symbol_->enterPressed().connect(this, &ServerPanel::trade_go_symbol);
  auto* next = p1row->addNew<Wt::WPushButton>("Size →");
  next->setStyleClass("ok-btn");
  next->clicked().connect(this, &ServerPanel::trade_go_symbol);

  trade_phase2_ = trade_bar_->addNew<Wt::WContainerWidget>();
  trade_phase2_->setStyleClass("trade-phase trade-phase-inline");
  trade_phase2_->setHidden(true);
  trade_current_ =
      plain(trade_phase2_, "Current hyperion demand: —", "trade-meta");
  trade_quote_ =
      plain(trade_phase2_, "Bid / Ask / Last: —", "trade-meta quote-live");
  auto* p2row = trade_phase2_->addNew<Wt::WContainerWidget>();
  p2row->setStyleClass("trade-inline-row");
  plain(p2row, "Position", "trade-field-label");
  trade_qty_ = p2row->addNew<Wt::WLineEdit>();
  trade_qty_->setPlaceholderText("50 long · -20 short · 0 flat");
  trade_qty_->setStyleClass("trade-input trade-input-qty");
  trade_qty_->changed().connect(this, &ServerPanel::update_trade_quotes);
  trade_qty_->keyWentUp().connect(this, &ServerPanel::update_trade_quotes);
  trade_qty_->enterPressed().connect(this, &ServerPanel::trade_submit);
  auto* back = p2row->addNew<Wt::WPushButton>("←");
  back->setStyleClass("hedge-btn");
  back->clicked().connect(this, &ServerPanel::trade_back);
  auto* submit = p2row->addNew<Wt::WPushButton>("Set position");
  submit->setStyleClass("ok-btn");
  submit->clicked().connect(this, &ServerPanel::trade_submit);
  trade_impact_ = plain(trade_phase2_, "Cash impact: —", "trade-impact");

  plain(pos.get(), "Positions", "section-label");
  plain(pos.get(),
        "Block freezes trading (keeps the position). Liquidate uses Alpaca "
        "close-position and also blocks. Uncertain / in-flight rows sort high.",
        "section-hint");
  pos_table_ = pos->addNew<Wt::WTable>();
  pos_table_->setStyleClass("pos-table");
  pos_table_->setHeaderCount(1);
  pos_table_->elementAt(0, 0)->addNew<Wt::WText>("Sym");
  pos_table_->elementAt(0, 1)->addNew<Wt::WText>("Qty");
  pos_table_->elementAt(0, 2)->addNew<Wt::WText>("Target");
  pos_table_->elementAt(0, 3)->addNew<Wt::WText>("Price");
  pos_table_->elementAt(0, 4)->addNew<Wt::WText>("Notional");
  pos_table_->elementAt(0, 5)->addNew<Wt::WText>("State");
  pos_table_->elementAt(0, 6)->addNew<Wt::WText>("Actions");
  plain(pos.get(), "Account (live equity + cash)", "section-label chart-label");
  plain(pos.get(),
        "Fills move cash immediately; marked equity stays ~flat when you swap "
        "cash for stock. Same series as Charts → Entire account.",
        "section-hint");
  chart_ = pos->addNew<EquityChart>();
  screens_->addTab(std::move(pos), "Positions");

  // --- Charts: Alpaca rate + price viewer ---
  auto charts = std::make_unique<Wt::WContainerWidget>();
  charts->setStyleClass("screen charts-screen");
  plain(charts.get(), "Alpaca REST rate (trailing 60s)", "section-label");
  plain(charts.get(),
        "Every jinghong Alpaca call is published on ACCOUNT:ALPACA:<server> "
        "(verb + uuid) and written to the alpaca JSONL log. Limit is 200/min.",
        "section-hint");
  rate_chart_ = charts->addNew<RateChart>();

  plain(charts.get(), "Price / account viewer", "section-label chart-label");
  plain(charts.get(),
        "Ticks are equispaced by event. Switch symbols or entire account "
        "(equity + free cash).",
        "section-hint");
  chart_buttons_ = charts->addNew<Wt::WContainerWidget>();
  chart_buttons_->setStyleClass("chart-selector");
  chart_mode_label_ = plain(charts.get(), "Mode: entire account", "trade-meta");
  price_viewer_ = charts->addNew<PriceViewer>();
  screens_->addTab(std::move(charts), "Charts");

  // --- Robots ---
  auto robots = std::make_unique<Wt::WContainerWidget>();
  robots->setStyleClass("screen robots-screen");
  plain(robots.get(), "Robots (sources)", "section-label");
  plain(robots.get(),
        "Robots appear from heartbeats/intents on ACCOUNT:ORDER (also STATUS "
        "type=heartbeat). Bots should beat every ~5s; silent >15s → not alive "
        "and jinghong zeros their book. Mute drops demand to zero.",
        "section-hint");
  robot_table_ = robots->addNew<Wt::WTable>();
  robot_table_->setStyleClass("pos-table");
  robot_table_->setHeaderCount(1);
  robot_table_->elementAt(0, 0)->addNew<Wt::WText>("Source");
  robot_table_->elementAt(0, 1)->addNew<Wt::WText>("Alive");
  robot_table_->elementAt(0, 2)->addNew<Wt::WText>("Age s");
  robot_table_->elementAt(0, 3)->addNew<Wt::WText>("Muted");
  robot_table_->elementAt(0, 4)->addNew<Wt::WText>("Demand");
  robot_table_->elementAt(0, 5)->addNew<Wt::WText>("Action");
  screens_->addTab(std::move(robots), "Robots");

  // --- Locks ---
  auto locks = std::make_unique<Wt::WContainerWidget>();
  locks->setStyleClass("screen locks-screen");
  plain(locks.get(), "Blocks & liquidations", "section-label");
  plain(locks.get(),
        "Block = freeze trading only. Liquidate = Alpaca close-position + block. "
        "Mute is per-robot on the Robots screen.",
        "section-hint");
  lock_summary_ = plain(locks.get(), "—", "lock-summary");
  auto* lock_btns = locks->addNew<Wt::WContainerWidget>();
  lock_btns->setStyleClass("lock-btns");
  auto* b1 = lock_btns->addNew<Wt::WPushButton>("Block ALL (no liquidate)");
  b1->setStyleClass("warn-btn");
  b1->clicked().connect([this] { post_action("POST", "/v1/block/all"); });
  auto* b2 = lock_btns->addNew<Wt::WPushButton>("Liquidate ALL + block");
  b2->setStyleClass("danger-btn");
  b2->clicked().connect([this] { post_action("POST", "/v1/liquidate/all"); });
  auto* b3 = lock_btns->addNew<Wt::WPushButton>("Unblock all (keep mutes)");
  b3->setStyleClass("ok-btn");
  b3->clicked().connect([this] { post_action("POST", "/v1/unblock/all"); });
  auto* b4 = lock_btns->addNew<Wt::WPushButton>("Clear blocks + mutes");
  b4->setStyleClass("ok-btn");
  b4->clicked().connect(
      [this] { post_action("POST", "/v1/unlock/all_locks"); });
  plain(locks.get(), "Blocked symbols", "section-label");
  blocked_table_ = locks->addNew<Wt::WTable>();
  blocked_table_->setStyleClass("pos-table");
  blocked_table_->setHeaderCount(1);
  blocked_table_->elementAt(0, 0)->addNew<Wt::WText>("Symbol");
  blocked_table_->elementAt(0, 1)->addNew<Wt::WText>("Action");
  screens_->addTab(std::move(locks), "Locks");

}

void ServerPanel::trade_focus_symbol(const std::string& symbol) {
  const std::string sym = upper_sym(symbol);
  if (sym.empty() || !trade_symbol_) {
    return;
  }
  trade_symbol_->setText(sym);
  trade_go_symbol();
}

void ServerPanel::trade_go_symbol() {
  const std::string sym = upper_sym(trade_symbol_->text().toUTF8());
  if (sym.empty()) {
    action_note_->setText("Enter a symbol first");
    return;
  }
  trade_active_symbol_ = sym;
  if (feed_) {
    feed_->set_watch_extra(cfg_.id, {sym});
  }
  trade_phase1_->setHidden(true);
  trade_phase2_->setHidden(false);
  const long long cur =
      feed_ ? feed_->hyperion_position(cfg_.id, sym) : 0;
  trade_current_->setText(Wt::WString::fromUTF8(
      std::format("Symbol {} · current hyperion demand: {}", sym, cur)));
  trade_qty_->setText(std::to_string(cur));
  if (trade_live_hint_) {
    trade_live_hint_->setText(Wt::WString::fromUTF8(
        std::format("Sizing {} — quotes stream live; table updates on fill",
                    sym)));
    trade_live_hint_->setStyleClass("trade-live-hint is-active");
  }
  update_trade_quotes();
}

void ServerPanel::trade_back() {
  trade_phase2_->setHidden(true);
  trade_phase1_->setHidden(false);
  trade_active_symbol_.clear();
  if (feed_) {
    feed_->set_watch_extra(cfg_.id, {});
  }
  if (trade_live_hint_) {
    trade_live_hint_->setText(
        "Pick a symbol — fills land in the table below instantly");
    trade_live_hint_->setStyleClass("trade-live-hint");
  }
}

void ServerPanel::trade_submit() {
  if (trade_active_symbol_.empty()) {
    return;
  }
  long long pos = 0;
  try {
    pos = std::stoll(trade_qty_->text().toUTF8());
  } catch (...) {
    action_note_->setText("Invalid position — use an integer share count");
    return;
  }
  nlohmann::json body = {{"source", "hyperion"},
                         {"symbol", trade_active_symbol_},
                         {"position", pos}};
  post_action("POST", "/v1/intent", body.dump());
  action_note_->setText(Wt::WString::fromUTF8(std::format(
      "hyperion → {} {} · watch Qty/Target update live below",
      trade_active_symbol_, pos)));
  if (trade_live_hint_) {
    trade_live_hint_->setText(Wt::WString::fromUTF8(std::format(
        "Submitted {} → {} — waiting for fill on positions stream…",
        trade_active_symbol_, pos)));
    trade_live_hint_->setStyleClass("trade-live-hint is-live");
  }
}

void ServerPanel::update_trade_quotes() {
  if (trade_active_symbol_.empty() || !feed_) {
    return;
  }
  const auto q = feed_->quote(cfg_.id, trade_active_symbol_);
  const long long cur = feed_->hyperion_position(cfg_.id, trade_active_symbol_);

  std::string bid_s = q.bid ? money(*q.bid) : "—";
  std::string ask_s = q.ask ? money(*q.ask) : "—";
  std::string last_s = q.last ? money(*q.last) : "—";
  trade_quote_->setText(Wt::WString::fromUTF8(
      std::format("Bid {}   Ask {}   Last {}", bid_s, ask_s, last_s)));

  long long want = cur;
  try {
    want = std::stoll(trade_qty_->text().toUTF8());
  } catch (...) {
    trade_impact_->setText("Cash impact: enter an integer position");
    return;
  }
  const long long delta = want - cur;
  double px = 0.0;
  const char* side = "flat";
  if (delta > 0) {
    // Buying more → pay ask
    px = q.ask.value_or(q.last.value_or(q.bid.value_or(0.0)));
    side = "buy";
  } else if (delta < 0) {
    // Selling → receive bid
    px = q.bid.value_or(q.last.value_or(q.ask.value_or(0.0)));
    side = "sell";
  }
  const double notional = static_cast<double>(std::llabs(delta)) * px;
  const double cash_after =
      delta > 0 ? latest_.cash - notional : latest_.cash + notional;

  if (delta == 0) {
    trade_impact_->setText(Wt::WString::fromUTF8(
        std::format("No change · cash stays {}", money(latest_.cash))));
  } else if (px <= 0.0) {
    trade_impact_->setText(Wt::WString::fromUTF8(std::format(
        "Δ {} shares ({}) · waiting for quote to estimate cash impact "
        "(cash now {})",
        delta, side, money(latest_.cash))));
  } else {
    trade_impact_->setText(Wt::WString::fromUTF8(std::format(
        "Δ {:+} shares ≈ {} @ {} → cash {} → ~{}", delta, side, money(px),
        money(latest_.cash), money(cash_after))));
  }

  trade_current_->setText(Wt::WString::fromUTF8(std::format(
      "Symbol {} · current hyperion demand: {} · new: {}", trade_active_symbol_,
      cur, want)));
}

void ServerPanel::apply(const ServerSnapshot& snap) {
  latest_ = snap;

  const std::string mode = snap.paper ? "paper" : "LIVE";
  // Prefer the redis note when down (e.g. "UDS connect failed", "reconnecting").
  const std::string redis_part =
      snap.redis_ok ? "redis up"
                    : (snap.note.empty() ? "redis down"
                                         : ("redis down (" + snap.note + ")"));
  const std::string http_part =
      snap.http_ok ? "http up"
                   : (snap.bootstrapped ? "http stale" : "http down");
  const std::string st = std::format(
      "{} · {} · {}{}", mode, redis_part, http_part,
      snap.locks.global_block ? " · GLOBAL BLOCK" : "");
  status_->setText(Wt::WString::fromUTF8(st));
  status_->setStyleClass(snap.locks.global_block
                             ? "panel-status is-lock"
                             : (snap.redis_ok && snap.http_ok
                                    ? "panel-status is-ok"
                                    : "panel-status is-warn"));

  if (snap.cash_discrepancy_alert) {
    cash_->setText(Wt::WString::fromUTF8(std::format(
        "Cash  {}  broker {}  Δ {:+.2f}  MISMATCH", money(snap.cash),
        money(snap.cash_broker), snap.cash_discrepancy)));
    cash_->setStyleClass("metric cash-metric is-mismatch");
  } else {
    cash_->setText(Wt::WString::fromUTF8(
        std::format("Cash  {}  (broker {})", money(snap.cash),
                    money(snap.cash_broker))));
    cash_->setStyleClass("metric cash-metric");
  }
  equity_->setText(Wt::WString::fromUTF8(
      std::format("Equity  {}   (broker {})", money(snap.equity_marked),
                  money(snap.equity_reported))));
  alpaca_rate_->setText(Wt::WString::fromUTF8(std::format(
      "Alpaca  {}/{} qpm", snap.alpaca_qpm, snap.alpaca_limit_qpm)));
  alpaca_rate_->setStyleClass(
      snap.alpaca_qpm >= snap.alpaca_limit_qpm * 0.85
          ? "metric alpaca-metric is-hot"
          : "metric alpaca-metric");

  if (rate_chart_) {
    rate_chart_->set_series(snap.alpaca_qpm_series, snap.alpaca_limit_qpm,
                            snap.alpaca_qpm);
  }

  std::string sym_sig;
  for (const auto& s : snap.chart_symbols) {
    sym_sig += s;
    sym_sig += ',';
  }
  if (sym_sig != last_chart_syms_) {
    last_chart_syms_ = sym_sig;
    rebuild_chart_selector(snap);
  }
  update_price_viewer();

  const auto ps = pos_sig(snap);
  if (ps != last_pos_sig_) {
    last_pos_sig_ = ps;
    rebuild_positions(snap);
  }
  const auto rs = robot_sig(snap);
  if (rs != last_robot_sig_) {
    last_robot_sig_ = rs;
    rebuild_robots(snap);
  }
  const auto ls = lock_sig(snap);
  if (ls != last_lock_sig_) {
    last_lock_sig_ = ls;
    rebuild_locks(snap);
  }

  chart_->set_series(snap.equity_series);

  if (!trade_active_symbol_.empty()) {
    update_trade_quotes();
    // Reflect live book state next to the trade bar after fills.
    if (trade_live_hint_) {
      long long qty = 0;
      long long intended = 0;
      long long inflight = 0;
      bool found = false;
      for (const auto& p : snap.positions) {
        if (p.symbol == trade_active_symbol_) {
          qty = p.qty;
          intended = p.intended;
          inflight = p.inflight_qty;
          found = true;
          break;
        }
      }
      if (found) {
        if (inflight > 0) {
          trade_live_hint_->setText(Wt::WString::fromUTF8(std::format(
              "{} · qty {} · target {} · IN FLIGHT {} — updating…",
              trade_active_symbol_, qty, intended, inflight)));
          trade_live_hint_->setStyleClass("trade-live-hint is-live");
        } else if (qty == intended) {
          trade_live_hint_->setText(Wt::WString::fromUTF8(std::format(
              "{} · qty {} matches target — live book in sync",
              trade_active_symbol_, qty)));
          trade_live_hint_->setStyleClass("trade-live-hint is-ok");
        } else {
          trade_live_hint_->setText(Wt::WString::fromUTF8(std::format(
              "{} · qty {} · target {} — reconciling…", trade_active_symbol_,
              qty, intended)));
          trade_live_hint_->setStyleClass("trade-live-hint is-active");
        }
      }
    }
  }
}

void ServerPanel::rebuild_chart_selector(const ServerSnapshot& snap) {
  if (!chart_buttons_) {
    return;
  }
  chart_buttons_->clear();
  auto* acct = chart_buttons_->addNew<Wt::WPushButton>("Entire account");
  acct->setStyleClass(chart_selection_ == "ACCOUNT" ? "ok-btn small"
                                                    : "hedge-btn small");
  acct->clicked().connect([this] {
    chart_selection_ = "ACCOUNT";
    update_price_viewer();
    rebuild_chart_selector(latest_);
  });
  for (const auto& sym : snap.chart_symbols) {
    auto* b = chart_buttons_->addNew<Wt::WPushButton>(sym);
    b->setStyleClass(chart_selection_ == sym ? "ok-btn small"
                                             : "hedge-btn small");
    b->clicked().connect([this, sym] {
      chart_selection_ = sym;
      if (feed_) {
        feed_->set_watch_extra(cfg_.id, {sym});
      }
      update_price_viewer();
      rebuild_chart_selector(latest_);
    });
  }
}

void ServerPanel::update_price_viewer() {
  if (!price_viewer_) {
    return;
  }
  if (chart_selection_ == "ACCOUNT" || chart_selection_.empty()) {
    chart_mode_label_->setText("Mode: entire account (equity + cash)");
    price_viewer_->set_account_series(latest_.equity_series);
    return;
  }
  chart_mode_label_->setText(
      Wt::WString::fromUTF8("Mode: " + chart_selection_));
  long long hold = 0;
  for (const auto& p : latest_.positions) {
    if (p.symbol == chart_selection_) {
      hold = p.qty;
      break;
    }
  }
  auto it = latest_.price_series.find(chart_selection_);
  if (it != latest_.price_series.end()) {
    price_viewer_->set_symbol_series(chart_selection_, it->second, hold);
  } else {
    price_viewer_->set_symbol_series(chart_selection_, {}, hold);
  }
}

void ServerPanel::rebuild_positions(const ServerSnapshot& snap) {
  while (pos_table_->rowCount() > 1) {
    pos_table_->removeRow(pos_table_->rowCount() - 1);
  }

  int row = 1;
  for (const auto& p : snap.positions) {
    const bool unc = p.certainty == Certainty::Uncertain;
    auto set_cell = [&](int col, const std::string& text,
                        const char* extra = nullptr) {
      auto* cell = pos_table_->elementAt(row, col);
      cell->clear();
      auto* t = cell->addNew<Wt::WText>(Wt::WString::fromUTF8(text));
      t->setTextFormat(Wt::TextFormat::Plain);
      if (extra) {
        t->setStyleClass(extra);
      }
    };

    std::string row_class = "row-known";
    if (p.blocked) {
      row_class = "row-blocked";
    } else if (unc) {
      row_class = "row-uncertain";
    }
    pos_table_->rowAt(row)->setStyleClass(row_class);

    set_cell(0, p.symbol, "sym");
    set_cell(1, std::format("{}", p.qty));
    set_cell(2, std::format("{}", p.intended));
    set_cell(3, p.last_price ? money(*p.last_price) : "—");
    const double notional =
        static_cast<double>(p.qty) * p.last_price.value_or(0.0);
    set_cell(4, p.last_price ? money(notional) : "—");

    std::string state;
    if (p.blocked) {
      state = "BLOCKED";
    } else if (p.inflight_qty > 0) {
      state = std::format("IN FLIGHT {}", p.inflight_qty);
    } else if (unc) {
      state = "UNCERTAIN";
    } else {
      state = "known";
    }
    if (!p.reason.empty()) {
      state += " — " + p.reason;
    }
    set_cell(5, state,
             p.blocked ? "state-block" : (unc ? "state-unc" : "state-ok"));

    auto* act = pos_table_->elementAt(row, 6);
    act->clear();
    auto* box = act->addNew<Wt::WContainerWidget>();
    box->setStyleClass("row-actions");

    const std::string sym = p.symbol;
    auto* trade = box->addNew<Wt::WPushButton>("Trade");
    trade->setStyleClass("ok-btn small");
    trade->clicked().connect([this, sym] { trade_focus_symbol(sym); });

    if (!p.blocked) {
      auto* blk = box->addNew<Wt::WPushButton>("Block");
      blk->setStyleClass("warn-btn small");
      blk->clicked().connect([this, sym] {
        post_action("POST", "/v1/block/symbol",
                    nlohmann::json{{"symbol", sym}}.dump());
      });
      auto* liq = box->addNew<Wt::WPushButton>("Liquidate");
      liq->setStyleClass("danger-btn small");
      liq->clicked().connect([this, sym] {
        post_action("POST", "/v1/liquidate/symbol",
                    nlohmann::json{{"symbol", sym}}.dump());
      });
    } else if (!snap.locks.global_block) {
      auto* un = box->addNew<Wt::WPushButton>("Unblock");
      un->setStyleClass("ok-btn small");
      un->clicked().connect([this, sym] {
        post_action("POST", "/v1/unblock/symbol",
                    nlohmann::json{{"symbol", sym}}.dump());
      });
      if (p.qty != 0) {
        auto* liq = box->addNew<Wt::WPushButton>("Liquidate");
        liq->setStyleClass("danger-btn small");
        liq->clicked().connect([this, sym] {
          post_action("POST", "/v1/liquidate/symbol",
                      nlohmann::json{{"symbol", sym}}.dump());
        });
      }
    } else {
      auto* t = box->addNew<Wt::WText>("(global block)");
      t->setTextFormat(Wt::TextFormat::Plain);
      t->setStyleClass("state-ok");
    }
    ++row;
  }

  if (snap.positions.empty()) {
    auto* cell = pos_table_->elementAt(1, 0);
    cell->setColumnSpan(7);
    cell->clear();
    auto* t = cell->addNew<Wt::WText>(
        "No positions yet — use Live trade above; fills appear here instantly");
    t->setTextFormat(Wt::TextFormat::Plain);
    t->setStyleClass("empty");
  }
}

void ServerPanel::rebuild_robots(const ServerSnapshot& snap) {
  while (robot_table_->rowCount() > 1) {
    robot_table_->removeRow(robot_table_->rowCount() - 1);
  }

  int row = 1;
  for (const auto& r : snap.robots) {
    robot_table_->rowAt(row)->setStyleClass(r.muted ? "row-blocked"
                                                    : "row-known");
    auto set_cell = [&](int col, const std::string& text,
                        const char* extra = nullptr) {
      auto* cell = robot_table_->elementAt(row, col);
      cell->clear();
      auto* t = cell->addNew<Wt::WText>(Wt::WString::fromUTF8(text));
      t->setTextFormat(Wt::TextFormat::Plain);
      if (extra) {
        t->setStyleClass(extra);
      }
    };
    set_cell(0, r.source, "sym");
    set_cell(1, r.alive ? "yes" : "no", r.alive ? "state-ok" : "state-unc");
    set_cell(2, r.age_sec < 0 ? "—" : std::format("{:.0f}", r.age_sec));
    set_cell(3, r.muted ? "MUTED" : "active",
             r.muted ? "state-block" : "state-ok");

    std::string demand;
    for (const auto& [sym, qty] : r.positions) {
      if (!demand.empty()) {
        demand += "  ";
      }
      demand += std::format("{}:{}", sym, qty);
    }
    if (demand.empty()) {
      demand = "—";
    }
    set_cell(4, demand);

    auto* act = robot_table_->elementAt(row, 5);
    act->clear();
    if (r.muted) {
      auto* btn = act->addNew<Wt::WPushButton>("Unmute");
      btn->setStyleClass("ok-btn small");
      const std::string src = r.source;
      btn->clicked().connect([this, src] {
        post_action("POST", "/v1/unmute/source",
                    nlohmann::json{{"source", src}}.dump());
      });
    } else {
      auto* btn = act->addNew<Wt::WPushButton>("Mute (demand 0)");
      btn->setStyleClass("danger-btn small");
      const std::string src = r.source;
      btn->clicked().connect([this, src] {
        post_action("POST", "/v1/mute/source",
                    nlohmann::json{{"source", src}}.dump());
      });
    }
    ++row;
  }

  if (snap.robots.empty()) {
    auto* cell = robot_table_->elementAt(1, 0);
    cell->setColumnSpan(6);
    cell->clear();
    auto* t = cell->addNew<Wt::WText>(
        "No robots seen yet — use Live trade on Positions or wait for ORDER traffic");
    t->setTextFormat(Wt::TextFormat::Plain);
    t->setStyleClass("empty");
  }
}

void ServerPanel::rebuild_locks(const ServerSnapshot& snap) {
  if (!snap.locks.liquidator_lock_supported && snap.http_ok) {
    lock_summary_->setText(
        "jinghong on this port does not advertise liquidator_lock yet");
    lock_summary_->setStyleClass("lock-summary is-warn");
  } else if (snap.locks.global_block) {
    lock_summary_->setText(Wt::WString::fromUTF8(std::format(
        "GLOBAL BLOCK — no new trades.  symbol blocks: {}  muted robots: {}",
        snap.locks.blocked_symbols.size(), snap.locks.muted_sources.size())));
    lock_summary_->setStyleClass("lock-summary is-lock");
  } else {
    lock_summary_->setText(Wt::WString::fromUTF8(std::format(
        "Trading open.  symbol blocks: {}  muted robots: {}",
        snap.locks.blocked_symbols.size(), snap.locks.muted_sources.size())));
    lock_summary_->setStyleClass("lock-summary");
  }

  while (blocked_table_->rowCount() > 1) {
    blocked_table_->removeRow(blocked_table_->rowCount() - 1);
  }
  int row = 1;
  for (const auto& sym : snap.locks.blocked_symbols) {
    auto* c0 = blocked_table_->elementAt(row, 0);
    c0->clear();
    auto* t = c0->addNew<Wt::WText>(Wt::WString::fromUTF8(sym));
    t->setTextFormat(Wt::TextFormat::Plain);
    t->setStyleClass("sym");

    auto* c1 = blocked_table_->elementAt(row, 1);
    c1->clear();
    auto* un = c1->addNew<Wt::WPushButton>("Unblock");
    un->setStyleClass("ok-btn small");
    un->clicked().connect([this, sym] {
      post_action("POST", "/v1/unblock/symbol",
                  nlohmann::json{{"symbol", sym}}.dump());
    });
    auto* liq = c1->addNew<Wt::WPushButton>("Liquidate");
    liq->setStyleClass("danger-btn small");
    liq->clicked().connect([this, sym] {
      post_action("POST", "/v1/liquidate/symbol",
                  nlohmann::json{{"symbol", sym}}.dump());
    });
    ++row;
  }
  if (snap.locks.blocked_symbols.empty()) {
    auto* cell = blocked_table_->elementAt(1, 0);
    cell->setColumnSpan(2);
    cell->clear();
    auto* t = cell->addNew<Wt::WText>("No per-symbol blocks");
    t->setTextFormat(Wt::TextFormat::Plain);
    t->setStyleClass("empty");
  }
}

}  // namespace hyperion
