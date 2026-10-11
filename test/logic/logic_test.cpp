// Unit test LOGIC ANALYZER (chạy trên máy tính): ./run_tests.sh
// Tín hiệu trong test được sinh bằng code (mô phỏng) với kết quả biết trước.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "HCCLogicCore.hpp"
#include "HCCLogicDecode.hpp"
#include "HCCLogicFile.hpp"
#include "HCCSpmiSynth.hpp"

using namespace HCCLogicCore;
using namespace HCCLogicDecode;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (c) {                                                         \
            ++g_pass;                                                    \
        } else {                                                         \
            ++g_fail;                                                    \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
        }                                                                \
    } while (0)

// Bộ sinh tín hiệu nhiều kênh: giữ trạng thái 8 bit, set kênh rồi hold n mẫu.
struct Gen {
    std::vector<uint8_t> s;
    uint8_t v = 0;
    void set(int ch, int b) { v = (uint8_t)(b ? (v | (1u << ch)) : (v & ~(1u << ch))); }
    void hold(int n)
    {
        for (int i = 0; i < n; ++i) {
            s.push_back(v);
        }
    }
};

// I2C: SCL = ch0, SDA = ch1, nửa chu kỳ h mẫu.
static void i2cByte(Gen &g, int h, uint8_t b, int ack)
{
    for (int k = 7; k >= -1; --k) {
        const int bit = k >= 0 ? (b >> k) & 1 : ack;
        g.set(1, bit);
        g.hold(h);
        g.set(0, 1);
        g.hold(h);
        g.set(0, 0);
    }
    g.hold(1);
}

static void testI2c()
{
    Gen g;
    const int h = 10;
    g.set(0, 1);
    g.set(1, 1);
    g.hold(50);
    g.set(1, 0);  // START
    g.hold(h);
    g.set(0, 0);
    g.hold(h);
    i2cByte(g, h, (0x50 << 1) | 0, 0);  // địa chỉ 0x50 W, ACK
    i2cByte(g, h, 0x3C, 0);             // dữ liệu ACK
    i2cByte(g, h, 0xA5, 1);             // dữ liệu NACK
    g.set(1, 0);
    g.hold(h);
    g.set(0, 1);
    g.hold(h);
    g.set(1, 1);  // STOP
    g.hold(50);
    Config c;
    c.proto = P_I2C;
    c.ch[0] = 0;
    c.ch[1] = 1;
    Result r;
    decode(c, g.s.data(), g.s.size(), 1000000, r);
    CHECK(r.ann.size() == 5);
    if (r.ann.size() == 5) {
        CHECK(r.ann[0].kind == A_START && !strcmp(r.ann[0].text, "S"));
        CHECK(r.ann[1].kind == A_ADDR && !strcmp(r.ann[1].text, "50 W A"));
        CHECK(r.ann[2].kind == A_DATA && !strcmp(r.ann[2].text, "3C A"));
        CHECK(r.ann[3].kind == A_NACK && !strcmp(r.ann[3].text, "A5 N"));
        CHECK(r.ann[4].kind == A_STOP);
    }
}

// UART 8N1 trên ch2, idle 1.
static void uartByte(Gen &g, int bit, uint8_t b, bool bad_stop = false)
{
    g.set(2, 0);
    g.hold(bit);
    for (int k = 0; k < 8; ++k) {
        g.set(2, (b >> k) & 1);
        g.hold(bit);
    }
    g.set(2, bad_stop ? 0 : 1);
    g.hold(bit);
    g.set(2, 1);
    g.hold(bit * 2);
}

