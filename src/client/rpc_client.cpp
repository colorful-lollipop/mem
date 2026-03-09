#include "memrpc/client/rpc_client.h"

#include <poll.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <utility>

#include "core/session.h"
#include "core/slot_pool.h"
#include "virus_protection_service_log.h"

namespace memrpc {

namespace {

uint32_t MonotonicNowMs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count() & 0xffffffffu);
}

}  // namespace

struct RpcFuture::State {
  mutable std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  bool abandoned = false;
  RpcReply reply;
};

RpcFuture::RpcFuture() = default;

RpcFuture::RpcFuture(std::shared_ptr<State> state) : state_(std::move(state)) {}

RpcFuture::~RpcFuture() = default;

bool RpcFuture::IsReady() const {
  if (state_ == nullptr) {
    return false;
  }
  std::lock_guard<std::mutex> lock(state_->mutex);
  return state_->ready;
}

StatusCode RpcFuture::Wait(RpcReply* reply) {
  if (state_ == nullptr || reply == nullptr) {
    return StatusCode::InvalidArgument;
  }
  std::unique_lock<std::mutex> lock(state_->mutex);
  state_->cv.wait(lock, [this] { return state_->ready; });
  *reply = state_->reply;
  return reply->status;
}

struct RpcClient::Impl {
  explicit Impl(std::shared_ptr<IBootstrapChannel> bootstrap_channel)
      : bootstrap(std::move(bootstrap_channel)) {}

  std::shared_ptr<IBootstrapChannel> bootstrap;
  Session session;
  std::unique_ptr<SlotPool> slot_pool;
  std::mutex reconnect_mutex;
  std::mutex session_mutex;
  std::mutex pending_mutex;
  std::unordered_map<uint64_t, std::shared_ptr<RpcFuture::State>> pending_calls;
  std::thread dispatcher_thread;
  std::atomic<bool> dispatcher_running{false};
  std::atomic<bool> shutting_down{false};
  std::atomic<uint64_t> next_request_id{1};
  std::mutex event_mutex;
  RpcEventCallback event_callback;
  uint64_t current_session_id = 0;
  bool session_dead = true;

  RpcFuture MakeReadyFuture(StatusCode status) {
    auto state = std::make_shared<RpcFuture::State>();
    state->ready = true;
    state->reply.status = status;
    return RpcFuture(std::move(state));
  }

  void StopDispatcher() {
    dispatcher_running.store(false);
    if (dispatcher_thread.joinable()) {
      dispatcher_thread.join();
    }
  }

  void StartDispatcher() {
    dispatcher_running.store(true);
    dispatcher_thread = std::thread([this] { ResponseLoop(); });
  }

  void FailAllPending(StatusCode status_code) {
    std::unordered_map<uint64_t, std::shared_ptr<RpcFuture::State>> pending_calls;
    {
      std::lock_guard<std::mutex> lock(pending_mutex);
      pending_calls.swap(this->pending_calls);
    }

    for (auto& [_, pending] : pending_calls) {
      std::lock_guard<std::mutex> lock(pending->mutex);
      pending->reply.status = status_code;
      pending->ready = true;
      pending->cv.notify_one();
    }
  }

