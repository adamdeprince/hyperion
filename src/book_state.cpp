#include "book_state.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <unordered_set>

namespace hyperion {
namespace {

long long json_ll(const nlohmann::json& j, const char* key, long long def = 0) {
  if (!j.contains(key)) {
    return def;
  }
  const auto& v = j[key];
  try {
    if (v.is_number_integer()) {
      return v.get<long long>();
    }
    if (v.is_number()) {
      return static_cast<long long>(v.get<double>());
    }
    if (v.is_string()) {
      return std::stoll(v.get<std::string>());
    }
  } catch (...) {
  }
  return def;
}

double json_num(const nlohmann::json& j, const char* key, double def = 0.0) {
  if (!j.contains(key) || j[key].is_null()) {
    return def;
  }
  const auto& v = j[key];
  try {
    if (v.is_number()) {
      return v.get<double>();
    }
    if (v.is_string()) {
      return std::stod(v.get<std::string>());
    }
  } catch (...) {
  }
  return def;
}

std::string json_str(const nlohmann::json& j, const char* key) {
  if (!j.contains(key) || j[key].is_null()) {
    return {};
  }
  const auto& v = j[key];
  if (v.is_string()) {
    return v.get<std::string>();
  }
  return v.dump();
}

std::unordered_map<std::string, long long> map_qty(const nlohmann::json& obj) {
  std::unordered_map<std::string, long long> out;
  if (!obj.is_object()) {
    return out;
  }
  for (auto it = obj.begin(); it != obj.end(); ++it) {
    try {
      if (it.value().is_number_integer()) {
        out[it.key()] = it.value().get<long long>();
      } else if (it.value().is_number()) {
        out[it.key()] = static_cast<long long>(it.value().get<double>());
      } else if (it.value().is_string()) {
        out[it.key()] = std::stoll(it.value().get<std::string>());
      }
    } catch (...) {
    }
  }
  return out;
}

}  // namespace

BookState::BookState(ServerConfig cfg, std::size_t equity_history_points,
                     std::size_t price_history_points, int alpaca_limit_qpm)
    : cfg_(std::move(cfg)),
      equity_history_points_(equity_history_points),
      price_history_points_(price_history_points),
      alpaca_limit_qpm_(alpaca_limit_qpm) {}

std::int64_t BookState::now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

void BookState::set_redis_ok(bool ok, std::string note) {
  redis_ok_ = ok;
  redis_note_ = std::move(note);
}

void BookState::mark_uncertain(const std::string& symbol, std::string reason) {
  if (symbol.empty()) {
    return;
  }
  // Do not create a zero-qty ghost inflight row (shows as Qty 0 / Target 0).
  auto it = inflight_.find(symbol);
  if (it != inflight_.end()) {
    if (it->second.reason.empty() || reason.find("order_") == 0) {
      it->second.reason = std::move(reason);
    }
    return;
  }
  pending_intent_[symbol] = std::move(reason);
}

void BookState::clear_uncertain(const std::string& symbol) {
  inflight_.erase(symbol);
  pending_intent_.erase(symbol);
}

void BookState::clear_acked_pending() {
  for (auto it = pending_intent_.begin(); it != pending_intent_.end();) {
    const auto& sym = it->first;
    if (inflight_.count(sym)) {
      ++it;
      continue;
    }
    const long long act = actual_.count(sym) ? actual_.at(sym) : 0;
    const long long want = intended_.count(sym) ? intended_.at(sym) : 0;
    if (act == want) {
      it = pending_intent_.erase(it);
    } else {
      ++it;
    }
  }
}

void BookState::recompute_equity() {
  double marked = cash_;
  for (const auto& [sym, qty] : actual_) {
    if (qty == 0) {
      continue;
    }
    auto pit = last_price_.find(sym);
    if (pit != last_price_.end()) {
      marked += static_cast<double>(qty) * pit->second;
    }
  }
  equity_marked_ = marked;
}

void BookState::apply_locks_json(const nlohmann::json& locks) {
  locks_ = {};
  locks_.liquidator_lock_supported = true;
  if (!locks.is_object()) {
    return;
  }
  locks_.global_block = locks.value("global_block", false);
  locks_.blocked_symbols.clear();
  if (locks.contains("blocked_symbols") && locks["blocked_symbols"].is_array()) {
    for (const auto& s : locks["blocked_symbols"]) {
      if (s.is_string()) {
        locks_.blocked_symbols.push_back(s.get<std::string>());
      }
    }
  }
  locks_.muted_sources.clear();
  if (locks.contains("muted_sources") && locks["muted_sources"].is_array()) {
    for (const auto& s : locks["muted_sources"]) {
      if (s.is_string()) {
        locks_.muted_sources.push_back(s.get<std::string>());
      }
    }
  }
}

void BookState::touch_robot(const std::string& source, bool /*from_heartbeat*/) {
  if (source.empty()) {
    return;
  }
  const auto now = now_ms();
  for (auto& r : robots_) {
    if (r.source == source) {
      r.last_seen_ms = now;
      r.age_sec = 0.0;
      r.alive = true;
      return;
    }
  }
  RobotRow r;
  r.source = source;
  r.alive = true;
  r.age_sec = 0.0;
  r.last_seen_ms = now;
  robots_.push_back(std::move(r));
}

void BookState::refresh_robot_liveness() const {
  const auto now = now_ms();
  for (auto& r : robots_) {
    if (r.last_seen_ms > 0) {
      r.age_sec = static_cast<double>(now - r.last_seen_ms) / 1000.0;
      r.alive = r.age_sec <= kRobotAliveSec;
    }
  }
}

void BookState::apply_sources_json(const nlohmann::json& sources) {
  // Merge jinghong's source list into robots_ (do not wipe live heartbeats).
  if (!sources.is_array()) {
    return;
  }
  const auto now = now_ms();
  for (const auto& s : sources) {
    if (!s.is_object()) {
      continue;
    }
    const std::string source = json_str(s, "source");
    if (source.empty()) {
      continue;
    }
    std::unordered_map<std::string, long long> positions;
    if (s.contains("positions") && s["positions"].is_object()) {
      positions = map_qty(s["positions"]);
    }
    if (source == "hyperion") {
      hyperion_qty_ = positions;
    }
    const bool muted = s.value("muted", false);
    const double age = json_num(s, "age_sec", -1.0);
    bool found = false;
    for (auto& r : robots_) {
      if (r.source == source) {
        r.muted = muted;
        r.positions = positions;
        if (age >= 0.0 && age < r.age_sec) {
          r.age_sec = age;
          r.last_seen_ms = now - static_cast<std::int64_t>(age * 1000.0);
        }
        if (s.value("alive", false) || (age >= 0.0 && age <= kRobotAliveSec)) {
          r.alive = true;
        }
        found = true;
        break;
      }
    }
    if (!found) {
      RobotRow r;
      r.source = source;
      r.muted = muted;
      r.positions = std::move(positions);
      r.age_sec = age;
      r.alive = s.value("alive", false) ||
                (age >= 0.0 && age <= kRobotAliveSec);
      if (age >= 0.0) {
        r.last_seen_ms = now - static_cast<std::int64_t>(age * 1000.0);
      }
      robots_.push_back(std::move(r));
    }
  }
  refresh_robot_liveness();
  std::sort(robots_.begin(), robots_.end(),
            [](const RobotRow& a, const RobotRow& b) {
              if (a.muted != b.muted) {
                return a.muted && !b.muted;
              }
              if (a.alive != b.alive) {
                return a.alive && !b.alive;
              }
              return a.source < b.source;
            });
}

void BookState::set_watch_extra(std::vector<std::string> symbols) {
  watch_extra_ = std::move(symbols);
}

Quote BookState::quote(const std::string& symbol) const {
  Quote q;
  if (auto it = bid_.find(symbol); it != bid_.end()) {
    q.bid = it->second;
  }
  if (auto it = ask_.find(symbol); it != ask_.end()) {
    q.ask = it->second;
  }
  if (auto it = last_price_.find(symbol); it != last_price_.end()) {
    q.last = it->second;
  }
  return q;
}

long long BookState::hyperion_position(const std::string& symbol) const {
  auto it = hyperion_qty_.find(symbol);
  return it == hyperion_qty_.end() ? 0 : it->second;
}

void BookState::apply_fill(const std::string& symbol, const std::string& side,
                           long long fill_qty, double fill_price) {
  if (symbol.empty() || fill_qty <= 0) {
    return;
  }
  long long& pos = actual_[symbol];
  if (side == "buy") {
    pos += fill_qty;
    if (fill_price > 0.0) {
      const double d = static_cast<double>(fill_qty) * fill_price;
      cash_ -= d;
      // Keep broker shadow in step so UI does not flash MISMATCH until poll.
      cash_broker_ -= d;
    }
  } else if (side == "sell") {
    pos -= fill_qty;
    if (fill_price > 0.0) {
      const double d = static_cast<double>(fill_qty) * fill_price;
      cash_ += d;
      cash_broker_ += d;
    }
  }
  if (pos == 0) {
    actual_.erase(symbol);
  }
  last_fill_ms_ = now_ms();
  recompute_equity();
  sample_equity(last_fill_ms_);
}

void BookState::apply_bootstrap_json(const nlohmann::json& j) {
  // One-shot seed from jinghong local status (positions already loaded at
  // jinghong start — this does not re-hit Alpaca if status is local).
  apply_status_json(j);
  bootstrapped_ = true;
  cash_broker_ = cash_;
}

void BookState::apply_account_json(const nlohmann::json& j) {
  // Minute reconcile against broker cash (authoritative for discrepancy).
  const double broker = json_num(j, "cash", cash_broker_);
  cash_broker_ = broker;
  last_account_ms_ = now_ms();
  if (j.contains("buying_power")) {
    buying_power_ = json_num(j, "buying_power", buying_power_);
  }
  if (j.contains("equity")) {
    equity_reported_ = json_num(j, "equity", equity_reported_);
  }
  http_ok_ = j.value("ok", http_ok_);
  ts_ms_ = json_ll(j, "ts_ms", now_ms());
  recompute_equity();
  sample_equity(last_account_ms_);
}

void BookState::apply_status_json(const nlohmann::json& j) {
  http_ok_ = j.value("ok", false);
  paper_ = j.value("paper", true);
  dry_run_ = j.value("dry_run", false);
  // Only seed cash from status on first bootstrap; afterwards fills own cash_.
  if (!bootstrapped_) {
    cash_ = json_num(j, "cash", cash_);
    cash_broker_ = cash_;
  }
  buying_power_ = json_num(j, "buying_power", buying_power_);
  equity_reported_ = json_num(j, "equity", equity_reported_);
  ts_ms_ = json_ll(j, "ts_ms", now_ms());

  if (j.contains("actual")) {
    // On bootstrap take full book; later STATUS events update fills.
    if (!bootstrapped_) {
      actual_ = map_qty(j["actual"]);
    }
  }
  if (j.contains("intended")) {
    intended_ = map_qty(j["intended"]);
  }

  if (!bootstrapped_) {
    inflight_.clear();
    if (j.contains("inflight") && j["inflight"].is_object()) {
      for (auto it = j["inflight"].begin(); it != j["inflight"].end(); ++it) {
        InFlight inf;
        if (it.value().is_object()) {
          inf.order_id = json_str(it.value(), "order_id");
          inf.side = json_str(it.value(), "side");
          inf.qty = json_ll(it.value(), "qty");
          inf.filled_qty = json_ll(it.value(), "filled_qty");
          inf.reason = "inflight";
        }
        inflight_[it.key()] = std::move(inf);
      }
    }
  }

  if (j.contains("locks")) {
    apply_locks_json(j["locks"]);
  } else if (j.value("liquidator_lock", false)) {
    locks_.liquidator_lock_supported = true;
  }
  if (j.contains("sources")) {
    apply_sources_json(j["sources"]);
  }

  clear_acked_pending();
  recompute_equity();
}

void BookState::apply_holdings_json(const nlohmann::json& j) {
  if (!j.is_object()) {
    return;
  }
  actual_ = map_qty(j);
  recompute_equity();
  http_ok_ = true;
  ts_ms_ = now_ms();
}

void BookState::on_status_message(const std::string& payload) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(payload);
  } catch (...) {
    return;
  }
  ts_ms_ = json_ll(j, "ts_ms", now_ms());
  const std::string type = json_str(j, "type");
  const std::string symbol = json_str(j, "symbol");

  if (type == "heartbeat") {
    const std::string source = json_str(j, "source");
    touch_robot(source, true);
    if (j.contains("sources")) {
      apply_sources_json(j["sources"]);
    }
    return;
  }

  if (type == "heartbeat_expire") {
    if (j.contains("book") && j["book"].is_object() &&
        j["book"].contains("sources") && j["book"]["sources"].is_object()) {
      nlohmann::json arr = nlohmann::json::array();
      for (auto it = j["book"]["sources"].begin();
           it != j["book"]["sources"].end(); ++it) {
        nlohmann::json row = it.value();
        row["source"] = it.key();
        arr.push_back(row);
      }
      apply_sources_json(arr);
    }
    if (j.contains("book") && j["book"].is_object() &&
        j["book"].contains("aggregate")) {
      intended_ = map_qty(j["book"]["aggregate"]);
    }
    refresh_robot_liveness();
    return;
  }

  if (type == "liquidator_lock") {
    if (j.contains("locks")) {
      apply_locks_json(j["locks"]);
    }
    if (j.contains("book") && j["book"].is_object()) {
      if (j["book"].contains("sources") && j["book"]["sources"].is_object()) {
        // debug_json sources is object; convert to array for UI
        nlohmann::json arr = nlohmann::json::array();
        for (auto it = j["book"]["sources"].begin();
             it != j["book"]["sources"].end(); ++it) {
          nlohmann::json row = it.value();
          row["source"] = it.key();
          arr.push_back(row);
        }
        apply_sources_json(arr);
      }
      if (j["book"].contains("aggregate")) {
        intended_ = map_qty(j["book"]["aggregate"]);
      }
    }
    return;
  }

  if (type == "order_submit") {
    const long long qty = json_ll(j, "qty");
    // Never track a zero-share "order" — that is a no-op / bug, not a fill.
    if (qty <= 0) {
      clear_uncertain(symbol);
      return;
    }
    InFlight inf;
    inf.side = json_str(j, "side");
    inf.qty = qty;
    if (j.contains("body") && j["body"].is_object()) {
      inf.order_id = json_str(j["body"], "id");
      inf.filled_qty = json_ll(j["body"], "filled_qty");
      const std::string st = json_str(j["body"], "status");
      if (st == "filled") {
        // Keep real qty on the row; fill_applied is authoritative for cash/pos.
        inf.reason = "filled (awaiting fill_applied)";
        inflight_[symbol] = std::move(inf);
        pending_intent_.erase(symbol);
        return;
      }
    }
    inf.reason = "order_submit (awaiting fill)";
    inflight_[symbol] = std::move(inf);
    pending_intent_.erase(symbol);
    return;
  }

  if (type == "reconcile_skip" || type == "inflight_cleared_at_target") {
    // Already at target / no work — intent is acknowledged without a fill.
    clear_uncertain(symbol);
    return;
  }

  if (type == "reconcile") {
    // Jinghong planned work; keep uncertainty only if a real delta remains.
    if (j.contains("actual") && j.contains("target")) {
      const long long act = json_ll(j, "actual", 0);
      const long long tgt = json_ll(j, "target", 0);
      if (act == tgt) {
        clear_uncertain(symbol);
      }
    }
    return;
  }

  if (type == "fill_applied") {
    // Authoritative post-fill snapshot from jinghong (local book, no poll).
    if (j.contains("position")) {
      const long long pos = json_ll(j, "position", 0);
      if (pos == 0) {
        actual_.erase(symbol);
      } else {
        actual_[symbol] = pos;
      }
      // Position set absolutely — still apply cash via field or delta below.
      if (!(j.contains("cash") && !j["cash"].is_null())) {
        // No absolute cash: fall back to fill delta on cash only.
        const long long fq = json_ll(j, "fill_qty", 0);
        const std::string side = json_str(j, "side");
        const double px = json_num(j, "fill_price", 0.0);
        if (fq > 0 && px > 0.0) {
          const double d = static_cast<double>(fq) * px;
          if (side == "buy") {
            cash_ -= d;
            cash_broker_ -= d;
          } else if (side == "sell") {
            cash_ += d;
            cash_broker_ += d;
          }
        }
      }
    } else {
      apply_fill(symbol, json_str(j, "side"), json_ll(j, "fill_qty", 0),
                 json_num(j, "fill_price", 0.0));
      // apply_fill already sampled equity / advanced broker shadow.
      const std::string ev = json_str(j, "event");
      if (ev == "fill" || ev.empty()) {
        clear_uncertain(symbol);
      } else if (ev == "partial_fill") {
        mark_uncertain(symbol, "partial_fill");
      }
      return;
    }
    if (j.contains("cash") && !j["cash"].is_null()) {
      // Jinghong fill-tracked cash is the post-fill truth until next poll.
      cash_ = json_num(j, "cash", cash_);
      cash_broker_ = cash_;
    }
    last_fill_ms_ = now_ms();
    const std::string ev = json_str(j, "event");
    if (ev == "fill" || ev.empty()) {
      clear_uncertain(symbol);
    } else if (ev == "partial_fill") {
      mark_uncertain(symbol, "partial_fill");
    }
    recompute_equity();
    // Point on the chart at the fill instant (don't wait for equity_loop).
    sample_equity(last_fill_ms_);
    return;
  }

  if (type == "order_reject") {
    clear_uncertain(symbol);
    return;
  }

  if (type == "order_cancel" || type == "order_canceled" ||
      type == "order_cancel_no_id") {
    if (type == "order_canceled") {
      clear_uncertain(symbol);
    } else {
      mark_uncertain(symbol, type + " (pending)");
    }
    return;
  }

  if (type == "dry_fill") {
    const long long pos = json_ll(j, "position");
    const long long qty = json_ll(j, "qty", 0);
    const std::string side = json_str(j, "side");
    // Approximate cash if we have a last price.
    double px = last_price_.count(symbol) ? last_price_.at(symbol) : 0.0;
    if (qty > 0 && !side.empty()) {
      apply_fill(symbol, side, qty, px);
    } else {
      if (pos == 0) {
        actual_.erase(symbol);
      } else {
        actual_[symbol] = pos;
      }
    }
    clear_uncertain(symbol);
    recompute_equity();
    return;
  }

  if (type == "intent") {
    // Jinghong accepted the ORDER intent into its book (ack).
    if (j.contains("book") && j["book"].is_object() &&
        j["book"].contains("aggregate")) {
      intended_ = map_qty(j["book"]["aggregate"]);
    }
    // Same-size re-intent or flat: actual already matches → not "unacked".
    if (!symbol.empty()) {
      const long long act = actual_.count(symbol) ? actual_.at(symbol) : 0;
      const long long want =
          intended_.count(symbol) ? intended_.at(symbol) : 0;
      if (act == want) {
        clear_uncertain(symbol);
      }
    } else {
      clear_acked_pending();
    }
    // If changed=false, jinghong did nothing — clear any sticky pending.
    if (j.contains("changed") && j["changed"].is_boolean() &&
        !j["changed"].get<bool>()) {
      if (!symbol.empty()) {
        // Prefer symbol from raw intent if present.
        std::string raw_sym = symbol;
        if (j.contains("raw") && j["raw"].is_object()) {
          raw_sym = json_str(j["raw"], "symbol");
        }
        if (!raw_sym.empty()) {
          const long long act =
              actual_.count(raw_sym) ? actual_.at(raw_sym) : 0;
          const long long want =
              intended_.count(raw_sym) ? intended_.at(raw_sym) : 0;
          if (act == want) {
            clear_uncertain(raw_sym);
          }
        }
      } else {
        clear_acked_pending();
      }
    }
    return;
  }

  if (type == "alpaca_ws") {
    // Prefer fill_applied events from jinghong; still track uncertainty here.
    try {
      const nlohmann::json* d = nullptr;
      if (j.contains("event") && j["event"].is_object()) {
        const auto& ev = j["event"];
        if (ev.contains("data") && ev["data"].is_object()) {
          d = &ev["data"];
        } else if (ev.contains("event")) {
          d = &ev;
        }
      }
      if (d == nullptr) {
        return;
      }
      const std::string ev = d->value("event", "");
      std::string sym;
      nlohmann::json order = nlohmann::json::object();
      if (d->contains("order") && (*d)["order"].is_object()) {
        order = (*d)["order"];
        sym = json_str(order, "symbol");
      }
      if (sym.empty()) {
        sym = symbol;
      }
      if (ev == "partial_fill" || ev == "pending_new" || ev == "new" ||
          ev == "accepted" || ev == "pending_cancel") {
        mark_uncertain(sym, "alpaca:" + ev);
        if (!order.empty()) {
          InFlight inf;
          inf.order_id = json_str(order, "id");
          inf.side = json_str(order, "side");
          inf.qty = json_ll(order, "qty", 0);
          inf.filled_qty = json_ll(order, "filled_qty", 0);
          inf.reason = "alpaca:" + ev;
          inflight_[sym] = std::move(inf);
        }
      } else if (ev == "fill" || ev == "canceled" || ev == "rejected" ||
                 ev == "expired" || ev == "done_for_day") {
        // fill_applied should have updated holdings; clear outstanding.
        clear_uncertain(sym);
      }
    } catch (...) {
    }
    return;
  }

  if (type == "started" || type == "heartbeat_expire" ||
      type == "cancel_timeout" || type == "reconcile_error" ||
      type == "intent_error") {
    // Informational; surface via note if needed.
    return;
  }
}

