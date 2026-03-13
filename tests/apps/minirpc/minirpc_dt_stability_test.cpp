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

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/parent/minirpc_client.h"
#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/server/rpc_server.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

void CloseHandles(MemRpc::BootstrapHandles& handles) {
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

}  // namespace

TEST(MiniRpcDtStabilityTest, ShortRandomLoadStaysHealthy) {
  const int duration_ms = GetEnvInt("MEMRPC_DT_DURATION_MS", 3000);
  const int progress_timeout_ms = GetEnvInt("MEMRPC_DT_PROGRESS_TIMEOUT_MS", 200);
  const uint32_t thread_count = GetThreadCount();

  auto bootstrap = std::make_shared<MemRpc::PosixDemoBootstrapChannel>();
  MemRpc::BootstrapHandles unused_handles;
  ASSERT_EQ(bootstrap->OpenSession(unused_handles), MemRpc::StatusCode::Ok);
  CloseHandles(unused_handles);

  MemRpc::RpcServer server;
  server.SetBootstrapHandles(bootstrap->serverHandles());
  MemRpc::ServerOptions options;
  options.high_worker_threads = thread_count;
  options.normal_worker_threads = thread_count;
  server.SetOptions(options);
  MiniRpcService service;
  service.RegisterHandlers(&server);
  ASSERT_EQ(server.Start(), MemRpc::StatusCode::Ok);

  MiniRpcClient client(bootstrap);
  ASSERT_EQ(client.Init(), MemRpc::StatusCode::Ok);

  std::atomic<uint64_t> success{0};
  std::atomic<MemRpc::StatusCode> first_error{MemRpc::StatusCode::Ok};
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
      std::mt19937 rng(static_cast<uint32_t>(i + 7));
      while (std::chrono::steady_clock::now() < deadline) {
        const int choice = static_cast<int>(rng() % 100);
        MemRpc::StatusCode status = MemRpc::StatusCode::Ok;
        if (choice < 70) {
          EchoReply reply;
          status = client.Echo("ping", &reply);
        } else if (choice < 95) {
          AddReply reply;
          status = client.Add(1, 2, &reply);
        } else {
          SleepReply reply;
          status = client.Sleep(1, &reply);
        }
        if (status != MemRpc::StatusCode::Ok) {
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

  EXPECT_EQ(first_error.load(), MemRpc::StatusCode::Ok);
  EXPECT_TRUE(progress_ok.load());
  EXPECT_GT(success.load(), 0u);
}

}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
