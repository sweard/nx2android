# Android 新游戏运行时检查点接入

[English](CHECKPOINT_ONBOARDING.en.md)

从一次手动保存和紧接着的一次冷启动恢复入手。先定位第一个返回错误的操作，再判断缺的是**版本参数**、**引擎特有行为**，还是**宿主捕获或重建某类资源的能力**。只定位到阶段时，就记录阶段和下一条需要的证据。本文针对 `porter port --field-checkpoint` 选用的现场快照后端。[构建说明](DEVELOPMENT_GUIDE.md#runtime-checkpoints)

## 先取得同一次尝试的两份日志

1. 确认游戏能正常进入可操作场景。记录 Title ID、包名、宿主构建、数据包版本、设备和 Vulkan 驱动。记录实际数据包 `mods/main.fixups.tsv` 和（如有）`mods/sdk.fixups.tsv` 中的 `# BUILD_ID`。区服、更新版本或 SDK 变化时分别记录。新包使用单屏 NVN/Vulkan、默认原生键盘 GameActivity 和 `--field-checkpoint`。没有菜单入口时先查打包选项、Activity 和已装宿主；尚未发生现场捕获。[模块身份来源](../nxnative/dm/host/loader.c) · [已装 APK 能力检查](../porter/host.py)
2. 在一个容易辨认的场景点“保存检查点”，记下角色位置或菜单状态、是否开启固定分辨率、UI 结果。保存进程的 `log.txt` 应在下一次启动前复制。失败时本轮保存到此为止；已有旧快照的恢复结果不能证明这轮保存成功。
3. 保存成功后结束游戏进程，重新启动并选择恢复，再复制新进程的 `log.txt`。宿主把 stderr 写到应用文件目录；每次新进程启动会将上一份轮换为 `log.prev.txt`。如果刚好只冷启动了一次，仍能一起取回。发生 Android 崩溃时补同一时间的 logcat。保留完整日志，恢复阶段多条 `refused` 行直接写 stderr，没有 `[snapshot]` 前缀。[日志打开和轮换](../nxnative/dm/android/front_android.c)

下面的只读命令适用于使用外部应用文件目录、且 adb 可读取该目录的测试包。将示例变量替换为实际设备和包名，本地在本次复现专用目录执行。若宿主回退到内部目录，debuggable 包可用 `adb -s "$serial" exec-out run-as "$pkg" cat files/log.txt` 读取当前日志。

```sh
serial='设备序列号'
pkg='实际包名'
files="/sdcard/Android/data/$pkg/files"
# 保存返回后、重启前执行
adb -s "$serial" pull "$files/log.txt" capture.log
# 新进程选“恢复”后执行
adb -s "$serial" pull "$files/log.txt" restore.log
# 若漏取上一轮，且中间只启动了一次
adb -s "$serial" pull "$files/log.prev.txt" previous.log
rg -an -C 3 '\[snapshot\]|refused|FATAL|failed|\[RED\]' capture.log restore.log
```

## 从第一条失败定位调用

| 阶段与可搜索的原文片段 | 该条日志能说明什么 | 下一步 |
| --- | --- | --- |
| 启动时出现 `snapshot layout setup failed`、`snapshot module baseline failed` 或 `unexpected checkpoint heap constructor site` | 第一条汇总模块和内存布局初始化，第二条来自模块基线检查，第三条表示已匹配堆规则但指令位置校验失败。 | 对第一条检查实际模块清单、有无 `nro.tsv`、地址预留结果。对第三条核对实际 main Build ID、重写后的镜像和该版本的堆规则。先排除混装数据包。[布局检查](../nxnative/dm/android/checkpoint_modules.inc) · [加载器](../nxnative/dm/host/loader.c) |
| 保存时出现 `pending thread=`、`no SVC gate frame`、`frame outside checkpoint memory` 或 `thread sample:` 结果非零 | 失败可能发生在暂停、等待现场、音频或图形同步。线程采样行汇总此前的返回值。 | 找首条具体的线程句柄、wait/SVC、PC/栈或未暂停线程信息。若只在切场景时出现，和同构建稳定场景比较。确认是未覆盖的等待类型后，由线程模块补捕获和恢复。[捕获顺序](../nxnative/dm/android/checkpoint/runtime.c) · [等待现场校验](../nxnative/dm/android/checkpoint_wait.inc) |
| `thread sample:` 成功，随后只有 `rendezvous released: result=-11` | 在带 GSR2 输入控制的构建中，撤销分辨率覆盖失败可返回 `-EAGAIN`。这个汇总值只给出候选。 | 对照同构建、同场景下自动和固定分辨率，检查是否进入 `survey_core()`。仍不明确时，在 `dm_gsr2_release_resolution()` 返回处采一条结果。确认后到对应游戏或引擎的分辨率实现定位。[保存调用点](../nxnative/dm/android/checkpoint/runtime.c) · [分辨率实现](../nxnative/dm/android/gsr2_resolution.c) |
| `scene events result=`、`scene through bindings result=`、`scene through modules result=`、`scene core result=` 连续报错 | `survey_core()` 只在 `rc == 0` 时执行下一步，后续行可重复同一个错误。第一条 events 汇总还包括前面的文件、挂载、音频、图形等操作。 | 从最后一个成功点到第一个失败点查调用。比如 events 成功、bindings 首次失败，再看绑定记录。若遇到 `unsupported pool` 或 `cannot capture image`，保留 pool 标志或图像格式、采样数及返回值。若只有汇总，在这段调用间补操作名和返回值即可。[分段写入顺序](../nxnative/dm/android/checkpoint/runtime.c) · [GPU 池](../nxnative/dm/android/checkpoint_pools.inc) |
| 下次启动没有恢复选项，或出现 `current checkpoint could not be verified` | 启动时会检查当前槽标记、目录名及必需分段是否为非空普通文件。 | 对照上次保存的 `scene core result`、本次包名与发布结果。若返回磁盘空间不足或文件 I/O 错误，先处理存储条件；保留现场以区分发布失败与读取失败。[保存槽发布和检查](../nxnative/dm/android/checkpoint/slot.c) |
| 恢复时报 `scene guest address space refused`、`scene module data refused`、`scene GPU pools refused` 或 `scene image storage refused` | 可以定位到一个子系统或一组调用。末尾的 `restored session ended, result=1` 已丢失底层错误码。 | 取前面的原始码，核对快照来源，同宿主和数据包创建新快照复测。address space 覆盖模块预留、基线和内存恢复；module data 也含 SDK 时钟重绑定。只有明确到失败校验后才决定改哪里。[恢复顺序及错误转换](../nxnative/dm/android/checkpoint/restore.c) |
| `resumed playable field session` 后崩溃、黑屏或游戏内写档失败 | 放行线程只说明恢复已走到运行阶段。之后还可能有线程续跑、渲染、音频或正式存档问题。 | 看后续 `field continuation`、崩溃栈、呈现帧和实际操作。`game save commit refused` 来自正式存档事务；`-ESTALE` 的冲突提示可由快照之后同一文件已被修改引起。保留两次写档顺序和涉及的文件，按冲突或写入错误分别处理。[线程放行](../nxnative/dm/android/checkpoint/restore.c) · [游戏内提交](../nxnative/dm/host/hle_fs.c) · [合并与备份](../nxnative/dm/android/checkpoint/save_commit.c) |

## 决定改动放在哪里

| 结论 | 足以作决定的证据 | 最小改动及验证 |
| --- | --- | --- |
| **补版本配置** | 已在目标构建上确认机制与已支持版本相同，差异可以完整写入现有表。例：同一 UE 堆构造机制仅变更 main 构建及调用位置；同一 SDK 时钟绑定机制仅变更 SDK 构建及槽位置。缺少匹配行本身不足以证明原因。 | 分别核对游戏 profile 的 `checkpoint_heap.tsv` 和共享的 `checkpoint_sdk_clock.tsv`。确认构建实际选取的表及运行时模块身份。修复后核对预期分支确实执行，并做冷恢复。时钟表按 SDK Build ID 复用。[构建时选表](../nxnative/dm/android/build.py) · [SDK 时钟规则](../nxnative/dm/games/checkpoint_sdk_clock.tsv) |
| **复用或增加窄接口** | 同一捕获时机、同一成功条件，但某个引擎需执行不同操作。开关对照加调用返回值已经把失败定位到这个操作。例：宿主施加的分辨率覆盖要在暂停后、写快照前撤销。 | 先实现已有 `dm_gsr2_release_resolution()` 所需的游戏行为。只有现有接口表达不了另一项已确认的操作时，再在管理该状态的模块增加接口。写明调用时机、成功条件及失败如何释放暂停；验证开启、关闭覆盖和撤销失败三条路径。[GSR2 接口](../nxnative/dm/android/gsr2_profile.h) · [失败后的释放流程](../nxnative/dm/android/checkpoint/runtime.c) |
| **改通用后端** | 失败定位到缺少资源表示、序列化、重建或引用校验。例如确认 `nro.tsv` 命中动态模块拒绝分支，或确认合法的 GPU 池类型无法捕获和冷恢复。只在一款游戏上触发也足以归入对应子系统。 | 在模块、线程、GPU、音频或文件系统的拥有方补齐保存与恢复，保留版本/完整性校验、发布顺序和线程放行条件。验证新资源能往返、损坏或不匹配输入仍被拒绝，并让既有支持样本回归。[模块支持条件](../nxnative/dm/android/checkpoint_modules.inc) · [分段恢复](../nxnative/dm/android/checkpoint/restore.c) |

**允许最终结论是“暂不改代码”。** 仓库记录过《歧路旅人 II》的旧原型快照在 GPU 池恢复时返回 `-EINVAL`，同机用受测构建新建快照可以恢复。记录只证实差异与样本有关，具体失败的 pool 校验仍未知。遇到相同模式，保留旧样本，报告兼容性范围；若目标包含读取旧快照，再定位对应格式或校验分支。[实际验证记录](ARCHITECTURE.md#lossless-dll-import-and-frame-generation-timing)

## 怎样算接入成功

- **验证目标场景**：同一构建保存成功 → 新进程恢复到先前记录的位置 → 持续出帧、输入有效，场景有音频时检查音频 → 在测试槽做游戏内存档 → 正常冷启动后从该槽读取。保留设备、构建、场景、两份日志及观察结果。只验证过的组合标为通过。
- **验证改动范围**：版本表改动检查命中与不匹配的构建；窄接口检查触发和未触发场景；通用后端改动增加相应资源的往返和拒绝用例。涉及正式存档合并时，在测试存档上核对独立槽保留、同文件冲突提示及覆盖前备份。[已有存档验证样例](ARCHITECTURE.md#lossless-dll-import-and-frame-generation-timing)
- **使用已有工具**：`python3 devtools/runtime_checkpoint/verify_runtime.py --help` 列出设备探针所需的宿主产物、模块和驱动参数。探针覆盖具体资源的冷进程重建；游戏内操作、画面和读档仍在目标游戏验证。[设备探针](../devtools/runtime_checkpoint/verify_runtime.py) · [保存槽测试](../tests/test_checkpoint_android.py)
