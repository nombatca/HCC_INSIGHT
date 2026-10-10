#include "HCCSpmiCapFile.hpp"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace HCCSpmiCapFile {

using HCCSpmiDecode::Txn;

namespace {

const char MAGIC[8] = {'H', 'C', 'C', 'S', 'P', 'M', 'I', '1'};
constexpr uint32_t VERSION = 1;
constexpr size_t HDR = 64;
constexpr size_t REC = 40;

void put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}
void put32(uint8_t *p, uint32_t v)
{
    put16(p, (uint16_t)v);
    put16(p + 2, (uint16_t)(v >> 16));
}
void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}
uint16_t get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}
uint32_t get32(const uint8_t *p)
{
    return get16(p) | ((uint32_t)get16(p + 2) << 16);
}
uint64_t get64(const uint8_t *p)
{
    return get32(p) | ((uint64_t)get32(p + 4) << 32);
}

void packRecord(const Txn &t, uint8_t *r)
{
    memset(r, 0, REC);
    put64(r + 0, t.t_sample);
    put32(r + 8, t.dur);
    r[12] = t.sid;
    r[13] = t.cmd;
    r[14] = (uint8_t)t.kind;
    r[15] = t.len;
    put16(r + 16, t.addr);
    memcpy(r + 18, t.data, 8);
    r[26] = t.parity_err;
    r[27] = t.short_bits;
    put16(r + 28, t.nbits);
    put16(r + 30, t.extra_bits);
    put16(r + 32, t.frame_err);
    r[34] = t.extra;
    r[35] = t.flags;
}

bool unpackRecord(const uint8_t *r, Txn &t)
{
    memset(&t, 0, sizeof(t));
    t.t_sample = get64(r);
    t.dur = get32(r + 8);
    t.sid = r[12] & 0x0F;
    t.cmd = r[13];
    if (r[14] > HCCSpmiDecode::K_BAD) {
        return false;
    }
    t.kind = (HCCSpmiDecode::Kind)r[14];
    t.len = r[15] > 8 ? 8 : r[15];
    t.addr = get16(r + 16);
    memcpy(t.data, r + 18, 8);
    t.parity_err = r[26];
    t.short_bits = r[27];
    t.nbits = get16(r + 28);
    t.extra_bits = get16(r + 30);
    t.frame_err = get16(r + 32);
    t.extra = r[34];
    t.flags = r[35];
    return true;
}

void packHeader(const Info &info, uint32_t npkt, uint32_t nseg, uint8_t *h)
{
    memset(h, 0, HDR);
    memcpy(h, MAGIC, 8);
    put32(h + 8, VERSION);
    put32(h + 12, info.flags);
    put32(h + 16, info.sample_hz);
    put32(h + 20, npkt);
    put32(h + 24, nseg);
    put32(h + 28, info.lost_chunks);
    put64(h + 32, info.total_samples);
    put32(h + 40, info.dropped_pkts);
    memcpy(h + 44, info.source, 15);
}

bool readHeader(FILE *f, Info &info, uint32_t &npkt, uint32_t &nseg, char *err, size_t es)
{
    uint8_t h[HDR];
    if (fread(h, 1, HDR, f) != HDR || memcmp(h, MAGIC, 8) != 0) {
        snprintf(err, es, "Không phải file capture SPMI (.spmc)");
        return false;
    }
    if (get32(h + 8) != VERSION) {
        snprintf(err, es, "Phiên bản file %u không hỗ trợ", (unsigned)get32(h + 8));
        return false;
    }
    info.flags = get32(h + 12);
    info.sample_hz = get32(h + 16);
    npkt = get32(h + 20);
    nseg = get32(h + 24);
    info.lost_chunks = get32(h + 28);
    info.total_samples = get64(h + 32);
    info.dropped_pkts = get32(h + 40);
    memcpy(info.source, h + 44, 15);
    info.source[15] = '\0';
    return true;
}

}  // namespace

bool save(const char *path, const Info &info_in, const HCCSpmiBuffers::PacketBuf &pk,
          const HCCSpmiBuffers::RawStore *raw, char *err, size_t es)
{
    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        snprintf(err, es, "Không tạo được %s", path);
        return false;
    }
    setvbuf(f, nullptr, _IOFBF, 16 * 1024);
    Info info = info_in;
    const uint32_t npkt = pk.count();
    const uint32_t nseg = (raw && raw->enabled()) ? raw->segCount() : 0;
    if (nseg > 0) {
        info.flags |= CF_HAS_RAW;
        if (raw->truncated()) {
            info.flags |= CF_RAW_TRUNC;
        }
    }
    uint8_t h[HDR];
    packHeader(info, npkt, nseg, h);
    bool ok = fwrite(h, 1, HDR, f) == HDR;
    uint8_t r[REC];
    for (uint32_t i = 0; ok && i < npkt; ++i) {
        packRecord(pk.at(i), r);
        ok = fwrite(r, 1, REC, f) == REC;
    }
    for (uint32_t i = 0; ok && i < nseg; ++i) {
        const HCCSpmiBuffers::Seg s = raw->seg(i);
        uint8_t sh[12];
        put64(sh, s.t0);
        put32(sh + 8, s.nbytes);
        ok = fwrite(sh, 1, 12, f) == 12 && fwrite(raw->bytes(s), 1, s.nbytes, f) == s.nbytes;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        snprintf(err, es, "Lỗi ghi %s (thẻ đầy?)", path);
    }
    return ok;
}

