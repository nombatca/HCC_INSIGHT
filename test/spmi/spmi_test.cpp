// Unit test SPMI MONITOR (chạy trên máy tính, không cần ESP32): ./run_tests.sh
// Vector bit viết tay có kết quả tính trước; dữ liệu sóng sinh bằng HCCSpmiSynth (mô phỏng - chỉ để kiểm thử).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

#include "HCCSpmiAnalyzer.hpp"
#include "HCCSpmiBuffers.hpp"
#include "HCCSpmiCapFile.hpp"
#include "HCCSpmiDecode.hpp"
#include "HCCSpmiSynth.hpp"

using namespace HCCSpmiDecode;
using HCCSpmiSynth::Synth;

static int g_fail = 0, g_pass = 0;
#define CHECK(c)                                                              \
    do {                                                                      \
        if (c) {                                                              \
            ++g_pass;                                                         \
        } else {                                                              \
            ++g_fail;                                                         \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);      \
        }                                                                     \
    } while (0)

static std::vector<uint8_t> bits(const char *s)
{
    std::vector<uint8_t> v;
    for (; *s; ++s) {
        if (*s == '0' || *s == '1') {
            v.push_back(*s - '0');
        }
    }
    return v;
}

static Txn parseBits(const char *s)
{
    std::vector<uint8_t> b = bits(s);
    Txn t;
    memset(&t, 0, sizeof(t));
    parse(b.data(), (int)b.size(), t);
    return t;
}

// ---------- parse(): vector tính tay ----------
static void testParseVectors()
{
    // Register Write SID 1, thanh ghi 0x05, dữ liệu 0xA5.
    // Khung lệnh: 0001 | 010 00101 (0x45) -> 4 bit 1 -> P = 1. Khung dữ liệu 1010 0101 -> 4 bit 1 -> P = 1. Park 0.
    Txn t = parseBits("0001 0100 0101 1   1010 0101 1   0");
    CHECK(t.kind == K_REG_WRITE);
    CHECK(t.sid == 1 && t.cmd == 0x45 && t.addr == 0x05);
    CHECK(t.len == 1 && t.data[0] == 0xA5);
    CHECK(t.parity_err == 0 && t.short_bits == 0 && t.extra_bits == 1);
    CHECK(status(t) == ST_OK);

    // Cùng gói, parity khung dữ liệu sai (P = 0).
    t = parseBits("0001 0100 0101 1   1010 0101 0   0");
    CHECK(t.kind == K_REG_WRITE && t.parity_err == 1 && t.frame_err == 0x2);
    CHECK(status(t) == ST_PARITY);

    // Khung lệnh sai parity -> LỖI LỆNH.
    t = parseBits("0001 0100 0101 0   1010 0101 1   0");
    CHECK(t.kind == K_BAD && t.frame_err == 0x1 && status(t) == ST_BAD_CMD);

    // Gói bị cắt giữa khung dữ liệu.
    t = parseBits("0001 0100 0101 1   1010 01");
    CHECK(t.kind == K_REG_WRITE && t.short_bits == 1 && t.len == 0 && status(t) == ST_SHORT);

    // Chưa đủ 13 bit khung lệnh.
    t = parseBits("0001 0100 01");
    CHECK(t.kind == K_BAD && t.short_bits == 1);

    // Register Read SID 0xE, thanh ghi 0x1F (lệnh 0x7F): 1110 0111 1111 -> 10 bit 1 -> P = 1.
    // Bus park 1 bit, dữ liệu 0x3C (0011 1100, 4 bit 1 -> P = 1), park.
    t = parseBits("1110 0111 1111 1  0  0011 1100 1  0");
    CHECK(t.kind == K_REG_READ && t.sid == 0xE && t.addr == 0x1F && t.len == 1 && t.data[0] == 0x3C);
    CHECK(status(t) == ST_OK);

    // Extended Register Read Long SID 2, 2 byte (lệnh 0x39), địa chỉ 0x0105, dữ liệu 0x12 0x80.
    // 0010 0011 1001 -> 5 bit 1 -> P = 0. 0x01 -> P = 0. 0x05 -> 2 bit -> P = 1. park.
    // 0x12 (2 bit) -> P = 1. 0x80 (1 bit) -> P = 0. park.
    t = parseBits("0010 0011 1001 0  0000 0001 0  0000 0101 1  0  0001 0010 1  1000 0000 0  0");
    CHECK(t.kind == K_EXTL_READ && t.sid == 2 && t.addr == 0x0105 && t.len == 2);
    CHECK(t.data[0] == 0x12 && t.data[1] == 0x80 && t.parity_err == 0);

    // Register 0 Write SID 3, giá trị 0x55 (lệnh 0xD5): 0011 1101 0101 -> 7 bit 1 -> P = 0.
    t = parseBits("0011 1101 0101 0  0");
    CHECK(t.kind == K_REG0_WRITE && t.data[0] == 0x55 && status(t) == ST_OK);

    // Wakeup SID 0xF (0x13): 1111 0001 0011 -> 7 bit 1 -> P = 0.
    t = parseBits("1111 0001 0011 0");
    CHECK(t.kind == K_WAKEUP && t.sid == 0xF);

    // Bảng lệnh.
    CHECK(kindOf(0x00) == K_EXT_WRITE && kindOf(0x0F) == K_EXT_WRITE);
    CHECK(kindOf(0x2F) == K_EXT_READ && kindOf(0x37) == K_EXTL_WRITE && kindOf(0x3F) == K_EXTL_READ);
    CHECK(kindOf(0x10) == K_RESET && kindOf(0x14) == K_OTHER && kindOf(0x1F) == K_OTHER);
    CHECK(kindOf(0x40) == K_REG_WRITE && kindOf(0x7F) == K_REG_READ && kindOf(0xFF) == K_REG0_WRITE);
}

