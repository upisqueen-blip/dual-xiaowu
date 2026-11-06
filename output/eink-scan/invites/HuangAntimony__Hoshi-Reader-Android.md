<!-- 目标：HuangAntimony/Hoshi-Reader-Android　作者：HuangAntimony　tier=A score=88.7 -->
<!-- 标题：Second 400×300 panel for Hoshi-Reader-Android — worth a branch? -->
<!-- 建议：0 天前还有人提交、近 180 天 448 次提交、近 180 天 10 个 PR 在动、近 180 天 16 个 issue 有回复 -->

Hi,

how `app/src/androidTest/java/moe/antimony/hoshi/features/anki/AnkiClientProviderTest.kt` separates fetching from drawing is the reason this worked: I never had to touch your layout code.

I'm building a small e-ink device: an ESP32-S3 with two 400×300 panels side by side, one black/white and one tri-colour. Right now every project that wants to run on it has to write another display driver, which is a bad trade for everyone. So I moved the rendering to the server instead — POST a JSON card, get back the exact frame for both panels:

    {"version":1,"title":"API quota","items":[{"label":"Left","value":"$42.10"}]}

The frame is 400×300 — 1-bit for the B/W panel, 2-bit for the tri-colour one — packed row-major, so it drops straight into the buffer you already have.

There's a try page, no account and no key: https://dual.xiaowuai.vip/try

So for Hoshi-Reader-Android you wouldn't need a driver at all: the JSON path returns a 400×300 frame directly, so it's one more output format next to what you have.

If that's interesting I can send a branch that adds a second-panel sink and leaves your current path untouched — you decide whether it's worth merging, and I won't chase it.

One disclosure: this is a commercial device I'm building, so this isn't a drive-by. If Hoshi-Reader-Android gets listed on our community page it links back to your repo, and your code and licence stay exactly as they are.

If you'd rather not get messages like this, say so and I won't contact you again.

— upisqueen-blip
https://dual.xiaowuai.vip/
