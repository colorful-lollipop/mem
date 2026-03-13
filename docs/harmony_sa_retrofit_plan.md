# HarmonyOS SystemAbility 接入改造总结与计划

## 1. 目标和当前判断

当前 `memrpc` 的共享内存传输核心已经和平台拉起机制解耦，真正需要替换的是 bootstrap 层，而不是 `EngineClient` / `EngineServer` 本体。

这意味着后续接入 HarmonyOS `SystemAbility` 时，最合理的方向是：

- 保留现有 `EngineClient <-> IBootstrapChannel <-> EngineServer` 边界
- 把 SA 发现、启动、死亡监听、fd 交换都封装进 `SaBootstrapChannel`
- 让“主进程 SA”作为扫描请求发起方
- 让“子进程 SA”作为扫描引擎承载方
- SA 只负责控制面和 bootstrap
- 共享内存 ring + `eventfd` 继续作为数据面

换句话说，后续不是“把扫描流量改成 Binder 调用”，而是“用 SA/Binder 建立和维护共享内存扫描通道”。

## 2. 当前仓库里已经具备的接入基础

### 2.1 平台边界已经收敛在 bootstrap 层

当前接口抽象在 `include/memrpc/bootstrap.h`：

- `StartEngine()`
- `Connect(BootstrapHandles*)`
- `NotifyPeerRestarted()`
- `SetEngineDeathCallback()`

`EngineClient` 只依赖这组能力：

1. 初始化时注册死亡回调
2. 调 `StartEngine()`
3. 调 `Connect()` 获取 `BootstrapHandles`
4. 把句柄附着到 `Session`
5. 子进程死亡后让旧 `session` 失效，下一次 `Scan()` 懒恢复

这说明正式鸿蒙接入时，不需要把 SA 逻辑扩散到扫描主流程。

### 2.2 `sa_bootstrap` 现在还是 fake，但接口形状是对的

`src/bootstrap/sa_bootstrap.cpp` 当前只是把调用透传给 `PosixDemoBootstrapChannel`，本质是 Linux 开发态 fake SA。

但这层已经提前固定了正式实现最重要的契约：

- `SaBootstrapConfig` 已经预留了 `service_name`、`instance_name`、`lazy_connect`
- `tests/sa_bootstrap_stub_test.cpp` 固定了 `StartEngine()` / `Connect()` 的句柄交换语义
- `tests/bootstrap_callback_test.cpp` 固定了死亡回调、会话切换、迟到死亡事件处理语义

后续正式 SA 实现必须守住这些语义，避免影响 `EngineClient` 的恢复模型。

## 3. 从 security_guard 抽出来的可复用经验

参考代码目录：`/root/code/oh/base/security/security_guard`

### 3.1 SA 注册模式

`security_guard` 里的服务端都采用同一套模式：

- service 类同时继承 `SystemAbility` 和对应的 `Stub`
- 用 `REGISTER_SYSTEM_ABILITY_BY_ID(...)` 注册
- 在 `OnStart()` 里做初始化，然后 `Publish(this)`
- 在 `sa_profile/*.json` 里声明 `process`、`name`、`libpath`、`run-on-create`

典型例子：

- `services/data_collect/sa/data_collect_manager_service.cpp`
- `services/risk_classify/src/risk_analysis_manager_service.cpp`
- `services/security_collector/src/security_collector_manager_service.cpp`
- `sa_profile/3523.json`
- `sa_profile/3524.json`
- `sa_profile/3525.json`

这个模式适合我们后续的新扫描引擎 SA。

### 3.2 调用 SA 的客户端模式

`security_guard` 的客户端不是到处直接写 `GetSystemAbility()`，而是通常先包一层 manager / loader：

- 直接 `GetSystemAbility()` 的例子：
  - `frameworks/common/collect/src/data_collect_manager.cpp`
  - `frameworks/common/classify/src/sg_classify_client.cpp`
- 需要按需拉起时，先 `CheckSystemAbility()`，不存在再 `LoadSystemAbility()`：
  - `frameworks/common/collector/src/collector_service_loader.cpp`

这个经验很关键：