// ---------- Decoder: từ sóng ----------
static std::vector<Txn> g_out;
static void sink(const Txn &t, void *)
{
    g_out.push_back(t);
}

static void feedAll(Decoder &d, const std::vector<uint8_t> &p, size_t chunk)
{
    for (size_t i = 0; i < p.size(); i += chunk) {
        d.feed(p.data() + i, std::min(chunk, p.size() - i));
    }
}

static void buildSeq(Synth &s)
{
    const uint8_t d2[2] = {0x12, 0x80};
    const uint8_t d3[3] = {0xDE, 0xAD, 0x01};
    s.idle(100);
    s.regWrite(1, 0x05, 0xA5);
    s.idle(80);
    s.regRead(0xE, 0x1F, 0x3C);
    s.idle(80);
    s.extLongRead(2, 0x0105, d2, 2);
    s.idle(80);
    s.extWrite(4, 0x40, d3, 3);
    s.idle(80);
    s.reg0Write(3, 0x55);
    s.idle(80);
    s.regWrite(1, 0x06, 0x77, true);  // sai parity dữ liệu
    s.idle(80);
    s.command(5, 0x10);  // Reset
    s.park();
    s.idle(200);
}

static void checkSeq(const std::vector<Txn> &v)
{
    CHECK(v.size() == 7);
    if (v.size() != 7) {
        return;
    }
    CHECK(v[0].kind == K_REG_WRITE && v[0].sid == 1 && v[0].addr == 5 && v[0].data[0] == 0xA5 && status(v[0]) == ST_OK);
    CHECK(v[1].kind == K_REG_READ && v[1].sid == 0xE && v[1].data[0] == 0x3C && status(v[1]) == ST_OK);
    CHECK(v[2].kind == K_EXTL_READ && v[2].addr == 0x0105 && v[2].len == 2 && v[2].data[1] == 0x80);
    CHECK(v[3].kind == K_EXT_WRITE && v[3].addr == 0x40 && v[3].len == 3 && v[3].data[2] == 0x01);
    CHECK(v[4].kind == K_REG0_WRITE && v[4].data[0] == 0x55);
    CHECK(v[5].kind == K_REG_WRITE && status(v[5]) == ST_PARITY && v[5].frame_err == 0x2);
    CHECK(v[6].kind == K_RESET && v[6].sid == 5);
    for (size_t i = 1; i < v.size(); ++i) {
        CHECK(v[i].t_sample > v[i - 1].t_sample);
    }
}

