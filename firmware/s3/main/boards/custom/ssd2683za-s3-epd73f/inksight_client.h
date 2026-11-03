#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

class NetworkInterface;

// /api/render 的可选上报字段（与 InkSight 官方固件 network.cpp::fetchBMP
// 完全对齐：后端 core/schemas.py::RenderQuery 接受这些参数）。
//   v         电池电压（伏）；<=0 时不带该参数，后端默认 3.3
//   rssi      WiFi 信号强度（dBm，负数）；0 表示不上报
//   refresh   设备当前生效的刷新间隔（分钟）；0 表示不上报
//   next_mode true 时追加 &next=1，请求后端切到下一个模式
// 后两个输出指针用于回传响应头 X-Mode-Id / X-Content-Fallback。
struct InkSightRenderHints {
    float voltage = 0.0f;
    int rssi = 0;
    int refresh_minutes = 0;
    bool next_mode = false;
    std::string* mode_id_out = nullptr;
    bool* content_fallback_out = nullptr;
};

// InkSight 后端对接（ssd2683za-s3-epd73f 板型专用 800x480 版本）。
//
// 与 epd42 板的 inksight_client 的区别：
//   - 分辨率常量为 800x480（七彩屏面板尺寸），2bpp 流为 96000 字节；
//   - Load2bpp 额外输出独立的黄色平面：七彩屏（7 色）可以真正显示黄色，
//     不再像三色屏那样把黄映射为红。三平面按 红 > 黄 > 黑白 优先级合成。
//   - Load4bpp 拉「规范 4bpp 帧」（0黑1白2黄3红4蓝5绿），把六色面板的
//     蓝/绿两个通道也用起来；后端不支持时返回 false，调用方回落 Load2bpp。
//
// 后端要求：/api/render 支持 w=800&h=480&bpp=2&colors=4（与 7.5" 800x480
// 单色设备的 w/h 参数同规格，bpp=2 通道需后端按分辨率放行）。
class InkSightClient {
public:
    static constexpr int kWidth = 800;
    static constexpr int kHeight = 480;
    static constexpr size_t kStride = (kWidth + 7) / 8;       // =100
    static constexpr size_t kFrameBytes = kStride * kHeight;  // =48000
    static constexpr size_t kPlaneBytes = kFrameBytes;        // 每平面 48000
    // 4bpp 帧字节数：800*480/2 = 192000（每字节 2 像素，高半字节在左），
    // 与面板原生帧格式一致，拉下来可直接刷。
    static constexpr size_t kFrame4bppBytes = kWidth * kHeight / 2;

    // 加载/刷新一张 1-bit 画面到 caller 提供的 dst[kFrameBytes] 缓冲区。
    static bool Load(NetworkInterface* network,
                     const std::string& server,
                     const std::string& persona,
                     const std::string& device_id,
                     uint8_t* dst,
                     std::string& error,
                     int* server_refresh_minutes = nullptr);

    // 2bpp 彩色模式（七彩屏用）：请求 bpp=2&colors=4，后端返回
    // application/octet-stream 的 800*480/4 = 96000 字节原始 2bpp 打包
    // 数据（0=黑 1=白 2=黄 3=红，4 像素/字节 MSB first，行优先自顶向下）。
    // 分解输出三个 1bpp 平面（各 kPlaneBytes=48000 字节）：
    //   bw_plane:     1=白 0=黑（MSB 左，行优先）
    //   yellow_plane: 1=黄 0=非黄
    //   red_plane:    1=红 0=非红
    // yellow/red 可传 nullptr（此时对应颜色并入黑白：黄显示为白）。
    //
    // 请求原型（对齐官方固件）：
    //   GET /api/render?v=<电压>&mac=<MAC>&rssi=<dBm>&refresh_min=<分钟>
    //       &w=800&h=480&bpp=2&colors=4[&persona=<模式 ID>][&next=1]
    //   头：X-Device-Token / Accept-Encoding: identity / Connection: close
    // 后端在 colors>=3 时返回 96000 字节的 raw 2bpp；否则（设备未绑定 owner、
    // 或 colors<3）返回 800x480 1bpp BMP —— 两种情况都会正确解码。
    static bool Load2bpp(NetworkInterface* network,
                         const std::string& server,
                         const std::string& persona,
                         const std::string& device_id,
                         uint8_t* bw_plane,
                         uint8_t* yellow_plane,
                         uint8_t* red_plane,
                         std::string& error,
                         int* server_refresh_minutes = nullptr,
                         const InkSightRenderHints& hints = InkSightRenderHints());

