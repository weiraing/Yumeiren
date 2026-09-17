# 重构执行计划

## 1. 执行策略

基于审计结论（代码质量 A-，架构 A），采取保守策略：**只做低风险规范化和明确的性能改进，不进行大规模重构。**

## 2. 分阶段计划

### 阶段一：低风险规范化 ✅ 已完成

**内容：**
- 文件级注释补充（10 个文件）
- 结构体级注释补充（ComponentStatus、VideoOutput）
- 类级注释补充（VideoWallpaper、MainWindow）
- 注释规范文档生成

**状态：** 已完成（见 comment-refactor-report.md）

### 阶段二：代码格式规范化

**内容：**
- include 顺序统一（项目头文件 → Qt 头文件 → 标准库头文件）
- 删除无用 include
- const 正确性检查

**风险：** 极低（不改变逻辑）

### 阶段三：性能微优化

**内容：**
1. ImageProcess::boxBlur — 改为行级 in-place 处理，减少临时内存分配
2. Engine::restartExplorer — 添加注释说明阻塞原因
3. MainWindow 预览渲染 — 检查是否有不必要的 QImage 拷贝

**风险：** 低

### 阶段四：构建验证

**内容：**
- Debug 构建
- 编译警告清理

## 3. 不执行的优化

| 项目 | 原因 |
|------|------|
| 引擎重启移至后台线程 | 低频操作，风险高于收益 |
| 缩略图 LRU 清理 | 仅磁盘占用，不影响运行时 |
| 视频播放管线重写 | 架构已合理 |
| 引入新依赖 | YAGNI |

## 4. 提交策略

```
refactor: normalize include order and const correctness
perf: optimize boxBlur memory allocation
docs: add code quality and architecture audit reports
```
