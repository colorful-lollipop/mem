# 部门好代码评选 PPT 最终投喂版 Prompt

以下整份内容，直接输入给 PPT agent。  
不要再要求读取源码，不要再要求补充文件。  
这份 prompt 已经把背景、故事线、架构、代码亮点、测试证据、性能口径、页面顺序全部整理好。  
目标是让一个完全不了解这个项目的人，也能快速看懂“这件事为什么难、为什么重要、代码为什么好”。

---

你现在是一名“华为风格技术评审材料总策划 + 系统软件架构专家 + 高级视觉设计总监 + 代码评审专家”。

请基于以下内容，生成一套中文 PPT 成稿方案，用于“部门好代码评选”。  
评审对象聚焦两个模块：

- `memrpc`：共享内存 RPC 框架
- `vpsdemo`：这里统一理解为当前主线代码中的 `virus_executor_service` 模块，以及它的 supervisor / engine / client / testkit / DT / stress 验证链路

这套 PPT 的要求不是“介绍项目做了什么”，而是让评委清楚看懂：

- 这次工作本质上是一次系统层重构，而不是普通功能开发
- 背景是原来的安全防护能力基本是单进程模式，现在第一次在鸿蒙系统层引入多进程架构
- 这次拆分不仅是架构重构，也是一次 DFX 和可靠性优化
- 项目把高性能通信、异常隔离、任意 crash 恢复、结构化 DFX、测试闭环一起做成了体系
- 代码不是“写得整齐”，而是确实解决了复杂系统问题，具备世界级工程特征

另外，这套材料的目标受众要上探到 CTO / 平台负责人 / 系统架构负责人层级。  
因此成稿必须同时满足两类人：

- 技术负责人：会看架构边界、状态机、线程布局、故障收敛、测试矩阵
- 高层决策者：会看平台价值、战略意义、长期复用、组织效率和风险收益

## 一、最重要的叙事要求

PPT 必须按“外行也能看懂”的方式组织，严格遵守以下顺序：

1. 先讲背景，不要一上来讲模块名
2. 先讲旧模式是什么，为什么有问题
3. 再讲这次为什么要拆成多进程
4. 再讲新架构怎么工作
5. 再讲关键代码为什么高级
6. 最后再讲性能、DFX、可靠性和评奖价值

也就是说，这套 PPT 必须首先解决“看懂”，其次才是“震撼”。

并且必须形成下面这条 CTO 级叙事主线：

1. 这件事解决了什么系统级问题
2. 为什么这是一次关键架构动作，而不是局部优化
3. 为什么这套实现具备平台底座价值，而不是一次性项目代码
4. 为什么它构成团队的工程护城河

## 二、必须使用的背景口径

请统一用以下背景，不要改写成很空的行业话术：

- 过去安全防护相关能力，主要是单进程架构
- 单进程模式的优点是简单，但问题也明显：故障隔离弱、DFX 粒度粗、崩溃影响范围大、演进空间有限
- 这次工作不是局部优化，而是系统层重构：将原本偏单体的处理模式，升级为“控制面 + 共享内存数据面 + 独立引擎进程”的多进程架构
- 这是一次架构升级，也是一次 DFX 升级
- 可以按内部评审口径表达为：鸿蒙系统层第一次引入这样一套面向共享内存数据主通路的多进程架构样板

注意：

- “鸿蒙第一次”“首个”这类表述可以作为内部评审材料口径使用
- 页面备注里可补一句：如需对外或正式发布，可替换为审批后的标准表述

## 三、PPT 的总体定位

这套材料要体现三层价值：

### 价值 1：重构价值

从单进程走向多进程，不是为了“拆着好看”，而是为了解决：

- 崩溃影响面过大
- 运行态不可观测
- 故障恢复无统一语义
- 性能路径与控制路径耦合

### 价值 2：工程价值

新架构不只是把进程拆开，而是同步建立了：

- 共享内存高性能主路径
- 控制面与数据面分离
- 统一恢复状态机
- 结构化心跳与 DFX
- 完整测试矩阵

### 价值 3：平台价值

这不是一个只服务当下业务的实现，而是一种系统层工程范式：

- 后续其他业务可复用
- 未来鸿蒙系统能力场景可迁移
- 团队可以复制这套方法论

### 价值 4：管理价值

从 CTO / 平台负责人视角，这次工作还有三个管理层收益：

- 风险收益：把大故障从“整体失控”降低为“局部异常 + 可恢复”
- 组织收益：把复杂恢复和通信能力沉淀到框架，降低后续业务重复建设成本
- 资产收益：沉淀出可继续复用的系统层底座，而不是一次性交付物

## 四、必须让读者一眼看懂的核心结论

整套 PPT 必须围绕下面 6 个判断展开：

### 判断 1

这次工作首先是“系统架构重构”，其次才是“通信框架建设”。

### 判断 2

单进程转多进程的最大价值，不只是结构变化，而是故障隔离、DFX 能力和可演进性的系统提升。

### 判断 3

`memrpc` 的价值，不只是快，而是把“高性能 + 高可靠 + 可恢复 + 可观测”同时做成了底座能力。

### 判断 4

`vpsdemo` 的价值，不只是把业务接上去，而是把鸿蒙 SA 风格控制面、共享内存主数据面和业务 facade 组织成了清晰范式。

### 判断 5

真正的技术难点已经被解决：

- 任意 crash 可统一处理
- 异常被隔离在边界内
- DFX 从散点日志变成结构化快照和事件

### 判断 6

这套代码的优秀之处，在于它形成了“可长期复用的系统层工程样板”。

## 五、必须用到的源码事实

以下事实已经整理好，PPT 直接使用，不要再让用户提供代码。

### 1. 当前多进程拓扑

从代码可见，当前运行链路至少包含这些角色：

- `ves_supervisor_main.cpp`：负责 supervisor 进程，拉起 engine 和 client
- `virus_executor_service_main.cpp`：engine / service 进程
- `ves_client_main.cpp`：client 进程
- `registry_server.cpp`：控制面注册与发现

其中 `ves_supervisor_main.cpp` 明确存在：

- `SpawnEngine(...)`
- `SpawnClient(...)`
- `fork()`

这说明当前实现不是单体进程，而是明确的多进程编排。

### 2. 控制面与数据面分离

控制面能力包括：

- `OpenSession`
- `CloseSession`
- `Heartbeat`
- `AnyCall`

数据面能力包括：

- 共享内存 session
- request ring
- response ring
- eventfd 唤醒与 credit 控制

### 3. 共享内存协议事实

协议采用固定 entry 设计：

- `PROTOCOL_VERSION = 7`
- `RING_ENTRY_BYTES = 8192`
- 请求和响应都使用固定头部 + inline payload
- 小包走共享内存主路径
- 大包由上层旁路 `AnyCall` 承接

### 4. Session 安全接入事实

`Session::Attach()` 的真实流程是：

1. 先映射头部
2. 校验 magic/version
3. 读取真实 layout
4. 再 remap
5. client 侧检查并获取单活附着权

### 5. 恢复状态机事实

`RpcClient` 有明确生命周期：

- `Uninitialized`
- `Active`
- `NoSession`
- `Cooldown`
- `IdleClosed`
- `Recovering`
- `Closed`

并且对外暴露：

- `RecoveryRuntimeSnapshot`
- `RecoveryEventReport`

### 6. 业务封装事实

`VesClient` 并不重写 RPC，而是把：

- `RpcClient`
- bootstrap / control loader
- recovery policy
- typed API

组合起来。

`VesClient::InvokeApi()` 的关键逻辑是：

- 小包走 `InlineMemRpc`
- 大包走 `AnyCall`
- 统一通过 `RetryUntilRecoverySettles()` 处理恢复窗口

### 7. 双通路复用事实

`EngineSessionService` 会把同一套业务 handler 同时注册到：

- `RpcServer`
- `AnyCall` sink

这证明业务逻辑是“一份语义，两条承载路径复用”。

### 8. 结构化心跳事实

`Heartbeat` 返回的是结构化运行态，而不是简单 alive：

- `status`
- `reasonCode`
- `sessionId`
- `inFlight`
- `lastTaskAgeMs`
- `flags`
- `currentTask`

### 9. 异常处理与隔离事实

代码与测试证明系统显式处理：