static void testDecoderStream()
{
    Synth s(3);
    buildSeq(s);
    for (size_t chunk : {1u, 3u, 7u, 64u, 100000u}) {
        g_out.clear();
        Decoder d;
        d.reset(40, sink, nullptr);
        feedAll(d, s.packed(), chunk);
        d.flush();
        checkSeq(g_out);
    }
    // Đảo dây.
    g_out.clear();
    Decoder d;
    d.reset(40, sink, nullptr);
    d.swap = true;
    std::vector<uint8_t> sw = s.packedSwapped();
    feedAll(d, sw, 4096);
    d.flush();
    checkSeq(g_out);
    // Thời điểm SSC của gói đầu: 100 mẫu nghỉ -> SSC bắt đầu ở mẫu 100.
    // SSC: SDATA lên ở mẫu 100, xuống ở mẫu 103 (h = 3); thời điểm gói = cạnh xuống SDATA của SSC.
    CHECK(!g_out.empty() && g_out[0].t_sample == 103);
    // Ước tần số SCLK: chu kỳ 6 mẫu.
    CHECK(d.sclkFalls() > 0);
}

static void testDecoderLostAndIdle()
{
    Synth a(2), b(2);
    a.idle(40);
    a.regWrite(1, 0x02, 0x11);
    a.idle(40);
    b.idle(40);
    b.regWrite(1, 0x03, 0x22);
    b.idle(40);
    // Cắt giữa gói thứ 2 của luồng (mất dữ liệu) rồi tiếp tục.
    g_out.clear();
    Decoder d;
    d.reset(40, sink, nullptr);
    d.feed(a.packed().data(), a.packed().size());
    const uint64_t before = d.samples();
    d.lost(4000);
    CHECK(d.samples() == before + 4000);
    d.feed(b.packed().data(), b.packed().size());
    d.flush();
    CHECK(g_out.size() == 2);
    if (g_out.size() == 2) {
        CHECK(!(g_out[0].flags & F_LOST_BEFORE));
        CHECK(g_out[1].flags & F_LOST_BEFORE);
        CHECK(g_out[1].data[0] == 0x22 && g_out[1].t_sample > before + 4000);
    }
    // Gói bị cắt bởi mất dữ liệu: nửa đầu gói rồi lost() -> không sinh gói giả.
    g_out.clear();
    d.reset(40, sink, nullptr);
    const std::vector<uint8_t> &p = a.packed();
    d.feed(p.data(), p.size() / 2);
    d.lost(1000);
    d.feed(b.packed().data(), b.packed().size());
    d.flush();
    CHECK(g_out.size() == 1 && g_out[0].data[0] == 0x22);
    // idle(): khoảng nghỉ dài kết thúc gói đang dở.
    g_out.clear();
    d.reset(40, sink, nullptr);
    Synth c(2);
    c.regWrite(7, 0x01, 0x5A);
    d.feed(c.packed().data(), c.packed().size());
    d.idle(1000);
    CHECK(g_out.size() == 1 && g_out[0].sid == 7 && g_out[0].data[0] == 0x5A);
}

// ---------- PacketBuf ----------
static void testPacketBuf()
{
    std::vector<Txn> mem(4);
    HCCSpmiBuffers::PacketBuf pb;
    pb.init(mem.data(), 4);
    Txn t;
    memset(&t, 0, sizeof(t));
    for (int i = 0; i < 6; ++i) {
        t.t_sample = 10 * i;
        pb.push(t);
    }
    CHECK(pb.count() == 4 && pb.dropped() == 2 && pb.full());
    CHECK(pb.at(3).t_sample == 30);
    CHECK(pb.lowerBound(0) == 0 && pb.lowerBound(15) == 2 && pb.lowerBound(1000) == 4);
    pb.clear();
    CHECK(pb.count() == 0 && pb.dropped() == 0);
}

