// Kiểm thử tầng trình diễn/thống kê RFFE MONITOR trên host: UsidMap, Filter, format, aggregate.
#include "HCCRffeMonitor.hpp"

#include <cassert>
#include <cstring>
#include <cstdio>
#include <vector>

namespace rm = HCCRffeMonitor;
namespace rf = HCCRffeDecode;

static rm::Txn mk(uint8_t sid, uint8_t cmd, uint16_t addr, const uint8_t *data, uint8_t len,
                  uint8_t parity_err = 0, uint8_t short_bits = 0, uint64_t t = 0)
{
    rm::Txn x;
    memset(&x, 0, sizeof(x));
    x.sid = sid;
    x.cmd = cmd;
    x.addr = addr;
    x.len = len;
    for (uint8_t i = 0; i < len && i < 8; ++i) x.data[i] = data[i];
    x.parity_err = parity_err;
    x.short_bits = short_bits;
    x.nbits = 22;  // >= 13: khung lệnh hợp lệ
    x.frame_err = parity_err ? 0x2 : 0;  // lỗi ở khung dữ liệu (bit0 = khung lệnh)
    x.t_sample = t;
    return x;
}

int main()
{
    std::vector<HCCRffe::RefEntry> ref;
    HCCRffe::RefEntry e{};
    e.usid = 5;
    snprintf(e.ic, sizeof(e.ic), "PMU_A");
    ref.push_back(e);
    HCCRffe::RefEntry e2{};
    e2.usid = 7;
    snprintf(e2.ic, sizeof(e2.ic), "RF_SW");
    ref.push_back(e2);

    rm::UsidMap map;
    map.build(ref);
    assert(strcmp(map.icOf(5), "PMU_A") == 0);
    assert(strcmp(map.icOf(7), "RF_SW") == 0);
    assert(map.icOf(3)[0] == '\0' && !map.hasIc(3) && map.hasIc(5));

    // format: Register Read USID 5, reg 0x0A, data 0x3C @ sample 0, 80 MHz.
    {
        uint8_t d[] = {0x3C};
        rm::Txn t = mk(5, 0x6A, 0x0A, d, 1, 0, 0, 80 /*samples*/);
        rm::Row r;
        rm::format(t, map, 80000000, r);
        assert(strcmp(r.usid, "0x5") == 0);
        assert(strcmp(r.ic, "PMU_A") == 0);
        assert(strcmp(r.cmd, "REG R") == 0);
        assert(strcmp(r.reg, "0x0A") == 0);
        assert(strcmp(r.data, "3C") == 0);
        assert(strcmp(r.status, "OK") == 0);
        assert(r.is_read && !r.is_write && !r.is_error);
        // 80 mẫu / 80 MHz = 1 us = 0.001 ms.
        assert(strcmp(r.time, "0.001") == 0);
    }

    // format: Extended Register Read Long USID 7 (RF_SW), addr 0x1234, data {0xAB,0xCD}.
    {
        uint8_t d[] = {0xAB, 0xCD};
        rm::Txn t = mk(7, 0x39, 0x1234, d, 2);  // 0x38|1 = 2 byte
        rm::Row r;
        rm::format(t, map, 80000000, r);
        assert(strcmp(r.ic, "RF_SW") == 0);
        assert(strcmp(r.cmd, "EXTL R") == 0);
        assert(strcmp(r.reg, "0x1234") == 0);
        assert(strcmp(r.data, "AB CD") == 0);
        assert(r.is_read);
    }

    // format: broadcast (USID 0) Register Write.
    {
        uint8_t d[] = {0x01};
        rm::Txn t = mk(0, 0x40 | 0x1A, 0x1A, d, 1);
        rm::Row r;
        rm::format(t, map, 80000000, r);
        assert(strcmp(r.ic, "BROADCAST") == 0);
        assert(r.is_write);
    }

    // format: USID không có IC -> "—"; parity lỗi -> is_error, status SAI PARITY.
    {
        uint8_t d[] = {0xFF};
        rm::Txn t = mk(9, 0x6B, 0x0B, d, 1, /*parity_err=*/1);
        rm::Row r;
        rm::format(t, map, 80000000, r);
        assert(strcmp(r.ic, "—") == 0);
        assert(r.is_error && strcmp(r.status, "SAI PARITY") == 0);
    }

    // Filter.
    {
        uint8_t d[] = {0x3C};
        rm::Txn rd = mk(5, 0x6A, 0x0A, d, 1);          // read, USID 5
        rm::Txn wr = mk(7, 0x45, 0x05, d, 1);          // write, USID 7
        rm::Txn err = mk(5, 0x6A, 0x0A, d, 1, 1);      // read lỗi parity, USID 5

        rm::Filter f;  // mặc định: hiện tất cả
        assert(f.pass(rd, map) && f.pass(wr, map) && f.pass(err, map));

        f = rm::Filter{};
        f.usid = 5;
        assert(f.pass(rd, map) && !f.pass(wr, map));

        f = rm::Filter{};
        f.reads = false;  // chỉ ẩn read
        assert(!f.pass(rd, map) && f.pass(wr, map));

        f = rm::Filter{};
        f.writes = false;
        assert(f.pass(rd, map) && !f.pass(wr, map));

        f = rm::Filter{};
        f.errors_only = true;
        assert(!f.pass(rd, map) && f.pass(err, map));

        f = rm::Filter{};
        snprintf(f.ic, sizeof(f.ic), "RF_SW");
        assert(!f.pass(rd, map) && f.pass(wr, map));  // RF_SW = USID 7
    }

    // aggregate: đếm theo USID/IC.
    {
        uint8_t d[] = {0x00};
        std::vector<rm::Txn> v;
        v.push_back(mk(5, 0x6A, 0x0A, d, 1));        // read U5
        v.push_back(mk(5, 0x45, 0x05, d, 1));        // write U5
        v.push_back(mk(5, 0x6A, 0x0A, d, 1, 1));     // read lỗi U5
        v.push_back(mk(7, 0x6C, 0x0C, d, 1));        // read U7
        rm::IcCount out[16];
        int k = rm::aggregate(v.data(), (uint32_t)v.size(), map, out, 16);
        assert(k == 2);
        assert(out[0].usid == 5 && out[0].total == 3 && out[0].reads == 2 && out[0].writes == 1 && out[0].errors == 1);
        assert(strcmp(out[0].ic, "PMU_A") == 0);
        assert(out[1].usid == 7 && out[1].total == 1 && out[1].reads == 1);
        assert(strcmp(out[1].ic, "RF_SW") == 0);
    }

    printf("RFFE monitor: UsidMap, format, filter, aggregate PASS\n");
    return 0;
}
