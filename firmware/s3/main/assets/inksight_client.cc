#include "inksight_client.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <http.h>
#include <network_interface.h>
#include <settings.h>

namespace {
constexpr char kTag[] = "XiaowuAI";

// BMP 文件头 + 最小 InfoHeader 合计 54 字节；InfoHeader 中关心的字段偏移。
constexpr size_t kFileHeaderBytes = 14;
constexpr size_t kMinInfoHeaderBytes = 40; // BITMAPINFOHEADER
constexpr uint8_t kBmpSig[2] = {'B', 'M'};

struct BmpHeaderLayout {
    uint32_t file_size;         // 2..6
    uint32_t pixel_offset;      // 10..14  像素数据起点（相对 BMP 开头）
    uint32_t info_size;         // 14..18
    int32_t  width;             // 18..22
    int32_t  height;            // 22..26  正=bottom-up；负=top-down
    uint16_t bits_per_pixel;    // 28..30
    uint32_t compression;       // 30..34  BI_RGB=0, BI_BITFIELDS=3
    uint32_t image_size_bytes;  // 34..38  可=0（BI_RGB 未压缩）
    uint32_t colors_used;       // 46..50
};

// 从原始 BMP 头部字节提取关键字段，字段在文件中都是 little-endian。
bool ParseHeader(const uint8_t* raw, size_t raw_size, BmpHeaderLayout& out,
                 std::string& error) {
    if (raw_size < kFileHeaderBytes + kMinInfoHeaderBytes) {
        error = "BMP header too short";
        return false;
    }
    if (raw[0] != kBmpSig[0] || raw[1] != kBmpSig[1]) {
        error = "not a BMP (no BM signature)";
        return false;
    }
    auto u32 = [&](size_t off) {
        return static_cast<uint32_t>(raw[off])
             | (static_cast<uint32_t>(raw[off + 1]) << 8)
             | (static_cast<uint32_t>(raw[off + 2]) << 16)
             | (static_cast<uint32_t>(raw[off + 3]) << 24);
    };
    auto i32 = [&](size_t off) {
        return static_cast<int32_t>(u32(off));
    };
    auto u16 = [&](size_t off) {
        return static_cast<uint16_t>(raw[off])
             | (static_cast<uint16_t>(raw[off + 1]) << 8);
    };
    out.file_size        = u32(2);
    out.pixel_offset     = u32(10);
    out.info_size        = u32(14);
    out.width            = i32(18);
    out.height           = i32(22);
    out.bits_per_pixel   = u16(28);
    out.compression      = u32(30);
    out.image_size_bytes = u32(34);
    out.colors_used      = u32(46);
    if (out.info_size < kMinInfoHeaderBytes) {
        error = "unsupported BMP info header size";
        return false;
    }
    if (out.width != InkSightClient::kWidth ||
        (out.height != InkSightClient::kHeight &&
         out.height != -static_cast<int32_t>(InkSightClient::kHeight))) {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "BMP size mismatch: got %ldx%ld, expect %dx%d",
                      static_cast<long>(out.width), static_cast<long>(out.height),
                      InkSightClient::kWidth, InkSightClient::kHeight);
        error = buf;
        return false;
    }
    if (out.bits_per_pixel != 1 && out.bits_per_pixel != 8 &&
        out.bits_per_pixel != 24) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "unsupported bpp=%u",
                      static_cast<unsigned>(out.bits_per_pixel));
        error = buf;
        return false;
    }
    if (out.compression != 0 /* BI_RGB */) {
        error = "compressed BMP not supported";
        return false;
    }
    return true;
}

// Pillow image.convert("1").save BMP 色板 = [黑=0,#000000, 白=1,#FFFFFF]
// 对应 BMP 中 palette index -> B/W：0=黑，1=白。SSD1619 需要白=1 黑=0，
// 跟 Pillow 的 1-bit palette 完全一致；若收到 8-bit/24-bit 则按亮度阈值 128
// 归 1（白） / 0（黑）。
constexpr int kLumaThreshold = 128 * 256; // match LVGL: 77r+150g+29b >= 128*256