// ---------- RawStore ----------
static void testRawStore()
{
    std::vector<uint8_t> arena(4096);
    std::vector<HCCSpmiBuffers::Seg> segs(64);
    HCCSpmiBuffers::RawStore rs;
    rs.init(arena.data(), (uint32_t)arena.size(), segs.data(), (uint32_t)segs.size());
    Synth s(2);
    s.idle(2000);
    s.regWrite(1, 0x05, 0xA5);
    s.idle(3000);  // > GAP_BYTES*4 -> cắt thành 2 đoạn
    s.regRead(2, 0x01, 0x7E);
    s.idle(400);
    const std::vector<uint8_t> &p = s.packed();
    rs.append(0, p.data(), (uint32_t)p.size());
    rs.setEnd(s.samples());
    CHECK(rs.segCount() == 2);
    CHECK(rs.usedBytes() < p.size() / 4);
    // Mọi mẫu đọc lại đúng.
    bool same = true;
    for (uint64_t t = 0; t < s.samples(); ++t) {
        const int v = (p[t >> 2] >> (2 * (t & 3))) & 3;
        if (rs.sampleAt(t) != v) {
            same = false;
            break;
        }
    }
    CHECK(same);
    CHECK(rs.nextActivity(0) == 2000);
    // columns: 1 mẫu / cột tại vùng có SSC (SDATA lên khi SCLK thấp).
    uint8_t col[8];
    rs.columns(2000, 1.0, 8, col);
    CHECK((col[0] & HCCSpmiBuffers::COL_L1_HIGH) && (col[0] & HCCSpmiBuffers::COL_L0_LOW));
    // Đầy bộ nhớ -> truncated, cột sau đó là NODATA.
    std::vector<uint8_t> small(16);
    HCCSpmiBuffers::RawStore r2;
    r2.init(small.data(), 16, segs.data(), (uint32_t)segs.size());
    CHECK(!r2.append(0, p.data(), (uint32_t)p.size()));
    r2.setEnd(s.samples());
    CHECK(r2.truncated());
    r2.columns((double)s.samples() - 10, 1.0, 4, col);
    CHECK(col[0] == HCCSpmiBuffers::COL_NODATA);
}

