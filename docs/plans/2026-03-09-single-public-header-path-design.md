# 单一公开头路径设计

## 目标

收紧 `memrpc` 的公开头暴露面，只保留一套正式 include 路径，避免顶层转发头和分层头长期并存。

## 当前问题

目前仓库里既有分层路径：

- `memrpc/core/*`
- `memrpc/client/*`
- `memrpc/server/*`

也有顶层转发路径：

- `memrpc/rpc_client.h`
- `memrpc/rpc_server.h`
- `memrpc/bootstrap.h`
- `memrpc/types.h`
- `memrpc/client.h`
- `memrpc/server.h`
- `memrpc/demo_bootstrap.h`
- `memrpc/sa_bootstrap.h`
- `memrpc/handler.h`

这会导致：

- 新代码路径不统一
- 公开 API 边界模糊
- 后续重构时需要兼顾双入口

## 设计选择

只保留分层路径作为正式公开头：

- `include/memrpc/core/bootstrap.h`
- `include/memrpc/core/types.h`
- `include/memrpc/client/rpc_client.h`
- `include/memrpc/client/demo_bootstrap.h`
- `include/memrpc/client/sa_bootstrap.h`
- `include/memrpc/server/handler.h`
- `include/memrpc/server/rpc_server.h`

其余顶层转发头全部删除。

## 影响范围

需要同步修改：

- `src/*`
- `demo/minirpc_demo_main.cpp`
- `tests/memrpc/*`
- `tests/apps/minirpc/*`
- `apps/minirpc/*`

不应修改：

- `legacy/` 中的历史归档代码

## 预期结果

- 主线代码和测试全部只依赖一套公开头路径
- 头文件层级本身直接表达框架结构
- 后续新代码不再需要判断该用顶层头还是分层头