- 对端异常退出
- engine death
- 外部 kill
- timeout   
- heartbeat 异常
- eventfd 故障
- stale old session event

### 10. 测试与 baseline 事实

项目拥有完整测试矩阵：

- unit
- integration
- DT
- stress
- fuzz
- sanitizer

同时存在 baseline 性能数据，例如：

- `memrpc` 4 线程 0B echo：`9974.333 ops/s`，`p99 366.288 us`
- `memrpc` 4 线程 4KB echo：`6606.000 ops/s`，`p99 1048.098 us`
- `testkit add` 4 线程：`4044.667 ops/s`
- `testkit echo_0B` 4 线程：`4519.000 ops/s`

### 11. 多线程运行时布局事实

当前实现不是“一个线程包打天下”，而是明确分工：

- `RpcClient` 侧有：
  - `submitThread_`
  - `responseThread_`
  - `watchdogThread_`
- `RpcServer` 侧有：
  - `dispatcherThread`
  - `responseWriterThread`
  - `highWorkerThreads`
  - `normalWorkerThreads`

这说明项目对线程职责有明确设计：

- 请求提交
- 响应回收
- 超时与恢复监控
- 请求分发
- 响应写回
- 高优/普通任务执行

### 12. eventfd 不是装饰，而是关键同步机制

当前数据面显式交换并使用：

- `highReqEventFd`
- `normalReqEventFd`
- `respEventFd`
- `reqCreditEventFd`
- `respCreditEventFd`

这意味着：

- 服务端不需要靠纯轮询感知请求到来
- 客户端不需要靠纯轮询感知响应到来
- ring 满/空变化通过 credit 机制协同
- 局部 eventfd 故障时，系统还能退化到 polling 保持可用

### 13. SPSC ring 正确性是被显式约束的

共享内存 ring 的关键事实包括：

- `RingCursor` 只包含 `head` / `tail` / `capacity`
- `PushRingEntry()` 与 `PopRingEntry()` 基于原子 `head/tail` 操作
- `shm_layout.h` 里有显式断言：
  - `std::atomic<uint32_t> must be lock-free for SPSC ring correctness`

这说明 ring 设计不是随意实现，而是按 SPSC 正确性假设构建的热路径数据结构。

## 六、PPT 设计总原则

### 1. 页面数量

固定输出 17 页，不多不少。

### 2. 每页必须包含

- 标题
- 副标题
- 页面结论
- 上屏文案
- 图示建议
- 若有代码页，给代码截图建议
- 演讲备注

### 3. 页面风格

- 华为可信、鸿蒙系统软件、工业级
- 图文比例约 6:4
- 每页只讲一个核心判断
- 先结论，后证据

### 4. 禁止项

禁止：

- 把页面做成文件列表
- 大段罗列源码路径
- 一页塞 3 段以上代码
- 模糊空话

### 5. CTO 级审美要求

请把整套 PPT 做出“高层愿意翻完”的高级感：

- 开篇就有一句可以做年度汇报标题的话
- 每页标题都像结论，不像章节名
- 图要有秩序感、科技感、平台感
- 重点页要有“高层一句话 + 架构图 + 关键证据”的组合
- 不要出现“程序员视角自说自话”的排版

## 七、必须新增的“看懂型”页面

这次生成 PPT 时，必须重点把下面几页做好，因为它们是外部评委看懂全局的关键：

### 1. 旧架构页

讲清楚：

- 过去是单进程
- 单进程简单但边界不清
- 出故障时影响面大
- DFX 很难拆开看

### 2. 为什么拆多进程页

讲清楚：

- 不是为了拆而拆
- 是为了隔离故障、增强恢复、提升 DFX、沉淀平台能力

### 3. 新架构拓扑页

讲清楚：

- supervisor、client、engine、registry 各干什么
- 控制面走哪里
- 数据面走哪里

### 4. 一次调用的完整流程页

讲清楚：

- client 如何建立 session
- 小包如何走共享内存
- 大包如何走旁路
- reply 如何回来

### 5. 一次 crash 恢复流程页

讲清楚：

- engine 被 kill
- client 如何感知
- 如何进入恢复状态
- 如何重新建立新 session
- 如何恢复业务可用

### 6. 线程布局与信号机制页

讲清楚：

- client 为什么要分 submit / response / watchdog 三类线程
- server 为什么要分 dispatcher / worker / response writer
- eventfd 与 credit 为什么比纯轮询更高效
- SPSC ring 为什么能把热路径做短

### 7. CTO 价值收束页

讲清楚：

- 为什么这不是功能代码，而是平台资产
- 为什么这套设计会提升未来项目复用率
- 为什么这构成团队的工程护城河

## 八、逐页成稿结构

下面开始输出 PPT，严格按以下 17 页组织。

---

## 第 1 页：封面

### 标题

从单进程到多进程：鸿蒙系统层安全防护架构的一次关键重构

### 副标题

memrpc 与 vpsdemo 在高性能通信、DFX 优化与可靠恢复方向的系统化实践

### 页面结论

这次工作不是一般功能开发，而是一次系统层架构重构与工程能力升级。

### 上屏文案

项目围绕安全防护场景，完成了从单进程模式向多进程体系的升级。  
这次重构同时落地了共享内存高性能主数据面、鸿蒙风格控制面、统一恢复状态机、结构化 DFX 和完整测试闭环。  
它的价值不只是“性能更好”，更在于把复杂系统问题做成了可长期复用的工程范式。

### 图示建议

- 封面主视觉做“旧单进程 -> 新多进程”的演进箭头
- 底部放 4 个关键词：高性能、异常隔离、DFX、可恢复

### 演讲备注

开场先定性：这是一次系统层重构，不是一段普通业务代码。评委需要先接受“这是大事”，再进入具体技术细节。

---

## 第 2 页：背景页

### 标题

背景：安全防护能力为什么必须升级

### 副标题

单进程模式能工作，但已经难以支撑更高的可靠性与演进要求

### 页面结论

问题的根源不只是通信方式，而是单进程架构天然缺少隔离、恢复与精细 DFX 能力。

### 上屏文案

过去安全防护能力更多采用单进程模式。  
这种模式实现简单，但在系统层场景会逐渐暴露问题：

- 功能、控制、运行态耦合在一起
- 进程一旦异常，影响面大
- DFX 难以拆分到具体链路和状态
- 后续平台化扩展空间有限

因此，本次工作的起点不是“做一个更快的通信模块”，而是“重建一套更适合系统层长期演进的架构”。

### 图示建议

- 左侧画一个“单进程大盒子”，内部塞满控制、业务、通信、状态
- 右侧放 4 个红色问题标签：耦合、影响面大、DFX 粗、难扩展

### 演讲备注

这一页只讲为什么原来的方式不够了，不要急着上技术细节。让评委先建立问题意识。

---

## 第 3 页：高层摘要页

### 标题

这次重构的真正价值：把一类系统问题一次性做对

### 副标题

不是优化 1 个模块，而是同时重建性能、隔离、恢复与 DFX 能力

### 页面结论

从 CTO 视角看，这次工作最大的价值，是把原来分散、脆弱、难演进的问题收敛成了平台化能力。

### 上屏文案

请用 4 张高层卡片展示：

- 架构价值：从单进程演进到多进程系统拓扑
- 技术价值：控制面 / 数据面分离，建立共享内存主通路
- 工程价值：恢复、DFX、测试、性能形成闭环
- 平台价值：可向更多系统能力场景复制

底部放一句大结论：

这不是一次“把代码重写一遍”的重构，而是一次把系统工程能力产品化、资产化的升级。

### 图示建议

- 四卡片 executive summary
- 每张卡片配极简图标

### 演讲备注

这一页给高层快速建立“为什么值得看下去”的判断。先讲战略价值，再讲技术细节。

---

## 第 4 页：重构目标页

### 标题

这次重构到底想解决什么

### 副标题

不是简单拆进程，而是同时解决性能、隔离、恢复和 DFX

### 页面结论

重构目标不是单点优化，而是用一次架构升级同时解决四类系统问题。

### 上屏文案

这次重构的目标可以概括为四句话：

- 把控制路径和数据路径拆开
- 把故障影响范围收窄到可控边界
- 把 DFX 从“日志碎片”升级为“结构化状态”
- 把恢复从“补丁逻辑”升级为“统一机制”

