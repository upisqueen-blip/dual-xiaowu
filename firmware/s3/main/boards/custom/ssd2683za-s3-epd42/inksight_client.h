#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "config.h"  // DISPLAY_WIDTH / FPC2_WIDTH — panel-size menuconfig choices

class NetworkInterface;

// InkSight 后端对接：拉取设备端 BMP 图像，转换为 FPC1 黑白快刷屏（SSD1619）
// 可直接刷新的 1-bit 帧数据（每行 kStride 字节，行优先，MSB 左，白=1 黑=0）。
// 尺寸来自 menuconfig（FPC1 默认 4.2"=400x300，见 config.h）。
//
// InkSight /api/render?w=<W>&h=<H>&persona=WEATHER 返回 Pillow 输出的
// 1-bit BMP（BM 头 + InfoHeader + 4-bytes-row-aligned bottom-up 像素数据）。
class InkSightClient {
public:
    static constexpr int kWidth = DISPLAY_WIDTH;    // FPC1 主屏宽（黑白快刷）
    static constexpr int kHeight = DISPLAY_HEIGHT;  // FPC1 主屏高
    static constexpr size_t kStride = (kWidth + 7) / 8;
    static constexpr size_t kFrameBytes = kStride * kHeight;

    // FPC2 彩色屏（SSD2683ZA）维度，独立于 FPC1 主屏。
    static constexpr int kColorWidth = FPC2_WIDTH;
    static constexpr int kColorHeight = FPC2_HEIGHT;
    static constexpr size_t kColorStride = (kColorWidth + 7) / 8;
    static constexpr size_t kColorPlaneBytes = kColorStride * kColorHeight;
    static constexpr size_t kColorRaw2bppBytes = kColorWidth * kColorHeight / 4;

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
                     const std::string& device_id,
                     uint8_t* dst,
                     std::string& error,
                     int* server_refresh_minutes = nullptr,
                     int battery_percent = -1,  // >=0 时附加 &bat=xx 供后端绘制电量
                     bool* device_unbound_out = nullptr,
                     std::string* pair_code_out = nullptr);

    // 2bpp 彩色模式（FPC2 彩色屏 SSD2683ZA 用）：请求 bpp=2&colors=4，后端返回
    // application/octet-stream 的原始 2bpp 打包数据（尺寸 = FPC2 屏幕尺寸，
    // 默认 400*300/4=30000 字节；0=黑 1=白 2=黄 3=红，4 像素/字节 MSB first，
    // 行优先自顶向下）。分解输出两个 1bpp 平面（各 kColorPlaneBytes 字节）：
    //   bw_plane:  1=白 0=黑（MSB 左，行优先）
    //   red_plane: 1=红 0=非红
    // 黄色(2)按 EPD_YELLOW_AS_RED 宏决定映射为红色(1)或白色(0)，默认红色。
    static bool Load2bpp(NetworkInterface* network,
                         const std::string& server,
                         const std::string& persona,
                         const std::string& device_id,
                         uint8_t* bw_plane,
                         uint8_t* red_plane,
                         std::string& error,
                         int* server_refresh_minutes = nullptr,
                         bool* device_unbound_out = nullptr,
                         std::string* pair_code_out = nullptr);

    // 拉取双屏控制台配置（GET /api/device/<mac>/dual-config，原始 JSON 存入
    // json_out）。无配置时后端返回 {"config":null}（HTTP 200，仍算成功）。
    // 用于开机/定时探测「这台设备是否启用了双屏模式」。
    static bool LoadDualConfig(NetworkInterface* network,
                               const std::string& server,
                               const std::string& device_id,
                               std::string& json_out,
                               std::string* error = nullptr);

    // 上报设备电源模式，供后台设备配置页同步活跃/间歇及深睡状态。
    static bool ReportRuntimeMode(NetworkInterface* network,
                                  const std::string& server,
                                  const std::string& device_id,
                                  bool active,
                                  bool awake = true,
                                  std::string* error = nullptr,
                                  bool* refresh_required = nullptr);

    // 上报 TF 卡存储快照，供后台「本地 TF 卡」面板读取：
    // POST /api/device/<mac>/storage，body 为设备构造的 JSON 快照。
    static bool ReportStorage(NetworkInterface* network,
                             const std::string& server,
                             const std::string& device_id,
                             const std::string& snapshot_json,
                             std::string* error = nullptr);

    // 解除本设备与账号的绑定（用于重新配对）：POST /api/device/<mac>/unbind，
    // 成功后清本地 device_token。返回 false 不一定仍绑定——后端 404 如果是
    // "Not Found"（老后端没有该路由）会如实返回 false，提示需先升级后端。
    static bool UnbindSelf(NetworkInterface* network,
                           const std::string& server,
                           const std::string& device_id,
                           std::string* error);

    // 语音端/本机改 B 屏 K 线标的：query 可为 6 位代码或股票名称（如
    // "688018" / "乐鑫科技"）。后端解析后写入设备配置并置位立即刷新。
    // 成功时 name_out 回填证券名称。
    static bool SetFinanceKline(NetworkInterface* network,
                                const std::string& server,
                                const std::string& device_id,
                                const std::string& query,
                                std::string* name_out,
                                std::string* error);

    // NVS 读写服务器地址（命名空间 inksight / key "server_url"）。
    static std::string LoadServerUrl();
    static void SaveServerUrl(const std::string& server);

    // 自动选路（联网后调用）：优先 NVS 手工覆盖（仅当可达），否则按
    // INKSIGHT_SERVER_CANDIDATES 探测首个可达后端，最后兜底生产域名。
    // network 为空时直接返回编译期默认地址。
    static std::string ResolveServerUrl(NetworkInterface* network);

    // HTTPS 前置校时：S3 无备份 RTC，冷启动时钟停在 1970，mbedtls 会因证书
    // Not-Before 校验失败报 -76。线上后端走 https，故任何 HTTPS 请求前先调用
    // 本函数（时钟已有效时立即返回，无网络开销）。最多阻塞 6 秒，超时也继续。
    static void EnsureClockForTls();

    // 探测 base_url 是否为可达的真后端（GET /health 返回 2xx）。
    static bool ProbeServerHealth(NetworkInterface* network,
                                  const std::string& base_url);

    // 后端在下发渲染结果时若带 X-Pending-Refresh: 1（内容后台点了
    // 「应用到设备」），取图时会置位此标记。刷新循环每分钟调用一次
    // TakePendingRefresh()，返回 true 表示立即刷一次双屏。
    static bool TakePendingRefresh();
};
