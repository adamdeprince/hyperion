#include "redis_mux.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <functional>
#include <iostream>
#include <memory>
#include <poll.h>
#include <string_view>
#include <utility>
#include <vector>

#include <hiredis/hiredis.h>

namespace hyperion {
namespace {

struct ContextDeleter {
  void operator()(redisContext* c) const {
    if (c) {
      redisFree(c);
    }
  }
};

struct ReplyDeleter {
  void operator()(redisReply* r) const {
    if (r) {
      freeReplyObject(r);
    }
  }
};

using Context = std::unique_ptr<redisContext, ContextDeleter>;
using Reply = std::unique_ptr<redisReply, ReplyDeleter>;

Context connect_unix(const std::string& path) {
  Context ctx{redisConnectUnix(path.c_str())};
  if (!ctx || ctx->err) {
    return {};
  }
  // No SO_RCVTIMEO: we drive reads with poll(). A short timeout makes
  // redisBufferRead fail with EAGAIN and was flipping the UI to "redis down".
  timeval tv{.tv_sec = 0, .tv_usec = 0};
  redisSetTimeout(ctx.get(), tv);
  return ctx;
}

// SUBSCRIBE/UNSUBSCRIBE while possibly already in pubsub. Drain any interleaved
// "message" replies so we don't treat them as command failures.
bool issue_subscribe(
    redisContext* ctx, const std::string& channel, bool sub,
    const std::function<void(const std::string&, const std::string&)>& on_msg) {
  const char* cmd = sub ? "SUBSCRIBE" : "UNSUBSCRIBE";
  if (redisAppendCommand(ctx, "%s %b", cmd, channel.data(), channel.size()) !=
      REDIS_OK) {
    return false;
  }
  // Flush + collect replies until we see subscribe/unsubscribe ack.
  for (int guard = 0; guard < 10'000; ++guard) {
    redisReply* raw = nullptr;
    if (redisGetReply(ctx, reinterpret_cast<void**>(&raw)) != REDIS_OK) {
      return false;
    }
    Reply reply{raw};
    if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 3 ||
        reply->element[0]->type != REDIS_REPLY_STRING) {
      continue;
    }
    const std::string_view kind(reply->element[0]->str, reply->element[0]->len);
    if (kind == "message" || kind == "pmessage") {
      if (on_msg && reply->elements >= 3 &&
          reply->element[1]->type == REDIS_REPLY_STRING &&
          reply->element[2]->type == REDIS_REPLY_STRING) {
        on_msg(std::string(reply->element[1]->str, reply->element[1]->len),
               std::string(reply->element[2]->str, reply->element[2]->len));
      }
      continue;
    }
    if (kind == "subscribe" || kind == "unsubscribe") {
      return true;
    }
  }
  return false;
}

bool is_transient_io(redisContext* ctx) {
  if (ctx == nullptr || ctx->err != REDIS_ERR_IO) {
    return false;
  }
  return errno == EAGAIN || errno == EWOULDBLOCK || errno == ETIMEDOUT;
}

}  // namespace

RedisMux::RedisMux(std::string uds_path, MessageHandler on_message,
                   StatusHandler on_status)
    : uds_path_(std::move(uds_path)),
      on_message_(std::move(on_message)),
      on_status_(std::move(on_status)) {}

RedisMux::~RedisMux() { stop(); }

void RedisMux::start() {
  if (running_.exchange(true)) {
    return;
  }
  thr_ = std::thread([this] { run(); });
}

void RedisMux::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (thr_.joinable()) {
    thr_.join();
  }
}

void RedisMux::set_channels(std::set<std::string> channels) {
  {
    std::lock_guard lock(mu_);
    if (channels == desired_) {
      return;
    }
    desired_ = std::move(channels);
  }
  desired_dirty_.store(true, std::memory_order_release);
}

bool RedisMux::publish(const std::string& uds_path, const std::string& channel,
                       const std::string& payload) {
  if (uds_path.empty() || channel.empty()) {
    return false;
  }
  try {
    auto ctx = connect_unix(uds_path);
    if (!ctx) {
      return false;
    }
    Reply reply{static_cast<redisReply*>(
        redisCommand(ctx.get(), "PUBLISH %b %b", channel.data(), channel.size(),
                     payload.data(), payload.size()))};
    return reply != nullptr && reply->type != REDIS_REPLY_ERROR;
  } catch (...) {
    return false;
  }
}

