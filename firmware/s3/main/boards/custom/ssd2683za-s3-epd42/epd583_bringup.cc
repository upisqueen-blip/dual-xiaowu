#include "epd583_bringup.h"

#include <cstring>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "config.h"
#include "uc8179_panel.h"

namespace {

constexpr const char* kTag = "EPD583TEST";
constexpr int kW = Uc8179Panel::kWidth;
constexpr int kH = Uc8179Panel::kHeight;
constexpr size_t kStride = Uc8179Panel::kStride;
constexpr size_t kBytes = Uc8179Panel::kPlaneBytes;

void Fill(uint8_t* plane, uint8_t value) {
    std::memset(plane, value, kBytes);
}

// 逐像素写：white=true -> 1（白），false -> 0（黑）
void PutPixel(uint8_t* plane, int x, int y, bool white) {
    if (x < 0 || x >= kW || y < 0 || y >= kH) return;
    const size_t index = static_cast<size_t>(y) * kStride + (x >> 3);
    const uint8_t mask = static_cast<uint8_t>(0x80u >> (x & 7));
    if (white) {
        plane[index] |= mask;
    } else {
        plane[index] &= static_cast<uint8_t>(~mask);
    }
}

// cell x cell 的棋盘格：用来校验分辨率、X/Y 方向、MSB 位序。
void DrawCheckerboard(uint8_t* plane, int cell) {
    for (int y = 0; y < kH; ++y) {
        for (size_t xb = 0; xb < kStride; ++xb) {
            uint8_t value = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int x = static_cast<int>(xb) * 8 + bit;
                const bool black = (((x / cell) + (y / cell)) & 1) == 0;
                if (!black) value |= static_cast<uint8_t>(0x80u >> bit);
            }
            plane[y * kStride + xb] = value;
        }
    }
}

// 上半黑、下半白，四周留 border 像素黑边：用来校验上下方向与边框。
void DrawHalfSplit(uint8_t* plane, int border) {
    for (int y = 0; y < kH; ++y) {
        for (size_t xb = 0; xb < kStride; ++xb) {
            uint8_t value = 0;
            for (int bit = 0; bit < 8; ++bit) {
                const int x = static_cast<int>(xb) * 8 + bit;
                bool white;
                if (y < border || y >= kH - border || x < border ||
                    x >= kW - border) {
                    white = false;  // 黑边框
                } else {
                    white = (y >= kH / 2);
                }
                if (white) value |= static_cast<uint8_t>(0x80u >> bit);
            }
            plane[y * kStride + xb] = value;
        }
    }
}

// 三段横条：上 1/3 黑、中 1/3 白、下 1/3 红（红平面由调用方填）。
void DrawTriBars(uint8_t* bw, uint8_t* red) {
    Fill(bw, 0xff);
    Fill(red, 0x00);
    for (int y = 0; y < kH; ++y) {
        for (size_t xb = 0; xb < kStride; ++xb) {
            uint8_t bw_value = 0xff;
            uint8_t red_value = 0x00;
            for (int bit = 0; bit < 8; ++bit) {
                const int x = static_cast<int>(xb) * 8 + bit;
                bool white = true;
                bool is_red = false;
                if (y < kH / 3) {
                    white = false;  // 黑
                } else if (y >= (kH * 2) / 3) {
                    is_red = true;  // 红
                }
                // 12 px 黑边框，方便判断方向
                if (x < 12 || x >= kW - 12 || y < 12 || y >= kH - 12) {
                    white = false;
                    is_red = false;
                }
                const uint8_t mask = static_cast<uint8_t>(0x80u >> bit);
                if (!white) bw_value &= static_cast<uint8_t>(~mask);
                if (is_red) red_value |= mask;
            }
            bw[y * kStride + xb] = bw_value;
            red[y * kStride + xb] = red_value;
        }
    }
}

size_t CountBlack(const uint8_t* plane) {
    size_t black = 0;
    for (size_t i = 0; i < kBytes; ++i) {
        uint8_t v = plane[i];
        for (int bit = 0; bit < 8; ++bit) {
            if ((v & (0x80u >> bit)) == 0) ++black;
        }
    }
    return black;
}

[[noreturn]] void ParkForever() {
    ESP_LOGW(kTag, "自检结束，固件驻留（不启动应用）。");
    ESP_LOGW(kTag,
             "把 EPD_BRINGUP_TEST 改回 0 重新编译烧录，才回到正常业务固件。");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        ESP_LOGI(kTag, "自检驻留中…（画面应保持在最后一张自检图上）");
    }
}

}  // namespace

