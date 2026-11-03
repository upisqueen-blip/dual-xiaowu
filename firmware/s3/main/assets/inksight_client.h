#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class NetworkInterface;

// InkSight 后端对接：拉取设备端 BMP 图像，转换为 SSD1619 可直接刷新的
// 1-bit 帧数据（每行 kStride 字节，行优先，MSB 左，白=1 黑=0）。
//
// InkSight /api/render?w=400&h=300&persona=WEATHER 返回 Pillow 输出的
// 1-bit BMP（BM 头 + InfoHeader + 4-bytes-row-aligned bottom-up 像素数据）。
class InkSightClient {
public:
    static constexpr int kWidth = 400;
    static constexpr int kHeight = 300;
    static constexpr size_t kStride = (kWidth + 7) / 8;      // =50
    static constexpr size_t kFrameBytes = kStride * kHeight; // =15000

    // 加载/刷新一张画面到 caller 提供的 dst[kFrameBytes] 缓冲区。
    //   server:   如 "https://dual.xiaowuai.vip/backend" （不要尾部 /api/...）
    //   persona:  InkSight 模式名，例如 "WEATHER"、"COUNTDOWN"、"HABIT"、
    //             "ZEN"、"STOIC"、"VOCABULARY_REVIEW" 等；空字符串时用后端
    //             默认（有 MAC 时按绑定配置）。
    //   error:    失败时写错误描述（可作为屏上提示直接显示）
    // 返回 true 且 dst 被填满为目标帧；false 时 dst 会被填成全白。
    static bool Load(NetworkInterface* network,
                     const std::string& server,
                     const std::string& persona,
                     uint8_t* dst,
                     std::string& error);

    // 2bpp 彩色模式（三色屏 UC1679 用）：请求 bpp=2&colors=4，后端返回
    // application/octet-stream 的 400*300/4=30000 字节原始 2bpp 打包数据
    // （0=黑 1=白 2=黄 3=红，4 像素/字节 MSB first，行优先自顶向下）。
    // 分解输出两个 1bpp 平面（各 kPlaneBytes=15000 字节）：
    //   bw_plane:  1=白 0=黑（MSB 左，行优先）
    //   red_plane: 1=红 0=非红
    // 黄色(2)按 EPD_YELLOW_AS_RED 宏决定映射为红色(1)或白色(0)，默认红色。
    static bool Load2bpp(NetworkInterface* network,
                         const std::string& server,
                         const std::string& persona,
                         uint8_t* bw_plane,
                         uint8_t* red_plane,
                         std::string& error);

    // NVS 读写服务器地址（命名空间 inksight / key "server_url"）。
    static std::string LoadServerUrl();
    static void SaveServerUrl(const std::string& server);
};
