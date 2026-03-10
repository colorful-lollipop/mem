# Reserved Slot Removal And Default Sizing Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove `high_reserved_request_slots`, simplify slot admission, bump protocol version, and set smaller default ring/slot sizes.

**Architecture:** Drop the reserved-slot field from config and shared-memory header, simplify `SlotPool::Reserve` to be priority-agnostic, and bump the protocol version to force layout refresh. Defaults move to a 2x relationship between slots and per-queue rings.

**Tech Stack:** C++17, CMake, GoogleTest

---

### Task 1: Add Failing Tests For Version + API Removal

**Files:**
- Modify: `tests/memrpc/protocol_layout_test.cpp`
- Modify: `tests/memrpc/slot_pool_test.cpp`

**Step 1: Update protocol version expectation (failing test)**

```cpp
// tests/memrpc/protocol_layout_test.cpp
EXPECT_EQ(memrpc::kProtocolVersion, 3u);
```

**Step 2: Add compile-time guard for SlotPool API removal (failing build)**

```cpp
// tests/memrpc/slot_pool_test.cpp
#include <type_traits>

static_assert(!std::is_constructible_v<memrpc::SlotPool, uint32_t, uint32_t>,
              "SlotPool must not accept reserved-slot constructor");
```

**Step 3: Run targeted tests to confirm failure**

Run: `cmake --build build`
Expected: FAIL
- Protocol test expects version 3 but code still returns 2.
- Static assert fails because 2-arg SlotPool ctor still exists.

**Step 4: Commit failing-test changes**

```bash
git add tests/memrpc/protocol_layout_test.cpp tests/memrpc/slot_pool_test.cpp
git commit -m "test: lock in protocol bump and slot pool api change"
```

---

### Task 2: Remove Reserved Slot Logic From SlotPool And Client

**Files:**
- Modify: `src/core/slot_pool.h`
- Modify: `src/core/slot_pool.cpp`
- Modify: `src/client/rpc_client.cpp`
- Modify: `tests/memrpc/slot_pool_test.cpp`

**Step 1: Update SlotPool API to single-arg constructor and priority-agnostic reserve**

```cpp
// src/core/slot_pool.h
explicit SlotPool(uint32_t slot_count);
std::optional<uint32_t> Reserve();
```

```cpp
// src/core/slot_pool.cpp
SlotPool::SlotPool(uint32_t slot_count)
    : slot_count_(slot_count),
      states_(slot_count),
      next_free_(slot_count) {
  // initialization unchanged except removal of high_reserved_slots_
}

std::optional<uint32_t> SlotPool::Reserve() {
  const uint32_t count = free_count_.load(std::memory_order_acquire);
  if (count == 0) {
    return std::nullopt;
  }
  // pop from Treiber stack as before
}
```

**Step 2: Update client usage to call Reserve() with no priority**

```cpp
// src/client/rpc_client.cpp
const auto slot = slot_pool != nullptr ? slot_pool->Reserve() : std::optional<uint32_t>{};
```

**Step 3: Remove obsolete reserved-slot test**

```cpp
// tests/memrpc/slot_pool_test.cpp
// Remove NormalReservePreservesHighReservedSlots test.
```

**Step 4: Run targeted tests**

Run: `ctest --test-dir build --output-on-failure -R "ProtocolLayoutTest|SlotPoolTest"`
Expected: PASS

**Step 5: Commit**

```bash
git add src/core/slot_pool.h src/core/slot_pool.cpp src/client/rpc_client.cpp \
  tests/memrpc/slot_pool_test.cpp
git commit -m "refactor: drop reserved-slot admission"
```

---

### Task 3: Remove Reserved Slot Field From Layout And Bootstrap

**Files:**
- Modify: `include/memrpc/client/demo_bootstrap.h`
- Modify: `src/core/shm_layout.h`
- Modify: `src/core/session.cpp`
- Modify: `src/bootstrap/posix_demo_bootstrap.cpp`
- Modify: `tests/memrpc/rpc_client_integration_test.cpp`

**Step 1: Remove field from config/header/layout**

```cpp
// include/memrpc/client/demo_bootstrap.h
uint32_t high_ring_size = 32;
uint32_t normal_ring_size = 32;
uint32_t response_ring_size = 64;
uint32_t slot_count = 64;
// remove high_reserved_request_slots
```

```cpp
// src/core/shm_layout.h
struct LayoutConfig {
  uint32_t high_ring_size = 0;
  uint32_t normal_ring_size = 0;
  uint32_t response_ring_size = 0;
  uint32_t slot_count = 0;
  uint32_t slot_size = ComputeSlotSize(kDefaultMaxRequestBytes, kDefaultMaxResponseBytes);
  uint32_t max_request_bytes = kDefaultMaxRequestBytes;
  uint32_t max_response_bytes = kDefaultMaxResponseBytes;
};

struct SharedMemoryHeader {
  ...
  uint32_t slot_count = 0;
  uint32_t slot_size = ComputeSlotSize(...);
  uint32_t max_request_bytes = kDefaultMaxRequestBytes;
  uint32_t max_response_bytes = kDefaultMaxResponseBytes;
  ...
};
```

**Step 2: Update bootstrap and session to stop reading/writing the removed field**

```cpp
// src/bootstrap/posix_demo_bootstrap.cpp
const LayoutConfig layout_config{impl_->config.high_ring_size,
                                 impl_->config.normal_ring_size,
                                 impl_->config.response_ring_size,
                                 impl_->config.slot_count,
                                 ComputeSlotSize(...),
                                 impl_->config.max_request_bytes,
                                 impl_->config.max_response_bytes};

// src/core/session.cpp
config.high_ring_size = 32;
config.normal_ring_size = 32;
config.response_ring_size = 64;
config.slot_count = 64;
// no high_reserved_request_slots
```

**Step 3: Remove integration test that depends on reserved slots**

```cpp
// tests/memrpc/rpc_client_integration_test.cpp
// Remove HighPriorityRequestStillAdmitsWhenNormalTrafficUsesNonReservedSlots.
```

**Step 4: Run full tests**

Run: `ctest --test-dir build --output-on-failure`
Expected: PASS

**Step 5: Commit**

```bash
git add include/memrpc/client/demo_bootstrap.h src/core/shm_layout.h \
  src/core/session.cpp src/bootstrap/posix_demo_bootstrap.cpp \
  tests/memrpc/rpc_client_integration_test.cpp
git commit -m "feat: remove reserved slot field and shrink defaults"
```

---

### Task 4: Bump Protocol Version And Align Tests

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `tests/memrpc/protocol_layout_test.cpp`

**Step 1: Update protocol version**

```cpp
// src/core/protocol.h
inline constexpr uint32_t kProtocolVersion = 3u;
```

**Step 2: Run protocol layout test**

Run: `ctest --test-dir build --output-on-failure -R ProtocolLayoutTest`
Expected: PASS

**Step 3: Commit**

```bash
git add src/core/protocol.h tests/memrpc/protocol_layout_test.cpp
git commit -m "feat: bump protocol version for shm layout change"
```

---

Plan complete and saved to `docs/plans/2026-03-10-memrpc-defaults-reserved-slot-removal.md`. Two execution options:

1. Subagent-Driven (this session) - I dispatch fresh subagent per task, review between tasks, fast iteration
2. Parallel Session (separate) - Open new session with executing-plans, batch execution with checkpoints

Which approach?