- 老 SA 不应该直接散落一堆 Binder 细节
- 应该新增一层 `MemRpcSaClient` 或 `EngineBootstrapClient`
- 这层负责 `Get/LoadSystemAbility`、`iface_cast`、重连、错误码转换

### 3.3 callback / death recipient 模式

`security_guard` 大量使用 callback stub + death recipient 来处理异步返回和服务死亡：

- 服务端收到回调对象后 `iface_cast<...>(cb)` 再回调
- 客户端对 SA remote object 安装 `AddDeathRecipient`
- 远端死亡后清理本地状态并重建订阅或连接

典型例子：

- `frameworks/common/collect/src/data_collect_manager.cpp`
- `frameworks/common/collector/src/collector_manager.cpp`
- `frameworks/common/collect/src/event_subscribe_client.cpp`
- `services/data_collect/sa/data_collect_manager_service.cpp`
- `services/risk_classify/src/risk_analysis_manager_service.cpp`

这和我们现有 `SetEngineDeathCallback()` + `HandleEngineDeath()` 非常契合。

可以直接映射为：

- Binder death recipient / SA 回调触发 `SaBootstrapChannel`
- `SaBootstrapChannel` 再触发 `EngineClient::HandleEngineDeath()`
- `EngineClient` 继续沿用当前“旧 session 失败，下一次 Scan 懒恢复”的语义

### 3.4 老 SA 调新 SA 的常见组织方式

`security_guard` 给出的经验不是“服务之间互相 include service 实现”，而是：

1. 新 SA 暴露独立接口定义
2. 生成或实现自己的 stub/proxy
3. 老 SA 或公共框架层通过 `GetSystemAbility`/`LoadSystemAbility` 拿 proxy
4. 老 SA 只依赖 proxy/interface，不直接依赖新 SA 的 service 实现

这就是我们后续“原来的 SA 调新 SA”的推荐方式。

## 4. 推荐的目标架构

建议采用“双 SA + 共享内存数据面”的结构。

```text
旧主进程 SA
  -> MemRpcSaClient / SaBootstrapChannel
  -> GetSystemAbility / LoadSystemAbility
  -> 新扫描引擎 SA
     -> 创建/维护共享内存 + eventfd
     -> 导出 BootstrapHandles
     -> 承载 EngineServer + 真实扫描 Handler

扫描请求数据面：
旧主进程 SA <-> shared memory ring + eventfd <-> 新扫描引擎 SA

控制面：
旧主进程 SA <-> Binder / SystemAbility <-> 新扫描引擎 SA
```

### 4.1 为什么不建议把扫描正文全走 SA IPC

因为当前框架的优势就在于：

- 大 payload 不走 Binder 拷贝
- 服务端仍然可以做双队列和线程池调度
- 旧业务接口仍可保持同步 `Scan()`

如果把扫描正文完全改成 SA 同步调用，相当于把 `memrpc` 的价值抹掉了。

### 4.2 新 SA 的职责

建议新 SA 只承担以下职责：

- 对外暴露 bootstrap/control 接口
- 在需要时创建或重建共享内存和事件句柄
- 返回 `BootstrapHandles`
- 提供客户端死亡/服务死亡通知机制
- 内部托管 `EngineServer`
- 在引擎重启后切换新的 `session_id`

不建议让新 SA 直接暴露“逐文件扫描结果”作为 Binder 主数据通路。

### 4.3 旧 SA 的职责

旧 SA 继续承担：

- 接业务扫描请求
- 做旧接口到 `memrpc::ScanRequest` 的转换
- 持有 `EngineClient`
- 通过 `SaBootstrapChannel` 获取/恢复会话
- 保持对上层的同步调用语义

## 5. 我们项目里推荐新增的 Harmony SA 分层

建议后续按下面几层落：

### 5.1 `interfaces/` 层

新增一个 SA 接口，例如：

- `IEngineBootstrap`
- `IEngineBootstrapCallback`

接口建议只覆盖控制面：

- `StartEngineIfNeeded()`
- `OpenSession(...)`
- `RegisterClientCallback(...)`
- `NotifyClientSessionClosed(...)`

