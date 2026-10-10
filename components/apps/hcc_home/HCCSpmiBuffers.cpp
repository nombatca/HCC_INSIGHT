#include "HCCSpmiBuffers.hpp"

#include <string.h>

namespace HCCSpmiBuffers {

uint32_t PacketBuf::lowerBound(uint64_t t) const
{
    uint32_t lo = 0, hi = count();
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        if (_p[mid].t_sample < t) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

bool RawStore::add(uint64_t t, const uint8_t *d, uint32_t n)
{
    if (n == 0) {
        return true;
    }
    if (_trunc.load(std::memory_order_relaxed)) {
        return false;
    }
    uint32_t ns = _nseg.load(std::memory_order_relaxed);
    if (_used + n > _acap) {
        _trunc.store(true, std::memory_order_relaxed);
        return false;
    }
    memcpy(_a + _used, d, n);
    if (ns > 0) {
        Seg &last = _s[ns - 1];
        if (last.t0 + (uint64_t)last.nbytes * 4 == t && last.off + last.nbytes == _used) {
            _used += n;
            __atomic_store_n(&last.nbytes, last.nbytes + n, __ATOMIC_RELEASE);
            _stored_end.store(t + (uint64_t)n * 4, std::memory_order_release);
            return true;
        }
    }
    if (ns >= _scap) {
        _trunc.store(true, std::memory_order_relaxed);
        return false;
    }
    _s[ns].t0 = t;
    _s[ns].off = _used;
    _s[ns].nbytes = n;
    _used += n;
    _nseg.store(ns + 1, std::memory_order_release);
    _stored_end.store(t + (uint64_t)n * 4, std::memory_order_release);
    return true;
}

bool RawStore::appendSeg(uint64_t t, const uint8_t *d, uint32_t n)
{
    if (!enabled()) {
        return false;
    }
    return add(t & ~3ull, d, n);
}

bool RawStore::append(uint64_t t, const uint8_t *d, uint32_t n)
{
    if (!enabled() || _trunc.load(std::memory_order_relaxed)) {
        return false;
    }
    t &= ~3ull;
    uint32_t i = 0;
    while (i < n) {
        // Bỏ nhanh các byte 0.
        while (i < n && d[i] == 0) {
            if ((((uintptr_t)(d + i)) & 3) == 0 && i + 4 <= n && *(const uint32_t *)(d + i) == 0) {
                i += 4;
            } else {
                ++i;
            }
        }
        if (i >= n) {
            break;
        }
        const uint32_t first = i;
        uint32_t last = i;
        uint32_t j = i + 1;
        while (j < n) {
            if (d[j] != 0) {
                last = j;
            } else if (j - last > GAP_BYTES) {
                break;
            }
            ++j;
        }
        const uint32_t a = first > MARGIN ? first - MARGIN : 0;
        const uint32_t b = (last + 1 + MARGIN < n) ? last + 1 + MARGIN : n;
        if (!add(t + (uint64_t)a * 4, d + a, b - a)) {
            return false;
        }
        i = b;
    }
    return true;
}

uint32_t RawStore::findSeg(uint64_t t) const
{
    uint32_t lo = 0, hi = segCount();
    while (lo < hi) {
        const uint32_t mid = lo + (hi - lo) / 2;
        const Seg s = seg(mid);
        if (s.t0 + (uint64_t)s.nbytes * 4 <= t) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo;
}

int RawStore::sampleAt(uint64_t t) const
{
    const uint32_t i = findSeg(t);
    if (i >= segCount()) {
        return 0;
    }
    const Seg s = seg(i);
    if (t < s.t0) {
        return 0;
    }
    const uint64_t k = t - s.t0;
    return (_a[s.off + (k >> 2)] >> (2 * (k & 3))) & 3;
}

uint64_t RawStore::nextActivity(uint64_t t) const
{
    for (uint32_t i = findSeg(t); i < segCount(); ++i) {
        const Seg s = seg(i);
        uint64_t k = t > s.t0 ? t - s.t0 : 0;
        const uint64_t n = (uint64_t)s.nbytes * 4;
        for (; k < n; ++k) {
            if ((_a[s.off + (k >> 2)] >> (2 * (k & 3))) & 3) {
                return s.t0 + k;
            }
        }
    }
    return UINT64_MAX;
}

void RawStore::columns(double t0, double spp, int ncols, uint8_t *out) const
{
    if (ncols <= 0) {
        return;
    }
    const bool trunc = truncated();
    const uint64_t send = storedEnd();
    const uint64_t eend = end();
    const uint32_t nseg = segCount();
    uint32_t si = findSeg(t0 < 0 ? 0 : (uint64_t)t0);
    for (int c = 0; c < ncols; ++c) {
        const double fa = t0 + c * spp;
        const double fb = t0 + (c + 1) * spp;
        if (fb <= 0) {
            out[c] = COL_NODATA;
            continue;
        }
        uint64_t a = fa < 0 ? 0 : (uint64_t)fa;
        uint64_t b = (uint64_t)fb;
        if (b <= a) {
            b = a + 1;
        }
        if (a >= eend || (trunc && a >= send)) {
            out[c] = COL_NODATA;
            continue;
        }
        uint8_t m = 0;
        uint64_t p = a;
        while (p < b) {
            while (si < nseg) {
                const Seg s = seg(si);
                if (s.t0 + (uint64_t)s.nbytes * 4 > p) {
                    break;
                }
                ++si;
            }
            if (si >= nseg) {
                m |= COL_L0_LOW | COL_L1_LOW;  // sau đoạn cuối: bus nghỉ
                break;
            }
            const Seg s = seg(si);
            if (p < s.t0) {
                m |= COL_L0_LOW | COL_L1_LOW;  // khoảng nghỉ không lưu
                p = s.t0;
                continue;
            }
            const uint64_t se = s.t0 + (uint64_t)s.nbytes * 4;
            const uint64_t q = b < se ? b : se;
            const uint8_t *base = _a + s.off;
            uint64_t k = p - s.t0;
            const uint64_t ke = q - s.t0;
            while (k < ke) {
                if ((k & 3) == 0 && k + 4 <= ke) {
                    const uint8_t v = base[k >> 2];
                    if (v == 0) {
                        m |= COL_L0_LOW | COL_L1_LOW;
                    } else {
                        m |= (v & 0x55) ? COL_L0_HIGH : 0;
                        m |= ((v & 0x55) != 0x55) ? COL_L0_LOW : 0;
                        m |= (v & 0xAA) ? COL_L1_HIGH : 0;
                        m |= ((v & 0xAA) != 0xAA) ? COL_L1_LOW : 0;
                    }
                    k += 4;
                    continue;
                }
                const int v = (base[k >> 2] >> (2 * (k & 3))) & 3;
                m |= (v & 1) ? COL_L0_HIGH : COL_L0_LOW;
                m |= (v & 2) ? COL_L1_HIGH : COL_L1_LOW;
                ++k;
            }
            p = q;
        }
        out[c] = m;
    }
}

}  // namespace HCCSpmiBuffers
