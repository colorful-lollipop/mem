#include "memrpc/server.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include "core/protocol.h"
#include "core/session.h"

namespace memrpc {

namespace {

uint32_t MonotonicNowMs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count() & 0xffffffffu);
}

class WorkerPool {
 public:
  void Start(uint32_t thread_count, std::function<void(const RequestRingEntry&)> callback) {
    callback_ = std::move(callback);
    running_ = true;
    for (uint32_t i = 0; i < thread_count; ++i) {
      workers_.emplace_back([this] { WorkerLoop(); });
    }
  }

  void Enqueue(const RequestRingEntry& entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.push(entry);
    cv_.notify_one();
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      running_ = false;
    }
    cv_.notify_all();
    for (auto& worker : workers_) {
      if (worker.joinable()) {
        worker.join();
      }
    }
    workers_.clear();
  }

 private:
  void WorkerLoop() {
    while (true) {
      RequestRingEntry entry;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_.wait(lock, [this] { return !running_ || !queue_.empty(); });
        if (!running_ && queue_.empty()) {
          return;
        }
        entry = queue_.front();
        queue_.pop();
      }
      callback_(entry);
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::queue<RequestRingEntry> queue_;
  std::vector<std::thread> workers_;
  std::function<void(const RequestRingEntry&)> callback_;
  bool running_ = false;
};

}  // namespace

struct EngineServer::Impl {
  BootstrapHandles handles{};
  std::shared_ptr<IScanHandler> handler;
  ServerOptions options{};
  Session session;
  WorkerPool high_pool;
  WorkerPool normal_pool;
  std::thread dispatcher_thread;
  std::atomic<bool> running{false};

  void WriteResponse(const RequestRingEntry& request_entry, const ScanResult& result) {
    SlotPayload* payload = session.slot_payload(request_entry.slot_index);
    if (payload == nullptr) {
      return;
    }
    payload->status_code = static_cast<uint32_t>(result.status);
    payload->verdict = static_cast<uint32_t>(result.verdict);
    payload->engine_code = result.engine_code;
    payload->detail_code = result.detail_code;
    payload->message_length = std::min<uint32_t>(kMaxMessageSize - 1,
                                                 static_cast<uint32_t>(result.message.size()));
    std::memset(payload->message, 0, sizeof(payload->message));
    std::memcpy(payload->message, result.message.data(), payload->message_length);

    ResponseRingEntry response;
    response.request_id = request_entry.request_id;
    response.slot_index = request_entry.slot_index;
    response.status_code = static_cast<uint32_t>(result.status);
    response.result_size = payload->message_length;
    response.engine_errno = result.engine_code;
    if (session.PushResponse(response) == StatusCode::Ok) {
      const uint64_t signal_value = 1;
      write(session.handles().resp_event_fd, &signal_value, sizeof(signal_value));
    }
  }

  void ProcessEntry(const RequestRingEntry& request_entry) {
    SlotPayload* payload = session.slot_payload(request_entry.slot_index);
    if (payload == nullptr || handler == nullptr) {
      return;
    }

    ScanResult result;
    const uint32_t now_ms = MonotonicNowMs();
    if (payload->queue_timeout_ms > 0 &&
        now_ms - request_entry.enqueue_mono_ms > payload->queue_timeout_ms) {
      result.status = StatusCode::QueueTimeout;
      result.verdict = ScanVerdict::Error;
      result.message = "queue timeout";
      WriteResponse(request_entry, result);
      return;
    }

    ScanRequest request;
    if (payload->file_path_length >= kMaxFilePathSize) {
      result.status = StatusCode::ProtocolMismatch;
      result.verdict = ScanVerdict::Error;
      result.message = "invalid request payload";
      WriteResponse(request_entry, result);
      return;
    }
    request.file_path.assign(payload->file_path, payload->file_path_length);
    request.options.priority =
        payload->priority == static_cast<uint32_t>(Priority::High) ? Priority::High
                                                                    : Priority::Normal;
    request.options.queue_timeout_ms = payload->queue_timeout_ms;
    request.options.exec_timeout_ms = payload->exec_timeout_ms;
    request.options.flags = payload->flags;

    const auto start = std::chrono::steady_clock::now();
    result = handler->HandleScan(request);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (payload->exec_timeout_ms > 0 &&
        elapsed > static_cast<long long>(payload->exec_timeout_ms)) {
      result.status = StatusCode::ExecTimeout;
      result.verdict = ScanVerdict::Error;
      if (result.message.empty()) {
        result.message = "execution timeout";
      }
    }
    WriteResponse(request_entry, result);
  }

