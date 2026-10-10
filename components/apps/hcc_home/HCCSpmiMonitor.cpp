#include "HCCSpmiMonitor.hpp"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "HCCSpmiCapFile.hpp"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hcc_cores.h"
#include "nvs.h"

namespace HCCSpmiMonitor {

using HCCSpmiDecode::Txn;

namespace {

const char *TAG = "HCCSpmiMon";
constexpr uint32_t RAW_BYTES = 8 * 1024 * 1024;  // sóng thô: 32 triệu mẫu (0,4 s hoạt động liên tục @80 MHz)
constexpr uint32_t RAW_SEGS = 65536;

SemaphoreHandle_t s_mux;
HCCSpmiBuffers::PacketBuf s_pk;
HCCSpmiBuffers::RawStore s_raw;
bool s_mem_ok = false;

volatile bool s_running = false;
volatile bool s_stop_req = false;
Src s_src = Src::PARLIO;
HCCSpmiSource::Params s_params;
Snapshot s_pub;   // bản công bố cho giao diện (khoá s_mux)
int8_t s_hw_ok = -1;

volatile bool s_saving = false;
bool s_save_ok = false;
char s_save_msg[200] = "";

void lock(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
}
void unlock(void)
{
    xSemaphoreGive(s_mux);
}

HCCSpmiSource::Source *sourceOf(Src s)
{
    switch (s) {
    case Src::PARLIO: return HCCSpmiSource::parlio();
    case Src::DEMO: return HCCSpmiSource::demo();
    case Src::EXTERNAL: return HCCSpmiSource::external();
    case Src::FILE: return HCCSpmiSource::file();
    }
    return nullptr;
}

bool allocMem(void)
{
    if (s_mem_ok) {
        return true;
    }
    Txn *p = (Txn *)heap_caps_malloc(sizeof(Txn) * MAX_PACKETS, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p == nullptr) {
        return false;
    }
    s_pk.init(p, MAX_PACKETS);
    uint8_t *a = (uint8_t *)heap_caps_malloc(RAW_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    auto *sg = (HCCSpmiBuffers::Seg *)heap_caps_malloc(sizeof(HCCSpmiBuffers::Seg) * RAW_SEGS,
                                                       MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (a == nullptr || sg == nullptr) {
        heap_caps_free(a);
        heap_caps_free(sg);
        ESP_LOGW(TAG, "Không đủ PSRAM cho sóng thô - chỉ ghi gói");
        s_raw.init(nullptr, 0, nullptr, 0);
    } else {
        s_raw.init(a, RAW_BYTES, sg, RAW_SEGS);
    }
    s_mem_ok = true;
    return true;
}

// ==================== Feed: giải mã + ghi ====================

class Engine : public HCCSpmiSource::Feed {
public:
    void begin(Src src, uint32_t hz, const char *name, const char *file)
    {
        _live = Snapshot();
        snprintf(_live.source, sizeof(_live.source), "%s", name);
        snprintf(_live.file, sizeof(_live.file), "%s", file);
        _npk = 0;
        _file_dropped = 0;
        _live.src = src;
        _live.sample_hz = hz;
        _live.running = true;
        _live.replay = src == Src::FILE;
        _live.demo = src == Src::DEMO;
        _pos = 0;
        _dpos = 0;
        _orient = 0;
        _edges[0] = _edges[1] = 0;
        _prev = -1;
        _t0 = esp_timer_get_time();
        _last_act = _t0;
        _last_pub = 0;
        _last_falls = 0;
        _dec.reset(idleSamples(hz), &Engine::sinkCb, this);
        _dec.swap = false;
    }

    static uint32_t idleSamples(uint32_t hz)
    {
        const uint32_t n = hz / 500000;  // ~2 µs bus đứng yên = hết chuỗi
        return n < 32 ? 32 : n;
    }

    void samples(const uint8_t *d, size_t n) override
    {
        if (n == 0) {
            return;
        }
        s_raw.append(_pos, d, (uint32_t)n);
        if (_live.lines_known) {
            _dec.feed(d, n);
            _dpos = _pos + (uint64_t)n * 4;
        } else {
            detect(d, n);
        }
        _pos += (uint64_t)n * 4;
        afterChunk();
    }

    void idle(uint64_t n) override
    {
        if (_live.lines_known) {
            _dec.idle(n);
            _dpos = _pos + n;
        }
        _pos += n;
        afterChunk();
    }

    void lost(uint64_t n) override
    {
        ++_live.lost_chunks;
        if (_live.lines_known) {
            _dec.lost(n);
            _dpos = _pos + n;
        }
        _pos += n;
        afterChunk();
    }

    void packet(const Txn &t) override { sink(t); }

    HCCSpmiBuffers::RawStore &rawStore() override { return s_raw; }

    void setSampleHz(uint32_t hz) override
    {
        _live.sample_hz = hz ? hz : 1;
        _dec.reset(idleSamples(_live.sample_hz), &Engine::sinkCb, this);
        _dec.swap = _orient == 2;
    }

    void setOrientation(int o) override
    {
        _orient = o;
        if (o != 0) {
            _live.lines_known = true;
            _live.swapped = o == 2;
            _dec.swap = o == 2;
        }
    }

    void setDemo(bool demo) override { _live.demo = demo; }

    void setFlags(uint32_t f, uint32_t lost_chunks, uint32_t dropped) override
    {
        if (f & HCCSpmiCapFile::CF_DEMO) {
            _live.demo = true;
        }
        _live.lost_chunks += lost_chunks;
        _file_dropped = dropped;
    }

    bool stopRequested() override { return s_stop_req; }

    void message(const char *m) override
    {
        snprintf(_live.msg, sizeof(_live.msg), "%s", m);
        publish(true);
    }

    void finish()
    {
        if (_live.lines_known) {
            _dec.flush();
        }
        s_raw.setEnd(_pos);
        _live.running = false;
        publish(true);
    }

    void publish(bool force)
    {
        const int64_t now = esp_timer_get_time();
        if (!force && now - _last_pub < 50000) {
            return;
        }
        _last_pub = now;
        _live.elapsed_ms = (uint32_t)((now - _t0) / 1000);
        _live.frames = s_pk.count();
        _live.dropped_pkts = s_pk.dropped() + _file_dropped;
        _live.total_samples = _pos;
        _live.raw_truncated = s_raw.truncated();
        if (_live.lines_known && _dec.sclkHighSamples() > 0 && _dec.sclkFalls() > _npk) {
            _live.sclk_mhz = (float)((double)_live.sample_hz * (double)(_dec.sclkFalls() - _npk) /
                                     (double)_dec.sclkHighSamples() / 1e6);
            // Dưới 4 mẫu / chu kỳ SCLK: cạnh có thể bị bỏ sót -> giải mã không tin cậy.
            _live.undersampled = !_live.replay && _live.sclk_mhz * 4e6f > (float)_live.sample_hz;
        }
        lock();
        s_pub = _live;
        s_pub.state = _live.running ? (_live.replay ? State::REPLAY : liveState(now)) : State::STOPPED;
        unlock();
    }

    int64_t lastActivity() const { return _last_act; }

private:
    State liveState(int64_t now) const
    {
        if (_live.lost_chunks || _live.dropped_pkts) {
            return State::OVERFLOW;
        }
        return now - _last_act > 1000000 ? State::NO_SIGNAL : State::CAPTURING;
    }

    static void sinkCb(const Txn &t, void *u) { ((Engine *)u)->sink(t); }

    void sink(const Txn &t)
    {
        ++_npk;
        s_pk.push(t);
        const HCCSpmiDecode::PktStatus st = HCCSpmiDecode::status(t);
        switch (st) {
        case HCCSpmiDecode::ST_OK: break;
        case HCCSpmiDecode::ST_PARITY: ++_live.parity; break;
        case HCCSpmiDecode::ST_SHORT: ++_live.short_frames; break;
        case HCCSpmiDecode::ST_BAD_CMD: ++_live.bad_cmd; break;
        case HCCSpmiDecode::ST_UNKNOWN_CMD: ++_live.unknown_cmd; break;
        }
        if (st != HCCSpmiDecode::ST_OK && st != HCCSpmiDecode::ST_UNKNOWN_CMD) {
            ++_live.errors;
        }
        if (st != HCCSpmiDecode::ST_BAD_CMD) {
            ++_live.sid_count[t.sid & 0xF];
            _live.reads += HCCSpmiDecode::isRead(t.kind) ? 1 : 0;
            _live.writes += HCCSpmiDecode::isWrite(t.kind) ? 1 : 0;
        }
    }

    // Chưa biết dây nào là SCLK: đếm số lần đổi mức (SCLK đổi ~2 lần nhiều hơn SDATA). Đủ cạnh thì giải mã lại
    // phần đã lưu trong RawStore để không mất các gói đầu.
    void detect(const uint8_t *d, size_t n)
    {
        for (size_t i = 0; i < n; ++i) {
            uint8_t v = d[i];
            if (v == 0 && _prev == 0) {
                continue;
            }
            for (int k = 0; k < 4; ++k) {
                const int s = v & 3;
                v >>= 2;
                if (_prev >= 0) {
                    _edges[0] += (s ^ _prev) & 1;
                    _edges[1] += ((s ^ _prev) >> 1) & 1;
                }
                _prev = s;
            }
        }
        if (_edges[0] + _edges[1] < 400) {
            if (_edges[0] + _edges[1] > 0) {
                _last_act = esp_timer_get_time();
            }
            return;
        }
        _live.lines_known = true;
        _live.swapped = _edges[1] > _edges[0];
        _dec.swap = _live.swapped;
        ESP_LOGI(TAG, "Nhận vai dây: SCLK = dây %d (đổi mức %u / %u)", _live.swapped ? 1 : 0, (unsigned)_edges[0],
                 (unsigned)_edges[1]);
        uint64_t p = 0;
        const uint64_t end = _pos + (uint64_t)n * 4;
        if (!s_raw.truncated()) {
            for (uint32_t i = 0; i < s_raw.segCount(); ++i) {
                const HCCSpmiBuffers::Seg sg = s_raw.seg(i);
                if (sg.t0 > p) {
                    _dec.idle(sg.t0 - p);
                }
                _dec.feed(s_raw.bytes(sg), sg.nbytes);
                p = sg.t0 + (uint64_t)sg.nbytes * 4;
            }
        }
        if (end > p) {
            _dec.idle(end - p);
        }
        _dpos = end;
    }

    void afterChunk()
    {
        const uint64_t f = _dec.sclkFalls();
        if (f != _last_falls) {
            _last_falls = f;
            _last_act = esp_timer_get_time();
        }
        publish(false);
    }

    Snapshot _live;
    HCCSpmiDecode::Decoder _dec;
    uint64_t _pos = 0, _dpos = 0;
    int _orient = 0;
    uint32_t _edges[2] = {0, 0};
    int _prev = -1;
    int64_t _t0 = 0, _last_act = 0, _last_pub = 0;
    uint64_t _last_falls = 0;
    uint32_t _npk = 0;
    uint32_t _file_dropped = 0;
};

Engine s_eng;

void captureTask(void *)
{
    HCCSpmiSource::Source *src = sourceOf(s_src);
    char err[200] = "";
    const bool ok = src->run(s_params, s_eng, err, sizeof(err));
    s_eng.finish();
    lock();
    Snapshot &p = s_pub;
    if (!ok) {
        snprintf(p.msg, sizeof(p.msg), "%s", err[0] ? err : "Lỗi nguồn dữ liệu");
        p.state = State::ERROR;
    } else {
        if (!p.replay && !p.lines_known && p.frames == 0) {
            snprintf(p.msg, sizeof(p.msg),
                     "NO SIGNAL: không thấy hoạt động trên SCLK / SDATA (dây đo, mạch chuyển mức, main đã cấp nguồn?)");
        } else if (s_pk.full()) {
            snprintf(p.msg, sizeof(p.msg), "Bộ đệm gói đầy (%u gói) - đã dừng, %u gói bị bỏ", (unsigned)s_pk.capacity(),
                     (unsigned)p.dropped_pkts);
        } else if (p.msg[0] == '\0' || !p.replay) {
            snprintf(p.msg, sizeof(p.msg), "Đã dừng: %u gói, %u lỗi", (unsigned)p.frames, (unsigned)p.errors);
        }
        if (p.lost_chunks || p.dropped_pkts) {
            p.state = State::OVERFLOW;
        }
    }
    unlock();
    ESP_LOGI(TAG, "Dừng thu: %u gói, mất %u chunk", (unsigned)s_pk.count(), (unsigned)s_pub.lost_chunks);
    s_running = false;
    vTaskDelete(nullptr);
}

void ensureInit(void)
{
    if (s_mux == nullptr) {
        s_mux = xSemaphoreCreateMutex();
    }
}

void saveTask(void *)
{
    Snapshot sn = snapshot();
    char base[96] = "";
    char path[112] = "";
    char err[200] = "";
    bool ok = false;
    mkdir(BSP_SD_MOUNT_POINT "/spmi", 0775);
    for (int n = 1; n < 1000; ++n) {
        snprintf(base, sizeof(base), BSP_SD_MOUNT_POINT "/spmi/mon_%03d%s", n, sn.demo ? "_DEMO" : "");
        snprintf(path, sizeof(path), "%s.spmc", base);
        struct stat st;
        if (stat(path, &st) != 0) {
            break;
        }
    }
    HCCSpmiCapFile::Info info;
    uint32_t fl = 0;
    fl |= sn.demo ? (uint32_t)HCCSpmiCapFile::CF_DEMO : 0u;
    fl |= sn.swapped ? (uint32_t)HCCSpmiCapFile::CF_SWAPPED : 0u;
    fl |= sn.dropped_pkts ? (uint32_t)HCCSpmiCapFile::CF_PKT_TRUNC : 0u;
    fl |= sn.lost_chunks ? (uint32_t)HCCSpmiCapFile::CF_OVERFLOW : 0u;
    info.flags = fl;
    info.sample_hz = sn.sample_hz;
    info.total_samples = sn.total_samples;
    info.lost_chunks = sn.lost_chunks;
    info.dropped_pkts = sn.dropped_pkts;
    snprintf(info.source, sizeof(info.source), "%s", sn.source);
    ok = HCCSpmiCapFile::save(path, info, s_pk, &s_raw, err, sizeof(err));
    if (ok) {
        snprintf(path, sizeof(path), "%s.csv", base);
        ok = HCCSpmiCapFile::exportCsv(path, info, s_pk, err, sizeof(err));
    }
    if (ok) {
        snprintf(s_save_msg, sizeof(s_save_msg), "Đã lưu %u gói: %s.spmc / .csv%s", (unsigned)s_pk.count(),
                 base + strlen(BSP_SD_MOUNT_POINT) + 1, sn.demo ? " (DEMO)" : "");
    } else {
        snprintf(s_save_msg, sizeof(s_save_msg), "%s", err);
    }
    s_save_ok = ok;
    s_saving = false;
    vTaskDelete(nullptr);
}

}  // namespace

const char *stateName(State s)
{
    switch (s) {
    case State::IDLE: return "IDLE";
    case State::CAPTURING: return "CAPTURING";
    case State::NO_SIGNAL: return "NO SIGNAL";
    case State::STOPPED: return "STOPPED";
    case State::OVERFLOW: return "OVERFLOW";
    case State::HW_NOT_SUPPORTED: return "HARDWARE NOT SUPPORTED";
    case State::REPLAY: return "REPLAY";
    case State::ERROR: return "ERROR";
    }
    return "?";
}

bool start(Src src, const HCCSpmiSource::Params &p_in, char *err, size_t es)
{
    ensureInit();
    if (s_running || s_saving) {
        snprintf(err, es, "Đang bận");
        return false;
    }
    HCCSpmiSource::Params p = p_in;
    p.hw_confirmed = hwConfirmed();
    HCCSpmiSource::Source *source = sourceOf(src);
    char why[320] = "";
    if (!source->supported(p, why, sizeof(why))) {
        lock();
        s_pub.state = State::HW_NOT_SUPPORTED;
        s_pub.src = src;
        s_pub.sample_hz = p.sample_hz;
        snprintf(s_pub.source, sizeof(s_pub.source), "%s", source->name());
        s_pub.running = false;
        snprintf(s_pub.hw_why, sizeof(s_pub.hw_why), "%s", why);
        snprintf(s_pub.msg, sizeof(s_pub.msg), "%s", why);
        unlock();
        snprintf(err, es, "%s", why);
        return false;
    }
    if (!allocMem()) {
        snprintf(err, es, "Hết PSRAM cho bộ đệm gói");
        return false;
    }
    s_pk.clear();
    s_raw.clear();
    s_src = src;
    s_params = p;
    const char *b = strrchr(p.file, '/');
    s_eng.begin(src, src == Src::FILE ? 1 : p.sample_hz, source->name(), src == Src::FILE ? (b ? b + 1 : p.file) : "");
    lock();
    s_pub = Snapshot();
    unlock();
    s_eng.publish(true);
    s_stop_req = false;
    s_running = true;
    if (xTaskCreatePinnedToCore(captureTask, "hcc_spmi_mon", 8192, nullptr, 6, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_running = false;
        lock();
        s_pub.running = false;
        s_pub.state = State::ERROR;
        unlock();
        snprintf(err, es, "Không tạo được task thu");
        return false;
    }
    return true;
}

void stop(void)
{
    s_stop_req = true;
}

bool running(void)
{
    return s_running;
}

void clear(void)
{
    ensureInit();
    if (s_running || s_saving) {
        return;
    }
    s_pk.clear();
    s_raw.clear();
    lock();
    const Src src = s_pub.src;
    s_pub = Snapshot();
    s_pub.src = src;
    unlock();
}

Snapshot snapshot(void)
{
    ensureInit();
    lock();
    Snapshot s = s_pub;
    unlock();
    return s;
}

const HCCSpmiBuffers::PacketBuf &packets(void)
{
    return s_pk;
}

const HCCSpmiBuffers::RawStore &raw(void)
{
    return s_raw;
}

uint32_t sampleHz(void)
{
    ensureInit();
    lock();
    const uint32_t hz = s_pub.sample_hz;
    unlock();
    return hz;
}

bool hwConfirmed(void)
{
    if (s_hw_ok < 0) {
        s_hw_ok = 0;
        nvs_handle_t h;
        if (nvs_open("hcc_spmi", NVS_READONLY, &h) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(h, "hw_ok", &v) == ESP_OK) {
                s_hw_ok = v ? 1 : 0;
            }
            nvs_close(h);
        }
    }
    return s_hw_ok == 1;
}

void setHwConfirmed(bool ok)
{
    s_hw_ok = ok ? 1 : 0;
    nvs_handle_t h;
    if (nvs_open("hcc_spmi", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "hw_ok", ok ? 1 : 0);
        nvs_commit(h);
        nvs_close(h);
    }
}

bool startSave(void)
{
    ensureInit();
    if (s_running || s_saving || s_pk.count() == 0) {
        return false;
    }
    s_saving = true;
    s_save_msg[0] = '\0';
    if (xTaskCreatePinnedToCore(saveTask, "hcc_spmi_save", 6144, nullptr, 4, nullptr, HCC_CORE_WORK) != pdPASS) {
        s_saving = false;
        return false;
    }
    return true;
}

bool saving(void)
{
    return s_saving;
}

bool saveResult(char *msg, size_t size)
{
    snprintf(msg, size, "%s", s_save_msg);
    return s_save_ok;
}

}  // namespace HCCSpmiMonitor