  void HandleEngineDeath(uint64_t dead_session_id) {
    if (shutting_down.load()) {
      return;
    }
    std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex);
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      if (current_session_id == 0 || dead_session_id != current_session_id) {
        return;
      }
    }
    HLOGW("session died, session_id=%{public}llu",
          static_cast<unsigned long long>(dead_session_id));
    StopDispatcher();
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      session_dead = true;
      current_session_id = 0;
      session.Reset();
      slot_pool.reset();
    }
    FailAllPending(StatusCode::PeerDisconnected);
  }

  StatusCode EnsureLiveSession() {
    std::lock_guard<std::mutex> reconnect_lock(reconnect_mutex);
    {
      std::lock_guard<std::mutex> lock(session_mutex);
      if (!session_dead && session.valid() && slot_pool != nullptr) {
        return StatusCode::Ok;
      }
    }

    if (bootstrap == nullptr) {
      return StatusCode::InvalidArgument;
    }
    const StatusCode start_status = bootstrap->StartEngine();
    if (start_status != StatusCode::Ok) {
      HLOGE("StartEngine failed, status=%{public}d", static_cast<int>(start_status));
      return start_status;
    }

    BootstrapHandles handles;
    const StatusCode connect_status = bootstrap->Connect(&handles);
    if (connect_status != StatusCode::Ok) {
      HLOGE("Connect failed, status=%{public}d", static_cast<int>(connect_status));
      return connect_status;
    }

    StopDispatcher();
    std::lock_guard<std::mutex> lock(session_mutex);
    session_dead = true;
    current_session_id = 0;
    session.Reset();
    slot_pool.reset();

    const StatusCode attach_status = session.Attach(handles);
    if (attach_status != StatusCode::Ok) {
      HLOGE("Session attach failed, status=%{public}d", static_cast<int>(attach_status));
      return attach_status;
    }
    slot_pool = std::make_unique<SlotPool>(session.header()->slot_count);
    current_session_id = handles.session_id;
    session_dead = false;
    StartDispatcher();
    return StatusCode::Ok;
  }

  void CompleteRequest(const ResponseRingEntry& entry) {
    if (slot_pool == nullptr) {
      return;
    }

    RpcReply reply;
    reply.status = static_cast<StatusCode>(entry.status_code);
    reply.engine_code = entry.engine_errno;
    reply.detail_code = entry.detail_code;
    if (session.header() == nullptr || entry.result_size > session.header()->max_response_bytes) {
      reply.status = StatusCode::ProtocolMismatch;
    } else {
      reply.payload.assign(entry.payload, entry.payload + entry.result_size);
    }

    std::shared_ptr<RpcFuture::State> pending;
    {
      std::lock_guard<std::mutex> lock(pending_mutex);
      auto it = pending_calls.find(entry.request_id);
      if (it != pending_calls.end()) {
        pending = it->second;
      }
    }
    if (pending != nullptr) {
      std::lock_guard<std::mutex> lock(pending->mutex);
      if (!pending->abandoned) {
        pending->reply = std::move(reply);
        pending->ready = true;
        pending->cv.notify_one();
      }
    }
    slot_pool->Release(entry.slot_index);
    std::lock_guard<std::mutex> lock(pending_mutex);
    pending_calls.erase(entry.request_id);
  }

  void DeliverEvent(const ResponseRingEntry& entry) {
    RpcEvent event;
    event.event_domain = entry.event_domain;
    event.event_type = entry.event_type;
    event.flags = entry.flags;
    if (session.header() == nullptr || entry.result_size > session.header()->max_response_bytes) {
      HLOGW("drop invalid event, size=%{public}u", entry.result_size);
      return;
    }
    event.payload.assign(entry.payload, entry.payload + entry.result_size);

    RpcEventCallback callback;
    {
      std::lock_guard<std::mutex> lock(event_mutex);
      callback = event_callback;
    }
    if (callback) {
      callback(event);
    }
  }

  void ResponseLoop() {
    const int resp_fd = session.handles().resp_event_fd;
    if (resp_fd < 0) {
      return;
    }
    pollfd fd{resp_fd, POLLIN, 0};
    while (dispatcher_running.load()) {
      const int poll_result = poll(&fd, 1, 100);
      if (poll_result <= 0) {
        continue;
      }
      if ((fd.revents & POLLIN) == 0) {
        continue;
      }
      uint64_t counter = 0;
      while (read(fd.fd, &counter, sizeof(counter)) == sizeof(counter)) {
      }
      ResponseRingEntry entry;
      while (session.PopResponse(&entry)) {
        if (entry.message_kind == ResponseMessageKind::Event) {
          DeliverEvent(entry);
          continue;
        }
        CompleteRequest(entry);
      }
    }
  }

  RpcFuture InvokeAsync(const RpcCall& call) {
    for (int attempt = 0; attempt < 2; ++attempt) {
      const StatusCode ensure_status = EnsureLiveSession();
      if (ensure_status != StatusCode::Ok) {
        return this->MakeReadyFuture(ensure_status);
      }

      std::shared_ptr<RpcFuture::State> pending = std::make_shared<RpcFuture::State>();
      {
        std::lock_guard<std::mutex> session_lock(session_mutex);
        if (session_dead || slot_pool == nullptr || !session.valid()) {
          continue;
        }

        const auto slot = slot_pool->Reserve();
        if (!slot.has_value()) {
          return this->MakeReadyFuture(StatusCode::QueueFull);
        }
        SlotPayload* payload = session.slot_payload(*slot);
        uint8_t* request_payload = session.slot_request_bytes(*slot);
        if (payload == nullptr || request_payload == nullptr) {
          slot_pool->Release(*slot);
          return this->MakeReadyFuture(StatusCode::EngineInternalError);
        }
        if (call.payload.size() > session.header()->max_request_bytes) {
          slot_pool->Release(*slot);
          return this->MakeReadyFuture(StatusCode::InvalidArgument);
        }

        std::memset(payload, 0, sizeof(SlotPayload));
        payload->request.queue_timeout_ms = call.queue_timeout_ms;
        payload->request.exec_timeout_ms = call.exec_timeout_ms;
        payload->request.flags = call.flags;
        payload->request.priority = static_cast<uint32_t>(call.priority);
        payload->request.opcode = static_cast<uint16_t>(call.opcode);
        payload->request.payload_size = static_cast<uint32_t>(call.payload.size());
        std::memcpy(request_payload, call.payload.data(), call.payload.size());

        const uint64_t request_id = next_request_id.fetch_add(1);
        RequestRingEntry entry;
        entry.request_id = request_id;
        entry.slot_index = *slot;
        entry.opcode = static_cast<uint16_t>(call.opcode);
        entry.flags = static_cast<uint16_t>(call.flags);
        entry.enqueue_mono_ms = MonotonicNowMs();
        entry.payload_size = payload->request.payload_size;

        {
          std::lock_guard<std::mutex> pending_lock(pending_mutex);
          pending_calls.emplace(request_id, pending);
        }

        const bool high_priority = call.priority == Priority::High;
        slot_pool->Transition(
            *slot, high_priority ? SlotState::QueuedHigh : SlotState::QueuedNormal);
        const StatusCode queue_status =
            session.PushRequest(high_priority ? QueueKind::HighRequest : QueueKind::NormalRequest,
                                entry);
        if (queue_status != StatusCode::Ok) {
          std::lock_guard<std::mutex> pending_lock(pending_mutex);
          pending_calls.erase(request_id);
          slot_pool->Release(*slot);
          if (queue_status == StatusCode::PeerDisconnected && attempt == 0) {
            continue;
          }
          return this->MakeReadyFuture(queue_status);
        }

        const uint64_t signal_value = 1;
        const int req_fd =
            high_priority ? session.handles().high_req_event_fd : session.handles().normal_req_event_fd;
        if (write(req_fd, &signal_value, sizeof(signal_value)) != sizeof(signal_value)) {
          std::lock_guard<std::mutex> pending_lock(pending_mutex);
          pending_calls.erase(request_id);
          slot_pool->Release(*slot);
          return this->MakeReadyFuture(StatusCode::PeerDisconnected);
        }
      }

      return RpcFuture(std::move(pending));
    }

    return this->MakeReadyFuture(StatusCode::PeerDisconnected);
  }
};

