#pragma once
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#endif
#include <cstdint>
#include <string>

namespace wb {
constexpr uint32_t magic = 0x57425231;
constexpr uint32_t abi = 5;
constexpr uint32_t clipboard_capacity = 1024 * 1024;
constexpr uint32_t context_capacity = 1024 * 1024;
constexpr uint32_t capacity = 64 * 1024 * 1024;
struct State {
    uint64_t revision, frame, context_revision;
    uint32_t width, height, status, loading, can_back, can_forward;
    int32_t caret_x, caret_y, caret_width, caret_height;
    char url[4096], title[512], error[1024], cursor[16];
};
struct Header {
    uint32_t magic, abi, capacity, reserved;
    State state;
    uint64_t clipboard_revision;
    uint32_t clipboard_size, clipboard_reserved;
    char clipboard[clipboard_capacity];
    uint64_t context_revision;
    uint32_t context_size, context_reserved;
    char context[context_capacity];
};

class Mapping {
public:
    Mapping() = default;
    ~Mapping();
    Mapping(const Mapping&) = delete;
    Mapping& operator=(const Mapping&) = delete;
    bool open(const std::string& path, bool create);
    bool lock();
    void unlock();
    Header* header = nullptr;
    unsigned char* pixels() { return reinterpret_cast<unsigned char*>(header + 1); }
    std::string error;
private:
    intptr_t file = -1;
    void* mapping = nullptr;
    bool locked = false;
};
void text(char* target, size_t size, const std::string& value);
}