// ---------- File capture ----------
static void testCapFile()
{
    Synth s(2);
    buildSeq(s);
    g_out.clear();
    Decoder d;
    d.reset(40, sink, nullptr);
    d.feed(s.packed().data(), s.packed().size());
    d.flush();
    std::vector<Txn> mem(64);
    HCCSpmiBuffers::PacketBuf pb;
    pb.init(mem.data(), 64);
    for (const Txn &t : g_out) {
        pb.push(t);
    }
    std::vector<uint8_t> arena(1 << 16);
    std::vector<HCCSpmiBuffers::Seg> segs(256);
    HCCSpmiBuffers::RawStore rs;
    rs.init(arena.data(), (uint32_t)arena.size(), segs.data(), 256);
    rs.append(0, s.packed().data(), (uint32_t)s.packed().size());
    rs.setEnd(s.samples());

    const char *tmp = getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp";
    std::string path = std::string(tmp) + "/hcc_spmi_test.spmc";
    HCCSpmiCapFile::Info info;
    info.flags = HCCSpmiCapFile::CF_DEMO;
    info.sample_hz = 80000000;
    info.total_samples = s.samples();
    snprintf(info.source, sizeof(info.source), "TEST");
    char err[160] = "";
    CHECK(HCCSpmiCapFile::save(path.c_str(), info, pb, &rs, err, sizeof(err)));

    std::vector<Txn> mem2(64);
    HCCSpmiBuffers::PacketBuf pb2;
    pb2.init(mem2.data(), 64);
    std::vector<uint8_t> arena2(1 << 16);
    std::vector<HCCSpmiBuffers::Seg> segs2(256);
    HCCSpmiBuffers::RawStore rs2;
    rs2.init(arena2.data(), (uint32_t)arena2.size(), segs2.data(), 256);
    HCCSpmiCapFile::Info in2;
    CHECK(HCCSpmiCapFile::load(path.c_str(), in2, pb2, &rs2, err, sizeof(err)));
    CHECK(in2.flags & HCCSpmiCapFile::CF_DEMO);
    CHECK(in2.flags & HCCSpmiCapFile::CF_HAS_RAW);
    CHECK(in2.sample_hz == 80000000 && strcmp(in2.source, "TEST") == 0);
    CHECK(pb2.count() == pb.count());
    bool same = pb2.count() == pb.count();
    for (uint32_t i = 0; same && i < pb.count(); ++i) {
        const Txn &x = pb.at(i), &y = pb2.at(i);
        same = x.t_sample == y.t_sample && x.sid == y.sid && x.cmd == y.cmd && x.kind == y.kind &&
               x.addr == y.addr && x.len == y.len && !memcmp(x.data, y.data, 8) && x.parity_err == y.parity_err &&
               x.frame_err == y.frame_err && x.dur == y.dur;
    }
    CHECK(same);
    // Giải mã lại từ sóng trong file cho cùng kết quả.
    g_out.clear();
    Decoder d2;
    d2.reset(40, sink, nullptr);
    struct Ctx { Decoder *d; uint64_t pos; } ctx = {&d2, 0};
    std::vector<uint8_t> tmpb(97);  // cỡ lẻ để thử ghép đoạn
    CHECK(HCCSpmiCapFile::forEachSeg(path.c_str(), tmpb.data(), (uint32_t)tmpb.size(),
                                     [](uint64_t t0, const uint8_t *dd, uint32_t n, void *u) {
                                         Ctx *c = (Ctx *)u;
                                         if (t0 > c->pos) {
                                             c->d->idle(t0 - c->pos);
                                         }
                                         c->d->feed(dd, n);
                                         c->pos = t0 + (uint64_t)n * 4;
                                         return true;
                                     },
                                     &ctx, err, sizeof(err)));
    d2.flush();
    checkSeq(g_out);
    CHECK(g_out.size() == pb.count() && g_out[2].t_sample == pb.at(2).t_sample);

    // CSV xuất / nhập.
    std::string csv = std::string(tmp) + "/hcc_spmi_test.csv";
    CHECK(HCCSpmiCapFile::exportCsv(csv.c_str(), info, pb, err, sizeof(err)));
    HCCSpmiBuffers::PacketBuf pb3;
    std::vector<Txn> mem3(64);
    pb3.init(mem3.data(), 64);
    HCCSpmiCapFile::Info in3;
    CHECK(HCCSpmiCapFile::importCsv(csv.c_str(), in3, pb3, err, sizeof(err)));
    CHECK(in3.flags & HCCSpmiCapFile::CF_DEMO);
    CHECK(pb3.count() == pb.count());
    if (pb3.count() == pb.count()) {
        CHECK(pb3.at(2).kind == K_EXTL_READ && pb3.at(2).addr == 0x0105 && pb3.at(2).data[1] == 0x80);
        CHECK(status(pb3.at(5)) == ST_PARITY);
    }
    // File hỏng.
    FILE *f = fopen(path.c_str(), "r+b");
    fwrite("XXXX", 1, 4, f);
    fclose(f);
    CHECK(!HCCSpmiCapFile::load(path.c_str(), in2, pb2, nullptr, err, sizeof(err)));
    remove(path.c_str());
    remove(csv.c_str());
}

// ---------- Analyzer ----------
static Txn mk(uint8_t sid, Kind k, uint16_t addr, uint8_t v, uint64_t t)
{
    Txn x;
    memset(&x, 0, sizeof(x));
    x.sid = sid;
    x.kind = k;
    x.addr = addr;
    x.len = 1;
    x.data[0] = v;
    x.t_sample = t;
    x.cmd = k == K_REG_WRITE ? (0x40 | (addr & 0x1F)) : k == K_REG_READ ? (0x60 | (addr & 0x1F)) : 0x00;
    return x;
}