static void testUart()
{
    const uint32_t hz = 10000000, baud = 115200;
    const int bit = (int)(hz / baud);  // 86 mẫu
    Gen g;
    g.set(2, 1);
    g.hold(200);
    uartByte(g, bit, 'O');
    uartByte(g, bit, 'K');
    uartByte(g, bit, 0x55);  // có xung 1 bit -> đoán baud
    uartByte(g, bit, 0x00, true);
    Config c;
    c.proto = P_UART;
    c.ch[0] = 2;
    c.baud = baud;
    Result r;
    decode(c, g.s.data(), g.s.size(), hz, r);
    CHECK(r.ann.size() == 4);
    if (r.ann.size() == 4) {
        CHECK(!strcmp(r.ann[0].text, "4F 'O'") && !strcmp(r.ann[1].text, "4B 'K'"));
        CHECK(!strcmp(r.ann[2].text, "55 'U'"));
        CHECK(r.ann[3].kind == A_ERR && r.errors == 1);
    }
    CHECK(guessBaud(g.s.data(), g.s.size(), 2, hz) == 115200);
    c.baud = 0;
    decode(c, g.s.data(), g.s.size(), hz, r);
    CHECK(r.baud_used == 115200 && r.ann.size() == 4);
}

// SPI: CLK ch3, MOSI ch4, MISO ch5, CS ch6.
static void testSpi()
{
    for (int mode = 0; mode < 4; ++mode) {
        const int cpol = mode >> 1, cpha = mode & 1;
        Gen g;
        g.set(6, 1);
        g.set(3, cpol);
        g.hold(20);
        g.set(6, 0);
        g.hold(10);
        const uint8_t mo[2] = {0xA5, 0x0F}, mi[2] = {0x3C, 0xF0};
        for (int b = 0; b < 2; ++b) {
            for (int k = 7; k >= 0; --k) {
                // CPHA = 0: dữ liệu đặt trước cạnh đầu; CPHA = 1: đặt ở cạnh đầu, lấy mẫu ở cạnh sau.
                if (!cpha) {
                    g.set(4, (mo[b] >> k) & 1);
                    g.set(5, (mi[b] >> k) & 1);
                    g.hold(5);
                    g.set(3, !cpol);
                    g.hold(5);
                    g.set(3, cpol);
                } else {
                    g.set(3, !cpol);
                    g.set(4, (mo[b] >> k) & 1);
                    g.set(5, (mi[b] >> k) & 1);
                    g.hold(5);
                    g.set(3, cpol);
                    g.hold(5);
                }
            }
        }
        g.hold(5);
        g.set(6, 1);
        g.hold(20);
        Config c;
        c.proto = P_SPI;
        c.ch[0] = 3;
        c.ch[1] = 4;
        c.ch[2] = 5;
        c.ch[3] = 6;
        c.spi_mode = (uint8_t)mode;
        Result r;
        decode(c, g.s.data(), g.s.size(), 1000000, r);
        CHECK(r.ann.size() == 4);
        if (r.ann.size() == 4) {
            CHECK(r.ann[0].kind == A_START);
            CHECK(!strcmp(r.ann[1].text, "A5/3C"));
            CHECK(!strcmp(r.ann[2].text, "0F/F0"));
            CHECK(r.ann[3].kind == A_STOP);
        }
    }
}

static void testSpmi()
{
    HCCSpmiSynth::Synth s(4);
    s.idle(100);
    s.regWrite(1, 0x05, 0xA5);
    s.idle(100);
    s.regRead(2, 0x10, 0x3C, true);  // sai parity dữ liệu
    s.idle(200);
    // 2 bit / mẫu -> kênh 6 (SCLK), 7 (SDATA)
    std::vector<uint8_t> d(s.samples());
    for (size_t i = 0; i < d.size(); ++i) {
        const int v = (s.packed()[i >> 2] >> (2 * (i & 3))) & 3;
        d[i] = (uint8_t)(((v & 1) << 6) | ((v >> 1) << 7));
    }
    Config c;
    c.proto = P_SPMI;
    c.ch[0] = 6;
    c.ch[1] = 7;
    Result r;
    decode(c, d.data(), d.size(), 80000000, r);
    CHECK(r.ann.size() == 2);
    if (r.ann.size() == 2) {
        CHECK(!strcmp(r.ann[0].text, "S1 W 5=A5") && r.ann[0].kind == A_DATA);
        CHECK(r.ann[1].kind == A_ERR);
    }
}

