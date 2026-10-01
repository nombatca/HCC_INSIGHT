#!/usr/bin/env python3
"""Sinh các panic mẫu TỔNG HỢP (synthetic) cho test Panic Parser v2.

Đây KHÔNG phải panic thật. Mỗi mẫu chỉ dựng lại phần ĐỊNH DẠNG đã biết để kiểm tra cơ chế
tách fact; nguồn của từng định dạng ghi trong SOURCES và README.md. Panic .ips thật đặt
vào samples/real/ (xem README.md), không sinh bằng script này.

Chạy: python3 test/panic/make_samples.py   (ghi đè samples/*.ips)
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "samples")

# Phần metadata xnu ghi sau thông điệp (osfmk/arm/model_dep.c) - giá trị giả.
TAIL = (
    "Debugger message: panic\n"
    "Memory ID: 0x6\n"
    "OS release type: User\n"
    "OS version: 21F90\n"
    "Kernel version: Darwin Kernel Version 23.5.0: synthetic; root:xnu-10063/RELEASE_ARM64_T8110\n"
    "Fileset Kernelcache UUID: 00000000000000000000000000000000\n"
    "iBoot version: iBoot-10151.120.3\n"
    "secure boot?: YES\n"
    "roots installed: 0\n"
    "Paniclog version: 14\n"
    "Kernel slide:      0x0000000016d5c000\n"
    "Kernel text base:  0xfffffff01dd60000\n"
    "Panicked task 0xffffffe2a8d2c680: 0 pages, 312 threads: pid 0: kernel_task\n"
    "Panicked thread: 0xffffffe2a8e5e000, backtrace: 0xffffffea0b19f3d0, tid: 445\n"
    "\t\t  lr: 0xfffffff01de9b9f8  fp: 0xffffffea0b19f440\n"
    "Last started kext at 1290418: com.apple.driver.AppleSMC 1 (addr 0xfffffff01b2c0000, size 16384)\n"
    "loaded kexts:\ncom.apple.driver.AppleSEPManager\ncom.apple.driver.AppleANS3NVMeController\n"
    "com.apple.driver.AppleAOPAudio\ncom.apple.driver.AppleBasebandPCI\n"
)


def ips(name, product, panic_string, ts="2024-06-01 10:00:00.00 +0700", incident="00000000-SYNTH",
        os_version="iPhone OS 17.5 (21F90)", flags="0x902", bug_type="210", broken_payload=False):
    header = {"bug_type": bug_type, "timestamp": ts, "os_version": os_version,
              "roots_installed": 0, "incident_id": incident}
    payload = {"build": os_version, "product": product, "socId": "0x8110",
               "kernel": "Darwin Kernel Version 23.5.0", "incident": incident,
               "panicFlags": flags, "panicString": panic_string}
    body = json.dumps(payload)
    if broken_payload:
        # Payload hỏng như log thực tế từ máy iOS 27 (cắt cụt ở giữa): parser phải lấy
        # panicString thẳng từ text.
        body = body[: body.index('"panicFlags"')] + body[body.index('"panicString"'):]
        body = body[:-40]
    with open(os.path.join(OUT, name + ".ips"), "w") as f:
        f.write(json.dumps(header) + "\n" + body)


SOURCES = {}


def sample(name, source, **kw):
    SOURCES[name] = source
    ips(name, **kw)


def main():
    os.makedirs(OUT, exist_ok=True)

    # 1. busy timeout, nhiều entry (định dạng xnu iokit/Kernel/IOService.cpp).
    #    Tên driver lấy từ ví dụ có sẵn trong HCCPanicAI.cpp (atc0-dpphy, atc-dpphy0, usb-drd).
    sample("busy_timeout_usbc", "xnu IOService.cpp + vi du trong HCCPanicAI.cpp",
           product="iPhone15,4",
           panic_string="panic(cpu 2 caller 0xfffffff02a5f3c10): busy timeout[0], (60s): multiple "
           "entries holding the registry busy, IOKit termination queue depth 0: 'atc0-dpphy' (2d,0), "
           "'atc-dpphy0' (13,0), 'usb-drd0' (1,1) @IOService.cpp:6050\n" + TAIL)
    # 2. Cùng lỗi, khác cpu / caller / loops / trạng thái / thời điểm -> signature phải bằng #1.
    sample("busy_timeout_usbc_variant", "bien the cua #1 (kiem tra signature on dinh)",
           product="iPhone15,4", ts="2024-06-03 21:12:09.00 +0700", incident="11111111-SYNTH",
           panic_string="panic(cpu 5 caller 0xfffffff0279a1c10): busy timeout[2], (180s): multiple "
           "entries holding the registry busy, IOKit termination queue depth 1: 'usb-drd0' (5,0), "
           "'atc-dpphy0' (7,0), 'atc0-dpphy' (2f,1) @IOService.cpp:6061\n" + TAIL)
    # 3. busy timeout một entry.
    sample("busy_timeout_single", "xnu IOService.cpp (nhanh mot entry)", product="iPhone14,2",
           panic_string="panic(cpu 0 caller 0xfffffff01e9c2d34): busy timeout[0], (60s): "
           "'AppleANS3NVMeController' (5,0) @IOService.cpp:5960\n" + TAIL)
    # 4. userspace watchdog + Missing sensor(s) (định dạng quan sát được trên iFixit/Apple Community).
    sample("userspace_watchdog_missing_sensor", "quan sat: iFixit Answers / Apple Community",
           product="iPhone10,3",
           panic_string="panic(cpu 1 caller 0xfffffff007a3e1c0): userspace watchdog timeout: no "
           "successful checkins from thermalmonitord in 180 seconds\n"
           "service: thermalmonitord, total successful checkins since load (180 seconds ago): 0, "
           "last successful checkin: 180 seconds ago\n"
           "Missing sensor(s): mic1 PRS0 \n" + TAIL)
    # 5. watchdog timeout (watchdogd).
    sample("watchdog_timeout", "quan sat: Apple Community", product="iPhone12,1",
           panic_string="panic(cpu 3 caller 0xfffffff0218b6a3c): watchdog timeout: no checkins from "
           "watchdogd in 90 seconds (4 total checkins since monitoring last enabled)\n" + TAIL)
    # 6. SMC PANIC ASSERT (định dạng chi tiết KHÔNG có tài liệu: mẫu tổng hợp để test cơ chế).
    sample("smc_panic_assert", "TONG HOP - dinh dang chua co tai lieu", product="iPhone15,2",
           panic_string="panic(cpu 0 caller 0xfffffff02b3c4d10): SMC PANIC - ASSERT at "
           "smc_sensor.c:212 status 0x41 S.sensor[3] mask 0x00000A05\n" + TAIL)
    # 7. AOP.
    sample("aop_panic", "TONG HOP", product="iPhone13,2",
           panic_string="panic(cpu 1 caller 0xfffffff0256a1e20): AOP PANIC - aop_crash: code "
           "0x2c rtk_assert\n" + TAIL)
    # 8. SEP.
    sample("sep_panic", "TONG HOP", product="iPhone14,5",
           panic_string="panic(cpu 4 caller 0xfffffff028c1a0e4): SEP Panic: :sks /sks : 0x0001f2e8 "
           "0x00000000 0x00000000 0x00000000\n" + TAIL)
    # 9. ANS / NVMe.
    sample("ans_nvme", "TONG HOP", product="iPhone12,8",
           panic_string="panic(cpu 0 caller 0xfffffff0170e2b88): ANS2 Recoverable Panic - assert "
           "failed: status 0x7 error=-536870212 @AppleANS3NVMeController.cpp:1234\n" + TAIL)
    # 10. Baseband.
    sample("baseband", "TONG HOP", product="iPhone11,8",
           panic_string="panic(cpu 1 caller 0xfffffff00e1f3a40): AppleBasebandPCI: baseband reset "
           "timeout, reason 0x14\n" + TAIL)
    # 11. PMU (mơ hồ).
    sample("pmu", "TONG HOP", product="iPhone13,4",
           panic_string="panic(cpu 2 caller 0xfffffff02133cc08): PMU fault: status 0x800 "
           "@ApplePMGR.cpp:880\n" + TAIL)
    # 12. i2c.
    sample("i2c_bus", "TONG HOP", product="iPhone12,3",
           panic_string="panic(cpu 0 caller 0xfffffff01ae2f4a8): i2c2::_checkBusStatus SDA stuck "
           "low, status 0x10\n" + TAIL)
    # 13. Kernel data abort + register dump (định dạng xnu osfmk/arm64/sleh.c).
    sample("kernel_data_abort", "xnu arm64/sleh.c", product="iPhone14,7",
           panic_string="panic(cpu 3 caller 0xfffffff028e64a7c): Kernel data abort. at pc "
           "0xfffffff02a1b2c3c, lr 0xfffffff02a1b2bf0 (saved state: 0xffffffeb0c06f2a0)\n"
           "\t  x0:  0x0000000000000000 x1:  0xffffffe1a3c55f00  x2:  0x0000000000000010  x3:  0x0000000000000000\n"
           "\t  fp:  0xffffffeb0c06f5f0 lr:  0xfffffff02a1b2bf0  sp:  0xffffffeb0c06f5f0\n"
           "\t  pc:  0xfffffff02a1b2c3c cpsr: 0x60401208         esr: 0x0000000096000004  far: 0x0000000000000010\n"
           + TAIL)
    # 14. Header thật (ảnh chụp màn hình người dùng, iPhone19,7 / iOS 27.2) + payload hỏng.
    sample("iphone19_7_broken_payload", "header tu anh chup nguoi dung; panic TONG HOP",
           product="iPhone19,7", os_version="iPhone OS 27.2 (24B5089g)",
           ts="2026-09-30 17:42:39.00 +0700", incident="2015C49A-4065-49E3-B164-9596A467B399",
           broken_payload=True,
           panic_string="panic(cpu 4 caller 0xfffffff0280c1d2c): userspace watchdog timeout: no "
           "successful checkins from thermalmonitord in 180 seconds\nMissing sensor(s): TG0B\n" + TAIL)
    # 15. Panic kernel có thông điệp nhưng không thuộc nhóm nào.
    sample("kernel_generic", "TONG HOP", product="iPhone16,1",
           panic_string="panic(cpu 1 caller 0xfffffff0233a8e14): zalloc: zone map exhausted while "
           "allocating from zone [data.kalloc.32] @zalloc.c:4471\n" + TAIL)

    # 16. Text thô (không JSON) - vd chữ AI chép từ ảnh panic.
    with open(os.path.join(OUT, "text_only.ips"), "w") as f:
        f.write("bug_type: 210\nproduct: iPhone13,2\nOS Version: iPhone OS 16.7.2 (20H115)\n"
                "panic(cpu 0 caller 0xfffffff0123abc10): i2c0::_checkBusStatus SDA stuck low\n"
                "Debugger message: panic\n")
    SOURCES["text_only"] = "TONG HOP - dang chu chep tu anh"

    with open(os.path.join(OUT, "SOURCES.txt"), "w") as f:
        for k in sorted(SOURCES):
            f.write("%s: %s\n" % (k, SOURCES[k]))


if __name__ == "__main__":
    main()