因此，多进程只是表象，真正的目标是：  
建立一套面向系统层的高性能、高可靠、可观测、可演进架构。

### 图示建议

- 四象限目标图：性能、隔离、DFX、恢复

### 演讲备注

这一页帮助评委建立评分坐标。后面所有架构和代码亮点，都是围绕这四个目标展开的。

---

## 第 5 页：旧架构 vs 新架构

### 标题

从单进程到多进程，变化到底在哪里

### 副标题

架构的本质变化，是边界和职责重新被定义清楚

### 页面结论

这次重构的核心不是“进程数量变多”，而是职责被正确拆分。

### 上屏文案

旧模式：  
业务处理、控制逻辑、状态判断、异常影响都集中在单进程内部。

新模式：  

- supervisor 负责编排与托管
- registry / control 负责发现、加载、心跳和旁路调用
- engine 负责真正的业务执行
- 共享内存数据面负责高频主调用

架构一旦拆开，隔离、恢复、DFX 和性能优化才真正有了落地空间。

### 图示建议

- 左右对比图
- 左边：单进程一体化
- 右边：supervisor / client / engine / registry 分布图

### 演讲备注

这一页一定要做清楚，因为外部评委只要看懂了这页，后面就不会觉得“为什么要搞这么复杂”。

---

## 第 6 页：新架构拓扑

### 标题

新架构拓扑：谁负责什么

### 副标题

多进程不是分散复杂度，而是让复杂度各归其位

### 页面结论

每个角色职责都很清楚，这正是系统代码可维护、可演进的前提。

### 上屏文案

当前代码中的多进程拓扑可以清晰理解为：

- supervisor：拉起和托管 engine / client，负责整体运行编排
- registry：承担服务注册与发现
- client：发起业务请求、维护恢复状态、感知故障
- engine：承载实际业务执行与系统能力服务

这套拓扑的关键价值是：  
故障不再全部挤在一个进程内处理，状态也不再只靠经验推断。

### 图示建议

- 画 supervisor、registry、client、engine 四角色图
- 标明 `fork()`、注册、加载、连接、会话建立关系

### 演讲备注

这页是“外行看懂”的关键页。请让图足够直观，避免出现太多类名和函数名。

---

## 第 7 页：控制面与数据面

### 标题

控制面与数据面彻底分离

### 副标题

控制面解决管理问题，数据面解决高性能问题

### 页面结论

这是整个方案最关键的架构设计点：不同类型的问题，用不同路径解决。

### 上屏文案

控制面负责：

- `OpenSession`
- `CloseSession`
- `Heartbeat`
- `AnyCall`

数据面负责：

- request ring
- response ring
- eventfd 唤醒
- credit/backpressure 控制

这样做的意义是：

- 管理能力不污染高性能主链路
- 高性能主链路不承载过多平台复杂性
- 恢复、降级与旁路都有自然落点

### 图示建议

- 双泳道图
- 上泳道：控制面
- 下泳道：共享内存数据面

### 演讲备注

请强调“控制面不等于慢，数据面不等于全能”。优秀系统代码往往来自合理分工，而不是一条链路做所有事情。

---

## 第 8 页：线程布局与信号机制

### 标题

为什么这套多进程架构还能跑得快

### 副标题

线程分工明确，eventfd 负责唤醒，SPSC ring 负责缩短热路径

### 页面结论

高性能不是只靠共享内存四个字，而是靠“线程布局 + 信号机制 + 数据结构”三件事一起成立。

### 上屏文案

`RpcClient` 侧明确拆成三类线程：

- submit thread：负责请求提交
- response thread：负责响应回收与事件分发
- watchdog thread：负责超时与恢复监控

`RpcServer` 侧明确拆成四类角色：

- dispatcher：负责从 request ring 取请求
- high worker pool：处理高优任务
- normal worker pool：处理普通任务
- response writer：专门负责写回 response ring

再配合 `eventfd` 与 credit 机制：

- 新请求到来就唤醒，而不是一味轮询
- 响应可及时唤醒 client
- ring 满/空变化通过 credit 协同

底层 ring 采用 SPSC 正确性约束设计，让热路径保持极短。

### 图示建议

- 左侧画 client 线程布局
- 右侧画 server 线程布局
- 中间画 eventfd/credit 信号箭头

### 代码截图建议

- `submitThread_ / responseThread_ / watchdogThread_`
- `dispatcherThread / responseWriterThread / highWorkerThreads / normalWorkerThreads`
- `std::atomic<uint32_t> must be lock-free for SPSC ring correctness`

### 演讲备注

这一页是把“为什么多进程之后没有变慢，反而能更稳定地高性能”讲清楚。要突出：不是盲目并发，而是职责明确的运行时布局。

---

## 第 9 页：一次调用如何完成

### 标题

一次业务调用的完整路径

### 副标题

让评委先看懂系统怎么工作，再看代码为什么好

### 页面结论

调用链路设计清晰，是代码可读性、可维护性的直接体现。

### 上屏文案

一次调用的标准路径是：

1. `VesClient` 发起 typed 调用
2. 控制面建立或确认 session
3. 小包请求进入共享内存 request ring
4. `RpcServer` 从 ring 取请求并分发到业务 handler
5. 业务结果写回 response ring
6. `RpcClient` 收到 reply，完成 future

若请求超过 inline 边界：

- 自动走 `AnyCall` 旁路
- 但业务语义仍然复用同一套 handler 逻辑

### 图示建议

- 时序图
- 单独标注“小包主路径”“大包旁路”

### 演讲备注

这页是承上启下页。它把抽象架构变成具体流程，让评委知道后面的代码亮点是在支撑什么。

---

## 第 10 页：为什么性能好

### 标题

高性能不是靠调参，而是靠正确的数据面设计

### 副标题

固定协议、固定 entry、热路径短，天然适合高吞吐低时延

### 页面结论

性能优势来源于架构选择，而不是后期补救。

### 上屏文案

`memrpc` 采用固定 entry 共享内存协议：

- `PROTOCOL_VERSION = 7`
- `RING_ENTRY_BYTES = 8192`
- 请求与响应采用固定头部 + inline payload

这种设计的价值非常直接：

- 热路径短
- 数据结构稳定
- 避免复杂大包机制拖慢主链路
- 容易形成稳定性能画像

同时，`VesClient` 明确规定：

- 小包走共享内存主路径
- 大包走 `AnyCall` 旁路

主路径因此始终保持纯净。

### 图示建议

- 左侧协议结构图
- 右侧“小包/大包”路径图

### 代码截图建议

- 固定协议常量与 `static_assert`
- `payload.size() > DEFAULT_MAX_REQUEST_BYTES` 的路由判断

### 演讲备注

这页不要讲太多 benchmark 细节，重点讲“为什么会快”。原理讲清楚之后，后面再上数字更有说服力。

---

## 第 11 页：为什么结构好

### 标题

代码结构清楚，是因为职责真的拆清楚了

### 副标题

不是模块多，而是每一层都只做自己该做的事

### 页面结论

`memrpc` 与 `vpsdemo` 的结构优秀，关键在于分层边界稳定。

### 上屏文案

建议从四个角色去讲代码结构：

- `Session`：只管共享内存 attach、ring push/pop、底层协议边界
- `RpcClient`：只管 session 生命周期、pending、response 分发、恢复状态机
- `EngineSessionService`：只管把业务 handler 接到框架
- `VesClient`：只管业务 facade、策略装配和路径选择

这种结构意味着：

- 读代码时不容易迷路
- 改一个层面，不必理解全部实现
- 后续维护不会因为耦合过深而变得脆弱

### 图示建议

- 角色职责图
- 每个角色一张卡片，只写“负责什么 / 不负责什么”

### 演讲备注

这一页是“可读性、可维护性”的核心论据。不要用抽象形容词，要用职责边界说服评委。

---

## 第 12 页：关键代码亮点页一

### 标题

关键写法一：先验证，再 attach

### 副标题

共享内存接入是可信边界，不允许靠默认信任

### 页面结论

这段代码体现的是基础软件思维，而不是业务思维。

### 上屏文案

建议展示两段代码：

- 固定协议与 `static_assert`
- `Session::Attach()` 的两段式 attach 流程

上屏解说词：

- 协议不靠约定俗成，而靠明确常量和编译期约束
- attach 前先校验头部，再按真实布局 remap
- client 还要做单活附着控制，防止脏 session 污染运行态

