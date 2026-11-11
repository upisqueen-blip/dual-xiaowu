#include "epaper_bus_lock.h"

#include <atomic>

SemaphoreHandle_t EpaperBusMutex() {
    static SemaphoreHandle_t mutex = xSemaphoreCreateMutex();
    return mutex;
}

namespace {
// 共享 RST 的复位序号。两块面板可能在不同任务里复位，用原子量避免撕裂。
std::atomic<unsigned> g_shared_reset_epoch{0};
}  // namespace

void NoteSharedReset() { g_shared_reset_epoch.fetch_add(1); }

unsigned SharedResetEpoch() { return g_shared_reset_epoch.load(); }
