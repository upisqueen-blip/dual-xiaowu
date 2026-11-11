#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// FPC1(SSD1619) 与 FPC2(SSD2683ZA) 共享 SPI 总线与 DC/RST 控制线。
// 双屏取图/刷屏已移出 Application 主循环（否则 20~45s 的面板刷新会
// 阻塞语音唤醒等事件处理），因此两块面板的物理刷新可能在不同任务
// 并发发生——所有触碰总线的刷新入口都必须经过这把全局锁。
SemaphoreHandle_t EpaperBusMutex();

inline void EpaperBusLock() { xSemaphoreTake(EpaperBusMutex(), portMAX_DELAY); }
inline void EpaperBusUnlock() { xSemaphoreGive(EpaperBusMutex()); }

// RAII：函数内任意 return 都保证解锁。
struct EpaperBusGuard {
    EpaperBusGuard() { EpaperBusLock(); }
    ~EpaperBusGuard() { EpaperBusUnlock(); }
};

// 共享 RST（GPIO11）同时接两块面板：任何一方拉复位，另一方也会被复位。
// 被复位的一方会丢掉 0x22/0x2X 写进去的升压与 LUT 状态，之后再发 0x20
// 只会空转——BUSY 照翻、日志照打 "refresh complete"，玻璃却一个墨点都不动。
// 各面板在 HardwareReset() 之后调用 NoteSharedReset() 递增计数；完成初始化时
// 记下当时的序号，每次刷新前比对：不一致即说明被对方复位过，必须重新初始化。
void NoteSharedReset();
unsigned SharedResetEpoch();