### 图示建议

- 左右双代码卡片

### 演讲备注

评委看到这里应该形成一个印象：这段代码不是“能跑就行”，而是在关键边界上非常谨慎。

---

## 第 13 页：关键代码亮点页二

### 标题

关键写法二：恢复是统一机制，不是各处补丁

### 副标题

状态机、快照、事件三件套，把恢复做成了一等公民

### 页面结论

真正高级的地方，不是能恢复，而是恢复可被理解、可被观察、可被验证。

### 上屏文案

建议展示：

- `ClientLifecycleState`
- `RecoveryRuntimeSnapshot`
- `RecoveryEventReport`
- `RetryUntilRecoverySettles()`

解说词：

- 故障一旦发生，不是各层各自猜测，而是统一进入框架状态机
- 业务侧不再重写恢复逻辑，只消费框架提供的恢复窗口能力
- DFX 有统一观测面，测试也能对状态进行断言

### 图示建议

- 上半部分状态机
- 下半部分代码截图

### 演讲备注

这页直接支撑“可靠、可维护、接口设计好、DFX 做得好”几个评审关键词。

---

## 第 14 页：关键代码亮点页三

### 标题

关键写法三：一套业务逻辑，复用两条链路

### 副标题

主路径与旁路不复制业务语义，这是高质量解耦的关键

### 页面结论

解耦不是把东西拆散，而是把复用点放在正确位置。

### 上屏文案

建议展示：

- `VesClient` 小包/大包路径选择
- `EngineSessionService` 同时向 `RpcServer` 与 `AnyCall` 注册 handler

解说词：

- 主路径专注高性能
- 旁路处理边界场景
- 业务 handler 不重复实现
- 传输变化不影响业务语义

### 图示建议

- 左图：路径选择
- 右图：双注册复用

### 演讲备注

这页是“解耦、复用、可演进”的最强证据页之一。

---

## 第 15 页：DFX 与异常隔离

### 标题

这次重构真正解决了什么难点

### 副标题

把 DFX、任意 crash 和异常隔离从痛点变成能力

### 页面结论

这套代码最难能可贵的地方，是把以前最容易失控的部分做成了可治理能力。

### 上屏文案

请按三列展示：

第一列：DFX 收敛  
- 统一 snapshot、event、heartbeat、runtime stats
- 从“看日志猜问题”升级为“看状态判断问题”

第二列：任意 crash 处理  
- `CrashedDuringExecution` 成为统一故障语义
- crash、hang、OOM、stack overflow、外部 kill 都进入统一恢复链路

第三列：异常隔离  
- 协议边界隔离
- 旧 session / 新 session 隔离
- eventfd 失效时回退到 polling，不把局部问题扩大

### 图示建议

- 三列价值卡片

### 演讲备注

这里要明确说：最难的部分不是“把链路做通”，而是把系统异常做成可控能力。这正是评奖材料应该打动人的地方。

---

## 第 16 页：测试与性能证据

### 标题

代码好，不靠自夸，靠证据

### 副标题

测试矩阵完整，性能有 baseline，故障场景可复现可验证

### 页面结论

项目已经具备成熟工程的两个标志：验证体系和性能体系。

### 上屏文案

建议分两部分展示：

测试矩阵：

- unit
- DT
- stress
- integration
- fuzz
- sanitizer

代表性验证场景：

- 重复外部 kill 后恢复且 fd 数稳定
- 并发流量下 crash 后仍可恢复
- backpressure 下提交最终被释放
- eventfd 故障后回退到 polling

性能基线：

- 0B echo：`9974.333 ops/s`，`p99 366.288 us`
- 4KB echo：`6606.000 ops/s`，`p99 1048.098 us`
- testkit add：`4044.667 ops/s`
- testkit echo_0B：`4519.000 ops/s`

### 图示建议

- 左侧测试矩阵图
- 右侧性能条形图 + p99 卡片

### 演讲备注

请强调：这套代码不是一次性演示，而是可持续守门、可持续回归的工程作品。

---

## 第 17 页：CTO 价值收束

### 标题

从项目代码到平台资产：这次重构真正沉淀了什么

### 副标题

高层更关心的不是“写了多少”，而是这套能力未来还能创造多少价值

### 页面结论

这次工作的最终成果，不是一个模块升级，而是一套可被复用、可被复制、可继续放大的系统层资产。

### 上屏文案

请用三层金字塔总结：

第一层：当前收益  
- 更高性能
- 更强隔离
- 更清晰 DFX
- 更稳恢复

第二层：团队收益  
- 复杂能力下沉到框架
- 后续业务不再重复造轮子
- 多进程系统设计方法可复制

第三层：平台收益  
- 形成鸿蒙系统层共享内存通信样板
- 为后续更多系统能力场景提供底座
- 构成团队工程护城河

底部总结语：

真正高价值的代码，不是完成一次交付，而是形成长期可复用、可放大的能力资产。

### 图示建议

- 三层金字塔
- 右侧配“风险下降 / 复用上升 / 资产沉淀”三指标箭头

### 演讲备注

这页是 CTO 最看重的一页。它要把前面的技术证据全部收束成“平台价值、组织价值、资产价值”。

---

## 第 18 页：世界级工程特征总结

### 标题

为什么说它已经具备世界级工程特征

### 副标题

不是口号，而是结构、机制、测试和数据共同证明

### 页面结论

优秀不在于某个函数写得巧，而在于多个高标准维度同时成立。

### 上屏文案

请做 8 宫格总结：

- 架构分层清晰
- 控制面与数据面分离
- 协议边界严格
- 接口设计克制
- 异常隔离明确
- 恢复机制统一
- DFX 结构化
- 测试与性能有证据

页面总结语：

这套代码已经具备国际一流系统软件项目常见的工程特征：  
边界清晰、状态明确、故障可控、验证完整、性能可守门、平台可演进。

### 图示建议

- 8 宫格
- 中间大字：世界级工程特征 = 体系能力

### 演讲备注

这一页是评审归因页，帮助评委把前面所有证据收束到“为什么它值得获奖”。

---

## 第 19 页：结束页

### 标题

结论：这不是一次普通重构，而是系统层工程范式的建立

### 副标题

从单进程到多进程，从功能实现到体系能力

### 页面结论

项目已经从“完成需求”上升为“沉淀系统层工程方法论”。

### 上屏文案

最终价值可以概括为三点：

- 对当前业务：获得了更高性能、更强隔离、更好恢复和更清晰 DFX
- 对团队工程：沉淀了可复制的多进程系统层设计方法
- 对平台未来：形成了可继续推广的共享内存通信与恢复样板

结束语建议：

这是一套真正写给未来的代码。  
它不仅解决了今天的问题，也为鸿蒙系统层后续演进提供了可信、清晰、可复用的工程底座。

### 图示建议

- 深色收束页
- 三条价值结论

### 演讲备注

最后要把评委带回“获奖价值”，即：它解决了难问题，形成了方法论，而且可以复制。

## 九、代码截图统一要求

所有代码页遵守以下规则：

- 每页最多 2 段代码
- 每段代码不超过 16 行
- 只高亮最关键的 3 到 5 行
- 代码旁边必须写明“这段代码体现了什么工程价值”

建议优先选这些主题：

- 固定协议与 `static_assert`
- `Session::Attach()` 两段式 attach
- `LockSharedMutex()` 对异常退出的处理
- `ClientLifecycleState` 与恢复快照
- `RetryUntilRecoverySettles()`
- 小包/大包路由选择
- 双通路 handler 注册
- 结构化 heartbeat

## 十、图示统一要求

PPT 中至少要有以下 6 张图：

1. 单进程旧架构图
2. 多进程新架构图
3. 控制面 / 数据面双泳道图
4. 一次业务调用时序图
5. crash 恢复流程图
6. 测试矩阵图

## 十一、文风要求

文风必须：

- 清楚
- 硬朗
- 克制
- 面向评审

推荐句式：

- “真正的变化不是……而是……”
- “多进程只是表象，核心是……”
- “项目解决的不是单点问题，而是一类系统问题”
- “把不可控变成可治理”
- “把复杂性收敛进框架，把简洁性释放给业务”

避免：

- “感觉很好”
- “看起来先进”
- “大概更快”
- “文件很多所以很强”