static void testTrigger()
{
    std::vector<uint8_t> d(1000, 0);
    for (size_t i = 500; i < 1000; ++i) {
        d[i] |= 0x04;  // ch2 lên tại 500
    }
    for (size_t i = 700; i < 1000; ++i) {
        d[i] |= 0x80;  // ch7 lên tại 700
    }
    Trigger t;
    t.type = T_RISE;
    t.ch = 2;
    CHECK(findTrigger(t, d.data(), d.size(), -1) == 500);
    CHECK(findTrigger(t, d.data() + 3, d.size() - 3, d[2]) == 497);  // lệch căn hàng
    t.type = T_FALL;
    CHECK(findTrigger(t, d.data(), d.size(), -1) == -1);
    CHECK(findTrigger(t, d.data() + 600, 400, 0x00) == -1);
    CHECK(findTrigger(t, d.data() + 500, 500, 0x00) == -1);  // trước đó 0 -> giờ 1 là cạnh lên
    t.type = T_EDGE;
    t.ch = 7;
    CHECK(findTrigger(t, d.data(), d.size(), -1) == 700);
    // Ranh giới chunk: cạnh nằm ngay ở mẫu đầu chunk.
    t.type = T_RISE;
    CHECK(findTrigger(t, d.data() + 700, 300, 0x04) == 0);
    t.type = T_PATTERN;
    t.mask = 0x84;
    t.value = 0x84;
    CHECK(findTrigger(t, d.data(), d.size(), -1) == 700);
    t.value = 0x04;
    CHECK(findTrigger(t, d.data(), d.size(), -1) == 500);
    t.type = T_NONE;
    CHECK(findTrigger(t, d.data(), d.size(), -1) == 0);
}

static void testColumnsMeasure()
{
    // ch0: xung vuông chu kỳ 100 mẫu, duty 30%; ch1 luôn 1.
    std::vector<uint8_t> d(100000);
    for (size_t i = 0; i < d.size(); ++i) {
        d[i] = (uint8_t)(((i % 100) < 30 ? 1 : 0) | 2);
    }
    Mipmap mm;
    mm.build(d.data(), d.size());
    CHECK(mm.blocks() == (d.size() + 255) / 256);
    std::vector<uint16_t> a(100), b(100);
    columns(d.data(), d.size(), &mm, 0, 1000.0, 100, a.data());
    columns(d.data(), d.size(), nullptr, 0, 1000.0, 100, b.data());
    CHECK(a == b);
    CHECK((a[5] & 0x0101) == 0x0101);         // ch0 có cả 0 và 1
    CHECK((a[5] & 0x0202) == 0x0200);         // ch1 chỉ có 1
    columns(d.data(), d.size(), &mm, 10, 1.0, 4, a.data());  // mẫu 10..13: ch0 = 1
    CHECK((a[0] & 0x0101) == 0x0100);
    columns(d.data(), d.size(), &mm, -10, 1.0, 4, a.data());
    CHECK(a[0] == 0);
    Measure m = measure(d.data(), 0, d.size(), 0, 10000000);
    CHECK(m.valid);
    CHECK(fabs(m.freq_hz - 100000.0) < 1.0);
    CHECK(fabs(m.duty - 0.30) < 0.001);
    CHECK(fabs(m.high_min_s - 30e-7) < 1e-12 && fabs(m.low_min_s - 70e-7) < 1e-12);
    Measure m1 = measure(d.data(), 0, d.size(), 1, 10000000);
    CHECK(!m1.valid && m1.level == 1);
}