void RunEpd583BringupTest() {
    ESP_LOGW(kTag, "==== 5.83\" UC8179 双屏自检开始（约 90 秒，请勿断电）====");
    ESP_LOGW(kTag, "A=GPIO%d/GPIO%d(黑白)  B=GPIO%d/GPIO%d(三色)",
             EPD_CS_PIN_FPC1, EPD_BUSY_PIN_FPC1, EPD_CS_PIN_FPC2,
             EPD_BUSY_PIN_FPC2);

    auto* bw = static_cast<uint8_t*>(
        heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto* red = static_cast<uint8_t*>(
        heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (bw == nullptr || red == nullptr) {
        ESP_LOGE(kTag, "帧缓冲分配失败（需要 2 x %u 字节 PSRAM）",
                 static_cast<unsigned>(kBytes));
        ParkForever();
    }

    // ---------------- A 屏：黑白快刷 ----------------
    ESP_LOGW(kTag, "---- A 屏（FPC1，黑白）----");
    {
        Uc8179Panel a(EPD_CS_PIN_FPC1, EPD_BUSY_PIN_FPC1,
                      Uc8179Panel::Mode::kMono);
        if (!a.Begin("A屏")) {
            ESP_LOGE(kTag, "A 屏 SPI 初始化失败");
        } else if (!a.Init()) {
            ESP_LOGE(kTag, "A 屏 UC8179 初始化失败");
        } else {
            static const char* kNames[4] = {"全黑", "全白", "32px 棋盘格",
                                            "上半黑/下半白 + 黑边框"};
            for (int index = 0; index < 4; ++index) {
                switch (index) {
                    case 0:
                        Fill(bw, 0x00);
                        break;
                    case 1:
                        Fill(bw, 0xff);
                        break;
                    case 2:
                        DrawCheckerboard(bw, 32);
                        break;
                    default:
                        DrawHalfSplit(bw, 8);
                        break;
                }
                const size_t black = CountBlack(bw);
                const bool ok = a.Show(bw, nullptr);
                ESP_LOGW(kTag,
                         "A 自检 %d/4【%s】%s：黑像素=%u/%u，拉忙=%u ms，波形=%u ms",
                         index + 1, kNames[index], ok ? "完成" : "未确认",
                         static_cast<unsigned>(black),
                         static_cast<unsigned>(kW * kH),
                         static_cast<unsigned>(a.last_assert_ms()),
                         static_cast<unsigned>(a.last_busy_ms()));
                vTaskDelay(pdMS_TO_TICKS(2000));
            }
            a.Sleep();
            ESP_LOGW(kTag, "A 屏四张图案已下发完毕（这是 A 屏的最终判定依据）");
        }
    }

    // ---------------- B 屏：黑白红 ----------------
    ESP_LOGW(kTag, "---- B 屏（FPC2，三色）----");
    {
        Uc8179Panel b(EPD_CS_PIN_FPC2, EPD_BUSY_PIN_FPC2,
                      Uc8179Panel::Mode::kTriColor);
        if (!b.Begin("B屏")) {
            ESP_LOGE(kTag, "B 屏 SPI 初始化失败");
        } else if (!b.Init()) {
            ESP_LOGE(kTag, "B 屏 UC8179 初始化失败");
        } else {
            // 第 1、2 张只差红平面极性：全白 / 全红。哪张显示全红，
            // 就说明红平面是「0=红」；哪张全白，说明是「1=红」。
            Fill(bw, 0xff);
            Fill(red, 0x00);
            bool ok = b.Show(bw, red);
            ESP_LOGW(kTag, "B 自检 1/3【白底 + 红平面全 0x00】%s：期望整屏全白",
                     ok ? "完成" : "未确认");

            vTaskDelay(pdMS_TO_TICKS(3000));
            Fill(bw, 0xff);
            Fill(red, 0xff);
            ok = b.Show(bw, red);
            ESP_LOGW(kTag,
                     "B 自检 2/3【白底 + 红平面全 0xFF】%s：期望整屏全红"
                     "（若这屏全白而上一屏全红，则红平面极性是 1=红）",
                     ok ? "完成" : "未确认");

            vTaskDelay(pdMS_TO_TICKS(3000));
            DrawTriBars(bw, red);
            ok = b.Show(bw, red);
            ESP_LOGW(kTag,
                     "B 自检 3/3【上黑 / 中白 / 下红 三段横条 + 黑边框】%s："
                     "拉忙=%u ms，波形=%u ms",
                     ok ? "完成" : "未确认",
                     static_cast<unsigned>(b.last_assert_ms()),
                     static_cast<unsigned>(b.last_busy_ms()));
            ESP_LOGW(kTag, "B 屏三张图案已下发完毕（这是 B 屏的最终判定依据）");
        }
    }

    ESP_LOGW(kTag, "==================== 判读 ====================");
    ESP_LOGW(kTag, "① A 屏四张图有明显黑白变化 → A 屏（黑白）驱动成功");
    ESP_LOGW(kTag, "② B 屏第 1/2 张整屏变色、第 3 张出现红条 → B 屏（三色）驱动成功");
    ESP_LOGW(kTag, "③ 某块屏完全不动且日志里『BUSY 未拉忙』→ 该屏 IC 不是 UC8179，");
    ESP_LOGW(kTag, "   或 CS/BUSY 座子对不上 / FPC 没压好，把这屏的日志发我");
    ESP_LOGW(kTag, "=============================================");
    ParkForever();
}
