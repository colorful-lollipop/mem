# Problem 1: 第二次 crash 恢复后 ScanFile 挂住

## 现象

`vpsdemo_dt_crash_recovery` 中：

- **第一次 crash → 恢复 → 正常调用：OK**（Step 2-4 全部通过）
- **第二次 crash → 恢复后调用：挂住**（Step 5 卡死）

压测 `vpsdemo_stress_client --iterations 100`（含 5% crash）也会卡住，根因应该一致。

## 复现

```bash
cmake --build demo/vpsdemo/build
timeout 20 ./demo/vpsdemo/build/vpsdemo_dt_crash_recovery
```

输出停在：
```
step5: crash returned status=8
VpsDemoService initialized
EngineSessionService initialized
```
之后无任何输出，直到 timeout 杀掉进程。

## 已确认正常的部分

1. 死亡检测：`session died` 日志正常
2. EngineDeathHandler 回调：`poison_pills=1` 正确识别
3. restart callback：新 engine 成功 spawn 并 ready
4. 新 engine 的 EngineSessionService 初始化成功

## 卡住位置（推测）

卡在 Step 5 的 restart wait 之后、或第一个 `ScanFile` 调用内部。
具体是 `RpcClient` 框架的 `RestartAfterDeath` → `EnsureLiveSession` → `OpenSession` 链路，或者 submitter 线程未能正确恢复。

## 排查方向

1. **RestartAfterDeath 线程是否完成？** — 在 `EnsureLiveSession` 前后加日志
2. **VpsBootstrapProxy::OpenSession 重连是否成功？** — connect / SCM_RIGHTS 交换是否正常
3. **submitter 线程状态** — 是否在 `submit_cv.wait()` 上睡死，还是卡在 `WaitForRequestCredit`
4. **reconnect_mutex 竞争** — HandleEngineDeath 持锁期间 callback sleep 300ms，是否影响后续
5. **第二次 OpenSession 时 VpsBootstrapProxy 的旧 monitor_thread / sock_fd 清理** — 虽然已加了 cleanup 代码，但需验证第二次 reconnect 是否命中正确分支

## 相关文件

- `src/client/rpc_client.cpp` — `RestartAfterDeath`, `EnsureLiveSession`, `HandleEngineDeath`
- `demo/vpsdemo/src/ves_bootstrap_proxy.cpp` — `OpenSession` reconnect 清理逻辑
- `demo/vpsdemo/src/ves_client.cpp` — `SetEngineDeathHandler`, restart callback
- `demo/vpsdemo/src/vesdemo_dt_crash_recovery.cpp` — DT 复现用例
