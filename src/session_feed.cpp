#include "session_feed.h"

#include "http_client.h"

#include <Wt/WApplication.h>
#include <Wt/WServer.h>

#include <chrono>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <utility>

namespace hyperion {
namespace {

std::int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

SessionFeed::SessionFeed(Options options, Wt::WServer& server,
                         std::string session_id)
    : options_(std::move(options)),
      server_(server),
      session_id_(std::move(session_id)),
      publish_interval_(std::chrono::duration_cast<
          std::chrono::steady_clock::duration>(
          std::chrono::duration<double>{1.0 / options_.ui_hz})) {
  for (const auto& sc : options_.servers) {
    books_[sc.id] = std::make_unique<BookState>(
        sc, options_.equity_history_points, options_.price_history_points,
        options_.alpaca_limit_qpm);
    status_by_channel_[sc.status_channel] = sc.id;
    order_by_channel_[sc.order_channel] = sc.id;
    if (!sc.alpaca_channel.empty()) {
      alpaca_by_channel_[sc.alpaca_channel] = sc.id;
    }
  }
}

SessionFeed::~SessionFeed() { stop(); }

void SessionFeed::start(SnapshotHandler on_snapshots) {
  on_snapshots_ = std::move(on_snapshots);

  redis_ = std::make_unique<RedisMux>(
      options_.redis_path,
      [this](const std::string& ch, const std::string& payload) {
        redis_message(ch, payload);
      },
      [this](bool ok, const std::string& note) { redis_status(ok, note); });

  // Bootstrap channels: ORDER + STATUS + ALPACA admin.
  std::set<std::string> channels;
  for (const auto& sc : options_.servers) {
    channels.insert(sc.order_channel);
    channels.insert(sc.status_channel);
    if (!sc.alpaca_channel.empty()) {
      channels.insert(sc.alpaca_channel);
    }
  }
  redis_->set_channels(std::move(channels));
  redis_->start();

  // Immediate HTTP bootstrap in background.
  http_thr_ = std::jthread([this](std::stop_token stop) {
    // First poll immediately.
    http_poll_loop(stop);
  });

  equity_thr_ = std::jthread([this](std::stop_token stop) {
    equity_loop(stop);
  });

  // Keep source "hyperion" alive (bots use ~5s; jinghong TTL is 15s).
  heartbeat_thr_ = std::jthread([this](std::stop_token stop) {
    heartbeat_loop(stop);
  });

  publish(true);
}

void SessionFeed::stop() {
  if (http_thr_.joinable()) {
    http_thr_.request_stop();
    http_thr_.join();
  }
  if (equity_thr_.joinable()) {
    equity_thr_.request_stop();
    equity_thr_.join();
  }
  if (heartbeat_thr_.joinable()) {
    heartbeat_thr_.request_stop();
    heartbeat_thr_.join();
  }
  if (redis_) {
    redis_->stop();
    redis_.reset();
  }
}

void SessionFeed::set_watch_extra(const std::string& server_id,
                                  std::vector<std::string> symbols) {
  {
    std::lock_guard lock(mu_);
    auto it = books_.find(server_id);
    if (it != books_.end()) {
      it->second->set_watch_extra(std::move(symbols));
    }
  }
  refresh_price_channels();
}

Quote SessionFeed::quote(const std::string& server_id,
                         const std::string& symbol) const {
  std::lock_guard lock(mu_);
  auto it = books_.find(server_id);
  if (it == books_.end()) {
    return {};
  }
  return it->second->quote(symbol);
}

long long SessionFeed::hyperion_position(const std::string& server_id,
                                         const std::string& symbol) const {
  std::lock_guard lock(mu_);
  auto it = books_.find(server_id);
  if (it == books_.end()) {
    return 0;
  }
  return it->second->hyperion_position(symbol);
}

void SessionFeed::heartbeat_loop(std::stop_token stop) {
  // Hyperion is just another robot (source id "hyperion"): PUBLISH the same
  // heartbeat as algo bots every ~5s on each server's ORDER channel.
  // No HTTP fallback — if goblin-core is down, we are down.
  // See jinghong docs/HEARTBEATS.md — silence TTL is 15s.
  constexpr const char* kHb = R"({"source":"hyperion"})";
  while (!stop.stop_requested()) {
    for (const auto& sc : options_.servers) {
      if (stop.stop_requested()) {
        break;
      }
      if (!RedisMux::publish(options_.redis_path, sc.order_channel, kHb)) {
        continue;
      }
      // Local robots list (we also see our own ORDER pub via redis_mux sub).
      {
        std::lock_guard lock(mu_);
        auto it = books_.find(sc.id);
        if (it != books_.end()) {
          it->second->on_order_message(kHb);
        }
      }
    }
    publish(false);
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!stop.stop_requested() &&
           std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
  }
}

std::vector<ServerSnapshot> SessionFeed::current_snapshots() const {
  std::lock_guard lock(mu_);
  std::vector<ServerSnapshot> out;
  out.reserve(options_.servers.size());
  for (const auto& sc : options_.servers) {
    auto it = books_.find(sc.id);
    if (it != books_.end()) {
      out.push_back(it->second->snapshot());
    }
  }
  return out;
}

void SessionFeed::redis_status(bool connected, const std::string& note) {
  redis_connected_.store(connected);
  {
    std::lock_guard lock(mu_);
    for (auto& [id, book] : books_) {
      static_cast<void>(id);
      book->set_redis_ok(connected, note);
    }
  }
  publish(true);
}

void SessionFeed::redis_message(const std::string& channel,
                                const std::string& payload) {
  // Market data: T:SYM or Q:SYM
  if (channel.size() > 2 && channel[1] == ':') {
    const char kind = channel[0];
    const std::string symbol = channel.substr(2);
    if (kind == 'T' || kind == 'Q') {
      try {
        const auto j = nlohmann::json::parse(payload);
        double price = 0.0;
        std::int64_t ts = 0;
        if (kind == 'T') {
          if (j.contains("p")) {
            if (j["p"].is_number()) {
              price = j["p"].get<double>();
            } else if (j["p"].is_string()) {
              price = std::stod(j["p"].get<std::string>());
            }
          }
          if (j.contains("t") && j["t"].is_number()) {
            // Massive stock trades use ms timestamps.
            ts = j["t"].get<std::int64_t>();
          }
          std::lock_guard lock(mu_);
          for (auto& [id, book] : books_) {
            static_cast<void>(id);
            book->on_trade_tick(symbol, price, ts);
          }
        } else {
          double bid = 0.0;
          double ask = 0.0;
          if (j.contains("bp")) {
            bid = j["bp"].is_number() ? j["bp"].get<double>()
                                      : std::stod(j["bp"].get<std::string>());
          }
          if (j.contains("ap")) {
            ask = j["ap"].is_number() ? j["ap"].get<double>()
                                      : std::stod(j["ap"].get<std::string>());
          }
          if (j.contains("t") && j["t"].is_number()) {
            ts = j["t"].get<std::int64_t>();
          }
          std::lock_guard lock(mu_);
          for (auto& [id, book] : books_) {
            static_cast<void>(id);
            book->on_quote_tick(symbol, bid, ask, ts);
          }
        }
        publish(false);
      } catch (...) {
      }
      return;
    }
  }

  {
    std::lock_guard lock(mu_);
    if (auto it = status_by_channel_.find(channel);
        it != status_by_channel_.end()) {
      auto bit = books_.find(it->second);
      if (bit != books_.end()) {
        bit->second->on_status_message(payload);
      }
    } else if (auto oit = order_by_channel_.find(channel);
               oit != order_by_channel_.end()) {
      auto bit = books_.find(oit->second);
      if (bit != books_.end()) {
        bit->second->on_order_message(payload);
      }
    } else if (auto ait = alpaca_by_channel_.find(channel);
               ait != alpaca_by_channel_.end()) {
      auto bit = books_.find(ait->second);
      if (bit != books_.end()) {
        bit->second->on_alpaca_message(payload);
      }
    }
  }
  refresh_price_channels();
  publish(false);
}

void SessionFeed::refresh_price_channels() {
  std::set<std::string> channels;
  for (const auto& sc : options_.servers) {
    channels.insert(sc.order_channel);
    channels.insert(sc.status_channel);
    if (!sc.alpaca_channel.empty()) {
      channels.insert(sc.alpaca_channel);
    }
  }
  {
    std::lock_guard lock(mu_);
    for (const auto& [id, book] : books_) {
      static_cast<void>(id);
      for (const auto& sym : book->watched_symbols()) {
        channels.insert("T:" + sym);
        channels.insert("Q:" + sym);
      }
    }
  }
  if (redis_) {
    redis_->set_channels(std::move(channels));
  }
}

void SessionFeed::http_poll_loop(std::stop_token stop) {
  // 1) Bootstrap once: local GET /status (no Alpaca poll on jinghong).
  for (const auto& sc : options_.servers) {
    if (stop.stop_requested()) {
      return;
    }
    auto res = http_get(sc.http_base + "/status", 4000);
    if (res.ok()) {
      try {
        const auto j = nlohmann::json::parse(res.body);
        std::lock_guard lock(mu_);
        auto it = books_.find(sc.id);
        if (it != books_.end()) {
          it->second->apply_bootstrap_json(j);
        }
      } catch (const std::exception& e) {
        std::cerr << "hyperion: bootstrap " << sc.id << ": " << e.what()
                  << "\n";
      }
    } else {
      std::cerr << "hyperion: bootstrap failed " << sc.id << " "
                << res.error << "\n";
    }
  }
  refresh_price_channels();
  publish(true);

  // 2) Once a minute: GET /v1/account only (discrepancy check vs fill-tracked cash).
  while (!stop.stop_requested()) {
    const auto end =
        std::chrono::steady_clock::now() +
        std::chrono::seconds(std::max(15, options_.account_poll_sec));
    while (!stop.stop_requested() &&
           std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds{200});
    }
    if (stop.stop_requested()) {
      break;
    }
    for (const auto& sc : options_.servers) {
      if (stop.stop_requested()) {
        break;
      }
      auto res = http_get(sc.http_base + "/v1/account", 4000);
      if (res.ok()) {
        try {
          const auto j = nlohmann::json::parse(res.body);
          std::lock_guard lock(mu_);
          auto it = books_.find(sc.id);
          if (it != books_.end()) {
            it->second->apply_account_json(j);
          }
        } catch (const std::exception& e) {
          std::cerr << "hyperion: account " << sc.id << ": " << e.what()
                    << "\n";
        }
      }
    }
    publish(true);
  }
}

void SessionFeed::equity_loop(std::stop_token stop) {
  const auto interval = std::chrono::duration_cast<
      std::chrono::steady_clock::duration>(
      std::chrono::duration<double>{1.0 / options_.equity_sample_hz});
  while (!stop.stop_requested()) {
    {
      std::lock_guard lock(mu_);
      const auto ms = wall_ms();
      for (auto& [id, book] : books_) {
        static_cast<void>(id);
        book->sample_equity(ms);
      }
    }
    publish(false);
    const auto end = std::chrono::steady_clock::now() + interval;
    while (!stop.stop_requested() &&
           std::chrono::steady_clock::now() < end) {
      std::this_thread::sleep_for(std::chrono::milliseconds{50});
    }
  }
}

void SessionFeed::publish(bool force) {
  const auto now = std::chrono::steady_clock::now();
  if (!force && last_publish_ != std::chrono::steady_clock::time_point{} &&
      now - last_publish_ < publish_interval_) {
    return;
  }
  last_publish_ = now;
  auto snaps = current_snapshots();
  post_to_session(std::move(snaps));
}

void SessionFeed::post_to_session(std::vector<ServerSnapshot> snaps) {
  if (!on_snapshots_) {
    return;
  }
  server_.post(
      session_id_,
      [handler = on_snapshots_, snaps = std::move(snaps)]() mutable {
        if (handler) {
          handler(snaps);
        }
      },
      [] {});
}

}  // namespace hyperion
