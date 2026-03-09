# VirusEngineManager 兼容层 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** 保留真实 `VirusEngineManager` 接口外观，同时把实现切换到基于共享内存和 IPC 的新框架，并提供多 fake engine 的 demo 应用层。

**Architecture:** 主进程只保留 `VirusEngineManager` facade、任务线程、IPC client 和 listener 广播；子进程新增 `VirusEngineService`，内部保留多引擎结构、fake loader 和 fake engine，并通过同步 RPC 和异步事件队列与主进程通信。

**Tech Stack:** C++17、shared memory、eventfd、现有 memrpc 核心、GTest、CMake、GN

---

### Task 1: 为应用层兼容类型增加失败测试

**Files:**
- Create: `tests/vps_types_test.cpp`
- Create: `include/vps_demo/virus_protection_service_define.h`
- Modify: `tests/CMakeLists.txt`
- Modify: `BUILD.gn`

**Step 1: Write the failing test**

新增测试覆盖：

- `ScanTask`
- `ScanResult`
- `BehaviorScanResult`
- `VirusEngine`
- `SUCCESS/FAILED`

至少验证默认构造和枚举值可用。

**Step 2: Run test to verify it fails**

Run: `cmake -S . -B build && cmake --build build --target memrpc_vps_types_test`

Expected:

- FAIL，头文件不存在

**Step 3: Write minimal implementation**

把真实接口里本次 demo 需要的类型裁剪到可编译版本，放入：

- `include/vps_demo/virus_protection_service_define.h`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_vps_types_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/vps_demo/virus_protection_service_define.h tests/vps_types_test.cpp tests/CMakeLists.txt BUILD.gn
git commit -m "feat: add vps demo compatibility types"
```

### Task 2: 为应用层 codec 增加失败测试

**Files:**
- Create: `tests/vps_codec_test.cpp`
- Create: `include/vps_demo/vps_codec.h`
- Create: `src/vps_demo/vps_codec.cpp`
- Modify: `src/CMakeLists.txt`
- Modify: `tests/CMakeLists.txt`
- Modify: `BUILD.gn`

**Step 1: Write the failing test**

覆盖：

- `ScanTask` round-trip
- `ScanResult` round-trip
- `BehaviorScanResult` round-trip

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_vps_codec_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

实现简易编解码，支持：

- `string`
- `vector`
- `shared_ptr<T>` 非空/空标记
- `EngineResult`
- `BasicFileInfo`
- `BundleInfo`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_vps_codec_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/vps_demo/vps_codec.h src/vps_demo/vps_codec.cpp tests/vps_codec_test.cpp src/CMakeLists.txt tests/CMakeLists.txt BUILD.gn
git commit -m "feat: add vps demo codecs"
```

### Task 3: 为子进程 fake 引擎服务增加失败测试

**Files:**
- Create: `tests/vps_service_test.cpp`
- Create: `include/vps_demo/i_virus_engine.h`
- Create: `include/vps_demo/lib_loader.h`
- Create: `include/vps_demo/virus_engine_service.h`
- Create: `src/vps_demo/fake_virus_engine.cpp`
- Create: `src/vps_demo/fake_lib_loader.cpp`
- Create: `src/vps_demo/virus_engine_service.cpp`

**Step 1: Write the failing test**

覆盖：

- `PerformInit` 后能创建多 fake engines
- `ScanFile` 会遍历多引擎
- `ScanBehavior` 只走动态引擎
- `Create/Destroy/IsExistAnalysisEngine` 维护 fake token 状态

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_vps_service_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

实现：

- fake `IVirusEngine`
- fake `LibLoader`
- `VirusEngineService`

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_vps_service_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add include/vps_demo/i_virus_engine.h include/vps_demo/lib_loader.h include/vps_demo/virus_engine_service.h src/vps_demo/fake_virus_engine.cpp src/vps_demo/fake_lib_loader.cpp src/vps_demo/virus_engine_service.cpp tests/vps_service_test.cpp
git commit -m "feat: add vps fake engine service"
```

### Task 4: 为兼容层 RPC 接口增加失败测试

**Files:**
- Modify: `src/core/protocol.h`
- Modify: `tests/protocol_layout_test.cpp`
- Create: `tests/vps_manager_integration_test.cpp`

**Step 1: Write the failing test**

覆盖：

- `Init/DeInit`
- `ScanFile`
- `ScanBehavior`
- `IsExistAnalysisEngine`
- `CreateAnalysisEngine`
- `DestroyAnalysisEngine`
- `UpdateFeatureLib`

通过主进程 `VirusEngineManager` 调用验证。

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_vps_manager_integration_test`

