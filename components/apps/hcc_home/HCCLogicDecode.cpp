#include "HCCLogicDecode.hpp"

#include <algorithm>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "HCCSpmiDecode.hpp"

namespace HCCLogicDecode {

const char *protoName(Proto p)
{
    switch (p) {
    case P_NONE: return "TẮT";
    case P_I2C: return "I2C";
    case P_UART: return "UART";
    case P_SPI: return "SPI";
    case P_SPMI: return "SPMI";
    }
    return "?";
}

namespace {

void add(Result &r, uint32_t t0, uint32_t t1, AnnKind k, const char *fmt, ...) __attribute__((format(printf, 5, 6)));
void add(Result &r, uint32_t t0, uint32_t t1, AnnKind k, const char *fmt, ...)
{
    if (r.ann.size() >= MAX_ANN) {
        r.truncated = true;
        return;
    }
    Annot a;
    a.t0 = t0;
    a.t1 = t1 > t0 ? t1 : t0;
    a.kind = k;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(a.text, sizeof(a.text), fmt, ap);
    va_end(ap);
    if (k == A_ERR || k == A_NACK) {
        r.errors += k == A_ERR ? 1 : 0;
    }
    r.ann.push_back(a);
}

inline int bitOf(uint8_t v, int ch)
{
    return (v >> ch) & 1;
}

// ---------- I2C ----------
void decodeI2c(const Config &c, const uint8_t *d, size_t n, Result &r)
{
    const int scl = c.ch[0], sda = c.ch[1];
    if (scl < 0 || sda < 0 || n < 2) {
        return;
    }
    int ps = bitOf(d[0], scl), pd = bitOf(d[0], sda);
    bool in = false;     // trong giao dịch (sau START)
    int nbit = 0;
    uint32_t byte = 0, t_first = 0;
    bool first_byte = false;
    for (size_t i = 1; i < n; ++i) {
        const int s = bitOf(d[i], scl), a = bitOf(d[i], sda);
        if (s && ps && a != pd) {
            if (!a) {  // SDA xuống khi SCL cao: START
                add(r, (uint32_t)i, (uint32_t)i, A_START, in ? "Sr" : "S");
                in = true;
                first_byte = true;
            } else if (in) {  // SDA lên khi SCL cao: STOP
                add(r, (uint32_t)i, (uint32_t)i, A_STOP, "P");
                in = false;
            }
            nbit = 0;
            byte = 0;
        } else if (in && s && !ps) {  // cạnh lên SCL: lấy bit
            if (nbit == 0) {
                t_first = (uint32_t)i;
            }
            if (nbit < 8) {
                byte = (byte << 1) | (uint32_t)a;
                ++nbit;
            } else {
                // bit 9: ACK (0) / NACK (1)
                const bool ack = a == 0;
                if (first_byte) {
                    add(r, t_first, (uint32_t)i, A_ADDR, "%02X %s %s", (unsigned)(byte >> 1), (byte & 1) ? "R" : "W",
                        ack ? "A" : "N");
                    first_byte = false;
                } else {
                    add(r, t_first, (uint32_t)i, ack ? A_DATA : A_NACK, "%02X %s", (unsigned)byte, ack ? "A" : "N");
                }
                nbit = 0;
                byte = 0;
            }
        }
        ps = s;
        pd = a;
    }
}

// ---------- UART ----------
const uint32_t STD_BAUD[] = {300, 1200, 2400, 4800, 9600, 14400, 19200, 38400, 57600, 74880, 115200, 230400, 250000,
                             460800, 500000, 921600, 1000000, 1500000, 2000000, 3000000};

}  // namespace

uint32_t guessBaud(const uint8_t *d, size_t n, int ch, uint32_t hz)
{
    if (ch < 0 || n < 3 || hz == 0) {
        return 0;
    }
    int p = bitOf(d[0], ch);
    long last = -1;
    uint32_t wmin = UINT32_MAX;
    uint32_t edges = 0;
    for (size_t i = 1; i < n; ++i) {
        const int c = bitOf(d[i], ch);
        if (c == p) {
            continue;
        }
        if (last >= 0) {
            const uint32_t w = (uint32_t)(i - last);
            if (w >= 2 && w < wmin) {  // bỏ nhiễu 1 mẫu
                wmin = w;
            }
        }
        last = (long)i;
        p = c;
        ++edges;
    }
    if (edges < 4 || wmin == UINT32_MAX) {
        return 0;
    }
    const double b = (double)hz / wmin;
    uint32_t best = 0;
    double berr = 1e9;
    for (uint32_t s : STD_BAUD) {
        const double e = b > s ? b / s : s / b;
        if (e < berr) {
            berr = e;
            best = s;
        }
    }
    return berr < 1.12 ? best : (uint32_t)(b + 0.5);
}

namespace {

void decodeUart(const Config &c, const uint8_t *d, size_t n, uint32_t hz, Result &r)
{
    const int ch = c.ch[0];
    if (ch < 0 || n < 2) {
        return;
    }
    uint32_t baud = c.baud ? c.baud : guessBaud(d, n, ch, hz);
    r.baud_used = baud;
    if (baud == 0 || hz / baud < 3) {
        add(r, 0, 0, A_ERR, baud ? "baud quá cao" : "không đoán được baud");
        return;
    }
    const double bit = (double)hz / baud;
    const int inv = c.invert ? 1 : 0;
    auto lv = [&](size_t i) { return bitOf(d[i], ch) ^ inv; };
    size_t i = 1;
    // Bỏ qua đến khi đường ở mức nghỉ (1).
    while (i < n && !lv(i)) {
        ++i;
    }
    while (i < n) {
        // tìm cạnh xuống (start bit)
        while (i < n && !(lv(i - 1) == 1 && lv(i) == 0)) {
            ++i;
        }
        if (i >= n) {
            break;
        }
        const double s0 = (double)i;
        const size_t mid_start = (size_t)(s0 + bit * 0.5);
        const size_t end = (size_t)(s0 + bit * 10);
        if (end >= n) {
            break;
        }
        if (lv(mid_start) != 0) {  // nhiễu, không phải start bit
            ++i;
            continue;
        }
        uint32_t v = 0;
        for (int k = 0; k < 8; ++k) {
            const size_t m = (size_t)(s0 + bit * (1.5 + k));
            v |= (uint32_t)lv(m) << k;
        }
        const size_t stop_m = (size_t)(s0 + bit * 9.5);
        const bool ok = lv(stop_m) == 1;
        if (ok) {
            if (v >= 0x20 && v < 0x7F) {
                add(r, (uint32_t)i, (uint32_t)end, A_DATA, "%02X '%c'", (unsigned)v, (char)v);
            } else {
                add(r, (uint32_t)i, (uint32_t)end, A_DATA, "%02X", (unsigned)v);
            }
        } else {
            add(r, (uint32_t)i, (uint32_t)end, A_ERR, "%02X LỖI KHUNG", (unsigned)v);
        }
        i = stop_m;
        // chờ về mức nghỉ trước khi tìm start bit kế
        while (i < n && !lv(i)) {
            ++i;
        }
    }
}

// ---------- SPI ----------
void decodeSpi(const Config &c, const uint8_t *d, size_t n, Result &r)
{
    const int clk = c.ch[0], mosi = c.ch[1], miso = c.ch[2], cs = c.ch[3];
    if (clk < 0 || (mosi < 0 && miso < 0) || n < 2) {
        return;
    }
    const int cpol = (c.spi_mode >> 1) & 1, cpha = c.spi_mode & 1;
    // Cạnh lấy mẫu: mode 0/3 = cạnh lên, mode 1/2 = cạnh xuống.
    const int sample_rise = (cpol ^ cpha) == 0;
    int pc = bitOf(d[0], clk);
    int pcs = cs >= 0 ? bitOf(d[0], cs) : 0;
    int nbit = 0;
    uint32_t vo = 0, vi = 0, t_first = 0;
    for (size_t i = 1; i < n; ++i) {
        const int k = bitOf(d[i], clk);
        if (cs >= 0) {
            const int s = bitOf(d[i], cs);
            if (s != pcs) {
                if (!s) {
                    add(r, (uint32_t)i, (uint32_t)i, A_START, "CS");
                } else {
                    if (nbit) {
                        add(r, t_first, (uint32_t)i, A_ERR, "%d bit lẻ", nbit);
                    }
                    add(r, (uint32_t)i, (uint32_t)i, A_STOP, "/CS");
                }
                nbit = 0;
                vo = vi = 0;
                pcs = s;
            }
            if (s) {
                pc = k;
                continue;  // CS không tích cực
            }
        }
        if (k != pc && k == sample_rise) {
            if (nbit == 0) {
                t_first = (uint32_t)i;
            }
            const uint32_t bo = mosi >= 0 ? (uint32_t)bitOf(d[i], mosi) : 0;
            const uint32_t bi = miso >= 0 ? (uint32_t)bitOf(d[i], miso) : 0;
            if (c.lsb_first) {
                vo |= bo << nbit;
                vi |= bi << nbit;
            } else {
                vo = (vo << 1) | bo;
                vi = (vi << 1) | bi;
            }
            if (++nbit == 8) {
                if (mosi >= 0 && miso >= 0) {
                    add(r, t_first, (uint32_t)i, A_DATA, "%02X/%02X", (unsigned)vo, (unsigned)vi);
                } else {
                    add(r, t_first, (uint32_t)i, A_DATA, "%02X", (unsigned)(mosi >= 0 ? vo : vi));
                }
                nbit = 0;
                vo = vi = 0;
            }
        }
        pc = k;
    }
}

// ---------- SPMI ----------
struct SpmiCtx {
    Result *r;
};

void spmiSink(const HCCSpmiDecode::Txn &t, void *u)
{
    Result &r = *((SpmiCtx *)u)->r;
    const uint32_t t0 = (uint32_t)t.t_sample;
    const uint32_t t1 = t0 + t.dur;
    const HCCSpmiDecode::PktStatus st = HCCSpmiDecode::status(t);
    if (st == HCCSpmiDecode::ST_BAD_CMD) {
        add(r, t0, t1, A_ERR, "LỖI LỆNH");
        return;
    }
    char dat[12] = "";
    if (t.len) {
        snprintf(dat, sizeof(dat), "=%02X%s", t.data[0], t.len > 1 ? ".." : "");
    }
    const bool addr = HCCSpmiDecode::isRead(t.kind) || HCCSpmiDecode::isWrite(t.kind);
    if (addr && t.kind != HCCSpmiDecode::K_REG0_WRITE) {
        add(r, t0, t1, st == HCCSpmiDecode::ST_OK ? A_DATA : A_ERR, "S%X %s %X%s", t.sid,
            HCCSpmiDecode::isRead(t.kind) ? "R" : "W", t.addr, dat);
    } else {
        add(r, t0, t1, st == HCCSpmiDecode::ST_OK ? A_DATA : A_ERR, "S%X %s%s", t.sid,
            HCCSpmiDecode::kindName(t.kind), dat);
    }
}

void decodeSpmi(const Config &c, const uint8_t *d, size_t n, uint32_t hz, Result &r)
{
    const int sclk = c.ch[0], sdata = c.ch[1];
    if (sclk < 0 || sdata < 0) {
        return;
    }
    SpmiCtx ctx = {&r};
    HCCSpmiDecode::Decoder dec;
    uint32_t idle = hz / 500000;
    dec.reset(idle < 8 ? 8 : idle, spmiSink, &ctx);
    uint8_t buf[1024];
    size_t i = 0;
    while (i < n) {
        size_t k = 0;
        while (k < sizeof(buf) && i < n) {
            uint8_t v = 0;
            for (int j = 0; j < 4; ++j) {
                const uint8_t s = i < n ? d[i] : 0;
                v |= (uint8_t)((bitOf(s, sclk) | (bitOf(s, sdata) << 1)) << (2 * j));
                ++i;
            }
            buf[k++] = v;
        }
        dec.feed(buf, k);
    }
    dec.flush();
}

}  // namespace

void decode(const Config &cfg, const uint8_t *d, size_t n, uint32_t hz, Result &out)
{
    out = Result();
    switch (cfg.proto) {
    case P_I2C: decodeI2c(cfg, d, n, out); break;
    case P_UART: decodeUart(cfg, d, n, hz, out); break;
    case P_SPI: decodeSpi(cfg, d, n, out); break;
    case P_SPMI: decodeSpmi(cfg, d, n, hz, out); break;
    default: break;
    }
    std::stable_sort(out.ann.begin(), out.ann.end(), [](const Annot &a, const Annot &b) { return a.t0 < b.t0; });
}

size_t firstVisible(const std::vector<Annot> &a, uint32_t t)
{
    // t0 tăng dần; t1 >= t0. Lùi thêm vài phần tử để không bỏ chú thích dài bắt đầu trước t.
    size_t lo = (size_t)(std::lower_bound(a.begin(), a.end(), t, [](const Annot &x, uint32_t v) { return x.t0 < v; }) -
                         a.begin());
    size_t k = lo;
    for (int back = 0; k > 0 && back < 64; ++back) {
        if (a[k - 1].t1 >= t) {
            lo = k - 1;
        }
        --k;
    }
    return lo;
}

}  // namespace HCCLogicDecode