## 十二、关键代码素材库

以下代码片段已经整理成可直接上 PPT 的版本。  
生成 PPT 时，必须至少选用其中 6 段。  
每段代码都要配一句“为什么这段代码体现了高质量工程能力”。

### 代码 1：固定协议边界

适合页面：

- 第 8 页“为什么性能好”
- 第 10 页“关键写法一”

建议标题：

`固定协议边界，性能与稳定性的共同基础`

建议上屏代码：

```cpp
inline constexpr uint32_t PROTOCOL_VERSION = 7U;
inline constexpr uint32_t RING_ENTRY_BYTES = 8192U;

struct RequestRingEntry {
    uint64_t requestId = 0;
    uint32_t execTimeoutMs = 0;
    uint16_t opcode = OPCODE_INVALID;
    uint8_t priority = 0;
    uint32_t payloadSize = 0;
    std::array<uint8_t, INLINE_PAYLOAD_BYTES> payload{};
};

static_assert(sizeof(RequestRingEntry) == RING_ENTRY_BYTES,
              "RequestRingEntry size must stay fixed");
```

高亮建议：

- `PROTOCOL_VERSION = 7U`
- `RING_ENTRY_BYTES = 8192U`
- `payload`
- `static_assert`

解读文案：

这段代码体现的是“把协议当成系统边界来设计”。  
固定 entry 和编译期断言，保证 ABI 不会在演进中悄然漂移；  
高性能和高可靠，都是从这种强约束开始成立的。

---

### 代码 2：共享内存 attach 的先验校验

适合页面：

- 第 10 页“关键写法一”

建议标题：

`不是直接 attach，而是先验证、再映射、再接管`

建议上屏代码：

```cpp
StatusCode Session::Attach(BootstrapHandles* handles, AttachRole role)
{
    Reset();
    StatusCode status = MapAndValidateHeader(adoptedHandles.shmFd);
    if (status != StatusCode::Ok) {
        return status;
    }

    status = RemapWithActualLayout(adoptedHandles.shmFd);
    if (status != StatusCode::Ok) {
        return status;
    }

    if (role == AttachRole::Client) {
        status = TryAcquireClientAttachment();
    }
    return status;
}
```

高亮建议：

- `MapAndValidateHeader`
- `RemapWithActualLayout`
- `TryAcquireClientAttachment`

解读文案：

这不是普通业务代码的写法，而是典型系统代码写法。  
先校验头部，再按真实布局重映射，最后做 client 单活控制。  
共享内存接入被当成可信边界处理，而不是默认信任环境正确。

---

### 代码 3：异常退出被统一收敛

适合页面：

- 第 11 页“关键写法二”
- 第 13 页“DFX 与异常隔离”

建议标题：

`对端异常不会把系统拖死，而是转成统一故障语义`

建议上屏代码：

```cpp
const int rc = pthread_mutex_timedlock(mutex, &deadline);
if (rc == EOWNERDEAD) {
    pthread_mutex_consistent(mutex);
    pthread_mutex_unlock(mutex);
    return StatusCode::PeerDisconnected;
}
if (rc == ETIMEDOUT) {
    return StatusCode::PeerDisconnected;
}
if (rc == ENOTRECOVERABLE) {
    return StatusCode::PeerDisconnected;
}
```

高亮建议：

- `EOWNERDEAD`
- `ETIMEDOUT`
- `ENOTRECOVERABLE`
- `PeerDisconnected`

解读文案：

最难处理的不是正常路径，而是异常路径。  
这段代码把 owner death、超时、不可恢复锁状态统一收敛成明确状态码，  
避免了“死锁挂死”“状态悬空”“只能靠日志猜”的典型问题。

---

### 代码 4：恢复状态机显式建模

适合页面：

- 第 11 页“关键写法二”

建议标题：

`恢复被做成状态机，而不是散落逻辑`

建议上屏代码：

```cpp
enum class ClientLifecycleState : uint8_t {
    Uninitialized = 0,
    Active = 1,
    NoSession = 2,
    Cooldown = 3,
    IdleClosed = 4,
    Recovering = 5,
    Closed = 6,
};

struct RecoveryRuntimeSnapshot {
    ClientLifecycleState lifecycleState;
    bool recoveryPending;
    uint32_t cooldownRemainingMs;
    uint64_t currentSessionId;
};
```

高亮建议：

- `Active`
- `Cooldown`
- `Recovering`
- `Closed`
- `RecoveryRuntimeSnapshot`

解读文案：

这一段代码直接决定了整个项目的可靠性上限。  
只要状态机是显式的，恢复才可观测、可断言、可测试、可维护；  
否则恢复逻辑只会越来越散、越来越不可控。

---

### 代码 5：业务统一复用恢复窗口能力

适合页面：

- 第 11 页“关键写法二”

建议标题：

`把复杂性收敛进框架，把简洁性释放给业务`

建议上屏代码：

```cpp
return client_.RetryUntilRecoverySettles([&]() {
    const VesInvokeExecutionContext context{
        &client_,
        CurrentControl(),
    };
    return ExecuteInvokeRoute(route, context, invokeRequest, reply);
});
```

高亮建议：

- `RetryUntilRecoverySettles`
- `CurrentControl`
- `ExecuteInvokeRoute`

解读文案：

这段代码很短，但价值很大。  
它说明业务调用不再自己维护一套恢复逻辑，而是直接复用框架统一的恢复窗口能力。  
这是好接口设计最典型的特征：让上层简单，而不是把复杂性推给上层。

---

### 代码 6：小包 / 大包路径选择

适合页面：

- 第 8 页“为什么性能好”
- 第 12 页“关键写法三”

建议标题：

`主路径保持纯净，边界场景走旁路`

建议上屏代码：

```cpp
VesInvokeRoute route = VesInvokeRoute::InlineMemRpc;
if (payload.size() > MemRpc::DEFAULT_MAX_REQUEST_BYTES) {
    HILOGW("VesClient::InvokeApi oversized request uses AnyCall");
    route = VesInvokeRoute::AnyCall;
}
```

高亮建议：

- `InlineMemRpc`
- `DEFAULT_MAX_REQUEST_BYTES`
- `AnyCall`

解读文案：

真正成熟的设计，不是让一条链路兼容所有情况。  
这段代码明确保护了共享内存主路径的纯度：  
小包走高性能主链路，大包走旁路，性能与兼容性各归其位。

---

### 代码 7：一套 handler 复用两条链路

适合页面：

- 第 12 页“关键写法三”

建议标题：

`业务逻辑只写一遍，主路径和旁路共同复用`

建议上屏代码：

```cpp
RpcServerHandlerSink rpcServerSink(rpcServer_.get());
AnyCallHandlerSinkImpl anyCallSink(&anyCallHandlers_);
for (auto* registrar : registrars_) {
    if (registrar != nullptr) {
        registrar->RegisterHandlers(&rpcServerSink);
        registrar->RegisterHandlers(&anyCallSink);
    }
}
```

高亮建议：

- `RpcServerHandlerSink`
- `AnyCallHandlerSinkImpl`
- `RegisterHandlers`

解读文案：

这段代码直接证明系统做到了“同一套业务语义，两条传输通路复用”。  
这不是简单复用代码，而是说明架构边界真的设计对了：  
业务语义独立于传输承载方式。

---

### 代码 8：结构化心跳

适合页面：

- 第 13 页“DFX 与异常隔离”

建议标题：

`心跳不是 alive，而是结构化运行态`

建议上屏代码：

```cpp
reply->sessionId = sessionId;
reply->flags = VES_HEARTBEAT_FLAG_HAS_SESSION | VES_HEARTBEAT_FLAG_INITIALIZED;
reply->inFlight = stats.activeRequestExecutions;
reply->lastTaskAgeMs = stats.oldestExecutionAgeMs;
if (stats.activeRequestExecutions == 0) {
    reply->status = static_cast<uint32_t>(VesHeartbeatStatus::OkIdle);
} else if (stats.oldestExecutionAgeMs >= LONG_RUNNING_REQUEST_THRESHOLD_MS) {
    reply->status = static_cast<uint32_t>(VesHeartbeatStatus::DegradedLongRunning);
}
```

高亮建议：

- `sessionId`
- `inFlight`
- `lastTaskAgeMs`
- `OkIdle`
- `DegradedLongRunning`

