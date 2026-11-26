/*
 * HCCRffeMonitor: tầng trình diễn / thống kê RFFE cho MONITOR thụ động.
 *
 * Phần THU dùng lại HCCSpmiMonitor (PARLIO + DMA + ring buffer + đếm mất frame + đo SCLK
 * + lưu SD) vì bus RFFE và SPMI dùng chung chân và chung khung ở mức bit; bộ đệm gói lưu
 * nguyên Txn thô (USID, lệnh, địa chỉ, dữ liệu, parity). Lớp này diễn giải mỗi Txn theo
 * NGỮ NGHĨA RFFE và đối chiếu USID với dữ liệu tham chiếu RFFE:
 *   - UsidMap: USID (0..15) -> tên IC theo tham chiếu (HCCRffe::RefEntry.ic).
 *   - Filter : lọc theo USID / IC / READ / WRITE / ERROR.
 *   - Row    : định dạng một dòng bảng (USID, IC, thời gian, lệnh, địa chỉ, DATA HEX, STATUS).
 *   - aggregate: thống kê số gói / read / write / lỗi theo từng USID (IC).
 *
 * Thuần (không ESP-IDF) -> kiểm thử trên host (tests/rffe_decode).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <vector>

#include "HCCRffe.hpp"
#include "HCCRffeDecode.hpp"

namespace HCCRffeMonitor {

using Txn = HCCRffeDecode::Txn;

// USID (0..15) -> tên IC theo dữ liệu tham chiếu RFFE.
struct UsidMap {
    char ic[16][24];
    bool known[16];
    void build(const std::vector<HCCRffe::RefEntry> &ref);
    const char *icOf(uint8_t usid) const;  // "" nếu chưa biết
    bool hasIc(uint8_t usid) const;
};

struct Filter {
    int usid = -1;            // -1 = mọi USID
    bool reads = true;        // hiện lệnh đọc
    bool writes = true;       // hiện lệnh ghi
    bool others = true;       // hiện lệnh khác (dành riêng / broadcast không read-write)
    bool errors_only = false; // chỉ hiện gói lỗi (parity / thiếu bit / lỗi lệnh)
    char ic[24] = "";         // "" = mọi IC; khác -> chỉ USID có IC này
    bool pass(const Txn &t, const UsidMap &m) const;
};

// Một dòng bảng đã định dạng sẵn (tiếng Việt).
struct Row {
    char usid[6];    // "0x5"
    char ic[24];     // tên IC hoặc "—"
    char time[24];   // "ms.us" kể từ đầu phiên
    char cmd[16];    // "REG R", "EXT R"...
    char reg[10];    // "0x0A", "0x1234" hoặc "—"
    char data[32];   // "3C" hoặc "11 22"
    char status[14]; // "OK", "SAI PARITY"...
    bool is_error;
    bool is_read;
    bool is_write;
};

void format(const Txn &t, const UsidMap &m, uint32_t sample_hz, Row &out);

// Thống kê theo USID. out[0..k) sắp theo USID tăng dần; trả số USID có gói.
struct IcCount {
    uint8_t usid;
    char ic[24];
    uint32_t total;
    uint32_t reads;
    uint32_t writes;
    uint32_t errors;
};
int aggregate(const Txn *pkts, uint32_t n, const UsidMap &m, IcCount *out, int max);

}  // namespace HCCRffeMonitor
