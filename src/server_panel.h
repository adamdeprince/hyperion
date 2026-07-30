#pragma once

#include "book_state.h"
#include "session_feed.h"

#include <functional>
#include <string>
#include <vector>

#include <Wt/WContainerWidget.h>
#include <Wt/WPaintedWidget.h>

namespace Wt {
class WText;
class WPushButton;
class WTable;
class WTabWidget;
class WLineEdit;
class WSpinBox;
}

namespace hyperion {

// WPaintedWidget percentage widths bake a tiny canvas unless we follow layout.
class FullWidthChart : public Wt::WPaintedWidget {
 public:
  FullWidthChart();

 protected:
  void layoutSizeChanged(int width, int height) override;
  int preferred_height_ = 220;
};

class EquityChart : public FullWidthChart {
 public:
  EquityChart();
  void set_series(std::vector<EquityPoint> series);

 protected:
  void paintEvent(Wt::WPaintDevice* paintDevice) override;

 private:
  std::vector<EquityPoint> series_;
};

// Alpaca REST requests per trailing minute (limit bar at 200).
class RateChart : public FullWidthChart {
 public:
  RateChart();
  void set_series(std::vector<RatePoint> series, int limit_qpm, int current);

 protected:
  void paintEvent(Wt::WPaintDevice* paintDevice) override;

 private:
  std::vector<RatePoint> series_;
  int limit_qpm_ = 200;
  int current_ = 0;
};

// Symbol price (equispaced events) or entire-account (equity + cash).
class PriceViewer : public FullWidthChart {
 public:
  enum class Mode { Symbol, Account };
  PriceViewer();
  void set_symbol_series(const std::string& symbol,
                         std::vector<PricePoint> series, long long holdings);
  void set_account_series(std::vector<EquityPoint> series);

 protected:
  void paintEvent(Wt::WPaintDevice* paintDevice) override;

 private:
  Mode mode_ = Mode::Account;
  std::string symbol_;
  std::vector<PricePoint> price_series_;
  std::vector<EquityPoint> account_series_;
  long long holdings_ = 0;
};

// One server tab: Positions (live trade) | Charts | Robots | Locks
class ServerPanel : public Wt::WContainerWidget {
 public:
  ServerPanel(ServerConfig cfg, SessionFeed* feed);

  void apply(const ServerSnapshot& snap);
  const ServerConfig& config() const { return cfg_; }

  void set_pdf_handler(std::function<void()> handler);

 private:
  void build();
  void rebuild_positions(const ServerSnapshot& snap);
  void rebuild_robots(const ServerSnapshot& snap);
  void rebuild_locks(const ServerSnapshot& snap);
  void rebuild_chart_selector(const ServerSnapshot& snap);
  void update_trade_quotes();
  void update_price_viewer();
  void post_action(const std::string& method, const std::string& path,
                   const std::string& body = {});
  void trade_focus_symbol(const std::string& symbol);
  void trade_go_symbol();
  void trade_submit();
  void trade_back();

  ServerConfig cfg_;
  SessionFeed* feed_{};
  ServerSnapshot latest_;

  Wt::WText* title_{};
  Wt::WText* status_{};
  Wt::WText* cash_{};
  Wt::WText* equity_{};
  Wt::WText* alpaca_rate_{};
  Wt::WText* action_note_{};
  Wt::WPushButton* pdf_btn_{};
  Wt::WTabWidget* screens_{};

  Wt::WTable* pos_table_{};
  EquityChart* chart_{};
  Wt::WTable* robot_table_{};
  Wt::WText* lock_summary_{};
  Wt::WTable* blocked_table_{};

  RateChart* rate_chart_{};
  PriceViewer* price_viewer_{};
  Wt::WContainerWidget* chart_buttons_{};
  Wt::WText* chart_mode_label_{};
  std::string chart_selection_ = "ACCOUNT";  // or symbol

  // Live trade bar lives on the Positions screen (see fills update the table).
  Wt::WContainerWidget* trade_bar_{};
  Wt::WContainerWidget* trade_phase1_{};
  Wt::WContainerWidget* trade_phase2_{};
  Wt::WLineEdit* trade_symbol_{};
  Wt::WLineEdit* trade_qty_{};
  Wt::WText* trade_quote_{};
  Wt::WText* trade_impact_{};
  Wt::WText* trade_current_{};
  Wt::WText* trade_live_hint_{};
  std::string trade_active_symbol_;

  std::function<void()> pdf_provider_;
  std::string last_pos_sig_;
  std::string last_robot_sig_;
  std::string last_lock_sig_;
  std::string last_chart_syms_;
};

}  // namespace hyperion
