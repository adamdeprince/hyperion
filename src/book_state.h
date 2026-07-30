#pragma once

#include "options.h"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <nlohmann/json.hpp>

namespace hyperion {

enum class Certainty {
  Known,
  Uncertain,
};

struct Quote {
  std::optional<double> bid;
  std::optional<double> ask;
  std::optional<double> last;
};

struct PositionRow {
  std::string symbol;
  long long qty = 0;
  long long intended = 0;
  long long hyperion_qty = 0;  // source "hyperion" demand
  std::optional<double> last_price;
  std::optional<double> bid;
  std::optional<double> ask;
  Certainty certainty = Certainty::Known;
  std::string reason;
  std::string side;
  long long inflight_qty = 0;
  std::string order_id;
  bool blocked = false;  // trading freeze (not liquidation)
};

struct RobotRow {
  std::string source;
  bool muted = false;
  bool alive = false;
  double age_sec = -1.0;
  std::int64_t last_seen_ms = 0;  // wall clock of last ORDER/STATUS heartbeat
  std::unordered_map<std::string, long long> positions;
};

struct EquityPoint {
  std::int64_t ts_ms = 0;
  double equity = 0.0;
  double cash = 0.0;
};

struct PricePoint {
  std::int64_t seq = 0;  // equispaced event index
  double price = 0.0;
  long long holdings = 0;
};

struct RatePoint {
  std::int64_t ts_ms = 0;
  int qpm = 0;  // requests in trailing 60s
};

struct AlpacaRequestSummary {
  std::string verb;
  std::string uuid;
  std::string path;
  long http_status = 0;
  std::int64_t ts_ms = 0;
};

struct LocksState {
  bool global_block = false;
  std::vector<std::string> blocked_symbols;
  std::vector<std::string> muted_sources;
  bool liquidator_lock_supported = false;
};

struct ServerSnapshot {
  std::string server_id;
  std::string label;
  std::string http_base;
  std::string alpaca_channel;
  bool http_ok = false;
  bool redis_ok = false;
  std::string note;
  bool paper = true;
  bool dry_run = false;
  double cash = 0.0;              // expected cash (fill-tracked)
  double cash_broker = 0.0;       // last Alpaca account poll
  double cash_discrepancy = 0.0;  // broker - expected
  bool cash_discrepancy_alert = false;
  double buying_power = 0.0;
  double equity_reported = 0.0;
  double equity_marked = 0.0;
  std::int64_t ts_ms = 0;
  bool bootstrapped = false;
  std::vector<PositionRow> positions;
  std::vector<RobotRow> robots;
  LocksState locks;
  std::vector<EquityPoint> equity_series;
  std::vector<RatePoint> alpaca_qpm_series;
  int alpaca_qpm = 0;          // last-minute request count
  int alpaca_limit_qpm = 200;
  std::vector<AlpacaRequestSummary> alpaca_recent;  // newest last
  // symbol -> equispaced price path (for chart switcher)
  std::unordered_map<std::string, std::vector<PricePoint>> price_series;
  std::vector<std::string> chart_symbols;  // sorted keys with series
  std::size_t uncertain_count = 0;
  std::size_t known_count = 0;
};

class BookState {
 public:
  BookState(ServerConfig cfg, std::size_t equity_history_points = 900,
            std::size_t price_history_points = 300, int alpaca_limit_qpm = 200);

  const ServerConfig& config() const { return cfg_; }

  void set_redis_ok(bool ok, std::string note);
  // Bootstrap only (once): seed actual + cash from jinghong local status.
  void apply_bootstrap_json(const nlohmann::json& j);
  // Minute account poll: broker cash for discrepancy check.
  void apply_account_json(const nlohmann::json& j);
  void apply_status_json(const nlohmann::json& j);
  void apply_holdings_json(const nlohmann::json& j);
  void on_status_message(const std::string& payload);
  void on_order_message(const std::string& payload);
  void on_alpaca_message(const std::string& payload);
  void on_trade_tick(const std::string& symbol, double price, std::int64_t ts_ms);
  void on_quote_tick(const std::string& symbol, double bid, double ask,
                     std::int64_t ts_ms);

