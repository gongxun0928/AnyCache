#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

#include <grpcpp/grpcpp.h>

namespace anycache {

// ChannelPool caches gRPC Channels by target address.
//
// gRPC Channel and Stub are thread-safe: a single Channel multiplexes
// concurrent RPCs over one HTTP/2 connection. This pool ensures we
// create at most one Channel per remote address, avoiding the cost of
// repeated TCP handshakes and HTTP/2 negotiation.
//
// Health checking: on each GetChannel() call, the pool inspects the
// cached Channel's connectivity state. If the channel has entered
// SHUTDOWN (terminal — never recovers) it is discarded and a fresh
// channel is created. For TRANSIENT_FAILURE the channel is kept
// because gRPC's built-in back-off will automatically attempt to
// reconnect; however, callers may force eviction via RemoveChannel().
//
// Thread-safe: all methods can be called concurrently.
class ChannelPool {
public:
  ChannelPool() = default;

  // Get (or create) a healthy Channel to the given address.
  //
  // If a cached Channel exists and is still usable (not SHUTDOWN),
  // it is returned directly. If the Channel has entered SHUTDOWN,
  // it is evicted and a new one is created transparently.
  std::shared_ptr<grpc::Channel> GetChannel(const std::string &address) {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = channels_.find(address);
    if (it != channels_.end()) {
      auto state = it->second->GetState(/*try_to_connect=*/false);
      if (state == GRPC_CHANNEL_SHUTDOWN) {
        // SHUTDOWN is terminal — the channel will never recover.
        // Evict it and fall through to create a fresh one.
        channels_.erase(it);
      } else {
        // IDLE / CONNECTING / READY / TRANSIENT_FAILURE are all
        // recoverable; gRPC handles reconnection internally.
        return it->second;
      }
    }

    auto channel = grpc::CreateCustomChannel(
        address, grpc::InsecureChannelCredentials(), MakeChannelArgs());
    channels_[address] = channel;
    return channel;
  }

  // Remove a cached Channel (e.g., when a worker is known to be down,
  // or after repeated TRANSIENT_FAILURE).
  void RemoveChannel(const std::string &address) {
    std::lock_guard<std::mutex> lock(mu_);
    channels_.erase(address);
  }

  // Number of cached Channels.
  size_t Size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return channels_.size();
  }

  // Clear all cached Channels.
  void Clear() {
    std::lock_guard<std::mutex> lock(mu_);
    channels_.clear();
  }

private:
  static grpc::ChannelArguments MakeChannelArgs() {
    grpc::ChannelArguments args;
    // ─── Keep-alive ──────────────────────────────────────────
    // Send pings every 30 s to detect dead connections early.
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 30000);
    // Wait up to 10 s for a ping ACK before considering dead.
    args.SetInt(GRPC_ARG_KEEPALIVE_TIMEOUT_MS, 10000);
    // Allow pings even when there are no active RPCs so idle
    // connections are checked too.
    args.SetInt(GRPC_ARG_KEEPALIVE_PERMIT_WITHOUT_CALLS, 1);

    // ─── Message size ────────────────────────────────────────
    // 128 MB — large enough for full-block transfers.
    args.SetMaxReceiveMessageSize(128 * 1024 * 1024);
    args.SetMaxSendMessageSize(128 * 1024 * 1024);

    // ─── Reconnection back-off ───────────────────────────────
    // Initial back-off 100 ms, max 5 s (defaults are 1 s / 120 s).
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 100);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 5000);

    return args;
  }

  mutable std::mutex mu_;
  std::unordered_map<std::string, std::shared_ptr<grpc::Channel>> channels_;
};

} // namespace anycache
