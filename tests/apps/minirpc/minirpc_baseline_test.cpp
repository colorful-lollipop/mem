#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <string>
#include <vector>

#include "apps/minirpc/child/minirpc_service.h"
#include "apps/minirpc/common/minirpc_codec.h"

namespace OHOS::Security::VirusProtectionService::MiniRpc {
namespace {

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

TEST(MiniRpcBaselineTest, DirectHandlerCallOpsPerSec) {
  const int durationMs = GetEnvInt("MEMRPC_PERF_durationMs", 1000);
  const int warmup_ms = GetEnvInt("MEMRPC_PERF_WARMUP_MS", 200);

  MiniRpcService service;

  // Warm up.
  {
    const auto warmup_end =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(warmup_ms);
    while (std::chrono::steady_clock::now() < warmup_end) {
      EchoRequest req;
      req.text = "ping";
      EchoReply reply = service.Echo(req);
      (void)reply;
    }
  }

  struct Case {
    const char* name;
    uint64_t ops = 0;
  };
  std::vector<Case> cases = {{"echo"}, {"add"}, {"echo+codec"}};

  for (auto& c : cases) {
    const auto end_time =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
    if (std::string(c.name) == "echo") {
      EchoRequest req;
      req.text = "ping";
      while (std::chrono::steady_clock::now() < end_time) {
        EchoReply reply = service.Echo(req);
        (void)reply;
        ++c.ops;
      }
    } else if (std::string(c.name) == "add") {
      AddRequest req;
      req.lhs = 1;
      req.rhs = 2;
      while (std::chrono::steady_clock::now() < end_time) {
        AddReply reply = service.Add(req);
        (void)reply;
        ++c.ops;
      }
    } else if (std::string(c.name) == "echo+codec") {
      EchoRequest req;
      req.text = "ping";
      while (std::chrono::steady_clock::now() < end_time) {
        std::vector<uint8_t> encoded;
        EncodeMessage(req, &encoded);
        EchoRequest decoded;
        DecodeMessage(encoded, &decoded);
        EchoReply reply = service.Echo(decoded);
        std::vector<uint8_t> reply_encoded;
        EncodeMessage(reply, &reply_encoded);
        EchoReply reply_decoded;
        DecodeMessage(reply_encoded, &reply_decoded);
        (void)reply_decoded;
        ++c.ops;
      }
    }
  }

  const double duration_sec = std::max(1, durationMs) / 1000.0;
  std::cout << "\n=== In-Process Baseline (no RPC overhead) ===" << std::endl;
  for (const auto& c : cases) {
    const double ops_per_sec = c.ops / duration_sec;
    std::cout << std::setw(14) << c.name << ": " << std::fixed << std::setprecision(0)
              << ops_per_sec << " ops/sec" << std::endl;
    EXPECT_GT(c.ops, 0u);
  }
}

}  // namespace
}  // namespace OHOS::Security::VirusProtectionService::MiniRpc