其中 `OpenSession(...)` 负责把 `shmFd`、`eventfd`、`session_id`、协议版本等信息返回给客户端。

### 5.2 `services/engine_sa/` 层

新增引擎 SA service：

- 继承 `SystemAbility`
- 继承 `IEngineBootstrapStub`
- `OnStart()` 里初始化扫描引擎运行环境
- `Publish(this)`
- 内部持有 `EngineServer`
- 维护当前 `BootstrapHandles` 和 `session_id`

### 5.3 `frameworks/common/engine_client/` 层

新增客户端适配层，参考 `security_guard/frameworks/common/...`：

- 负责 `GetSystemAbility` / `LoadSystemAbility`
- 做 `iface_cast`
- 注册 death recipient
- 注册 callback stub
- 向 `SaBootstrapChannel` 提供正式鸿蒙实现支撑

### 5.4 `src/bootstrap/sa_bootstrap.cpp`

把当前 fake 实现改造成双态实现：

- Linux / 单元测试环境：继续走 `PosixDemoBootstrapChannel`
- HarmonyOS 正式环境：走 `EngineBootstrapClient + Binder fd 交换`

这样能最大化复用现有测试和 Linux 开发体验。

## 6. 原来的 SA 怎么调用新 SA

这是本次改造里最关键的接口组织问题。

### 6.1 推荐调用路径

不建议：

- 旧 SA 直接 include 新 SA service 头文件
- 旧 SA 直接操作 Binder `MessageParcel`
- 旧 SA 自己维护 fd/session/death recipient 细节

建议：

1. 新 SA 定义独立 interface/stub/proxy
2. 提供一个公共 client wrapper
3. 旧 SA 只依赖 wrapper 或 `SaBootstrapChannel`
4. 旧 SA 继续通过 `EngineClient::Scan()` 发起扫描

推荐调用链如下：

```text
旧 SA 业务代码
  -> EngineClient
  -> SaBootstrapChannel
  -> EngineBootstrapClient
  -> GetSystemAbility / LoadSystemAbility
  -> 新扫描引擎 SA proxy
  -> OpenSession() 返回 fd + session_id
  -> EngineClient 附着共享内存后执行 Scan()
```

### 6.2 如果旧 SA 本身也是 SystemAbility

这不会改变模式。

SystemAbility 调另一个 SystemAbility，仍然是：

- 通过 `SystemAbilityManagerClient::GetInstance().GetSystemAbilityManager()`
- `GetSystemAbility(new_sa_id)` 或 `LoadSystemAbility(new_sa_id, callback)`
- `iface_cast<INewSaInterface>(object)`
- 通过 proxy 调用新 SA

也就是说，“旧 SA 是 SA” 和 “普通 native client 调 SA” 在调用模式上没有本质区别，差别只在权限、生命周期和部署位置。

### 6.3 是否需要 `LoadSystemAbility`

如果新扫描 SA 是常驻的，优先：

- `GetSystemAbility()`

如果新扫描 SA 是按需拉起的，参考 `CollectorServiceLoader`：

- 先 `CheckSystemAbility()`
- 不存在时 `LoadSystemAbility()`
- 用 load callback 等待拉起完成

建议我们优先支持这两种模式，但在业务封装层屏蔽差异。

## 7. 新 SA 接口设计建议

建议不要把现有 `IBootstrapChannel` 直接暴露成 Binder 接口，而是定义更贴近 SA 语义的接口。

建议的 Binder 接口职责如下：

- `EnsureEngineReady()`
  - 确保引擎 SA 已准备好共享内存会话
- `AcquireSession()`
  - 返回 `shmFd`、`highReqEventFd`、`normalReqEventFd`、`respEventFd`
  - 返回 `session_id`、`protocol_version`
- `RegisterClientObserver(callback)`
  - 用于服务死亡、session 更新、可选的主动通知
- `PingSession(session_id)`
  - 可选，用于调试和一致性校验

这样做的原因是：

- Binder 接口表达的是“会话管理”
- `memrpc` 内部接口表达的是“bootstrap 通道”
- 两者职责接近，但不必强行一一同名

