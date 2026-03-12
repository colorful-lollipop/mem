# vpsdemo Heartbeat Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Add a 10s SA-socket heartbeat with a versioned health reply; client restarts the SA by calling `CloseSession` and re-opening when heartbeat is unhealthy or times out.

**Architecture:** Introduce a new `Heartbeat` command on the SA socket (cmd=3) that returns a fixed-size health snapshot. The server builds the reply from `VpsDemoService` and `EngineSessionService`, and the client (`VpsBootstrapProxy`) sends short-lived heartbeat requests while reusing the existing monitor thread. Mock SA transport is extended to send data-only replies and optionally close after reply.

**Tech Stack:** C++17, gtest, CMake, Unix domain sockets

---

### Task 1: Add Heartbeat Types + Direct Service Heartbeat (Unit Test)

**Files:**
- Modify: `demo/vpsdemo/include/vps_bootstrap_interface.h`
- Modify: `demo/vpsdemo/include/virus_executor_service.h`
- Modify: `demo/vpsdemo/src/virus_executor_service.cpp`
- Modify: `demo/vpsdemo/include/vpsdemo_service.h`
- Modify: `demo/vpsdemo/src/vpsdemo_service.cpp`
- Modify: `demo/vpsdemo/include/ves_session_service.h`
- Modify: `demo/vpsdemo/src/ves_session_service.cpp`
- Create: `demo/vpsdemo/tests/vps_heartbeat_test.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Write the failing test**

Add `demo/vpsdemo/tests/vps_heartbeat_test.cpp`:

```cpp
#include <gtest/gtest.h>

#include "virus_executor_service.h"
#include "vps_bootstrap_interface.h"

namespace vpsdemo {

TEST(VpsHeartbeatTest, UnhealthyBeforeOpenSession) {
    VirusExecutorService service;
    service.OnStart();

    VpsHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VpsHeartbeatStatus::Unhealthy));

    service.OnStop();
}

}  // namespace vpsdemo
```

Update `demo/vpsdemo/CMakeLists.txt` to add the test target when `VPSDEMO_ENABLE_TESTS` is ON:

```cmake
  add_executable(vpsdemo_heartbeat_test tests/vps_heartbeat_test.cpp)
  target_link_libraries(vpsdemo_heartbeat_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_heartbeat_test COMMAND vpsdemo_heartbeat_test)
```

**Step 2: Run test to verify it fails**

Run:
- `cmake -S demo/vpsdemo -B build/vpsdemo -DVPSDEMO_ENABLE_TESTS=ON`
- `cmake --build build/vpsdemo`
- `ctest --test-dir build/vpsdemo --output-on-failure`

Expected: compile failure because `VpsHeartbeatReply` / `Heartbeat` do not exist.

**Step 3: Write minimal implementation**

In `demo/vpsdemo/include/vps_bootstrap_interface.h` add:

```cpp
enum class VpsHeartbeatStatus : uint32_t {
    Ok = 0,
    Unhealthy = 1,
};

struct VpsHeartbeatReply {
    uint32_t version = 1;
    uint32_t status = static_cast<uint32_t>(VpsHeartbeatStatus::Unhealthy);
    uint64_t session_id = 0;
    uint32_t in_flight = 0;
    uint32_t last_task_age_ms = 0;
    char current_task[64] = {};
    uint32_t reserved[4] = {};
};
```

Extend `IVpsBootstrap` with:

```cpp
virtual memrpc::StatusCode Heartbeat(VpsHeartbeatReply& reply) = 0;
```

In `demo/vpsdemo/include/virus_executor_service.h` add:

```cpp
memrpc::StatusCode Heartbeat(VpsHeartbeatReply& reply) override;
```

In `demo/vpsdemo/src/virus_executor_service.cpp` implement:

```cpp
memrpc::StatusCode VirusExecutorService::Heartbeat(VpsHeartbeatReply& reply) {
    reply = VpsHeartbeatReply{};
    const bool healthy = (session_service_ != nullptr) && service_.initialized();
    reply.status = static_cast<uint32_t>(healthy ? VpsHeartbeatStatus::Ok
                                                : VpsHeartbeatStatus::Unhealthy);
    return memrpc::StatusCode::Ok;
}
```

Add minimal health snapshot plumbing in `VpsDemoService` (start with defaults so the test passes):

```cpp
struct VpsHealthSnapshot {
    uint32_t in_flight = 0;
    uint32_t last_task_age_ms = 0;
    std::string current_task = "idle";
};

