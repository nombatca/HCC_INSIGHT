/*
 * HCCSpmiCapFile: lưu / đọc capture SPMI (không phụ thuộc ESP-IDF, chỉ stdio).
 *
 * File .spmc (nhị phân, little-endian):
 *   Header 64 byte: "HCCSPMI1", u32 version (1), u32 flags (CF_*), u32 sample_hz, u32 số gói, u32 số đoạn sóng,
 *                   u32 số chunk mất, u64 tổng số mẫu, u32 gói bị bỏ (đầy bộ đệm), char source[16], 4 byte dự trữ.
 *   Gói: mỗi gói 40 byte (xem packRecord trong .cpp).
 *   Sóng: mỗi đoạn u64 t0 + u32 số byte + dữ liệu (4 mẫu / byte, bit0 = dây 0, bit1 = dây 1 như lúc thu).
 * CSV: bảng gói đọc được bằng Excel; dòng '#' đầu file ghi nguồn, tốc độ lấy mẫu và chữ DEMO nếu là dữ liệu mô phỏng.
 * importCsv đọc cả CSV của SPMI MONITOR lẫn file spmi_NNN.csv của màn BẮT DỮ LIỆU SPMI cũ.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "HCCSpmiBuffers.hpp"

namespace HCCSpmiCapFile {

enum : uint32_t {
    CF_DEMO = 1,        // dữ liệu mô phỏng (nguồn DEMO / test) - KHÔNG phải kết quả đo
    CF_SWAPPED = 2,     // dây 1 (bit1) là SCLK
    CF_RAW_TRUNC = 4,   // sóng thô không đủ (hết bộ nhớ)
    CF_PKT_TRUNC = 8,   // có gói bị bỏ (đầy bộ đệm gói)
    CF_OVERFLOW = 16,   // có dữ liệu mất trong lúc thu (tràn DMA)
    CF_HAS_RAW = 32,
};

struct Info {
    uint32_t flags = 0;
    uint32_t sample_hz = 0;
    uint64_t total_samples = 0;
    uint32_t lost_chunks = 0;
    uint32_t dropped_pkts = 0;
    char source[16] = "";
};

bool save(const char *path, const Info &info, const HCCSpmiBuffers::PacketBuf &pk,
          const HCCSpmiBuffers::RawStore *raw, char *err, size_t err_size);

// Đọc header (để liệt kê file / kiểm tra DEMO).
bool readInfo(const char *path, Info &info, uint32_t *npkt, uint32_t *nseg, char *err, size_t err_size);

// Đọc gói vào pk (pk được clear trước). raw != nullptr: đọc cả sóng.
bool load(const char *path, Info &info, HCCSpmiBuffers::PacketBuf &pk, HCCSpmiBuffers::RawStore *raw, char *err,
          size_t err_size);

// Duyệt các đoạn sóng của file (để giải mã lại): cb trả false để dừng.
typedef bool (*SegCb)(uint64_t t0, const uint8_t *d, uint32_t n, void *user);
bool forEachSeg(const char *path, uint8_t *tmp, uint32_t tmp_size, SegCb cb, void *user, char *err, size_t err_size);

bool exportCsv(const char *path, const Info &info, const HCCSpmiBuffers::PacketBuf &pk, char *err, size_t err_size);
bool importCsv(const char *path, Info &info, HCCSpmiBuffers::PacketBuf &pk, char *err, size_t err_size);

}  // namespace HCCSpmiCapFile
