/*
 * XiaozhiImageAvatar: xem XiaozhiImageAvatar.hpp.
 *
 * Bộ nhớ: mỗi khung w*h*3 byte (370x520 ~ 577 KB), tối đa 7 khung ~ 4 MB PSRAM. Khi nạp lại (file đổi), bộ
 * mới được dựng song song rồi mới thay bộ cũ trên luồng LVGL, nên lúc đó tạm dùng gấp đôi.
 *
 * Luồng: create() -> (luồng LVGL) chụp chữ ký bộ đang có -> task "xz_avatar" (nhân WORK) kiểm tra thẻ SD,
 * so chữ ký, giải mã nếu cần -> đặt kết quả -> tick() trên luồng LVGL nhận kết quả, đổi bộ khung, hiện ảnh.
 */
#include "XiaozhiImageAvatar.hpp"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <mutex>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "XiaozhiPng.h"
#include "hcc_cores.h"
#include "hcc_home/HCCUsbDisk.hpp"

namespace {

const char *TAG = "XzAvatarImg";

#define AVATAR_DIR BSP_SD_MOUNT_POINT "/xiaozhi/avatar"

enum FrameId : int { F_BASE, F_BLINK, F_TALK1, F_TALK2, F_SMILE, F_SAD, F_SURPRISED, F_COUNT };
const char *const FRAME_NAMES[F_COUNT] = {"base", "blink", "talk1", "talk2", "smile", "sad", "surprised"};

struct FileSig {
    bool exists;
    int64_t size;
    int64_t mtime;
};

struct FrameSet {
    uint8_t *data[F_COUNT];
    lv_img_dsc_t dsc[F_COUNT];
    FileSig sig[F_COUNT];
    int w;
    int h;
};

enum class LoadState : uint8_t { Idle, Loading, Done };
enum class LoadResult : uint8_t { Reuse, NewSet, NoBase };

std::mutex s_mu;
LoadState s_state = LoadState::Idle;     // bảo vệ bởi s_mu
LoadResult s_result = LoadResult::NoBase;
FrameSet *s_pending = nullptr;           // bộ mới do task nạp dựng, chờ luồng LVGL nhận

// Chỉ luồng LVGL dùng.
FrameSet s_active = {};
bool s_active_valid = false;
bool s_adopted = false;                  // đã nhận kết quả của lần nạp gần nhất

struct LoadArgs {
    int w;
    int h;
    bool have_active;
    FileSig active_sig[F_COUNT];
    int active_w;
    int active_h;
};

// Chỉ giải phóng bộ đệm. Bộ đã từng gắn vào LVGL phải được bỏ khỏi cache ảnh của LVGL trước (trên luồng LVGL),
// vì cache tra theo địa chỉ lv_img_dsc_t, không theo dữ liệu.
void freeSet(FrameSet &set)
{
    for (int i = 0; i < F_COUNT; ++i) {
        if (set.data[i] != nullptr) {
            xz_png_free(set.data[i]);
            set.data[i] = nullptr;
        }
    }
}

// Luồng LVGL: bỏ các khung của s_active khỏi cache ảnh LVGL (địa chỉ dsc sẽ được dùng lại cho bộ mới).
void invalidateActive(void)
{
    for (int i = 0; i < F_COUNT; ++i) {
        lv_img_cache_invalidate_src(&s_active.dsc[i]);
    }
}

FileSig statFile(int id)
{
    char path[96];
    snprintf(path, sizeof(path), AVATAR_DIR "/%s.png", FRAME_NAMES[id]);
    struct stat st;
    FileSig sig = {};
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
        sig.exists = true;
        sig.size = st.st_size;
        sig.mtime = st.st_mtime;
    }
    return sig;
}

void finish(LoadResult r, FrameSet *set)
{
    std::lock_guard<std::mutex> lk(s_mu);
    s_result = r;
    s_pending = set;
    s_state = LoadState::Done;
}

