#include "HCCEeprom.hpp"

#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"

// Trần cứng cho buffer ghi trang trên stack: [địa chỉ 2 byte] + [1 trang].
static constexpr uint16_t kMaxPageSize = 256;

esp_err_t HCCEeprom::addDevice(i2c_master_bus_handle_t bus, const Config &cfg,
                               i2c_master_dev_handle_t *out_dev)
{
    if (bus == nullptr || out_dev == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_device_config_t dev_cfg = {};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address = cfg.device_address;
    dev_cfg.scl_speed_hz = cfg.scl_speed_hz;

    return i2c_master_bus_add_device(bus, &dev_cfg, out_dev);
}

// Ghi địa chỉ ô nhớ vào buf theo big-endian; trả về số byte đã ghi (1 hoặc 2).
size_t HCCEeprom::putMemAddress(uint8_t *buf, const Config &cfg, uint32_t mem_addr)
{
    if (cfg.address_width >= 2) {
        buf[0] = static_cast<uint8_t>((mem_addr >> 8) & 0xFF);
        buf[1] = static_cast<uint8_t>(mem_addr & 0xFF);
        return 2;
    }
    buf[0] = static_cast<uint8_t>(mem_addr & 0xFF);
    return 1;
}

esp_err_t HCCEeprom::addDeviceAt(i2c_master_bus_handle_t bus, const Config &cfg, uint16_t dev_addr,
                                 i2c_master_dev_handle_t *out_dev)
{
    Config c = cfg;
    c.device_address = dev_addr;
    return addDevice(bus, c, out_dev);
}

// Chip địa chỉ ô nhớ 1 byte nhưng > 256 B (24C04/08/16): bit A8..A10 nằm ở bit thấp địa chỉ thiết bị.
uint16_t HCCEeprom::blockAddress(const Config &cfg, uint32_t mem_addr)
{
    if (cfg.address_width >= 2 || cfg.total_size <= 256) {
        return cfg.device_address;
    }
    uint16_t mask = static_cast<uint16_t>((cfg.total_size / 256) - 1) & 0x07;
    return static_cast<uint16_t>((cfg.device_address & ~mask) | ((mem_addr >> 8) & mask));
}

esp_err_t HCCEeprom::read(i2c_master_bus_handle_t bus, const Config &cfg, uint32_t offset,
                          uint8_t *out, size_t len, Progress cb, void *user)
{
    if (bus == nullptr || out == nullptr || len == 0 || offset + len > cfg.total_size) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    i2c_master_dev_handle_t dev = nullptr;
    uint16_t dev_addr = 0xFFFF;
    const size_t chunk_size = 128;
    size_t done = 0;
    while (done < len) {
        const uint32_t mem = offset + done;
        size_t n = len - done < chunk_size ? len - done : chunk_size;
        if (cfg.address_width < 2) {
            const uint32_t room = 256 - (mem & 0xFF);  // không vượt khối 256 B
            if (n > room) {
                n = room;
            }
        }
        const uint16_t a = blockAddress(cfg, mem);
        if (a != dev_addr) {
            if (dev) {
                i2c_master_bus_rm_device(dev);
                dev = nullptr;
            }
            ret = addDeviceAt(bus, cfg, a, &dev);
            if (ret != ESP_OK) {
                break;
            }
            dev_addr = a;
        }
        uint8_t addr_buf[2];
        const size_t addr_len = putMemAddress(addr_buf, cfg, mem);
        ret = i2c_master_transmit_receive(dev, addr_buf, addr_len, out + done, n, cfg.xfer_timeout_ms);
        if (ret != ESP_OK) {
            printf("[HCC EEPROM] read @0x%04X (dev 0x%02X) failed: %s\n", static_cast<unsigned>(mem), a,
                   esp_err_to_name(ret));
            break;
        }
        done += n;
        if (cb) {
            cb(done, len, user);
        }
    }
    if (dev) {
        i2c_master_bus_rm_device(dev);
    }
    return ret;
}

esp_err_t HCCEeprom::write(i2c_master_bus_handle_t bus, const Config &cfg, uint32_t offset,
                           const uint8_t *data, size_t len, Progress cb, void *user)
{
    if (bus == nullptr || data == nullptr || len == 0 || offset + len > cfg.total_size) {
        return ESP_ERR_INVALID_ARG;
    }
    const uint16_t page = cfg.page_size;
    if (page == 0 || page > kMaxPageSize) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = ESP_OK;
    i2c_master_dev_handle_t dev = nullptr;
    uint16_t dev_addr = 0xFFFF;
    uint8_t tx[2 + kMaxPageSize];
    size_t done = 0;
    while (done < len) {
        const uint32_t mem = offset + done;
        size_t n = page - (mem % page);  // không vượt biên trang
        if (n > len - done) {
            n = len - done;
        }
        const uint16_t a = blockAddress(cfg, mem);
        if (a != dev_addr) {
            if (dev) {
                i2c_master_bus_rm_device(dev);
                dev = nullptr;
            }
            ret = addDeviceAt(bus, cfg, a, &dev);
            if (ret != ESP_OK) {
                break;
            }
            dev_addr = a;
        }
        const size_t addr_len = putMemAddress(tx, cfg, mem);
        memcpy(tx + addr_len, data + done, n);
        ret = i2c_master_transmit(dev, tx, addr_len + n, cfg.xfer_timeout_ms);
        if (ret != ESP_OK) {
            printf("[HCC EEPROM] write @0x%04X (dev 0x%02X) failed: %s\n", static_cast<unsigned>(mem), a,
                   esp_err_to_name(ret));
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(cfg.write_cycle_ms));  // chờ chu kỳ ghi nội bộ t_WR
        done += n;
        if (cb) {
            cb(done, len, user);
        }
    }
    if (dev) {
        i2c_master_bus_rm_device(dev);
    }
    return ret;
}

esp_err_t HCCEeprom::readAll(i2c_master_bus_handle_t bus, const Config &cfg,
                             uint8_t *out, size_t out_size)
{
    if (out == nullptr || out_size < cfg.total_size || cfg.total_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = read(bus, cfg, 0, out, cfg.total_size);
    if (ret == ESP_OK) {
        printf("[HCC EEPROM] read %u bytes @0x%02X OK\n", static_cast<unsigned>(cfg.total_size), cfg.device_address);
    }
    return ret;
}

esp_err_t HCCEeprom::writeAll(i2c_master_bus_handle_t bus, const Config &cfg,
                              const uint8_t *data, size_t data_size)
{
    if (data == nullptr || data_size == 0 || data_size > cfg.total_size) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t ret = write(bus, cfg, 0, data, data_size);
    if (ret == ESP_OK) {
        printf("[HCC EEPROM] wrote %u bytes @0x%02X OK\n", static_cast<unsigned>(data_size), cfg.device_address);
    }
    return ret;
}

esp_err_t HCCEeprom::ensureSdMounted(bool *mounted_here)
{
    *mounted_here = false;
    esp_err_t ret = bsp_sdcard_mount();
    if (ret == ESP_OK) {
        *mounted_here = true;
        return ESP_OK;
    }
    if (ret == ESP_ERR_INVALID_STATE) {
        // Đã được mount trước đó ở nơi khác: dùng chung, không unmount.
        return ESP_OK;
    }
    printf("[HCC EEPROM] mount SD failed: %s\n", esp_err_to_name(ret));
    return ret;
}

void HCCEeprom::buildPath(char *out, size_t out_size, const char *filename)
{
    while (*filename == '/') {
        ++filename;  // bỏ dấu '/' đầu để không tạo '//'
    }
    snprintf(out, out_size, "%s/%s", BSP_SD_MOUNT_POINT, filename);
}

esp_err_t HCCEeprom::saveBufferToSd(const char *filename,
                                    const uint8_t *data, size_t size)
{
    if (filename == nullptr || data == nullptr || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool mounted_here = false;
    esp_err_t ret = ensureSdMounted(&mounted_here);
    if (ret != ESP_OK) {
        return ret;
    }

    char path[128];
    buildPath(path, sizeof(path), filename);

    FILE *f = fopen(path, "wb");
    if (f == nullptr) {
        printf("[HCC EEPROM] open '%s' để ghi thất bại\n", path);
        if (mounted_here) bsp_sdcard_unmount();
        return ESP_FAIL;
    }

    const size_t written = fwrite(data, 1, size, f);
    fclose(f);
    if (mounted_here) bsp_sdcard_unmount();

    if (written != size) {
        printf("[HCC EEPROM] ghi '%s' thiếu: %u/%u byte\n",
               path, static_cast<unsigned>(written), static_cast<unsigned>(size));
        return ESP_FAIL;
    }

    printf("[HCC EEPROM] đã lưu %u byte -> %s\n",
           static_cast<unsigned>(size), path);
    return ESP_OK;
}

esp_err_t HCCEeprom::loadBufferFromSd(const char *filename,
                                      uint8_t *out, size_t out_size, size_t *out_len)
{
    if (filename == nullptr || out == nullptr || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (out_len != nullptr) {
        *out_len = 0;
    }

    bool mounted_here = false;
    esp_err_t ret = ensureSdMounted(&mounted_here);
    if (ret != ESP_OK) {
        return ret;
    }

    char path[128];
    buildPath(path, sizeof(path), filename);

    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        printf("[HCC EEPROM] open '%s' để đọc thất bại\n", path);
        if (mounted_here) bsp_sdcard_unmount();
        return ESP_FAIL;
    }

    const size_t read = fread(out, 1, out_size, f);
    fclose(f);
    if (mounted_here) bsp_sdcard_unmount();

    if (out_len != nullptr) {
        *out_len = read;
    }
    printf("[HCC EEPROM] đã đọc %u byte <- %s\n",
           static_cast<unsigned>(read), path);
    return ESP_OK;
}

int HCCEeprom::listFiles(char *out, size_t out_size, const char *ext_filter)
{
    if (out == nullptr || out_size == 0) {
        return -1;
    }
    out[0] = '\0';

    bool mounted_here = false;
    if (ensureSdMounted(&mounted_here) != ESP_OK) {
        return -1;
    }

    DIR *dir = opendir(BSP_SD_MOUNT_POINT);
    if (dir == nullptr) {
        printf("[HCC EEPROM] mở thư mục '%s' thất bại\n", BSP_SD_MOUNT_POINT);
        if (mounted_here) bsp_sdcard_unmount();
        return -1;
    }

    const size_t ext_len = (ext_filter != nullptr) ? strlen(ext_filter) : 0;
    int count = 0;
    size_t used = 0;
    struct dirent *ent = nullptr;
    while ((ent = readdir(dir)) != nullptr) {
        if (ent->d_type == DT_DIR) {
            continue;
        }
        const char *name = ent->d_name;
        if (name[0] == '.') {
            continue;  // bỏ file ẩn / thư mục hiện tại
        }
        if (ext_filter != nullptr) {
            const size_t nl = strlen(name);
            if (nl < ext_len || strcasecmp(name + nl - ext_len, ext_filter) != 0) {
                continue;
            }
        }

        const size_t name_len = strlen(name);
        const size_t need = name_len + (count > 0 ? 1 : 0);
        if (used + need + 1 > out_size) {
            break;  // hết chỗ đệm
        }
        if (count > 0) {
            out[used++] = '\n';
        }
        memcpy(out + used, name, name_len);
        used += name_len;
        ++count;
    }
    out[used] = '\0';

    closedir(dir);
    if (mounted_here) bsp_sdcard_unmount();
    return count;
}

esp_err_t HCCEeprom::dumpToSd(i2c_master_bus_handle_t bus, const Config &cfg,
                              const char *filename)
{
    uint8_t *buffer = static_cast<uint8_t *>(malloc(cfg.total_size));
    if (buffer == nullptr) {
        printf("[HCC EEPROM] không cấp phát được %u byte\n",
               static_cast<unsigned>(cfg.total_size));
        return ESP_ERR_NO_MEM;
    }

    esp_err_t ret = readAll(bus, cfg, buffer, cfg.total_size);
    if (ret == ESP_OK) {
        ret = saveBufferToSd(filename, buffer, cfg.total_size);
    }

    free(buffer);
    return ret;
}

esp_err_t HCCEeprom::programFromSd(i2c_master_bus_handle_t bus, const Config &cfg,
                                   const char *filename)
{
    uint8_t *buffer = static_cast<uint8_t *>(malloc(cfg.total_size));
    if (buffer == nullptr) {
        printf("[HCC EEPROM] không cấp phát được %u byte\n",
               static_cast<unsigned>(cfg.total_size));
        return ESP_ERR_NO_MEM;
    }

    size_t len = 0;
    esp_err_t ret = loadBufferFromSd(filename, buffer, cfg.total_size, &len);
    if (ret == ESP_OK) {
        if (len == 0) {
            printf("[HCC EEPROM] file '%s' rỗng, bỏ qua ghi\n", filename);
            ret = ESP_ERR_INVALID_SIZE;
        } else {
            ret = writeAll(bus, cfg, buffer, len);
        }
    }

    free(buffer);
    return ret;
}

esp_err_t HCCEeprom::verifyAgainstSd(i2c_master_bus_handle_t bus, const Config &cfg,
                                     const char *filename, size_t *mismatch_offset)
{
    if (mismatch_offset != nullptr) {
        *mismatch_offset = 0;
    }

    uint8_t *file_buf = static_cast<uint8_t *>(malloc(cfg.total_size));
    uint8_t *eeprom_buf = static_cast<uint8_t *>(malloc(cfg.total_size));
    if (file_buf == nullptr || eeprom_buf == nullptr) {
        free(file_buf);
        free(eeprom_buf);
        printf("[HCC EEPROM] verify: không cấp phát được buffer\n");
        return ESP_ERR_NO_MEM;
    }

    size_t file_len = 0;
    esp_err_t ret = loadBufferFromSd(filename, file_buf, cfg.total_size, &file_len);
    if (ret == ESP_OK && file_len == 0) {
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret == ESP_OK) {
        ret = readAll(bus, cfg, eeprom_buf, cfg.total_size);
    }
    if (ret == ESP_OK) {
        for (size_t i = 0; i < file_len; ++i) {
            if (file_buf[i] != eeprom_buf[i]) {
                if (mismatch_offset != nullptr) {
                    *mismatch_offset = i;
                }
                printf("[HCC EEPROM] verify lệch tại 0x%04X: file=0x%02X eeprom=0x%02X\n",
                       static_cast<unsigned>(i), file_buf[i], eeprom_buf[i]);
                ret = ESP_ERR_INVALID_CRC;
                break;
            }
        }
    }
    if (ret == ESP_OK) {
        printf("[HCC EEPROM] verify OK: %u byte khớp\n",
               static_cast<unsigned>(file_len));
    }

    free(file_buf);
    free(eeprom_buf);
    return ret;
}