在 `SaBootstrapChannel` 内部做映射更稳。

## 8. fd 交换和死亡通知的改造要点

### 8.1 fd 交换

鸿蒙正式接入时，核心难点不是扫描逻辑，而是把这些句柄通过 Binder 正确传过去：

- `shmFd`
- `highReqEventFd`
- `normalReqEventFd`
- `respEventFd`

建议把 fd 交换集中在新 SA 的 `AcquireSession()` 中一次性完成，避免分多次调用造成 session 不一致。

### 8.2 session 一致性

每次返回句柄时必须同时返回：

- `session_id`
- `protocol_version`

客户端必须把句柄组和 `session_id` 视为原子快照。

这和当前测试约束一致：

- 新会话必须生成新的 `session_id`
- 迟到的旧会话死亡通知不能冲掉当前会话

### 8.3 death recipient 和 callback 的分工

建议两条线都保留：

- death recipient
  - 用于感知 SA remote object 彻底死亡
- callback / observer
  - 用于感知“服务活着但 session 已切换”这类 finer-grained 事件

然后统一收口到 `SaBootstrapChannel::SetEngineDeathCallback()`。

## 9. 分阶段实施计划

### Phase 1: 固化 bootstrap 抽象，不改扫描核心

- 保持 `EngineClient` / `EngineServer` 不动
- 明确 `BootstrapHandles` 是否需要补充元信息
- 在 `SaBootstrapChannel` 内预留正式鸿蒙分支
- 增加文档和设计约束，避免 SA 细节渗透进扫描逻辑

### Phase 2: 定义新 SA 接口和 profile

- 新增新扫描引擎 SA 的 interface/stub/proxy
- 新增 service 实现
- 新增 `sa_profile/<new_id>.json`
- 明确 `run-on-create` 策略
- 明确权限和进程归属

### Phase 3: 完成 `SaBootstrapChannel` 的 Harmony 实现

- 接 `GetSystemAbility` / `LoadSystemAbility`
- 完成 fd + `session_id` 交换
- 完成 death recipient / callback
- 保留 Linux fake fallback

### Phase 4: 旧 SA 接入新 SA

- 旧 SA 引入 `SaBootstrapChannel`
- 旧接口继续调用 `EngineClient::Scan()`
- 旧 SA 不直接感知共享内存创建细节
- 验证首次拉起、引擎重启、会话恢复、超时失败语义

### Phase 5: 补完验证

- 单元测试继续覆盖 fake SA 语义
- 鸿蒙侧补系统集成测试
- 重点验证：
  - 首次 `Get/LoadSystemAbility`
  - 句柄交换
  - 扫描成功路径
  - 引擎 SA 死亡
  - 重连后新的 `session_id`
  - 旧请求失败但新请求可恢复

## 10. 推荐优先改造的当前仓库入口

优先级从高到低：

1. `include/memrpc/sa_bootstrap.h`
2. `src/bootstrap/sa_bootstrap.cpp`
3. `include/memrpc/bootstrap.h`
4. `tests/sa_bootstrap_stub_test.cpp`
5. `tests/bootstrap_callback_test.cpp`
6. `docs/sa_integration.md`

其中：

- `sa_bootstrap.*` 是正式鸿蒙适配的主入口
- `bootstrap.h` 是控制 bootstrap 契约的边界
- 两个测试文件负责守住迁移语义

## 11. 最终建议

建议我们按下面原则推进：

- 不改 `memrpc` 数据面，只补 SA 控制面
- 不让旧 SA 直接碰 Binder 细节，而是通过 `SaBootstrapChannel` / wrapper 接新 SA
- 新 SA 只负责 bootstrap、session、死亡通知、引擎托管
- 共享内存 + `eventfd` 继续承载扫描数据流
- 用 `security_guard` 的 `Publish`、`Get/LoadSystemAbility`、callback、death recipient 模式作为模板

如果按这个边界改，后续“新 SA 接入”和“原有 SA 调新 SA”会是一次局部平台适配，而不是对扫描框架做结构性重写。