VpsHealthSnapshot GetHealthSnapshot() const;
```

Return defaults in `GetHealthSnapshot()` and keep state unchanged for now. No other behavior changes yet.

**Step 4: Run test to verify it passes**

Run:
- `cmake --build build/vpsdemo`
- `ctest --test-dir build/vpsdemo --output-on-failure`

Expected: `vpsdemo_heartbeat_test` PASS.

**Step 5: Commit**

```bash
git add demo/vpsdemo/include/vps_bootstrap_interface.h \
        demo/vpsdemo/include/virus_executor_service.h \
        demo/vpsdemo/src/virus_executor_service.cpp \
        demo/vpsdemo/include/vpsdemo_service.h \
        demo/vpsdemo/src/vpsdemo_service.cpp \
        demo/vpsdemo/include/ves_session_service.h \
        demo/vpsdemo/src/ves_session_service.cpp \
        demo/vpsdemo/tests/vps_heartbeat_test.cpp \
        demo/vpsdemo/CMakeLists.txt

git commit -m "feat: add heartbeat types and base handler"
```

---

### Task 2: Populate Health Snapshot + Session Tracking (Unit Test Update)

**Files:**
- Modify: `demo/vpsdemo/include/vpsdemo_service.h`
- Modify: `demo/vpsdemo/src/vpsdemo_service.cpp`
- Modify: `demo/vpsdemo/include/ves_session_service.h`
- Modify: `demo/vpsdemo/src/ves_session_service.cpp`
- Modify: `demo/vpsdemo/src/virus_executor_service.cpp`
- Modify: `demo/vpsdemo/tests/vps_heartbeat_test.cpp`

**Step 1: Write the failing test**

Update `demo/vpsdemo/tests/vps_heartbeat_test.cpp` to add:

```cpp
TEST(VpsHeartbeatTest, OkAfterOpenSession) {
    VirusExecutorService service;
    service.OnStart();

    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(service.OpenSession(handles), memrpc::StatusCode::Ok);
    const uint64_t session_id = handles.session_id;

    VpsHeartbeatReply reply{};
    EXPECT_EQ(service.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VpsHeartbeatStatus::Ok));
    EXPECT_EQ(reply.session_id, session_id);
    EXPECT_STREQ(reply.current_task, "idle");

    service.CloseSession();
    service.OnStop();
}
```

**Step 2: Run test to verify it fails**

Run:
- `cmake --build build/vpsdemo`
- `ctest --test-dir build/vpsdemo --output-on-failure`

Expected: FAIL because `Heartbeat` does not populate `session_id`/`current_task` or return OK.

**Step 3: Write minimal implementation**

Add lightweight health tracking in `VpsDemoService`:

```cpp
class VpsDemoService {
 public:
    struct VpsHealthSnapshot {
        uint32_t in_flight = 0;
        uint32_t last_task_age_ms = 0;
        std::string current_task = "idle";
    };

    VpsHealthSnapshot GetHealthSnapshot() const;
    // ... existing methods ...

