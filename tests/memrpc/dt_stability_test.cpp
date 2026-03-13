#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>
#include <unistd.h>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/client/rpc_client.h"
#include "memrpc/server/rpc_server.h"

namespace {

constexpr memrpc::Opcode kTestOpcode = 1u;

void CloseHandles(memrpc::BootstrapHandles& handles) {
  if (handles.shmFd >= 0) close(handles.shmFd);
  if (handles.high_req_event_fd >= 0) close(handles.high_req_event_fd);
  if (handles.normal_req_event_fd >= 0) close(handles.normal_req_event_fd);
  if (handles.resp_event_fd >= 0) close(handles.resp_event_fd);
  if (handles.req_credit_event_fd >= 0) close(handles.req_credit_event_fd);
  if (handles.resp_credit_event_fd >= 0) close(handles.resp_credit_event_fd);
}

int GetEnvInt(const char* name, int default_value) {
  const char* value = std::getenv(name);
  if (value == nullptr || *value == '\0') {
    return default_value;
  }
  try {
    const int parsed = std::stoi(value);
    return parsed > 0 ? parsed : default_value;
  } catch (const std::exception&) {
    return default_value;
  }
}

uint32_t GetThreadCount() {
  const unsigned int hw = std::thread::hardware_concurrency();
  const unsigned int hw_threads = hw == 0 ? 1u : hw;
  const int default_threads = std::max(1, static_cast<int>(std::min(4u, hw_threads)));
  return static_cast<uint32_t>(GetEnvInt("MEMRPC_DT_THREADS", default_threads));
}

std::vector<uint8_t> MakePayload(size_t size, uint8_t seed) {
  return std::vector<uint8_t>(size, static_cast<uint8_t>(seed));
}

}  // namespace

TEST(DtStabilityTest, ShortRandomLoadStaysHealthy) {
  const int duration_ms = GetEnvInt("MEMRPC_DT_DURATION_MS", 3000);
  const int progress_timeout_ms = GetEnvInt("MEMRPC_DT_PROGRESS_TIMEOUT_MS", 200);
  const uint32_t thread_count = GetThreadCount();

  auto bootstrap = std::make_shared<memrpc::PosixDemoBootstrapChannel>();
  memrpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), memrpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  memrpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  memrpc::ServerOptions options;
  options.high_worker_threads = thread_count;
  options.normal_worker_threads = thread_count;
  server.SetOptions(options);
  server.RegisterHandler(kTestOpcode,
                         [](const memrpc::RpcServerCall& call, memrpc::RpcServerReply* reply) {
                           reply->status = memrpc::StatusCode::Ok;
                           reply->payload = call.payload;
                         });
  ASSERT_EQ(server.Start(), memrpc::StatusCode::Ok);

  memrpc::RpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), memrpc::StatusCode::Ok);

  std::atomic<uint64_t> success{0};
  std::atomic<memrpc::StatusCode> first_error{memrpc::StatusCode::Ok};
  std::atomic<int64_t> last_success_ms{0};

  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::milliseconds(duration_ms);

  auto now_ms = []() -> int64_t {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  };
  last_success_ms.store(now_ms());

  std::vector<std::thread> workers;
  workers.reserve(thread_count);
  for (uint32_t i = 0; i < thread_count; ++i) {
    workers.emplace_back([&, i]() {
      std::mt19937 rng(static_cast<uint32_t>(i + 1));
      const std::vector<size_t> sizes = {0, 128, 512, 2048, 4096};
      while (std::chrono::steady_clock::now() < deadline) {
        const size_t payload_size = sizes[rng() % sizes.size()];
        memrpc::RpcCall call;
        call.opcode = kTestOpcode;
        call.payload = MakePayload(payload_size, static_cast<uint8_t>(i));

        auto future = client.InvokeAsync(call);
        memrpc::RpcReply reply;
        memrpc::StatusCode status = future.Wait(&reply);
        if (status != memrpc::StatusCode::Ok) {
          first_error.store(status);
          return;
        }
        ++success;
        last_success_ms.store(now_ms());
      }
    });
  }

  std::atomic<bool> progress_ok{true};
  std::thread watchdog([&]() {
    while (std::chrono::steady_clock::now() < deadline) {
      const int64_t last = last_success_ms.load();
      if (now_ms() - last > progress_timeout_ms) {
        progress_ok.store(false);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  for (auto& t : workers) {
    t.join();
  }
  watchdog.join();

  client.Shutdown();
  server.Stop();

  EXPECT_EQ(first_error.load(), memrpc::StatusCode::Ok);
  EXPECT_TRUE(progress_ok.load());
  EXPECT_GT(success.load(), 0u);
}
