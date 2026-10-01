#pragma once

#include <cstdint>
#include <string>
#include <vector>

// Panic Parser v2: dữ kiện có cấu trúc tách từ panic log iPhone (.ips).
//
// Nguyên tắc:
//  - Mỗi fact giữ nguyên đoạn log gốc làm evidence (field + số dòng + trích đoạn).
//  - Chỉ tách những gì log THỰC SỰ ghi. Không có mapping phần cứng ở đây (driver ->
//    linh kiện, sensor -> cáp, bit -> sensor...): việc đó thuộc Knowledge Base (bước 2).
//  - Địa chỉ kernel, địa chỉ caller, thanh ghi, và cặp (x,y) trạng thái IOService trong
//    "busy timeout" KHÔNG phải mã lỗi: được ghi riêng vào ignored_numbers kèm lý do.
//  - Không xác định được thì để trống / UNKNOWN, ghi lý do vào notes.
//
// Không phụ thuộc ESP-IDF / LVGL: chạy được trên máy host (test/panic).

// Đoạn log gốc chứng minh một fact.
struct PanicEvidence {
    std::string field;   // "panicString", "header.bug_type", "payload.product", ...
    int line = 0;        // dòng (1-based) trong field; 0 nếu field là một giá trị đơn
    std::string excerpt; // trích nguyên văn (cắt <= 240 ký tự)
};

// Loại panic, nhận diện từ CẤU TRÚC thông điệp (không phải phần cứng).
enum class PanicKind : uint8_t {
    Unknown,
    BusyTimeout,         // "busy timeout[N], (Ns): 'driver' (x,y)..."  (xnu IOService.cpp)
    IOKitDaemonStall,    // "IOKit Daemon (...) stall[N], (Ns): ..."    (xnu IOService.cpp)
    UserspaceWatchdog,   // "userspace watchdog timeout: no successful checkins from <daemon>"
    WatchdogTimeout,     // "watchdog timeout: no checkins from watchdogd ..."
    SmcPanic,            // "SMC PANIC ..." / AppleSMC
    AopPanic,            // "AOP PANIC ..." / AOP
    SepPanic,            // "SEP Panic ..." / AppleSEP
    AnsNvme,             // ANS2 / ANS3 / AppleANS / NVMe
    Baseband,            // baseband / AppleBaseband / bbpanic
    Pmu,                 // "PMU" (ngữ nghĩa mơ hồ: xem notes)
    I2c,                 // i2cN / I2C
    UsbPhy,              // atc*/dpphy/usb-drd/AppleTypeC... được nêu trong panic
    KernelException,     // "Kernel data abort." ... (xnu arm64/sleh.c)
    KernelGeneric,       // panic kernel khác có thông điệp nhưng không thuộc nhóm trên
};
const char *panicKindName(PanicKind kind);

// Driver / IOService / daemon được log nêu tên.
struct PanicDriverFact {
    std::string name;      // nguyên văn, vd "atc0-dpphy", "usb-drd0", "thermalmonitord"
    std::string role;      // "busy_entry" | "watchdog_daemon" | "mentioned"
    std::string state;     // busy_entry: "(x,y)" trạng thái IOService (KHÔNG phải mã lỗi)
    std::vector<std::string> tags; // nhóm theo TÊN (từ vựng), vd "usb_phy"; không phải phần cứng
    PanicEvidence evidence;
};

// Sensor được log nêu tên.
struct PanicSensorFact {
    std::string name;      // nguyên văn, vd "mic1", "PRS0", "S.sensor[3]"
    std::string context;   // "missing_sensor" | "smc_sensor_index"
    int index = -1;        // S.sensor[N] -> N
    PanicEvidence evidence;
};

// Thời hạn chờ (timeout) được log ghi.
struct PanicTimeoutFact {
    std::string what;      // "busy_timeout" | "iokit_daemon_stall" | "userspace_watchdog" | "watchdog"
    long seconds = -1;     // số giây log ghi; -1 nếu không có
    long loops = -1;       // busy timeout[N] -> N
    std::string subject;   // daemon / dịch vụ liên quan nếu có
    PanicEvidence evidence;
};

// Một con số có ý nghĩa mã lỗi / giá trị trạng thái (không phải địa chỉ).
struct PanicCodeFact {
    std::string label;     // nhãn đứng trước, vd "code", "status", "panicFlags", "SMC PANIC"
    std::string raw;       // nguyên văn, vd "0x41", "65"
    int base = 16;         // 16 hoặc 10 theo cách log ghi
    uint64_t value = 0;
    std::string hex;       // "0x41"
    std::string dec;       // "65"
    std::vector<int> bits; // vị trí bit đang bật (0 = LSB)
    bool labeled_bitmask = false; // nhãn nói rõ là mask/flags/bits
    std::string context;   // kind của dòng chứa số: "smc", "aop", "sep", "ans", "message", ...
    bool in_message = false; // nằm trong thông điệp panic chính (dùng cho signature)
    PanicEvidence evidence;
};

