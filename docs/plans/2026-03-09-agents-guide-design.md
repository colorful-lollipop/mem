# AGENTS 指南设计

## 目标

新增仓库根目录 `AGENTS.md`，作为当前主线开发的统一贡献指南。

## 设计边界

- 只覆盖当前主线：
  - `include/memrpc`
  - `src/core` / `src/client` / `src/server` / `src/bootstrap`
  - `include/apps/minirpc`
  - `src/apps/minirpc`
  - `tests`
  - `docs`
- 不在正文中展开过渡目录、历史兼容目录和未纳入主线的实验代码。

## 风格规范

- 类型、函数、类方法使用 `UpperCamelCase`
- 变量、参数、成员使用 `lowerCamelCase`
- 常量和宏使用 `ALL_CAPS_WITH_UNDERSCORES`
- 命名空间统一放在 `OHOS::Security::VirusProtectionService` 下，框架位于 `...::MemRpc`
- 新代码按新规范；旧代码允许顺手收敛，但不做纯重命名式改动

## 内容范围

- 项目结构
- 构建、测试、运行命令
- 代码风格
- 测试约定
- 提交与 PR 约定
- 架构和日志约束

## 过滤目录原则

过滤目录与过渡代码不放入 `AGENTS.md` 正文，只在实现计划中记录，避免干扰当前主线协作。
