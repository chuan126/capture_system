# cloud_visualization

核对日期：2026-08-24

该包生成浏览器点云预览 Topic，不创建 WebSocket，也不参与净空计算。预览继续使用补偿后的
局部东北天点云；净空算法改用原始点云后，两条链路保持单向旁路关系。

## 输入输出

```text
/capture/lidar/points_compensated_enu
/capture/clearance/raw_diagnostics
→ cloud_visualization_node
├→ /capture/visualization/cloud_preview
└→ /capture/visualization/diagnostics
```

补偿节点保持原始点的数量、顺序和源时间戳，因此预览节点只在时间戳精确相等时使用诊断中的
原始线性索引：全扫描有效点为蓝色，当前圆柱 ROI 为绿色，有效最低可信簇始终覆盖为红色。
颜色不依赖任务状态、安装高度或高度阈值，优先级为红 > 绿 > 蓝。诊断缺失、错帧或点数不符
时不复用旧标签，安全退化为蓝色；同帧诊断晚到时会重新发布该帧的正确分类。

诊断链路采用 best-effort、16 帧有界缓存；预览输出采用 best-effort 最新帧，不能反压正式净空。

预览以 5 Hz 处理最新帧，过滤非有限点和 `(0,0,0)` 占位点。超过 10,000 点时按红、绿、蓝
优先级保留分类点，再进行体素代表点和扫描位置补足。输出布局为 XYZ FLOAT32 加一个 UINT8
`classification` 字段，`point_step=16`；FastAPI 将其封装为 PCV2，浏览器仅按分类着色。
预览诊断以 best-effort 发布输入/输出点数、分类匹配状态，以及分类生成、限点转换和总处理耗时，
仅用于回放性能验收。

协议见[PCV1/PCV2 点云预览协议](../../../docs/interfaces/PCV1点云预览协议.md)。
