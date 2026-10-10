/*
 * HCCSpmiLink: điểm nối từ SPMI MONITOR / PMIC ANALYZER sang các module BASEBAND và BOARDVIEW (chưa có).
 * Module đích gọi registerTarget() khi sẵn sàng; trước đó nút liên kết hiện "CHƯA LIÊN KẾT" và bị khoá.
 * Chỉ truyền giá trị thô (SID, địa chỉ thanh ghi) - module đích tự tra theo dữ liệu đã xác minh của nó.
 */
#pragma once

#include <stdint.h>

namespace HCCSpmiLink {

enum Target : uint8_t { BASEBAND = 0, BOARDVIEW = 1, TARGET_COUNT = 2 };

struct Ref {
    uint8_t sid;
    uint16_t addr;
    bool has_addr;
};

typedef void (*OpenFn)(const Ref &ref, void *user);

void registerTarget(Target t, OpenFn fn, void *user);
bool available(Target t);
bool open(Target t, const Ref &ref);
const char *targetName(Target t);

}  // namespace HCCSpmiLink
