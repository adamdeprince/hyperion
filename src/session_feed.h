#pragma once

#include "book_state.h"
#include "options.h"
#include "redis_mux.h"

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace Wt {
class WServer;
}

namespace hyperion {

// Per Wt session: owns a RedisMux on goblin-core UDS, polls jinghong HTTP,
// maintains one BookState per server tab, and posts snapshots to the UI.
class SessionFeed {
 public:
  using SnapshotHandler =
      std::function<void(const std::vector<ServerSnapshot>& snaps)>;

  SessionFeed(Options options, Wt::WServer& server, std::string session_id);
  ~SessionFeed();

  SessionFeed(const SessionFeed&) = delete;
  SessionFeed& operator=(const SessionFeed&) = delete;

  void start(SnapshotHandler on_snapshots);
  void stop();

  std::vector<ServerSnapshot> current_snapshots() const;
  std::vector<ServerConfig> servers() const { return options_.servers; }

  // Extra symbols to price-subscribe (trade wizard).
  void set_watch_extra(const std::string& server_id,
                       std::vector<std::string> symbols);
  Quote quote(const std::string& server_id, const std::string& symbol) const;
  long long hyperion_position(const std::string& server_id,
                              const std::string& symbol) const;

 private:
  void redis_message(const std::string& channel, const std::string& payload);
  void redis_status(bool connected, const std::string& note);
  void http_poll_loop(std::stop_token stop);
  void equity_loop(std::stop_token stop);
  void heartbeat_loop(std::stop_token stop);
  void refresh_price_channels();
  void publish(bool force = false);
  void post_to_session(std::vector<ServerSnapshot> snaps);

  Options options_;
  Wt::WServer& server_;
  std::string session_id_;

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::unique_ptr<BookState>> books_;
  // channel -> server id for ORDER/STATUS/ALPACA routing
  std::unordered_map<std::string, std::string> status_by_channel_;
  std::unordered_map<std::string, std::string> order_by_channel_;
  std::unordered_map<std::string, std::string> alpaca_by_channel_;

  SnapshotHandler on_snapshots_;
  std::unique_ptr<RedisMux> redis_;
  std::jthread http_thr_;
  std::jthread equity_thr_;
  std::jthread heartbeat_thr_;

  std::chrono::steady_clock::time_point last_publish_{};
  std::chrono::steady_clock::duration publish_interval_;
  std::atomic<bool> redis_connected_{false};
};

}  // namespace hyperion
