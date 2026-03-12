# vpsdemo OpenSession Init Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Make `OpenSession` synchronously initialize the vpsdemo engine in the SA server, while keeping the socket SCM_RIGHTS path as a pure transport mock and removing client-side `InitEngine` usage.

**Architecture:** Introduce an engine-side `EngineSessionService` that owns real `OpenSession` logic (synchronous init + handle creation). `VirusExecutorService` (renamed SA class) delegates to this provider and only handles the socket protocol. `VpsDemoService` initialization becomes server-driven and idempotent.

**Tech Stack:** C++17, CMake, memrpc core, GoogleTest (optional demo tests)

---

### Task 1: Add vpsdemo session-service test scaffold

**Files:**
- Create: `demo/vpsdemo/tests/vps_session_service_test.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Write the failing test**

```cpp
#include <gtest/gtest.h>

#include <unistd.h>

#include "memrpc/core/types.h"
#include "vps_session_service.h"
#include "vpsdemo_service.h"

namespace vpsdemo {

namespace {
void CloseHandles(memrpc::BootstrapHandles* handles) {
    if (handles == nullptr) return;
    int* fds[] = {
        &handles->shm_fd,
        &handles->high_req_event_fd,
        &handles->normal_req_event_fd,
        &handles->resp_event_fd,
        &handles->req_credit_event_fd,
        &handles->resp_credit_event_fd,
    };
    for (int* fd : fds) {
        if (fd != nullptr && *fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
}
}  // namespace

TEST(VpsSessionServiceTest, OpenSessionInitializesService) {
    VpsDemoService service;
    EngineSessionService sessionService(&service);

    memrpc::BootstrapHandles handles{};
    EXPECT_EQ(sessionService.OpenSession(&handles), memrpc::StatusCode::Ok);
    EXPECT_TRUE(service.initialized());

    CloseHandles(&handles);
}

}  // namespace vpsdemo
```

**Step 2: Wire the test target**

Add to `demo/vpsdemo/CMakeLists.txt`:

```cmake
option(VPSDEMO_ENABLE_TESTS "Build vpsdemo tests" OFF)

if (VPSDEMO_ENABLE_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)
  add_executable(vpsdemo_session_service_test tests/vps_session_service_test.cpp)
  target_link_libraries(vpsdemo_session_service_test PRIVATE vpsdemo_lib GTest::gtest_main)
  add_test(NAME vpsdemo_session_service_test COMMAND vpsdemo_session_service_test)
endif()
```

**Step 3: Run test to verify it fails**

Run:
- `cmake -S demo/vpsdemo -B demo/vpsdemo/build -DVPSDEMO_ENABLE_TESTS=ON`
- `cmake --build demo/vpsdemo/build`
- `ctest --test-dir demo/vpsdemo/build -R vpsdemo_session_service_test --output-on-failure`

Expected: FAIL (missing `EngineSessionService` / `VpsDemoService::initialized()`)

**Step 4: Commit**

```bash
git add demo/vpsdemo/tests/vps_session_service_test.cpp demo/vpsdemo/CMakeLists.txt
git commit -m "feat: add vpsdemo session service test scaffold"
```

### Task 2: Implement EngineSessionService and demo initialization

**Files:**
- Create: `demo/vpsdemo/include/vps_session_service.h`
- Create: `demo/vpsdemo/src/vps_session_service.cpp`
- Modify: `demo/vpsdemo/include/vpsdemo_service.h`
- Modify: `demo/vpsdemo/src/vpsdemo_service.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Implement the provider interface + service**

`demo/vpsdemo/include/vps_session_service.h`

```cpp
#ifndef VPSDEMO_VPS_SESSION_SERVICE_H_
#define VPSDEMO_VPS_SESSION_SERVICE_H_

#include <memory>
#include <mutex>

#include "memrpc/client/demo_bootstrap.h"
#include "memrpc/core/bootstrap.h"
#include "memrpc/core/types.h"
#include "memrpc/server/rpc_server.h"

namespace vpsdemo {

class VpsDemoService;

class VesSessionProvider {
 public:
    virtual ~VesSessionProvider() = default;
    virtual memrpc::StatusCode OpenSession(memrpc::BootstrapHandles* handles) = 0;
    virtual memrpc::StatusCode CloseSession() = 0;
};

class EngineSessionService final : public VesSessionProvider {
 public:
    explicit EngineSessionService(VpsDemoService* service);

    memrpc::StatusCode OpenSession(memrpc::BootstrapHandles* handles) override;
    memrpc::StatusCode CloseSession() override;

 private:
    VpsDemoService* service_ = nullptr;
    std::shared_ptr<memrpc::PosixDemoBootstrapChannel> bootstrap_;
    std::unique_ptr<memrpc::RpcServer> rpc_server_;
    std::mutex init_mutex_;
    bool initialized_ = false;
};

}  // namespace vpsdemo

#endif  // VPSDEMO_VPS_SESSION_SERVICE_H_
```

`demo/vpsdemo/src/vps_session_service.cpp`

```cpp
#include "vps_session_service.h"

#include <unistd.h>

#include "memrpc/server/rpc_server.h"
#include "vpsdemo_service.h"
#include "virus_protection_service_log.h"

namespace vpsdemo {

namespace {
void CloseHandles(memrpc::BootstrapHandles* handles) {
    if (handles == nullptr) return;
    int* fds[] = {
        &handles->shm_fd,
        &handles->high_req_event_fd,
        &handles->normal_req_event_fd,
        &handles->resp_event_fd,
        &handles->req_credit_event_fd,
        &handles->resp_credit_event_fd,
    };
    for (int* fd : fds) {
        if (fd != nullptr && *fd >= 0) {
            close(*fd);
            *fd = -1;
        }
    }
}
}  // namespace

EngineSessionService::EngineSessionService(VpsDemoService* service)
    : service_(service),
      bootstrap_(std::make_shared<memrpc::PosixDemoBootstrapChannel>()) {}

memrpc::StatusCode EngineSessionService::OpenSession(memrpc::BootstrapHandles* handles) {
    if (handles == nullptr) {
        return memrpc::StatusCode::InvalidArgument;
    }
    std::lock_guard<std::mutex> lock(init_mutex_);
    if (!initialized_) {
        const memrpc::StatusCode open_status = bootstrap_->OpenSession(handles);
        if (open_status != memrpc::StatusCode::Ok) {
            HLOGE("bootstrap OpenSession failed");
            return open_status;
        }

        const memrpc::BootstrapHandles server_handles = bootstrap_->serverHandles();
        rpc_server_ = std::make_unique<memrpc::RpcServer>(server_handles);
        if (service_ != nullptr) {
            service_->RegisterHandlers(rpc_server_.get());
        }
        const memrpc::StatusCode start_status = rpc_server_->Start();
        if (start_status != memrpc::StatusCode::Ok) {
            HLOGE("RpcServer start failed");
            CloseHandles(handles);
            return start_status;
        }

        if (service_ != nullptr) {
            service_->Initialize();
        }
        initialized_ = true;
        HLOGI("EngineSessionService initialized");
        return memrpc::StatusCode::Ok;
    }
    return bootstrap_->OpenSession(handles);
}

memrpc::StatusCode EngineSessionService::CloseSession() {
    return memrpc::StatusCode::Ok;
}

}  // namespace vpsdemo
```

**Step 2: Update `VpsDemoService` to be idempotently initialized**

`demo/vpsdemo/include/vpsdemo_service.h`

```cpp
class VpsDemoService {
 public:
    void RegisterHandlers(memrpc::RpcServer* server);
    void Initialize();
    bool initialized() const;

 private:
    bool initialized_ = false;
};
```

`demo/vpsdemo/src/vpsdemo_service.cpp`

```cpp
void VpsDemoService::Initialize() {
    if (initialized_) {
        return;
    }
    initialized_ = true;
    HLOGI("VpsDemoService initialized");
}

bool VpsDemoService::initialized() const {
    return initialized_;
}
```

Update handler logic to call `Initialize()` in `DemoInit` for compatibility.

**Step 3: Add new sources to vpsdemo_lib**

In `demo/vpsdemo/CMakeLists.txt` add:

```cmake
    src/vps_session_service.cpp
```

**Step 4: Run tests to verify pass**

Run:
- `cmake -S demo/vpsdemo -B demo/vpsdemo/build -DVPSDEMO_ENABLE_TESTS=ON`
- `cmake --build demo/vpsdemo/build`
- `ctest --test-dir demo/vpsdemo/build -R vpsdemo_session_service_test --output-on-failure`

Expected: PASS

**Step 5: Commit**

```bash
git add demo/vpsdemo/include/vps_session_service.h demo/vpsdemo/src/vps_session_service.cpp \
  demo/vpsdemo/include/vpsdemo_service.h demo/vpsdemo/src/vpsdemo_service.cpp \
  demo/vpsdemo/CMakeLists.txt
git commit -m "feat: add engine session service for vpsdemo"
```

### Task 3: Rename SA class and wire provider

**Files:**
- Modify: `demo/vpsdemo/include/virus_executor_service.h`
- Modify: `demo/vpsdemo/src/virus_executor_service.cpp`
- Modify: `demo/vpsdemo/src/vpsdemo_engine_sa.cpp`
- Modify: `demo/vpsdemo/CMakeLists.txt`

**Step 1: Rename SA class and files**

Run:
- `git mv demo/vpsdemo/include/vps_bootstrap_stub.h demo/vpsdemo/include/virus_executor_service.h`
- `git mv demo/vpsdemo/src/vps_bootstrap_stub.cpp demo/vpsdemo/src/virus_executor_service.cpp`

Update include guards, filenames, and class name `VpsBootstrapStub` -> `VirusExecutorService` in both files.

**Step 2: Update SA class to use provider**

`demo/vpsdemo/include/virus_executor_service.h`

```cpp
#include "vps_session_service.h"

class VirusExecutorService : public OHOS::SystemAbility,
                              public OHOS::IRemoteStub<IVpsBootstrap> {
 public:
    explicit VirusExecutorService(std::shared_ptr<VesSessionProvider> provider);
    // ...

 private:
    std::shared_ptr<VesSessionProvider> provider_;
};
```

`demo/vpsdemo/src/virus_executor_service.cpp`

```cpp
VirusExecutorService::VirusExecutorService(std::shared_ptr<VesSessionProvider> provider)
    : OHOS::SystemAbility(VPS_BOOTSTRAP_SA_ID, true),
      provider_(std::move(provider)) {}

// In AcceptLoop, before SendFds:
memrpc::BootstrapHandles handles{};
const memrpc::StatusCode open_status = provider_->OpenSession(&handles);
if (open_status != memrpc::StatusCode::Ok) {
    close(client_fd);
    continue;
}
// then send handles via SendFds
```

Also have `OpenSession()` direct path delegate to `provider_->OpenSession()`.

Update `demo/vpsdemo/CMakeLists.txt` to replace `src/vps_bootstrap_stub.cpp` with
`src/virus_executor_service.cpp` in `vpsdemo_lib`.

**Step 3: Update engine SA to construct provider and SA**

`demo/vpsdemo/src/vpsdemo_engine_sa.cpp`

```cpp
vpsdemo::VpsDemoService service;
auto session_service = std::make_shared<vpsdemo::EngineSessionService>(&service);

auto stub = std::make_shared<vpsdemo::VirusExecutorService>(session_service);
```

Remove the old `PosixDemoBootstrapChannel` / `RpcServer` bootstrap code from `main` (now owned by `EngineSessionService`).

**Step 4: Build and run the vpsdemo test**

Run:
- `cmake -S demo/vpsdemo -B demo/vpsdemo/build -DVPSDEMO_ENABLE_TESTS=ON`
- `cmake --build demo/vpsdemo/build`
- `ctest --test-dir demo/vpsdemo/build -R vpsdemo_session_service_test --output-on-failure`

Expected: PASS

**Step 5: Commit**

```bash
git add demo/vpsdemo/include/virus_executor_service.h demo/vpsdemo/src/virus_executor_service.cpp \
  demo/vpsdemo/src/vpsdemo_engine_sa.cpp demo/vpsdemo/CMakeLists.txt
git commit -m "feat: wire vpsdemo SA to session provider"
```

### Task 4: Remove client-side InitEngine usage + manual verification

**Files:**
- Modify: `demo/vpsdemo/src/vpsdemo_client.cpp`

**Step 1: Remove InitEngine call**

Delete the `InitEngine()` invocation in the first session.

**Step 2: Build and run demo**

Run:
- `cmake -S demo/vpsdemo -B demo/vpsdemo/build`
- `cmake --build demo/vpsdemo/build`
- `./demo/vpsdemo/build/vpsdemo_supervisor`

Expected: demo completes without requiring client `InitEngine` and logs normal `ScanFile`/`UpdateFeatureLib` outputs.

**Step 3: Commit**

```bash
git add demo/vpsdemo/src/vpsdemo_client.cpp
git commit -m "fix: drop vpsdemo client InitEngine call"
```