bool readInfo(const char *path, Info &info, uint32_t *npkt, uint32_t *nseg, char *err, size_t es)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        snprintf(err, es, "Không mở được %s", path);
        return false;
    }
    uint32_t np = 0, ns = 0;
    const bool ok = readHeader(f, info, np, ns, err, es);
    fclose(f);
    if (npkt) {
        *npkt = np;
    }
    if (nseg) {
        *nseg = ns;
    }
    return ok;
}

bool load(const char *path, Info &info, HCCSpmiBuffers::PacketBuf &pk, HCCSpmiBuffers::RawStore *raw, char *err,
          size_t es)
{
    pk.clear();
    if (raw) {
        raw->clear();
    }
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        snprintf(err, es, "Không mở được %s", path);
        return false;
    }
    setvbuf(f, nullptr, _IOFBF, 16 * 1024);
    uint32_t npkt = 0, nseg = 0;
    bool ok = readHeader(f, info, npkt, nseg, err, es);
    uint8_t r[REC];
    for (uint32_t i = 0; ok && i < npkt; ++i) {
        Txn t;
        if (fread(r, 1, REC, f) != REC || !unpackRecord(r, t)) {
            snprintf(err, es, "File hỏng ở gói %u", (unsigned)i);
            ok = false;
            break;
        }
        pk.push(t);  // đầy thì đếm dropped()
    }
    if (ok && raw && raw->enabled()) {
        uint8_t *tmp = (uint8_t *)malloc(4096);
        for (uint32_t i = 0; ok && tmp && i < nseg; ++i) {
            uint8_t sh[12];
            if (fread(sh, 1, 12, f) != 12) {
                snprintf(err, es, "File hỏng ở đoạn sóng %u", (unsigned)i);
                ok = false;
                break;
            }
            uint64_t t0 = get64(sh);
            uint32_t n = get32(sh + 8);
            while (n > 0) {
                const uint32_t k = n > 4096 ? 4096 : n;
                if (fread(tmp, 1, k, f) != k) {
                    snprintf(err, es, "File hỏng ở đoạn sóng %u", (unsigned)i);
                    ok = false;
                    break;
                }
                raw->appendSeg(t0, tmp, k);
                t0 += (uint64_t)k * 4;
                n -= k;
            }
        }
        free(tmp);
        raw->setEnd(info.total_samples);
    }
    fclose(f);
    return ok;
}

bool forEachSeg(const char *path, uint8_t *tmp, uint32_t tmp_size, SegCb cb, void *user, char *err, size_t es)
{
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        snprintf(err, es, "Không mở được %s", path);
        return false;
    }
    Info info;
    uint32_t npkt = 0, nseg = 0;
    bool ok = readHeader(f, info, npkt, nseg, err, es);
    if (ok && fseek(f, (long)(npkt * REC), SEEK_CUR) != 0) {
        ok = false;
    }
    for (uint32_t i = 0; ok && i < nseg; ++i) {
        uint8_t sh[12];
        if (fread(sh, 1, 12, f) != 12) {
            snprintf(err, es, "File hỏng ở đoạn sóng %u", (unsigned)i);
            ok = false;
            break;
        }
        uint64_t t0 = get64(sh);
        uint32_t n = get32(sh + 8);
        while (ok && n > 0) {
            const uint32_t k = n > tmp_size ? tmp_size : n;
            if (fread(tmp, 1, k, f) != k) {
                snprintf(err, es, "File hỏng ở đoạn sóng %u", (unsigned)i);
                ok = false;
                break;
            }
            if (!cb(t0, tmp, k, user)) {
                fclose(f);
                return true;
            }
            t0 += (uint64_t)k * 4;
            n -= k;
        }
    }
    fclose(f);
    return ok;
}

