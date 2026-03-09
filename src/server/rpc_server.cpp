#include "memrpc/server/rpc_server.h"

#include <algorithm>
#include <poll.h>
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
#include <unordered_map>
#include <vector>

#include "core/protocol.h"
#include "core/session.h"
#include "virus_protection_service_log.h"

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

struct RpcServer::Impl {
  BootstrapHandles handles{};
  ServerOptions options{};
  Session session;
  WorkerPool high_pool;
  WorkerPool normal_pool;
  std::thread dispatcher_thread;
  std::atomic<bool> running{false};
  std::unordered_map<uint16_t, RpcHandler> handlers;

  void WriteResponse(const RequestRingEntry& request_entry, const RpcServerReply& reply) {
    if (session.header() == nullptr) {
      return;
    }

    ResponseRingEntry response;
    response.request_id = request_entry.request_id;
    response.slot_index = request_entry.slot_index;
    response.status_code = static_cast<uint32_t>(reply.status);
    response.engine_errno = reply.engine_code;
    response.detail_code = reply.detail_code;
    response.result_size = static_cast<uint32_t>(reply.payload.size());
    if (response.result_size > session.header()->max_response_bytes) {
      response.status_code = static_cast<uint32_t>(StatusCode::EngineInternalError);
      response.engine_errno = 0;
      response.detail_code = 0;
      response.result_size = 0;
    } else if (response.result_size != 0) {
      std::memcpy(response.payload, reply.payload.data(), response.result_size);
    }
    if (session.PushResponse(response) == StatusCode::Ok) {
      const uint64_t signal_value = 1;
      write(session.handles().resp_event_fd, &signal_value, sizeof(signal_value));
    }
  }

  StatusCode PublishEvent(const RpcEvent& event) {
    if (!session.valid() || session.handles().resp_event_fd < 0 || session.header() == nullptr) {
      HLOGE("PublishEvent failed, session is not ready");
      return StatusCode::EngineInternalError;
    }
    if (event.payload.size() > session.header()->max_response_bytes) {
      HLOGW("PublishEvent payload too large, size=%{public}zu", event.payload.size());
      return StatusCode::InvalidArgument;
    }

    ResponseRingEntry entry;
    entry.message_kind = ResponseMessageKind::Event;
    entry.event_domain = event.event_domain;
    entry.event_type = event.event_type;
    entry.flags = event.flags;
    entry.result_size = static_cast<uint32_t>(event.payload.size());
    if (!event.payload.empty()) {
      std::memcpy(entry.payload, event.payload.data(), event.payload.size());
    }

    const StatusCode status = session.PushResponse(entry);
    if (status != StatusCode::Ok) {
      HLOGW("PushResponse for event failed, status=%{public}d", static_cast<int>(status));
      return status;
    }

    const uint64_t signal_value = 1;
    if (write(session.handles().resp_event_fd, &signal_value, sizeof(signal_value)) !=
        sizeof(signal_value)) {
      HLOGE("eventfd write failed while publishing event");
      return StatusCode::PeerDisconnected;
    }
    return StatusCode::Ok;
  }

  void ProcessEntry(const RequestRingEntry& request_entry) {
    SlotPayload* payload = session.slot_payload(request_entry.slot_index);
    uint8_t* request_bytes = session.slot_request_bytes(request_entry.slot_index);
    if (payload == nullptr || request_bytes == nullptr || session.header() == nullptr) {
      return;
    }

    RpcServerReply reply;
    const uint32_t now_ms = MonotonicNowMs();
    if (payload->request.queue_timeout_ms > 0 &&
        now_ms - request_entry.enqueue_mono_ms > payload->request.queue_timeout_ms) {
      reply.status = StatusCode::QueueTimeout;
      WriteResponse(request_entry, reply);
      return;
    }

    if (payload->request.payload_size > session.header()->max_request_bytes) {
      reply.status = StatusCode::ProtocolMismatch;
      WriteResponse(request_entry, reply);
      return;
    }

    RpcServerCall call;
    call.opcode = static_cast<Opcode>(request_entry.opcode);
    call.priority = payload->request.priority == static_cast<uint32_t>(Priority::High)
                        ? Priority::High
                        : Priority::Normal;
    call.queue_timeout_ms = payload->request.queue_timeout_ms;
    call.exec_timeout_ms = payload->request.exec_timeout_ms;
    call.flags = payload->request.flags;
    call.payload.assign(request_bytes, request_bytes + payload->request.payload_size);

    const auto it = handlers.find(request_entry.opcode);
    if (it == handlers.end()) {
      reply.status = StatusCode::InvalidArgument;
      WriteResponse(request_entry, reply);
      return;
    }

    const auto start = std::chrono::steady_clock::now();
    it->second(call, &reply);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - start)
                             .count();
    if (payload->request.exec_timeout_ms > 0 &&
        elapsed > static_cast<long long>(payload->request.exec_timeout_ms)) {
      reply.status = StatusCode::ExecTimeout;
    }
    WriteResponse(request_entry, reply);
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

RpcServer::RpcServer() : impl_(std::make_unique<Impl>()) {}

RpcServer::RpcServer(BootstrapHandles handles, ServerOptions options)
    : impl_(std::make_unique<Impl>()) {
  impl_->handles = handles;
  impl_->options = options;
}

RpcServer::~RpcServer() {
  Stop();
}

void RpcServer::SetBootstrapHandles(BootstrapHandles handles) {
  impl_->handles = handles;
}

void RpcServer::RegisterHandler(Opcode opcode, RpcHandler handler) {
  if (!handler) {
    impl_->handlers.erase(static_cast<uint16_t>(opcode));
    return;
  }
  impl_->handlers[static_cast<uint16_t>(opcode)] = std::move(handler);
}

void RpcServer::SetOptions(ServerOptions options) {
  impl_->options = options;
}

StatusCode RpcServer::PublishEvent(const RpcEvent& event) {
  return impl_->PublishEvent(event);
}

StatusCode RpcServer::Start() {
  if (impl_->handlers.empty()) {
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

void RpcServer::Run() {
  if (!impl_->running.load()) {
    if (Start() != StatusCode::Ok) {
      return;
    }
  }
  while (impl_->running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
}

void RpcServer::Stop() {
  impl_->running.store(false);
  if (impl_->dispatcher_thread.joinable()) {
    impl_->dispatcher_thread.join();
  }
  impl_->high_pool.Stop();
  impl_->normal_pool.Stop();
  impl_->session.Reset();
}

}  // namespace memrpc
