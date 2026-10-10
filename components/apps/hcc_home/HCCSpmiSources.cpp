/*
 * Nguồn dữ liệu SPMI MONITOR không cần phần cứng thu: DEMO (mô phỏng), FILE (phân tích lại), EXTERNAL (chỗ cho
 * FPGA / STM32, chưa có driver).
 */
#include <algorithm>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "HCCSpmiCapFile.hpp"
#include "HCCSpmiSource.hpp"
#include "HCCSpmiSynth.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace HCCSpmiSource {

namespace {

// ==================== DEMO ====================
// Dữ liệu MÔ PHỎNG - mọi gói / file từ nguồn này đều gắn cờ DEMO. Trình tự cố định: vài lệnh ghi khởi tạo,
// sau đó đọc / ghi định kỳ; cứ 97 gói có 1 gói sai parity và 1 gói bị cắt để thử hiển thị lỗi.
class DemoSource : public Source {
public:
    const char *name() const override { return "DEMO"; }
    bool supported(const Params &, char *, size_t) const override { return true; }

    bool run(const Params &p, Feed &f, char *, size_t) override
    {
        const uint32_t hz = 80000000;
        f.setSampleHz(hz);
        f.setDemo(true);
        f.message("DEMO: dữ liệu mô phỏng, không phải kết quả đo");
        HCCSpmiSynth::Synth s(4);  // SCLK = 10 MHz ở 80 MHz lấy mẫu
        uint32_t k = 0;
        uint8_t val = 0;
        while (!f.stopRequested()) {
            s.clear();
            for (int i = 0; i < 24; ++i, ++k) {
                s.idle(300 + (int)(k % 7) * 40);
                const uint8_t sid = (k % 5 == 0) ? 4 : 1;
                switch (k % 6) {
                case 0: s.regWrite(sid, (uint8_t)(0x10 + k % 8), val++); break;
                case 1: s.regRead(sid, 0x12, (uint8_t)(0x5A ^ (k & 0x0F))); break;
                case 2: {
                    const uint8_t d[2] = {(uint8_t)(k >> 4), (uint8_t)k};
                    s.extLongRead(sid, 0x0105, d, 2);
                    break;
                }
                case 3: s.reg0Write(sid, (uint8_t)(k & 0x7F)); break;
                case 4: {
                    const uint8_t d[3] = {0x01, 0x80, (uint8_t)k};
                    s.extWrite(sid, 0x40, d, 3);
                    break;
                }
                default: s.regWrite(sid, 0x13, 0x0F, k % 97 == 5); break;
                }
                if (k % 97 == 50) {
                    s.ssc();
                    s.frame(0x145, 12);  // gói bị cắt: chỉ có khung lệnh Register Write
                }
            }
            s.idle(2000);
            const std::vector<uint8_t> &d = s.packed();
            for (size_t i = 0; i < d.size(); i += 4032) {
                f.samples(d.data() + i, std::min<size_t>(4032, d.size() - i));
            }
            f.idle((uint64_t)hz / 50 - s.samples());  // ~ 24 gói / 20 ms
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        (void)p;
        return true;
    }
};

// ==================== FILE ====================
bool endsWith(const char *s, const char *ext)
{
    const size_t a = strlen(s), b = strlen(ext);
    return a >= b && strcasecmp(s + a - b, ext) == 0;
}

class FileSource : public Source {
public:
    const char *name() const override { return "FILE"; }
    bool live() const override { return false; }
    bool supported(const Params &p, char *why, size_t n) const override
    {
        if (!endsWith(p.file, ".spmc") && !endsWith(p.file, ".csv")) {
            snprintf(why, n, "Chỉ phân tích được file .spmc hoặc .csv");
            return false;
        }
        return true;
    }