 private:
    mutable std::mutex health_mutex_;
    uint32_t in_flight_ = 0;
    uint32_t last_task_start_mono_ms_ = 0;
    std::string current_task_ = "idle";
};
```

In `demo/vpsdemo/src/vpsdemo_service.cpp`:
- Add `MonotonicNowMs()` helper (steady_clock).
- In `ScanFile`, update `in_flight_`, `current_task_`, `last_task_start_mono_ms_` under lock at start/end.
- Implement `GetHealthSnapshot()` to compute `last_task_age_ms` if `in_flight_ > 0`.

Track session id in `EngineSessionService`:

```cpp
class EngineSessionService final : public VesSessionProvider {
 public:
    uint64_t session_id() const;
 private:
    uint64_t session_id_ = 0;
};
```

Set `session_id_ = handles.session_id` on `OpenSession`, reset to 0 on `CloseSession`.

Update `VirusExecutorService::Heartbeat` to populate reply:

```cpp
memrpc::StatusCode VirusExecutorService::Heartbeat(VpsHeartbeatReply& reply) {
    reply = VpsHeartbeatReply{};
    if (!session_service_) {
        return memrpc::StatusCode::Ok;
    }
    reply.session_id = session_service_->session_id();
    const auto snapshot = service_.GetHealthSnapshot();
    reply.in_flight = snapshot.in_flight;
    reply.last_task_age_ms = snapshot.last_task_age_ms;
    std::snprintf(reply.current_task, sizeof(reply.current_task), "%s",
                  snapshot.current_task.c_str());

    const bool healthy = service_.initialized() && reply.session_id != 0;
    reply.status = static_cast<uint32_t>(healthy ? VpsHeartbeatStatus::Ok
                                                : VpsHeartbeatStatus::Unhealthy);
    return memrpc::StatusCode::Ok;
}
```

**Step 4: Run test to verify it passes**

Run:
- `cmake --build build/vpsdemo`
- `ctest --test-dir build/vpsdemo --output-on-failure`

Expected: `vpsdemo_heartbeat_test` PASS.

**Step 5: Commit**

```bash
git add demo/vpsdemo/include/vpsdemo_service.h \
        demo/vpsdemo/src/vpsdemo_service.cpp \
        demo/vpsdemo/include/ves_session_service.h \
        demo/vpsdemo/src/ves_session_service.cpp \
        demo/vpsdemo/src/virus_executor_service.cpp \
        demo/vpsdemo/tests/vps_heartbeat_test.cpp

git commit -m "feat: add vpsdemo health snapshot for heartbeat"
```

---

### Task 3: SA Socket Heartbeat + Proxy Monitor Integration (Integration Test)

**Files:**
- Modify: `third_party/ohos_sa_mock/include/mock_ipc_types.h`
- Modify: `third_party/ohos_sa_mock/src/mock_service_socket.cpp`
- Modify: `third_party/ohos_sa_mock/include/scm_rights.h` (optional helper)
- Modify: `third_party/ohos_sa_mock/src/scm_rights.cpp` (optional helper)
- Modify: `demo/vpsdemo/include/vps_bootstrap_stub.h`
- Modify: `demo/vpsdemo/include/vps_bootstrap_proxy.h`
- Modify: `demo/vpsdemo/src/vps_bootstrap_proxy.cpp`
- Modify: `demo/vpsdemo/tests/vps_heartbeat_test.cpp`

**Step 1: Write the failing test**

Extend `demo/vpsdemo/tests/vps_heartbeat_test.cpp` with an SA-socket round trip:

```cpp
#include <unistd.h>