void BookState::on_order_message(const std::string& payload) {
  // Intent or heartbeat from a trading agent on ACCOUNT:ORDER:*.
  // Heartbeats alone must put the robot on Hyperion's Robots list.
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(payload);
  } catch (...) {
    return;
  }
  const std::string source = json_str(j, "source");
  if (!source.empty()) {
    touch_robot(source, true);
  }
  const std::string symbol = json_str(j, "symbol");
  if (symbol.empty() || !j.contains("position")) {
    return;  // heartbeat only — robot already registered
  }
  long long pos = 0;
  try {
    if (j["position"].is_number_integer()) {
      pos = j["position"].get<long long>();
    } else if (j["position"].is_number()) {
      pos = static_cast<long long>(j["position"].get<double>());
    }
  } catch (...) {
  }
  // Track demand on the robot row.
  if (!source.empty()) {
    for (auto& r : robots_) {
      if (r.source == source) {
        if (pos == 0) {
          r.positions.erase(symbol);
        } else {
          r.positions[symbol] = pos;
        }
        break;
      }
    }
    if (source == "hyperion") {
      if (pos == 0) {
        hyperion_qty_.erase(symbol);
      } else {
        hyperion_qty_[symbol] = pos;
      }
    }
  }
  // Optimistic intended update so UI target moves immediately; jinghong
  // STATUS type=intent will reconcile multi-source aggregate.
  if (pos == 0) {
    // Single-source clear is ambiguous for multi-source aggregate; only
    // mark pending if we still hold stock and need a sell.
    const long long act = actual_.count(symbol) ? actual_.at(symbol) : 0;
    if (act == 0) {
      clear_uncertain(symbol);  // flat → flat: acknowledged no-op
    } else {
      pending_intent_[symbol] = "intent from " +
                                (source.empty() ? "?" : source) +
                                " flatten (awaiting fill)";
    }
    return;
  }
  const long long act = actual_.count(symbol) ? actual_.at(symbol) : 0;
  if (act == pos) {
    // Same-size re-intent: already holding target — not unacked.
    clear_uncertain(symbol);
    return;
  }
  pending_intent_[symbol] =
      "intent from " + (source.empty() ? "?" : source) + " (awaiting fill)";
}