    // 4bpp 彩色模式（六色 Spectra 6 面板用）：请求 bpp=4&colors=6，后端返回
    // application/octet-stream 的 800*480/2 = 192000 字节原始 4bpp 打包数据。
    // 像素值是「规范色索引」，与 Spectra 6 面板原生编号一致：
    //   0=黑 1=白 2=黄 3=红 4=蓝 5=绿
    // 每字节 2 像素、高半字节在左，行优先自顶向下。
    //
    // 输出 dst[kFrame4bppBytes]（PSRAM），可直接交给
    // Epd7in3fPanel::ShowNormalizedFrame 刷新（内部按面板型号重映射索引）。
    //
    // 返回 false 的常见情形（调用方应回落到 Load2bpp）：
    //   - 后端是旧版本，不认识 bpp=4 / colors=6（返回 4xx 或 json 错误）
    //   - 设备未绑定 owner，后端返回的是配对码 BMP
    static bool Load4bpp(NetworkInterface* network,
                         const std::string& server,
                         const std::string& persona,
                         const std::string& device_id,
                         uint8_t* dst,
                         std::string& error,
                         int* server_refresh_minutes = nullptr,
                         const InkSightRenderHints& hints = InkSightRenderHints());

    // 设备自解绑（重新配网时调用）：带本地 device_token 请求
    //   POST /api/device/<mac>/unbind
    // 后端删除该 MAC 下的全部绑定关系，设备回到「未绑定」状态，
    // 下次 /api/render 就会重新返回带 8 位配对码的引导图。
    //
    // 返回 true 的两种情形：
    //   - 2xx：解绑成功；
    //   - 404：后端说这台设备本就未绑定（也算达成目标）；
    //   - 本地从来没有 device_token：不可能绑定过，直接算成功。
    // 返回 true 时会顺带清除本地缓存的 device_token（下次自动重新申请）。
    // 返回 false 时 error 给出原因（网络不通 / 旧后端 405 / token 失效等）。
    static bool UnbindSelf(NetworkInterface* network,
                           const std::string& server,
                           const std::string& device_id,
                           std::string* error = nullptr);

    // 清掉本地缓存的 device_token（下次请求会重新向后端申请一个）。
    static void ClearDeviceToken();

    // 上报一次心跳：POST /api/device/<mac>/heartbeat
    //   body {"battery_voltage":3.9,"wifi_rssi":-52}   头 X-Device-Token
    // 后端「设备配置」页的在线状态就是看心跳表（默认 15 分钟窗口）。
    // 固件默认渲染间隔 30 分钟，只靠渲染刷屏会有一半时间显示「离线」，
    // 所以由 HeartbeatTask 每 5 分钟补一次。rssi < 0 时才带上该字段。
    static bool ReportHeartbeat(NetworkInterface* network,
                                const std::string& server,
                                const std::string& device_id,
                                float voltage,
                                int rssi,
                                std::string* error = nullptr,
                                bool* is_bound = nullptr);

    // 上报当前运行模式（活跃 / 间歇）：POST /api/device/<mac>/runtime
    //   body {"mode":"active"|"interval"}   头 X-Device-Token
    // 后端「设备配置」页的状态角标（device_state.runtime_mode）读的就是它，
    // 长按「+」切换电源模式后要立刻上报一次，网页才能马上跟着变。
    static bool ReportRuntimeMode(NetworkInterface* network,
                                  const std::string& server,
                                  const std::string& device_id,
                                  bool active,
                                  bool awake = true,
                                  std::string* error = nullptr);

    // NVS 读写服务器地址（命名空间 inksight / key "server_url"）。
    static std::string LoadServerUrl();
    static void SaveServerUrl(const std::string& server);

    // HTTPS 前置校时：S3 无备份 RTC，冷启动时钟停在 1970，mbedtls 会因证书
    // Not-Before 校验失败报 -76。线上后端走 https，故首个网络请求前先调用本
    // 函数（时钟已有效时立即返回）。最多阻塞 6 秒，超时也继续。
    static void EnsureClockForTls();
};