Expected:

- FAIL，缺少 opcode/client facade/service handler

**Step 3: Write minimal implementation**

增加：

- 新 opcode
- 子进程 handler 注册
- 主进程 manager facade

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_vps_manager_integration_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add src/core/protocol.h tests/protocol_layout_test.cpp tests/vps_manager_integration_test.cpp include/vps_demo/virus_engine_manager.h src/vps_demo/virus_engine_manager.cpp
git commit -m "feat: add vps manager sync rpc facade"
```

### Task 5: 为 listener 广播增加失败测试

**Files:**
- Create: `tests/vps_listener_integration_test.cpp`
- Create: `src/vps_demo/vps_listener_bridge.cpp`
- Modify: `include/vps_demo/virus_engine_manager.h`
- Modify: `src/vps_demo/virus_engine_manager.cpp`
- Modify: `src/vps_demo/virus_engine_service.cpp`

**Step 1: Write the failing test**

覆盖：

- 注册两个 listener
- 子进程产生一条 `BehaviorScanResult`
- 主进程都能收到广播
- 注销 listener 后不再收到

**Step 2: Run test to verify it fails**

Run: `cmake --build build --target memrpc_vps_listener_integration_test`

Expected:

- FAIL

**Step 3: Write minimal implementation**

实现：

- 异步事件队列
- 主进程事件线程
- 广播式 listener 分发

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R memrpc_vps_listener_integration_test`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/vps_listener_integration_test.cpp src/vps_demo/vps_listener_bridge.cpp include/vps_demo/virus_engine_manager.h src/vps_demo/virus_engine_manager.cpp src/vps_demo/virus_engine_service.cpp
git commit -m "feat: add vps listener broadcast bridge"
```

### Task 6: 为子进程异常恢复增加失败测试

**Files:**
- Modify: `tests/vps_manager_integration_test.cpp`
- Modify: `tests/vps_listener_integration_test.cpp`
- Modify: `src/vps_demo/virus_engine_manager.cpp`

**Step 1: Write the failing test**

覆盖：

- 子进程死亡后 pending `ScanFile` 失败
- 后续 `Init/ScanBehavior` 能恢复
- 事件线程能切换到新 session

**Step 2: Run test to verify it fails**

Run: `ctest --test-dir build --output-on-failure -R \"memrpc_vps_manager_integration_test|memrpc_vps_listener_integration_test\"`

Expected:

- FAIL

**Step 3: Write minimal implementation**

把旧 signal 处理替换为：

- 死亡回调
- pending 失败
- 事件线程重绑
- 下一次请求恢复

**Step 4: Run test to verify it passes**

Run: `ctest --test-dir build --output-on-failure -R \"memrpc_vps_manager_integration_test|memrpc_vps_listener_integration_test\"`

Expected:

- PASS

**Step 5: Commit**

```bash
git add tests/vps_manager_integration_test.cpp tests/vps_listener_integration_test.cpp src/vps_demo/virus_engine_manager.cpp
git commit -m "refactor: move vps crash handling into framework"
```

### Task 7: Demo、文档与最终验证

**Files:**
- Create: `demo/vps_manager_demo_main.cpp`
- Modify: `docs/architecture.md`
- Modify: `docs/porting_guide.md`
- Modify: `docs/demo_guide.md`

**Step 1: Baseline demo expectation**

Run: `./build/demo/memrpc_demo_dual_process`

Expected:

- 现有 memrpc demo 正常

**Step 2: Write minimal implementation**

增加：

- `vps_manager_demo_main.cpp`
- 中文文档更新

**Step 3: Run full verification**

Run:

- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
- `./build/demo/memrpc_demo_dual_process`
- `./build/demo/vps_manager_demo_main`

Expected:

- 全通过

**Step 4: Final commit**

```bash
git add demo/vps_manager_demo_main.cpp docs/architecture.md docs/porting_guide.md docs/demo_guide.md
git commit -m "feat: add vps manager compatibility demo"
```