void BookState::on_trade_tick(const std::string& symbol, double price,
                              std::int64_t ts_ms) {
  if (symbol.empty() || !(price > 0.0) || !std::isfinite(price)) {
    return;
  }
  last_price_[symbol] = price;
  if (ts_ms > 0) {
    ts_ms_ = ts_ms;
  }
  // Equispaced price series (one sample per incoming event).
  long long hold = actual_.count(symbol) ? actual_.at(symbol) : 0;
  auto& series = price_series_[symbol];
  series.push_back(PricePoint{++price_seq_, price, hold});
  while (series.size() > price_history_points_) {
    series.pop_front();
  }
  recompute_equity();
}

void BookState::on_alpaca_message(const std::string& payload) {
  nlohmann::json j;
  try {
    j = nlohmann::json::parse(payload);
  } catch (...) {
    return;
  }
  const std::int64_t ts = j.contains("ts_ms") && j["ts_ms"].is_number()
                              ? j["ts_ms"].get<std::int64_t>()
                              : now_ms();
  alpaca_request_times_ms_.push_back(ts);
  // Keep ~2 minutes of timestamps for trailing window.
  while (!alpaca_request_times_ms_.empty() &&
         ts - alpaca_request_times_ms_.front() > 120'000) {
    alpaca_request_times_ms_.pop_front();
  }

  AlpacaRequestSummary s;
  if (j.contains("verb") && j["verb"].is_string()) {
    s.verb = j["verb"].get<std::string>();
  }
  if (j.contains("uuid") && j["uuid"].is_string()) {
    s.uuid = j["uuid"].get<std::string>();
  }
  if (j.contains("path") && j["path"].is_string()) {
    s.path = j["path"].get<std::string>();
  }
  if (j.contains("http_status") && j["http_status"].is_number()) {
    s.http_status = j["http_status"].get<long>();
  }
  s.ts_ms = ts;
  alpaca_recent_.push_back(std::move(s));
  while (alpaca_recent_.size() > 40) {
    alpaca_recent_.pop_front();
  }
  sample_alpaca_rate(ts);
}

