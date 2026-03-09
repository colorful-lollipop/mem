# AGENTS 指南实施计划

1. 新增仓库根目录 `AGENTS.md`
   - 标题固定为 `Repository Guidelines`
   - 只描述当前主线模块和工作流

2. 在正文中固化命名规则
   - `UpperCamelCase`：类型、函数、方法
   - `lowerCamelCase`：变量、参数、成员
   - `ALL_CAPS_WITH_UNDERSCORES`：常量、宏

3. 在正文中明确日志门面
   - 统一使用 `virus_protection_service_log.h`
   - 仅在必要位置加日志

4. 在计划中记录过滤目录原则
   - 当前不作为贡献指南正文范围：
     - `include/vps_demo`
     - `src/vps_demo`
     - `src/rpc`
     - 旧 `compat` 业务 codec
     - 其他未纳入主构建的过渡文件

5. 校验
   - 文档控制在 200-400 英文词左右
   - 命令、路径、命名规则可直接执行或对照