    bool run(const Params &p, Feed &f, char *err, size_t es) override
    {
        if (endsWith(p.file, ".csv")) {
            return runCsv(p, f, err, es);
        }
        HCCSpmiCapFile::Info info;
        uint32_t npkt = 0, nseg = 0;
        if (!HCCSpmiCapFile::readInfo(p.file, info, &npkt, &nseg, err, es)) {
            return false;
        }
        f.setSampleHz(info.sample_hz);
        f.setOrientation((info.flags & HCCSpmiCapFile::CF_SWAPPED) ? 2 : 1);
        f.setFlags(info.flags, info.lost_chunks, info.dropped_pkts);
        const uint32_t bad = HCCSpmiCapFile::CF_RAW_TRUNC | HCCSpmiCapFile::CF_OVERFLOW | HCCSpmiCapFile::CF_PKT_TRUNC;
        if ((info.flags & HCCSpmiCapFile::CF_HAS_RAW) && !(info.flags & bad) && nseg > 0) {
            // Giải mã lại từ sóng thô bằng bộ giải mã hiện tại.
            struct Ctx {
                Feed *f;
                uint64_t pos;
            } ctx = {&f, 0};
            static uint8_t tmp[4096];
            const bool ok = HCCSpmiCapFile::forEachSeg(
                p.file, tmp, sizeof(tmp),
                [](uint64_t t0, const uint8_t *d, uint32_t n, void *u) {
                    Ctx *c = (Ctx *)u;
                    if (c->f->stopRequested()) {
                        return false;
                    }
                    if (t0 > c->pos) {
                        c->f->idle(t0 - c->pos);
                    }
                    c->f->samples(d, n);
                    c->pos = t0 + (uint64_t)n * 4;
                    return true;
                },
                &ctx, err, es);
            if (!ok) {
                return false;
            }
            if (info.total_samples > ctx.pos) {
                f.idle(info.total_samples - ctx.pos);
            }
            char m[160];
            snprintf(m, sizeof(m), "Đã giải mã lại từ sóng (%u đoạn). File gốc có %u gói.%s", (unsigned)nseg,
                     (unsigned)npkt, (info.flags & HCCSpmiCapFile::CF_DEMO) ? " DEMO" : "");
            f.message(m);
            return true;
        }
        // Thiếu sóng hoặc capture gốc có mất dữ liệu: dùng gói đã lưu (giữ cờ mất dữ liệu), nạp sóng để xem.
        if (!loadPackets(p.file, f, err, es)) {
            return false;
        }
        char m[160];
        snprintf(m, sizeof(m), "Đã nạp %u gói đã lưu (%s)%s", (unsigned)npkt,
                 (info.flags & HCCSpmiCapFile::CF_HAS_RAW) ? "sóng không đủ để giải mã lại" : "file không có sóng",
                 (info.flags & HCCSpmiCapFile::CF_DEMO) ? " DEMO" : "");
        f.message(m);
        return true;
    }

private:
    bool loadPackets(const char *path, Feed &f, char *err, size_t es)
    {
        // CapFile::load cần PacketBuf: nạp vào bộ đệm tạm (malloc lớn -> PSRAM) rồi chuyển sang Feed.
        HCCSpmiCapFile::Info info;
        uint32_t npkt = 0;
        if (!HCCSpmiCapFile::readInfo(path, info, &npkt, nullptr, err, es)) {
            return false;
        }
        const uint32_t cap = npkt ? npkt : 1;
        auto *mem = (HCCSpmiDecode::Txn *)malloc(sizeof(HCCSpmiDecode::Txn) * cap);
        if (mem == nullptr) {
            snprintf(err, es, "Hết bộ nhớ để nạp %u gói", (unsigned)npkt);
            return false;
        }
        HCCSpmiBuffers::PacketBuf tmp;
        tmp.init(mem, cap);
        const bool ok = HCCSpmiCapFile::load(path, info, tmp, &f.rawStore(), err, es);
        for (uint32_t i = 0; ok && i < tmp.count(); ++i) {
            f.packet(tmp.at(i));
        }
        f.idle(info.total_samples);
        free(mem);
        return ok;
    }

    bool runCsv(const Params &p, Feed &f, char *err, size_t es)
    {
        // CSV không có sóng: chỉ gói.
        const uint32_t cap = 150000;
        auto *mem = (HCCSpmiDecode::Txn *)malloc(sizeof(HCCSpmiDecode::Txn) * cap);
        if (mem == nullptr) {
            snprintf(err, es, "Hết bộ nhớ để nạp CSV");
            return false;
        }
        HCCSpmiBuffers::PacketBuf tmp;
        tmp.init(mem, cap);
        HCCSpmiCapFile::Info info;
        const bool ok = HCCSpmiCapFile::importCsv(p.file, info, tmp, err, es);
        if (ok) {
            f.setSampleHz(info.sample_hz);
            f.setFlags(info.flags, 0, tmp.dropped());
            for (uint32_t i = 0; i < tmp.count(); ++i) {
                f.packet(tmp.at(i));
            }
            char m[160];
            snprintf(m, sizeof(m), "Đã nạp %u gói từ CSV (không có sóng)%s", (unsigned)tmp.count(),
                     (info.flags & HCCSpmiCapFile::CF_DEMO) ? " DEMO" : "");
            f.message(m);
        }
        free(mem);
        return ok;
    }
};

// ==================== EXTERNAL (FPGA / STM32) ====================
// Dự kiến: bộ thu ngoài lấy mẫu SPMI (>= 4 x SCLK) hoặc giải mã tại chỗ, gửi về qua USB / UART / SPI:
//   - luồng mẫu 2 bit giống PARLIO  -> Feed::samples / lost
//   - hoặc gói đã giải mã           -> Feed::packet
// Chưa có driver: trả HARDWARE NOT SUPPORTED, không sinh dữ liệu.
class ExternalSource : public Source {
public:
    const char *name() const override { return "FPGA/STM32"; }
    bool supported(const Params &, char *why, size_t n) const override
    {
        snprintf(why, n, "Chưa có driver bộ thu ngoài (FPGA / STM32). Giao diện nguồn đã sẵn (HCCSpmiSource.hpp).");
        return false;
    }
    bool run(const Params &, Feed &, char *err, size_t n) override
    {
        snprintf(err, n, "Không hỗ trợ");
        return false;
    }
};

DemoSource s_demo;
FileSource s_file;
ExternalSource s_ext;

}  // namespace

Source *demo()
{
    return &s_demo;
}
Source *file()
{
    return &s_file;
}
Source *external()
{
    return &s_ext;
}

}  // namespace HCCSpmiSource
