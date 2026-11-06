# 墨水屏开源项目 · 可邀请名单

> 生成时间：2026-11-06 15:28　窗口：近 180 天　许可口径：AGPL-3.0/Apache-2.0/BSD-2-Clause/BSD-3-Clause/GPL-2.0/GPL-3.0/LGPL-2.1/LGPL-3.0/MIT/MPL-2.0

## 一轮筛完的漏斗

| 环节 | 数量 |
| --- | --- |
| 搜索命中（去重） | 927 |
| 有明确许可、非 fork/归档 | 434 |
| 进入活跃度核查 | 80 |
| 近 180 天有人类提交（非仅机器人） | 77 |
| 有驱动/数据/出图能力 | 55 |
| A 级优先联系 | 17 |

> 分级口径：**A = 本轮前 30%**（这周先联系这批），B = 30%~70%，C = 其余。分数只用于排序，门槛在上一张表里已经卡过了。

## A 级优先联系（17 个）

### 1. [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk)　★2609　Apache-2.0
- 一句话：Open source SDK to build Muse gadgets
- 为什么是他：0 天前还有人提交；近 180 天 216 次提交；近 180 天 10 个 PR 在动；近 180 天 14 个 issue 有回复；Apache-2.0 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`esp32/components/muse/boards/board_waveshare_c6_18.c`
  - 数据：`esp32/components/noise_core/include/xplat/noise/core/ClientSession.h`
  - 出图：`esp32/components/libvpx_vp8/src/vpx/src/vpx_image.c`
