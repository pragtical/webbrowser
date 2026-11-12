#include "transport.h"
#include <algorithm>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace wb {
void text(char* target, size_t size, const std::string& value) {
    size_t count = std::min(size - 1, value.size());
    if (count < value.size())
        while (count && (static_cast<unsigned char>(value[count]) & 0xc0) == 0x80) count--;
    std::memcpy(target, value.data(), count);
    target[count] = 0;
}
bool Mapping::open(const std::string& path, bool create) {
    const size_t size = sizeof(Header) + capacity;
#ifdef _WIN32
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path.c_str(), -1, nullptr, 0);
    if (!length) { error = "Invalid UTF-8 transport path"; return false; }
    std::wstring wide(length, 0);
    MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, wide.data(), length);
    HANDLE handle = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        create ? CREATE_NEW : OPEN_EXISTING, FILE_ATTRIBUTE_TEMPORARY, nullptr);
    if (handle == INVALID_HANDLE_VALUE) { error = "Cannot open browser transport file"; return false; }
    file = reinterpret_cast<intptr_t>(handle);
    if (create) {
        LARGE_INTEGER length; length.QuadPart = size;
        if (!SetFilePointerEx(handle, length, nullptr, FILE_BEGIN) || !SetEndOfFile(handle)) {
            error = "Cannot size browser transport file"; return false;
        }
    }
    LARGE_INTEGER actual;
    if (!GetFileSizeEx(handle, &actual) || actual.QuadPart != static_cast<LONGLONG>(size)) {
        error = "Incorrect browser transport size"; return false;
    }
    mapping = CreateFileMappingW(handle, nullptr, PAGE_READWRITE, 0, 0, nullptr);
    if (mapping) header = static_cast<Header*>(MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0, size));
#else
    file = ::open(path.c_str(), O_RDWR | (create ? O_CREAT | O_EXCL : 0), 0600);
    if (file < 0) { error = std::strerror(errno); return false; }
    if (create && ftruncate(file, size) != 0) { error = std::strerror(errno); return false; }
    off_t actual = lseek(file, 0, SEEK_END);
    if (actual != static_cast<off_t>(size)) { error = "Incorrect browser transport size"; return false; }
    void* address = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
    if (address != MAP_FAILED) header = static_cast<Header*>(address);
#endif
    if (!header) { error = "Cannot map browser transport"; return false; }
    if (create) {
        std::memset(header, 0, sizeof(*header));
        header->magic = magic; header->abi = abi; header->capacity = capacity;
        text(header->state.cursor, sizeof(header->state.cursor), "arrow");
    } else if (header->magic != magic || header->abi != abi || header->capacity != capacity) {
        error = "Browser transport ABI mismatch"; return false;
    }
    return true;
}
bool Mapping::lock() {
    if (locked || !header) return false;
#ifdef _WIN32
    OVERLAPPED overlap{};
    if (!LockFileEx(reinterpret_cast<HANDLE>(file), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                    0, 1, 0, &overlap)) return false;
#else
    if (flock(file, LOCK_EX | LOCK_NB) != 0) return false;
#endif
    locked = true; return true;
}
void Mapping::unlock() {
    if (!locked) return;
#ifdef _WIN32
    OVERLAPPED overlap{};
    UnlockFileEx(reinterpret_cast<HANDLE>(file), 0, 1, 0, &overlap);
#else
    flock(file, LOCK_UN);
#endif
    locked = false;
}
Mapping::~Mapping() {
    unlock();
#ifdef _WIN32
    if (header) UnmapViewOfFile(header);
    if (mapping) CloseHandle(mapping);
    if (file != -1) CloseHandle(reinterpret_cast<HANDLE>(file));
#else
    if (header) munmap(header, sizeof(Header) + capacity);
    if (file >= 0) ::close(file);
#endif
}
}
