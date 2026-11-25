#include "HCCRffeDecode.hpp"

namespace HCCRffeDecode {

Kind kindOf(uint8_t c)
{
    if (c & 0x80) {
        return K_REG0_WRITE;        // 1xxx xxxx
    }
    if ((c & 0xE0) == 0x40) {
        return K_REG_WRITE;         // 010a aaaa
    }
    if ((c & 0xE0) == 0x60) {
        return K_REG_READ;          // 011a aaaa
    }
    if ((c & 0xF0) == 0x00) {
        return K_EXT_WRITE;         // 0000 nnnn
    }
    if ((c & 0xF0) == 0x20) {
        return K_EXT_READ;          // 0010 nnnn
    }
    if ((c & 0xF8) == 0x30) {
        return K_EXTL_WRITE;        // 0011 0nnn
    }
    if ((c & 0xF8) == 0x38) {
        return K_EXTL_READ;         // 0011 1nnn
    }
    return K_RESERVED;              // 0001 xxxx (0x10-0x1F)
}

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
    case K_RESERVED: return "DÀNH RIÊNG";
    case K_BAD: return "LỖI KHUNG";
    }
    return "?";
}

const char *kindLongName(Kind k)
{
    switch (k) {
    case K_REG0_WRITE: return "Register 0 Write";
    case K_REG_WRITE: return "Register Write";
    case K_REG_READ: return "Register Read";
    case K_EXT_WRITE: return "Extended Register Write";
    case K_EXT_READ: return "Extended Register Read";
    case K_EXTL_WRITE: return "Extended Register Write Long";
    case K_EXTL_READ: return "Extended Register Read Long";
    case K_RESERVED: return "Lệnh đặc biệt / dành riêng";
    case K_BAD: return "Khung lệnh hỏng";
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

int addrFrames(Kind k)
{
    switch (k) {
    case K_EXT_WRITE:
    case K_EXT_READ: return 1;
    case K_EXTL_WRITE:
    case K_EXTL_READ: return 2;
    default: return 0;
    }
}

PktStatus status(const Txn &t)
{
    if (t.nbits < 13 || (t.frame_err & 1u) != 0) {
        return ST_BAD_CMD;          // khung lệnh thiếu bit / sai parity
    }
    if (t.short_bits) {
        return ST_SHORT;
    }
    if (t.parity_err) {
        return ST_PARITY;
    }
    if (kindOf(t.cmd) == K_RESERVED) {
        return ST_RESERVED;
    }
    return ST_OK;
}

const char *statusName(PktStatus s)
{
    switch (s) {
    case ST_OK: return "OK";
    case ST_PARITY: return "SAI PARITY";
    case ST_SHORT: return "THIẾU BIT";
    case ST_BAD_CMD: return "LỖI LỆNH";
    case ST_RESERVED: return "DÀNH RIÊNG";
    }
    return "?";
}

bool parse(const uint8_t *bits, int n, Txn &out)
{
    // Engine lấy mẫu + đếm khung giống hệt RFFE (vùng 0x10-0x1F không mang payload
    // thanh ghi ở cả SPMI lẫn RFFE), nên tái dùng bộ phân tích khung đã kiểm thử.
    return HCCSpmiDecode::parse(bits, n, out);
}

}  // namespace HCCRffeDecode