void BookState::sample_alpaca_rate(std::int64_t now_ms) {
  const std::int64_t cutoff = now_ms - 60'000;
  while (!alpaca_request_times_ms_.empty() &&
         alpaca_request_times_ms_.front() < cutoff) {
    alpaca_request_times_ms_.pop_front();
  }
  const int qpm = static_cast<int>(alpaca_request_times_ms_.size());
  alpaca_qpm_series_.push_back(RatePoint{now_ms, qpm});
  while (alpaca_qpm_series_.size() > equity_history_points_) {
    alpaca_qpm_series_.pop_front();
  }
}

void BookState::on_quote_tick(const std::string& symbol, double bid, double ask,
                              std::int64_t ts_ms) {
  if (bid > 0.0 && std::isfinite(bid)) {
    bid_[symbol] = bid;
  }
  if (ask > 0.0 && std::isfinite(ask)) {
    ask_[symbol] = ask;
  }
  double mid = 0.0;
  if (bid > 0.0 && ask > 0.0) {
    mid = 0.5 * (bid + ask);
  } else if (bid > 0.0) {
    mid = bid;
  } else if (ask > 0.0) {
    mid = ask;
  }
  if (mid > 0.0) {
    on_trade_tick(symbol, mid, ts_ms);
  }
}

void BookState::sample_equity(std::int64_t now_ms) {
  recompute_equity();
  double eq = equity_marked_;
  if (last_price_.empty() && equity_reported_ > 0.0) {
    eq = equity_reported_;
  }
  equity_series_.push_back(EquityPoint{now_ms, eq, cash_});
  while (equity_series_.size() > equity_history_points_) {
    equity_series_.pop_front();
  }
  sample_alpaca_rate(now_ms);
}

