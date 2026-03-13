#ifndef INCLUDE_VPSDEMO_TESTKIT_TESTKIT_FAILURE_MONITOR_H_
#define INCLUDE_VPSDEMO_TESTKIT_TESTKIT_FAILURE_MONITOR_H_

#include <chrono>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

#include "memrpc/client/rpc_client.h"

namespace vpsdemo::testkit {

class FailureMonitor {
 public:
    struct Options {
        uint32_t windowMs = 60000;
        uint32_t execTimeoutThreshold = 3;
        bool triggerOnDisconnect = true;
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
        execTimeouts_.clear();
    }

    void OnFailure(const memrpc::RpcFailure& failure) {
        ThresholdCallback callback;
        bool fire = false;
        if (failure.status == memrpc::StatusCode::PeerDisconnected ||
            failure.status == memrpc::StatusCode::ProtocolMismatch) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (options_.triggerOnDisconnect && callback_) {
                callback = callback_;
                fire = true;
            }
        } else if (failure.status == memrpc::StatusCode::ExecTimeout) {
            const auto now = std::chrono::steady_clock::now();
            std::lock_guard<std::mutex> lock(mutex_);
            execTimeouts_.push_back(now);
            while (!execTimeouts_.empty() &&
                   std::chrono::duration_cast<std::chrono::milliseconds>(
                       now - execTimeouts_.front()).count() > options_.windowMs) {
                execTimeouts_.pop_front();
            }
            if (execTimeouts_.size() >= options_.execTimeoutThreshold && callback_) {
                callback = callback_;
                fire = true;
                execTimeouts_.clear();
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
    std::deque<std::chrono::steady_clock::time_point> execTimeouts_;
};

}  // namespace vpsdemo::testkit

#endif  // INCLUDE_VPSDEMO_TESTKIT_TESTKIT_FAILURE_MONITOR_H_
