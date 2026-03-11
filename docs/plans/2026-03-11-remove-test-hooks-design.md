# Remove Test Hooks Design

## 背景

当前代码中存在两类 test hook：

- `core/session_test_hook.h`：用于 ring push/pop 的 trace 回调，测试用来统计线程分布。
- `PosixDemoBootstrapChannel` 的 dup 失败注入：用于模拟 fd 复制失败路径。

这些 hook 直接存在于主线实现中，虽然默认不启用，但仍然侵入核心逻辑。需求是彻底移除 hook 和依赖它们的测试，改由稳定性与并发测试覆盖。

## 目标

- 从主线移除所有 test hook 代码。
- 删除依赖 hook 的测试用例与构建目标。
- 保持主线功能行为不变（除去测试 hook 特性本身）。

## 非目标

- 不新增或改写稳定性 / 并发测试。
- 不引入编译开关或运行时开关来保留 hook。
- 不改动 shared memory 协议或 RPC 行为。

## 设计概览

### 1) 移除 ring trace hook

- 删除 `src/core/session_test_hook.h`。
- `src/core/session.cpp` 移除 trace callback 相关状态与调用。
- `PushRingEntry` / `PopRingEntry` 不再触发 trace。

### 2) 移除 fd 失败注入

- 删除 `PosixDemoBootstrapChannel` 中的 `dup_fail_after_count` 以及测试接口。
- `DuplicateHandles` 直接调用 `dup`，不再有失败注入逻辑。

### 3) 删除依赖测试

- 删除 `tests/memrpc/rpc_client_integration_test.cpp`。
- 删除 `tests/memrpc/response_queue_event_test.cpp`。
- 删除 `tests/memrpc/bootstrap_callback_test.cpp`。
- 清理 `tests/memrpc/CMakeLists.txt` 中的对应 test target。

## 风险与约束

- 失去 ring 操作线程追踪与 eventfd 触发策略的回归覆盖。
- 依赖稳定性/并发测试来兜底整体行为，但不会再校验内部实现细节。

## 验证

- `cmake -S . -B build`
- `cmake --build build`
- `ctest --test-dir build --output-on-failure`
