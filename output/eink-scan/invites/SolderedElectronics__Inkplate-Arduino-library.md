<!-- 目标：SolderedElectronics/Inkplate-Arduino-library　作者：SolderedElectronics　tier=A score=90.7 -->
<!-- 标题：Second 400×300 panel for Inkplate-Arduino-library — worth a branch? -->
<!-- 建议：16 天前还有人提交、近 180 天 100 次提交、近 180 天 10 个 PR 在动、近 180 天 13 个 issue 有回复 -->

Hi,

`examples/Inkplate10/Advanced/Communications/Inkplate10_Bluetooth_Serial/Inkplate10_Bluetooth_Serial.ino` is the part I read closest — the panel init sequence is short and readable, which is rarer than it sounds.

I'm building a small e-ink device: an ESP32-S3 with two 400×300 panels side by side, one black/white and one tri-colour. Right now every project that wants to run on it has to write another display driver, which is a bad trade for everyone. So I moved the rendering to the server instead — POST a JSON card, get back the exact frame for both panels:

    {"version":1,"title":"API quota","items":[{"label":"Left","value":"$42.10"}]}

The frame is 400×300 — 1-bit for the B/W panel, 2-bit for the tri-colour one — packed row-major, so it drops straight into the buffer you already have.

There's a try page, no account and no key: https://dual.xiaowuai.vip/try

So for Inkplate-Arduino-library the gap is small: everything that touches the panel goes through one place, and the values are already plain data — a second sink is additive, not a rewrite.

If that's interesting I can send a branch that adds a second-panel sink and leaves your current path untouched — you decide whether it's worth merging, and I won't chase it.

One disclosure: this is a commercial device I'm building, so this isn't a drive-by. If Inkplate-Arduino-library gets listed on our community page it links back to your repo, and your code and licence stay exactly as they are.

If you'd rather not get messages like this, say so and I won't contact you again.

— upisqueen-blip
https://dual.xiaowuai.vip/
