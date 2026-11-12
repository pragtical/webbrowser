#include "transport.h"
#include <cstring>
#include <memory>
#ifdef _WIN32
#define WB_EXPORT extern "C" __declspec(dllexport)
#else
#define WB_EXPORT extern "C" __attribute__((visibility("default")))
#endif
static thread_local std::string last_error;
WB_EXPORT unsigned wb_abi_version() { return wb::abi; }
WB_EXPORT unsigned wb_state_size() { return sizeof(wb::State); }
WB_EXPORT const char* wb_error() { return last_error.c_str(); }
WB_EXPORT void* wb_create(const char* path) {
    auto result = std::make_unique<wb::Mapping>();
    if (!path || !result->open(path, true)) { last_error = result->error; return nullptr; }
    return result.release();
}
WB_EXPORT int wb_poll(void* handle, wb::State* state) {
    auto* map = static_cast<wb::Mapping*>(handle);
    if (!map || !state || !map->lock()) return 0;
    *state = map->header->state;
    map->unlock(); return 1;
}
WB_EXPORT const void* wb_lock_frame(void* handle, uint64_t last_frame, wb::State* state) {
    auto* map = static_cast<wb::Mapping*>(handle);
    if (!map || !state || !map->lock()) return nullptr;
    const auto& current = map->header->state;
    if (!current.frame || current.frame == last_frame || !current.width || !current.height ||
        uint64_t(current.width) * current.height * 4 > wb::capacity) {
        map->unlock(); return nullptr;
    }
    *state = current; return map->pixels();
}
WB_EXPORT const char* wb_lock_clipboard(void* handle, uint64_t last, uint32_t* size, uint64_t* revision) {
    auto* map = static_cast<wb::Mapping*>(handle);
    if (!map || !size || !revision || !map->lock()) return nullptr;
    const auto* header = map->header;
    if (header->clipboard_revision == last || header->clipboard_size > wb::clipboard_capacity) {
        map->unlock(); return nullptr;
    }
    *size = header->clipboard_size; *revision = header->clipboard_revision;
    return header->clipboard;
}
WB_EXPORT void wb_unlock(void* handle) {
    if (handle) static_cast<wb::Mapping*>(handle)->unlock();
}
WB_EXPORT const char* wb_lock_context(void* handle, uint64_t last, uint32_t* size, uint64_t* revision) {
    auto* map = static_cast<wb::Mapping*>(handle);
    if (!map || !size || !revision || !map->lock()) return nullptr;
    const auto* header = map->header;
    if (header->context_revision == last || header->context_size > wb::context_capacity) {
        map->unlock(); return nullptr;
    }
    *size = header->context_size; *revision = header->context_revision;
    return header->context;
}
WB_EXPORT void wb_destroy(void* handle) { delete static_cast<wb::Mapping*>(handle); }