// 1-bit BMP 的每行字节数：((w+31)/32)*4；8-bit: ((w+3)/4)*4；24-bit: ((w*3+3)/4)*4。
inline size_t RowBytes(const BmpHeaderLayout& h, unsigned bpp, int w) {
    if (bpp == 1)       return static_cast<size_t>((w + 31) / 32 * 4);
    if (bpp == 8)       return static_cast<size_t>((w + 3) / 4 * 4);
    if (bpp == 24)      return static_cast<size_t>((w * 3 + 3) / 4 * 4);
    return 0;
}
}  // namespace

bool InkSightClient::Load(NetworkInterface* network,
                          const std::string& server,
                          const std::string& persona,
                          uint8_t* dst,
                          std::string& error) {
    // 先填白，出错时 caller 也能直接刷掉上一帧
    std::memset(dst, 0xFF, kFrameBytes);
    if (!network) {
        error = "no network interface";
        return false;
    }
    if (server.empty()) {
        error = "XiaowuAI server URL not set";
        return false;
    }

    std::string url = server;
    if (!url.empty() && url.back() == '/') url.pop_back();
    // v3 渲染协议：/api/render?w=400&h=300&v=3.2
    url += "/api/render?w=";
    url += std::to_string(kWidth);
    url += "&h=";
    url += std::to_string(kHeight);
    url += "&v=3.2";
    if (!persona.empty()) {
        url += "&persona=";
        // 粗转义：保证常见 ASCII 字符、汉字经 Python QUERY_STRING 可接收
        for (unsigned char c : persona) {
            char hex[4];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
                url.push_back(static_cast<char>(c));
            } else {
                std::snprintf(hex, sizeof(hex), "%%%02X", c);
                url += hex;
            }
        }
    }

    // 判定 TCP 层错误是否可重试：
    // 104 ECONNRESET  110 ETIMEDOUT  111 ECONNREFUSED
    // 112 EHOSTDOWN   113 EHOSTUNREACH  114 EALREADY
    // 115 EINPROGRESS 116 EINTR（轻量重试无妨） 118 ENOTCONN
    // 超时也一并当作连接失败处理。
    auto IsTcpRetryable = [](int err) {
        switch (err) {
            case 0:   // 静默断开（对方 FIN 无错误码）
            case 104: case 110: case 111: case 112: case 113:
            case 114: case 115: case 116: case 118:
                return true;
            default: return false;
        }
    };
    auto IsStatusRetryable = [](int code) {
        return code == 429 || code == 500 || code == 502 ||
               code == 503 || code == 504;
    };

    // 最多 5 次尝试，连接层/响应层错误做指数退避。
    constexpr int kMaxAttempts = 5;
    int last_tcp_err = 0;
    int last_http_code = 0;
    int last_stream_err = 0;
    size_t last_stream_got = 0;
    int last_rows_done = 0;
    for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
        if (attempt > 0) {
            // 250ms * 2^(attempt-1): 250,500,1000,2000 ms
            const int delay_ms = 250 * (1 << (attempt - 1));
            ESP_LOGW(kTag,
                     "XiaowuAI retry %d/%d after %d ms "
                     "(tcp_err=%d http=%d stream_err=%d rows=%d/%d got=%u)",
                     attempt, kMaxAttempts - 1, delay_ms,
                     last_tcp_err, last_http_code,
                     last_stream_err, last_rows_done,
                     InkSightClient::kHeight,
                     static_cast<unsigned>(last_stream_got));
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }

        auto http = network->CreateHttp(/*connect_id=*/-1);
        if (!http) {
            error = "CreateHttp failed";
            continue;
        }
        // 45s：/render 在返回任何字节前要等整图渲染完（冷缓存含 LLM 调用，
        // 可能超过 30s），头部等待和流中静默都按这个超时算。
        http->SetTimeout(45000);
        http->SetKeepAlive(false);
        // InkSight API 文档示例的简单 UA；identity 明确拒绝 gzip 等压缩封装
        http->SetHeader("User-Agent", "xiaozhi-epaper/1.0 (Ssd1619)");
        http->SetHeader("Accept-Encoding", "identity");
        if (!http->Open("GET", url)) {
            last_tcp_err = http->GetLastError();
            last_http_code = 0;
            if (!IsTcpRetryable(last_tcp_err)) {
                char buf[64];
                std::snprintf(buf, sizeof(buf),
                              "HTTP open failed err=%d", last_tcp_err);
                error = buf;
                return false;
            }
            continue;
        }
        last_tcp_err = 0;
        const int code = http->GetStatusCode();
        last_http_code = code;
        if (code < 200 || code >= 300) {
            if (IsStatusRetryable(code) && attempt + 1 < kMaxAttempts) continue;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "HTTP %d", code);
            error = buf;
            return false;
        }

    // 1. 先吞 BMP 头（文件头+InfoHeader+可能的调色板）。我们需要拿到
    //    pixel_offset 才能准确定位像素起点，但流式读的前面一部分是连续的，
    //    先攒够 minimum header。
    constexpr size_t kHeaderBudget = kFileHeaderBytes + kMinInfoHeaderBytes
                                   + 256 * 4;   // 最多 256 色 调色板
    uint8_t header_buf[kHeaderBudget];
    size_t header_got = 0;
    // 返回的总像素字节数（仅像素部分）；流式读可能没有 Content-Length。
    const size_t content_length = static_cast<size_t>(
        std::max<int64_t>(0, static_cast<int64_t>(http->GetBodyLength())));
    (void)content_length;

    bool header_done = false;
    BmpHeaderLayout hdr{};
    size_t pixel_start_abs = 0; // 像素起点的绝对字节数（相对 BMP 起点）

    // 2. 像素段缓冲区 & 行解码状态。
    //    我们按 BMP row_bytes 缓存一行，然后翻译成 SSD1619 的一行 50 字节。
    //    BMP 行是 4 字节对齐，50/行 -> row_bytes 1-bit = 52。
    constexpr int kW = InkSightClient::kWidth;
    constexpr int kH = InkSightClient::kHeight;

    // 状态机
    enum Phase : uint8_t {
        kPhaseHeader,       // 还没收够 header_buf
        kPhaseDiscardPad,   // 丢掉 (pixel_offset - header_got) 个字节
        kPhasePixels,       // 读像素 -> 翻译
        kPhaseDone,
        kPhaseError,
    };
    Phase phase = kPhaseHeader;
    size_t discard_remain = 0;
    unsigned bpp = 0;
    size_t row_bytes = 0;
    size_t row_got = 0;   // 当前行已收字节数
    // 动态分配行缓冲（row_bytes 不超过 52，小栈也放得下），但为了后续兼容
    // 8/24-bit 方案，统一放在外部 PSRAM。
    uint8_t* row_buf = static_cast<uint8_t*>(heap_caps_malloc(
        4096, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!row_buf) {
        error = "PSRAM OOM for BMP row";
        return false;
    }
    int rows_done = 0; // 已写完多少行（SSD1619 行号 0=顶部）
    bool bmp_bottom_up = true;
    size_t total_body_got = 0; // 已收到的响应体字节（诊断用）

    // 把一行 BMP 像素（src，长度必须等于 row_bytes）翻译到 SSD1619 帧
    // 的第 y_dst 行。供 streaming 与 header-surplus 两条路径复用。
    auto DecodeRow = [&](const uint8_t* src, int y_dst) {
        if (y_dst < 0 || y_dst >= kH) return; // 行溢出：丢弃
        uint8_t* dst_row = dst
            + static_cast<size_t>(y_dst) * InkSightClient::kStride;
        if (bpp == 1) {
            // Pillow 1-bit BMP：每字节 8 像素，bit7=最左；
            // palette[0]=黑,palette[1]=白。SSD1619 帧同语义，直接拷贝
            // 前 kStride(50) 字节即可，行尾 2 字节对齐 padding 自然截断。
            std::memcpy(dst_row, src, InkSightClient::kStride);
        } else if (bpp == 8) {
            for (int x = 0; x < kW; ++x) {
                const uint8_t g = src[x];
                const bool white =
                    (static_cast<int>(g) * 256) >= kLumaThreshold;
                const size_t bit = 7 - (x & 7);
                const size_t bi = x >> 3;
                if (white) dst_row[bi] |= (1u << bit);
                else       dst_row[bi] &= ~(1u << bit);
            }
        } else if (bpp == 24) {
            for (int x = 0; x < kW; ++x) {
                const uint8_t b = src[x * 3 + 0];
                const uint8_t g = src[x * 3 + 1];
                const uint8_t r = src[x * 3 + 2];
                const int luma = 77 * r + 150 * g + 29 * b;
                const bool white = luma >= kLumaThreshold;
                const size_t bit = 7 - (x & 7);
                const size_t bi = x >> 3;
                if (white) dst_row[bi] |= (1u << bit);
                else       dst_row[bi] &= ~(1u << bit);
            }
        }
    };

    // flush_header 之后，消费 header_buf 里 pixel_offset 起的全部像素前缀：
    // 逐行解码完整行，剩余不足一行的部分留给 streaming 阶段拼行。
    auto ConsumeSurplusPixels = [&]() {
        size_t avail = header_got - pixel_start_abs;
        const uint8_t* pp = header_buf + pixel_start_abs;
        while (avail >= row_bytes && rows_done < kH && phase != kPhaseError) {
            const int y_dst = bmp_bottom_up ? (kH - 1 - rows_done) : rows_done;
            DecodeRow(pp, y_dst);
            ++rows_done;
            pp += row_bytes;
            avail -= row_bytes;
        }
        if (rows_done >= kH) {
            phase = kPhaseDone;
            row_got = 0;
            return;
        }
        std::memcpy(row_buf, pp, avail);
        row_got = avail;
        phase = kPhasePixels;
    };

    auto flush_header = [&]() {
        if (!ParseHeader(header_buf, header_got, hdr, error)) {
            phase = kPhaseError;
            return;
        }
        bpp = hdr.bits_per_pixel;
        row_bytes = RowBytes(hdr, bpp, hdr.width);
        if (row_bytes == 0 || row_bytes > 4096) {
            error = "bmp row_bytes invalid";
            phase = kPhaseError;
            return;
        }
        bmp_bottom_up = hdr.height > 0;
        pixel_start_abs = hdr.pixel_offset;
        if (pixel_start_abs < header_got) {
            // 常见路径：第一次 Read 就带回了 pixel_offset 之后的一段像素
            // （header_buf 预算 1078 字节，首个 chunk 通常 1024+ 字节）。
            // 必须完整消化前缀，逐行解码，否则会丢行导致 rows=282/300。
            ConsumeSurplusPixels();
            discard_remain = 0;
        } else {
            discard_remain = pixel_start_abs - header_got;
            phase = discard_remain == 0 ? kPhasePixels : kPhaseDiscardPad;
        }
        header_done = true;
    };

    uint8_t chunk[1024];
    while (phase != kPhaseDone && phase != kPhaseError) {
        const int n = http->Read(reinterpret_cast<char*>(chunk), sizeof(chunk));
        if (n <= 0) {
            if (phase == kPhasePixels && rows_done == kH) {
                phase = kPhaseDone;
            } else {
                char buf[112];
                std::snprintf(buf, sizeof(buf),
                              "HTTP stream end early: rows=%d/%d phase=%d "
                              "got=%u err=%d",
                              rows_done, kH, phase,
                              static_cast<unsigned>(total_body_got),
                              http->GetLastError());
                error = buf;
                phase = kPhaseError;
            }
            break;
        }
        total_body_got += static_cast<size_t>(n);
        const uint8_t* p = chunk;
        size_t remain = static_cast<size_t>(n);
        while (remain && phase != kPhaseError && phase != kPhaseDone) {
            switch (phase) {
                case kPhaseHeader: {
                    const size_t want = kHeaderBudget - header_got;
                    const size_t take = std::min(want, remain);
                    std::memcpy(header_buf + header_got, p, take);
                    header_got += take;
                    p += take;
                    remain -= take;
                    // 我们需要的最小 header = pixel_offset >= 14+40 = 54；
                    // 当 header_got 达到了 pixel_offset 的最小值就可以
                    // 尝试 parse（ParseHeader 会自行读足够字节）。
                    if (header_got >= kFileHeaderBytes + kMinInfoHeaderBytes) {
                        // 需要能读取 pixel_offset 字段（偏移 10），所以
                        // >= 14 字节就够；ParseHeader 再决定是否完整。
                        flush_header();
                        if (phase == kPhaseError) break;
                        // 如果 kHeaderBudget 不够 pixel_offset，说明调色板
                        // 特别大，需要 discard 掉差值（kPhaseDiscardPad）。
                    }
                    break;
                }
                case kPhaseDiscardPad: {
                    const size_t take = std::min(discard_remain, remain);
                    discard_remain -= take;
                    p += take;
                    remain -= take;
                    if (discard_remain == 0) phase = kPhasePixels;
                    break;
                }
                case kPhasePixels: {
                    // 填 row_buf[row_got ... row_bytes-1]，填完就翻译写 dst。
                    size_t need = row_bytes - row_got;
                    size_t take = std::min(need, remain);
                    std::memcpy(row_buf + row_got, p, take);
                    row_got += take;
                    p += take;
                    remain -= take;
                    if (row_got == row_bytes) {
                        const int y_dst =
                            bmp_bottom_up ? (kH - 1 - rows_done) : rows_done;
                        DecodeRow(row_buf, y_dst);
                        ++rows_done;
                        row_got = 0;
                        if (rows_done >= kH) {
                            phase = kPhaseDone;
                            // 丢弃剩余流
                            break;
                        }
                    }
                    break;
                }
                default: break;
            }
        }
    }

    heap_caps_free(row_buf);
    if (phase == kPhaseError) {
        // 流读取失败：可能是网络瞬断，收集状态让外层重试
        last_stream_err = last_tcp_err;
        last_stream_got = total_body_got;
        last_rows_done = rows_done;
        if (attempt + 1 < kMaxAttempts) continue;
        return false;  // 错误信息已在 phase==kPhaseError 处设置
    }
    if (rows_done != kH) {
        error = "incomplete BMP pixel rows";
        last_stream_err = last_tcp_err;
        last_stream_got = total_body_got;
        last_rows_done = rows_done;
        if (attempt + 1 < kMaxAttempts) continue;
        return false;
    }
    ESP_LOGI(kTag, "BMP loaded: %dx%d (bpp=%u, %s) attempt=%d",
             kW, kH, bpp, bmp_bottom_up ? "bottom-up" : "top-down",
             attempt + 1);
        return true;
    }  // for (attempt ...) 重试循环
    // 所有重试耗尽仍未成功（每次都以可重试错误 continue 到这里结束）
    if (error.empty()) {
        char buf[64];
        std::snprintf(buf, sizeof(buf),
                      "retries exhausted (tcp=%d http=%d rows=%d)",
                      last_tcp_err, last_http_code, last_rows_done);
        error = buf;
    }
    return false;
}  // InkSightClient::Load

