#include "HCCLogicCore.hpp"

#include <string.h>

namespace HCCLogicCore {

const char *trigName(TrigType t)
{
    switch (t) {
    case T_NONE: return "NGAY";
    case T_RISE: return "CẠNH LÊN";
    case T_FALL: return "CẠNH XUỐNG";
    case T_EDGE: return "CẠNH BẤT KỲ";
    case T_PATTERN: return "MẪU";
    }
    return "?";
}

long findTrigger(const Trigger &t, const uint8_t *d, size_t n, int prev)
{
    if (n == 0) {
        return -1;
    }
    if (t.type == T_NONE) {
        return 0;
    }
    if (t.type == T_PATTERN) {
        const uint8_t m = t.mask, v = t.value & t.mask;
        bool was = prev >= 0 && (((uint8_t)prev & m) == v);
        for (size_t i = 0; i < n; ++i) {
            const bool now = (d[i] & m) == v;
            if (now && !was) {
                return (long)i;
            }
            was = now;
        }
        return -1;
    }
    const uint8_t bit = (uint8_t)(1u << (t.ch & 7));
    int p = prev >= 0 ? ((prev & bit) ? 1 : 0) : ((d[0] & bit) ? 1 : 0);
    size_t i = prev >= 0 ? 0 : 1;
    const uint32_t wm = 0x01010101u * bit;
    while (i < n) {
        // Bỏ nhanh 4 mẫu liền không đổi mức của kênh.
        if (((uintptr_t)(d + i) & 3) == 0 && i + 4 <= n) {
            uint32_t w;
            memcpy(&w, d + i, 4);
            const uint32_t x = w & wm;
            if ((p == 0 && x == 0) || (p == 1 && x == wm)) {
                i += 4;
                continue;
            }
        }
        const int c = (d[i] & bit) ? 1 : 0;
        if (c != p) {
            if (t.type == T_EDGE || (t.type == T_RISE && c) || (t.type == T_FALL && !c)) {
                return (long)i;
            }
            p = c;
        }
        ++i;
    }
    return -1;
}

void Mipmap::build(const uint8_t *d, size_t n)
{
    _n = n;
    const size_t nb = (n + BLOCK - 1) / BLOCK;
    _or.assign(nb, 0);
    _and.assign(nb, 0xFF);
    for (size_t b = 0; b < nb; ++b) {
        const size_t a = b * BLOCK;
        const size_t e = a + BLOCK < n ? a + BLOCK : n;
        uint8_t o = 0, an = 0xFF;
        size_t i = a;
        for (; i + 4 <= e; i += 4) {
            uint32_t w;
            memcpy(&w, d + i, 4);
            o |= (uint8_t)(w | (w >> 8) | (w >> 16) | (w >> 24));
            an &= (uint8_t)(w & (w >> 8) & (w >> 16) & (w >> 24));
        }
        for (; i < e; ++i) {
            o |= d[i];
            an &= d[i];
        }
        _or[b] = o;
        _and[b] = an;
    }
}

void columns(const uint8_t *d, size_t n, const Mipmap *mm, double t0, double spp, int ncols, uint16_t *out)
{
    for (int c = 0; c < ncols; ++c) {
        const double fa = t0 + c * spp, fb = t0 + (c + 1) * spp;
        if (fb <= 0 || fa >= (double)n) {
            out[c] = 0;
            continue;
        }
        size_t a = fa < 0 ? 0 : (size_t)fa;
        size_t b = (size_t)fb;
        if (b <= a) {
            b = a + 1;
        }
        if (b > n) {
            b = n;
        }
        uint8_t o = 0, an = 0xFF;
        size_t i = a;
        const size_t B = Mipmap::BLOCK;
        if (mm && mm->samples() == n && b - a >= 2 * B) {
            while (i < b && (i % B) != 0) {
                o |= d[i];
                an &= d[i];
                ++i;
            }
            while (i + B <= b) {
                o |= mm->orAt(i / B);
                an &= mm->andAt(i / B);
                i += B;
            }
        }
        for (; i < b; ++i) {
            o |= d[i];
            an &= d[i];
        }
        // có mức 1 = OR; có mức 0 = NOT AND
        out[c] = (uint16_t)(((uint16_t)o << 8) | (uint8_t)~an);
    }
}

Measure measure(const uint8_t *d, size_t from, size_t to, int ch, uint32_t hz)
{
    Measure m;
    if (to <= from + 1 || hz == 0) {
        if (to > from) {
            m.level = (d[from] >> ch) & 1;
        }
        return m;
    }
    const uint8_t bit = (uint8_t)(1u << (ch & 7));
    int p = (d[from] & bit) ? 1 : 0;
    long first_rise = -1, last_rise = -1, last_edge = -1;
    uint64_t high_in = 0;  // số mẫu mức 1 giữa cạnh lên đầu và cạnh lên cuối
    uint32_t hmin = UINT32_MAX, lmin = UINT32_MAX;
    for (size_t i = from + 1; i < to; ++i) {
        const int c = (d[i] & bit) ? 1 : 0;
        if (first_rise >= 0 && p) {
            ++high_in;
        }
        if (c == p) {
            continue;
        }
        if (last_edge >= 0) {
            const uint32_t w = (uint32_t)(i - last_edge);
            if (p) {
                hmin = w < hmin ? w : hmin;
            } else {
                lmin = w < lmin ? w : lmin;
            }
        }
        last_edge = (long)i;
        if (c) {
            ++m.rises;
            if (first_rise < 0) {
                first_rise = (long)i;
                high_in = 0;
            }
            last_rise = (long)i;
            m.duty = (double)high_in;  // tạm: số mẫu mức 1 tới cạnh lên này
        } else {
            ++m.falls;
        }
        p = c;
    }
    if (m.rises == 0 && m.falls == 0) {
        m.level = p;
        return m;
    }
    m.high_min_s = hmin == UINT32_MAX ? 0 : (double)hmin / hz;
    m.low_min_s = lmin == UINT32_MAX ? 0 : (double)lmin / hz;
    if (m.rises >= 2 && last_rise > first_rise) {
        const double span = (double)(last_rise - first_rise);
        m.valid = true;
        m.period_s = span / (m.rises - 1) / hz;
        m.freq_hz = 1.0 / m.period_s;
        m.duty = m.duty / span;
    } else {
        m.duty = 0;
    }
    return m;
}

namespace {

struct DemoGen {
    uint8_t *d;
    size_t n;
    size_t pos = 0;
    uint8_t v = 0;
    void set(int ch, int b) { v = (uint8_t)(b ? (v | (1u << ch)) : (v & ~(1u << ch))); }
    void hold(size_t k)
    {
        for (size_t i = 0; i < k && pos < n; ++i) {
            d[pos++] = v;
        }
    }
};

}  // namespace

uint32_t makeDemo(uint8_t *d, size_t n, uint32_t hz)
{
    memset(d, 0, n);
    if (n == 0 || hz == 0) {
        return 0;
    }
    // Ba khối độc lập nối tiếp nhau rồi lặp lại: I2C (100 kHz), UART (115200), SPI (1 MHz). D7 chồng lên suốt.
    DemoGen g{d, n};
    g.set(0, 1);
    g.set(1, 1);
    g.set(2, 1);
    g.set(6, 1);
    const size_t h_i2c = hz / 200000 ? hz / 200000 : 1;  // nửa chu kỳ SCL 100 kHz
    const size_t b_uart = hz / 115200 ? hz / 115200 : 1;
    const size_t h_spi = hz / 2000000 ? hz / 2000000 : 1;
    uint32_t trig = 0;
    int round = 0;
    while (g.pos < n) {
        g.hold(hz / 20000 + 1);  // nghỉ 50 µs
        if (round == 0) {
            trig = (uint32_t)g.pos;
        }
        // I2C: START, 0x50 W, 0x00, 0x3C (+round), STOP
        g.set(1, 0);
        g.hold(h_i2c);
        g.set(0, 0);
        g.hold(h_i2c);
        const uint8_t bytes[3] = {0xA0, 0x00, (uint8_t)(0x3C + round)};
        for (uint8_t b : bytes) {
            for (int k = 7; k >= -1; --k) {
                g.set(1, k >= 0 ? (b >> k) & 1 : 0);
                g.hold(h_i2c);
                g.set(0, 1);
                g.hold(h_i2c);
                g.set(0, 0);
            }
        }
        g.set(1, 0);
        g.hold(h_i2c);
        g.set(0, 1);
        g.hold(h_i2c);
        g.set(1, 1);
        g.hold(hz / 50000 + 1);
        // UART: "HCC\r\n"
        static const char msg[] = "HCC\r\n";
        for (const char *p = msg; *p; ++p) {
            g.set(2, 0);
            g.hold(b_uart);
            for (int k = 0; k < 8; ++k) {
                g.set(2, (*p >> k) & 1);
                g.hold(b_uart);
            }
            g.set(2, 1);
            g.hold(b_uart * 2);
        }
        g.hold(hz / 50000 + 1);
        // SPI mode 0: CS thấp, 2 byte MOSI A5 0F / MISO 3C F0
        g.set(6, 0);
        g.hold(h_spi * 2);
        const uint8_t mo[2] = {0xA5, (uint8_t)round}, mi[2] = {0x3C, 0xF0};
        for (int b = 0; b < 2; ++b) {
            for (int k = 7; k >= 0; --k) {
                g.set(4, (mo[b] >> k) & 1);
                g.set(5, (mi[b] >> k) & 1);
                g.hold(h_spi);
                g.set(3, 1);
                g.hold(h_spi);
                g.set(3, 0);
            }
        }
        g.hold(h_spi * 2);
        g.set(6, 1);
        ++round;
    }
    // D7: xung vuông 10 kHz chồng lên toàn bộ.
    const size_t half = hz / 20000 ? hz / 20000 : 1;
    for (size_t i = 0; i < n; ++i) {
        if ((i / half) & 1) {
            d[i] |= 0x80;
        } else {
            d[i] &= 0x7F;
        }
    }
    return trig;
}

}  // namespace HCCLogicCore

