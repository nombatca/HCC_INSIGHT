#include "HCCNews.hpp"

#include <deque>
#include <stdio.h>
#include <string.h>
#include <string>
#include <vector>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hcc_cores.h"
#include "news/HCCNewsNet.hpp"
#include "news/HCCNewsParse.hpp"

LV_IMG_DECLARE(img_app_news);

using HCCNewsNet::Image;
using HCCNewsParse::Block;
using HCCNewsParse::Item;

namespace {

const char *TAG = "HCCNews";

// ==================== Nguồn tin ====================

struct Feed {
    std::string name;
    std::string url;
};

struct Source {
    std::string name;
    std::vector<Feed> feeds;
};

std::vector<Source> builtinSources(void)
{
    return {
        {"VnExpress",
         {{"Tin mới", "https://vnexpress.net/rss/tin-moi-nhat.rss"},
          {"Thời sự", "https://vnexpress.net/rss/thoi-su.rss"},
          {"Thế giới", "https://vnexpress.net/rss/the-gioi.rss"},
          {"Kinh doanh", "https://vnexpress.net/rss/kinh-doanh.rss"},
          {"Thể thao", "https://vnexpress.net/rss/the-thao.rss"},
          {"Số hóa", "https://vnexpress.net/rss/so-hoa.rss"},
          {"Giải trí", "https://vnexpress.net/rss/giai-tri.rss"}}},
        {"Tuổi Trẻ",
         {{"Tin mới", "https://tuoitre.vn/rss/tin-moi-nhat.rss"},
          {"Thời sự", "https://tuoitre.vn/rss/thoi-su.rss"},
          {"Thế giới", "https://tuoitre.vn/rss/the-gioi.rss"},
          {"Kinh doanh", "https://tuoitre.vn/rss/kinh-doanh.rss"},
          {"Thể thao", "https://tuoitre.vn/rss/the-thao.rss"},
          {"Công nghệ", "https://tuoitre.vn/rss/nhip-song-so.rss"},
          {"Giải trí", "https://tuoitre.vn/rss/giai-tri.rss"}}},
        {"Thanh Niên",
         {{"Tin mới", "https://thanhnien.vn/rss/home.rss"},
          {"Thời sự", "https://thanhnien.vn/rss/thoi-su.rss"},
          {"Thế giới", "https://thanhnien.vn/rss/the-gioi.rss"},
          {"Kinh tế", "https://thanhnien.vn/rss/kinh-te.rss"},
          {"Thể thao", "https://thanhnien.vn/rss/the-thao.rss"},
          {"Công nghệ", "https://thanhnien.vn/rss/cong-nghe.rss"},
          {"Giải trí", "https://thanhnien.vn/rss/giai-tri.rss"}}},
        {"Dân Trí",
         {{"Tin mới", "https://dantri.com.vn/rss/home.rss"},
          {"Xã hội", "https://dantri.com.vn/rss/xa-hoi.rss"},
          {"Thế giới", "https://dantri.com.vn/rss/the-gioi.rss"},
          {"Kinh doanh", "https://dantri.com.vn/rss/kinh-doanh.rss"},
          {"Thể thao", "https://dantri.com.vn/rss/the-thao.rss"},
          {"Công nghệ", "https://dantri.com.vn/rss/cong-nghe.rss"},
          {"Giải trí", "https://dantri.com.vn/rss/giai-tri.rss"}}},
    };
}

// /sdcard/news/feeds.txt: "Tên|URL" mỗi dòng -> nguồn "Thẻ SD".
void loadSdSource(std::vector<Source> &out)
{
    FILE *f = fopen("/sdcard/news/feeds.txt", "r");
    if (f == nullptr) {
        return;
    }
    Source s;
    s.name = "Thẻ SD";
    char line[512];
    while (fgets(line, sizeof(line), f) && s.feeds.size() < 30) {
        std::string l(line);
        while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) {
            l.pop_back();
        }
        if (l.empty() || l[0] == '#') {
            continue;
        }
        const size_t bar = l.find('|');
        Feed fd;
        if (bar == std::string::npos) {
            fd.name = l;
            fd.url = l;
        } else {
            fd.name = l.substr(0, bar);
            fd.url = l.substr(bar + 1);
        }
        if (fd.url.compare(0, 4, "http") == 0) {
            s.feeds.push_back(fd);
        }
    }
    fclose(f);
    if (!s.feeds.empty()) {
        out.push_back(s);
    }
}

// ==================== Hàng đợi tải nền ====================