void loadTask(void *arg)
{
    LoadArgs a = *static_cast<LoadArgs *>(arg);
    delete static_cast<LoadArgs *>(arg);
    const bool same_size = a.have_active && a.active_w == a.w && a.active_h == a.h;

    // Thẻ đang giao cho máy tính (USB DISK): không được mount song song. Có cache thì dùng tiếp.
    if (HCCUsbDisk::state() != HCCUsbDisk::State::Off) {
        ESP_LOGW(TAG, "USB DISK đang bật - không đọc thẻ SD");
        finish(same_size ? LoadResult::Reuse : LoadResult::NoBase, nullptr);
        vTaskDelete(nullptr);
        return;
    }
    const esp_err_t mount = bsp_sdcard_mount();
    if (mount != ESP_OK && mount != ESP_ERR_INVALID_STATE) {
        ESP_LOGI(TAG, "Không có thẻ SD (%s)", esp_err_to_name(mount));
        finish(same_size ? LoadResult::Reuse : LoadResult::NoBase, nullptr);
        vTaskDelete(nullptr);
        return;
    }

    FileSig sig[F_COUNT];
    for (int i = 0; i < F_COUNT; ++i) {
        sig[i] = statFile(i);
    }
    if (!sig[F_BASE].exists) {
        ESP_LOGI(TAG, "Không có %s/base.png - dùng avatar hoạt hình", AVATAR_DIR);
        finish(LoadResult::NoBase, nullptr);
        vTaskDelete(nullptr);
        return;
    }
    if (same_size) {
        bool unchanged = true;
        for (int i = 0; i < F_COUNT && unchanged; ++i) {
            unchanged = sig[i].exists == a.active_sig[i].exists && sig[i].size == a.active_sig[i].size &&
                        sig[i].mtime == a.active_sig[i].mtime;
        }
        if (unchanged) {
            ESP_LOGI(TAG, "Ảnh không đổi - dùng cache trong PSRAM");
            finish(LoadResult::Reuse, nullptr);
            vTaskDelete(nullptr);
            return;
        }
    }

    auto *set = static_cast<FrameSet *>(heap_caps_calloc(1, sizeof(FrameSet), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (set == nullptr) {
        finish(same_size ? LoadResult::Reuse : LoadResult::NoBase, nullptr);
        vTaskDelete(nullptr);
        return;
    }
    set->w = a.w;
    set->h = a.h;
    int base_w = 0;
    int base_h = 0;
    for (int i = 0; i < F_COUNT; ++i) {
        set->sig[i] = sig[i];
        if (!sig[i].exists) {
            continue;
        }
        char path[96];
        snprintf(path, sizeof(path), AVATAR_DIR "/%s.png", FRAME_NAMES[i]);
        if (i != F_BASE) {
            int w = 0;
            int h = 0;
            if (!xz_png_size(path, &w, &h) || w != base_w || h != base_h) {
                ESP_LOGW(TAG, "Bỏ %s.png: kích thước %dx%d khác base.png %dx%d (các khung phải chồng khít)",
                         FRAME_NAMES[i], w, h, base_w, base_h);
                set->sig[i].exists = false;
                continue;
            }
        }
        const int64_t t0 = esp_timer_get_time();
        char err[96] = "";
        int sw = 0;
        int sh = 0;
        uint8_t *px = xz_png_load_cover(path, a.w, a.h, &sw, &sh, err, sizeof(err));
        if (px == nullptr) {
            ESP_LOGW(TAG, "Bỏ %s.png: %s", FRAME_NAMES[i], err);
            set->sig[i].exists = false;
            if (i == F_BASE) {
                break;
            }
            continue;
        }
        if (i == F_BASE) {
            base_w = sw;
            base_h = sh;
        }
        set->data[i] = px;
        lv_img_dsc_t &d = set->dsc[i];
        memset(&d, 0, sizeof(d));
        d.header.cf = LV_IMG_CF_TRUE_COLOR_ALPHA;
        d.header.w = a.w;
        d.header.h = a.h;
        d.data_size = (uint32_t)a.w * a.h * LV_IMG_PX_SIZE_ALPHA_BYTE;
        d.data = px;
        ESP_LOGI(TAG, "Nạp %s.png %dx%d -> %dx%d trong %d ms", FRAME_NAMES[i], sw, sh, a.w, a.h,
                 (int)((esp_timer_get_time() - t0) / 1000));
    }
    if (set->data[F_BASE] == nullptr) {
        ESP_LOGW(TAG, "base.png không dùng được - dùng avatar hoạt hình");
        freeSet(*set);
        heap_caps_free(set);
        finish(LoadResult::NoBase, nullptr);
    } else {
        finish(LoadResult::NewSet, set);
    }
    vTaskDelete(nullptr);
}

// Gọi trên luồng LVGL: bắt đầu nạp nếu chưa có task nào đang chạy.
void startLoad(int w, int h)
{
    {
        std::lock_guard<std::mutex> lk(s_mu);
        if (s_state == LoadState::Loading) {
            return;  // lần mở trước còn đang nạp: chờ kết quả đó
        }
        if (s_state == LoadState::Done && !s_adopted) {
            return;  // có kết quả chưa nhận
        }
        s_state = LoadState::Loading;
    }
    s_adopted = false;
    auto *a = new LoadArgs();
    a->w = w;
    a->h = h;
    a->have_active = s_active_valid;
    a->active_w = s_active.w;
    a->active_h = s_active.h;
    memcpy(a->active_sig, s_active.sig, sizeof(a->active_sig));
    // libpng + zlib: stack ~8 KB là đủ (bộ đệm dòng / zlib nằm trong heap).
    if (xTaskCreatePinnedToCore(loadTask, "xz_avatar", 10 * 1024, a, 3, nullptr, HCC_CORE_WORK) != pdPASS) {
        delete a;
        std::lock_guard<std::mutex> lk(s_mu);
        s_result = LoadResult::NoBase;
        s_state = LoadState::Done;
    }
}

uint32_t randRange(uint32_t a, uint32_t b)
{
    return a + esp_random() % (b - a + 1);
}

}  // namespace

lv_obj_t *XiaozhiImageAvatar::create(lv_obj_t *parent, int w, int h)
{
    _w = w;
    _h = h;
    _shown = false;
    _cur_frame = -1;
    _box = lv_obj_create(parent);
    lv_obj_remove_style_all(_box);
    lv_obj_set_size(_box, w, h);
    lv_obj_center(_box);
    lv_obj_clear_flag(_box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(_box, LV_OBJ_FLAG_CLICKABLE);
    // Viền sáng khi đang nghe: border vẽ đè lên ảnh (border_post), độ đậm theo giọng người nói.
    lv_obj_set_style_border_width(_box, 4, 0);
    lv_obj_set_style_border_color(_box, lv_color_hex(0x3DD66E), 0);
    lv_obj_set_style_border_opa(_box, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_post(_box, true, 0);
    lv_obj_set_style_radius(_box, 10, 0);
    lv_obj_add_flag(_box, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(_box, onDelete, LV_EVENT_DELETE, this);

    _img = lv_img_create(_box);
    lv_obj_set_size(_img, w, h);
    lv_obj_center(_img);
    lv_img_set_antialias(_img, true);
    lv_img_set_pivot(_img, w / 2, h / 2);  // thở: phóng to / thu nhỏ quanh tâm, không dịch chuyển ảnh

    _t0 = lv_tick_get();
    _next_blink = _t0 + randRange(1500, 3500);
    _timer = lv_timer_create(onTick, 40, this);
    startLoad(w, h);
    return _box;
}

void XiaozhiImageAvatar::detach(void)
{
    _box = nullptr;
    _img = nullptr;
    _timer = nullptr;  // Brookesia thu dọn timer cùng app
    _shown = false;
}

void XiaozhiImageAvatar::onDelete(lv_event_t *e)
{
    auto *self = static_cast<XiaozhiImageAvatar *>(lv_event_get_user_data(e));
    if (self != nullptr) {
        self->_box = nullptr;
        self->_img = nullptr;
        self->_shown = false;
    }
}

void XiaozhiImageAvatar::onTick(lv_timer_t *t)
{
    auto *self = static_cast<XiaozhiImageAvatar *>(t->user_data);
    if (self != nullptr && self->_box != nullptr) {
        self->tick();
    }
}

bool XiaozhiImageAvatar::unavailable(void) const
{
    if (_shown) {
        return false;
    }
    std::lock_guard<std::mutex> lk(s_mu);
    return s_adopted && !s_active_valid;
}

void XiaozhiImageAvatar::setEmotion(const char *emotion)
{
    _emotion_frame = -1;
    if (emotion == nullptr) {
        return;
    }
    static const char *const SMILE[] = {"happy", "laughing", "funny", "loving", "winking", "confident",
                                        "relaxed", "delicious", "kissy", "silly"};
    static const char *const SAD[] = {"sad", "crying", "embarrassed", "confused"};
    static const char *const SURPRISED[] = {"surprised", "shocked"};
    for (const char *e : SMILE) {
        if (strcmp(e, emotion) == 0) _emotion_frame = F_SMILE;
    }
    for (const char *e : SAD) {
        if (strcmp(e, emotion) == 0) _emotion_frame = F_SAD;
    }
    for (const char *e : SURPRISED) {
        if (strcmp(e, emotion) == 0) _emotion_frame = F_SURPRISED;
    }
}

int XiaozhiImageAvatar::pickFrame(void)
{
    const uint32_t now = lv_tick_get();
    auto has = [](int id) { return s_active.data[id] != nullptr; };

    // Miệng: ngưỡng có trễ (lên 0.08 / 0.30, xuống 0.05 / 0.22), giữ mỗi trạng thái tối thiểu 80 ms.
    int want = 0;
    if (_mode == Mode::Speaking) {
        const float up1 = 0.08f, up2 = 0.30f, dn1 = 0.05f, dn2 = 0.22f;
        if (_mouth == 2) {
            want = _speak > dn2 ? 2 : (_speak > dn1 ? 1 : 0);
        } else if (_mouth == 1) {
            want = _speak > up2 ? 2 : (_speak > dn1 ? 1 : 0);
        } else {
            want = _speak > up2 ? 2 : (_speak > up1 ? 1 : 0);
        }
    }
    if (want != _mouth && now - _mouth_since >= 80) {
        _mouth = want;
        _mouth_since = now;
    }
    int talk = -1;
    if (_mouth == 1) {
        talk = has(F_TALK1) ? F_TALK1 : (has(F_TALK2) ? F_TALK2 : -1);
    } else if (_mouth == 2) {
        talk = has(F_TALK2) ? F_TALK2 : (has(F_TALK1) ? F_TALK1 : -1);
    }
    if (talk >= 0) {
        return talk;
    }

    // Nét mặt lúc không mấp máy môi.
    int rest = F_BASE;
    if (_mode == Mode::Sad && has(F_SAD)) {
        rest = F_SAD;
    } else if (_emotion_frame >= 0 && has(_emotion_frame)) {
        rest = _emotion_frame;
    }

    // Chớp mắt: chỉ trên base (blink.png vẽ từ base).
    if (rest == F_BASE && has(F_BLINK)) {
        if (now >= _next_blink) {
            _blink_until = now + 130;
            _next_blink = (esp_random() % 100 < 15) ? now + 350 : now + randRange(2500, 6000);
        }
        if (now < _blink_until) {
            return F_BLINK;
        }
    }
    return rest;
}

void XiaozhiImageAvatar::tick(void)
{
    // Nhận kết quả nạp (nếu có).
    LoadState st;
    LoadResult res;
    FrameSet *pending = nullptr;
    {
        std::lock_guard<std::mutex> lk(s_mu);
        st = s_state;
        res = s_result;
        if (st == LoadState::Done && !s_adopted) {
            pending = s_pending;
            s_pending = nullptr;
        }
    }
    if (st == LoadState::Done && !s_adopted) {
        if (res == LoadResult::NewSet && pending != nullptr) {
            if (_img != nullptr) {
                lv_img_set_src(_img, nullptr);
            }
            invalidateActive();
            FrameSet old = s_active;
            const bool had_old = s_active_valid;
            s_active = *pending;
            s_active_valid = true;
            heap_caps_free(pending);
            if (_img != nullptr) {
                lv_img_set_src(_img, &s_active.dsc[F_BASE]);  // bỏ tham chiếu khung cũ trước khi giải phóng
                _cur_frame = F_BASE;
            }
            if (had_old) {
                freeSet(old);
            }
        } else if (res == LoadResult::NoBase && s_active_valid) {
            if (_img != nullptr) {
                lv_img_set_src(_img, nullptr);
            }
            invalidateActive();
            freeSet(s_active);
            memset(&s_active, 0, sizeof(s_active));
            s_active_valid = false;
            _cur_frame = -1;
        }
        std::lock_guard<std::mutex> lk(s_mu);
        s_adopted = true;
    }

    if (_box == nullptr || _img == nullptr) {
        return;
    }
    if (!s_active_valid || s_active.w != _w || s_active.h != _h) {
        if (_shown) {
            lv_obj_add_flag(_box, LV_OBJ_FLAG_HIDDEN);
            _shown = false;
        }
        return;
    }
    if (!_shown) {
        lv_obj_clear_flag(_box, LV_OBJ_FLAG_HIDDEN);
        _shown = true;
        _cur_frame = -1;
    }

    const float k = _speak_target > _speak ? 0.7f : 0.4f;
    _speak += (_speak_target - _speak) * k;
    _mic += (_mic_target - _mic) * 0.4f;

    const int frame = pickFrame();
    if (frame != _cur_frame && s_active.data[frame] != nullptr) {
        lv_img_set_src(_img, &s_active.dsc[frame]);
        _cur_frame = frame;
    }

    // Thở: chỉ zoom rất nhẹ quanh tâm, cập nhật ~10 lần/giây (phép biến đổi ảnh khá nặng).
    const uint32_t now = lv_tick_get();
    if (now - _last_zoom_ms >= 100) {
        _last_zoom_ms = now;
        // 256..258 (tối đa ~0.8%), chu kỳ 4 s: mỗi bậc zoom chỉ làm mép ảnh nhích ~1 px.
        const float ph = (float)((now - _t0) % 4000) / 4000.0f;
        const uint16_t z = (uint16_t)(256 + lroundf(1.0f - cosf(6.2831853f * ph)));
        if (z != _zoom) {
            _zoom = z;
            lv_img_set_zoom(_img, z);
        }
    }

    // Viền sáng khi nghe.
    const lv_opa_t want_glow = _mode == Mode::Listening ? (lv_opa_t)(90 + 165 * fminf(1.0f, _mic * 1.5f)) : 0;
    if (abs((int)want_glow - (int)_glow) > 6) {
        _glow = (lv_opa_t)((_glow + want_glow) / 2);
        if (want_glow == 0 && _glow < 12) {
            _glow = 0;
        }
        lv_obj_set_style_border_opa(_box, _glow, 0);
    }
}
