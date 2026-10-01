// operator new cho toàn app: dùng PSRAM.
//
// sdkconfig hiện tại tắt CONFIG_SPIRAM_USE_MALLOC (PSRAM chỉ cấp qua heap_caps), nên new /
// std::string chỉ lấy RAM nội. Panic log (vài chục - vài trăm KB), Knowledge Base và kết nối TLS
// của AI cùng lúc làm cạn RAM nội -> new thất bại -> abort (exception bị tắt) -> màn hình xanh.
//
//  - Khối >= kPsramThreshold: PSRAM trước, hết thì RAM nội.
//  - Khối nhỏ: RAM nội như cũ (nhanh, an toàn cho code chạy khi cache tắt), hết thì PSRAM.
// delete giữ mặc định: free() của ESP-IDF giải phóng được cả hai vùng.
#include <cstdio>
#include <cstdlib>
#include <new>

#include "esp_heap_caps.h"

namespace {

constexpr size_t kPsramThreshold = 4096;

void *hccAlloc(size_t n)
{
    if (n == 0) n = 1;
    void *p = nullptr;
    if (n >= kPsramThreshold) {
        p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!p) p = heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    } else {
        p = malloc(n);
        if (!p) p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    return p;
}

[[noreturn]] void outOfMemory(size_t n)
{
    printf("[HCC MEM] operator new(%u) thất bại: RAM nội trống %u B, PSRAM trống %u B\n", (unsigned)n,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    abort();
}

}  // namespace

void *operator new(std::size_t n)
{
    void *p = hccAlloc(n);
    if (!p) outOfMemory(n);
    return p;
}

void *operator new[](std::size_t n)
{
    void *p = hccAlloc(n);
    if (!p) outOfMemory(n);
    return p;
}

void *operator new(std::size_t n, const std::nothrow_t &) noexcept
{
    return hccAlloc(n);
}

void *operator new[](std::size_t n, const std::nothrow_t &) noexcept
{
    return hccAlloc(n);
}

// Gọi từ HCCHome để chắc chắn file này được link (và log để xác nhận trên monitor).
void hccMemoryReport(const char *where)
{
    printf("[HCC MEM] %s: RAM nội trống %u B (khối lớn nhất %u B), PSRAM trống %u B\n", where,
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
           (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