std::vector<std::string> BookState::watched_symbols() const {
  std::vector<std::string> out;
  auto add = [&](const std::string& s) {
    if (!s.empty() &&
        std::find(out.begin(), out.end(), s) == out.end()) {
      out.push_back(s);
    }
  };
  for (const auto& [s, q] : actual_) {
    if (q != 0) {
      add(s);
    }
  }
  for (const auto& [s, _] : inflight_) {
    add(s);
  }
  for (const auto& [s, _] : pending_intent_) {
    add(s);
  }
  for (const auto& [s, q] : intended_) {
    if (q != 0) {
      add(s);
    }
  }
  for (const auto& s : watch_extra_) {
    add(s);
  }
  return out;
}

ServerSnapshot BookState::snapshot() const {
  ServerSnapshot snap;
  snap.server_id = cfg_.id;
  snap.label = cfg_.label;
  snap.http_base = cfg_.http_base;
  snap.alpaca_channel = cfg_.alpaca_channel;
  snap.http_ok = http_ok_;
  snap.redis_ok = redis_ok_;
  snap.note = redis_note_;
  snap.paper = paper_;
  snap.dry_run = dry_run_;
  snap.cash = cash_;
  snap.cash_broker = cash_broker_;
  snap.cash_discrepancy = cash_broker_ - cash_;
  // True mismatches only after an account poll that post-dates the last fill.
  // (Fill path keeps cash_broker_ provisional so the header does not flash.)
  const bool poll_after_fill =
      last_account_ms_ > 0 &&
      last_account_ms_ >= last_fill_ms_ + kCashDiscrepancyGraceMs;
  snap.cash_discrepancy_alert =
      bootstrapped_ && poll_after_fill &&
      std::abs(snap.cash_discrepancy) > kCashDiscrepancyTol;
  snap.buying_power = buying_power_;
  snap.equity_reported = equity_reported_;
  snap.equity_marked = equity_marked_;
  snap.ts_ms = ts_ms_;
  snap.bootstrapped = bootstrapped_;
  snap.equity_series.assign(equity_series_.begin(), equity_series_.end());
  snap.alpaca_qpm_series.assign(alpaca_qpm_series_.begin(),
                                alpaca_qpm_series_.end());
  snap.alpaca_qpm = alpaca_qpm_series_.empty() ? 0 : alpaca_qpm_series_.back().qpm;
  snap.alpaca_limit_qpm = alpaca_limit_qpm_;
  snap.alpaca_recent.assign(alpaca_recent_.begin(), alpaca_recent_.end());
  for (const auto& [sym, series] : price_series_) {
    if (!series.empty()) {
      snap.price_series[sym].assign(series.begin(), series.end());
      snap.chart_symbols.push_back(sym);
    }
  }
  std::sort(snap.chart_symbols.begin(), snap.chart_symbols.end());
  refresh_robot_liveness();
  snap.robots = robots_;
  snap.locks = locks_;

  std::unordered_set<std::string> blocked_set(locks_.blocked_symbols.begin(),
                                              locks_.blocked_symbols.end());

  std::vector<std::string> symbols;
  auto add_sym = [&](const std::string& s) {
    if (!s.empty() &&
        std::find(symbols.begin(), symbols.end(), s) == symbols.end()) {
      symbols.push_back(s);
    }
  };
  for (const auto& [s, q] : actual_) {
    if (q != 0) {
      add_sym(s);
    }
  }
  for (const auto& [s, q] : intended_) {
    if (q != 0) {
      add_sym(s);
    }
  }
  // Skip zero-qty ghost inflight (looks like "buy 0" / Qty 0 Target 0).
  for (const auto& [s, inf] : inflight_) {
    if (inf.qty <= 0) {
      const long long act = actual_.count(s) ? actual_.at(s) : 0;
      const long long want = intended_.count(s) ? intended_.at(s) : 0;
      if (act == want) {
        continue;  // ghost — do not surface
      }
    }
    add_sym(s);
  }
  for (const auto& [s, _] : pending_intent_) {
    add_sym(s);
  }

  for (const auto& sym : symbols) {
    PositionRow row;
    row.symbol = sym;
    auto ait = actual_.find(sym);
    row.qty = ait == actual_.end() ? 0 : ait->second;
    auto iit = intended_.find(sym);
    row.intended = iit == intended_.end() ? 0 : iit->second;
    auto pit = last_price_.find(sym);
    if (pit != last_price_.end()) {
      row.last_price = pit->second;
    }
    if (auto it = bid_.find(sym); it != bid_.end()) {
      row.bid = it->second;
    }
    if (auto it = ask_.find(sym); it != ask_.end()) {
      row.ask = it->second;
    }
    if (auto it = hyperion_qty_.find(sym); it != hyperion_qty_.end()) {
      row.hyperion_qty = it->second;
    }

    row.blocked = locks_.global_block || blocked_set.count(sym) > 0;

    auto fit = inflight_.find(sym);
    auto pend = pending_intent_.find(sym);
    if (fit != inflight_.end()) {
      row.certainty = Certainty::Uncertain;
      row.reason = fit->second.reason.empty() ? "in-flight order"
                                              : fit->second.reason;
      row.side = fit->second.side;
      row.inflight_qty = fit->second.qty;
      row.order_id = fit->second.order_id;
      ++snap.uncertain_count;
    } else if (pend != pending_intent_.end()) {
      row.certainty = Certainty::Uncertain;
      row.reason = pend->second;
      ++snap.uncertain_count;
    } else {
      row.certainty = Certainty::Known;
      ++snap.known_count;
    }
    if (row.blocked && row.reason.empty()) {
      row.reason = locks_.global_block ? "global liquidator lock"
                                       : "symbol liquidator lock";
    }
    snap.positions.push_back(std::move(row));
  }

  // Uncertain first, then known; within each group sort by |notional| desc.
  std::stable_sort(snap.positions.begin(), snap.positions.end(),
                   [](const PositionRow& a, const PositionRow& b) {
                     const bool au = a.certainty == Certainty::Uncertain;
                     const bool bu = b.certainty == Certainty::Uncertain;
                     if (au != bu) {
                       return au && !bu;
                     }
                     const double pa = a.last_price.value_or(0.0);
                     const double pb = b.last_price.value_or(0.0);
                     return std::abs(static_cast<double>(a.qty) * pa) >
                            std::abs(static_cast<double>(b.qty) * pb);
                   });
  return snap;
}

}  // namespace hyperion
