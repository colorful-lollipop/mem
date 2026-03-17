# VES Bootstrap Channel Simplification Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove `VesClient` bootstrap wiring's dependency on concrete `VesControlProxy` and make the bootstrap facade depend only on `IVesControl`.

**Architecture:** Keep `IVesControl` as the remote control-plane contract. Introduce a bootstrap facade that implements `MemRpc::IBootstrapChannel`, owns callback retention and SA reload, and uses `IVesControl` for session/heartbeat operations while preserving optional concrete-proxy optimizations internally.

**Tech Stack:** C++17, OHOS SA mock, memrpc client recovery/watchdog, GoogleTest

---

### Task 1: Replace Concrete Proxy Bootstrap Wiring

**Files:**
- Modify: `virus_executor_service/include/transport/ves_control_proxy.h`
- Modify: `virus_executor_service/src/transport/ves_control_proxy.cpp`
- Modify: `virus_executor_service/include/client/ves_client.h`
- Modify: `virus_executor_service/src/client/ves_client.cpp`

**Step 1: Introduce a bootstrap facade API**

- Rename the public bootstrap adapter role to `VesBootstrapChannel`.
- Change its constructor and reload path to accept `OHOS::sptr<IVesControl>` plus a reload callback instead of `std::shared_ptr<VesControlProxy>`.
- Keep `VesControlProxy` available as the broker factory product, but stop exposing it as a client bootstrap requirement.

**Step 2: Preserve recovery semantics**

- Translate `Heartbeat` replies into `MemRpc::ChannelHealthResult` inside the bootstrap facade.
- Retain `HealthSnapshotCallback` and `EngineDeathCallback` in the bootstrap facade and rebind them after reload.
- Preserve best-effort early engine-death signaling by attaching a death recipient and, when the interface instance is actually backed by `VesControlProxy`, forwarding the callback there too.

**Step 3: Update `VesClient` composition**

- Store `OHOS::sptr<IVesControl>` for control operations.
- Store `std::shared_ptr<VesBootstrapChannel>` for `RpcClient`.
- Route `AnyCall` through the interface pointer rather than the concrete proxy.

### Task 2: Add Focused Regression Coverage

**Files:**
- Modify: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`

**Step 1: Add an interface-backed bootstrap test**

- Create a `VesBootstrapChannel` from an `IVesControl` pointer returned by `iface_cast`.
- Verify `OpenSession`, `CheckHealth`, and `CloseSession` work without any `dynamic_pointer_cast<VesControlProxy>` in the test.

### Task 3: Verify the Affected Paths

**Files:**
- Test: `virus_executor_service/tests/unit/ves/ves_heartbeat_test.cpp`
- Test: `virus_executor_service/tests/unit/ves/ves_health_subscription_test.cpp`
- Test: `virus_executor_service/tests/unit/ves/ves_policy_test.cpp`

**Step 1: Build and run focused tests**

Run:

```bash
tools/build_and_test.sh --test-regex 'ves_(heartbeat|health_subscription|policy)'
```

Expected:
- build succeeds
- affected VES unit tests pass

