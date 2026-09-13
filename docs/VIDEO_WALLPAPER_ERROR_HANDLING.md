# 视频壁纸错误处理与状态反馈（阶段 3 交付物）

对应实现：`src/videowallpaper.cpp`（`mediaErrorText` / `handleUnplayable` / `nextTrack` 失败名单 / errorOccurred、mediaStatusChanged、playbackStateChanged 连接）；实测依据：`tools/perf/results/t_*.csv`。

## 1. 错误分类

`QMediaPlayer::Error`（Qt 6.10：NoError/ResourceError/FormatError/NetworkError/AccessDeniedError）→ 用户可读分类（`mediaErrorText`）：

| 分类 | 触发 | 用户可见 |
|---|---|---|
| 资源错误 | ResourceError：文件缺失、损坏、读取中断 | "资源错误（文件缺失、损坏或读取失败）" |
| 格式不支持 | FormatError | "格式不支持" |
| 网络流错误 | NetworkError | "网络流错误" |
| 访问被拒绝 | AccessDeniedError | "访问被拒绝" |
| 未知 | 其他/空 detail | "未知错误"或透传后端文本 |
| 无视频轨 | LoadedMedia 时 `hasVideo()==false` 且无分辨率元数据 | "没有视频轨"（不重试） |
| 播放结束 / 用户停止 / 系统挂起 | EndOfMedia / stopAll / Suspended | 正常状态文本，**不计为错误** |

任务书分类中的 FileAccessDenied→AccessDeniedError、AudioOutputError/VideoOutputError 在 Qt 6.10 无对应独立错误码（音频初始化失败会以 ResourceError 或静音降级呈现），归入既有分类；PlaybackTimeout 未实现自动超时（依赖 errorOccurred + 后端行为，避免误杀慢速打开的大文件），列入已知限制。

## 2. 有限重试与失败名单（防止乒乓循环）

```text
单文件失败 → 重试 #1（500ms 后清空 source 强制重开）→ 重试 #2（1500ms）→ 跳过
跳过 → m_trackFails[index]+1
      ├─ 计数 <2：下一轮轮换还会尝试（应对瞬态故障）
      ├─ 计数 ≥2：进入失败名单(m_deadTracks)，不再参与轮换
      └─ 全部曲目入名单：整体停播（终态文案，无重试循环）
成功起播（PlayingState 且素材确有视频轨）→ 清除该曲目失败计数
起播会话（startPlaying）/列表变更（setPlaylist）/停止（stopAll）→ 清空全部失败记录
```

设计约束与实测教训：
1. **错误态播放器对同一 source 不会再次发 errorOccurred**——重试前必须 `setSource(QUrl())` 清空再经 `playIndex` 重开（首轮实测缺失该步导致流程卡死，t_all_broken/t_retry_skip 复现并修复）。
2. **无视频轨素材会进入 PlayingState**（音频在播），不得据此重置失败计数——PlayingState 处理器仅在 `hasVideo()` 或分辨率元数据有效时清计数；LoadedMedia 检出无视频轨时先 `stop()` 再跳过（t_audio_only 乒乓循环复现并修复）。
3. 退避递增（500→1500ms）+ 重试上限（2 次）+ 失败名单（2 轮）三重上限，杜绝高频重建与死循环（t_all_broken2：两个坏文件 ~15s 内受控停播；t_retry_skip2：~9s 跳过坏曲进入正常播放，基线稳定）。

## 3. 错误记录（连接的信号及理由）

| 信号 | 用途 |
|---|---|
| `errorOccurred` | 错误分类/重试/跳过/停播的唯一收口 |
| `mediaStatusChanged` | EndOfMedia 推进、LoadedMedia 无视频轨检测、长挂起恢复定位 |
| `playbackStateChanged` | 成功起播清除失败额度、UI 按钮/状态文本同步 |
| `metaDataChanged` | 音频轨策略、帧率上限、分辨率提示（均为加载后一次性动作） |

未连接 `positionChanged/durationChanged/hasAudioChanged`：当前无进度条/时长展示需求，避免无用途的信号泵（任务书 6.2）。

## 4. 用户可见状态反馈（playbackStateChanged 文本集）

打开中… / 第 N 个 播放中 / 已暂停 / 已停止 / 播放结束 / 第 N 个打开失败（原因），重试 x/2 / 第 N 个无法播放（原因），自动跳过 / 第 N 个多次失败（原因），已跳过 / 所有视频都无法播放（原因），已停止 / 已清空播放列表 / 提示：视频分辨率高于主屏… / 挂起原因文本（全屏/遮挡/锁屏/熄屏/电池）。

## 5. 已知限制

1. 无 PlaybackTimeout：若后端打开媒体时既不报错也不进入任何状态（理论情形），当前依赖用户手动停止；诊断日志（阶段 7）可暴露此类停滞。
2. 重试次数/退避为内置常量，未暴露配置（保持简单，避免误配）。
3. MirrorAll 下仅首个输出的错误参与推进，副本播放器的错误静默忽略（防重复跳曲）——副本静默失败无独立提示。
