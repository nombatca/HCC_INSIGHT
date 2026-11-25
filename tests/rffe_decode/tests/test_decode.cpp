// Kiểm thử bộ giải mã MIPI RFFE trên host: tổng hợp sóng 2 dây SCLK/SDATA cho các
// loại giao dịch RFFE rồi nạp qua HCCRffeDecode::Decoder và so với kết quả mong đợi.
// Không phần cứng, không giả lập kết quả - dựng đúng dạng sóng theo giao thức.
#include "HCCRffeDecode.hpp"

#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace rf = HCCRffeDecode;

namespace {

constexpr int OS = 3;  // số mẫu mỗi nửa chu kỳ (oversampling)

// Dựng luồng mẫu 2 bit: bit0 = SCLK, bit1 = SDATA, 4 mẫu/byte, mẫu đầu ở 2 bit thấp.
struct Wave {
    std::vector<uint8_t> packed;
    uint8_t acc = 0;
    int cur = 0;
    void push(int clk, int dat)
    {
        acc |= (uint8_t)(((clk & 1) | ((dat & 1) << 1)) << (2 * cur));
        if (++cur == 4) {
            packed.push_back(acc);
            acc = 0;
            cur = 0;
        }
    }
    void flush()
    {
        if (cur) {
            packed.push_back(acc);
            acc = 0;
            cur = 0;
        }
    }
};

void idle(Wave &w, int n)
{
    for (int i = 0; i < n; ++i) w.push(0, 0);
}

// SSC: SDATA lên rồi xuống trong khi SCLK giữ mức 0.
void ssc(Wave &w)
{
    for (int i = 0; i < OS; ++i) w.push(0, 0);
    for (int i = 0; i < OS; ++i) w.push(0, 1);
    for (int i = 0; i < OS; ++i) w.push(0, 0);
}

// Một bit: SCLK 0 rồi 1, SDATA = v suốt chu kỳ (bên nhận lấy ở cạnh xuống kế tiếp).
void bit(Wave &w, int v)
{
    for (int i = 0; i < OS; ++i) w.push(0, v);
    for (int i = 0; i < OS; ++i) w.push(1, v);
}

int ones(uint32_t v, int n)
{
    int c = 0;
    for (int i = 0; i < n; ++i) c += (v >> i) & 1;
    return c;
}

// Khung lệnh 13 bit: USID[3:0] + Command[7:0] + Parity (lẻ). force_parity<0 = đúng parity.
void cmdFrame(Wave &w, uint8_t usid, uint8_t cmd, int force_parity = -1)
{
    uint32_t v = ((uint32_t)(usid & 0x0F) << 8) | cmd;  // 12 bit
    for (int i = 11; i >= 0; --i) bit(w, (v >> i) & 1);
    int p = force_parity >= 0 ? force_parity : ((ones(v, 12) & 1) ? 0 : 1);
    bit(w, p);
}

// Khung 8 bit + parity lẻ (địa chỉ / dữ liệu).
void frame8(Wave &w, uint8_t val, int force_parity = -1)
{
    for (int i = 7; i >= 0; --i) bit(w, (val >> i) & 1);
    int p = force_parity >= 0 ? force_parity : ((ones(val, 8) & 1) ? 0 : 1);
    bit(w, p);
}

// Bus park: một chu kỳ clock, SDATA thả (0). Decoder bỏ qua 1 bit này trước dữ liệu đọc.
void park(Wave &w)
{
    bit(w, 0);
}

// Thu các Txn giải mã được.
std::vector<rf::Txn> g_txns;
void sink(const rf::Txn &t, void *) { g_txns.push_back(t); }

void run(Wave &w)
{
    w.flush();
    g_txns.clear();
    rf::Decoder dec;
    dec.reset(16, sink, nullptr);
    dec.feed(w.packed.data(), w.packed.size());
    dec.flush();
}

}  // namespace