enum JobKind { J_FEED, J_ARTICLE, J_IMAGE_THUMB, J_IMAGE_ART };

struct Job {
    JobKind kind;
    uint32_t gen;
    int id;
    std::string url;
    int w;
    int h;
};

struct Result {
    JobKind kind;
    uint32_t gen;
    int id;
    bool ok;
    std::string err;
    std::vector<Item> items;
    std::vector<Block> blocks;
    Image *img = nullptr;
};

SemaphoreHandle_t s_mux;
SemaphoreHandle_t s_sem;
std::deque<Job> s_jobs;
std::deque<Result *> s_results;
volatile uint32_t s_feed_gen = 1;  // danh sách tin + ảnh nhỏ
volatile uint32_t s_art_gen = 1;   // bài đang đọc + ảnh trong bài

constexpr size_t FEED_MAX = 2 * 1024 * 1024;
constexpr size_t PAGE_MAX = 3 * 1024 * 1024;
constexpr size_t IMG_MAX = 4 * 1024 * 1024;

bool stale(const Job &j)
{
    return (j.kind == J_FEED || j.kind == J_IMAGE_THUMB) ? j.gen != s_feed_gen : j.gen != s_art_gen;
}

int prio(JobKind k)
{
    return k == J_ARTICLE ? 3 : k == J_FEED ? 2 : k == J_IMAGE_ART ? 1 : 0;
}

void pushJob(const Job &j)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    s_jobs.push_back(j);
    xSemaphoreGive(s_mux);
    xSemaphoreGive(s_sem);
}

void workerTask(void *)
{
    char err[96];
    while (true) {
        xSemaphoreTake(s_sem, portMAX_DELAY);
        xSemaphoreTake(s_mux, portMAX_DELAY);
        // Bỏ việc đã lỗi thời, lấy việc ưu tiên cao nhất (giữ thứ tự trong cùng mức).
        for (auto it = s_jobs.begin(); it != s_jobs.end();) {
            it = stale(*it) ? s_jobs.erase(it) : it + 1;
        }
        if (s_jobs.empty()) {
            xSemaphoreGive(s_mux);
            continue;
        }
        auto best = s_jobs.begin();
        for (auto it = s_jobs.begin(); it != s_jobs.end(); ++it) {
            if (prio(it->kind) > prio(best->kind)) {
                best = it;
            }
        }
        Job job = *best;
        s_jobs.erase(best);
        const bool more = !s_jobs.empty();
        xSemaphoreGive(s_mux);
        if (more) {
            xSemaphoreGive(s_sem);  // còn việc: vòng sau lấy tiếp
        }

        Result *r = new Result();
        r->kind = job.kind;
        r->gen = job.gen;
        r->id = job.id;
        err[0] = '\0';
        HCCNewsNet::Buffer buf;
        const size_t max = job.kind == J_FEED ? FEED_MAX : job.kind == J_ARTICLE ? PAGE_MAX : IMG_MAX;
        r->ok = HCCNewsNet::httpGet(job.url, max, buf, err, sizeof(err));
        if (r->ok && !stale(job)) {
            if (job.kind == J_FEED) {
                r->items = HCCNewsParse::parseRss((const char *)buf.data, buf.len);
                if (r->items.empty()) {
                    r->ok = false;
                    snprintf(err, sizeof(err), "Không đọc được tin (không phải RSS?)");
                }
            } else if (job.kind == J_ARTICLE) {
                r->blocks = HCCNewsParse::parseArticle((const char *)buf.data, buf.len, buf.final_url);
                if (r->blocks.empty()) {
                    r->ok = false;
                    snprintf(err, sizeof(err), "Không tách được nội dung bài");
                }
            } else {
                r->img = HCCNewsNet::decodeImage(buf.data, buf.len, job.w, job.h, job.kind == J_IMAGE_THUMB, err,
                                                 sizeof(err));
                r->ok = r->img != nullptr;
            }
        }
        buf.release();
        r->err = err;
        xSemaphoreTake(s_mux, portMAX_DELAY);
        s_results.push_back(r);
        xSemaphoreGive(s_mux);
    }
}

void startWorker(void)
{
    if (s_mux != nullptr) {
        return;
    }
    s_mux = xSemaphoreCreateMutex();
    s_sem = xSemaphoreCreateCounting(1000, 0);
    // TLS cần ngăn xếp lớn.
    xTaskCreatePinnedToCore(workerTask, "hcc_news", 12288, nullptr, 4, nullptr, HCC_CORE_WORK);
}

