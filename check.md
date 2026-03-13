# C++ 函数行数检测与优化 - AI 指导手册

本文档指导 AI 助手使用 `check_function_length.py` 工具检测并优化超长 C++ 函数。

---

## 1. 工具简介

`tools/lint/check_function_length.py` 是基于 `lizard` 的函数行数检测工具，用于识别超过公司规范（50行）的函数。

### 核心能力
- 检测所有 C++ 函数的行数和圈复杂度
- 识别超长函数（默认阈值：50行）
---

## 2. AI 使用指南

### 步骤 1：运行检测

```bash
# 基础检测 - 查看所有超长函数
python3 tools/lint/check_function_length.py memrpc/src/ --max-lines 50

# 生成 AI 优化提示（复制输出用于下一步）
python3 tools/lint/check_function_length.py memrpc/src/ --max-lines 50 --ai-prompt
```

### 步骤 2：分析输出

工具输出格式：
```
行数 | 圈复杂度 | 函数名                              | 文件
----------------------------------------------------------------------------------------------------
 163 |       30 | memrpc::RpcClient::Impl::SubmitOne | memrpc/src/client/rpc_client.cpp:620
```

关注指标：
- **行数 > 50**：需要拆分
- **圈复杂度 > 10**：逻辑过于复杂，需要简化

### 步骤 3：逐函数优化

按以下优先级处理：
1. 行数最多且圈复杂度高的函数
2. 核心模块中的函数（client/server/session）
3. 测试代码中的函数

---

## 3. 重构策略（AI 优化手册）


## 4. 优化检查清单

优化完成后，逐项确认：

- [ ] 函数行数 <= 50 行
- [ ] 圈复杂度 <= 10（理想情况 <= 5）
- [ ] 函数职责单一（只做一件事）
- [ ] 命名清晰（提取的子函数名能说明用途）
- [ ] 无代码重复（DRY 原则）
- [ ] 早返回减少嵌套层级
- [ ] 编译通过，测试通过

---

## 5. 工作流模板

### 批量优化工作流
```
1. 运行工具获取全部超长函数列表
2. 按文件分组，优先处理核心模块
3. 对单个文件：
   - 先优化最长的函数
   - 每次优化后编译检查
   - 完成文件后运行单元测试
4. 全部完成后，再次运行工具验证
5. 提交变更
```
---

## 6. 常见反模式（避免）

| 反模式 | 说明 | 正确做法 |
|--------|------|----------|
| 简单拆分 | 将连续代码粗暴截断 | 按语义分组提取 |
| 过度抽象 | 1-2行的函数也提取 | 内联简单逻辑 |
| 命名模糊 | `Process1`, `Handle2` | `ParseRequestHeader`, `ValidateToken` |
| 参数爆炸 | 提取的函数需要10+参数 | 封装参数为结构体 |

---

## 7. 示例：完整优化案例

---

## 8. 工具命令速查

```bash
# 检测全部源码
python3 tools/lint/check_function_length.py memrpc/src/ --max-lines 50

# 生成 AI 提示（用于批量优化）
python3 tools/lint/check_function_length.py memrpc/src/ --max-lines 50 --ai-prompt > ai_tasks.md

# JSON 输出（用于脚本处理）
python3 tools/lint/check_function_length.py memrpc/src/ --max-lines 50 --json
```

---

## 9. 与 CI 集成建议

将以下检查加入预提交钩子或 CI：

```bash
#!/bin/bash
# .git/hooks/pre-commit 或 CI 脚本
set -e

echo "Checking function length..."
python3 tools/lint/check_function_length.py memrpc/src/ --max-lines 50

echo "All functions are within 50 lines!"
```

---

## 10. 总结

AI 助手使用此工具的核心流程：

1. **扫描** → 运行工具识别超标函数
2. **分析** → 理解函数逻辑，识别提取点
3. **重构** → 提取子函数，应用早返回等策略
4. **验证** → 确认行数达标，编译测试通过
5. **迭代** → 重复直到所有函数符合规范

---
