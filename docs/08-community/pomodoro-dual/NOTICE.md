# 授权边界说明（NOTICE）

本目录用于**向上游提交社区贡献**，不属于产品发行物。以下边界对打包、分发、
镜像、二次销售一律有效。

## 一、上游授权状态

| 项 | 值 |
|---|---|
| 上游仓库 | https://github.com/Rukenshia/pomodoro |
| LICENSE 文件 | **无** |
| GitHub API `license` 字段 | `null` |
| 法律后果 | 默认「保留所有权利」（Berne Convention），无明示授权即无授权 |

"公开可读"不等于"开源可分发"。仓库公开只授予**阅读**与**平台级 fork** 的权利，
不授予复制、修改、再分发、商用、打包进产品的权利。

## 二、本目录文件的性质

| 文件 | 性质 | 能否进发行包 |
|---|---|---|
| `patches/*.patch` | 对上游仓库的修改建议（diff 内含上游代码片段） | ❌ 禁止 |
| `PR_DESCRIPTION.md` | 提交给上游的说明文本 | ⚠️ 仅文档，不含代码，可留可删 |
| `README.md` / `NOTICE.md` | 内部说明 | ✅ 可保留 |

`patches/` 之所以不能进发行包：`git format-patch` 生成的补丁必须携带上下文行
和文件路径才能应用，因此它**必然**包含上游代码片段（三个补丁合计约 101 行
上游上下文/删除行）。它在法律上属于上游作品的衍生形式，与"我们自己的代码"
不是一回事。

发行前自检：

```bash
# 打包产物里不应出现任何 patch 或上游标识
grep -rl "Rukenshia\|GxEPD2\|SecondarySink\|TimerSnapshot" <打包目录> || echo "干净"
```

## 三、我们自己的实现不受影响

| 资产 | 授权 | 说明 |
|---|---|---|
| `xiaowu_platform/pomodoro.py` 及配套改动 | 本仓库 Apache-2.0 | clean-room 重写，仅采用「番茄工作法」这一公开方法 |
| 本仓库其余全部代码 | 本仓库 Apache-2.0 | — |

两者之间**没有代码层面的依赖**，删掉整个 `docs/08-community/` 目录，平台功能
不受任何影响。这是刻意设计的：产品分发不建立在第三方未授权代码之上。

## 四、拿到授权之后

作者若在仓库中加入 MIT / Apache-2.0 的 `LICENSE`：

1. 记录授权快照：仓库 `LICENSE` 文件内容 + 加许可那一次 commit 的 SHA；
2. 在本文档补一行"授权已获得，日期，SHA，许可证类型"；
3. 此时才可以把上游代码或补丁纳入发行物，并按许可证要求保留版权声明。

在此之前，任何"我们把番茄钟固件打包发给用户"的动作都不做。

---

## 五、对外动作记录

| 日期 | 动作 | 地址 | 状态 |
|---|---|---|---|
| 2026-11-05 | fork 上游仓库 | https://github.com/upisqueen-blip/pomodoro | 已创建 |
| 2026-11-05 | 推送分支 `feat/dual-display` | 同上，3 commit / +527 −2 / 9 文件 | 已推送 |
| 2026-11-05 | 提交 PR（含 MIT 授权声明 + 产品披露） | https://github.com/Rukenshia/pomodoro/pull/3 | OPEN / MERGEABLE |
| 2026-11-05 | 提交 issue 询问许可证 | https://github.com/Rukenshia/pomodoro/issues/4 | OPEN，等待作者 |

风险提示：作者最后 push 为 2025-06-27，距今约 17 个月，PR 与 issue 都可能长期无回应。
**不要因为提了 PR 就改变分发口径**——分发仍然只用我们自己的 Apache-2.0 实现。