// Thanh ghi CPU (từ register dump của kernel exception).
struct PanicRegisterFact {
    std::string name;      // "pc", "lr", "esr", "far", "cpsr", "x0"...
    std::string raw;
    uint64_t value = 0;
    PanicEvidence evidence;
};

// Số bị loại khỏi danh sách mã lỗi, kèm lý do (minh bạch, để test).
struct PanicIgnoredNumber {
    std::string raw;
    std::string reason;    // "kernel_address" | "caller_address" | "register" |
                           // "ioservice_state" | "metadata" | "backtrace"
    PanicEvidence evidence;
};

struct PanicFacts {
    int schema_version = 1;
    bool valid = false;
    std::string format;    // "ips_json" | "ips_text" | "text"

    // ---- Trường file (.ips header / payload) ----
    std::string bug_type;          // "210" = panic-full (theo .ips)
    std::string product;           // "iPhone14,2"; "" nếu log không có
    std::string os_version;        // header.os_version
    std::string build;             // payload.build
    std::string timestamp;
    std::string incident_id;
    std::string soc_id;            // payload.socId (nguyên văn)
    std::vector<PanicEvidence> field_evidence; // nguồn của các trường trên

    // ---- Cấu trúc panicString (xnu debug.c / arm/model_dep.c) ----
    int panic_cpu = -1;            // "panic(cpu N caller 0x...)"
    std::string caller_address;    // địa chỉ caller (chỉ để tham khảo, không phải mã lỗi)
    std::string message;           // thông điệp panic chính (sau "): ", trước metadata)
    std::string source_location;   // "@IOService.cpp:6050" nếu có
    std::string debugger_message;  // "Debugger message: ..."
    std::string panicked_task;     // tên tiến trình trong "Panicked task ...: <name>"
    std::string kernel_version;
    std::string iboot_version;
    std::string os_version_line;   // "OS version: ..." trong panicString
    std::string memory_id;
    std::string roots_installed;
    std::string secure_boot;
    std::string paniclog_version;

    // ---- Phân loại + fact có kiểu ----
    PanicKind primary_kind = PanicKind::Unknown;
    std::vector<PanicKind> kinds;  // mọi nhóm nhận ra (đã khử trùng)
    std::vector<PanicEvidence> kind_evidence; // song song với kinds
    std::vector<PanicDriverFact> drivers;
    std::vector<PanicSensorFact> sensors;
    std::vector<PanicTimeoutFact> timeouts;
    std::vector<PanicCodeFact> codes;
    std::vector<PanicRegisterFact> registers;
    int esr_ec = -1;               // ESR_ELx.EC (bit 31:26) nếu có thanh ghi esr
    std::string esr_ec_name;       // theo ARM ARM; "" nếu không thuộc bảng đã biết
    std::vector<int> i2c_buses;    // "i2cN" -> N
    std::vector<PanicIgnoredNumber> ignored_numbers;

    // Những gì CHƯA xác định được (UNKNOWN / NEEDS VERIFICATION) và lý do.
    std::vector<std::string> notes;

    // ---- Signature chuẩn hoá (xem HCCPanicFacts::buildSignature) ----
    std::string signature;         // có model: dùng đối chiếu VERIFIED CASE
    std::string signature_generic; // bỏ model: tìm case tương tự ở máy khác
};

class HCCPanicFacts {
public:
    // Trường đã đọc từ file (.ips header/payload), mỗi trường kèm nguồn.
    struct FileFields {
        std::string format;
        std::string bug_type, product, os_version, build, timestamp, incident_id, soc_id,
            panic_flags;
        std::vector<PanicEvidence> evidence;
    };

    // Tách fact từ panicString (+ trường file). Không bao giờ ném lỗi; thiếu thì UNKNOWN.
    static PanicFacts extract(const std::string &panic_string, const FileFields &fields);

    // Signature v1:
    //   v1|<product|UNKNOWN>|<bug_type|UNKNOWN>|<primary kind>|k=<kinds>|d=<drivers>|
    //   s=<sensors>|c=<codes trong thông điệp>|w=<daemon watchdog>|x=<ESR.EC>
    // Mọi danh sách: chữ thường, khử trùng, sắp xếp. Không chứa địa chỉ, số cpu, thời gian,
    // số giây timeout, trạng thái (x,y), số dòng -> ổn định giữa các lần panic cùng lỗi.
    static void buildSignature(PanicFacts &facts);

    // Chuỗi đọc được (để log / test).
    static std::string describe(const PanicFacts &facts);
};
