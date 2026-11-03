#ifndef _SYSTEM_INFO_H_
#define _SYSTEM_INFO_H_

#include <string>

#include <esp_err.h>
#include <freertos/FreeRTOS.h>

class SystemInfo {
public:
    static size_t GetFlashSize();
    static size_t GetMinimumFreeHeapSize();
    static size_t GetFreeHeapSize();
    static std::string GetMacAddress();
    static std::string GetChipModelName();
    static std::string GetUserAgent();
    static esp_err_t PrintTaskCpuUsage(TickType_t xTicksToWait);
    static void PrintTaskList();
    static void PrintHeapStats();
    static void PrintPmLocks();

    // ChipID = 从 eFuse MAC 地址构造的唯一 48 位整数
    static uint64_t GetChipId();
};

#endif // _SYSTEM_INFO_H_
