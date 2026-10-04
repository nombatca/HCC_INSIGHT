/*
 * HCCAudioMeta: xem HCCAudioMeta.h. Viết bằng stdio thuần để chạy thử được trên máy tính
 * (test/music/meta_test.c).
 */
#include "HCCAudioMeta.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p)
{
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static uint32_t le32(const uint8_t *p)
{
    return ((uint32_t)p[3] << 24) | ((uint32_t)p[2] << 16) | ((uint32_t)p[1] << 8) | p[0];
}

static uint16_t le16(const uint8_t *p)
{
    return (uint16_t)(p[0] | (p[1] << 8));
}

static size_t read_at(FILE *f, int64_t pos, void *buf, size_t len)
{
    if (pos < 0 || fseek(f, (long)pos, SEEK_SET) != 0) {
        return 0;
    }
    return fread(buf, 1, len, f);
}

hcc_audio_fmt_t hcc_audio_fmt_from_name(const char *name)
{
    const char *dot = name ? strrchr(name, '.') : NULL;
    if (dot == NULL) {
        return HCC_AUDIO_FMT_UNKNOWN;
    }
    if (strcasecmp(dot, ".mp3") == 0) return HCC_AUDIO_FMT_MP3;
    if (strcasecmp(dot, ".aac") == 0) return HCC_AUDIO_FMT_AAC;
    if (strcasecmp(dot, ".flac") == 0) return HCC_AUDIO_FMT_FLAC;
    if (strcasecmp(dot, ".wav") == 0) return HCC_AUDIO_FMT_WAV;
    if (strcasecmp(dot, ".m4a") == 0 || strcasecmp(dot, ".mp4") == 0) return HCC_AUDIO_FMT_M4A;
    if (strcasecmp(dot, ".ogg") == 0 || strcasecmp(dot, ".opus") == 0) return HCC_AUDIO_FMT_OGG;
    return HCC_AUDIO_FMT_UNKNOWN;
}

const char *hcc_audio_fmt_name(hcc_audio_fmt_t fmt)
{
    switch (fmt) {
    case HCC_AUDIO_FMT_MP3: return "MP3";
    case HCC_AUDIO_FMT_AAC: return "AAC";
    case HCC_AUDIO_FMT_FLAC: return "FLAC";
    case HCC_AUDIO_FMT_WAV: return "WAV";
    case HCC_AUDIO_FMT_M4A: return "M4A";
    case HCC_AUDIO_FMT_OGG: return "OGG";
    default: return "?";
    }
}

// Kích thước thẻ ID3v2 ở đầu file (0 nếu không có).
static int64_t id3v2_size(FILE *f)
{
    uint8_t h[10];
    if (read_at(f, 0, h, sizeof(h)) != sizeof(h) || memcmp(h, "ID3", 3) != 0 ||
            ((h[6] | h[7] | h[8] | h[9]) & 0x80) != 0) {
        return 0;
    }
    int64_t size = ((int64_t)h[6] << 21) | ((int64_t)h[7] << 14) | ((int64_t)h[8] << 7) | h[9];
    size += 10;
    if (h[5] & 0x10) {
        size += 10;  // footer
    }
    return size;
}

/* ---------------------------------- MP3 ---------------------------------- */

typedef struct {
    int version;   // 1 = MPEG1, 2 = MPEG2, 25 = MPEG2.5
    uint32_t bitrate;
    uint32_t sample_rate;
    uint8_t channels;
    uint32_t frame_len;
    uint32_t samples;  // mẫu / khung
} mp3_hdr_t;

static bool mp3_parse_hdr(const uint8_t *b, mp3_hdr_t *h)
{
    static const uint16_t br_v1[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static const uint16_t br_v2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
    static const uint32_t sr_v1[3] = {44100, 48000, 32000};
    if (b[0] != 0xFF || (b[1] & 0xE0) != 0xE0) {
        return false;
    }
    const int ver_bits = (b[1] >> 3) & 3;
    const int layer = (b[1] >> 1) & 3;
    const int br_idx = b[2] >> 4;
    const int sr_idx = (b[2] >> 2) & 3;
    if (ver_bits == 1 || layer != 1 || br_idx == 0 || br_idx == 15 || sr_idx == 3) {
        return false;  // chỉ Layer III, không nhận bitrate "free"
    }
    h->version = ver_bits == 3 ? 1 : (ver_bits == 2 ? 2 : 25);
    h->bitrate = (uint32_t)(h->version == 1 ? br_v1[br_idx] : br_v2[br_idx]) * 1000;
    h->sample_rate = sr_v1[sr_idx] / (h->version == 1 ? 1 : (h->version == 2 ? 2 : 4));
    h->channels = ((b[3] >> 6) == 3) ? 1 : 2;
    const uint32_t pad = (b[2] >> 1) & 1;
    h->samples = h->version == 1 ? 1152 : 576;
    h->frame_len = (h->samples / 8) * h->bitrate / h->sample_rate + pad;
    return h->frame_len >= 4;
}

// Tìm khung MP3 đầu tiên (có khung kế tiếp hợp lệ để tránh nhận nhầm byte rác) từ 'from'.
static int64_t mp3_find_frame(FILE *f, int64_t from, int64_t limit, mp3_hdr_t *out)
{
    uint8_t buf[4096];
    int64_t pos = from;
    while (pos < limit) {
        size_t n = read_at(f, pos, buf, sizeof(buf));
        if (n < 4) {
            return -1;
        }
        for (size_t i = 0; i + 4 <= n; ++i) {
            mp3_hdr_t h;
            if (buf[i] != 0xFF || !mp3_parse_hdr(buf + i, &h)) {
                continue;
            }
            uint8_t nb[4];
            mp3_hdr_t h2;
            if (read_at(f, pos + (int64_t)i + h.frame_len, nb, 4) == 4 && mp3_parse_hdr(nb, &h2) &&
                    h2.sample_rate == h.sample_rate && h2.version == h.version) {
                *out = h;
                return pos + (int64_t)i;
            }
        }
        pos += (int64_t)n - 3;
    }
    return -1;
}

static void mp3_read(FILE *f, hcc_audio_meta_t *m)
{
    const int64_t id3 = id3v2_size(f);
    mp3_hdr_t h;
    const int64_t first = mp3_find_frame(f, id3, id3 + 256 * 1024, &h);
    if (first < 0) {
        return;
    }
    m->audio_start = first;
    m->sample_rate = h.sample_rate;
    m->channels = h.channels;
    m->bits = 16;
    uint8_t tag[3];
    if (m->file_size > 128 && read_at(f, m->file_size - 128, tag, 3) == 3 && memcmp(tag, "TAG", 3) == 0) {
        m->audio_end = m->file_size - 128;
    }

    uint8_t fr[200];
    const size_t n = read_at(f, first, fr, sizeof(fr));
    const int side = h.version == 1 ? (h.channels == 1 ? 17 : 32) : (h.channels == 1 ? 9 : 17);
    uint32_t frames = 0;
    if (n >= (size_t)(4 + side + 8) &&
            (memcmp(fr + 4 + side, "Xing", 4) == 0 || memcmp(fr + 4 + side, "Info", 4) == 0)) {
        const uint8_t *x = fr + 4 + side;
        const uint32_t flags = be32(x + 4);
        size_t off = 8;
        if ((flags & 1) && n >= (size_t)(4 + side) + off + 4) {
            frames = be32(x + off);
            off += 4;
        }
        if (flags & 2) {
            off += 4;  // tổng số byte
        }
        if ((flags & 4) && n >= (size_t)(4 + side) + off + 100) {
            memcpy(m->toc, x + off, 100);
            m->has_toc = true;
        }
        // Khung Xing không chứa âm thanh: bỏ qua cả khung.
        m->audio_start = first + h.frame_len;
    } else if (n >= 4 + 32 + 18 && memcmp(fr + 4 + 32, "VBRI", 4) == 0) {
        frames = be32(fr + 4 + 32 + 14);
        m->audio_start = first + h.frame_len;
    }

    if (frames > 0) {
        m->duration_ms = (uint32_t)((uint64_t)frames * h.samples * 1000 / h.sample_rate);
        const int64_t bytes = m->audio_end - m->audio_start;
        if (m->duration_ms > 0 && bytes > 0) {
            m->bitrate = (uint32_t)((uint64_t)bytes * 8 * 1000 / m->duration_ms);
        }
    } else {
        hcc_audio_meta_update_bitrate(m, h.bitrate);
    }
    m->seekable = true;
}

/* ---------------------------------- AAC (ADTS) ---------------------------------- */

static void aac_read(FILE *f, hcc_audio_meta_t *m)
{
    static const uint32_t sr_tab[13] = {96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                        22050, 16000, 12000, 11025, 8000, 7350};
    int64_t pos = id3v2_size(f);
    uint8_t b[7];
    // Tìm khung ADTS đầu tiên.
    int64_t first = -1;
    for (int64_t p = pos; p < pos + 64 * 1024; ++p) {
        if (read_at(f, p, b, 7) != 7) {
            return;
        }
        if (b[0] == 0xFF && (b[1] & 0xF6) == 0xF0 && ((b[2] >> 2) & 0xF) < 13) {
            first = p;
            break;
        }
    }
    if (first < 0) {
        return;
    }
    m->audio_start = first;
    m->sample_rate = sr_tab[(b[2] >> 2) & 0xF];
    m->channels = (uint8_t)(((b[2] & 1) << 2) | (b[3] >> 6));
    m->bits = 16;
    // Bitrate trung bình của tối đa 200 khung đầu.
    uint64_t bytes = 0;
    uint32_t frames = 0;
    pos = first;
    while (frames < 200 && read_at(f, pos, b, 7) == 7 && b[0] == 0xFF && (b[1] & 0xF6) == 0xF0) {
        const uint32_t len = ((uint32_t)(b[3] & 3) << 11) | ((uint32_t)b[4] << 3) | (b[5] >> 5);
        if (len < 7) {
            break;
        }
        bytes += len;
        frames++;
        pos += len;
    }
    if (frames > 0 && m->sample_rate > 0) {
        hcc_audio_meta_update_bitrate(m, (uint32_t)(bytes * 8 * m->sample_rate / (1024ULL * frames)));
    }
    m->seekable = true;
}

/* ---------------------------------- WAV ---------------------------------- */

static void wav_read(FILE *f, hcc_audio_meta_t *m)
{
    uint8_t h[12];
    if (read_at(f, 0, h, 12) != 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0) {
        return;
    }
    int64_t pos = 12;
    bool have_fmt = false;
    while (pos + 8 <= m->file_size) {
        uint8_t c[8];
        if (read_at(f, pos, c, 8) != 8) {
            return;
        }
        const uint32_t size = le32(c + 4);
        if (memcmp(c, "fmt ", 4) == 0 && size >= 16) {
            uint8_t fm[16];
            if (read_at(f, pos + 8, fm, 16) != 16) {
                return;
            }
            m->wav_format = le16(fm);
            m->channels = (uint8_t)le16(fm + 2);
            m->sample_rate = le32(fm + 4);
            m->byte_rate = le32(fm + 8);
            m->block_align = le16(fm + 12);
            m->bits = (uint8_t)le16(fm + 14);
            have_fmt = true;
        } else if (memcmp(c, "data", 4) == 0) {
            m->audio_start = pos + 8;
            int64_t end = m->audio_start + size;
            if (size == 0 || size == 0xFFFFFFFFu || end > m->file_size) {
                end = m->file_size;
            }
            m->audio_end = end;
            break;
        }
        pos += 8 + (int64_t)size + (size & 1);
    }
    if (!have_fmt || m->byte_rate == 0 || m->block_align == 0 || m->audio_start == 0) {
        return;
    }
    m->bitrate = m->byte_rate * 8;
    m->duration_ms = (uint32_t)((uint64_t)(m->audio_end - m->audio_start) * 1000 / m->byte_rate);
    m->seekable = (m->wav_format == 1 || m->wav_format == 0xFFFE) &&
                  (m->bits == 16 || m->bits == 24 || m->bits == 32) && m->channels >= 1 && m->channels <= 2;
}

/* ---------------------------------- FLAC ---------------------------------- */

static void flac_read(FILE *f, hcc_audio_meta_t *m)
{
    const int64_t start = id3v2_size(f);
    uint8_t b[4 + 4 + 34];
    if (read_at(f, start, b, sizeof(b)) != sizeof(b) || memcmp(b, "fLaC", 4) != 0 || (b[4] & 0x7F) != 0) {
        return;
    }
    const uint8_t *si = b + 8;  // STREAMINFO
    m->sample_rate = ((uint32_t)si[10] << 12) | ((uint32_t)si[11] << 4) | (si[12] >> 4);
    m->channels = (uint8_t)(((si[12] >> 1) & 7) + 1);
    m->bits = (uint8_t)((((si[12] & 1) << 4) | (si[13] >> 4)) + 1);
    const uint64_t total = ((uint64_t)(si[13] & 0xF) << 32) | be32(si + 14);
    if (m->sample_rate > 0 && total > 0) {
        m->duration_ms = (uint32_t)(total * 1000 / m->sample_rate);
        m->bitrate = m->duration_ms ? (uint32_t)((uint64_t)(m->file_size - start) * 8000 / m->duration_ms) : 0;
    }
    m->audio_start = start;
}

/* ---------------------------------- M4A ---------------------------------- */

// Tìm atom 'type' trong khoảng [pos, end). Trả về vị trí phần thân (sau header), -1 nếu không có.
static int64_t mp4_find(FILE *f, int64_t pos, int64_t end, const char *type, int64_t *body_end)
{
    while (pos + 8 <= end) {
        uint8_t h[16];
        if (read_at(f, pos, h, 8) != 8) {
            return -1;
        }
        uint64_t size = be32(h);
        int64_t hdr = 8;
        if (size == 1) {
            if (read_at(f, pos + 8, h + 8, 8) != 8) {
                return -1;
            }
            size = be64(h + 8);
            hdr = 16;
        } else if (size == 0) {
            size = (uint64_t)(end - pos);
        }
        if (size < (uint64_t)hdr) {
            return -1;
        }
        if (memcmp(h + 4, type, 4) == 0) {
            *body_end = pos + (int64_t)size;
            return pos + hdr;
        }
        pos += (int64_t)size;
    }
    return -1;
}

static void m4a_read(FILE *f, hcc_audio_meta_t *m)
{
    int64_t moov_end = 0;
    const int64_t moov = mp4_find(f, 0, m->file_size, "moov", &moov_end);
    if (moov < 0) {
        return;
    }
    int64_t mvhd_end = 0;
    const int64_t mvhd = mp4_find(f, moov, moov_end, "mvhd", &mvhd_end);
    uint8_t b[32];
    if (mvhd < 0 || read_at(f, mvhd, b, sizeof(b)) != sizeof(b)) {
        return;
    }
    uint32_t timescale;
    uint64_t duration;
    if (b[0] == 1) {
        timescale = be32(b + 20);
        duration = be64(b + 24);
    } else {
        timescale = be32(b + 12);
        duration = be32(b + 16);
    }
    if (timescale > 0) {
        m->duration_ms = (uint32_t)(duration * 1000 / timescale);
        m->bitrate = m->duration_ms ? (uint32_t)((uint64_t)m->file_size * 8000 / m->duration_ms) : 0;
    }
}

/* ---------------------------------- chung ---------------------------------- */

void hcc_audio_meta_update_bitrate(hcc_audio_meta_t *m, uint32_t bitrate)
{
    if (m == NULL || bitrate == 0) {
        return;
    }
    if (m->fmt != HCC_AUDIO_FMT_MP3 && m->fmt != HCC_AUDIO_FMT_AAC) {
        return;
    }
    if (m->has_toc && m->duration_ms > 0) {
        return;  // MP3 VBR: thời lượng từ khung Xing đã chính xác
    }
    m->bitrate = bitrate;
    const int64_t bytes = m->audio_end - m->audio_start;
    if (bytes > 0) {
        m->duration_ms = (uint32_t)((uint64_t)bytes * 8 * 1000 / bitrate);
    }
}

bool hcc_audio_meta_read(const char *path, hcc_audio_meta_t *m)
{
    memset(m, 0, sizeof(*m));
    m->fmt = hcc_audio_fmt_from_name(path);
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }
    if (fseek(f, 0, SEEK_END) == 0) {
        m->file_size = ftell(f);
    }
    m->audio_end = m->file_size;
    switch (m->fmt) {
    case HCC_AUDIO_FMT_MP3: mp3_read(f, m); break;
    case HCC_AUDIO_FMT_AAC: aac_read(f, m); break;
    case HCC_AUDIO_FMT_WAV: wav_read(f, m); break;
    case HCC_AUDIO_FMT_FLAC: flac_read(f, m); break;
    case HCC_AUDIO_FMT_M4A: m4a_read(f, m); break;
    default: break;
    }
    fclose(f);
    if (m->audio_end <= m->audio_start) {
        m->seekable = false;
    }
    return true;
}

int64_t hcc_audio_meta_seek_offset(const hcc_audio_meta_t *m, uint32_t ms)
{
    const int64_t span = m->audio_end - m->audio_start;
    if (span <= 0) {
        return m->audio_start;
    }
    int64_t off;
    if (m->fmt == HCC_AUDIO_FMT_WAV && m->byte_rate > 0 && m->block_align > 0) {
        off = (int64_t)((uint64_t)ms * m->byte_rate / 1000);
        off -= off % m->block_align;
    } else if (m->has_toc && m->duration_ms > 0) {
        // MP3 VBR: nội suy bảng TOC (100 điểm phần trăm -> vị trí byte /256).
        double pct = (double)ms * 100.0 / m->duration_ms;
        if (pct < 0) pct = 0;
        if (pct > 99.999) pct = 99.999;
        const int i = (int)pct;
        const double a = m->toc[i];
        const double b = i < 99 ? m->toc[i + 1] : 256.0;
        off = (int64_t)((a + (b - a) * (pct - i)) / 256.0 * (double)span);
    } else if (m->duration_ms > 0) {
        off = (int64_t)((double)ms / m->duration_ms * (double)span);
    } else if (m->bitrate > 0) {
        off = (int64_t)((uint64_t)ms * m->bitrate / 8000);
    } else {
        off = 0;
    }
    if (off < 0) off = 0;
    if (off >= span) off = span - 1;
    return m->audio_start + off;
}