  bool DrainQueue(QueueKind kind, WorkerPool* pool) {
    bool drained = false;
    RequestRingEntry entry;
    while (session.PopRequest(kind, &entry)) {
      drained = true;
      pool->Enqueue(entry);
    }
    return drained;
  }

  void DispatcherLoop() {
    pollfd fds[2] = {
        {session.handles().high_req_event_fd, POLLIN, 0},
        {session.handles().normal_req_event_fd, POLLIN, 0},
    };

    while (running.load()) {
      const int poll_result = poll(fds, 2, 100);
      if (poll_result <= 0) {
        continue;
      }

      uint64_t counter = 0;
      bool high_work = false;
      if ((fds[0].revents & POLLIN) != 0) {
        while (read(fds[0].fd, &counter, sizeof(counter)) == sizeof(counter)) {
        }
        high_work = DrainQueue(QueueKind::HighRequest, &high_pool);
      }
      if (!high_work && (fds[1].revents & POLLIN) != 0) {
        while (read(fds[1].fd, &counter, sizeof(counter)) == sizeof(counter)) {
        }
        DrainQueue(QueueKind::NormalRequest, &normal_pool);
      }
    }
  }
};

EngineServer::EngineServer() : impl_(std::make_unique<Impl>()) {}

EngineServer::EngineServer(BootstrapHandles handles,
                           std::shared_ptr<IScanHandler> handler,
                           ServerOptions options)
    : impl_(std::make_unique<Impl>()) {
  impl_->handles = handles;
  impl_->handler = std::move(handler);
  impl_->options = options;
}

EngineServer::~EngineServer() {
  Stop();
}

void EngineServer::SetBootstrapHandles(BootstrapHandles handles) {
  impl_->handles = handles;
}

void EngineServer::SetScanHandler(std::shared_ptr<IScanHandler> handler) {
  impl_->handler = std::move(handler);
}

void EngineServer::SetOptions(ServerOptions options) {
  impl_->options = options;
}

StatusCode EngineServer::Start() {
  if (impl_->handler == nullptr) {
    return StatusCode::InvalidArgument;
  }
  const StatusCode attach_status = impl_->session.Attach(impl_->handles);
  if (attach_status != StatusCode::Ok) {
    return attach_status;
  }
  impl_->running.store(true);
  impl_->high_pool.Start(std::max(1u, impl_->options.high_worker_threads),
                         [this](const RequestRingEntry& entry) {
                           impl_->ProcessEntry(entry);
                         });
  impl_->normal_pool.Start(std::max(1u, impl_->options.normal_worker_threads),
                           [this](const RequestRingEntry& entry) {
                             impl_->ProcessEntry(entry);
                           });
  impl_->dispatcher_thread = std::thread([this] { impl_->DispatcherLoop(); });
  return StatusCode::Ok;
}

void EngineServer::Run() {
  if (!impl_->running.load()) {
    if (Start() != StatusCode::Ok) {
      return;
    }
  }
  while (impl_->running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void EngineServer::Stop() {
  impl_->running.store(false);
  if (impl_->dispatcher_thread.joinable()) {
    impl_->dispatcher_thread.join();
  }
  impl_->high_pool.Stop();
  impl_->normal_pool.Stop();
  impl_->session.Reset();
}

}  // namespace memrpc
