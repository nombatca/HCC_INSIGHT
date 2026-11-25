/*
 * HCCRffeDecode: ngữ nghĩa MIPI RFFE cho bộ giải mã mẫu 2 dây (SCLK / SDATA).
 *
 * RFFE và SPMI dùng CHUNG cơ chế khung ở mức bit: SSC (Sequence Start Condition),
 * khung lệnh USID[3:0] + Command[7:0] + Parity (13 bit), các khung địa chỉ / dữ liệu
 * 8 bit + parity lẻ, 1 chu kỳ Bus Park trước dữ liệu do slave trả. Vì vậy lớp này
 * TÁI DÙNG engine lấy mẫu + cấu trúc khung của HCCSpmiDecode (đã kiểm thử) và chỉ bổ
 * sung phần riêng của RFFE:
 *   - Bảng lệnh MIPI RFFE (Register / Register 0 / Extended / Extended Long Read-Write).
 *   - Thuật ngữ USID (Unique Slave ID 4-bit) và nhận diện địa chỉ broadcast / GSID.
 *   - Tên lệnh, trạng thái parity / khung theo chuẩn RFFE.
 *
 * Không phụ thuộc ESP-IDF -> giải mã được kiểm thử trên host (tests/rffe_decode).
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "HCCSpmiDecode.hpp"

namespace HCCRffeDecode {

// Dùng lại cấu trúc khung và engine lấy mẫu (SSC / cạnh xuống SCLK / idle / lost / swap).
using Txn = HCCSpmiDecode::Txn;
using Decoder = HCCSpmiDecode::Decoder;

// Lớp lệnh MIPI RFFE theo mã Command[7:0] (không xét parity).
enum Kind : uint8_t {
    K_REG0_WRITE,  // 1xxx xxxx (0x80-0xFF): Register 0 Write (7 bit dữ liệu nằm trong lệnh)
    K_REG_WRITE,   // 010a aaaa (0x40-0x5F): Register Write (địa chỉ 5 bit + 1 byte dữ liệu)
    K_REG_READ,    // 011a aaaa (0x60-0x7F): Register Read
    K_EXT_WRITE,   // 0000 nnnn (0x00-0x0F): Extended Register Write (địa chỉ 8 bit, nnnn+1 byte)
    K_EXT_READ,    // 0010 nnnn (0x20-0x2F): Extended Register Read
    K_EXTL_WRITE,  // 0011 0nnn (0x30-0x37): Extended Register Write Long (địa chỉ 16 bit)
    K_EXTL_READ,   // 0011 1nnn (0x38-0x3F): Extended Register Read Long
    K_RESERVED,    // 0001 xxxx (0x10-0x1F): lệnh đặc biệt / dành riêng - chỉ hiển thị mã thô
    K_BAD,         // khung lệnh sai parity / thiếu bit
};

// Trạng thái gói (cột STATUS). Ưu tiên: lệnh hỏng > thiếu bit > sai parity > lệnh khác > OK.
enum PktStatus : uint8_t {
    ST_OK,
    ST_PARITY,       // khung địa chỉ / dữ liệu sai parity
    ST_SHORT,        // chuỗi bị cắt (thiếu bit / bus park)
    ST_BAD_CMD,      // khung lệnh sai parity hoặc < 13 bit
    ST_RESERVED,     // lệnh hợp lệ nhưng thuộc vùng dành riêng (chỉ có mã)
};

// RFFE: USID 0 là địa chỉ broadcast / GSID mặc định (mọi slave nhận; không map về 1 IC cụ thể).
constexpr uint8_t kBroadcastUsid = 0x0;
inline bool isBroadcast(uint8_t usid) { return usid == kBroadcastUsid; }

Kind kindOf(uint8_t cmd);
const char *kindName(Kind k);      // nhãn ngắn cho bảng (REG R, EXT W...)
const char *kindLongName(Kind k);  // tên chuẩn MIPI RFFE (Register Read...)
bool isRead(Kind k);
bool isWrite(Kind k);
// Số khung địa chỉ 8 bit sau khung lệnh (Extended = 1, Extended Long = 2, còn lại 0).
int addrFrames(Kind k);

PktStatus status(const Txn &t);
const char *statusName(PktStatus s);

// Phân tích một chuỗi bit sau SSC thành Txn (USID = t.sid, Command = t.cmd, địa chỉ,
// dữ liệu, parity). Dùng lại HCCSpmiDecode::parse (đếm khung giống hệt cho RFFE vì
// vùng 0x10-0x1F không mang payload thanh ghi). Trả false nếu < 13 bit khung lệnh.
bool parse(const uint8_t *bits, int n, Txn &out);

}  // namespace HCCRffeDecode