- 建议开场：「muse-gadget-sdk」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 2. [usetrmnl/trmnl-firmware](https://github.com/usetrmnl/trmnl-firmware)　★1204　GPL-3.0
- 一句话：TRMNL ePaper device firmware
- 为什么是他：0 天前还有人提交；近 180 天 288 次提交；近 180 天 10 个 PR 在动；近 180 天 18 个 issue 有回复；GPL-3.0 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`include/epd_driver.h`
  - 数据：`include/http_result_handling.h`
  - 出图：`include/cached_images.h`
- 建议开场：「trmnl-firmware」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 3. [bigbag/papyrix-reader](https://github.com/bigbag/papyrix-reader)　★505　MIT
- 一句话：Lightweight open-source firmware for Xteink  e-paper reader (Xteink X3, X4, X4 Pro, X4 v2 Classic, Seeed reTerminal Sticky, and Murphy M4)  • EPUB/FB2/MD/TXT support • Custom themes & fonts
- 为什么是他：2 天前还有人提交；近 180 天 192 次提交；近 180 天 5 个 PR 在动；近 180 天 25 个 issue 有回复；MIT 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`lib/EInkDisplay/src/Ssd1677Driver.h`
  - 数据：`lib/AsyncTask/src/AsyncTask.h`
  - 出图：`lib/GfxRenderer/src/GfxRenderer.cpp`
- 建议开场：「papyrix-reader」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 4. [Free-Ink/freeink-sdk](https://github.com/Free-Ink/freeink-sdk)　★292　MIT
- 一句话：A hardware-independent SDK for building e-paper reader firmware.
- 为什么是他：0 天前还有人提交；近 180 天 860 次提交；近 180 天 10 个 PR 在动；近 180 天 9 个 issue 有回复；MIT 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`libs/display/FreeInkDisplay/include/LgfxEpdConfig.h`
  - 数据：`libs/network/Opds/include/OpdsClient.h`
  - 出图：`libs/assets/ImageToneMapper/include/ImageToneMapper.h`
- 建议开场：「freeink-sdk」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 5. [aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe)　★271　MIT
- 一句话：Feature-rich firmware for ESP32-based e-paper displays with superior image quality.
- 为什么是他：1 天前还有人提交；近 180 天 197 次提交；近 180 天 10 个 PR 在动；近 180 天 24 个 issue 有回复；MIT 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`components/board_hal/include/board_waveshare_photopainter_73.h`
  - 数据：`host_tests/test_http_auth.cpp`
  - 出图：`components/board_hal/include/board_waveshare_photopainter_73.h`
- 建议开场：「esp32-photoframe」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 6. [fatihak/InkyPi](https://github.com/fatihak/InkyPi)　★4274　GPL-3.0
- 一句话：E-Ink Display with a Raspberry Pi and a Web Interface to customize and update the display with various plugins
- 为什么是他：1 天前还有人提交；近 180 天 50 次提交；近 180 天 10 个 PR 在动；近 180 天 24 个 issue 有回复；GPL-3.0 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`src/display/waveshare_display.py`
  - 数据：`src/utils/http_client.py`
  - 出图：`src/plugins/ai_image/ai_image.py`
- 建议开场：「InkyPi」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 7. [FrameOS/frameos](https://github.com/FrameOS/frameos)　★460　AGPL-3.0
- 一句话：Operating system for single function smart frames
- 为什么是他：0 天前还有人提交；近 180 天 1018 次提交；近 180 天 10 个 PR 在动；近 180 天 12 个 issue 有回复；AGPL-3.0 许可（copyleft，注意分发方式）；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`backend/app/drivers/waveshare.py`
  - 数据：`backend/app/api/__init__.py`
  - 出图：`backend/app/api/scene_images.py`
- 建议开场：「frameos」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 8. [usetrmnl/terminus](https://github.com/usetrmnl/terminus)　★777　MIT
- 一句话：The flagship TRMNL BYOS application.
- 为什么是他：0 天前还有人提交；近 180 天 829 次提交；近 180 天 10 个 PR 在动；近 180 天 30 个 issue 有回复；MIT 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`README: …icial firmware specific Waveshare (device and display) can be found h…`
  - 数据：`app/actions/api/base.rb`
  - 出图：`README: ….adoc[Raspberry PI] :doc_render_link: link:[link:https://github.com…`
- 建议开场：「terminus」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 9. [SolderedElectronics/Inkplate-Arduino-library](https://github.com/SolderedElectronics/Inkplate-Arduino-library)　★308　LGPL-3.0
- 一句话：Inkplate family Arduino library. The easiest way to add e-paper to your project.
- 为什么是他：16 天前还有人提交；近 180 天 100 次提交；近 180 天 10 个 PR 在动；近 180 天 13 个 issue 有回复；LGPL-3.0 许可（copyleft，注意分发方式）；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`examples/Inkplate10/Advanced/Communications/Inkplate10_Bluetooth_Serial/Inkplate`
  - 数据：`examples/Inkplate10/Advanced/WEB_WiFi/Inkplate10_HTTPS_POST_Request/Inkplate10_H`
  - 出图：`examples/Inkplate10/Basic/Inkplate10_Grayscale/image.h`
- 建议开场：「Inkplate-Arduino-library」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 10. [mt-empty/pi-inky-weather-epd](https://github.com/mt-empty/pi-inky-weather-epd)　★104　GPL-3.0
- 一句话：A beautiful weather dashboard for any colour E-Paper Display
- 为什么是他：4 天前还有人提交；近 180 天 163 次提交；近 180 天 10 个 PR 在动；近 180 天 4 个 issue 有回复；GPL-3.0 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`README: …t-empty/pi-inky-weather-epd/actions/workflows/test.yml/badge.sv…`
  - 数据：`src/apis/bom/mod.rs`
  - 出图：`README: …disable with: ```toml [render_options] use_moon_phase_instead_of_c…`
- 建议开场：「pi-inky-weather-epd」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 11. [dmellok/tesserae](https://github.com/dmellok/tesserae)　★1008　AGPL-3.0
- 一句话：E-ink dashboard companion. Compose dashboards in a browser, render server-side, and push to e-ink panels over REST or MQTT.
- 为什么是他：0 天前还有人提交；近 180 天 2078 次提交；近 180 天 10 个 PR 在动；近 180 天 3 个 issue 有回复；AGPL-3.0 许可（copyleft，注意分发方式）；懂屏驱动；有 HTTP 数据源；能出图
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`README: …grey, 480×800 portrait (SSD1677) | [tesserae-device-firmware](https…`
  - 数据：`app/bundle_sync.py`
  - 出图：`app/gemini_images.py`
- 建议开场：「tesserae」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 12. [fossasia/magic-epaper-app](https://github.com/fossasia/magic-epaper-app)　★1088　Apache-2.0
- 一句话：Magic ePaper App
- 为什么是他：5 天前还有人提交；近 180 天 367 次提交；近 180 天 10 个 PR 在动；近 180 天 7 个 issue 有回复；Apache-2.0 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源
  - 驱动：`README: …| SE0213A1-B04-YP-N | | Waveshare 2.13" NFC | tri-color | 17745 | | W…`
  - 数据：`lib/src/rust/api/simple.dart`
- 建议开场：「magic-epaper-app」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 13. [kstevica/captain-claw](https://github.com/kstevica/captain-claw)　★193　MIT
- 一句话：Command a fleet of AI agents, on every deck, every desk, every wrist. Self-hosted multi-agent platform with memory, a coding pipeline, Flows, a team kiosk and voice devices. Any LLM, or 100% local with Ollama.
- 为什么是他：0 天前还有人提交；近 180 天 1807 次提交；近 180 天 10 个 PR 在动；近 180 天 1 个 issue 有动静；MIT 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`firmware/device/src/epd_ssd1681.cpp`
  - 数据：`botport/botport/server.py`
  - 出图：`build_support/render_desktop_art.py`
- 建议开场：「captain-claw」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 14. [HuangAntimony/Hoshi-Reader-Android](https://github.com/HuangAntimony/Hoshi-Reader-Android)　★476　GPL-3.0
- 一句话：Hoshi Reader Android: Japanese EPUB reader with Yomitan lookup, Anki mining, audiobook read-along, and e-ink support.
- 为什么是他：0 天前还有人提交；近 180 天 448 次提交；近 180 天 10 个 PR 在动；近 180 天 16 个 issue 有回复；GPL-3.0 许可，合作口径清楚；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：有 HTTP 数据源、能出图
  - 数据：`app/src/androidTest/java/moe/antimony/hoshi/features/anki/AnkiClientProviderTest`
  - 出图：`app/src/androidTest/java/moe/antimony/hoshi/features/reader/EpubImageFallbackDev`
- 建议开场：「Hoshi-Reader-Android」取数和渲染是分开的，出图逻辑独立、移植成本低。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 15. [hthoene/paperpaw](https://github.com/hthoene/paperpaw)　★43　Apache-2.0
- 一句话：A lean, fast and endlessly customisable e-reader for e-ink devices, made of plugins.
- 为什么是他：0 天前还有人提交；近 180 天 3330 次提交；近 180 天 10 个 PR 在动；近 180 天 11 个 issue 有回复；Apache-2.0 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`crates/paperpaw-x4/src/uc8179.rs`
  - 数据：`crates/paperpaw-app/tests/network_settings.rs`
  - 出图：`crates/paperpaw-app/tests/draw.rs`
- 建议开场：「paperpaw」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 16. [dmellok/tesserae-device-firmware](https://github.com/dmellok/tesserae-device-firmware)　★18　AGPL-3.0
- 一句话：（无简介）
- 为什么是他：5 天前还有人提交；近 180 天 281 次提交；近 180 天 10 个 PR 在动；近 180 天 11 个 issue 有回复；AGPL-3.0 许可（copyleft，注意分发方式）；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`boards/epdiy_v7.h`
  - 数据：`src/image_fetcher.c`
  - 出图：`boards/waveshare_photopainter_73.h`
- 建议开场：「tesserae-device-firmware」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

### 17. [Xinyuan-LilyGO/T-Echo](https://github.com/Xinyuan-LilyGO/T-Echo)　★241　MIT
- 一句话：（无简介）
- 为什么是他：4 天前还有人提交；近 180 天 10 次提交；近 180 天 4 个 issue 有回复；MIT 许可，合作口径清楚；懂屏驱动；有 HTTP 数据源；能出图；工程能编译进嵌入式（PlatformIO / Arduino / ESP-IDF）
- 能力证据：懂屏驱动、有 HTTP 数据源、能出图
  - 驱动：`lib/Adafruit_EPD/examples/EPDTest/EPDTest.ino`
  - 数据：`examples/A7682/HttpClient/HttpClient.ino`
  - 出图：`lib/Adafruit-GFX-Library/Adafruit_GFX.cpp`
- 建议开场：「T-Echo」屏驱动那块写得干净，取数和渲染是分开的。我们在做一块 4.2 寸双屏墨水屏（A 屏黑白 / B 屏三色），照着你的做法接了一版，改动都收在一个分支里，想请你看看值不值得合回去。

## 完整排序

| # | 仓库 | 分 | 级 | ★ | 许可 | 最后人类提交 | 提交 | 回复 | 能力 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk) | 98.0 | A | 2609 | Apache-2.0 | 2026-11-06 | 216 | 14 | 驱数图 |
| 2 | [usetrmnl/trmnl-firmware](https://github.com/usetrmnl/trmnl-firmware) | 98.0 | A | 1204 | GPL-3.0 | 2026-11-05 | 288 | 18 | 驱数图 |
| 3 | [bigbag/papyrix-reader](https://github.com/bigbag/papyrix-reader) | 96.5 | A | 505 | MIT | 2026-11-03 | 192 | 25 | 驱数图 |
| 4 | [Free-Ink/freeink-sdk](https://github.com/Free-Ink/freeink-sdk) | 95.9 | A | 292 | MIT | 2026-11-05 | 860 | 9 | 驱数图 |
| 5 | [aitjcize/esp32-photoframe](https://github.com/aitjcize/esp32-photoframe) | 95.6 | A | 271 | MIT | 2026-11-04 | 197 | 24 | 驱数图 |
| 6 | [fatihak/InkyPi](https://github.com/fatihak/InkyPi) | 93.9 | A | 4274 | GPL-3.0 | 2026-11-04 | 50 | 24 | 驱数图 |
| 7 | [FrameOS/frameos](https://github.com/FrameOS/frameos) | 93.7 | A | 460 | AGPL-3.0 | 2026-11-05 | 1018 | 12 | 驱数图 |
| 8 | [usetrmnl/terminus](https://github.com/usetrmnl/terminus) | 93.6 | A | 777 | MIT | 2026-11-05 | 829 | 30 | 驱数图 |
| 9 | [SolderedElectronics/Inkplate-Arduino-library](https://github.com/SolderedElectronics/Inkplate-Arduino-library) | 90.7 | A | 308 | LGPL-3.0 | 2026-10-20 | 100 | 13 | 驱数图 |
| 10 | [mt-empty/pi-inky-weather-epd](https://github.com/mt-empty/pi-inky-weather-epd) | 89.5 | A | 104 | GPL-3.0 | 2026-11-02 | 163 | 4 | 驱数图 |
| 11 | [dmellok/tesserae](https://github.com/dmellok/tesserae) | 89.4 | A | 1008 | AGPL-3.0 | 2026-11-06 | 2078 | 3 | 驱数图 |
| 12 | [fossasia/magic-epaper-app](https://github.com/fossasia/magic-epaper-app) | 89.3 | A | 1088 | Apache-2.0 | 2026-10-31 | 367 | 7 | 驱数· |
| 13 | [kstevica/captain-claw](https://github.com/kstevica/captain-claw) | 89.2 | A | 193 | MIT | 2026-11-05 | 1807 | 0 | 驱数图 |
| 14 | [HuangAntimony/Hoshi-Reader-Android](https://github.com/HuangAntimony/Hoshi-Reader-Android) | 88.7 | A | 476 | GPL-3.0 | 2026-11-05 | 448 | 16 | ·数图 |
| 15 | [hthoene/paperpaw](https://github.com/hthoene/paperpaw) | 88.6 | A | 43 | Apache-2.0 | 2026-11-05 | 3330 | 11 | 驱数图 |
| 16 | [dmellok/tesserae-device-firmware](https://github.com/dmellok/tesserae-device-firmware) | 87.4 | A | 18 | AGPL-3.0 | 2026-10-31 | 281 | 11 | 驱数图 |
| 17 | [Xinyuan-LilyGO/T-Echo](https://github.com/Xinyuan-LilyGO/T-Echo) | 87.2 | A | 241 | MIT | 2026-11-02 | 10 | 4 | 驱数图 |
| 18 | [clackups/draftling](https://github.com/clackups/draftling) | 86.4 | B | 124 | MIT | 2026-11-05 | 868 | 4 | 驱数· |
| 19 | [spirosbond/FlickBook](https://github.com/spirosbond/FlickBook) | 85.7 | B | 12 | GPL-3.0 | 2026-10-26 | 46 | 2 | 驱数图 |
| 20 | [bitbank2/FastEPD](https://github.com/bitbank2/FastEPD) | 85.0 | B | 226 | Apache-2.0 | 2026-10-20 | 26 | 6 | 驱·图 |
| 21 | [mendhak/waveshare-epaper-display](https://github.com/mendhak/waveshare-epaper-display) | 84.2 | B | 530 | MIT | 2026-10-31 | 85 | 7 | 驱数· |
| 22 | [zeeyado/koassistant.koplugin](https://github.com/zeeyado/koassistant.koplugin) | 83.9 | B | 290 | GPL-3.0 | 2026-11-05 | 909 | 28 | ·数图 |
| 23 | [sbrin/lopaka](https://github.com/sbrin/lopaka) | 83.6 | B | 1771 | Apache-2.0 | 2026-10-19 | 45 | 6 | 驱·图 |
| 24 | [priyanshdas/inkleaf](https://github.com/priyanshdas/inkleaf) | 83.6 | B | 17 | GPL-3.0 | 2026-10-31 | 67 | 2 | 驱数图 |
| 25 | [wanderwildwood/kotozute](https://github.com/wanderwildwood/kotozute) | 83.6 | B | 13 | AGPL-3.0 | 2026-11-06 | 827 | 5 | 驱数图 |
| 26 | [SolderedElectronics/Inkplate-micropython](https://github.com/SolderedElectronics/Inkplate-micropython) | 83.4 | B | 93 | MIT | 2026-10-15 | 134 | 2 | 驱数图 |
| 27 | [koreader/koreader](https://github.com/koreader/koreader) | 83.0 | B | 30898 | AGPL-3.0 | 2026-11-06 | 511 | 26 | ·数图 |
| 28 | [EmirErtorer/ink-away.koplugin](https://github.com/EmirErtorer/ink-away.koplugin) | 82.7 | B | 149 | MIT | 2026-11-05 | 457 | 22 | ·数图 |
| 29 | [cryptomilk/hass-eink-dashboard](https://github.com/cryptomilk/hass-eink-dashboard) | 82.5 | B | 130 | Apache-2.0 | 2026-11-05 | 326 | 22 | ·数图 |
| 30 | [CasaVargas/deckle](https://github.com/CasaVargas/deckle) | 80.4 | B | 39 | GPL-3.0 | 2026-11-05 | 968 | 30 | ·数图 |
| 31 | [OpenDisplay/Firmware](https://github.com/OpenDisplay/Firmware) | 78.4 | B | 139 | GPL-3.0 | 2026-11-04 | 123 | 20 | 驱·· |
| 32 | [arnegiacomo/fugleramme](https://github.com/arnegiacomo/fugleramme) | 78.0 | B | 3991 | MIT | 2026-11-05 | 655 | 28 | ·数· |
| 33 | [ganten1998/grimoire-scribe](https://github.com/ganten1998/grimoire-scribe) | 77.9 | B | 23 | GPL-3.0 | 2026-11-05 | 859 | 3 | ·数图 |
| 34 | [ajaxjiang96/quote0-burnout](https://github.com/ajaxjiang96/quote0-burnout) | 77.1 | B | 10 | MIT | 2026-10-29 | 56 | 5 | ·数图 |
| 35 | [usetrmnl/larapaper](https://github.com/usetrmnl/larapaper) | 76.6 | B | 444 | MIT | 2026-11-05 | 224 | 22 | ·数· |
| 36 | [palemoky/paper-pi](https://github.com/palemoky/paper-pi) | 76.3 | B | 14 | GPL-3.0 | 2026-11-02 | 88 | 0 | 驱数图 |
| 37 | [chrisjtwomey/inkplate10-weather-cal](https://github.com/chrisjtwomey/inkplate10-weather-cal) | 75.8 | B | 129 | MIT | 2026-10-31 | 161 | 0 | 驱数· |
| 38 | [usetrmnl/trmnlp](https://github.com/usetrmnl/trmnlp) | 74.8 | B | 239 | MIT | 2026-10-31 | 252 | 9 | ·数· |
| 39 | [pavellunev/tablo](https://github.com/pavellunev/tablo) | 74.7 | B | 20 | MIT | 2026-11-04 | 17 | 0 | 驱数图 |
| 40 | [c0de111/tinta](https://github.com/c0de111/tinta) | 73.4 | C | 39 | Apache-2.0 | 2026-10-27 | 10 | 0 | 驱数图 |
| 41 | [melastmohican/epdsi](https://github.com/melastmohican/epdsi) | 73.3 | C | 12 | Apache-2.0 | 2026-11-06 | 182 | 1 | 驱·图 |
| 42 | [OpenDisplay/Home_Assistant_Integration](https://github.com/OpenDisplay/Home_Assistant_Integration) | 72.8 | C | 94 | Apache-2.0 | 2026-10-28 | 234 | 21 | ··图 |
| 43 | [wr/featherframe](https://github.com/wr/featherframe) | 72.8 | C | 26 | Apache-2.0 | 2026-10-30 | 386 | 0 | ·数图 |
| 44 | [Rhiz3K/InkyCloud-F1](https://github.com/Rhiz3K/InkyCloud-F1) | 72.6 | C | 17 | MIT | 2026-11-02 | 107 | 0 | ·数图 |
| 45 | [dietrichmax/dashink](https://github.com/dietrichmax/dashink) | 72.5 | C | 12 | MIT | 2026-11-05 | 85 | 0 | ·数图 |
| 46 | [eigger/hass-ble-esl](https://github.com/eigger/hass-ble-esl) | 71.4 | C | 23 | MIT | 2026-11-05 | 388 | 9 | ··图 |
| 47 | [OpenDisplay/epaper-dithering](https://github.com/OpenDisplay/epaper-dithering) | 68.9 | C | 16 | MIT | 2026-10-07 | 106 | 0 | 驱·图 |
| 48 | [btrekkie/eink-server](https://github.com/btrekkie/eink-server) | 68.2 | C | 26 | MIT | 2026-10-23 | 2 | 0 | 驱数图 |
| 49 | [kovaacs/sky_overhead](https://github.com/kovaacs/sky_overhead) | 67.3 | C | 11 | MIT | 2026-10-29 | 54 | 0 | ··图 |
| 50 | [psychoplath9450/SUMI](https://github.com/psychoplath9450/SUMI) | 63.8 | C | 184 | MIT | 2026-05-30 | 6 | 21 | 驱数图 |

<details><summary>被排除的仓库（前 40 条）</summary>

| 仓库 | 原因 |
| --- | --- |
| diyiwuyan/onebench | 近 180 天只有机器人在提交（dependabot / CI） |
| svg153/awesome-stars | 近 180 天只有机器人在提交（dependabot / CI） |
| PhyberApex/kuroshiro | 近 180 天只有机器人在提交（dependabot / CI） |
| ai-boost/awesome-image-video-prompts | 没有驱动 / 数据 / 出图能力信号 |
| YSGStudyHards/Awesome-Tools | 没有驱动 / 数据 / 出图能力信号 |
| Xiaoc7r/JLUCS_Speedrun | 没有驱动 / 数据 / 出图能力信号 |
| thypon/eLauncher | 没有驱动 / 数据 / 出图能力信号 |
| BookHeaven/BookHeaven.Reader | 没有驱动 / 数据 / 出图能力信号 |
| lautaroelkin/KiKomeScrubber.koplugin | 没有驱动 / 数据 / 出图能力信号 |
| AnkioTomas/ko-paper-sprite | 没有驱动 / 数据 / 出图能力信号 |
| ccch1mneyyy/dsh-TUI | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| CyrilPeng/Venera-Next | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| emby-keeper/emby-keeper | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| LC044/TrailSnap | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| Moyf/moys-asr-workflow | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| tribixbite/CleverKeys | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| guaner-334/civ-atlas | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| imsai-sh/awesome-deepseek-harness-plugins | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| sakurawwwxh/qq-agent-plus | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| KristanLaimon/PanelsPlus | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| yzfly/LightRead | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| Dudu0831/daodian | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| yasneg616/marathon-like-deepseek-harness-skin | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| jadzziaa/moonglow-toolset | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |
| xinzhuzi/MYStudio | 与墨水屏关系弱（文档里提到过，但仓库不以此为主） |

</details>

## 后续动作建议

1. 先发 PR / issue，不要一上来谈合作 —— 让对方在自己的仓库里看到真实改动。
2. PR 描述里只放一条身份链接（项目主页），别堆营销文案。
3. 对方回应后，再邀请到众创平台：注册 → 导入仓库 → 生成设备出图 → 上架。
4. 上架不等于买断：作者保留自己仓库的著作权，平台只承载「设备端呈现」。