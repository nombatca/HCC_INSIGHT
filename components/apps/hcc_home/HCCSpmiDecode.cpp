#include "HCCSpmiDecode.hpp"

#include <string.h>

namespace HCCSpmiDecode {

const char *kindName(Kind k)
{
    switch (k) {
    case K_REG0_WRITE: return "REG0 W";
    case K_REG_WRITE: return "REG W";
    case K_REG_READ: return "REG R";
    case K_EXT_WRITE: return "EXT W";
    case K_EXT_READ: return "EXT R";
    case K_EXTL_WRITE: return "EXTL W";
    case K_EXTL_READ: return "EXTL R";
    case K_RESET: return "RESET";
    case K_SLEEP: return "SLEEP";
    case K_SHUTDOWN: return "SHUTDOWN";
    case K_WAKEUP: return "WAKEUP";
    case K_OTHER: return "KHÁC";
    case K_BAD: return "LỖI KHUNG";
    }
    return "?";
}

bool isRead(Kind k)
{
    return k == K_REG_READ || k == K_EXT_READ || k == K_EXTL_READ;
}

bool isWrite(Kind k)
{
    return k == K_REG0_WRITE || k == K_REG_WRITE || k == K_EXT_WRITE || k == K_EXTL_WRITE;
}

namespace {

// Đọc khung 8 bit + parity tại pos. Trả false nếu thiếu bit; *perr++ khi sai parity lẻ.
bool frame8(const uint8_t *b, int n, int &pos, uint8_t *v, uint8_t *perr)
{
    if (pos + 9 > n) {
        return false;
    }
    int x = 0, ones = 0;
    for (int i = 0; i < 8; ++i) {
        x = (x << 1) | b[pos + i];
        ones += b[pos + i];
    }
    ones += b[pos + 8];
    pos += 9;
    *v = (uint8_t)x;
    if (!(ones & 1)) {
        ++*perr;
    }
    return true;
}

}  // namespace

bool parse(const uint8_t *b, int n, Txn &t)
{
    t.nbits = (uint16_t)n;
    t.len = 0;
    t.addr = 0;
    t.parity_err = 0;
    t.short_bits = 0;
    t.extra_bits = 0;
    t.extra = 0;
    memset(t.data, 0, sizeof(t.data));
    if (n < 13) {
        t.kind = K_BAD;
        t.sid = 0;
        t.cmd = 0;
        t.short_bits = 1;
        return false;
    }
    int x = 0, ones = 0;
    for (int i = 0; i < 12; ++i) {
        x = (x << 1) | b[i];
        ones += b[i];
    }
    ones += b[12];
    t.sid = (uint8_t)(x >> 8);
    t.cmd = (uint8_t)(x & 0xFF);
    int pos = 13;
    if (!(ones & 1)) {
        t.kind = K_BAD;
        t.parity_err = 1;
        return true;
    }
    const uint8_t c = t.cmd;
    int ndata = 0;
    int naddr = 0;
    bool read = false;
    if (c & 0x80) {
        t.kind = K_REG0_WRITE;
        t.data[0] = c & 0x7F;
        t.len = 1;
    } else if ((c & 0xE0) == 0x40) {
        t.kind = K_REG_WRITE;
        t.addr = c & 0x1F;
        ndata = 1;
    } else if ((c & 0xE0) == 0x60) {
        t.kind = K_REG_READ;
        t.addr = c & 0x1F;
        ndata = 1;
        read = true;
    } else if ((c & 0xF0) == 0x00) {
        t.kind = K_EXT_WRITE;
        naddr = 1;
        ndata = (c & 0x0F) + 1;
    } else if ((c & 0xF0) == 0x20) {
        t.kind = K_EXT_READ;
        naddr = 1;
        ndata = (c & 0x0F) + 1;
        read = true;
    } else if ((c & 0xF8) == 0x30) {
        t.kind = K_EXTL_WRITE;
        naddr = 2;
        ndata = (c & 0x07) + 1;
    } else if ((c & 0xF8) == 0x38) {
        t.kind = K_EXTL_READ;
        naddr = 2;
        ndata = (c & 0x07) + 1;
        read = true;
    } else if (c == 0x10) {
        t.kind = K_RESET;
    } else if (c == 0x11) {
        t.kind = K_SLEEP;
    } else if (c == 0x12) {
        t.kind = K_SHUTDOWN;
    } else if (c == 0x13) {
        t.kind = K_WAKEUP;
    } else {
        t.kind = K_OTHER;
    }
    if (t.kind != K_OTHER) {
        for (int i = 0; i < naddr; ++i) {
            uint8_t a = 0;
            if (!frame8(b, n, pos, &a, &t.parity_err)) {
                t.short_bits = 1;
                return true;
            }
            t.addr = (uint16_t)((t.addr << 8) | a);
        }
        if (read) {
            if (pos + 1 > n) {
                t.short_bits = 1;
                return true;
            }
            pos += 1;  // bus park trước khi slave trả dữ liệu
        }
        for (int i = 0; i < ndata && i < 8; ++i) {
            if (!frame8(b, n, pos, &t.data[i], &t.parity_err)) {
                t.short_bits = 1;
                return true;
            }
            t.len = (uint8_t)(i + 1);
        }
    }
    if (pos < n) {
        t.extra_bits = (uint16_t)(n - pos);
        int e = 0;
        for (int i = 0; i < 8 && pos + i < n; ++i) {
            e = (e << 1) | b[pos + i];
        }
        t.extra = (uint8_t)e;
    }
    return true;
}

// ==================== Bộ giải mã theo mẫu ====================

void Decoder::reset(uint32_t idle_samples, TxnSink sink, void *user)
{
    _sink = sink;
    _user = user;
    _idle = idle_samples < 8 ? 8 : idle_samples;
    _t = 0;
    _pc = _pd = 0;
    _arm = false;
    _in = false;
    _n = 0;
    _falls = 0;
    _clk_samples = 0;
}

void Decoder::resync()
{
    _in = false;
    _arm = false;
    _n = 0;
}

void Decoder::finish()
{
    if (_in && _n > 0 && _sink) {
        Txn t;
        t.t_sample = _start;
        parse(_bits, _n, t);
        _sink(t, _user);
        if (_last_fall > _first_fall) {
            _clk_samples += _last_fall - _first_fall;
        }
    }
    _in = false;
    _n = 0;
}

inline void Decoder::sample(int c, int d)
{
    if (c == 0 && _pc == 0 && d != _pd) {
        if (d) {
            _arm = true;
        } else if (_arm) {  // SSC: SDATA lên rồi xuống khi SCLK thấp
            finish();
            _in = true;
            _start = _t;
            _n = 0;
            _arm = false;
        }
    } else if (_pc == 1 && c == 0) {  // cạnh xuống SCLK: lấy bit
        _arm = false;
        if (_in) {
            if (_n == 0) {
                _first_fall = _t;
            }
            if (_n < (int)sizeof(_bits)) {
                _bits[_n++] = (uint8_t)_pd;
            }
            _last_fall = _t;
            ++_falls;
        }
    } else if (_pc == 0 && c == 1) {
        _arm = false;
    }
    if (_in && _n > 0 && _t - _last_fall > _idle) {
        finish();
    }
    _pc = c;
    _pd = d;
    ++_t;
}

void Decoder::feed(const uint8_t *data, size_t len)
{
    size_t i = 0;
    while (i < len) {
        // Bus nghỉ (SCLK = 0, SDATA = 0) và không có gì đang dở: bỏ nhanh từng 4 byte (16 mẫu).
        if (!_arm && _pc == 0 && _pd == 0 && (!_in || _t - _last_fall > _idle) && ((uintptr_t)(data + i) & 3) == 0 &&
            i + 4 <= len && *(const uint32_t *)(data + i) == 0) {
            if (_in) {
                finish();
            }
            _t += 16;
            i += 4;
            continue;
        }
        uint8_t v = data[i++];
        for (int k = 0; k < 4; ++k) {
            const int a = v & 1, b = (v >> 1) & 1;
            v >>= 2;
            if (swap) {
                sample(b, a);
            } else {
                sample(a, b);
            }
        }
    }
}

void Decoder::flush()
{
    finish();
}

}  // namespace HCCSpmiDecode
