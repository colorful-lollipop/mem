#ifndef APPS_MINIRPC_PARENT_MINIRPC_FAILURE_MONITOR_H_
#define APPS_MINIRPC_PARENT_MINIRPC_FAILURE_MONITOR_H_

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>

#include "memrpc/client/rpc_client.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {

class FailureMonitor {
 public:
  struct Options {
    uint32_t window_ms = 60000;
    uint32_t exec_timeout_threshold = 3;
    bool trigger_on_disconnect = true;
  };

  using ThresholdCallback = std::function<void()>;

  FailureMonitor() = default;

  explicit FailureMonitor(Options options, ThresholdCallback callback = nullptr)
      : options_(options), callback_(std::move(callback)) {}

  void SetThresholdCallback(ThresholdCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    callback_ = std::move(callback);
  }

  void Reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    exec_timeouts_.clear();
  }

  void OnFailure(const MemRpc::RpcFailure& failure) {
    ThresholdCallback callback;
    bool fire = false;
    if (failure.status == MemRpc::StatusCode::PeerDisconnected ||
        failure.status == MemRpc::StatusCode::ProtocolMismatch) {
      std::lock_guard<std::mutex> lock(mutex_);
      if (options_.trigger_on_disconnect && callback_) {
        callback = callback_;
        fire = true;
      }
    } else if (failure.status == MemRpc::StatusCode::ExecTimeout) {
      const auto now = std::chrono::steady_clock::now();
      std::lock_guard<std::mutex> lock(mutex_);
      exec_timeouts_.push_back(now);
      while (!exec_timeouts_.empty() &&
             std::chrono::duration_cast<std::chrono::milliseconds>(now - exec_timeouts_.front()).count() >
                 options_.window_ms) {
        exec_timeouts_.pop_front();
      }
      if (exec_timeouts_.size() >= options_.exec_timeout_threshold && callback_) {
        callback = callback_;
        fire = true;
        exec_timeouts_.clear();
      }
    }
    if (fire && callback) {
      callback();
    }
  }

 private:
  Options options_;
  ThresholdCallback callback_;
  std::mutex mutex_;
  std::deque<std::chrono::steady_clock::time_point> exec_timeouts_;
};

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc

#endif  // APPS_MINIRPC_PARENT_MINIRPC_FAILURE_MONITOR_H_