static void testFiles()
{
    std::vector<uint8_t> d(5000);
    for (size_t i = 0; i < d.size(); ++i) {
        d[i] = (uint8_t)((i / 7) ^ (i / 13));
    }
    HCCLogicFile::Info in;
    in.sample_hz = 80000000;
    in.samples = (uint32_t)d.size();
    in.trig = 1234;
    in.flags = HCCLogicFile::LF_DEMO;
    snprintf(in.names[0], 8, "SCL");
    snprintf(in.names[1], 8, "SDA");
    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    std::string p = std::string(tmp) + "/hcc_logic_test.lab";
    char err[160] = "";
    CHECK(HCCLogicFile::saveLab(p.c_str(), in, d.data(), err, sizeof(err)));
    HCCLogicFile::Info out;
    std::vector<uint8_t> e(8000);
    CHECK(HCCLogicFile::loadLab(p.c_str(), out, e.data(), (uint32_t)e.size(), err, sizeof(err)));
    CHECK(out.samples == d.size() && out.trig == 1234 && out.sample_hz == 80000000);
    CHECK((out.flags & HCCLogicFile::LF_DEMO) && !strcmp(out.names[1], "SDA"));
    CHECK(memcmp(d.data(), e.data(), d.size()) == 0);
    // Bộ đệm nhỏ hơn file -> cắt, đánh dấu PARTIAL.
    CHECK(HCCLogicFile::loadLab(p.c_str(), out, e.data(), 100, err, sizeof(err)));
    CHECK(out.samples == 100 && (out.flags & HCCLogicFile::LF_PARTIAL) && out.trig == 0xFFFFFFFF);
    // VCD: đọc lại và dựng lại tín hiệu ch0 / ch1.
    std::string v = std::string(tmp) + "/hcc_logic_test.vcd";
    CHECK(HCCLogicFile::exportVcd(v.c_str(), in, d.data(), 0x03, err, sizeof(err)));
    FILE *f = fopen(v.c_str(), "r");
    char line[128];
    bool defs = false;
    int cur = 0;
    long long t = 0;
    std::vector<int> rebuilt(d.size(), -1);
    long long last_t = 0;
    while (f && fgets(line, sizeof(line), f)) {
        if (strstr(line, "$enddefinitions")) {
            defs = true;
            continue;
        }
        if (!defs) {
            continue;
        }
        if (line[0] == '#') {
            t = atoll(line + 1);
            const long long idx = t / 12500;
            for (long long i = last_t; i < idx && i < (long long)d.size(); ++i) {
                rebuilt[i] = cur;
            }
            last_t = idx;
        } else if (line[0] == '0' || line[0] == '1') {
            const int k = line[1] - '!';
            cur = line[0] == '1' ? (cur | (1 << k)) : (cur & ~(1 << k));
        }
    }
    if (f) {
        fclose(f);
    }
    bool same = true;
    for (size_t i = 0; i < d.size(); ++i) {
        if (rebuilt[i] != (d[i] & 3)) {
            same = false;
            break;
        }
    }
    CHECK(same);
    remove(p.c_str());
    remove(v.c_str());
}

static void testDemo()
{
    const uint32_t hz = 10000000;
    std::vector<uint8_t> d(200000);
    makeDemo(d.data(), d.size(), hz);
    Config c;
    Result r;
    c.proto = P_I2C;
    c.ch[0] = 0;
    c.ch[1] = 1;
    decode(c, d.data(), d.size(), hz, r);
    CHECK(r.ann.size() >= 5 && !strcmp(r.ann[1].text, "50 W A") && !strcmp(r.ann[3].text, "3C A"));
    c = Config();
    c.proto = P_UART;
    c.ch[0] = 2;
    c.baud = 0;
    decode(c, d.data(), d.size(), hz, r);
    CHECK(r.baud_used == 115200 && r.ann.size() >= 5 && !strcmp(r.ann[0].text, "48 'H'"));
    c = Config();
    c.proto = P_SPI;
    c.ch[0] = 3;
    c.ch[1] = 4;
    c.ch[2] = 5;
    c.ch[3] = 6;
    decode(c, d.data(), d.size(), hz, r);
    CHECK(r.ann.size() >= 4 && !strcmp(r.ann[1].text, "A5/3C"));
    Measure m = measure(d.data(), 0, d.size(), 7, hz);
    CHECK(m.valid && fabs(m.freq_hz - 10000.0) < 1.0);
}

int main()
{
    testDemo();
    testI2c();
    testUart();
    testSpi();
    testSpmi();
    testTrigger();
    testColumnsMeasure();
    testFiles();
    printf("LOGIC test: %d dat, %d loi\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