RpcClient::RpcClient(std::shared_ptr<IBootstrapChannel> bootstrap)
    : impl_(std::make_unique<Impl>(std::move(bootstrap))) {}

RpcClient::~RpcClient() {
  Shutdown();
}

void RpcClient::SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap) {
  impl_->bootstrap = std::move(bootstrap);
}

void RpcClient::SetEventCallback(RpcEventCallback callback) {
  std::lock_guard<std::mutex> lock(impl_->event_mutex);
  impl_->event_callback = std::move(callback);
}

StatusCode RpcClient::Init() {
  if (impl_->bootstrap == nullptr) {
    return StatusCode::InvalidArgument;
  }
  impl_->bootstrap->SetEngineDeathCallback(
      [this](uint64_t session_id) { impl_->HandleEngineDeath(session_id); });
  return impl_->EnsureLiveSession();
}

RpcFuture RpcClient::InvokeAsync(const RpcCall& call) {
  return impl_->InvokeAsync(call);
}

StatusCode RpcClient::InvokeSync(const RpcCall& call, RpcReply* reply) {
  if (reply == nullptr) {
    return StatusCode::InvalidArgument;
  }
  RpcFuture future = InvokeAsync(call);
  const auto wait_budget = std::chrono::milliseconds(
      static_cast<int64_t>(call.queue_timeout_ms) + static_cast<int64_t>(call.exec_timeout_ms) +
      1000);

  const auto deadline = std::chrono::steady_clock::now() + wait_budget;
  while (!future.IsReady()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return StatusCode::PeerDisconnected;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return future.Wait(reply);
}

void RpcClient::Shutdown() {
  impl_->shutting_down.store(true);
  if (impl_->bootstrap != nullptr) {
    impl_->bootstrap->SetEngineDeathCallback({});
  }
  impl_->StopDispatcher();
  {
    std::lock_guard<std::mutex> lock(impl_->session_mutex);
    impl_->session_dead = true;
    impl_->current_session_id = 0;
    impl_->session.Reset();
    impl_->slot_pool.reset();
  }
  impl_->FailAllPending(StatusCode::PeerDisconnected);
}

}  // namespace memrpc
