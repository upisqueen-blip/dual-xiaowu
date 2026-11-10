#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class AudioCodec;

class SdCardStorage {
public:
    SdCardStorage() = default;
    ~SdCardStorage();

    bool Mount();
    // Retry mounting when the card was inserted after boot; no-op if mounted.
    bool EnsureMounted();
    bool mounted() const { return mounted_; }
    // Human-readable reason of the last Mount() failure (e.g. exFAT card).
    const std::string& MountError() const { return mount_error_; }
    std::string Status() const;
    // Capacity in MB. Returns false when the card is not mounted or the
    // filesystem info is unavailable.
    bool Capacity(uint32_t& total_mb, uint32_t& free_mb) const;
    std::string List(const std::string& category) const;
    bool Load(const std::string& category, const std::string& name,
              std::vector<uint8_t>& data, size_t limit, std::string& error) const;
    bool RecordWav(AudioCodec* codec, const std::string& name, int seconds, std::string& error);
    bool PlayWav(AudioCodec* codec, const std::string& name, std::string& error) const;

private:
    bool mounted_ = false;
    void* card_ = nullptr;
    std::string mount_error_;

    static bool SafeName(const std::string& name);
    static std::string Directory(const std::string& category);
    static std::string Path(const std::string& category, const std::string& name);
};
