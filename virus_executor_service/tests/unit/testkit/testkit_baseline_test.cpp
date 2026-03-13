#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include "testkit/testkit_codec.h"
#include "testkit/testkit_service.h"

namespace virus_executor_service::testkit {
namespace {

int GetEnvInt(const char* name, int defaultValue) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        return defaultValue;
    }
    try {
        const int parsed = std::stoi(value);
        return parsed > 0 ? parsed : defaultValue;
    } catch (const std::exception&) {
        return defaultValue;
    }
}

}  // namespace

TEST(TestkitBaselineTest, DirectHandlerCallOpsPerSec) {
    const int durationMs = GetEnvInt("MEMRPC_PERF_durationMs", 1000);
    const int warmupMs = GetEnvInt("MEMRPC_PERF_WARMUP_MS", 200);

    TestkitService service;

    const auto warmupEnd =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(warmupMs);
    while (std::chrono::steady_clock::now() < warmupEnd) {
        EchoRequest request;
        request.text = "ping";
        EchoReply reply = service.Echo(request);
        (void)reply;
    }

    struct Case {
        const char* name;
        uint64_t ops = 0;
    };
    std::vector<Case> cases = {{"echo"}, {"add"}, {"echo+codec"}};

    for (auto& current : cases) {
        const auto endTime =
            std::chrono::steady_clock::now() + std::chrono::milliseconds(durationMs);
        if (std::string(current.name) == "echo") {
            EchoRequest request;
            request.text = "ping";
            while (std::chrono::steady_clock::now() < endTime) {
                EchoReply reply = service.Echo(request);
                (void)reply;
                ++current.ops;
            }
        } else if (std::string(current.name) == "add") {
            AddRequest request;
            request.lhs = 1;
            request.rhs = 2;
            while (std::chrono::steady_clock::now() < endTime) {
                AddReply reply = service.Add(request);
                (void)reply;
                ++current.ops;
            }
        } else {
            EchoRequest request;
            request.text = "ping";
            while (std::chrono::steady_clock::now() < endTime) {
                std::vector<uint8_t> encoded;
                MemRpc::EncodeMessage(request, &encoded);
                EchoRequest decoded;
                MemRpc::DecodeMessage(encoded, &decoded);
                EchoReply reply = service.Echo(decoded);
                std::vector<uint8_t> replyEncoded;
                MemRpc::EncodeMessage(reply, &replyEncoded);
                EchoReply replyDecoded;
                MemRpc::DecodeMessage(replyEncoded, &replyDecoded);
                (void)replyDecoded;
                ++current.ops;
            }
        }
    }

    const double durationSec = std::max(1, durationMs) / 1000.0;
    std::cout << "\n=== Testkit In-Process Baseline ===" << std::endl;
    for (const auto& current : cases) {
        const double opsPerSec = current.ops / durationSec;
        std::cout << std::setw(14) << current.name << ": " << std::fixed
                  << std::setprecision(0) << opsPerSec << " ops/sec" << std::endl;
        EXPECT_GT(current.ops, 0u);
    }
}

}  // namespace virus_executor_service::testkit