解读文案：

这段代码体现了非常强的 DFX 意识。  
系统不是只告诉外部“还活着”，而是告诉外部“现在处于什么状态、为什么是这个状态、问题可能在哪里”。  
这正是可信系统应有的运行态表达方式。

---

### 代码 9：重复死亡信号不会污染新 session

适合页面：

- 第 13 页“DFX 与异常隔离”

建议标题：

`旧故障不会打断新会话，恢复边界清晰`

建议上屏代码：

```cpp
if (IsStaleObservedSession(observedSessionId, currentSessionId)) {
    LogIgnoredStaleRecovery("RpcClient::HandleEngineDeath",
                            observedSessionId,
                            currentSessionId);
    return;
}
HandleEngineDeathLocked(observedSessionId, deadSessionId);
```

高亮建议：

- `IsStaleObservedSession`
- `LogIgnoredStaleRecovery`
- `return`

解读文案：

这是异常隔离最能说明水平的代码之一。  
旧 session 的死亡事件不会把已恢复的新 session 再次打断，  
说明恢复不是粗暴重试，而是具备严格边界意识的状态迁移。

---

### 代码 10：eventfd 故障退化为 polling

适合页面：

- 第 13 页“DFX 与异常隔离”
- 第 14 页“测试与性能证据”

建议标题：

`局部信号故障不扩大，系统自动退化运行`

建议上屏代码：

```cpp
auto future = client.InvokeAsync(call);
ASSERT_TRUE(WaitFor([&]() { return future.IsReady(); }, std::chrono::seconds(1)));
EXPECT_EQ(std::move(future).Wait(&reply), StatusCode::Ok);
EXPECT_EQ(reply.payload, call.payload);
```

配套说明：

该片段需结合测试标题一起展示：

- `ClientRequestSignalFailureFallsBackToPolling`
- `ServerResponseSignalFailureFallsBackToPolling`

高亮建议：

- `FallsBackToPolling`
- `future.IsReady()`
- `StatusCode::Ok`

解读文案：

这不是普通成功路径测试，而是在证明：  
即使 eventfd 失效，系统仍然能够通过 polling 退化保持服务可用。  
这体现的是“可靠退化”能力，而不是简单报错退出。

---

### 代码 11：客户端三线程布局

适合页面：

- 第 7 页“线程布局与信号机制”

建议标题：

`客户端不是一条线程死扛，而是提交、回收、监控分工明确`

建议上屏代码：

```cpp
submitThread_ = std::thread([this] { submitWorker_.Run(); });
responseThread_ = std::thread([this] { responseWorker_.Run(); });
watchdogThread_ = std::thread([this] { WatchdogLoop(); });
```

高亮建议：

- `submitThread_`
- `responseThread_`
- `watchdogThread_`

解读文案：

这三条线程对应三种完全不同的职责：  
提交请求、接收响应、做超时与恢复监控。  
职责拆清之后，运行时行为才可预测，恢复逻辑也不会污染热路径。

---

### 代码 12：服务端多线程布局

适合页面：

- 第 7 页“线程布局与信号机制”

建议标题：

`服务端把分发、执行、写回拆开，避免互相拖慢`

建议上屏代码：

```cpp
struct ServerOptions {
    uint32_t highWorkerThreads = 1;
    uint32_t normalWorkerThreads = 1;
    uint32_t completionQueueCapacity = 0;
};

impl_->responseWriterThread = std::thread([impl] { impl->ResponseWriterLoop(); });
impl_->dispatcherThread = std::thread([impl] { impl->DispatcherLoop(); });
```

高亮建议：

- `highWorkerThreads`
- `normalWorkerThreads`
- `responseWriterThread`
- `dispatcherThread`

解读文案：

这里体现的是成熟的服务端运行时设计：  
请求分发、任务执行、响应写回彼此分工，高优请求和普通请求也被分池处理。  
这样既保障了吞吐，也防止普通流量拖慢关键请求。

---

### 代码 13：SPSC ring 正确性约束

适合页面：

- 第 7 页“线程布局与信号机制”
- 第 9 页“为什么性能好”

建议标题：

`SPSC ring 不是口头概念，而是被代码显式约束`

建议上屏代码：

```cpp
struct RingCursor {
    std::atomic<uint32_t> head{0};
    std::atomic<uint32_t> tail{0};
    uint32_t capacity = 0;
};

static_assert(std::atomic<uint32_t>::is_always_lock_free,
              "std::atomic<uint32_t> must be lock-free for SPSC ring correctness");
```

高亮建议：

- `head`
- `tail`
- `lock-free`
- `SPSC ring correctness`

解读文案：

这一段非常适合体现“底层设计是认真做过的”。  
SPSC ring 的正确性前提被显式写进断言，说明这套共享内存热路径不是经验实现，而是带约束、带假设、带验证意识的系统设计。

---

### 代码 14：eventfd + credit 避免纯轮询

适合页面：

- 第 7 页“线程布局与信号机制”

建议标题：

`不是靠傻轮询，而是用 eventfd 和 credit 做有信号的协同`

建议上屏代码：

```cpp
const int fd = session.Handles().respCreditEventFd;
pollfd pollFd{fd, POLLIN, 0};
const auto waitResult = PollEventFd(&pollFd, static_cast<int>(remainingMs));

if (status == StatusCode::Ok && !SignalEventFd(session.Handles().respEventFd)) {
    HILOGW("response published without wakeup signal");
}
```

高亮建议：

- `respCreditEventFd`
- `PollEventFd`
- `SignalEventFd`
- `respEventFd`

解读文案：

这一段体现的是“信号驱动 + 反压协同”的设计思路。  
系统优先通过 eventfd 唤醒，而不是靠持续轮询烧 CPU；  
只有在局部故障场景下，才退化到 polling 维持可用性。

---

### 代码 15：线程池背压与有界执行

适合页面：

- 第 8 页“线程布局与信号机制”
- 第 11 页“为什么结构好”

建议标题：

`不是无限排队，而是有界执行与显式背压`

建议上屏代码：

```cpp
bool TrySubmit(std::function<void()> task) override
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!running_ || queue_.size() >= queueCapacity_) {
        return false;
    }
    queue_.push(std::move(task));
    cv_.notify_one();
    return true;
}
```

高亮建议：

- `queue_.size() >= queueCapacity_`
- `return false`
- `notify_one`

解读文案：

这段代码体现的是成熟系统常见的控制哲学：  
宁可显式背压，也不把系统拖进无限堆积、长尾失控和恢复困难。  
它直接支撑了系统的可预测性和稳定性。

---

### 代码 16：late reply 被识别并丢弃

适合页面：

- 第 15 页“DFX 与异常隔离”
- 第 16 页“测试与性能证据”

建议标题：

`超时后的迟到响应不会污染当前状态`

建议上屏代码：

```cpp
std::optional<PendingRequest> pending = owner_->requestStore_.Take(entry.requestId);
if (!pending.has_value()) {
    HILOGW("RpcClient::ResolveCompletedRequest ignored late reply");
    return;
}
```

高亮建议：

- `Take(entry.requestId)`
- `!pending.has_value()`
- `ignored late reply`

解读文案：

这段代码体现的是非常关键的正确性意识。  
请求一旦超时结束，迟到响应不会再反向污染当前状态。  
这让 timeout、恢复、重连这些复杂场景仍然保持语义干净。

## 十三、最后要求

请直接输出完整 19 页 PPT 成稿。  
不要再问我要文件。  
不要让我补充代码。  
不要输出“建议参考源码”。  
直接把以上内容整理成最终可制作 PPT 的成稿方案。

## 十四、只增不减增强要求

以下内容是对前文的增强，不是替代。  
PPT agent 在生成最终成稿时，必须遵守：

- 前文已有内容全部保留其核心意思
- 本节内容作为增强层追加吸收
- 不允许因为版面控制而删掉前文的重要技术点
- 如果需要做取舍，只能合并表达，不能删除关键结论、关键代码、关键测试、关键架构事实

换句话说：

- 内容只多不少
- 结论只更强，不更弱
- 证据只更完整，不更稀释

## 十五、CTO 冲击版强化表达

为了让材料更适合 CTO / 平台负责人场景，请在整套 PPT 中有意识强化以下表达层：

### 1. 这不是“模块优化”，而是“系统工程升级”