static void testAnalyzer()
{
    using namespace HCCSpmiAnalyzer;
    std::vector<Txn> A = {
        mk(1, K_REG_WRITE, 0x10, 0x01, 1), mk(1, K_REG_WRITE, 0x11, 0x80, 2), mk(1, K_REG_READ, 0x12, 0x5A, 3),
        mk(1, K_REG_WRITE, 0x13, 0x0F, 4), mk(2, K_REG_WRITE, 0x01, 0xAA, 5),
    };
    std::vector<Txn> B = {
        mk(1, K_REG_WRITE, 0x10, 0x01, 1),
        // thiếu 0x11
        mk(1, K_REG_READ, 0x12, 0x00, 3),   // khác dữ liệu
        mk(3, K_REG_WRITE, 0x05, 0x01, 4),  // thừa
        mk(1, K_REG_WRITE, 0x13, 0x0F, 5), mk(2, K_REG_WRITE, 0x01, 0xAA, 6),
    };
    B.back().parity_err = 1;  // lỗi giao tiếp
    Result r;
    compare(A.data(), (uint32_t)A.size(), B.data(), (uint32_t)B.size(), r);
    CHECK(r.rows.size() == 6);
    if (r.rows.size() == 6) {
        CHECK(r.rows[0].type == D_SAME);
        CHECK(r.rows[1].type == D_MISSING && r.rows[1].ia == 1 && r.rows[1].ib == -1);
        CHECK(r.rows[2].type == D_DATA && r.rows[2].ia == 2 && r.rows[2].ib == 1);
        CHECK(r.rows[3].type == D_EXTRA && r.rows[3].ib == 2);
        CHECK(r.rows[4].type == D_SAME);
        CHECK(r.rows[5].type == D_ERR);
    }
    CHECK(r.same == 2 && r.missing == 1 && r.data == 1 && r.extra == 1 && r.err == 1);
    CHECK(r.first_diff == 1 && r.err_b == 1 && r.err_a == 0);

    // Hai capture giống hệt -> không có khác biệt.
    Result r2;
    compare(A.data(), (uint32_t)A.size(), A.data(), (uint32_t)A.size(), r2);
    CHECK(r2.same == A.size() && r2.first_diff == -1);

    // Gói hỏng ở cùng vị trí trong cả hai capture -> một dòng LỖI GIAO TIẾP có cả A và B.
    std::vector<Txn> A2 = {mk(1, K_REG_WRITE, 0x10, 0x01, 1), mk(1, K_REG_WRITE, 0x11, 0x02, 2)};
    std::vector<Txn> B2 = A2;
    A2[1].short_bits = 1;
    B2[1].short_bits = 1;
    Result r3;
    compare(A2.data(), 2, B2.data(), 2, r3);
    CHECK(r3.rows.size() == 2 && r3.rows[1].type == D_ERR && r3.rows[1].ia == 1 && r3.rows[1].ib == 1);

    // Thanh ghi cuối: gói lỗi parity không tính.
    std::vector<RegRow> regs = finalRegs(A.data(), (uint32_t)A.size(), B.data(), (uint32_t)B.size());
    bool saw11 = false, saw21 = false;
    for (const RegRow &x : regs) {
        if (x.sid == 1 && x.addr == 0x11) {
            saw11 = x.has_a && !x.has_b && x.last_a == 0x80;
        }
        if (x.sid == 2 && x.addr == 0x01) {
            saw21 = x.has_a && !x.has_b;
        }
    }
    CHECK(saw11 && saw21);

    // Mapping: dòng thiếu nguồn bị loại; không có mapping mặc định.
    PmicMap m;
    const int n = parseMap("# vi du\nsid,dia_chi,mask,gia_tri,nhan,nguon\n"
                           "1,0x13,0x0F,0x0F,EN_TEST,datasheet X trang 12\n"
                           "1,0x11,0x80,0x80,THIEU_NGUON,\n"
                           "abc,1,2,3,x,y\n",
                           m);
    CHECK(n == 1 && m.rejected == 2);
    uint8_t lv = 0;
    CHECK(applyMap(m.entries[0], A.data(), (uint32_t)A.size(), &lv) == M_MATCH && lv == 0x0F);
    MapEntry e2 = m.entries[0];
    e2.value = 0x00;
    CHECK(applyMap(e2, A.data(), (uint32_t)A.size(), nullptr) == M_DIFF);
    e2.addr = 0x77;
    CHECK(applyMap(e2, A.data(), (uint32_t)A.size(), nullptr) == M_NO_WRITE);
}

int main()
{
    testParseVectors();
    testDecoderStream();
    testDecoderLostAndIdle();
    testPacketBuf();
    testRawStore();
    testCapFile();
    testAnalyzer();
    printf("SPMI test: %d dat, %d loi\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