TEST(VpsHeartbeatTest, HeartbeatOverSaSocket) {
    const std::string socketPath = "/tmp/vpsdemo_hb_" + std::to_string(getpid());

    auto stub = std::make_shared<VirusExecutorService>();
    stub->AsObject()->SetServicePath(socketPath);
    stub->OnStart();
    ASSERT_TRUE(stub->Publish(stub.get()));

    VpsBootstrapProxy proxy(stub->AsObject(), socketPath);
    memrpc::BootstrapHandles handles{};
    ASSERT_EQ(proxy.OpenSession(handles), memrpc::StatusCode::Ok);
    const uint64_t session_id = handles.session_id;

    VpsHeartbeatReply reply{};
    EXPECT_EQ(proxy.Heartbeat(reply), memrpc::StatusCode::Ok);
    EXPECT_EQ(reply.status, static_cast<uint32_t>(VpsHeartbeatStatus::Ok));
    EXPECT_EQ(reply.session_id, session_id);

    proxy.CloseSession();
    stub->OnStop();
}
```

**Step 2: Run test to verify it fails**

Run:
- `cmake --build build/vpsdemo`
- `ctest --test-dir build/vpsdemo --output-on-failure`

Expected: FAIL (no cmd=3 handling and data-only reply path).

**Step 3: Write minimal implementation**

Extend mock IPC reply to support data-only + close-after-reply:

`third_party/ohos_sa_mock/include/mock_ipc_types.h`

```cpp
struct MockIpcReply {
    static constexpr size_t MAX_FDS = 16;
    int fds[MAX_FDS] = {};
    size_t fd_count = 0;
    char data[256] = {};
    size_t data_len = 0;
    bool close_after_reply = false;
};
```

Update `MockServiceSocket::AcceptLoop` to send reply data when `fd_count == 0` and close if requested. Example:

```cpp
if (reply.fd_count > 0) {
    SendFds(...);
} else if (reply.data_len > 0) {
    send(client_fd, reply.data, reply.data_len, MSG_NOSIGNAL);
}
if (reply.close_after_reply) {
    close(client_fd);
    continue;
}
```

Add cmd=3 handling in `demo/vpsdemo/include/vps_bootstrap_stub.h`:

```cpp
case 3: {  // Heartbeat
    VpsHeartbeatReply hb{};
    if (Heartbeat(hb) != memrpc::StatusCode::Ok) {
        return false;
    }
    std::memcpy(reply->data, &hb, sizeof(hb));
    reply->data_len = sizeof(hb);
    reply->close_after_reply = true;
    return true;
}
```

Implement `Heartbeat` in `VpsBootstrapProxy` and integrate into `MonitorSocket`:

`demo/vpsdemo/include/vps_bootstrap_proxy.h`

```cpp
memrpc::StatusCode Heartbeat(VpsHeartbeatReply& reply) override;
```

`demo/vpsdemo/src/vps_bootstrap_proxy.cpp`:
- Add a helper to send cmd=3 on a short-lived connection and `recv()` a fixed-size reply.
- In `MonitorSocket`, keep the existing death-detection poll loop, but every 10s call `Heartbeat()`. On failure or `reply.status != Ok`, call `CloseSession()` and invoke `death_callback_` with the current session id.
- Update `CloseSession()` to send cmd=2 over a short-lived connection before closing the monitor socket.
- Guard `CloseSession()` against self-join when called from the monitor thread.

**Step 4: Run test to verify it passes**

Run:
- `cmake --build build/vpsdemo`
- `ctest --test-dir build/vpsdemo --output-on-failure`

Expected: `vpsdemo_heartbeat_test` PASS.

**Step 5: Commit**

```bash
git add third_party/ohos_sa_mock/include/mock_ipc_types.h \
        third_party/ohos_sa_mock/src/mock_service_socket.cpp \
        third_party/ohos_sa_mock/include/scm_rights.h \
        third_party/ohos_sa_mock/src/scm_rights.cpp \
        demo/vpsdemo/include/vps_bootstrap_stub.h \
        demo/vpsdemo/include/vps_bootstrap_proxy.h \
        demo/vpsdemo/src/vps_bootstrap_proxy.cpp \
        demo/vpsdemo/tests/vps_heartbeat_test.cpp

git commit -m "feat: add SA-socket heartbeat and proxy monitor"
```

---

Plan complete and saved to `docs/plans/2026-03-12-vpsdemo-heartbeat-implementation-plan.md`. Two execution options:

1. Subagent-Driven (this session) - I dispatch fresh subagent per task, review between tasks, fast iteration
2. Parallel Session (separate) - Open new session with executing-plans, batch execution with checkpoints

Which approach?