推荐用语：

- “这是系统层架构能力的一次升级”
- “这是一次把局部能力提升为平台能力的动作”
- “这次重构改变的不是一个模块，而是系统处理问题的方式”

### 2. 这不是“拆进程”，而是“重建故障边界”

推荐用语：

- “多进程只是形式，真正重建的是故障边界”
- “单进程里无法收敛的问题，在多进程边界下终于可以被治理”
- “控制面、数据面、执行面被重新切开之后，恢复与 DFX 才真正成立”

### 3. 这不是“做一个快的链路”，而是“建立可持续的高性能机制”

推荐用语：

- “高性能来自机制，而不是偶然”
- “共享内存只是起点，真正的护城河是协议、线程、信号和状态协同”
- “不是一次压测快，而是长期可守门的性能工程”

### 4. 这不是“写了很多测试”，而是“建立了系统自证能力”

推荐用语：

- “系统能够自证其正确性和稳定性”
- “可靠性不是宣称出来的，而是被设计、被观测、被测试出来的”
- “项目已经具备成熟底座代码应有的验证闭环”

### 5. 这不是“完成需求”，而是“沉淀资产”

推荐用语：

- “一次交付，沉淀长期资产”
- “这套方案后续可以继续复用、继续演进、继续放大”
- “它不只是解决当前问题，也提升了团队未来的工程上限”

## 十六、可选加页指令

如果 PPT agent 支持生成扩展版本，请在主稿 19 页之外，再输出 4 页附录备选页。  
这些附录页用于 CTO 或评委追问时继续展开，正常汇报时可隐藏。

### 附录 A：线程与数据流深水区

内容：

- client 三线程布局
- server 分发/执行/写回布局
- request / response / credit / eventfd 协同关系
- 为什么这种布局比“单线程 + 纯轮询”更可控

### 附录 B：恢复状态迁移明细

内容：

- `Active`
- `Cooldown`
- `Recovering`
- `NoSession`
- `IdleClosed`
- `Closed`

补充说明：

- 哪些触发源会进入哪条迁移路径
- 哪些状态是对外可见的稳定语义
- 哪些状态是内部恢复窗口

### 附录 C：故障注入与异常隔离明细

内容：

- eventfd fault injection
- repeated external kill
- stale old session signal
- timeout 后 late reply 丢弃
- fd 稳定性验证

### 附录 D：代码护城河页

内容：

- 为什么这套代码不是“普通 IPC 封装”
- 为什么 SPSC ring + eventfd + recovery state machine 的组合难以被简单复制
- 为什么这构成团队的工程壁垒

## 十七、建议加入的“高层一句话”

请在整套 PPT 中自然穿插以下句子，可用于页眉、副标题或收束大字：

- “多进程不是目的，重建系统边界才是目的。”
- “真正的高性能，来自克制的主路径设计。”
- “真正的可靠性，来自显式状态和统一故障语义。”
- “真正的 DFX，不是日志更多，而是状态更清楚。”
- “真正的好代码，不是实现复杂，而是把复杂性收敛得足够好。”
- “真正的平台资产，不是可运行，而是可复用、可演进、可守门。”

## 十八、需要额外强调的深水区技术点

下面这些点在最终成稿里必须至少点到一次，最好能单独上图或上代码：

### 1. 有界队列与背压

不能只讲高性能，还要讲：

- 不无限排队
- 不无边界吞任务
- 背压是显式设计的一部分

这体现了系统的“稳定优先”哲学。

### 2. 高优与普通请求分流

不能只讲 worker pool，还要讲：

- 高优请求和普通请求分池
- 防止普通流量拖慢关键路径

这是系统层代码常见但很有价值的设计细节。

### 3. response writer 单独抽离

要强调：

- 响应写回不是顺手做，而是单独线程承担
- 这样可以避免执行线程直接卡在 response ring credit 上

### 4. 迟到响应丢弃

要强调：

- timeout 之后，late reply 不会污染系统当前状态
- 这是复杂恢复场景下非常关键的正确性设计

### 5. 信号驱动优先，轮询只是退化

要强调：

- 正常情况下依赖 eventfd 做唤醒
- polling 只是局部故障时的保底机制
- 这体现了高性能与高可靠之间的平衡

## 十九、视觉冲击加强指令

为了让材料更有“CTO 级精美汇报”的质感，请强化以下视觉方法：

### 1. 首页要像年度技术战略页

- 标题要大
- 副标题要有判断
- 不能像项目周报

### 2. 对比页必须有明显的“旧 vs 新”冲击感

- 左边单进程用暗红 / 灰黑问题态
- 右边多进程新架构用深蓝 / 青蓝能力态

### 3. 关键代码页不要平铺

- 代码截图只做局部
- 用高亮框和注释指向核心 3 行
- 每页代码旁边必须有一句“高层也能听懂”的结论

### 4. 性能页不要只有条形图

- 要有结论大字
- 要有基线卡片
- 要有“为什么能快”的结构提示

### 5. 收束页要有资产感

- 不要只写“谢谢”
- 要写“这套代码沉淀了什么”
- 让高层觉得这是一项可继续投资的能力

## 二十、额外代码素材补充

以下代码也可作为备选素材，进一步增强技术深度：

### 代码 17：request ring 的极短热路径

适合页面：

- 第 10 页“为什么性能好”
- 附录 A

建议上屏代码：

```cpp
const uint32_t head = access.cursor->head.load(std::memory_order_acquire);
const uint32_t tail = access.cursor->tail.load(std::memory_order_relaxed);
if (tail - head >= access.cursor->capacity) {
    return StatusCode::QueueFull;
}
entries[tail % access.cursor->capacity] = entry;
access.cursor->tail.store(tail + 1U, std::memory_order_release);
```

解读文案：

这段代码体现的是共享内存热路径极度克制的设计方式：  
核心写路径只围绕 head/tail/capacity 运转，没有额外复杂对象管理。  
这正是高吞吐、低时延场景里最有价值的代码风格。

### 代码 18：高优先级与普通优先级分流

适合页面：

- 第 8 页“线程布局与信号机制”
- 第 11 页“为什么结构好”

建议上屏代码：

```cpp
struct ServerOptions {
    uint32_t highWorkerThreads = 1;
    uint32_t normalWorkerThreads = 1;
    std::shared_ptr<TaskExecutor> highExecutor;
    std::shared_ptr<TaskExecutor> normalExecutor;
};
```

解读文案：

这段代码短，但信息量很大。  
它说明系统并不把所有请求混在一起处理，而是从运行时层就把服务质量分层。  
这类设计通常只会出现在真正考虑系统行为上限的代码里。

### 代码 19：运行时统计直接服务 heartbeat / DFX

适合页面：

- 第 15 页“DFX 与异常隔离”
- 第 17 页“CTO 价值收束”

建议上屏代码：

```cpp
struct RpcServerRuntimeStats {
    uint32_t completionBacklog = 0;
    uint32_t completionBacklogCapacity = 0;
    uint32_t highRequestRingPending = 0;
    uint32_t normalRequestRingPending = 0;
    uint32_t responseRingPending = 0;
    uint32_t activeRequestExecutions = 0;
    uint32_t oldestExecutionAgeMs = 0;
};
```

解读文案：

这段代码说明系统从设计之初就考虑了运行时透明度。  
DFX 不是后来补日志，而是底层运行时就把 backlog、执行中请求、最老请求年龄这些信息纳入结构化观测。

### 代码 20：transport selection 被测试直接证明

适合页面：

- 第 14 页“关键代码亮点页三”
- 第 16 页“测试与性能证据”

建议上屏代码：

```cpp
ASSERT_EQ(client.ScanFile(task, &reply), MemRpc::StatusCode::Ok);
EXPECT_EQ(control->memrpcCount(), 1);
EXPECT_EQ(control->anyCallCount(), 0);

ASSERT_EQ(client.ScanFile(task, &reply), MemRpc::StatusCode::Ok);
EXPECT_EQ(control->memrpcCount(), 0);
EXPECT_EQ(control->anyCallCount(), 1);
```

解读文案：

这组测试不是普通功能验证，而是在直接证明：  
小包确实走共享内存主路径，大包确实走 `AnyCall` 旁路。  
也就是说，架构设计不是写在图上的，而是被测试精确约束住了。

## 二十一、页面视觉布局脚本

