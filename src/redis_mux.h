#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace hyperion {

// Multi-channel Redis/goblin-core subscriber over Unix domain socket (RESP).
// While subscribed, additional channels can be requested; the loop issues
// SUBSCRIBE / UNSUBSCRIBE as needed. Designed for one instance per Wt session.
class RedisMux {
 public:
  using MessageHandler =
      std::function<void(const std::string& channel, const std::string& payload)>;
  using StatusHandler =
      std::function<void(bool connected, const std::string& note)>;

  RedisMux(std::string uds_path, MessageHandler on_message,
           StatusHandler on_status);
  ~RedisMux();

  RedisMux(const RedisMux&) = delete;
  RedisMux& operator=(const RedisMux&) = delete;

  void start();
  void stop();

  // Desired channel set. Diffed against the live subscription.
  void set_channels(std::set<std::string> channels);

  // Best-effort PUBLISH (short-lived UDS connection; not the sub socket).
  static bool publish(const std::string& uds_path, const std::string& channel,
                      const std::string& payload);

 private:
  void run();
  bool consume_once();

  std::string uds_path_;
  MessageHandler on_message_;
  StatusHandler on_status_;

  std::mutex mu_;
  std::set<std::string> desired_;
  std::atomic<bool> desired_dirty_{false};

  std::atomic<bool> running_{false};
  std::thread thr_;
};

}  // namespace hyperion
