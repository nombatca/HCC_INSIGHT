/*
 * HCCSpmiAnalyzer: so sánh hai capture SPMI (main tốt A / main lỗi B) - không phụ thuộc ESP-IDF.
 *
 * compare(): dóng hàng trình tự gói theo khoá (SID, lớp lệnh, mã lệnh với lệnh không có địa chỉ, địa chỉ)
 *   bằng thuật toán tham lam có cửa sổ nhìn trước (window): gặp khác khoá thì tìm khoá đó gần nhất ở phía bên kia
 *   để quyết định "thiếu ở B" hay "thừa ở B". Đây là heuristic: với chuỗi lặp dày đặc có thể dóng lệch - kết quả
 *   là gợi ý để xem lại, không phải kết luận.
 *   Gói lỗi (khung lệnh hỏng / thiếu bit) không dóng được: ghi thành dòng LỖI GIAO TIẾP. Gói khớp khoá nhưng có
 *   khung sai parity cũng là LỖI GIAO TIẾP (dữ liệu không tin được).
 * finalRegs(): giá trị GHI cuối cùng theo (SID, địa chỉ) ở A và B (không phụ thuộc thứ tự).
 * PmicMap  : mapping thanh ghi PMIC DO NGƯỜI DÙNG CUNG CẤP (/sdcard/spmi/pmic_map.csv), mỗi dòng
 *              sid,dia_chi,mask,gia_tri,nhan,nguon
 *            Dòng không có cột nguồn (tài liệu / đo kiểm đã xác minh) bị loại. Firmware không kèm mapping nào.
 *            Kết quả chỉ nói lệnh ghi có khớp mapping hay không - KHÔNG kết luận rail đã có điện áp.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

#include "HCCSpmiDecode.hpp"

namespace HCCSpmiAnalyzer {

using HCCSpmiDecode::Txn;

enum DiffType : uint8_t {
    D_SAME,      // cùng lệnh, cùng dữ liệu
    D_DATA,      // cùng lệnh, khác dữ liệu
    D_MISSING,   // có ở A, không có ở B
    D_EXTRA,     // chỉ có ở B
    D_ERR,       // lỗi giao tiếp (một trong hai gói lỗi)
};

const char *diffName(DiffType d);

struct Row {
    DiffType type;
    int32_t ia;  // chỉ số gói ở A (-1 nếu không có)
    int32_t ib;  // chỉ số gói ở B
};

struct Result {
    std::vector<Row> rows;
    uint32_t same = 0, data = 0, missing = 0, extra = 0, err = 0;
    uint32_t err_a = 0, err_b = 0;   // số gói lỗi trong từng capture
    int32_t first_diff = -1;         // chỉ số dòng khác biệt đầu tiên
    bool limited = false;            // đã cắt bớt vì quá max_rows
};

// window: số gói nhìn trước để dóng lại (mặc định 64). max_rows: giới hạn số dòng kết quả.
void compare(const Txn *a, uint32_t na, const Txn *b, uint32_t nb, Result &out, int window = 64,
             uint32_t max_rows = 400000);

struct RegRow {
    uint8_t sid;
    uint16_t addr;
    bool has_a, has_b;
    uint8_t last_a, last_b;
    uint32_t writes_a, writes_b;
};
// Chỉ gói GHI không lỗi; nhiều byte -> địa chỉ addr + i. Sắp theo SID rồi địa chỉ.
std::vector<RegRow> finalRegs(const Txn *a, uint32_t na, const Txn *b, uint32_t nb);

struct MapEntry {
    uint8_t sid;
    uint16_t addr;
    uint8_t mask;
    uint8_t value;
    std::string label;
    std::string source;
};

struct PmicMap {
    std::vector<MapEntry> entries;
    uint32_t rejected = 0;  // dòng thiếu nguồn / sai định dạng
    bool loaded = false;
};

// Đọc từ text (nội dung file). Trả số dòng hợp lệ.
int parseMap(const char *text, PmicMap &map);
bool loadMap(const char *path, PmicMap &map);

enum MapState : uint8_t {
    M_NO_WRITE,   // không thấy lệnh ghi thanh ghi này
    M_MATCH,      // lệnh ghi cuối cùng khớp (giá trị & mask) == (mapping & mask)
    M_DIFF,       // có ghi nhưng không khớp
};
const char *mapStateName(MapState s);
// Áp mapping lên một capture (lệnh ghi cuối của thanh ghi). last_value nhận giá trị ghi cuối (nếu có).
MapState applyMap(const MapEntry &e, const Txn *t, uint32_t n, uint8_t *last_value);

}  // namespace HCCSpmiAnalyzer