Result *popResult(void)
{
    xSemaphoreTake(s_mux, portMAX_DELAY);
    Result *r = nullptr;
    if (!s_results.empty()) {
        r = s_results.front();
        s_results.pop_front();
    }
    xSemaphoreGive(s_mux);
    return r;
}

void freeResult(Result *r)
{
    HCCNewsNet::freeImage(r->img);
    delete r;
}

// ==================== Giao diện ====================

const lv_color_t C_BG = lv_color_hex(0x101820);
const lv_color_t C_CARD = lv_color_hex(0x1B2733);
const lv_color_t C_CARD_PR = lv_color_hex(0x27394A);
const lv_color_t C_THUMB = lv_color_hex(0x26333F);
const lv_color_t C_TEXT = lv_color_hex(0xE8EEF2);
const lv_color_t C_MUTED = lv_color_hex(0x93A4B1);
const lv_color_t C_ACCENT = lv_color_hex(0x35C2E0);
const lv_color_t C_WARN = lv_color_hex(0xF0A04B);

#if LV_FONT_MONTSERRAT_20
#define SYMBOL_FONT (&lv_font_montserrat_20)
#else
#define SYMBOL_FONT LV_FONT_DEFAULT
#endif

constexpr int TOP_H = 64;
constexpr int THUMB_W = 200;
constexpr int THUMB_H = 112;
constexpr int ART_IMG_MAX = 24;  // số ảnh tối đa tải trong một bài

struct Ui {
    lv_obj_t *root = nullptr;
    lv_obj_t *src_bar = nullptr;   // nút nguồn + chuyên mục (chế độ danh sách)
    lv_obj_t *src_btns = nullptr;
    lv_obj_t *cat_dd = nullptr;
    lv_obj_t *btn_back = nullptr;  // "< Tin" (chế độ đọc bài)
    lv_obj_t *art_src = nullptr;
    lv_obj_t *status = nullptr;
    lv_obj_t *list = nullptr;
    lv_obj_t *article = nullptr;
    lv_timer_t *timer = nullptr;
    int w = 1024;
    int h = 527;

    std::vector<Source> sources;
    int src = 0;
    int cat = 0;
    std::vector<Item> items;
    std::vector<lv_obj_t *> thumb_objs;
    std::vector<Image *> thumbs;
    bool feed_loading = false;

    int art_item = -1;
    std::vector<lv_obj_t *> art_boxes;  // khung ảnh trong bài
    std::vector<Image *> art_imgs;
    lv_obj_t *art_loading = nullptr;
    int art_pending = 0;  // ảnh trong bài chưa tải xong
};

Ui s_ui;
std::string s_src_map_store;
std::vector<const char *> s_src_map;

void setStatus(const char *t, lv_color_t c)
{
    if (s_ui.status) {
        lv_label_set_text(s_ui.status, t);
        lv_obj_set_style_text_color(s_ui.status, c, 0);
    }
}