以下内容不是可选建议，而是给 PPT agent 的硬性版式脚本。  
请在生成 PPT 时，尽量逐页遵守。  
目标不是“做出来”，而是“做得像 CTO 级汇报成品”。

### 通用页面规则

#### 1. 标题规则

- 标题必须像结论，不像目录
- 标题长度控制在 14 到 24 个字之间
- 优先使用“判断式标题”

推荐风格：

- “真正的变化，不是拆进程，而是重建系统边界”
- “高性能来自正确的主路径设计”
- “恢复之所以可信，是因为状态机是显式的”

避免风格：

- “架构设计”
- “系统介绍”
- “模块说明”

#### 2. 副标题规则

- 副标题用于补充判断，不重复标题
- 每页副标题不超过两行
- 副标题要把“为什么”说出来

#### 3. 正文规则

- 正文尽量控制在 60 到 120 字
- 不要整段大白话
- 不要一页超过 6 个 bullet
- 能用图表示的内容尽量不用长段文字

#### 4. 视觉网格规则

统一按以下三类版式轮换使用：

- `版式 A`：左文右图
- `版式 B`：上结论，下图解
- `版式 C`：左图右代码 / 左代码右图

避免连续 3 页都使用同一种版式。

#### 5. 配色规则

- 背景以浅白 / 冷灰为主
- 标题使用深蓝
- 强调路径、性能、恢复、世界级等关键词时使用青蓝高亮
- 旧架构或问题区域可用低饱和暗红 / 灰黑

#### 6. 代码块规则

- 代码块不要超过页面宽度的 45%
- 字体要明显比正文小一档，但仍可读
- 高亮仅用边框 + 浅色底，不要整段全亮
- 每段代码旁必须有 2 到 3 个“工程价值标签”

推荐标签：

- `强边界`
- `显式状态`
- `可恢复`
- `低开销`
- `解耦复用`
- `可靠退化`

## 二十二、逐页版式脚本

以下内容是对前面 19 页内容的排版增强。  
请保留前面的页内容，同时叠加这里的版式要求。

### 第 1 页版式脚本

- 使用 `版式 B`
- 顶部 40% 空间放大标题和副标题
- 中部放“单进程 -> 多进程”的演进箭头主视觉
- 底部放 4 个横向数据型标签：高性能 / 异常隔离 / DFX / 可恢复
- 标题要非常大，有年度汇报封面感

### 第 2 页版式脚本

- 使用 `版式 A`
- 左侧放“单进程大盒子”，盒子内用 4 个小块表示：业务、控制、状态、通信
- 右侧放问题卡片：耦合重、影响面大、DFX 粗、难演进
- 页面右下角加一句小字：`系统越复杂，单进程越容易成为故障耦合体`

### 第 3 页版式脚本

- 使用 `版式 B`
- 顶部一句高层判断
- 下方四张 executive 卡片横向排布
- 每张卡片一句价值判断，不要写太多字
- 整体像“董事会摘要页”

### 第 4 页版式脚本

- 使用 `版式 B`
- 中心做 2x2 目标矩阵：性能、隔离、DFX、恢复
- 每个象限只写一句定义
- 背景使用极浅网格线，增强秩序感

### 第 5 页版式脚本

- 使用 `版式 A`
- 左边旧架构，右边新架构
- 中间用一条粗箭头表示“架构跃迁”
- 左边颜色偏暗，右边颜色偏亮
- 让观众一秒看出“旧与新”的差异

### 第 6 页版式脚本

- 使用 `版式 B`
- supervisor / registry / client / engine 做成四节点拓扑图
- 节点之间要有不同箭头样式：
  - 控制关系
  - 建连关系
  - 调用关系
- 图下方补一句结论：`多进程不是分散复杂度，而是让复杂度各归其位`

### 第 7 页版式脚本

- 使用 `版式 A`
- 上方画控制面泳道，下方画数据面泳道
- 控制面用较细线，数据面用较粗线，体现“主数据路径”
- 右下角增加一个小卡片：`主通路：共享内存` / `旁路：AnyCall`

### 第 8 页版式脚本

- 使用 `版式 B`
- 左半部分画 client 三线程
- 右半部分画 server 线程布局
- 中间用 eventfd / credit 做桥
- 下方单独画一个很小的 ring cursor 示意图，标 `head/tail`
- 这页要有“硬核但不晦涩”的视觉效果

### 第 9 页版式脚本

- 使用 `版式 B`
- 用横向时序图
- 节点依次为：VesClient / Control / RpcClient / Session / RpcServer / Handler
- 小包主路径用实线
- 大包旁路用虚线
- 对关键节点加高亮圆角框

### 第 10 页版式脚本

- 使用 `版式 A`
- 左侧是协议结构示意图
- 右侧是“小包主路径 / 大包旁路”路径图
- 底部横向放性能判断标签：`固定 ABI` `热路径短` `主路径纯净`

### 第 11 页版式脚本

- 使用 `版式 B`
- 四张职责卡片横向铺开：
  - Session
  - RpcClient
  - EngineSessionService
  - VesClient
- 每张卡片两行：
  - `负责什么`
  - `不负责什么`
- 要有“组织结构图”的高级感

### 第 12 页版式脚本

- 使用 `版式 C`
- 左侧放“固定协议边界”代码
- 右侧放“attach 两段式校验”代码
- 每段代码旁边有 3 个小标签
- 页面底部加一句总结：`可信边界靠机制建立，不靠经验维护`

### 第 13 页版式脚本

- 使用 `版式 C`
- 上半部分放状态机图
- 下半部分放 `RetryUntilRecoverySettles()` 代码
- 右侧可以加一个小框：`业务层只提供策略，不重复维护状态机`

### 第 14 页版式脚本

- 使用 `版式 C`
- 左边路径选择代码
- 右边双注册复用代码
- 中间用一张极简示意图把二者连接起来
- 整体必须体现“主路径与旁路克制分工”

### 第 15 页版式脚本

- 使用 `版式 B`
- 三列价值卡片：
  - DFX 收敛
  - crash 可恢复
  - 异常隔离
- 每列上方一个图标，下方一个小流程图，再下方一句判断

### 第 16 页版式脚本

- 使用 `版式 A`
- 左侧测试矩阵图
- 右侧性能图表
- 性能图表分两层：
  - ops/s 条形图
  - p99 延迟卡片
- 页面顶部直接放一句结论大字：`高性能与高可靠都已经被证据证明`

### 第 17 页版式脚本

- 使用 `版式 B`
- 三层金字塔：
  - 当前收益
  - 团队收益
  - 平台收益
- 右侧竖排 3 个箭头指标：
  - 风险下降
  - 复用上升
  - 资产沉淀

### 第 18 页版式脚本

- 使用 `版式 B`
- 8 宫格世界级工程特征
- 中间可以叠一句半透明大字：`SYSTEM-GRADE ENGINEERING`
- 每格只写一个短标题和一句解释

### 第 19 页版式脚本

- 使用 `版式 B`
- 深色背景
- 中间一句大结论
- 下方三条总结横向排布
- 不要出现“谢谢”
- 让页面像“战略收官页”，不是礼貌结束页

## 二十三、CTO 级讲稿增强规则

为了让最终成稿不只是“PPT 漂亮”，还要“讲起来有压迫感”，请每页讲稿都遵守：

### 1. 先给判断，再讲原因

推荐结构：

- 第一行：一句判断
- 第二行：一句原因
- 第三行：一句价值

### 2. 多用“系统级词汇”

推荐词：

- 系统边界
- 主通路
- 旁路
- 恢复语义
- 故障收敛
- 运行态透明度
- 工程护城河
- 平台资产
- 可持续守门

### 3. 每页结尾最好落到价值

不要停在技术实现，要落回：

- 更快
- 更稳
- 更清楚
- 更易演进
- 更可复用

## 二十四、最终生成偏好

请在成稿中尽量体现下面这些高级特征：

- 让第一页和最后一页都可以单独截图使用
- 中间每张图都尽量可独立理解
- 关键代码页即使脱离汇报口述，也能看出其工程价值
- 如果需要二次修改，优先保留图和判断，不要先删结论

最终目标不是“做一个好看的汇报”，而是：  
做出一套既能打动 CTO，也能说服架构评审和核心研发的系统级优秀代码评选材料。
