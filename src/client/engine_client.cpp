#include "memrpc/client.h"

#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "core/protocol.h"
#include "core/session.h"
#include "core/slot_pool.h"

namespace memrpc {

namespace {

uint32_t MonotonicNowMs() {
  const auto now = std::chrono::steady_clock::now().time_since_epoch();
  return static_cast<uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(now).count() & 0xffffffffu);
}

struct PendingCall {
  std::mutex mutex;
  std::condition_variable cv;
  bool ready = false;
  bool abandoned = false;
  ScanResult result;
};

}  // namespace

struct EngineClient::Impl {
  explicit Impl(std::shared_ptr<IBootstrapChannel> bootstrap_channel)
      : bootstrap(std::move(bootstrap_channel)) {}

  std::shared_ptr<IBootstrapChannel> bootstrap;
  Session session;
  std::unique_ptr<SlotPool> slot_pool;
  std::mutex pending_mutex;
  std::unordered_map<uint64_t, std::shared_ptr<PendingCall>> pending_calls;
  std::thread dispatcher_thread;
  std::atomic<bool> running{false};
  std::atomic<uint64_t> next_request_id{1};

  void CompleteRequest(const ResponseRingEntry& entry) {
    SlotPayload* payload = session.slot_payload(entry.slot_index);
    if (payload == nullptr || slot_pool == nullptr) {
      return;
    }

    ScanResult result;
    result.status = static_cast<StatusCode>(payload->status_code);
    result.verdict = static_cast<ScanVerdict>(payload->verdict);
    result.engine_code = payload->engine_code;
    result.detail_code = payload->detail_code;
    const uint32_t safe_message_length =
        std::min<uint32_t>(payload->message_length, kMaxMessageSize - 1);
    result.message.assign(payload->message, safe_message_length);

    std::shared_ptr<PendingCall> pending;
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
        pending->result = std::move(result);
        pending->ready = true;
        pending->cv.notify_one();
      }
    }
    slot_pool->Release(entry.slot_index);
    std::lock_guard<std::mutex> lock(pending_mutex);
    pending_calls.erase(entry.request_id);
  }

  void ResponseLoop() {
    pollfd fd{session.handles().resp_event_fd, POLLIN, 0};
    while (running.load()) {
      const int poll_result = poll(&fd, 1, 100);
      if (poll_result <= 0) {
        continue;
      }
      if ((fd.revents & POLLIN) == 0) {
        continue;
      }

      uint64_t counter = 0;
      while (read(session.handles().resp_event_fd, &counter, sizeof(counter)) == sizeof(counter)) {
      }

      ResponseRingEntry entry;
      while (session.PopResponse(&entry)) {
        CompleteRequest(entry);
      }
    }
  }
};

EngineClient::EngineClient(std::shared_ptr<IBootstrapChannel> bootstrap)
    : impl_(std::make_unique<Impl>(std::move(bootstrap))) {}

EngineClient::~EngineClient() {
  Shutdown();
}

void EngineClient::SetBootstrapChannel(std::shared_ptr<IBootstrapChannel> bootstrap) {
  impl_->bootstrap = std::move(bootstrap);
}

StatusCode EngineClient::Init() {
  if (impl_->bootstrap == nullptr) {
    return StatusCode::kInvalidArgument;
  }
  BootstrapHandles handles;
  const StatusCode connect_status = impl_->bootstrap->Connect(&handles);
  if (connect_status != StatusCode::kOk) {
    return connect_status;
  }
  const StatusCode attach_status = impl_->session.Attach(handles);
  if (attach_status != StatusCode::kOk) {
    return attach_status;
  }
  impl_->slot_pool = std::make_unique<SlotPool>(impl_->session.header()->slot_count);
  impl_->running.store(true);
  impl_->dispatcher_thread = std::thread([this] { impl_->ResponseLoop(); });
  return StatusCode::kOk;
}

StatusCode EngineClient::Scan(const ScanRequest& request, ScanResult* result) {
  if (result == nullptr || impl_->slot_pool == nullptr || !impl_->session.valid()) {
    return StatusCode::kInvalidArgument;
  }
  if (request.file_path.empty() || request.file_path.size() >= kMaxFilePathSize) {
    return StatusCode::kInvalidArgument;
  }

  const auto slot = impl_->slot_pool->Reserve();
  if (!slot.has_value()) {
    return StatusCode::kQueueFull;
  }
  SlotPayload* payload = impl_->session.slot_payload(*slot);
  if (payload == nullptr) {
    impl_->slot_pool->Release(*slot);
    return StatusCode::kEngineInternalError;
  }

  std::memset(payload, 0, sizeof(SlotPayload));
  payload->queue_timeout_ms = request.options.queue_timeout_ms;
  payload->exec_timeout_ms = request.options.exec_timeout_ms;
  payload->flags = request.options.flags;
  payload->priority = static_cast<uint32_t>(request.options.priority);
  payload->file_path_length = static_cast<uint32_t>(request.file_path.size());
  std::memcpy(payload->file_path, request.file_path.data(), request.file_path.size());

  const uint64_t request_id = impl_->next_request_id.fetch_add(1);
  auto pending = std::make_shared<PendingCall>();
  {
    std::lock_guard<std::mutex> lock(impl_->pending_mutex);
    impl_->pending_calls.emplace(request_id, pending);
  }

  RequestRingEntry entry;
  entry.request_id = request_id;
  entry.slot_index = *slot;
  entry.flags = static_cast<uint16_t>(request.options.flags);
  entry.enqueue_mono_ms = MonotonicNowMs();
  entry.payload_size = sizeof(SlotPayload);

  const bool high_priority = request.options.priority == Priority::kHigh;
  impl_->slot_pool->Transition(*slot,
                               high_priority ? SlotState::kQueuedHigh : SlotState::kQueuedNormal);
  const StatusCode queue_status =
      impl_->session.PushRequest(high_priority ? QueueKind::kHighRequest
                                               : QueueKind::kNormalRequest,
                                 entry);
  if (queue_status != StatusCode::kOk) {
    std::lock_guard<std::mutex> lock(impl_->pending_mutex);
    impl_->pending_calls.erase(request_id);
    impl_->slot_pool->Release(*slot);
    return queue_status;
  }

  const uint64_t signal_value = 1;
  const int req_fd = high_priority ? impl_->session.handles().high_req_event_fd
                                   : impl_->session.handles().normal_req_event_fd;
  if (write(req_fd, &signal_value, sizeof(signal_value)) != sizeof(signal_value)) {
    std::lock_guard<std::mutex> lock(impl_->pending_mutex);
    impl_->pending_calls.erase(request_id);
    impl_->slot_pool->Release(*slot);
    return StatusCode::kPeerDisconnected;
  }

  const auto wait_budget = std::chrono::milliseconds(
      static_cast<int64_t>(request.options.queue_timeout_ms) +
      static_cast<int64_t>(request.options.exec_timeout_ms) + 1000);
  std::unique_lock<std::mutex> wait_lock(pending->mutex);
  if (!pending->cv.wait_for(wait_lock, wait_budget, [&pending] { return pending->ready; })) {
    pending->abandoned = true;
    return StatusCode::kPeerDisconnected;
  }

  *result = pending->result;
  return result->status;
}

void EngineClient::Shutdown() {
  impl_->running.store(false);
  if (impl_->dispatcher_thread.joinable()) {
    impl_->dispatcher_thread.join();
  }
  impl_->session.Reset();
  impl_->slot_pool.reset();
}

}  // namespace memrpc