void RedisMux::run() {
  int attempt = 0;
  while (running_) {
    if (consume_once()) {
      attempt = 0;
      // Clean exit = channel set changed → resubscribe immediately.
      continue;
    }
    ++attempt;
    if (!running_) {
      break;
    }
    if (on_status_) {
      on_status_(false, "redis reconnecting…");
    }
    const auto delay = std::min(std::chrono::milliseconds{5'000},
                                std::chrono::milliseconds{200} *
                                    (1 << std::min(attempt, 4)));
    std::this_thread::sleep_for(delay);
  }
  if (on_status_) {
    on_status_(false, "stopped");
  }
}

bool RedisMux::consume_once() {
  auto ctx = connect_unix(uds_path_);
  if (!ctx) {
    if (on_status_) {
      on_status_(false, "UDS connect failed: " + uds_path_);
    }
    std::cerr << "hyperion: redis UDS connect failed: " << uds_path_ << "\n";
    return false;
  }

  std::set<std::string> live;
  {
    std::lock_guard lock(mu_);
    live = desired_;
  }

  auto deliver = [this](const std::string& ch, const std::string& payload) {
    if (on_message_) {
      on_message_(ch, payload);
    }
  };

  for (const auto& ch : live) {
    if (!issue_subscribe(ctx.get(), ch, true, deliver)) {
      if (on_status_) {
        on_status_(false, "SUBSCRIBE failed: " + ch);
      }
      std::cerr << "hyperion: SUBSCRIBE failed: " << ch << "\n";
      return false;
    }
  }
  desired_dirty_.store(false, std::memory_order_release);
  if (on_status_) {
    on_status_(true, "subscribed " + std::to_string(live.size()) + " channels");
  }

  pollfd pfd{.fd = ctx->fd, .events = POLLIN, .revents = 0};
  while (running_) {
    // Channel set changed: reconnect with a clean SUBSCRIBE set.
    // (Mid-stream SUBSCRIBE races with message traffic on goblin-core.)
    if (desired_dirty_.load(std::memory_order_acquire)) {
      return true;
    }

    const int ready = ::poll(&pfd, 1, 250);
    if (ready < 0) {
      if (errno == EINTR) {
        continue;
      }
      if (on_status_) {
        on_status_(false, "poll failed");
      }
      return false;
    }
    if (ready == 0) {
      continue;
    }
    if ((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0) {
      if (on_status_) {
        on_status_(false, "connection closed");
      }
      return false;
    }
    if (redisBufferRead(ctx.get()) != REDIS_OK) {
      if (is_transient_io(ctx.get())) {
        ctx->err = 0;
        ctx->errstr[0] = '\0';
        continue;
      }
      if (on_status_) {
        on_status_(false, ctx->errstr ? ctx->errstr : "read error");
      }
      return false;
    }

    while (true) {
      void* raw = nullptr;
      if (redisGetReplyFromReader(ctx.get(), &raw) != REDIS_OK) {
        if (is_transient_io(ctx.get())) {
          ctx->err = 0;
          ctx->errstr[0] = '\0';
          break;
        }
        if (on_status_) {
          on_status_(false, ctx->errstr ? ctx->errstr : "reply error");
        }
        return false;
      }
      if (raw == nullptr) {
        break;
      }
      Reply reply{static_cast<redisReply*>(raw)};
      if (reply->type != REDIS_REPLY_ARRAY || reply->elements < 3) {
        continue;
      }
      const auto* kind = reply->element[0];
      const auto* channel = reply->element[1];
      const auto* payload = reply->element[2];
      if (kind == nullptr || channel == nullptr || payload == nullptr ||
          kind->str == nullptr || channel->str == nullptr ||
          payload->str == nullptr) {
        continue;
      }
      if (std::string_view(kind->str, kind->len) != "message") {
        continue;
      }
      deliver(std::string(channel->str, channel->len),
              std::string(payload->str, payload->len));
    }
  }
  return true;
}

}  // namespace hyperion