bool exportCsv(const char *path, const Info &info, const HCCSpmiBuffers::PacketBuf &pk, char *err, size_t es)
{
    FILE *f = fopen(path, "w");
    if (f == nullptr) {
        snprintf(err, es, "Không tạo được %s", path);
        return false;
    }
    setvbuf(f, nullptr, _IOFBF, 16 * 1024);
    fprintf(f, "# HCC INSIGHT SPMI MONITOR, nguon=%s, sample_hz=%u, mat_chunk=%u, goi_bo=%u\n", info.source,
            (unsigned)info.sample_hz, (unsigned)info.lost_chunks, (unsigned)info.dropped_pkts);
    if (info.flags & CF_DEMO) {
        fprintf(f, "# DEMO - DU LIEU MO PHONG, KHONG PHAI KET QUA DO\n");
    }
    fprintf(f, "stt,thoi_gian_us,sid,lenh_hex,loai,dia_chi_hex,so_byte,du_lieu_hex,loi_parity,thieu_bit,bit_du,"
               "trang_thai,mat_truoc\n");
    const uint32_t n = pk.count();
    const double us_per = 1e6 / (double)(info.sample_hz ? info.sample_hz : 1);
    for (uint32_t i = 0; i < n; ++i) {
        const Txn &t = pk.at(i);
        char data[32] = "";
        int k = 0;
        for (int j = 0; j < t.len && j < 8; ++j) {
            k += snprintf(data + k, sizeof(data) - k, "%s%02X", j ? " " : "", t.data[j]);
        }
        fprintf(f, "%u,%.3f,%u,%02X,%s,%04X,%u,%s,%u,%u,%u,%s,%u\n", (unsigned)(i + 1), t.t_sample * us_per,
                (unsigned)t.sid, t.cmd, HCCSpmiDecode::kindName(t.kind), t.addr, (unsigned)t.len, data,
                (unsigned)t.parity_err, (unsigned)t.short_bits, (unsigned)t.extra_bits,
                HCCSpmiDecode::statusName(HCCSpmiDecode::status(t)), (unsigned)((t.flags & HCCSpmiDecode::F_LOST_BEFORE) != 0));
    }
    const bool ok = fclose(f) == 0;
    if (!ok) {
        snprintf(err, es, "Lỗi ghi %s", path);
    }
    return ok;
}

namespace {

// Tách field CSV (không có dấu ngoặc kép trong file của mình).
int splitCsv(char *line, char **fields, int max)
{
    int n = 0;
    char *p = line;
    while (n < max) {
        fields[n++] = p;
        char *c = strchr(p, ',');
        if (c == nullptr) {
            break;
        }
        *c = '\0';
        p = c + 1;
    }
    return n;
}

}  // namespace

bool importCsv(const char *path, Info &info, HCCSpmiBuffers::PacketBuf &pk, char *err, size_t es)
{
    pk.clear();
    FILE *f = fopen(path, "r");
    if (f == nullptr) {
        snprintf(err, es, "Không mở được %s", path);
        return false;
    }
    info = Info();
    snprintf(info.source, sizeof(info.source), "CSV");
    // Không có sample_hz trong file: thời gian lưu theo đơn vị 1/100 µs.
    info.sample_hz = 100000000;
    char line[256];
    bool header = false;
    bool ok = true;
    uint32_t lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        ++lineno;
        char *nl = strpbrk(line, "\r\n");
        if (nl) {
            *nl = '\0';
        }
        if (line[0] == '#') {
            if (strstr(line, "DEMO")) {
                info.flags |= CF_DEMO;
            }
            const char *nm = strstr(line, "nguon=");
            if (nm) {
                int k = 0;
                for (nm += 6; *nm && *nm != ',' && k < 15; ++nm) {
                    info.source[k++] = *nm;
                }
                info.source[k] = '\0';
            }
            continue;
        }
        if (!header) {
            if (strncmp(line, "stt,", 4) != 0) {
                snprintf(err, es, "Không phải CSV SPMI (thiếu dòng tiêu đề)");
                ok = false;
                break;
            }
            header = true;
            continue;
        }
        if (line[0] == '\0') {
            continue;
        }
        char *fd[16];
        const int n = splitCsv(line, fd, 16);
        if (n < 11) {
            snprintf(err, es, "Dòng %u thiếu cột", (unsigned)lineno);
            ok = false;
            break;
        }
        Txn t;
        memset(&t, 0, sizeof(t));
        t.t_sample = (uint64_t)(atof(fd[1]) * 100.0 + 0.5);
        t.sid = (uint8_t)(atoi(fd[2]) & 0x0F);
        t.cmd = (uint8_t)strtoul(fd[3], nullptr, 16);
        t.addr = (uint16_t)strtoul(fd[5], nullptr, 16);
        t.len = (uint8_t)atoi(fd[6]);
        if (t.len > 8) {
            t.len = 8;
        }
        const char *d = fd[7];
        for (int j = 0; j < t.len && *d; ++j) {
            t.data[j] = (uint8_t)strtoul(d, (char **)&d, 16);
        }
        t.parity_err = (uint8_t)atoi(fd[8]);
        t.short_bits = (uint8_t)atoi(fd[9]);
        t.extra_bits = (uint16_t)atoi(fd[10]);
        // Lớp lệnh lấy từ mã lệnh; khung lệnh hỏng ghi "LỖI KHUNG" ở cột loại.
        const bool bad = strstr(fd[4], "LỖI") != nullptr || strstr(fd[4], "LOI") != nullptr;
        t.kind = bad ? HCCSpmiDecode::K_BAD : HCCSpmiDecode::kindOf(t.cmd);
        if (n >= 13 && atoi(fd[12])) {
            t.flags |= HCCSpmiDecode::F_LOST_BEFORE;
        }
        pk.push(t);
    }
    fclose(f);
    if (ok && !header) {
        snprintf(err, es, "File rỗng");
        ok = false;
    }
    return ok;
}

}  // namespace HCCSpmiCapFile
