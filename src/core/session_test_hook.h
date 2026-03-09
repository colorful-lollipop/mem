#ifndef MEMRPC_CORE_SESSION_TEST_HOOK_H_
#define MEMRPC_CORE_SESSION_TEST_HOOK_H_

#include <cstdint>

namespace memrpc {

enum class RingTraceOperation : uint8_t {
  PushHighRequest = 0,
  PushNormalRequest = 1,
  PopHighRequest = 2,
  PopNormalRequest = 3,
  PushResponse = 4,
  PopResponse = 5,
  Count = 6,
};

using RingTraceCallback = void (*)(RingTraceOperation operation, uint64_t thread_token);

void SetRingTraceCallbackForTest(RingTraceCallback callback);
void ClearRingTraceCallbackForTest();

}  // namespace memrpc

#endif  // MEMRPC_CORE_SESSION_TEST_HOOK_H_
