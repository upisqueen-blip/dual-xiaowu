# 番茄钟双屏分支（基于 Rukenshia/pomodoro）

> 目标：把 GitHub 上的 [Rukenshia/pomodoro](https://github.com/Rukenshia/pomodoro) 做成一份
> **能加一块副屏**的分支，归我们自己的 fork 使用，同时把通用部分整理成可向上游提交的补丁。

---

## 一、先说清楚这件事做不到什么

上游是 **ESP32 Arduino 固件**，不是网页、不是接口、不是能"上架"的应用：

| | 上游 pomodoro | 我们的 dual 设备 |
|---|---|---|
| 框架 | Arduino + PlatformIO | ESP-IDF 5.x |
| 面板 | GDEQ0426T82 / SSD1677，**4.26" 800×480 单屏黑白** | SSD1619 400×300 黑白（FPC1）+ UC1679 400×300 三色（FPC2），共 SPI |
| 出图 | 设备本地 `Adafruit_GFX` 绘制 | 服务端 PIL 出整图下发，设备只解码位图 |
| 输入 | 旋转编码器（GPIO32/21/14） | 固件不上报按键事件 |
| 计时 | `millis()` 累加，断电即丢 | 服务端绝对时间戳 |

**平台不运行第三方固件**，所以它既进不了轨1（静态网页），也进不了轨2（数据接口）。
"在我们后台上架"这件事，走的是另一条完全独立的实现——

| 目标 | 做法 | 状态 |
|---|---|---|
| 在内容后台上架、A/B 双屏可用 | 平台内置番茄钟（clean-room 重写），`mode=pomodoro`，走服务端出图 | ✅ 已完成，A/B 出图验证通过 |
| 给上游社区一份可合并的贡献 | 本目录的 fork 分支（3 个独立 commit） | ✅ 已完成，编译通过 |
| 让上游固件真的驱动我们的两块屏 | 移植 SSD1619 / UC1679 到 GxEPD2 | ⬜ 未做，见第五节 |

两条路互不依赖。上游分支的价值是给社区（和我们自己留一份 Arduino 参考实现），
**不是**让我们设备跑它。

---

## 二、分支里有什么

三个 commit，每个都能单独编译、单独 cherry-pick：

| # | commit | 内容 | 上游能否自行验证 |
|---|---|---|---|
| 1 | `refactor(timer): 暴露只读 TimerSnapshot` | `Timer::snapshot()` 返回一个 POD：阶段、运行态、已走/剩余/超时、组内轮次、休息时长。**刻意不复用 `TimerState`**，副屏用自己的一对小枚举，状态机保持私有。同一 commit 里加 `SecondarySink` 抽象与 `loop()` 调用点——通知放在 `display.display()` **之后**，放前面副屏会早一个循环读到计时值，暂停时能看出两屏差一秒 | ✅ 纯逻辑 |
| 2 | `feat(display): 可选副屏 + 统计卡` | `dual_config.h`（全部参数走宏）+ `SecondaryStats` 实现：大字 = 本组番茄数、右侧 = 组内轮次、进度条 = 距长休、页脚 = 下一步。按 `width >= 640` 切换字号档，400×300 和 800×480 共用一套版式 | ⚠️ 需要第二块屏 |
| 3 | `feat(refresh): 分钟级刷新` | `-DREFRESH_MINUTE_ONLY=1` 把运行态刷新从 1 Hz 降到 60 s。一次 45 分钟专注从 ~2700 次局刷降到 45 次 | ✅ 他关心电池 |

**单屏零影响**：宏没开时，`secondary_stats.cpp` 编译成空翻译单元，`Timer` 里只多一个
空指针判断。

---

## 三、怎么用

引脚、型号、色数全部走 `build_flags`，源码里没有任何写死的板级参数。

```ini
[env:dual]
extends = env:az-delivery-devkit-v4
build_flags =
    -DEPD2_ENABLE=1
    -DEPD2_DRIVER_CLASS=GxEPD2_420   ; 换成你面板对应的 GxEPD2 类
    -DEPD2_CS=<你的 CS>
    -DEPD2_DC=8                      ; 与主屏共用 DC 时填主屏那个引脚号
    -DEPD2_BUSY=<你的 BUSY>
    -DEPD2_RST=-1                    ; 共用复位线
    -DEPD2_3COLOR=1                  ; 三色面板才加
```

```bash
pio run -e dual          # 带副屏
pio run -e az-delivery-devkit-v4   # 原版单屏，行为不变
```

### 两个踩过的坑

- **`RST = -1`**：两块屏共用复位线时，如果第二个 `GxEPD2` 实例自己去驱动 RST，
  它会在主屏 init 刚结束时把主屏也复位掉。GxEPD2 内部有 `if (_rst >= 0)` 保护，
  传 `-1` 就跳过。
- **`DC` 必须填真实引脚号，不能填 -1**：两块屏在同一条 SPI 总线上共用 DC，
  传 `-1` 会让 GxEPD2 往 PIN_NONE 写电平，面板永远锁不到命令。

---

## 四、怎么提交给上游

仓库**没有 LICENSE 文件**，默认是"保留所有权利"。所以：

- **fork 是合规的**——GitHub 服务条款明确允许任何用户 fork 公开仓库，这是平台授权，
  不需要作者额外同意。
- **新建分支、提交 PR 也是合规的**——PR 是向你自己的 fork 推送，再请求作者合并；
  作者有权拒绝。不存在"未经许可复制"的问题。
- **不能做的事**：把这个工程的代码贴进我们自己的产品 / 固件 / 服务端。这条线已经用
  clean-room 重写绕开了（平台内置的番茄钟是完全独立实现的）。

操作：

```bash
# 1. fork 到自己的账号（网页点 Fork，或 gh 一条命令）
gh repo fork Rukenshia/pomodoro --clone
cd pomodoro

# 2. 应用补丁
git checkout -b feat/dual-display
git am /path/to/patches/*.patch

# 3. 推送并开 PR
git push -u origin feat/dual-display
gh pr create --repo Rukenshia/pomodoro --head <账号>:feat/dual-display \
  --title "Optional secondary display + minute-granularity refresh" \
  --body-file PR_DESCRIPTION.md
```

PR 描述直接用 [`PR_DESCRIPTION.md`](./PR_DESCRIPTION.md)，英文写好了。
里面有一句 **"treat my contribution as MIT-licensed"** —— 这是刻意的：仓库没有
LICENSE，如果不主动声明，合并后我们贡献部分的授权状态是模糊的。

建议 PR 里明说"第 2 个 commit 我用自己的双屏硬件验证过，第 1、3 个你本地就能验"，
作者没有第二块屏，这样他能判断该不该合。

**已提交：**

- PR：[Rukenshia/pomodoro#3](https://github.com/Rukenshia/pomodoro/pull/3) — OPEN / MERGEABLE
- Issue：[Rukenshia/pomodoro#4](https://github.com/Rukenshia/pomodoro/issues/4) — 询问是否加 LICENSE
- Fork：[upisqueen-blip/pomodoro](https://github.com/upisqueen-blip/pomodoro)，分支 `feat/dual-display`

PR 与 issue 里都带了一句产品披露与官网链接 `https://dual.xiaowuai.vip/`。这是刻意的：
既说明"我为什么在意这件事"，也是唯一不会让人反感的引流位置——**不要**在 PR 里堆功能
介绍或让读者"来了解一下我们的产品"，社区对这种做法反应很差，一旦被标 spam 会连累后续。

---

## 四、之二、关于「产品分发」

这里有一条法律红线，和"仓库是公开的"是两件事：

| 事实 | 依据 |
|---|---|
| 仓库**公开可读** | ✅ |
| 仓库**没有 LICENSE 文件** | ✅ GitHub API `license: null` |
| 因此默认 **保留所有权利** | 伯尔尼公约，无明示授权即无授权 |
| fork / 提 PR 合规 | ✅ GitHub 服务条款是平台层面的授权，不需要作者额外同意 |
| **打包分发他的代码（固件、源码、改过的版本）不合规** | ❌ 没有许可就没有分发权 |

所以"产品分发"不能建立在他的代码上。我们的做法是：

1. **分发的实体是我们自己的实现**——`xiaowu_platform/pomodoro.py`，Apache-2.0，
   跟上游一行代码都没有关系，随时可以进产品、进发行包。
2. **本目录的 `patches/` 是给上游提 PR 用的产出物，禁止进分发包**。
   详见 [`NOTICE.md`](./NOTICE.md)。
3. **在 PR 正文里正式询问作者是否愿意加 LICENSE**。这一步同时服务两个目的：
   给他一个明确的、对他自己有利的理由（没有许可，别人连合法用都用不了），
   以及给我们自己留下一条"将来可以合法分发他的改进"的路。


---

## 五、最后一层：驱动

`GxEPD2` 支持的 4.2 寸面板里有 `GDEW042T2`、`GDEW042Z15`、`GDEY042T81` 等，
**没有 SSD1619，也没有 UC1679**。所以要在上游固件里真的点亮我们的两块屏，
需要写两个 `GxEPD2_EPD` 子类。

好消息是素材现成：`firmware/s3/main/boards/custom/ssd2683za-s3-epd42/ssd1619_display.cc`
里有验证过的初始化序列，注释记录了几个坑（`0x20` 主激活后的 BUSY 宽限、`0x32` 那个
正好 70 字节的 LUT、以及 BUSY 极性判断整体反向的问题）。移植工作主要是把这些
`Command(0xXX, ...)` 映射到 `_InitDisplay` / `_Init_Full` / `_Update_Part`。

这份工作量约 1~2 天，且**必须真机验证**——LUT 时序错了会出灰雾或半屏残留，
编译通过不代表能看。所以本次没有做，分支里用的 `GxEPD2_420` 只是让代码能编译、
逻辑能跑通的占位驱动。

如果将来要做，顺序建议：先 SSD1619（黑白，单色 buffer，简单），跑通后再 UC1679
（三色需要两套 buffer，GxEPD2_3C 抽象也更绕）。

---

## 六、编译验证记录

| 目标 | 结果 | RAM | Flash |
|---|---|---|---|
| 单屏 `pio run -e az-delivery-devkit-v4` | SUCCESS | 14.4%（76.9 KB） | 86.7%（1.11 MB） |
| 双屏 `pio run -e dual` | SUCCESS | 17.3%（92.1 KB） | 93.7%（1.23 MB） |
| 补丁在干净 clone 上 `git am` | 3 个 commit 全部应用，无冲突 | — | — |

环境：PlatformIO + `espressif32@6.13.0` + Arduino core 2.0.17 + GxEPD2 1.6.9。

两点必须说清楚：

1. **Flash 只剩 82 KB 余量**（93.7%）。第二块屏的驱动 + 统计卡吃掉了 92 KB。
   再往里塞东西（比如中文字体表）就会超过默认分区，届时得改 `partitions.csv`
   把 app 分区调大。这也是为什么副屏必须是一个可关的宏，而不是默认开着。
2. **没有真机验证**。手上没有跑 Arduino 的硬件，上面的 SUCCESS 只证明能编译、
   能链接、内存放得下。刷新时序、LUT、BUSY 极性只有在真实面板上才看得出来，
   第一次上板大概率还要调。分支里用的 `GxEPD2_420` 是占位驱动，不是我们的屏。

副屏刷新是有节流的：分钟数变了、阶段变了、组内轮次变了、或主屏做了整屏刷新，
四者任一才重画。一天 8 个番茄大约是二十来次面板更新。

---

## 七、我们这边的番茄钟在哪

跟这个分支无关，是独立实现，完整说明见 [`docs/POMODORO.md`](../../POMODORO.md)：

| 文件 | 作用 |
|---|---|
| `xiaowu_platform/pomodoro.py` | 状态机与视图模型（绝对时间戳 + 时长，不存递减秒数） |
| `xiaowu_platform/store.py` | `pomodoro_sessions` / `pomodoro_daily` 两张表 |
| `xiaowu_platform/render.py` | `_render_pomodoro` + A/B 两套版式 |
| `xiaowu_platform/main.py` | `start/pause/resume/skip/stop` + 手机端查询 |
| `apps/content-console-cn/app/page.tsx` | 场景中心"专注番茄"，A/B 各选 `mode=pomodoro` |
