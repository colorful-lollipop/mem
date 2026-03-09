# 主线与遗留代码隔离设计

## 目标

收紧当前主线，只保留 `memrpc + minirpc` 的可编译、可测试、可演示路径；将 `vps_demo`、旧兼容层和非主线测试整体迁入仓库内临时归档目录，避免继续污染框架认知。

## 设计原则

- 主构建只包含：
  - `src/core`
  - `src/client/rpc_client.cpp`
  - `src/server/rpc_server.cpp`
  - `src/bootstrap`
  - `apps/minirpc`
  - `tests/memrpc`
  - `tests/apps/minirpc`
- 非主线代码不直接删除，先迁入 `legacy/`
- `legacy/` 只做历史参考，不纳入 `CMake`
- 旧业务兼容接口不再放在框架层主路径上

## 目录方案

- `legacy/vps_demo/`
  - 迁入 `include/vps_demo/*`
  - 迁入 `src/vps_demo/*`
- `legacy/memrpc_compat/`
  - 迁入 `include/memrpc/compat/*`
  - 迁入 `src/client/engine_client.cpp`
  - 迁入 `src/server/engine_server.cpp`
  - 迁入 `src/rpc/*`
  - 迁入 `src/memrpc/compat/*`
- `legacy/tests/`
  - 迁入 `tests/integration_end_to_end_test.cpp`
  - 迁入 `tests/scan_codec_test.cpp`
  - 迁入 `tests/scan_behavior_codec_test.cpp`
  - 迁入 `tests/vps_*`

## 预期结果

- 框架主线目录更干净
- `minirpc` 继续作为唯一主线样板
- 后续重做 `apps/vps` 时，仍可从 `legacy/` 中参考旧实现
- 所有主线构建、测试和 demo 行为保持不变
