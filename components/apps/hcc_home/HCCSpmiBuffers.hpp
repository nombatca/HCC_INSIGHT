/*
 * HCCSpmiBuffers: bộ đệm gói (PacketBuf) và bộ lưu sóng thô (RawStore) cho SPMI MONITOR.
 * Không phụ thuộc ESP-IDF (bộ nhớ do bên gọi cấp - PSRAM trên máy, malloc khi test).
 *
 * Một luồng ghi (task thu), nhiều luồng đọc (giao diện): phần tử [0, count) đã công bố thì không đổi nữa,
 * đọc không cần khoá. clear() chỉ gọi khi không có luồng ghi.
 *
 * PacketBuf: đầy thì gói mới bị bỏ và đếm vào dropped() (không ghi đè gói cũ - trình tự từ đầu quan trọng
 *            cho so sánh main tốt / main lỗi).
 * RawStore : chỉ lưu các đoạn có hoạt động (bỏ khoảng bus nghỉ toàn 0 dài >= GAP_BYTES byte); đầy thì
 *            dừng lưu và đánh dấu truncated() (gói vẫn được giải mã tiếp).
 */
#pragma once

#include <atomic>
#include <stddef.h>
#include <stdint.h>

#include "HCCSpmiDecode.hpp"

namespace HCCSpmiBuffers {

using HCCSpmiDecode::Txn;

class PacketBuf {
public:
    void init(Txn *mem, uint32_t capacity)
    {
        _p = mem;
        _cap = mem ? capacity : 0;
        clear();
    }
    void clear()
    {
        _count.store(0, std::memory_order_release);
        _dropped.store(0, std::memory_order_relaxed);
    }
    bool push(const Txn &t)
    {
        const uint32_t n = _count.load(std::memory_order_relaxed);
        if (n >= _cap) {
            _dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        _p[n] = t;
        _count.store(n + 1, std::memory_order_release);
        return true;
    }
    uint32_t count() const { return _count.load(std::memory_order_acquire); }
    uint32_t dropped() const { return _dropped.load(std::memory_order_relaxed); }
    uint32_t capacity() const { return _cap; }
    bool full() const { return count() >= _cap; }
    const Txn &at(uint32_t i) const { return _p[i]; }
    const Txn *data() const { return _p; }
    // Chỉ số gói đầu tiên có t_sample >= t (gói được thêm theo thứ tự thời gian).
    uint32_t lowerBound(uint64_t t) const;

private:
    Txn *_p = nullptr;
    uint32_t _cap = 0;
    std::atomic<uint32_t> _count{0};
    std::atomic<uint32_t> _dropped{0};
};

struct Seg {
    uint64_t t0;      // mẫu đầu tiên của đoạn (bội của 4)
    uint32_t off;     // vị trí trong arena
    uint32_t nbytes;  // số byte (4 mẫu / byte) - chỉ tăng
};

// Kết quả tóm tắt một cột điểm ảnh của dạng sóng.
enum : uint8_t {
    COL_L0_LOW = 1,
    COL_L0_HIGH = 2,
    COL_L1_LOW = 4,
    COL_L1_HIGH = 8,
    COL_NODATA = 16,  // sau điểm dừng lưu sóng / ngoài vùng đã thu
};

class RawStore {
public:
    static constexpr uint32_t GAP_BYTES = 64;  // >= 256 mẫu toàn 0 thì cắt đoạn
    static constexpr uint32_t MARGIN = 4;      // giữ 16 mẫu 0 quanh mỗi đoạn

    void init(uint8_t *arena, uint32_t arena_bytes, Seg *segs, uint32_t max_segs)
    {
        _a = arena;
        _acap = arena ? arena_bytes : 0;
        _s = segs;
        _scap = segs ? max_segs : 0;
        clear();
    }
    void clear()
    {
        _nseg.store(0, std::memory_order_release);
        _used = 0;
        _trunc.store(false, std::memory_order_relaxed);
        _end.store(0, std::memory_order_release);
        _stored_end.store(0, std::memory_order_release);
    }
    bool enabled() const { return _a != nullptr && _s != nullptr; }
    // d: n byte bắt đầu tại mẫu t (bội của 4). Trả false nếu vừa hết chỗ (truncated).
    bool append(uint64_t t, const uint8_t *d, uint32_t n);
    // Thêm nguyên một đoạn (không cắt khoảng nghỉ) - dùng khi nạp file.
    bool appendSeg(uint64_t t, const uint8_t *d, uint32_t n);
    // Thời điểm cuối của luồng mẫu đã đi qua (kể cả phần nghỉ / phần không lưu).
    void setEnd(uint64_t t) { _end.store(t, std::memory_order_release); }
    uint64_t end() const { return _end.load(std::memory_order_acquire); }
    // Mẫu cuối mà sóng còn được lưu (sau đó là COL_NODATA nếu truncated()).
    uint64_t storedEnd() const { return _stored_end.load(std::memory_order_acquire); }
    bool truncated() const { return _trunc.load(std::memory_order_relaxed); }
    uint32_t segCount() const { return _nseg.load(std::memory_order_acquire); }
    Seg seg(uint32_t i) const
    {
        Seg s = _s[i];
        s.nbytes = __atomic_load_n(&_s[i].nbytes, __ATOMIC_ACQUIRE);
        return s;
    }
    const uint8_t *bytes(const Seg &s) const { return _a + s.off; }
    uint32_t usedBytes() const { return _used; }
    uint32_t capacityBytes() const { return _acap; }

    // Mẫu tại t (bit0 = dây 0, bit1 = dây 1). Không có trong đoạn nào -> 0 (bus nghỉ).
    int sampleAt(uint64_t t) const;
    // Tóm tắt ncols cột, cột c phủ mẫu [t0 + c*spp, t0 + (c+1)*spp). out[c] = tổ hợp COL_*.
    void columns(double t0, double spp, int ncols, uint8_t *out) const;
    // Mẫu có hoạt động đầu tiên >= t (UINT64_MAX nếu không có).
    uint64_t nextActivity(uint64_t t) const;

private:
    bool add(uint64_t t, const uint8_t *d, uint32_t n);
    uint32_t findSeg(uint64_t t) const;  // đoạn đầu tiên có điểm cuối > t

    uint8_t *_a = nullptr;
    uint32_t _acap = 0;
    uint32_t _used = 0;
    Seg *_s = nullptr;
    uint32_t _scap = 0;
    std::atomic<uint32_t> _nseg{0};
    std::atomic<bool> _trunc{false};
    std::atomic<uint64_t> _end{0};
    std::atomic<uint64_t> _stored_end{0};
};

}  // namespace HCCSpmiBuffers
