# VirusEngineManager 兼容层设计

## 目标

在不改变对外 `VirusEngineManager` 接口形状的前提下，把原来进程内直接加载引擎的实现切换到当前的 IPC + 共享内存框架。

兼容目标包括：

- `Init`
- `DeInit`
- `ScanFile`
- `ScanBehavior`
- `IsExistAnalysisEngine`
- `CreateAnalysisEngine`
- `DestroyAnalysisEngine`
- `RegisterScanResultListener`
- `UnRegisterScanResultListener`
- `UpdateFeatureLib`

同时满足以下约束：

- 主进程不感知多引擎细节
- 子进程保留多引擎结构，但全部使用 fake engine
- 原来的 signal 兜底逻辑改为框架内部处理子进程异常
- listener 采用广播事件队列，不做复杂跨进程对象传递

## 总体拆分

### 主进程

主进程保留 `VirusEngineManager` 作为业务兼容门面。

它负责：

- 暴露原始接口
- 管理 `Init/DeInit` 后台任务队列外观
- 拉起和连接子进程
- 发起同步 RPC
- 管理本地 listener 集合
- 维护异步事件分发线程
- 响应子进程异常退出

主进程不再负责：

- `engineLoaders_`
- `LibLoader`
- `IVirusEngine`
- 真正的扫描执行
- 进程内 signal 保护

### 子进程

子进程新增 `VirusEngineService` 作为真实执行层。

它负责：

- 保留 `engineLoaders_`
- 使用 fake `LibLoader`
- 使用 fake `IVirusEngine`
- 执行 `PerformInit/PerformDeInit`
- 执行所有同步引擎 RPC
- 生成 `BehaviorScanResult` 异步事件

子进程保留多引擎结构，但不接真实动态库。

## 多引擎模型

为了验证真实兼容性，demo 中保留：

- `VirusEngine`
- `engineLoaders_`
- 驻留 / 非驻留引擎的基本分组概念
- 遍历所有引擎执行 `ScanFile`

但全部用 fake 组件：

- `FakeLibLoader`
- `FakeVirusEngine`

fake engine 只用规则化逻辑返回结果，例如：

- 文件路径含 `virus` 返回感染
- 行为文本含 `startup` 返回高风险
- `CreateAnalysisEngine` / `DestroyAnalysisEngine` 维护一组 fake token 状态

## 旧接口到新框架的映射

### 同步 RPC

以下接口全部映射为 request/response RPC：

- `Init`
- `DeInit`
- `ScanFile`
- `ScanBehavior`
- `IsExistAnalysisEngine`
- `CreateAnalysisEngine`
- `DestroyAnalysisEngine`
- `UpdateFeatureLib`

主进程只做参数编解码和结果映射，子进程执行真实业务逻辑。

### 异步事件

以下接口不直接跨进程传 listener 对象：

- `RegisterScanResultListener`
- `UnRegisterScanResultListener`

改造为：

1. 主进程维护本地 listener 集合
2. 子进程只知道“当前是否开启行为结果上报”
3. 子进程把 `BehaviorScanResult` 写入异步事件队列
4. 主进程事件线程读取后广播给所有 listener

事件广播按用户确认采用“广播模型”，不按 accessToken 定向分发。

## 异常处理替换

原实现中的：

- `SignalGuard`
- `SignalHandler`
- `g_Tasks`
- `g_taskMutex`

不再保留在主进程版 `VirusEngineManager` 中。

替代方案：

- 子进程异常退出由框架/bootstrap 检测
- 当前 session 标记失效
- 所有 pending 同步请求立即失败
- 异步事件线程停止消费旧 session
- 下一次调用自动拉起新子进程并重连

如果需要表达“扫描中途引擎崩溃”的业务语义，主进程应在 RPC 失败路径中统一映射错误码，而不是依赖 signal handler 直接写业务状态。

## 数据类型兼容

这次 demo 应用层直接以用户给出的真实头文件为准：

- `virus_protection_service_define.h`
- `i_virus_engine.h`
- `lib_loader.h`
- `virus_engine_manager.h`

这些文件已保存到 `docs/reference/real_api/` 作为设计和实现参考，不直接接入编译。

demo 实现时会新增一套可编译的兼容头与 codec：

- `ScanTask`
- `ScanResult`
- `BehaviorScanResult`
- `BundleInfo`
- `BasicFileInfo`
- `EngineResult`

需要支持嵌套结构和 `vector<shared_ptr<T>>` 的简易二进制编解码。

## 文件结构建议

建议新增单独的应用层 demo 目录，避免污染 `memrpc` 核心目录：

```text
include/vps_demo/
  virus_protection_service_define.h
  i_virus_engine.h
  lib_loader.h
  virus_engine_manager.h
  virus_engine_service.h

src/vps_demo/
  virus_engine_manager.cpp
  virus_engine_service.cpp
  fake_lib_loader.cpp
  fake_virus_engine.cpp
  vps_codec.cpp
  vps_listener_bridge.cpp

demo/
  vps_manager_demo_main.cpp

tests/
  vps_manager_integration_test.cpp
  vps_listener_integration_test.cpp
```

## 实施顺序

### 第一阶段

先实现同步路径：

- `Init`
- `DeInit`
- `ScanFile`
- `ScanBehavior`
- `IsExistAnalysisEngine`
- `CreateAnalysisEngine`
- `DestroyAnalysisEngine`
- `UpdateFeatureLib`

### 第二阶段

再实现异步 listener 广播：

- `RegisterScanResultListener`
- `UnRegisterScanResultListener`
- 子进程事件队列
- 主进程广播线程

### 第三阶段

最后接入子进程异常恢复：

- 子进程死亡回调
- pending 请求失败
- 事件线程切 session
- 下次请求恢复

## 结论

这次兼容层的核心不是保留旧实现，而是保留旧接口。

最终形态应是：

- 旧接口不变
- 主进程只做 facade + IPC + listener 管理
- 子进程承载多引擎 fake 执行层
- 信号处理迁移为框架内部的子进程异常处理
