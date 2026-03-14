# docs/plans 说明

`docs/plans/` 不再保存历史设计归档。

旧计划如果需要追溯，直接看 `git log` / `git show`。仓库里只保留当前仍有阅读价值的规范入口，不再把过期方案继续摆在文档树里。

## 当前规范入口

请优先阅读：

- [docs/guide.md](/root/mem/docs/guide.md)
- [plan.md](/root/mem/plan.md)
- [docs/architecture.md](/root/mem/docs/architecture.md)
- [docs/porting_guide.md](/root/mem/docs/porting_guide.md)
- [docs/demo_guide.md](/root/mem/docs/demo_guide.md)
- [docs/stress_fuzz_guide.md](/root/mem/docs/stress_fuzz_guide.md)

## 后续原则

如果后面需要临时计划文档，建议遵循这条规则：

- 只写当前要落地的最小方案
- 方案完成后，把最终结论回写到正式文档
- 临时计划删掉，不在 `docs/plans/` 留历史归档