std::string InkSightClient::LoadServerUrl() {
    Settings s("inksight", /*read_write=*/false);
    std::string url = s.GetString("server_url", "");
    return url;
}

void InkSightClient::SaveServerUrl(const std::string& server) {
    Settings s("inksight", /*read_write=*/true);
    s.SetString("server_url", server);
}

// ---------------------------------------------------------------------------
// 2bpp 彩色拉取（三色屏 UC1679）。
// 后端 /api/render?bpp=2&colors=4 返回 application/octet-stream：
// 400*300/4 = 30000 字节，每字节 4 像素（MSB first），像素值 0=黑 1=白
// 2=黄 3=红，行优先自顶向下（backend/core/renderer.py::image_to_raw_2bpp）。
// ---------------------------------------------------------------------------
bool InkSightClient::Load2bpp(NetworkInterface* network,
                              const std::string& server,
                              const std::string& persona,
                              uint8_t* bw_plane,
                              uint8_t* red_plane,
                              std::string& error) {
    static constexpr size_t kRaw2bppBytes = kWidth * kHeight / 4;  // 30000
    static constexpr size_t kPlaneBytes = kStride * kHeight;       // 15000

    // 出错时输出全白 + 无红，caller 可直接刷掉上一帧
    std::memset(bw_plane, 0xFF, kPlaneBytes);
    std::memset(red_plane, 0x00, kPlaneBytes);
    if (!network) {
        error = "no network interface";
        return false;
    }
    if (server.empty()) {
        error = "XiaowuAI server URL not set";
        return false;
    }

    std::string url = server;
    if (!url.empty() && url.back() == '/') url.pop_back();
    url += "/api/render?w=";
    url += std::to_string(kWidth);
    url += "&h=";
    url += std::to_string(kHeight);
    url += "&v=3.2&bpp=2&colors=4";
    if (!persona.empty()) {
        url += "&persona=";
        for (unsigned char c : persona) {
            char hex[4];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
                url.push_back(static_cast<char>(c));
            } else {
                std::snprintf(hex, sizeof(hex), "%%%02X", c);
                url += hex;
            }
        }
    }

    // raw 缓冲放 PSRAM（30000 字节）
    uint8_t* raw = static_cast<uint8_t*>(
        heap_caps_malloc(kRaw2bppBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (!raw) {
        error = "PSRAM OOM for 2bpp frame";
        return false;
    }

    auto IsTcpRetryable = [](int err) {
        switch (err) {
            case 0: case 104: case 110: case 111: case 112: case 113:
            case 114: case 115: case 116: case 118:
                return true;
            default: return false;
        }
    };
    auto IsStatusRetryable = [](int code) {
        return code == 429 || code == 500 || code == 502 ||
               code == 503 || code == 504;
    };

    bool ok = false;
    constexpr int kMaxAttempts = 5;
    for (int attempt = 0; attempt < kMaxAttempts && !ok; ++attempt) {
        if (attempt > 0) {
            const int delay_ms = 250 * (1 << (attempt - 1));
            ESP_LOGW(kTag, "XiaowuAI 2bpp retry %d/%d after %d ms",
                     attempt, kMaxAttempts - 1, delay_ms);
            vTaskDelay(pdMS_TO_TICKS(delay_ms));
        }
        auto http = network->CreateHttp(/*connect_id=*/-1);
        if (!http) {
            error = "CreateHttp failed";
            continue;
        }
        http->SetTimeout(45000);
        http->SetKeepAlive(false);
        http->SetHeader("User-Agent", "xiaozhi-epaper/1.0 (Uc1679)");
        http->SetHeader("Accept-Encoding", "identity");
        if (!http->Open("GET", url)) {
            const int tcp_err = http->GetLastError();
            if (!IsTcpRetryable(tcp_err)) {
                char buf[64];
                std::snprintf(buf, sizeof(buf),
                              "HTTP open failed err=%d", tcp_err);
                error = buf;
                break;
            }
            continue;
        }
        const int code = http->GetStatusCode();
        if (code < 200 || code >= 300) {
            if (IsStatusRetryable(code) && attempt + 1 < kMaxAttempts) continue;
            char buf[64];
            std::snprintf(buf, sizeof(buf), "HTTP %d", code);
            error = buf;
            break;
        }

        // 流式读满 kRaw2bppBytes；多读的部分丢弃。
        size_t got = 0;
        bool stream_ok = true;
        while (got < kRaw2bppBytes) {
            const int n = http->Read(
                reinterpret_cast<char*>(raw + got),
                static_cast<int>(kRaw2bppBytes - got));
            if (n <= 0) {
                char buf[96];
                std::snprintf(buf, sizeof(buf),
                              "stream end early: got=%u/%u err=%d",
                              static_cast<unsigned>(got),
                              static_cast<unsigned>(kRaw2bppBytes),
                              http->GetLastError());
                error = buf;
                stream_ok = false;
                break;
            }
            got += static_cast<size_t>(n);
        }
        if (!stream_ok) {
            if (attempt + 1 < kMaxAttempts) continue;
            break;
        }

        // 2bpp -> BW/RED 两个 1bpp 平面。
        // bw_plane:  1=白 0=黑
        // red_plane: 1=红 0=非红（黄按 EPD_YELLOW_AS_RED 映射）
        std::memset(bw_plane, 0xFF, kPlaneBytes);
        std::memset(red_plane, 0x00, kPlaneBytes);
        size_t pixel = 0;
        for (size_t i = 0; i < kRaw2bppBytes; ++i) {
            const uint8_t packed = raw[i];
            for (int nib = 3; nib >= 0; --nib, ++pixel) {
                const uint8_t value =
                    static_cast<uint8_t>((packed >> (nib * 2)) & 0x03);
                const size_t bi = pixel >> 3;
                const uint8_t mask = 0x80 >> (pixel & 7);
                switch (value) {
                    case 0:  // 黑
                        bw_plane[bi] &= static_cast<uint8_t>(~mask);
                        break;
                    case 1:  // 白：bw_plane 已预填 0xFF
                        break;
                    case 2:  // 黄
#if EPD_YELLOW_AS_RED
                        red_plane[bi] |= mask;
#else
                        // 丢弃为白色：bw_plane 已预填 0xFF
#endif
                        break;
                    case 3:  // 红
                        bw_plane[bi] &= static_cast<uint8_t>(~mask);
                        red_plane[bi] |= mask;
                        break;
                    default:
                        break;
                }
            }
        }
        ok = true;
        ESP_LOGI(kTag, "2bpp raw loaded: %u bytes -> bw+red planes",
                 static_cast<unsigned>(kRaw2bppBytes));
    }

    heap_caps_free(raw);
    return ok;
}
