#include "HCCRffeMonitor.hpp"

#include <stdio.h>
#include <string.h>

namespace HCCRffeMonitor {

namespace rf = HCCRffeDecode;

void UsidMap::build(const std::vector<HCCRffe::RefEntry> &ref)
{
    for (int i = 0; i < 16; ++i) {
        ic[i][0] = '\0';
        known[i] = false;
    }
    for (const auto &e : ref) {
        if (e.usid >= 16) {
            continue;
        }
        // Lấy tên IC đầu tiên không rỗng cho mỗi USID (các dòng cùng USID thường cùng IC).
        if (!known[e.usid] && e.ic[0] != '\0') {
            snprintf(ic[e.usid], sizeof(ic[e.usid]), "%s", e.ic);
            known[e.usid] = true;
        }
    }
}

const char *UsidMap::icOf(uint8_t usid) const
{
    if (usid >= 16 || !known[usid]) {
        return "";
    }
    return ic[usid];
}

bool UsidMap::hasIc(uint8_t usid) const
{
    return usid < 16 && known[usid];
}

bool Filter::pass(const Txn &t, const UsidMap &m) const
{
    if (usid >= 0 && t.sid != (uint8_t)usid) {
        return false;
    }
    if (ic[0] != '\0') {
        const char *name = m.icOf(t.sid);
        if (name[0] == '\0' || strcmp(name, ic) != 0) {
            return false;
        }
    }
    const rf::Kind k = rf::kindOf(t.cmd);
    const bool is_err = rf::status(t) != rf::ST_OK && rf::status(t) != rf::ST_RESERVED;
    if (errors_only && !is_err) {
        return false;
    }
    const bool rd = rf::isRead(k);
    const bool wr = rf::isWrite(k);
    if (rd) {
        return reads;
    }
    if (wr) {
        return writes;
    }
    return others;
}

void format(const Txn &t, const UsidMap &m, uint32_t sample_hz, Row &out)
{
    const rf::Kind k = rf::kindOf(t.cmd);

    snprintf(out.usid, sizeof(out.usid), "0x%X", t.sid & 0x0F);

    const char *ic = m.icOf(t.sid);
    if (ic[0] != '\0') {
        snprintf(out.ic, sizeof(out.ic), "%s", ic);
    } else if (rf::isBroadcast(t.sid)) {
        snprintf(out.ic, sizeof(out.ic), "BROADCAST");
    } else {
        snprintf(out.ic, sizeof(out.ic), "—");
    }

    // Thời gian kể từ đầu phiên: ms.us (microsecond trong ms).
    if (sample_hz > 0) {
        uint64_t us_total = (uint64_t)((double)t.t_sample * 1000000.0 / (double)sample_hz);
        unsigned long long ms = us_total / 1000ULL;
        unsigned us = (unsigned)(us_total % 1000ULL);
        snprintf(out.time, sizeof(out.time), "%llu.%03u", ms, us);
    } else {
        snprintf(out.time, sizeof(out.time), "—");
    }

    snprintf(out.cmd, sizeof(out.cmd), "%s", rf::kindName(k));

    // Địa chỉ thanh ghi.
    if (k == rf::K_REG0_WRITE) {
        snprintf(out.reg, sizeof(out.reg), "0x00");
    } else if (k == rf::K_REG_WRITE || k == rf::K_REG_READ) {
        snprintf(out.reg, sizeof(out.reg), "0x%02X", (unsigned)(t.addr & 0x1F));
    } else if (k == rf::K_EXT_WRITE || k == rf::K_EXT_READ) {
        snprintf(out.reg, sizeof(out.reg), "0x%02X", (unsigned)(t.addr & 0xFF));
    } else if (k == rf::K_EXTL_WRITE || k == rf::K_EXTL_READ) {
        snprintf(out.reg, sizeof(out.reg), "0x%04X", (unsigned)t.addr);
    } else {
        snprintf(out.reg, sizeof(out.reg), "—");
    }

    // DATA HEX (tối đa 8 byte, ngăn cách bằng khoảng trắng).
    out.data[0] = '\0';
    size_t pos = 0;
    int nd = t.len > 8 ? 8 : t.len;
    for (int i = 0; i < nd; ++i) {
        int w = snprintf(out.data + pos, sizeof(out.data) - pos, "%s%02X", i ? " " : "", t.data[i]);
        if (w <= 0 || (size_t)w >= sizeof(out.data) - pos) {
            break;
        }
        pos += (size_t)w;
    }
    if (out.data[0] == '\0') {
        snprintf(out.data, sizeof(out.data), "—");
    }

    const rf::PktStatus st = rf::status(t);
    snprintf(out.status, sizeof(out.status), "%s", rf::statusName(st));
    out.is_error = st != rf::ST_OK && st != rf::ST_RESERVED;
    out.is_read = rf::isRead(k);
    out.is_write = rf::isWrite(k);
}

int aggregate(const Txn *pkts, uint32_t n, const UsidMap &m, IcCount *out, int max)
{
    if (max <= 0) {
        return 0;
    }
    IcCount acc[16];
    for (int i = 0; i < 16; ++i) {
        acc[i].usid = (uint8_t)i;
        acc[i].ic[0] = '\0';
        acc[i].total = acc[i].reads = acc[i].writes = acc[i].errors = 0;
    }
    for (uint32_t i = 0; i < n; ++i) {
        const Txn &t = pkts[i];
        const uint8_t u = t.sid & 0x0F;
        const rf::Kind k = rf::kindOf(t.cmd);
        ++acc[u].total;
        if (rf::isRead(k)) {
            ++acc[u].reads;
        } else if (rf::isWrite(k)) {
            ++acc[u].writes;
        }
        const rf::PktStatus st = rf::status(t);
        if (st != rf::ST_OK && st != rf::ST_RESERVED) {
            ++acc[u].errors;
        }
    }
    int k = 0;
    for (int i = 0; i < 16 && k < max; ++i) {
        if (acc[i].total == 0) {
            continue;
        }
        out[k] = acc[i];
        const char *ic = m.icOf((uint8_t)i);
        snprintf(out[k].ic, sizeof(out[k].ic), "%s", ic[0] ? ic : (rf::isBroadcast((uint8_t)i) ? "BROADCAST" : ""));
        ++k;
    }
    return k;
}

}  // namespace HCCRffeMonitor
