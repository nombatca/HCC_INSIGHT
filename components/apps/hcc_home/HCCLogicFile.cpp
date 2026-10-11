#include "HCCLogicFile.hpp"

#include <stdio.h>
#include <string.h>

namespace HCCLogicFile {

namespace {

const char MAGIC[8] = {'H', 'C', 'C', 'L', 'O', 'G', 'I', 'C'};
constexpr size_t HDR = 128;

void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

uint32_t get32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool readHdr(FILE *f, Info &info, char *err, size_t es)
{
    uint8_t h[HDR];
    if (fread(h, 1, HDR, f) != HDR || memcmp(h, MAGIC, 8) != 0) {
        snprintf(err, es, "Không phải file LOGIC (.lab)");
        return false;
    }
    if (get32(h + 8) != 1) {
        snprintf(err, es, "Phiên bản file không hỗ trợ");
        return false;
    }
    info.sample_hz = get32(h + 12);
    info.samples = get32(h + 16);
    info.trig = get32(h + 20);
    info.flags = get32(h + 24);
    memcpy(info.names, h + 28, 64);
    for (auto &n : info.names) {
        n[7] = '\0';
    }
    return true;
}

}  // namespace

bool saveLab(const char *path, const Info &info, const uint8_t *d, char *err, size_t es)
{
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        snprintf(err, es, "Không tạo được %s", path);
        return false;
    }
    uint8_t h[HDR] = {};
    memcpy(h, MAGIC, 8);
    put32(h + 8, 1);
    put32(h + 12, info.sample_hz);
    put32(h + 16, info.samples);
    put32(h + 20, info.trig);
    put32(h + 24, info.flags);
    memcpy(h + 28, info.names, 64);
    bool ok = fwrite(h, 1, HDR, f) == HDR;
    size_t done = 0;
    while (ok && done < info.samples) {
        const size_t k = info.samples - done > 65536 ? 65536 : info.samples - done;
        ok = fwrite(d + done, 1, k, f) == k;
        done += k;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        snprintf(err, es, "Lỗi ghi %s (thẻ đầy?)", path);
    }
    return ok;
}

bool readLabInfo(const char *path, Info &info, char *err, size_t es)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        snprintf(err, es, "Không mở được %s", path);
        return false;
    }
    const bool ok = readHdr(f, info, err, es);
    fclose(f);
    return ok;
}

bool loadLab(const char *path, Info &info, uint8_t *d, uint32_t cap, char *err, size_t es)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        snprintf(err, es, "Không mở được %s", path);
        return false;
    }
    bool ok = readHdr(f, info, err, es);
    if (ok) {
        const uint32_t want = info.samples < cap ? info.samples : cap;
        const size_t got = fread(d, 1, want, f);
        if (got < want) {
            snprintf(err, es, "File bị cắt (%u / %u mẫu)", (unsigned)got, (unsigned)info.samples);
            info.flags |= LF_PARTIAL;
        }
        if (info.samples > cap) {
            info.flags |= LF_PARTIAL;
        }
        info.samples = (uint32_t)got;
        if (info.trig != 0xFFFFFFFF && info.trig >= info.samples) {
            info.trig = 0xFFFFFFFF;
        }
        ok = got > 0;
        if (!ok && err[0] == '\0') {
            snprintf(err, es, "File rỗng");
        }
    }
    fclose(f);
    return ok;
}

bool exportVcd(const char *path, const Info &info, const uint8_t *d, uint8_t mask, char *err, size_t es)
{
    FILE *f = fopen(path, "w");
    if (f == nullptr) {
        snprintf(err, es, "Không tạo được %s", path);
        return false;
    }
    setvbuf(f, nullptr, _IOFBF, 16 * 1024);
    fprintf(f, "$comment HCC INSIGHT LOGIC ANALYZER, %u Hz%s $end\n", (unsigned)info.sample_hz,
            (info.flags & LF_DEMO) ? ", DEMO - du lieu mo phong" : "");
    fprintf(f, "$timescale 1 ps $end\n$scope module logic $end\n");
    for (int k = 0; k < 8; ++k) {
        if (mask & (1u << k)) {
            fprintf(f, "$var wire 1 %c %s $end\n", '!' + k, info.names[k][0] ? info.names[k] : "D");
        }
    }
    fprintf(f, "$upscope $end\n$enddefinitions $end\n");
    const double ps = 1e12 / (double)(info.sample_hz ? info.sample_hz : 1);
    int prev = -1;
    for (uint32_t i = 0; i < info.samples; ++i) {
        const uint8_t v = d[i] & mask;
        if ((int)v == prev) {
            continue;
        }
        fprintf(f, "#%llu\n", (unsigned long long)(i * ps + 0.5));
        for (int k = 0; k < 8; ++k) {
            const uint8_t b = (uint8_t)(1u << k);
            if ((mask & b) && (prev < 0 || ((v ^ prev) & b))) {
                fprintf(f, "%d%c\n", (v & b) ? 1 : 0, '!' + k);
            }
        }
        prev = v;
    }
    fprintf(f, "#%llu\n", (unsigned long long)(info.samples * ps + 0.5));
    const bool ok = fclose(f) == 0;
    if (!ok) {
        snprintf(err, es, "Lỗi ghi %s", path);
    }
    return ok;
}

}  // namespace HCCLogicFile