  void sample_equity(std::int64_t now_ms);
  void sample_alpaca_rate(std::int64_t now_ms);
  std::vector<std::string> watched_symbols() const;
  void set_watch_extra(std::vector<std::string> symbols);
  Quote quote(const std::string& symbol) const;
  long long hyperion_position(const std::string& symbol) const;
  ServerSnapshot snapshot() const;

 private:
  void mark_uncertain(const std::string& symbol, std::string reason);
  void clear_uncertain(const std::string& symbol);
  // Drop pending/ghost rows when local actual already matches demand.
  void clear_acked_pending();
  void recompute_equity();
  void apply_locks_json(const nlohmann::json& locks);
  void apply_sources_json(const nlohmann::json& sources);
  void touch_robot(const std::string& source, bool from_heartbeat = false);
  void refresh_robot_liveness() const;
  static std::int64_t now_ms();
  // Robots silent longer than this are shown as not alive (matches jinghong TTL).
  static constexpr double kRobotAliveSec = 15.0;

  ServerConfig cfg_;
  std::size_t equity_history_points_;
  std::size_t price_history_points_;
  int alpaca_limit_qpm_ = 200;

  bool redis_ok_ = false;
  std::string redis_note_ = "starting";
  bool http_ok_ = false;
  bool paper_ = true;
  bool dry_run_ = false;

  double cash_ = 0.0;         // expected (fill-tracked)
  double cash_broker_ = 0.0;  // last minute account poll (or fill-provisional)
  double buying_power_ = 0.0;
  double equity_reported_ = 0.0;
  double equity_marked_ = 0.0;
  std::int64_t ts_ms_ = 0;
  std::int64_t last_fill_ms_ = 0;     // wall clock of last fill-applied cash update
  std::int64_t last_account_ms_ = 0;  // wall clock of last GET /v1/account
  bool bootstrapped_ = false;
  static constexpr double kCashDiscrepancyTol = 1.0;  // dollars
  // After a fill, broker cash lags until the next account poll — do not alert
  // until a poll has run after the fill (plus a short settle window).
  static constexpr std::int64_t kCashDiscrepancyGraceMs = 5000;

  std::unordered_map<std::string, long long> actual_;
  std::unordered_map<std::string, long long> intended_;
  std::unordered_map<std::string, double> last_price_;
  std::unordered_map<std::string, double> bid_;
  std::unordered_map<std::string, double> ask_;
  std::unordered_map<std::string, long long> hyperion_qty_;  // source hyperion
  std::vector<std::string> watch_extra_;

  struct InFlight {
    std::string order_id;
    std::string side;
    long long qty = 0;
    long long filled_qty = 0;
    std::string reason;
  };
  void apply_fill(const std::string& symbol, const std::string& side,
                  long long fill_qty, double fill_price);
  std::unordered_map<std::string, InFlight> inflight_;
  std::unordered_map<std::string, std::string> pending_intent_;

  // Robots last known from HTTP / status / ORDER heartbeats.
  // Mutable so snapshot() can refresh age/alive without non-const API.
  mutable std::vector<RobotRow> robots_;
  LocksState locks_;

  std::deque<EquityPoint> equity_series_;

  // Alpaca REST rate (timestamps of requests in the last minute+).
  std::deque<std::int64_t> alpaca_request_times_ms_;
  std::deque<RatePoint> alpaca_qpm_series_;
  std::deque<AlpacaRequestSummary> alpaca_recent_;
  std::int64_t price_seq_ = 0;
  std::unordered_map<std::string, std::deque<PricePoint>> price_series_;
};

}  // namespace hyperion
