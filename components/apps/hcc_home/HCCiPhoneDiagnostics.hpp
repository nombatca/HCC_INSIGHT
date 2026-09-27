#pragma once

#include <string>
#include <vector>
#include "HCCUSBTransport.hpp"

// Backend kiểm tra phần cứng iPhone qua phiên Lockdown hiện có (KHÔNG dùng LVGL,
// KHÔNG tự nói chuyện protocol - chỉ đọc getter đã cache của HCCUSBTransport).
class HCCiPhoneDiagnostics {
public:
    enum class Status { NotTested, Unsupported, Pass, Fail, Check };

    struct Item {
        std::string name;      // tên phép kiểm tra
        Status status = Status::NotTested;
        std::string source;    // service/khoá dùng làm nguồn
        std::string evidence;  // raw value / bằng chứng
        std::string code;      // error/status code nếu có
        uint32_t ts = 0;       // timestamp (unix)
    };

    struct Group {
        std::string name;
        Status status = Status::NotTested;  // rollup của các item
        std::vector<Item> items;
    };

    struct FullResult {
        Group sensors;
        Group faceid;
        Group baseband;
        Group battery;
    };

    explicit HCCiPhoneDiagnostics(const HCCUSBTransport *transport) : _t(transport) {}

    Group checkSensors();
    Group checkFaceID();
    Group checkBaseband();
    Group checkBattery();
    FullResult runFullCheck();

    static const char *statusText(Status s);

private:
    const HCCUSBTransport *_t;
    bool sessionReady() const;
    static Status rollup(const std::vector<Item> &items);
};
