---
name: nx2android-checkpoint-triage
description: 为 nx2android 新游戏接入 Android 运行时检查点，或根据保存、冷启动恢复和恢复后写档的日志，定位失败调用并判断应补版本配置、复用窄接口还是扩展通用后端。 Diagnose nx2android runtime checkpoint failures during capture, cold restore, or subsequent in-game saves and identify the smallest fix.
---

# nx2android 检查点排障

For requests in English, follow the [English workflow](SKILL.en.md).

在目标 nx2android 工作树中读取 `docs/CHECKPOINT_ONBOARDING.md`，其中有采集命令、真实日志片段、源码入口和验收步骤。源码与文档不一致时，以待诊断宿主的构建为准。

1. 对齐包名、游戏及 SDK Build ID、宿主、数据包、设备/驱动、场景及快照创建版本。标出故障发生在构建/启动、手动保存、槽发布、冷恢复、恢复后运行还是游戏内提交。从用户已有证据开始；缺少某项时说明它会影响哪个判断。
2. 分开读取保存与恢复进程的完整 stderr。启动会轮换 `log.txt` 和 `log.prev.txt`。给出最后一个成功点、最早能定位的失败、原始返回码及对应调用。保存阶段的多条 `scene … result=` 会沿用同一个错误，恢复末尾的 `result=1` 可掩盖底层原因。证据只到一个区间时，指出最小的对照实验或需要记录返回值的调用。
3. 选择最小改动。现有处理机制与目标版本一致、且表项足以表达差异时，按 main 或 SDK 的实际构建补表。确认特有的宿主覆盖需要在既定时机撤销时，先找管理该状态的已有接口。确认新增资源状态需要跨进程保存和重建时，把成对的改动放到拥有资源的后端模块。构建混装、存储错误、陈旧快照和预期存档冲突单独报告。
4. 按用户要求给出诊断或实施修复。报告引用本次日志和代码位置，写明证据支持的结论、还需区分的原因及验证结果。设备探针结果和目标游戏的“保存 → 冷恢复 → 可操作 → 游戏内写档 → 正常启动读档”分别记录。只做日志分析时，把未执行的验证标为待验。