lv_obj_t *makeLabel(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

// "Tue, 07 Oct 2026 10:15:00 +0700" / "2026-10-07T10:15:00+07:00" -> "10:15  07/10/2026"
std::string shortDate(const std::string &d)
{
    static const char *const MON[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int day = 0, year = 0, hh = 0, mm = 0, mon = 0;
    char mname[8] = "";
    const char *p = strchr(d.c_str(), ',');
    p = p ? p + 1 : d.c_str();
    if (sscanf(p, "%d %3s %d %d:%d", &day, mname, &year, &hh, &mm) == 5) {
        for (int i = 0; i < 12; ++i) {
            if (strcasecmp(mname, MON[i]) == 0) {
                mon = i + 1;
            }
        }
    } else if (sscanf(d.c_str(), "%d-%d-%dT%d:%d", &year, &mon, &day, &hh, &mm) != 5) {
        return d;
    }
    if (mon == 0) {
        return d;
    }
    char b[40];
    snprintf(b, sizeof(b), "%02d:%02d  %02d/%02d/%d", hh, mm, day, mon, year);
    return b;
}

void freeThumbs(void)
{
    for (Image *i : s_ui.thumbs) {
        HCCNewsNet::freeImage(i);
    }
    s_ui.thumbs.clear();
    s_ui.thumb_objs.clear();
}

void freeArticleImages(void)
{
    for (Image *i : s_ui.art_imgs) {
        HCCNewsNet::freeImage(i);
    }
    s_ui.art_imgs.clear();
    s_ui.art_boxes.clear();
    s_ui.art_pending = 0;
}

void onCardClicked(lv_event_t *e);

void buildList(void)
{
    lv_obj_clean(s_ui.list);
    freeThumbs();
    lv_img_cache_invalidate_src(nullptr);
    const int card_w = s_ui.w - 40;
    const int text_w = card_w - THUMB_W - 44;
    s_ui.thumbs.assign(s_ui.items.size(), nullptr);
    s_ui.thumb_objs.assign(s_ui.items.size(), nullptr);
    for (size_t i = 0; i < s_ui.items.size(); ++i) {
        const Item &it = s_ui.items[i];
        lv_obj_t *card = lv_obj_create(s_ui.list);
        lv_obj_set_size(card, card_w, THUMB_H + 20);
        lv_obj_set_style_bg_color(card, C_CARD, 0);
        lv_obj_set_style_bg_color(card, C_CARD_PR, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(card, 0, 0);
        lv_obj_set_style_radius(card, 12, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(card, onCardClicked, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *img = lv_img_create(card);
        lv_obj_set_size(img, THUMB_W, THUMB_H);
        lv_obj_align(img, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(img, C_THUMB, 0);
        lv_obj_set_style_bg_opa(img, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(img, 8, 0);
        lv_obj_set_style_clip_corner(img, true, 0);
        s_ui.thumb_objs[i] = img;

        lv_obj_t *title = makeLabel(card, it.title.c_str(), &lv_font_be_vietnam_20, C_TEXT);
        lv_obj_set_size(title, text_w, 60);  // 2 dòng
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        lv_obj_align(title, LV_ALIGN_TOP_LEFT, THUMB_W + 16, -4);

        lv_obj_t *sum = makeLabel(card, it.summary.c_str(), &lv_font_be_vietnam_16, C_MUTED);
        lv_obj_set_size(sum, text_w, 22);  // 1 dòng
        lv_label_set_long_mode(sum, LV_LABEL_LONG_DOT);
        lv_obj_align(sum, LV_ALIGN_TOP_LEFT, THUMB_W + 16, 60);

        lv_obj_t *date = makeLabel(card, shortDate(it.date).c_str(), &lv_font_be_vietnam_14, C_ACCENT);
        lv_obj_align(date, LV_ALIGN_BOTTOM_LEFT, THUMB_W + 16, 2);

        if (!it.image.empty()) {
            pushJob({J_IMAGE_THUMB, s_feed_gen, (int)i, it.image, THUMB_W, THUMB_H});
        }
    }
    lv_obj_scroll_to_y(s_ui.list, 0, LV_ANIM_OFF);
}

void loadFeed(void)
{
    s_feed_gen = s_feed_gen + 1;
    s_ui.items.clear();
    lv_obj_clean(s_ui.list);
    freeThumbs();
    if (s_ui.sources.empty() || s_ui.src >= (int)s_ui.sources.size()) {
        return;
    }
    const Source &src = s_ui.sources[s_ui.src];
    if (s_ui.cat >= (int)src.feeds.size()) {
        s_ui.cat = 0;
    }
    if (!HCCNewsNet::wifiConnected()) {
        setStatus("Chưa kết nối Wi-Fi", C_WARN);
        lv_obj_t *l = makeLabel(s_ui.list, "Chưa có Wi-Fi. Vào Cài đặt để kết nối mạng rồi bấm nút tải lại.",
                                &lv_font_be_vietnam_20, C_MUTED);
        lv_obj_set_width(l, s_ui.w - 60);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        return;
    }
    s_ui.feed_loading = true;
    char b[96];
    snprintf(b, sizeof(b), "Đang tải %s...", src.feeds[s_ui.cat].name.c_str());
    setStatus(b, C_ACCENT);
    pushJob({J_FEED, s_feed_gen, 0, src.feeds[s_ui.cat].url, 0, 0});
}

void updateCategories(void)
{
    std::string opts;
    if (s_ui.src < (int)s_ui.sources.size()) {
        for (const Feed &f : s_ui.sources[s_ui.src].feeds) {
            if (!opts.empty()) {
                opts += '\n';
            }
            opts += f.name;
        }
    }
    lv_dropdown_set_options(s_ui.cat_dd, opts.c_str());
    lv_dropdown_set_selected(s_ui.cat_dd, s_ui.cat);
}

void closeArticle(void)
{
    s_art_gen = s_art_gen + 1;
    if (s_ui.article) {
        lv_obj_del(s_ui.article);
        s_ui.article = nullptr;
    }
    s_ui.art_loading = nullptr;
    freeArticleImages();
    lv_img_cache_invalidate_src(nullptr);
    s_ui.art_item = -1;
    if (s_ui.list) {
        lv_obj_clear_flag(s_ui.list, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_ui.src_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.btn_back, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_ui.art_src, LV_OBJ_FLAG_HIDDEN);
    }
    setStatus(s_ui.items.empty() ? "" : (std::to_string(s_ui.items.size()) + " tin").c_str(), C_MUTED);
}

void openArticle(int idx)
{
    if (idx < 0 || idx >= (int)s_ui.items.size()) {
        return;
    }
    closeArticle();
    s_ui.art_item = idx;
    const Item &it = s_ui.items[idx];
    lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_ui.src_bar, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.btn_back, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_ui.art_src, LV_OBJ_FLAG_HIDDEN);
    const Source &src = s_ui.sources[s_ui.src];
    lv_label_set_text_fmt(s_ui.art_src, "%s - %s", src.name.c_str(), src.feeds[s_ui.cat].name.c_str());

    lv_obj_t *a = lv_obj_create(s_ui.root);
    s_ui.article = a;
    lv_obj_remove_style_all(a);
    lv_obj_set_size(a, s_ui.w, s_ui.h - TOP_H);
    lv_obj_set_pos(a, 0, TOP_H);
    lv_obj_set_style_pad_hor(a, 40, 0);
    lv_obj_set_style_pad_ver(a, 16, 0);
    lv_obj_set_style_pad_row(a, 16, 0);
    lv_obj_set_flex_flow(a, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(a, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(a, LV_DIR_VER);
    lv_obj_add_flag(a, LV_OBJ_FLAG_SCROLLABLE);

    const int tw = s_ui.w - 80;
    lv_obj_t *t = makeLabel(a, it.title.c_str(), &lv_font_be_vietnam_30, C_TEXT);
    lv_obj_set_width(t, tw);
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_t *d = makeLabel(a, shortDate(it.date).c_str(), &lv_font_be_vietnam_16, C_ACCENT);
    lv_obj_set_width(d, tw);
    if (!it.summary.empty()) {
        lv_obj_t *s = makeLabel(a, it.summary.c_str(), &lv_font_be_vietnam_22, lv_color_hex(0xC9D6DF));
        lv_obj_set_width(s, tw);
        lv_label_set_long_mode(s, LV_LABEL_LONG_WRAP);
    }
    s_ui.art_loading = makeLabel(a, "Đang tải bài...", &lv_font_be_vietnam_20, C_MUTED);
    lv_obj_set_width(s_ui.art_loading, tw);
    lv_label_set_long_mode(s_ui.art_loading, LV_LABEL_LONG_WRAP);
    setStatus("Đang tải bài...", C_ACCENT);
    pushJob({J_ARTICLE, s_art_gen, idx, it.link, 0, 0});
}

void renderArticle(const std::vector<Block> &blocks)
{
    lv_obj_t *a = s_ui.article;
    if (s_ui.art_loading) {
        lv_obj_del(s_ui.art_loading);
        s_ui.art_loading = nullptr;
    }
    const int tw = s_ui.w - 80;
    const std::string &summary = s_ui.items[s_ui.art_item].summary;
    int images = 0;
    for (const Block &b : blocks) {
        if (b.type == Block::Image) {
            if (images >= ART_IMG_MAX) {
                continue;
            }
            lv_obj_t *box = lv_obj_create(a);
            lv_obj_remove_style_all(box);
            lv_obj_set_size(box, tw, 220);
            lv_obj_set_style_bg_color(box, C_THUMB, 0);
            lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(box, 10, 0);
            lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_t *l = makeLabel(box, "Đang tải ảnh...", &lv_font_be_vietnam_16, C_MUTED);
            lv_obj_center(l);
            s_ui.art_boxes.push_back(box);
            s_ui.art_imgs.push_back(nullptr);
            pushJob({J_IMAGE_ART, s_art_gen, images, b.url, tw, 640});
            ++s_ui.art_pending;
            ++images;
            continue;
        }
        if (b.type == Block::Para && b.text == summary) {
            continue;  // tóm tắt đã hiện ở đầu bài
        }
        const lv_font_t *font = &lv_font_be_vietnam_22;
        lv_color_t color = C_TEXT;
        if (b.type == Block::Heading) {
            font = &lv_font_be_vietnam_24;
            color = C_ACCENT;
        } else if (b.type == Block::Caption) {
            font = &lv_font_be_vietnam_16;
            color = C_MUTED;
        }
        lv_obj_t *l = makeLabel(a, b.text.c_str(), font, color);
        lv_obj_set_width(l, tw);
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_line_space(l, 4, 0);
        if (b.type == Block::Caption) {
            lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        }
    }
    lv_obj_t *end = makeLabel(a, "- Hết bài -", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_style_pad_bottom(end, 24, 0);
    setStatus(images ? "Đang tải ảnh..." : "", C_MUTED);
}

void applyResult(Result *r)
{
    switch (r->kind) {
    case J_FEED:
        if (r->gen != s_feed_gen) {
            break;
        }
        s_ui.feed_loading = false;
        if (!r->ok) {
            setStatus("Lỗi tải tin", C_WARN);
            lv_obj_clean(s_ui.list);
            std::string msg = "Không tải được tin: " + r->err + "\nBấm nút tải lại để thử lần nữa.";
            lv_obj_t *l = makeLabel(s_ui.list, msg.c_str(), &lv_font_be_vietnam_20, C_WARN);
            lv_obj_set_width(l, s_ui.w - 60);
            lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
            break;
        }
        s_ui.items = std::move(r->items);
        buildList();
        if (s_ui.art_item < 0) {
            setStatus((std::to_string(s_ui.items.size()) + " tin").c_str(), C_MUTED);
        }
        break;
    case J_IMAGE_THUMB:
        if (r->gen != s_feed_gen || !r->ok || r->id < 0 || r->id >= (int)s_ui.thumb_objs.size()) {
            break;
        }
        s_ui.thumbs[r->id] = r->img;
        lv_img_set_src(s_ui.thumb_objs[r->id], &r->img->dsc);
        r->img = nullptr;  // thuộc về danh sách
        break;
    case J_ARTICLE:
        if (r->gen != s_art_gen || s_ui.article == nullptr) {
            break;
        }
        if (!r->ok) {
            if (s_ui.art_loading) {
                std::string msg = "Không tải được bài: " + r->err;
                lv_label_set_text(s_ui.art_loading, msg.c_str());
                lv_obj_set_style_text_color(s_ui.art_loading, C_WARN, 0);
            }
            setStatus("Lỗi tải bài", C_WARN);
            break;
        }
        renderArticle(r->blocks);
        break;
    case J_IMAGE_ART: {
        if (r->gen != s_art_gen || r->id < 0 || r->id >= (int)s_ui.art_boxes.size()) {
            break;
        }
        lv_obj_t *box = s_ui.art_boxes[r->id];
        lv_obj_clean(box);
        if (!r->ok) {
            lv_obj_set_height(box, 44);
            std::string msg = "[Ảnh không hiển thị được: " + r->err + "]";
            lv_obj_t *l = makeLabel(box, msg.c_str(), &lv_font_be_vietnam_14, C_MUTED);
            lv_obj_center(l);
        } else {
            s_ui.art_imgs[r->id] = r->img;
            lv_obj_set_size(box, r->img->dsc.header.w, r->img->dsc.header.h);
            lv_obj_set_style_bg_opa(box, LV_OPA_TRANSP, 0);
            lv_obj_t *img = lv_img_create(box);
            lv_img_set_src(img, &r->img->dsc);
            lv_obj_center(img);
            r->img = nullptr;
        }
        if (--s_ui.art_pending <= 0) {
            s_ui.art_pending = 0;
            setStatus("", C_MUTED);
        }
        break;
    }
    }
}

void onTimer(lv_timer_t *)
{
    for (int n = 0; n < 4; ++n) {  // mỗi nhịp áp tối đa 4 kết quả để giao diện mượt
        Result *r = popResult();
        if (r == nullptr) {
            break;
        }
        if (s_ui.root != nullptr) {
            applyResult(r);
        }
        freeResult(r);
    }
}

void onCardClicked(lv_event_t *e)
{
    openArticle((int)(intptr_t)lv_event_get_user_data(e));
}

void onBackClicked(lv_event_t *)
{
    closeArticle();
}

void onSourceChanged(lv_event_t *e)
{
    lv_obj_t *bm = lv_event_get_target(e);
    const uint16_t sel = lv_btnmatrix_get_selected_btn(bm);
    if (sel == LV_BTNMATRIX_BTN_NONE || (int)sel == s_ui.src) {
        return;
    }
    s_ui.src = sel;
    s_ui.cat = 0;
    updateCategories();
    loadFeed();
}

void onCategoryChanged(lv_event_t *e)
{
    s_ui.cat = lv_dropdown_get_selected(lv_event_get_target(e));
    loadFeed();
}

void onRefreshClicked(lv_event_t *)
{
    if (s_ui.article) {
        openArticle(s_ui.art_item);
    } else {
        loadFeed();
    }
}

void onRootDeleted(lv_event_t *)
{
    s_feed_gen = s_feed_gen + 1;
    s_art_gen = s_art_gen + 1;
    if (s_ui.timer) {
        lv_timer_del(s_ui.timer);
    }
    freeThumbs();
    freeArticleImages();
    lv_img_cache_invalidate_src(nullptr);
    Ui fresh;
    fresh.src = s_ui.src;  // nhớ nguồn / chuyên mục cho lần mở sau
    fresh.cat = s_ui.cat;
    s_ui = fresh;
}

void buildUi(lv_obj_t *screen)
{
    lv_obj_set_style_bg_color(screen, C_BG, 0);
    lv_obj_t *root = lv_obj_create(screen);
    s_ui.root = root;
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(root, C_BG, 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_clear_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(root, onRootDeleted, LV_EVENT_DELETE, nullptr);
    lv_obj_update_layout(root);
    s_ui.w = lv_obj_get_width(root);
    s_ui.h = lv_obj_get_height(root);
    if (s_ui.w < 600 || s_ui.w > 1280) {
        s_ui.w = 1024;
    }
    if (s_ui.h < 360 || s_ui.h > 800) {
        s_ui.h = 527;
    }

    // Thanh trên, chế độ danh sách: nguồn báo + chuyên mục.
    s_ui.src_bar = lv_obj_create(root);
    lv_obj_remove_style_all(s_ui.src_bar);
    lv_obj_set_size(s_ui.src_bar, s_ui.w - 260, TOP_H);
    lv_obj_set_pos(s_ui.src_bar, 0, 0);
    lv_obj_clear_flag(s_ui.src_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_src_map_store.clear();
    s_src_map.clear();
    for (const Source &s : s_ui.sources) {
        s_src_map_store += s.name;
        s_src_map_store.push_back('\0');
    }
    const char *p = s_src_map_store.c_str();
    for (size_t i = 0; i < s_ui.sources.size(); ++i) {
        s_src_map.push_back(p);
        p += strlen(p) + 1;
    }
    s_src_map.push_back("");
    lv_obj_t *bm = lv_btnmatrix_create(s_ui.src_bar);
    s_ui.src_btns = bm;
    lv_btnmatrix_set_map(bm, s_src_map.data());
    lv_btnmatrix_set_btn_ctrl_all(bm, LV_BTNMATRIX_CTRL_CHECKABLE);
    lv_btnmatrix_set_one_checked(bm, true);
    if (s_ui.src >= (int)s_ui.sources.size()) {
        s_ui.src = 0;
    }
    lv_btnmatrix_set_btn_ctrl(bm, s_ui.src, LV_BTNMATRIX_CTRL_CHECKED);
    const int n_src = s_ui.sources.empty() ? 1 : (int)s_ui.sources.size();
    int bw = (s_ui.w - 262 - 12 - 180 - 20) / n_src;  // chừa chỗ cho chuyên mục + trạng thái + tải lại
    if (bw > 130) {
        bw = 130;
    }
    lv_obj_set_size(bm, bw * n_src, 52);
    lv_obj_set_pos(bm, 12, 6);
    lv_obj_set_style_pad_all(bm, 4, 0);
    lv_obj_set_style_pad_gap(bm, 6, 0);
    lv_obj_set_style_bg_opa(bm, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(bm, 0, 0);
    lv_obj_set_style_text_font(bm, &lv_font_be_vietnam_18, 0);
    lv_obj_set_style_bg_color(bm, C_CARD, LV_PART_ITEMS);
    lv_obj_set_style_text_color(bm, C_TEXT, LV_PART_ITEMS);
    lv_obj_set_style_bg_color(bm, C_ACCENT, (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_text_color(bm, lv_color_hex(0x0B1A22), (lv_style_selector_t)LV_PART_ITEMS | LV_STATE_CHECKED);
    lv_obj_set_style_radius(bm, 10, LV_PART_ITEMS);
    lv_obj_add_event_cb(bm, onSourceChanged, LV_EVENT_VALUE_CHANGED, nullptr);

    s_ui.cat_dd = lv_dropdown_create(s_ui.src_bar);
    lv_obj_set_size(s_ui.cat_dd, 170, 48);
    lv_obj_align_to(s_ui.cat_dd, bm, LV_ALIGN_OUT_RIGHT_MID, 10, 0);
    lv_obj_set_style_text_font(s_ui.cat_dd, &lv_font_be_vietnam_18, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(s_ui.cat_dd), &lv_font_be_vietnam_20, 0);
    lv_obj_set_style_bg_color(s_ui.cat_dd, C_CARD, 0);
    lv_obj_set_style_text_color(s_ui.cat_dd, C_TEXT, 0);
    lv_obj_set_style_border_width(s_ui.cat_dd, 0, 0);
    lv_obj_add_event_cb(s_ui.cat_dd, onCategoryChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    updateCategories();

    // Thanh trên, chế độ đọc bài.
    s_ui.btn_back = lv_btn_create(root);
    lv_obj_set_size(s_ui.btn_back, 150, 48);
    lv_obj_set_pos(s_ui.btn_back, 16, 8);
    lv_obj_set_style_bg_color(s_ui.btn_back, C_CARD, 0);
    lv_obj_t *bl = makeLabel(s_ui.btn_back, LV_SYMBOL_LEFT "  Tin", &lv_font_be_vietnam_20, C_TEXT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(s_ui.btn_back, onBackClicked, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(s_ui.btn_back, LV_OBJ_FLAG_HIDDEN);
    s_ui.art_src = makeLabel(root, "", &lv_font_be_vietnam_20, C_MUTED);
    lv_obj_set_width(s_ui.art_src, s_ui.w - 186 - 280);
    lv_label_set_long_mode(s_ui.art_src, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(s_ui.art_src, 186, 20);
    lv_obj_add_flag(s_ui.art_src, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *rf = lv_btn_create(root);
    lv_obj_set_size(rf, 56, 48);
    lv_obj_set_pos(rf, s_ui.w - 72, 8);
    lv_obj_set_style_bg_color(rf, C_CARD, 0);
    lv_obj_t *rl = makeLabel(rf, LV_SYMBOL_REFRESH, SYMBOL_FONT, C_TEXT);
    lv_obj_center(rl);
    lv_obj_add_event_cb(rf, onRefreshClicked, LV_EVENT_CLICKED, nullptr);

    s_ui.status = makeLabel(root, "", &lv_font_be_vietnam_16, C_MUTED);
    lv_obj_set_width(s_ui.status, 180);
    lv_label_set_long_mode(s_ui.status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ui.status, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_pos(s_ui.status, s_ui.w - 262, 22);

    // Danh sách tin.
    s_ui.list = lv_obj_create(root);
    lv_obj_remove_style_all(s_ui.list);
    lv_obj_set_size(s_ui.list, s_ui.w, s_ui.h - TOP_H);
    lv_obj_set_pos(s_ui.list, 0, TOP_H);
    lv_obj_set_style_pad_hor(s_ui.list, 16, 0);
    lv_obj_set_style_pad_ver(s_ui.list, 8, 0);
    lv_obj_set_style_pad_row(s_ui.list, 10, 0);
    lv_obj_set_flex_flow(s_ui.list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_ui.list, LV_DIR_VER);
    lv_obj_add_flag(s_ui.list, LV_OBJ_FLAG_SCROLLABLE);

    s_ui.timer = lv_timer_create(onTimer, 50, nullptr);
}

}  // namespace

HCCNews::HCCNews():
    ESP_Brookesia_PhoneApp("News", &img_app_news, true)  // tên trên màn hình chính: font launcher không có dấu
{
}

HCCNews::~HCCNews()
{
}

bool HCCNews::init(void)
{
    return true;
}

bool HCCNews::run(void)
{
    startWorker();
    // Kết quả còn sót từ lần mở trước: bỏ.
    while (Result *r = popResult()) {
        freeResult(r);
    }
    s_ui.sources = builtinSources();
    loadSdSource(s_ui.sources);
    buildUi(lv_scr_act());
    loadFeed();
    ESP_LOGI(TAG, "mở, %d nguồn", (int)s_ui.sources.size());
    return true;
}

bool HCCNews::back(void)
{
    if (s_ui.article != nullptr) {
        closeArticle();
        return true;
    }
    notifyCoreClosed();
    return true;
}

bool HCCNews::close(void)
{
    if (s_ui.root != nullptr) {
        lv_obj_del(s_ui.root);  // onRootDeleted dọn ảnh, timer, hàng đợi
    }
    return true;
}
