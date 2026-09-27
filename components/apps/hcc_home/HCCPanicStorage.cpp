#include "HCCPanicStorage.hpp"

#include <cstdio>
#include <cstring>
#include <strings.h>
#include <dirent.h>
#include <sys/stat.h>

#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "esp_log.h"

static const char *TAG = "PanicStore";

// Trạng thái mount: 0 = chưa thử, 1 = đã mount OK, 2 = thử và thất bại.
// Chỉ thử mount MỘT lần/phiên: BSP giữ LDO on-chip cho SD, gọi lại khi đã
// fail sẽ luôn trả ESP_ERR_INVALID_ARG (LDO bị giữ) và làm ngập log.
static int s_mount_state = 0;

bool HCCPanicStorage::ensureMounted()
{
    if (s_mount_state == 1) return true;
    if (s_mount_state == 2) return false;

    esp_err_t ret = bsp_sdcard_mount();
    if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
        // ESP_ERR_INVALID_STATE = đã mount trước đó ở nơi khác.
        s_mount_state = 1;
        ESP_LOGI(TAG, "SD ready");
        return true;
    }
    ESP_LOGE(TAG, "SD mount failed: %s (chua cam the SD?)", esp_err_to_name(ret));
    s_mount_state = 2;
    return false;
}

std::string HCCPanicStorage::absPath(const std::string &rel_path)
{
    std::string p = rel_path;
    while (!p.empty() && p.front() == '/') {
        p.erase(p.begin());
    }
    return std::string(BSP_SD_MOUNT_POINT) + "/" + p;
}

bool HCCPanicStorage::ensureDir(const std::string &rel_dir)
{
    if (!ensureMounted()) {
        return false;
    }
    // Tạo từng cấp thư mục.
    std::string full = absPath(rel_dir);
    std::string acc;
    size_t i = 0;
    // Bắt đầu từ sau mount point.
    acc = BSP_SD_MOUNT_POINT;
    std::string rest = rel_dir;
    while (!rest.empty() && rest.front() == '/') rest.erase(rest.begin());

    size_t start = 0;
    while (start <= rest.size()) {
        size_t slash = rest.find('/', start);
        std::string seg = (slash == std::string::npos) ? rest.substr(start)
                                                        : rest.substr(start, slash - start);
        if (!seg.empty()) {
            acc += "/";
            acc += seg;
            if (mkdir(acc.c_str(), 0777) != 0 && errno != EEXIST) {
                ESP_LOGW(TAG, "mkdir %s: errno=%d", acc.c_str(), errno);
            }
        }
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    (void)full;
    (void)i;
    return true;
}

bool HCCPanicStorage::writeFile(const std::string &rel_path, const char *data, size_t len)
{
    if (!ensureMounted()) {
        return false;
    }
    // Tạo thư mục cha nếu có.
    size_t slash = rel_path.find_last_of('/');
    if (slash != std::string::npos) {
        ensureDir(rel_path.substr(0, slash));
    }

    std::string path = absPath(rel_path);
    FILE *f = fopen(path.c_str(), "wb");
    if (!f) {
        ESP_LOGE(TAG, "open write %s failed", path.c_str());
        return false;
    }
    size_t written = (len > 0 && data) ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    if (written != len) {
        ESP_LOGE(TAG, "write %s short %u/%u", path.c_str(), (unsigned)written, (unsigned)len);
        return false;
    }
    ESP_LOGI(TAG, "wrote %u bytes -> %s", (unsigned)len, path.c_str());
    return true;
}

bool HCCPanicStorage::readFile(const std::string &rel_path, std::string &out)
{
    if (!ensureMounted()) {
        return false;
    }
    std::string path = absPath(rel_path);
    FILE *f = fopen(path.c_str(), "rb");
    if (!f) {
        return false;
    }
    out.clear();
    char buf[1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    fclose(f);
    return true;
}

std::vector<std::string> HCCPanicStorage::listDir(const std::string &rel_dir, const char *ext)
{
    std::vector<std::string> names;
    if (!ensureMounted()) {
        return names;
    }
    std::string path = absPath(rel_dir);
    DIR *dir = opendir(path.c_str());
    if (!dir) {
        return names;
    }
    const size_t ext_len = ext ? strlen(ext) : 0;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type == DT_DIR) continue;
        const char *name = ent->d_name;
        if (name[0] == '.') continue;
        if (ext) {
            size_t nl = strlen(name);
            if (nl < ext_len || strcasecmp(name + nl - ext_len, ext) != 0) continue;
        }
        names.push_back(name);
    }
    closedir(dir);
    return names;
}
