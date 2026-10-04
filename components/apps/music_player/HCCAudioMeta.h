/*
 * HCCAudioMeta: đọc nhanh thông tin file nhạc (định dạng, thời lượng, vị trí dữ liệu âm thanh)
 * để Music hiện đúng tổng thời gian và tua (seek) được. Chỉ đọc header, không giải mã.
 *
 * Thời lượng:
 *   MP3  - khung Xing/Info hoặc VBRI (chính xác), nếu không có thì ước theo bitrate CBR.
 *   WAV  - kích thước khối data / byte rate.
 *   FLAC - STREAMINFO (tổng số mẫu / tần số).
 *   M4A  - moov/mvhd (duration / timescale).
 *   AAC  - (ADTS) ước theo bitrate trung bình của các khung đầu file.
 * Tua được: MP3, AAC (ADTS), WAV PCM - nhảy theo byte rồi bộ giải mã tự bắt lại khung.
 * FLAC / M4A / OGG: chưa tua được (cần bảng chỉ mục của container).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    HCC_AUDIO_FMT_UNKNOWN = 0,
    HCC_AUDIO_FMT_MP3,
    HCC_AUDIO_FMT_AAC,
    HCC_AUDIO_FMT_FLAC,
    HCC_AUDIO_FMT_WAV,
    HCC_AUDIO_FMT_M4A,
    HCC_AUDIO_FMT_OGG,
} hcc_audio_fmt_t;

typedef struct {
    hcc_audio_fmt_t fmt;
    uint32_t duration_ms;   // 0 = chưa biết
    uint32_t sample_rate;   // 0 = chưa biết (bộ giải mã sẽ báo)
    uint8_t  channels;
    uint8_t  bits;
    uint32_t bitrate;       // bit/s, 0 = chưa biết
    int64_t  file_size;
    int64_t  audio_start;   // byte đầu tiên của dữ liệu âm thanh (sau ID3v2 / header WAV)
    int64_t  audio_end;     // byte sau cùng (trừ thẻ ID3v1 "TAG" cuối file MP3)
    bool     seekable;
    bool     has_toc;       // MP3 VBR có bảng TOC 100 điểm trong khung Xing
    uint8_t  toc[100];
    // WAV PCM (đọc thẳng, không qua bộ giải mã)
    uint16_t wav_format;    // 1 = PCM, 0xFFFE = extensible
    uint16_t block_align;
    uint32_t byte_rate;
} hcc_audio_meta_t;

hcc_audio_fmt_t hcc_audio_fmt_from_name(const char *name);
const char *hcc_audio_fmt_name(hcc_audio_fmt_t fmt);

// false nếu không mở / không đọc được file. Định dạng lấy theo đuôi file, rồi kiểm tra nội dung.
bool hcc_audio_meta_read(const char *path, hcc_audio_meta_t *meta);

// Vị trí byte trong file ứng với thời điểm ms (chỉ dùng khi meta->seekable). Luôn trong [audio_start, audio_end).
int64_t hcc_audio_meta_seek_offset(const hcc_audio_meta_t *meta, uint32_t ms);

// Cập nhật thời lượng MP3/AAC khi bộ giải mã báo bitrate thật (file CBR không có khung Xing).
void hcc_audio_meta_update_bitrate(hcc_audio_meta_t *meta, uint32_t bitrate);

#ifdef __cplusplus
}
#endif
