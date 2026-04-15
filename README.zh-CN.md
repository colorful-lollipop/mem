# MemRPC & Virus Executor Service

一个基于 **C++17** 的共享内存 RPC 框架，并附带完整的参考应用（Virus Executor Service），支持在 **Linux** 与 **OpenHarmony** 上实现高吞吐、低延迟的跨进程通信。

> **一句话总结** — `memrpc` 通过无锁共享内存环形队列传输小包，当请求过大时自动回退到同步 `AnyCall` 旁路；会话生命周期、恢复策略和背压控制均由框架层统一处理，业务代码只需关心逻辑本身。
> 在 **OpenHarmony** 上，数据面继续沿用共享内存 + `eventfd`，控制面则通过 **SystemAbility (SA)** 桥接。

---

## 目录

- [项目简介](#项目简介)
- [核心特性](#核心特性)
- [支持平台](#支持平台)
- [架构概览](#架构概览)
- [快速开始](#快速开始)
  - [环境要求](#环境要求)
  - [构建](#构建)
  - [运行测试](#运行测试)
  - [运行示例](#运行示例)
- [项目结构](#项目结构)
- [测试矩阵](#测试矩阵)
- [相关文档](#相关文档)
- [OpenHarmony 部署](#openharmony-部署)
- [许可证](#许可证)

---

## 项目简介

本仓库包含两大部分：

1. **`memrpc/`** — 可复用的共享内存 RPC 框架
   - 采用固定大小的 Ring Entry，payload 直接内联，无需额外的外部 slot/pool。
   - 基于 `eventfd` 实现无锁的请求/响应环形队列唤醒。
   - 提供类型化的 C++ 封装（`TypedFuture`、`InvokeTypedSync`），自动完成编解码。
   - 客户端内置统一恢复状态机：引擎死亡检测、冷却期、空闲关闭、会话重连。

2. **`virus_executor_service/`** — 构建在 `memrpc` 之上的参考应用（病毒执行服务，简称 **VES**）
   - 演示如何将业务 handler 同时注册到 MemRPC 主路径和 `AnyCall` 兜底路径，而无需写两套传输逻辑。
   - 提供完整的 **testkit**（`Echo`、`Add`、`Sleep`、故障注入 handler），用最小、最确定的示例帮助你理解框架行为。

框架核心与平台无关，同一套 `RpcClient` / `RpcServer` / `Session` 代码既可在 Linux 开发工作站上运行，也可直接部署到 OpenHarmony 设备；只有 **bootstrap 适配层**（客户端如何发现服务端、如何交换文件描述符）是平台相关的。

---

## 核心特性

- ⚡ **零拷贝 fast path**：小包直接走共享内存 ring，无需拷贝  
- 🔀 **双路径传输**：请求超过 inline entry 限制时，自动 fallback 到同步 `AnyCall` 控制面旁路  
- 🛡️ **保守背压**：ring 容量和 worker 队列均有界，拒绝无界静默排队  
- 🔄 **客户端统一恢复**：`RpcClient` 持有会话生命周期、健康检查和重放策略；业务层只需配置 `RecoveryPolicy`  
- 🧪 **测试体系完整**：覆盖单元测试、集成测试、确定性测试（DT）、压力测试和模糊测试（Fuzz）  
- 📱 **OpenHarmony 原生架构**：SystemAbility 控制面 + 共享内存数据面；核心框架迁移到 OpenHarmony 时**无需任何改动**  

---

## 支持平台

| 平台 | 状态 | 说明 |
|------|------|------|
| **Linux (x86_64 / AArch64)** | ✅ 主要开发与测试环境 | 完整 CMake 构建、全部测试、Supervisor 示例 |
| **OpenHarmony** | ✅ 目标部署平台 | SA bootstrap 适配 + `init` 子进程管理；详见 [OpenHarmony 部署](#openharmony-部署) |

---

## 架构概览

```
┌─────────────────┐      类型化 API      ┌─────────────────┐
│   VesClient     │ ◄──────────────────► │  ScanFile()     │
│  (facade +      │                      │  Heartbeat()    │
│   恢复策略)      │                      │  AnyCall()      │
└────────┬────────┘                      └─────────────────┘
         │
         │  小包 ──► RpcClient ──► Request Ring  ──► RpcServer ──► handler
         │  大包 ──► AnyCall() ──► 控制面代理
         │
         ▼
┌─────────────────┐
│  memrpc Session │  ← 共享内存、mmap、eventfd、attach/校验
└─────────────────┘
```

**核心分层边界：**

- `memrpc::Session` — 负责 mmap、ring 游标、协议版本校验、客户端 attach。
- `memrpc::RpcClient` — 负责 pending 表、超时、响应分发线程、恢复状态机。
- `memrpc::RpcServer` — 负责 dispatcher、高/普通优先级 executor、response writer 线程。
- `VesEngineService` — 纯业务逻辑（如 `ScanFile`）。
- `EngineSessionService` — 将业务 handler 同时挂接到 `RpcServer` 和 `AnyCall` 两条通路。

想了解更多实现细节，请参阅 [`docs/architecture.md`](docs/architecture.md)。

---

## 快速开始

### 环境要求

- **Linux** 开发环境
- **Clang** 工具链（`clang`、`clang++`）
- **CMake** ≥ 3.16
- **Ninja**（推荐）
- **GoogleTest**（如要运行测试）

> 构建系统**强制使用 Clang**，主线配置不支持 GCC。

### 构建

最推荐的零门槛入口脚本：

```bash
./tools/build_and_test.sh
```

该脚本会自动完成配置、编译，并将完整测试套件运行到 `build_ninja/` 目录下。

常用变体：

```bash
# 彻底重建
./tools/build_and_test.sh --clean

# 开启更严格的警告
./tools/build_and_test.sh --strict

# AddressSanitizer + UBSan
./tools/build_and_test.sh --asan

# ThreadSanitizer
./tools/build_and_test.sh --tsan

# 只构建不跑测试
./tools/build_and_test.sh --build-only
```

如果你更喜欢直接调用 CMake：

```bash
cmake -S . -B build_ninja -G Ninja \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DVIRUS_EXECUTOR_SERVICE_ENABLE_TESTS=ON
cmake --build build_ninja --parallel
```

### 运行测试

运行全部测试：

```bash
ctest --test-dir build_ninja --output-on-failure --parallel
```

按模块或标签运行：

```bash
# 仅框架层测试
ctest --test-dir build_ninja -R memrpc_

# 仅应用层测试
ctest --test-dir build_ninja -R virus_executor_service_

# 压力测试
ctest --test-dir build_ninja -L stress

# 确定性测试（DT）
ctest --test-dir build_ninja -L dt

# 集成测试
ctest --test-dir build_ninja -L integration

# 模糊测试（Fuzz）
ctest --test-dir build_ninja -L fuzz
```

提交前的推荐门禁：

```bash
# 标准门禁
./tools/push_gate.sh

# 深度门禁（涉及并发、生命周期、恢复逻辑时强烈推荐）
./tools/push_gate.sh --deep
```

> **注意**：大量测试依赖 `shm_open`、`mmap`、Unix domain socket 和子进程。在沙箱化的 CI 环境中，可能需要提升权限才能跑通完整套件。

### 运行示例

构建完成后：

```bash
# 启动 Supervisor（服务生命周期示例）
./build_ninja/virus_executor_service/virus_executor_service_supervisor

# 独立客户端
./build_ninja/virus_executor_service/virus_executor_service_client

# 压力客户端
./build_ninja/virus_executor_service/virus_executor_service_stress_client --threads 2 --iterations 100

# Testkit 压力运行器
./build_ninja/virus_executor_service/virus_executor_service_testkit_stress_runner
```

---

## 项目结构

```
memrpc/
├── include/memrpc/core/      # 协议、基础类型、编解码、bootstrap 抽象
├── include/memrpc/client/    # RpcClient、TypedFuture、类型化调用封装
├── include/memrpc/server/    # RpcServer、handler 接口
├── src/core/                 # Session、字节编解码、共享内存布局
├── src/client/               # RpcClient 实现（线程、恢复状态机）
├── src/server/               # RpcServer 实现（分发器、worker 池）
├── src/bootstrap/            # DevBootstrapChannel（本地开发/测试）
└── tests/                    # 框架层单元与集成测试

virus_executor_service/
├── include/client/           # VesClient 对外 API
├── include/service/          # 系统能力壳层接口
├── include/transport/        # 控制面代理、注册中心
├── include/ves/              # 业务协议（opcode、类型化结构体）
├── include/testkit/          # Testkit 头文件
├── src/client/               # VesClient 实现
├── src/service/              # VirusExecutorService、EngineSessionService
├── src/transport/            # VesBootstrapChannel、注册中心后端
├── src/testkit/              # Testkit 服务与客户端
├── src/app/                  # Supervisor、client、stress、DT 可执行入口
├── tests/unit/               # 单元测试
├── tests/integration/        # 集成测试
├── tests/dt/                 # 确定性/恢复测试
├── tests/stress/             # 压力/吞吐测试
├── tests/fuzz/               # Fuzz 目标
└── perf_baselines/           # 性能基线

docs/                         # 架构说明、测试指南、移植指南
tools/                        # build_and_test.sh、push_gate.sh、ci_sweep.sh
```

---

## 测试矩阵

| 测试类别 | 典型目标 | 建议运行时机 |
|----------|----------|--------------|
| **单元测试** | `memrpc_*_test`、`virus_executor_service_codec_test`、`virus_executor_service_policy_test` | 每次改动 |
| **集成测试** | `virus_executor_service_supervisor_integration_test` | 修改 `src/app/`、transport 或注册中心时 |
| **DT 测试** | `virus_executor_service_crash_recovery_test`、`memrpc_dt_stability_test` | 修改恢复、生命周期、健康检查逻辑时 |
| **压力测试** | `virus_executor_service_stress_test`、`testkit_throughput_test` | 修改并发调度、性能敏感路径时 |
| **模糊测试** | `virus_executor_service_codec_fuzz_smoke` | 修改编解码、协议结构体或解析器时 |

关于“改了哪份源码应该跑哪些测试”的详细指引，请参阅 [`docs/testing_guide.md`](docs/testing_guide.md)。

---

## 相关文档

- [`docs/architecture.md`](docs/architecture.md) — 框架与应用各层如何协作的完整说明。
- [`docs/testing_guide.md`](docs/testing_guide.md) — 测试理念、代表性用例、日常验证矩阵。
- [`docs/demo_guide.md`](docs/demo_guide.md) — 如何构建、运行并验证示例。
- [`docs/porting_guide.md`](docs/porting_guide.md) — 新业务如何接入 `memrpc`。
- [`docs/recovery_ownership.md`](docs/recovery_ownership.md) — 恢复路径的职责边界（`RpcClient` 与 `VesClient` 的分工）。
- [`docs/sa_integration.md`](docs/sa_integration.md) — OpenHarmony SystemAbility 接入指南。

---

## OpenHarmony 部署

`memrpc` 的设计理念是：**框架核心**与**业务逻辑**均不依赖操作系统特定的传输细节，因此在 OpenHarmony 上部署时**无需修改核心代码**。

OpenHarmony 部署步骤：

1. **保持 `memrpc` 核心不变** — `Session`、`RpcClient`、`RpcServer`、ring 和 `eventfd` 逻辑均无需移植。
2. **保持业务 handler 不变** — `VesEngineService` 和类型化协议层完全复用。
3. **接入 SA bootstrap 适配层** — 将 `DevBootstrapChannel` 替换为生产环境的 `VesBootstrapChannel`，通过 `GetSystemAbility` / `LoadSystemAbility` 获取 SA 代理，并在 Binder 上交换文件描述符。
4. **由 `init` 管理引擎子进程** — 子进程生命周期从本地 Supervisor 示例移交给 OpenHarmony 的 `init` 子系统。

简言之，OpenHarmony 支持**不是下游分支**，而是**仅替换 bootstrap 适配层**；共享内存数据面保持原样。

完整的 SA 集成设计请参阅 [`docs/sa_integration.md`](docs/sa_integration.md) 与 [`docs/harmony_sa_retrofit_plan.md`](docs/harmony_sa_retrofit_plan.md)。

---

## 许可证

请参阅仓库顶层许可证文件，了解本项目的使用条款。

---

*祝你 hacking 愉快！如果你在沙箱环境中遇到共享内存权限问题，请确保容器或 CI Runner 具备 `shm_open`、`mmap` 和 Unix-domain socket 所需的权限。*