int main()
{
    // kindOf: bảng lệnh MIPI RFFE.
    assert(rf::kindOf(0x80) == rf::K_REG0_WRITE && rf::kindOf(0xFF) == rf::K_REG0_WRITE);
    assert(rf::kindOf(0x40) == rf::K_REG_WRITE && rf::kindOf(0x5F) == rf::K_REG_WRITE);
    assert(rf::kindOf(0x60) == rf::K_REG_READ && rf::kindOf(0x7F) == rf::K_REG_READ);
    assert(rf::kindOf(0x00) == rf::K_EXT_WRITE && rf::kindOf(0x0F) == rf::K_EXT_WRITE);
    assert(rf::kindOf(0x20) == rf::K_EXT_READ && rf::kindOf(0x2F) == rf::K_EXT_READ);
    assert(rf::kindOf(0x30) == rf::K_EXTL_WRITE && rf::kindOf(0x37) == rf::K_EXTL_WRITE);
    assert(rf::kindOf(0x38) == rf::K_EXTL_READ && rf::kindOf(0x3F) == rf::K_EXTL_READ);
    assert(rf::kindOf(0x10) == rf::K_RESERVED && rf::kindOf(0x1F) == rf::K_RESERVED);
    assert(rf::isRead(rf::K_REG_READ) && rf::isWrite(rf::K_REG0_WRITE));
    assert(rf::isBroadcast(0) && !rf::isBroadcast(5));

    // 1) Register Read: USID=5, addr=0x0A (cmd 0x6A), data=0x3C. Có bus park trước dữ liệu.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,5, 0x6A);
        park(w);
        frame8(w, 0x3C);
        idle(w, 40);
        run(w);
        assert(g_txns.size() == 1);
        const rf::Txn &t = g_txns[0];
        assert(t.sid == 5 && t.cmd == 0x6A);
        assert(rf::kindOf(t.cmd) == rf::K_REG_READ);
        assert(t.addr == 0x0A && t.len == 1 && t.data[0] == 0x3C);
        assert(t.parity_err == 0 && t.short_bits == 0);
        assert(rf::status(t) == rf::ST_OK);
    }

    // 2) Register Write: USID=0xA, addr=0x05 (cmd 0x45), data=0x80. Không bus park.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,0xA, 0x45);
        frame8(w, 0x80);
        idle(w, 40);
        run(w);
        assert(g_txns.size() == 1);
        const rf::Txn &t = g_txns[0];
        assert(t.sid == 0xA && t.cmd == 0x45);
        assert(rf::kindOf(t.cmd) == rf::K_REG_WRITE && rf::isWrite(rf::kindOf(t.cmd)));
        assert(t.addr == 0x05 && t.len == 1 && t.data[0] == 0x80);
        assert(rf::status(t) == rf::ST_OK);
    }

    // 3) Register 0 Write: USID=3, data 7 bit = 0x55 (cmd 0xD5). Dữ liệu nằm trong lệnh.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,3, 0xD5);
        idle(w, 40);
        run(w);
        assert(g_txns.size() == 1);
        const rf::Txn &t = g_txns[0];
        assert(t.sid == 3 && rf::kindOf(t.cmd) == rf::K_REG0_WRITE);
        assert(t.len == 1 && t.data[0] == 0x55);
        assert(rf::status(t) == rf::ST_OK);
    }

    // 4) Extended Register Read: USID=7, 2 byte (cmd 0x21), addr=0x40, data={0x11,0x22}.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,7, 0x21);
        frame8(w, 0x40);  // địa chỉ 8 bit
        park(w);
        frame8(w, 0x11);
        frame8(w, 0x22);
        idle(w, 40);
        run(w);
        assert(g_txns.size() == 1);
        const rf::Txn &t = g_txns[0];
        assert(t.sid == 7 && rf::kindOf(t.cmd) == rf::K_EXT_READ);
        assert(t.addr == 0x40 && t.len == 2 && t.data[0] == 0x11 && t.data[1] == 0x22);
        assert(rf::status(t) == rf::ST_OK);
    }

    // 5) Extended Register Read Long: USID=1, 1 byte (cmd 0x38), addr 16 bit = 0x1234, data=0xAB.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,1, 0x38);
        frame8(w, 0x12);  // addr[15:8]
        frame8(w, 0x34);  // addr[7:0]
        park(w);
        frame8(w, 0xAB);
        idle(w, 40);
        run(w);
        assert(g_txns.size() == 1);
        const rf::Txn &t = g_txns[0];
        assert(t.sid == 1 && rf::kindOf(t.cmd) == rf::K_EXTL_READ);
        assert(t.addr == 0x1234 && t.len == 1 && t.data[0] == 0xAB);
        assert(rf::status(t) == rf::ST_OK);
    }

    // 6a) Sai parity khung lệnh -> LỖI LỆNH.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,5, 0x6A, /*force_parity=*/0);  // ép sai parity lẻ
        park(w);
        frame8(w, 0x3C);
        idle(w, 40);
        run(w);
        assert(g_txns.size() == 1);
        assert(rf::status(g_txns[0]) == rf::ST_BAD_CMD);
    }

    // 6b) Sai parity khung dữ liệu -> SAI PARITY.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,5, 0x6A);
        park(w);
        frame8(w, 0x3C, /*force_parity=*/0);  // 0x3C: 4 bit 1 -> parity lẻ đúng = 1; ép 0 => sai
        idle(w, 40);
        run(w);
        assert(g_txns.size() == 1);
        const rf::Txn &t = g_txns[0];
        assert(t.parity_err >= 1 && rf::status(t) == rf::ST_PARITY);
    }

    // 7) Thiếu bit: Extended Register Read 2 byte nhưng chỉ có 1 byte dữ liệu -> THIẾU BIT.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,7, 0x21);  // EXT read 2 byte
        frame8(w, 0x40);
        park(w);
        frame8(w, 0x11);  // chỉ 1 byte rồi dừng -> thiếu byte thứ 2
        idle(w, 40);
        run(w);
        assert(g_txns.size() == 1);
        assert(rf::status(g_txns[0]) == rf::ST_SHORT);
    }

    // 8) Broadcast (USID=0): Register Write tới địa chỉ 0x1A, data=0x01.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,0, 0x40 | 0x1A);
        frame8(w, 0x01);
        idle(w, 40);
        run(w);
        assert(g_txns.size() == 1);
        assert(rf::isBroadcast(g_txns[0].sid));
    }

    // 9) Nhiều giao dịch liên tiếp + đảo dây (swap): vẫn tách đúng 2 gói.
    {
        Wave w;
        idle(w, 20);
        ssc(w);
        cmdFrame(w,2, 0x6C);  // REG R addr 0x0C
        park(w);
        frame8(w, 0xA5);
        idle(w, 40);
        ssc(w);
        cmdFrame(w,4, 0x45);  // REG W addr 0x05
        frame8(w, 0x33);
        idle(w, 40);
        w.flush();
        g_txns.clear();
        rf::Decoder dec;
        dec.reset(16, sink, nullptr);
        dec.feed(w.packed.data(), w.packed.size());
        dec.flush();
        assert(g_txns.size() == 2);
        assert(g_txns[0].sid == 2 && g_txns[0].data[0] == 0xA5);
        assert(g_txns[1].sid == 4 && g_txns[1].data[0] == 0x33);
    }

    printf("RFFE decode: command table, reg/ext read-write, bus park, parity, short, broadcast, multi PASS\n");
    return 0;
}
