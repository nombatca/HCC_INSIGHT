#include "HCCPanicFacts.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Nguồn định dạng (xem test/panic/README.md):
//  - "panic(cpu %u caller 0x%lx): "                         xnu osfmk/kern/debug.c
//  - "Debugger message", "Memory ID", "OS version", "Kernel version", "iBoot version",
//    "secure boot?", "roots installed", "Paniclog version", "Panicked task/thread"
//                                                              xnu osfmk/arm/model_dep.c
//  - "busy timeout[%d], (%llds): '%s' (%x,%x), ..."           xnu iokit/Kernel/IOService.cpp
//  - "@<file>:<line>" sau thông điệp (PANIC_LOCATION)          xnu osfmk/kern/debug.h
//  - "Kernel data abort." ..., register dump "x0: ... esr: ... far: ..."
//                                                              xnu osfmk/arm64/sleh.c
//  - ESR_ELx.EC                                                 ARM Architecture Reference Manual
//  - userspace watchdog / watchdogd / SMC / AOP / SEP / ANS / baseband: thông điệp do phần
//    mềm đóng của Apple tạo -> chỉ nhận diện theo chữ quan sát được trong log thật.

namespace {

constexpr size_t kExcerptMax = 240;

std::string trim(const std::string &s)
{
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return std::string();
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

std::string toLower(std::string s)
{
    for (auto &c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool isWordChar(char c)
{
    return isalnum((unsigned char)c) || c == '_';
}

bool startsWith(const std::string &s, const char *prefix)
{
    return s.compare(0, strlen(prefix), prefix) == 0;
}

PanicEvidence ev(const char *field, int line, const std::string &text)
{
    PanicEvidence e;
    e.field = field;
    e.line = line;
    std::string t = trim(text);
    if (t.size() > kExcerptMax) t = t.substr(0, kExcerptMax) + "...";
    e.excerpt = t;
    return e;
}

// tok xuất hiện như một từ: ký tự trước không phải chữ/số. Nếu whole: ký tự sau cũng vậy.
size_t findWord(const std::string &low, const char *tok, bool whole, size_t from = 0)
{
    const size_t n = strlen(tok);
    for (size_t p = low.find(tok, from); p != std::string::npos; p = low.find(tok, p + 1)) {
        const bool left = (p == 0) || !isalnum((unsigned char)low[p - 1]);
        const bool right = !whole || p + n >= low.size() || !isalnum((unsigned char)low[p + n]);
        if (left && right) return p;
    }
    return std::string::npos;
}

struct Line {
    std::string text;
    std::string low;
    int number; // 1-based
};

std::vector<Line> splitLines(const std::string &s)
{
    std::vector<Line> out;
    size_t start = 0;
    int n = 1;
    while (start <= s.size()) {
        size_t nl = s.find('\n', start);
        std::string t = s.substr(start, (nl == std::string::npos ? s.size() : nl) - start);
        if (!t.empty() && t.back() == '\r') t.pop_back();
        out.push_back({t, toLower(t), n++});
        if (nl == std::string::npos) break;
        start = nl + 1;
    }
    return out;
}

// Dòng metadata do xnu ghi sau thông điệp panic (kết thúc vùng thông điệp).
const char *kMetadataPrefixes[] = {
    "debugger message:", "device:", "hardware model:", "ecid:", "boot args:", "memory id:",
    "os release type:", "os version:", "macos version:", "kernel version:",
    "fileset kernelcache uuid:", "kernelcache uuid:", "boot session uuid", "iboot version:",
    "iboot stage 2 version:", "secure boot?:", "roots installed:", "paniclog version:",
    "kernel slide:", "kernel text base:", "mach_absolute_time:", "epoch time:", "zone info:",
    "panicked task", "panicked thread", "last started kext", "loaded kexts:", "core ",
    "compressor info:", "tpidrx_ely", "total cpu_usage:", "thread task pri cpu_usage",
    "appleHWAccess", "x86 ", "pcieup link state:", "panic diags", "last stopped kext",
    "device-id:", "kernel uuid:", "boot version:", "boot stage 2 version:", "kernelcache slide:",
    "kernelcache base:", "kernel text exec", "rtkit-",
};

// Dòng mở đầu / thuộc dump firmware coprocessor (RTKit) sau thông điệp: mỗi bản iOS ghi hơi khác
// ("RTKit-2758... - Client: ...", "RTKit: RTKit-...", "Firmware: ... Client: AppleSMCFirmware_H17...",
// "MailBox(0)", "Inbox AKF_...", "Call Stack:"...). Không phải thông điệp panic.
bool isFirmwareDumpLine(const std::string &low_trimmed)
{
    static const char *starts[] = {"rtkit", "mailbox(", "inbox ", "outbox ", "call stack", "naslr slide",
                                   "time: 0x", "uuid:", "mcm", "dir endpoint", "========="};
    for (const char *p : starts) {
        if (startsWith(low_trimmed, p)) return true;
    }
    return low_trimmed.find("client: apple") != std::string::npos ||
           low_trimmed.find("- client:") != std::string::npos ||
           low_trimmed.find("_err_sts") != std::string::npos;
}

bool isMetadataLine(const std::string &low_trimmed)
{
    for (const char *p : kMetadataPrefixes) {
        std::string lp = toLower(p);
        if (startsWith(low_trimmed, lp.c_str())) return true;
    }
    return isFirmwareDumpLine(low_trimmed);
}

// "Thông điệp" kernel exception do xnu arm64/sleh.c tạo (đầu thông điệp).
const char *kKernelExceptionStarts[] = {
    "kernel data abort", "unaligned kernel data abort", "kernel instruction fetch abort",
    "undefined kernel instruction", "pc alignment exception from kernel",
    "sp alignment exception from kernel", "kernel parity error", "serror",
    "invalid kernel stack pointer", "floating point exception from kernel",
    "synchronous exception taken while sp1 selected", "unexpected fault in kernel",
    "user abort from non-interruptible context", "panic lockdown initiated", "pac failure",
    "hardware breakpoint debug exception from kernel", "software step debug exception from kernel",
    "illegal instruction set exception", "msr/mrs trap", "unrecognized floating point exception",
    "sme exception from kernel", "unexpected debugger trap while sp1 selected",
};

// ESR_ELx.EC (ARM ARM D17.2.37). Chỉ các lớp hay gặp trong panic kernel.
const char *esrClassName(int ec)
{
    switch (ec) {
        case 0x00: return "Unknown reason";
        case 0x0E: return "Illegal Execution state";
        case 0x15: return "SVC (AArch64)";
        case 0x18: return "MSR/MRS/System instruction trap";
        case 0x20: return "Instruction Abort (lower EL)";
        case 0x21: return "Instruction Abort (same EL)";
        case 0x22: return "PC alignment fault";
        case 0x24: return "Data Abort (lower EL)";
        case 0x25: return "Data Abort (same EL)";
        case 0x26: return "SP alignment fault";
        case 0x2C: return "Floating-point exception (AArch64)";
        case 0x2F: return "SError interrupt";
        case 0x30: return "Breakpoint (lower EL)";
        case 0x31: return "Breakpoint (same EL)";
        case 0x3C: return "BRK instruction (AArch64)";
        default: return "";
    }
}

bool isRegisterName(const std::string &w)
{
    static const char *names[] = {"pc", "lr", "sp", "fp", "cpsr", "esr", "far", "elr", "spsr"};
    for (const char *n : names) {
        if (w == n) return true;
    }
    if (w.size() >= 2 && w.size() <= 3 && w[0] == 'x' && isdigit((unsigned char)w[1]) &&
        (w.size() == 2 || isdigit((unsigned char)w[2]))) {
        return atoi(w.c_str() + 1) <= 30;
    }
    return false;
}

// Nhãn cho một con số: từ đứng ngay trước (bỏ qua ' ', ':', '=', '(', '[', ',').
std::string labelBefore(const std::string &line, size_t pos)
{
    size_t i = pos;
    while (i > 0 && strchr(" \t:=([,", line[i - 1])) i--;
    size_t e = i;
    while (i > 0 && (isWordChar(line[i - 1]) || line[i - 1] == '.' || line[i - 1] == '-')) i--;
    std::string w = line.substr(i, e - i);
    while (!w.empty() && (w.back() == '.' || w.back() == '-')) w.pop_back();
    return w;
}

// "(a,4020001)": hai số hex = IOService __state[0], __state[1].
bool isIOServiceState(const std::string &s)
{
    if (s.size() < 5 || s.front() != '(' || s.back() != ')') return false;
    const std::string in = s.substr(1, s.size() - 2);
    size_t comma = in.find(',');
    if (comma == std::string::npos || comma == 0 || comma + 1 >= in.size()) return false;
    for (size_t i = 0; i < in.size(); ++i) {
        if (i != comma && !isxdigit((unsigned char)in[i])) return false;
    }
    return true;
}

// Gộp mọi khoảng trắng (kể cả xuống dòng) thành một dấu cách.
std::string squeeze(const std::string &s)
{
    std::string out;
    bool sp = false;
    for (char c : s) {
        if (isspace((unsigned char)c)) {
            sp = true;
            continue;
        }
        if (sp && !out.empty()) out += ' ';
        sp = false;
        out += c;
    }
    return out;
}

bool labelIsCode(const std::string &low_label)
{
    static const char *labels[] = {"code", "error", "err", "status", "result", "ret", "rc",
                                   "reason", "mask", "bitmask", "flags", "bits", "value", "errno",
                                   "assert", "assertion", "cause", "type", "id"};
    for (const char *l : labels) {
        if (low_label == l) return true;
    }
    return false;
}

bool labelIsBitmask(const std::string &low_label)
{
    return low_label.find("mask") != std::string::npos ||
           low_label.find("flags") != std::string::npos ||
           low_label.find("bits") != std::string::npos ||
           low_label.find("bitmap") != std::string::npos;
}

void fillNumber(PanicCodeFact &c)
{
    char buf[32];
    snprintf(buf, sizeof(buf), "0x%llX", (unsigned long long)c.value);
    c.hex = buf;
    snprintf(buf, sizeof(buf), "%llu", (unsigned long long)c.value);
    c.dec = buf;
    c.bits.clear();
    for (int b = 0; b < 64; ++b) {
        if (c.value & (1ULL << b)) c.bits.push_back(b);
    }
}

// Nhóm theo TÊN driver (từ vựng). Không phải mapping phần cứng.
std::vector<std::string> lexicalTags(const std::string &name)
{
    std::vector<std::string> tags;
    const std::string low = toLower(name);
    auto has = [&](const char *t) { return low.find(t) != std::string::npos; };
    if (has("dpphy") || has("typec") || has("type-c") || has("usb-drd") || has("usbc") ||
        findWord(low, "atc", false) != std::string::npos) {
        tags.push_back("usb_phy");
    }
    if (findWord(low, "ans", false) != std::string::npos || has("nvme")) tags.push_back("ans_nvme");
    if (findWord(low, "aop", false) != std::string::npos) tags.push_back("aop");
    if (findWord(low, "sep", false) != std::string::npos || has("applesep")) tags.push_back("sep");
    if (findWord(low, "smc", false) != std::string::npos || has("applesmc")) tags.push_back("smc");
    if (has("baseband") || findWord(low, "bb", false) == 0) tags.push_back("baseband");
    if (findWord(low, "i2c", false) != std::string::npos) tags.push_back("i2c");
    if (findWord(low, "pmu", false) != std::string::npos) tags.push_back("pmu");
    return tags;
}

class Extractor {
public:
    Extractor(const std::string &ps, const HCCPanicFacts::FileFields &ff) : _ps(ps), _ff(ff) {}

    PanicFacts run()
    {
        PanicFacts f;
        _f = &f;
        // Log chép từ màn hình có thể ngắt dòng giữa giá trị: gộp khoảng trắng.
        f.format = _ff.format;
        f.bug_type = squeeze(_ff.bug_type);
        f.product = squeeze(_ff.product);
        f.os_version = squeeze(_ff.os_version);
        f.build = squeeze(_ff.build);
        f.timestamp = squeeze(_ff.timestamp);
        f.incident_id = squeeze(_ff.incident_id);
        f.soc_id = squeeze(_ff.soc_id);
        f.field_evidence = _ff.evidence;

        _lines = splitLines(_ps);
        findMessageRegion();
        readMetadata();
        classify();
        readBusyTimeout();
        readWatchdogs();
        readSensors();
        readSmcDetails();
        if (std::find(f.kinds.begin(), f.kinds.end(), PanicKind::SmcPanic) != f.kinds.end()) readAssertTags();
        readKernelTarget();
        readDriversMentioned();
        readNumbers();
        readPanicFlags();
        addNotes();
        HCCPanicFacts::buildSignature(f);
        f.valid = !_ps.empty() || !f.bug_type.empty() || !f.product.empty();
        return f;
    }

private:
    const std::string &_ps;
    const HCCPanicFacts::FileFields &_ff;
    PanicFacts *_f = nullptr;
    std::vector<Line> _lines;
    size_t _msg_first = 0;   // index dòng đầu vùng thông điệp
    size_t _msg_end = 0;     // index sau dòng cuối
    size_t _msg_col = 0;     // cột bắt đầu thông điệp trong dòng đầu
    bool _structured = false;

    bool inMessage(size_t i) const { return i >= _msg_first && i < _msg_end; }

    // Phần thông điệp của dòng i (dòng đầu bỏ "panic(cpu ...): ").
    std::string msgText(size_t i) const
    {
        return (i == _msg_first) ? _lines[i].text.substr(std::min(_msg_col, _lines[i].text.size()))
                                 : _lines[i].text;
    }

    void findMessageRegion()
    {
        PanicFacts &f = *_f;
        _msg_first = 0;
        _msg_col = 0;
        for (size_t i = 0; i < _lines.size(); ++i) {
            const std::string &low = _lines[i].low;
            size_t p = low.find("panic(cpu ");
            if (p == std::string::npos) continue;
            _structured = true;
            _msg_first = i;
            const std::string &t = _lines[i].text;
            f.panic_cpu = atoi(t.c_str() + p + 10);
            size_t c = low.find("caller ", p);
            size_t close = low.find("):", p);
            if (c != std::string::npos && (close == std::string::npos || c < close)) {
                size_t a = c + 7, b = a;
                while (b < t.size() && isxdigit((unsigned char)t[b])) b++;
                if (b < t.size() && (t[b] == 'x' || t[b] == 'X')) {
                    b++;
                    while (b < t.size() && isxdigit((unsigned char)t[b])) b++;
                }
                f.caller_address = t.substr(a, b - a);
                PanicIgnoredNumber ig;
                ig.raw = f.caller_address;
                ig.reason = "caller_address";
                ig.evidence = ev("panicString", _lines[i].number, t);
                f.ignored_numbers.push_back(ig);
            }
            _msg_col = (close == std::string::npos) ? p : close + 2;
            break;
        }
        // Bỏ dòng trống đầu thông điệp.
        while (_msg_first < _lines.size() && trim(msgText(_msg_first)).empty()) {
            _msg_first++;
            _msg_col = 0;
        }
        _msg_end = _msg_first;
        while (_msg_end < _lines.size()) {
            const std::string lt = trim(_lines[_msg_end].low);
            if (_msg_end > _msg_first && isMetadataLine(lt)) break;
            _msg_end++;
        }
        std::string msg;
        for (size_t i = _msg_first; i < _msg_end; ++i) {
            std::string t = trim(msgText(i));
            if (t.empty()) continue;
            if (!msg.empty()) msg += "\n";
            msg += t;
        }
        f.message = msg;
        // "@file.cpp:1234" (PANIC_LOCATION).
        for (size_t p = msg.find('@'); p != std::string::npos; p = msg.find('@', p + 1)) {
            size_t e = p + 1;
            while (e < msg.size() && (isWordChar(msg[e]) || msg[e] == '.' || msg[e] == '/' ||
                                      msg[e] == '-' || msg[e] == ':')) {
                e++;
            }
            std::string loc = msg.substr(p, e - p);
            size_t colon = loc.rfind(':');
            if (colon != std::string::npos && colon + 1 < loc.size() &&
                isdigit((unsigned char)loc[colon + 1]) && loc.find('.') != std::string::npos) {
                f.source_location = loc;
                break;
            }
        }
    }

    void readMetadata()
    {
        PanicFacts &f = *_f;
        auto valueAfter = [](const std::string &t, size_t colon) { return trim(t.substr(colon + 1)); };
        for (size_t i = _msg_end; i < _lines.size(); ++i) {
            const std::string t = trim(_lines[i].text);
            const std::string lt = toLower(t);
            readMailbox(i);
            size_t colon = t.find(':');
            if (colon == std::string::npos) continue;
            if (startsWith(lt, "debugger message:") && f.debugger_message.empty()) {
                f.debugger_message = valueAfter(t, colon);
            } else if (startsWith(lt, "kernel version:") && f.kernel_version.empty()) {
                f.kernel_version = valueAfter(t, colon);
            } else if ((startsWith(lt, "iboot version:") || startsWith(lt, "boot version:")) &&
                       f.iboot_version.empty()) {
                f.iboot_version = valueAfter(t, colon);
            } else if (startsWith(lt, "os version:") && f.os_version_line.empty()) {
                f.os_version_line = valueAfter(t, colon);
            } else if (startsWith(lt, "memory id:") && f.memory_id.empty()) {
                f.memory_id = valueAfter(t, colon);
            } else if (startsWith(lt, "roots installed:") && f.roots_installed.empty()) {
                f.roots_installed = valueAfter(t, colon);
            } else if (startsWith(lt, "secure boot?:") && f.secure_boot.empty()) {
                f.secure_boot = valueAfter(t, colon);
            } else if (startsWith(lt, "paniclog version:") && f.paniclog_version.empty()) {
                f.paniclog_version = valueAfter(t, colon);
            } else if (startsWith(lt, "panicked task") && f.panicked_task.empty()) {
                // "Panicked task 0x...: 123 pages, 4 threads: pid 56: thermalmonitord"
                // (log chép từ màn hình có thể ngắt "pid ..." sang dòng sau).
                std::string pt = t;
                std::string plt = lt;
                if (plt.find("pid ") == std::string::npos && i + 1 < _lines.size()) {
                    pt += " " + trim(_lines[i + 1].text);
                    plt = toLower(pt);
                }
                size_t pid = plt.rfind("pid ");
                if (pid != std::string::npos) {
                    size_t c2 = pt.find(':', pid);
                    if (c2 != std::string::npos) f.panicked_task = trim(pt.substr(c2 + 1));
                }
            } else if (f.firmware_rtkit.empty() && lt.find("rtkit-") != std::string::npos) {
                // "RTKit-2758.40.19.release - Client: AppleSMCFirmware,HIT-..." (có thể có tiền tố)
                size_t r = lt.find("rtkit-");
                size_t sp = t.find_first_of(" ,", r);
                f.firmware_rtkit = t.substr(r, sp == std::string::npos ? std::string::npos : sp - r);
                size_t cl = lt.find("client:");
                if (cl != std::string::npos) f.firmware_client = trim(t.substr(cl + 7));
            }
            if (f.firmware_client.empty() && lt.find("client:") != std::string::npos && isFirmwareDumpLine(lt)) {
                f.firmware_client = trim(t.substr(lt.find("client:") + 7));
            }
        }
    }

    // "0x5447304200001013 [TX] user01 0x00000001169eae98": 4 byte cao in được ASCII -> ascii4.
    // Việc 4 byte đó là khoá SMC là QUAN SÁT, chưa có tài liệu.
    void readMailbox(size_t i)
    {
        PanicFacts &f = *_f;
        const std::string t = trim(_lines[i].text);
        if (!startsWith(t, "0x")) return;
        size_t b = t.find(" [");
        if (b == std::string::npos || b + 4 > t.size() || t[b + 4] != ']') return;
        const std::string dir = t.substr(b + 2, 2);
        if (dir != "TX" && dir != "RX") return;
        PanicMailboxFact m;
        m.dir = dir;
        m.raw = t.substr(0, b);
        const std::string hex = m.raw.substr(2);
        if (hex.size() == 16) {
            std::string a;
            for (int k = 0; k < 4; ++k) {
                char c = (char)strtol(hex.substr(k * 2, 2).c_str(), nullptr, 16);
                if (!isalnum((unsigned char)c)) {
                    a.clear();
                    break;
                }
                a += c;
            }
            m.ascii4 = a;
        }
        m.evidence = ev("panicString", _lines[i].number, _lines[i].text);
        f.mailbox.push_back(m);
    }

    void addKind(PanicKind k, const Line &line)
    {
        PanicFacts &f = *_f;
        if (std::find(f.kinds.begin(), f.kinds.end(), k) != f.kinds.end()) return;
        f.kinds.push_back(k);
        f.kind_evidence.push_back(ev("panicString", line.number, line.text));
    }

    // Nhận diện loại theo dòng (chỉ trong vùng thông điệp: tránh danh sách kext/metadata).
    void classify()
    {
        PanicFacts &f = *_f;
        PanicKind first_kind = PanicKind::Unknown;
        size_t first_line = SIZE_MAX;
        auto note = [&](PanicKind k, size_t i) {
            addKind(k, _lines[i]);
            // Loại chính = loại xuất hiện ở dòng sớm nhất; cùng dòng thì theo thứ tự kiểm tra.
            if (i < first_line) {
                first_line = i;
                first_kind = k;
            }
        };
        for (size_t i = _msg_first; i < _msg_end; ++i) {
            const std::string low = toLower(msgText(i));
            if (low.find("busy timeout[") != std::string::npos) note(PanicKind::BusyTimeout, i);
            if (low.find("iokit daemon (") != std::string::npos && low.find("stall[") != std::string::npos)
                note(PanicKind::IOKitDaemonStall, i);
            if (low.find("userspace watchdog timeout") != std::string::npos)
                note(PanicKind::UserspaceWatchdog, i);
            else if (findWord(low, "watchdog timeout", false) != std::string::npos &&
                     low.find("watchdogd") != std::string::npos)
                note(PanicKind::WatchdogTimeout, i);
            {
                const std::string lt = trim(low);
                for (const char *s : kKernelExceptionStarts) {
                    if (startsWith(lt, s)) {
                        note(PanicKind::KernelException, i);
                        break;
                    }
                }
            }
            if (low.find("smc panic") != std::string::npos || low.find("applesmc") != std::string::npos)
                note(PanicKind::SmcPanic, i);
            if (findWord(low, "aop", true) != std::string::npos || low.find("aop panic") != std::string::npos)
                note(PanicKind::AopPanic, i);
            if (low.find("sep panic") != std::string::npos || low.find("applesep") != std::string::npos ||
                findWord(low, "sep", true) != std::string::npos)
                note(PanicKind::SepPanic, i);
            if (findWord(low, "ans2", true) != std::string::npos || findWord(low, "ans3", true) != std::string::npos ||
                low.find("appleans") != std::string::npos || findWord(low, "nvme", false) != std::string::npos ||
                findWord(low, "ans", true) != std::string::npos)
                note(PanicKind::AnsNvme, i);
            if (low.find("baseband") != std::string::npos || low.find("bbpanic") != std::string::npos)
                note(PanicKind::Baseband, i);
            if (findWord(low, "i2c", false) != std::string::npos) note(PanicKind::I2c, i);
            if (findWord(low, "pmu", false) != std::string::npos) note(PanicKind::Pmu, i);
        }
        if (first_kind == PanicKind::Unknown && _structured && !f.message.empty()) {
            // Panic kernel có thông điệp nhưng không thuộc nhóm nào đã biết.
            note(PanicKind::KernelGeneric, _msg_first);
        }
        f.primary_kind = first_kind;
    }

    void addDriver(const std::string &name, const char *role, const std::string &state, const Line &line)
    {
        PanicFacts &f = *_f;
        for (const auto &d : f.drivers) {
            if (d.name == name && d.role == role) return;
        }
        PanicDriverFact d;
        d.name = name;
        d.role = role;
        d.state = state;
        d.tags = lexicalTags(name);
        d.evidence = ev("panicString", line.number, line.text);
        f.drivers.push_back(d);
    }

    // "busy timeout[N], (Ns): [multiple entries ..., IOKit termination queue depth D: ]'a' (x,y), 'b' (x,y)"
    void readBusyTimeout()
    {
        PanicFacts &f = *_f;
        for (size_t i = _msg_first; i < _msg_end; ++i) {
            if (toLower(msgText(i)).find("busy timeout[") == std::string::npos &&
                toLower(msgText(i)).find("stall[") == std::string::npos) {
                continue;
            }
            // Danh sách entry có thể bị ngắt dòng (log chép từ màn hình): nối các dòng tới hết
            // vùng thông điệp; dòng kết thúc bằng '-' nối liền ("'smc-" + "charger'").
            std::string t = msgText(i);
            for (size_t j = i + 1; j < _msg_end; ++j) {
                std::string nx = trim(msgText(j));
                if (nx.empty()) continue;
                if (!t.empty() && t.back() == '-') t += nx;
                else t += " " + nx;
            }
            const std::string low = toLower(t);
            size_t p = low.find("busy timeout[");
            const char *what = "busy_timeout";
            if (p == std::string::npos) {
                p = low.find("stall[");
                if (p == std::string::npos || low.find("iokit daemon (") == std::string::npos) continue;
                what = "iokit_daemon_stall";
            }
            PanicTimeoutFact to;
            to.what = what;
            size_t lb = t.find('[', p);
            if (lb != std::string::npos) to.loops = atol(t.c_str() + lb + 1);
            size_t lp = t.find("(", lb == std::string::npos ? p : lb);
            if (lp != std::string::npos) {
                char *end = nullptr;
                long secs = strtol(t.c_str() + lp + 1, &end, 10);
                if (end && *end == 's') to.seconds = secs;
            }
            to.evidence = ev("panicString", _lines[i].number, t);
            f.timeouts.push_back(to);

            if (low.find("registry root held busy") != std::string::npos) {
                f.notes.push_back("busy timeout: registry root held busy (không có driver cụ thể)");
            }
            // Các entry 'name' (x,y): (x,y) là __state[0..1] của IOService -> không phải mã lỗi.
            size_t q = t.find("):", lp == std::string::npos ? p : lp);
            size_t pos = (q == std::string::npos) ? p : q + 2;
            while ((pos = t.find('\'', pos)) != std::string::npos) {
                size_t close = t.find('\'', pos + 1);
                if (close == std::string::npos) break;
                std::string name = t.substr(pos + 1, close - pos - 1);
                std::string state;
                size_t k = close + 1;
                while (k < t.size() && t[k] == ' ') k++;
                if (k < t.size() && t[k] == '(') {
                    size_t rp = t.find(')', k);
                    std::string cand = (rp == std::string::npos) ? t.substr(k) : t.substr(k, rp - k + 1);
                    if (isIOServiceState(cand)) {
                        state = cand;
                        PanicIgnoredNumber ig;
                        ig.raw = state;
                        ig.reason = "ioservice_state";
                        ig.evidence = ev("panicString", _lines[i].number, t);
                        f.ignored_numbers.push_back(ig);
                        k = rp + 1;
                    } else if (!name.empty()) {
                        f.notes.push_back("busy entry '" + name + "': trạng thái (x,y) bị cắt/không đầy đủ");
                    }
                }
                if (!name.empty()) {
                    Line l{t, toLower(t), _lines[i].number};
                    addDriver(name, "busy_entry", state, l);
                }
                pos = k;
            }
            break;  // một panic chỉ có một dòng busy timeout
        }
    }

    void readWatchdogs()
    {
        PanicFacts &f = *_f;
        for (size_t i = _msg_first; i < _msg_end; ++i) {
            const std::string t = msgText(i);
            const std::string low = toLower(t);
            const bool user = low.find("userspace watchdog timeout") != std::string::npos;
            const bool wd = !user && low.find("watchdog timeout") != std::string::npos &&
                            low.find("watchdogd") != std::string::npos;
            if (user || wd) {
                PanicTimeoutFact to;
                to.what = user ? "userspace_watchdog" : "watchdog";
                size_t from = low.find("checkins from ");
                if (from != std::string::npos) {
                    size_t a = from + 14;
                    size_t in = low.find(" in ", a);
                    std::string list = t.substr(a, (in == std::string::npos ? t.size() : in) - a);
                    // "a, b and c" / "a, b"
                    std::string cur;
                    auto flush = [&]() {
                        std::string n = trim(cur);
                        if (startsWith(n, "and ")) n = trim(n.substr(4));
                        if (!n.empty() && n.find(' ') == std::string::npos) {
                            addDriver(n, "watchdog_daemon", "", _lines[i]);
                            if (to.subject.empty()) to.subject = n;
                        }
                        cur.clear();
                    };
                    for (char c : list) {
                        if (c == ',') flush();
                        else cur += c;
                    }
                    flush();
                    if (in != std::string::npos) {
                        char *end = nullptr;
                        long secs = strtol(t.c_str() + in + 4, &end, 10);
                        if (end && strncmp(end, " second", 7) == 0) to.seconds = secs;
                    }
                }
                to.evidence = ev("panicString", _lines[i].number, _lines[i].text);
                f.timeouts.push_back(to);
            }
            // "service: thermalmonitord, total successful checkins ..."
            if (startsWith(trim(low), "service: ")) {
                std::string rest = trim(t).substr(9);
                size_t comma = rest.find(',');
                std::string n = trim(rest.substr(0, comma));
                if (!n.empty() && n.find(' ') == std::string::npos) {
                    addDriver(n, "watchdog_daemon", "", _lines[i]);
                }
            }
        }
    }

    void readSensors()
    {
        PanicFacts &f = *_f;
        for (size_t i = _msg_first; i < _msg_end; ++i) {
            const std::string t = msgText(i);
            const std::string low = toLower(t);
            size_t p = low.find("missing sensor");
            if (p != std::string::npos) {
                size_t colon = t.find(':', p);
                if (colon != std::string::npos) {
                    std::string list = t.substr(colon + 1);
                    std::string cur;
                    auto flush = [&]() {
                        std::string n = trim(cur);
                        cur.clear();
                        if (n.empty()) return;
                        for (const auto &s : f.sensors) {
                            if (s.name == n && s.context == "missing_sensor") return;
                        }
                        PanicSensorFact s;
                        s.name = n;
                        s.context = "missing_sensor";
                        s.evidence = ev("panicString", _lines[i].number, _lines[i].text);
                        f.sensors.push_back(s);
                    };
                    for (char c : list) {
                        if (c == ',' || c == ' ' || c == '\t') flush();
                        else cur += c;
                    }
                    flush();
                }
            }
            // "S.sensor[N]" (định dạng quan sát được; ý nghĩa chỉ số chưa có tài liệu).
            for (size_t q = low.find("s.sensor["); q != std::string::npos; q = low.find("s.sensor[", q + 1)) {
                size_t e = low.find(']', q);
                if (e == std::string::npos || e - q > 14) continue;
                PanicSensorFact s;
                s.name = t.substr(q, e - q + 1);
                s.context = "indexed_sensor";
                s.index = atoi(t.c_str() + q + 9);
                s.evidence = ev("panicString", _lines[i].number, _lines[i].text);
                bool dup = false;
                for (const auto &x : f.sensors) dup = dup || (x.name == s.name);
                if (!dup) f.sensors.push_back(s);
            }
            // i2cN
            for (size_t q = findWord(low, "i2c", false); q != std::string::npos;
                 q = findWord(low, "i2c", false, q + 3)) {
                if (q + 3 < low.size() && isdigit((unsigned char)low[q + 3])) {
                    int bus = atoi(low.c_str() + q + 3);
                    if (std::find(f.i2c_buses.begin(), f.i2c_buses.end(), bus) == f.i2c_buses.end())
                        f.i2c_buses.push_back(bus);
                }
            }
        }
    }

    // "ASSERT: target/d47/target.cpp:194: 0, SMC BSC failure, ..." và
    // "S.sensor array 0 - 7 is 0, 1572864, 0, ..." (định dạng quan sát được trong SMC PANIC thật).
    void readSmcDetails()
    {
        PanicFacts &f = *_f;
        for (size_t i = _msg_first; i < _msg_end; ++i) {
            const std::string t = trim(msgText(i));
            const std::string low = toLower(t);
            size_t a = low.find("assert: ");
            if (a != std::string::npos && f.assert_location.empty()) {
                // <file>:<line>: <giá trị>, <chữ>, ...
                std::string rest = t.substr(a + 8);
                size_t c1 = rest.find(':');
                size_t c2 = (c1 == std::string::npos) ? c1 : rest.find(':', c1 + 1);
                if (c2 != std::string::npos && isdigit((unsigned char)rest[c1 + 1])) {
                    f.assert_location = rest.substr(0, c2);
                    // "target/d47/target.cpp" -> "d47"
                    size_t tp = f.assert_location.find("target/");
                    if (tp != std::string::npos) {
                        size_t te = f.assert_location.find('/', tp + 7);
                        if (te != std::string::npos && te > tp + 7) f.smc_target = f.assert_location.substr(tp + 7, te - tp - 7);
                    }
                    std::string after = rest.substr(c2 + 1);
                    size_t comma = after.find(',');
                    if (comma != std::string::npos) {
                        std::string txt = after.substr(comma + 1);
                        size_t c3 = txt.find(',');
                        f.assert_text = trim(c3 == std::string::npos ? txt : txt.substr(0, c3));
                    }
                }
            }
            size_t arr = low.find(".sensor array ");
            if (arr != std::string::npos) {
                size_t nstart = arr;
                while (nstart > 0 && isalpha((unsigned char)t[nstart - 1])) nstart--;
                PanicSensorArrayFact sa;
                sa.name = t.substr(nstart, arr - nstart) + ".sensor";
                const char *p = t.c_str() + arr + 14;
                char *end = nullptr;
                sa.first_index = (int)strtol(p, &end, 10);
                const char *dash = strchr(end, '-');
                if (dash) sa.last_index = (int)strtol(dash + 1, &end, 10);
                const char *is = strstr(end, " is ");
                if (is) {
                    p = is + 4;
                    while (*p) {
                        while (*p == ' ' || *p == ',') p++;
                        if (!isdigit((unsigned char)*p)) break;
                        sa.values.push_back(strtoull(p, &end, 10));
                        p = end;
                    }
                }
                sa.evidence = ev("panicString", _lines[i].number, _lines[i].text);
                bool dup = false;
                for (const auto &x : f.sensor_arrays) {
                    dup = dup || (x.name == sa.name && x.values == sa.values);  // khối ASSERT lặp lại
                }
                if (dup) continue;
                f.sensor_arrays.push_back(sa);
                // Phần tử khác 0 -> mã có nhãn "S.sensor[i]" (dec -> hex -> bit).
                for (size_t k = 0; k < sa.values.size(); ++k) {
                    if (sa.values[k] == 0) continue;
                    const int idx = sa.first_index + (int)k;
                    PanicCodeFact c;
                    c.label = sa.name + "[" + std::to_string(idx) + "]";
                    c.raw = std::to_string(sa.values[k]);
                    c.base = 10;
                    c.value = sa.values[k];
                    c.context = "smc";
                    c.in_message = true;
                    c.evidence = sa.evidence;
                    fillNumber(c);
                    f.codes.push_back(c);
                    PanicSensorFact sf;
                    sf.name = c.label;
                    sf.context = "sensor_array_nonzero";
                    sf.index = idx;
                    sf.evidence = sa.evidence;
                    f.sensors.push_back(sf);
                }
            }
        }
    }

    // Mã 4 ký tự in hoa (vd TAOP, TAOC) trên các dòng ASSERT / SMC trong thông điệp. Định dạng chưa
    // có tài liệu: chỉ ghi nhận để so khớp case, không gán ý nghĩa.
    void readAssertTags()
    {
        PanicFacts &f = *_f;
        static const char *stop[] = {"FAIL", "NULL", "TRUE", "DONE", "BUSY", "INIT", "READ", "SEND", "WAIT"};
        for (size_t i = _msg_first; i < _msg_end; ++i) {
            const std::string t = msgText(i);
            const std::string low = toLower(t);
            if (low.find("assert") == std::string::npos && low.find("smc") == std::string::npos) continue;
            size_t k = 0;
            while (k < t.size()) {
                while (k < t.size() && !isalnum((unsigned char)t[k])) k++;
                size_t a = k;
                while (k < t.size() && (isalnum((unsigned char)t[k]) || t[k] == '_')) k++;
                if (k - a != 4) continue;
                std::string tok = t.substr(a, 4);
                int letters = 0;
                bool ok = true;
                for (char c : tok) {
                    if (isupper((unsigned char)c)) letters++;
                    else if (!isdigit((unsigned char)c)) ok = false;
                }
                // Bỏ phần đường dẫn / tên file (vd "d47/target.cpp") và từ thông dụng.
                if (!ok || letters < 2 || (a > 0 && (t[a - 1] == '/' || t[a - 1] == '.'))) continue;
                bool stopword = false;
                for (const char *w : stop) stopword = stopword || tok == w;
                if (stopword) continue;
                if (std::find(f.assert_tags.begin(), f.assert_tags.end(), tok) == f.assert_tags.end())
                    f.assert_tags.push_back(tok);
            }
        }
        if (!f.assert_tags.empty()) {
            f.notes.push_back("ASSERT tag (mã 4 ký tự) là QUAN SÁT, ý nghĩa UNKNOWN - chỉ dùng để so khớp case");
        }
    }

    // "RELEASE_ARM64_T8101" (tên cấu hình kernel, trong trường kernel hoặc "Kernel version:").
    void readKernelTarget()
    {
        PanicFacts &f = *_f;
        const std::string src = squeeze(_ff.kernel) + " " + f.kernel_version + " " + _ps.substr(0, 20000);
        size_t p = src.find("RELEASE_ARM64_");
        if (p == std::string::npos) return;
        size_t a = p + 14, e = a;
        while (e < src.size() && isalnum((unsigned char)src[e])) e++;
        f.kernel_target = src.substr(a, e - a);
    }

    // Token trong thông điệp có tên thuộc nhóm từ vựng (atc*/dpphy/usb-drd/ANS/AOP...).
    void readDriversMentioned()
    {
        PanicFacts &f = *_f;
        for (size_t i = _msg_first; i < _msg_end; ++i) {
            const std::string t = msgText(i);
            size_t k = 0;
            while (k < t.size()) {
                while (k < t.size() && !isalnum((unsigned char)t[k])) k++;
                size_t a = k;
                while (k < t.size() && (isalnum((unsigned char)t[k]) || t[k] == '-' || t[k] == '_')) k++;
                if (k == a) break;
                std::string tok = t.substr(a, k - a);
                while (!tok.empty() && tok.back() == '-') tok.pop_back();
                if (tok.size() < 3) continue;
                // Tên file nguồn ("smc_sensor.c", "IOService.cpp") không phải driver.
                if (k + 1 < t.size() && t[k] == '.' && isalpha((unsigned char)t[k + 1])) continue;
                if (lexicalTags(tok).empty()) continue;
                const std::string lt = toLower(tok);
                // Bỏ từ chung chung (đã có loại panic riêng) và tiền tố "0x...".
                if (lt == "panic" || startsWith(lt, "0x") || lt.find("firmware") != std::string::npos) continue;
                bool known = false;
                for (const auto &d : f.drivers) {
                    // Mảnh của tên đã có (busy entry bị ngắt dòng "'smc-" / "charger'") -> bỏ.
                    known = known || d.name == tok ||
                            (d.role == "busy_entry" && toLower(d.name).find(lt) != std::string::npos);
                }
                if (!known) addDriver(tok, "mentioned", "", _lines[i]);
            }
        }
        bool usb = false;
        for (const auto &d : f.drivers) {
            usb = usb || std::find(d.tags.begin(), d.tags.end(), "usb_phy") != d.tags.end();
        }
        if (usb) {
            for (const auto &d : f.drivers) {
                if (std::find(d.tags.begin(), d.tags.end(), "usb_phy") != d.tags.end()) {
                    Line l{d.evidence.excerpt, toLower(d.evidence.excerpt), d.evidence.line};
                    addKind(PanicKind::UsbPhy, l);
                    break;
                }
            }
        }
    }

    // Số trong vùng thông điệp: mã lỗi / thanh ghi / địa chỉ (bị loại).
    void readNumbers()
    {
        PanicFacts &f = *_f;
        for (size_t i = _msg_first; i < _msg_end; ++i) {
            const std::string t = msgText(i);
            const std::string low = toLower(t);
            const std::string ctx = lineContext(low);
            // Chuỗi số liền nhau ("sks : 0x1f2e8 0x0 0x0"): số sau mang nhãn của số đầu + "#n".
            std::string run_label;
            int run_index = 0;
            auto labelFor = [&](size_t pos) {
                std::string l = labelBefore(t, pos);
                const bool numeric = !l.empty() && (isdigit((unsigned char)l[0]) ||
                                                    (l.size() > 1 && l[0] == '-' && isdigit((unsigned char)l[1])));
                if (numeric && !run_label.empty()) return run_label + "#" + std::to_string(++run_index);
                run_label = l;
                run_index = 1;
                return l;
            };
            size_t k = 0;
            while (k < t.size()) {
                // Hex 0x...
                if (t[k] == '0' && k + 1 < t.size() && (t[k + 1] == 'x' || t[k + 1] == 'X') &&
                    (k == 0 || !isWordChar(t[k - 1]))) {
                    size_t e = k + 2;
                    while (e < t.size() && isxdigit((unsigned char)t[e])) e++;
                    if (e == k + 2) {
                        k = e;
                        continue;
                    }
                    handleNumber(t.substr(k, e - k), 16, labelFor(k), ctx, _lines[i]);
                    k = e;
                    continue;
                }
                // Số thập phân có nhãn mã lỗi (vd "status 5", "error=-536870212").
                if ((isdigit((unsigned char)t[k]) || (t[k] == '-' && k + 1 < t.size() && isdigit((unsigned char)t[k + 1]))) &&
                    (k == 0 || !isWordChar(t[k - 1]))) {
                    size_t e = k + 1;
                    while (e < t.size() && isdigit((unsigned char)t[e])) e++;
                    const bool part_of_word = e < t.size() && (isalpha((unsigned char)t[e]) || t[e] == '.' || t[e] == ',');
                    const std::string label = labelBefore(t, k);
                    const char sep = (k > 0) ? t[k - 1] : ' ';
                    if (!part_of_word && labelIsCode(toLower(label)) && (sep == ' ' || sep == ':' || sep == '=')) {
                        handleNumber(t.substr(k, e - k), 10, label, ctx, _lines[i]);
                    }
                    k = e;
                    continue;
                }
                k++;
            }
        }
        if (f.esr_ec < 0) {
            for (const auto &r : f.registers) {
                if (r.name == "esr") {
                    f.esr_ec = (int)((r.value >> 26) & 0x3F);
                    f.esr_ec_name = esrClassName(f.esr_ec);
                    break;
                }
            }
        }
    }

    std::string lineContext(const std::string &low) const
    {
        if (low.find("smc") != std::string::npos) return "smc";
        if (findWord(low, "aop", false) != std::string::npos) return "aop";
        if (findWord(low, "sep", false) != std::string::npos) return "sep";
        if (findWord(low, "ans", false) != std::string::npos || low.find("nvme") != std::string::npos) return "ans";
        if (low.find("baseband") != std::string::npos) return "baseband";
        if (findWord(low, "i2c", false) != std::string::npos) return "i2c";
        if (findWord(low, "pmu", false) != std::string::npos) return "pmu";
        return "message";
    }

    void handleNumber(const std::string &raw, int base, const std::string &label, const std::string &ctx,
                      const Line &line)
    {
        PanicFacts &f = *_f;
        const std::string ll = toLower(label);
        const bool neg = !raw.empty() && raw[0] == '-';
        const size_t digits = (base == 16) ? raw.size() - 2 : raw.size() - (neg ? 1 : 0);
        uint64_t v = 0;
        if (base == 16) {
            v = (digits > 16) ? UINT64_MAX : strtoull(raw.c_str() + 2, nullptr, 16);
        } else if (neg) {
            long long sv = strtoll(raw.c_str(), nullptr, 10);
            // IOReturn & co. in dạng số âm 32-bit -> giá trị không dấu 32-bit.
            v = (sv >= INT32_MIN) ? (uint64_t)(uint32_t)(int32_t)sv : (uint64_t)sv;
        } else {
            v = strtoull(raw.c_str(), nullptr, 10);
        }

        if (isRegisterName(ll)) {
            for (const auto &x : f.registers) {
                if (x.name == ll && x.raw == raw) return;  // pc/lr lặp ở dòng đầu và register dump
            }
            PanicRegisterFact r;
            r.name = ll;
            r.raw = raw;
            r.value = v;
            r.evidence = ev("panicString", line.number, line.text);
            f.registers.push_back(r);
            return;
        }
        PanicIgnoredNumber ig;
        ig.raw = raw;
        ig.evidence = ev("panicString", line.number, line.text);
        if (ll == "caller") {
            ig.reason = "caller_address";
        } else if (ll.find("_ctrl") != std::string::npos || ll.find("mailbox") != std::string::npos ||
                   ll.find("inbox") != std::string::npos || ll.find("outbox") != std::string::npos ||
                   startsWith(ll, "akf_") || startsWith(ll, "kic_")) {
            ig.reason = "firmware_register";  // thanh ghi mailbox coprocessor (dump RTKit), không phải mã lỗi
        } else if (base == 16 && (digits > 10 || v >= 0xFFFFFF0000000000ULL)) {
            ig.reason = "kernel_address";  // con trỏ 64-bit (vd 0xfffffff0...)
        } else if (ll == "state" || ll == "thread" || ll == "task" || ll == "va" || ll == "addr" ||
                   ll == "address" || ll == "ptr" || ll == "frame" || ll == "kext" || ll == "base") {
            ig.reason = "kernel_address";
        }
        if (!ig.reason.empty()) {
            // Caller đã ghi ở findMessageRegion: tránh trùng.
            for (const auto &x : f.ignored_numbers) {
                if (x.raw == raw && x.reason == ig.reason) return;
            }
            f.ignored_numbers.push_back(ig);
            return;
        }
        PanicCodeFact c;
        c.label = label;
        c.raw = raw;
        c.base = base;
        c.value = v;
        c.labeled_bitmask = labelIsBitmask(ll);
        c.context = ctx;
        c.in_message = true;
        c.evidence = ev("panicString", line.number, line.text);
        fillNumber(c);
        for (const auto &x : f.codes) {
            if (x.value == c.value && x.label == c.label) return;
        }
        f.codes.push_back(c);
    }

    void readPanicFlags()
    {
        PanicFacts &f = *_f;
        if (_ff.panic_flags.empty()) return;
        PanicCodeFact c;
        c.label = "panicFlags";
        c.raw = _ff.panic_flags;
        const bool hex = startsWith(toLower(c.raw), "0x");
        c.base = hex ? 16 : 10;
        c.value = strtoull(c.raw.c_str() + (hex ? 2 : 0), nullptr, hex ? 16 : 10);
        c.labeled_bitmask = true;  // tên trường là "flags"
        c.context = "payload";
        c.in_message = false;
        c.evidence = ev("payload.panicFlags", 0, c.raw);
        fillNumber(c);
        f.codes.push_back(c);
    }

    void addNotes()
    {
        PanicFacts &f = *_f;
        if (f.product.empty()) f.notes.push_back("product: UNKNOWN - log không có mã máy (iPhoneN,M)");
        if (f.bug_type.empty()) f.notes.push_back("bug_type: UNKNOWN - không có header .ips");
        if (!_structured) f.notes.push_back("Không thấy dòng 'panic(cpu N caller ...)': thông điệp lấy từ đầu log");
        if (f.primary_kind == PanicKind::Unknown) f.notes.push_back("Loại panic: UNKNOWN - không khớp mẫu đã biết");
        for (const auto &s : f.sensors) {
            if (s.context == "indexed_sensor") {
                f.notes.push_back("Ý nghĩa " + s.name + ": UNKNOWN - định dạng S.sensor[N] chưa có tài liệu");
            }
        }
        if (!f.sensors.empty()) {
            f.notes.push_back("Vị trí/linh kiện của sensor: NEEDS VERIFICATION - cần Knowledge Base theo model");
        }
        for (const auto &c : f.codes) {
            if (c.bits.size() > 1 || c.labeled_bitmask) {
                f.notes.push_back("Ý nghĩa từng bit của " + c.label + " " + c.hex +
                                  ": UNKNOWN - chưa có bảng bit đã kiểm chứng");
            }
        }
        if (!f.sensor_arrays.empty()) {
            f.notes.push_back("Ý nghĩa từng phần tử/bit của sensor array: UNKNOWN - chưa có tài liệu, cần KB theo model");
        }
        bool ascii = false;
        for (const auto &m : f.mailbox) ascii = ascii || !m.ascii4.empty();
        if (ascii) {
            f.notes.push_back("Mailbox: 4 byte cao đọc được dạng ASCII (vd khoá SMC) - QUAN SÁT, chưa có tài liệu");
        }
        if (!f.firmware_rtkit.empty()) {
            f.notes.push_back("Dump firmware RTKit (call stack, mailbox, *_err_sts) không dùng làm mã lỗi");
        }
        if (std::find(f.kinds.begin(), f.kinds.end(), PanicKind::Pmu) != f.kinds.end()) {
            f.notes.push_back("'PMU' mơ hồ: Power Management Unit hoặc ARM Performance Monitoring Unit - NEEDS VERIFICATION");
        }
        bool tagged = false;
        for (const auto &d : f.drivers) tagged = tagged || !d.tags.empty();
        if (tagged) {
            f.notes.push_back("Tag driver là nhóm theo TÊN, chưa phải mapping phần cứng (bước 2)");
        }
        if (!f.registers.empty() && f.esr_ec >= 0 && f.esr_ec_name.empty()) {
            f.notes.push_back("ESR.EC chưa có trong bảng tên đã biết");
        }
    }
};

std::string joinSorted(std::vector<std::string> v, size_t cap)
{
    for (auto &s : v) s = toLower(s);
    std::sort(v.begin(), v.end());
    v.erase(std::unique(v.begin(), v.end()), v.end());
    if (v.size() > cap) v.resize(cap);
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += ",";
        out += v[i];
    }
    return out.empty() ? "-" : out;
}

}  // namespace

const char *panicKindName(PanicKind kind)
{
    switch (kind) {
        case PanicKind::BusyTimeout: return "busy_timeout";
        case PanicKind::IOKitDaemonStall: return "iokit_daemon_stall";
        case PanicKind::UserspaceWatchdog: return "userspace_watchdog";
        case PanicKind::WatchdogTimeout: return "watchdog_timeout";
        case PanicKind::SmcPanic: return "smc";
        case PanicKind::AopPanic: return "aop";
        case PanicKind::SepPanic: return "sep";
        case PanicKind::AnsNvme: return "ans_nvme";
        case PanicKind::Baseband: return "baseband";
        case PanicKind::Pmu: return "pmu";
        case PanicKind::I2c: return "i2c";
        case PanicKind::UsbPhy: return "usb_phy";
        case PanicKind::KernelException: return "kernel_exception";
        case PanicKind::KernelGeneric: return "kernel_generic";
        case PanicKind::Unknown: break;
    }
    return "unknown";
}

PanicFacts HCCPanicFacts::extract(const std::string &panic_string, const FileFields &fields)
{
    Extractor x(panic_string, fields);
    return x.run();
}

void HCCPanicFacts::buildSignature(PanicFacts &f)
{
    std::vector<std::string> kinds, drivers, sensors, codes, daemons;
    for (auto k : f.kinds) kinds.push_back(panicKindName(k));
    for (const auto &d : f.drivers) {
        if (d.role == "watchdog_daemon") daemons.push_back(d.name);
        else drivers.push_back(d.name);
    }
    for (const auto &s : f.sensors) sensors.push_back(s.name);
    for (const auto &c : f.codes) {
        if (c.in_message && c.value != 0) codes.push_back(c.hex);
    }
    const std::string bug = f.bug_type.empty() ? "UNKNOWN" : f.bug_type;
    std::string esr = "-";
    if (f.esr_ec >= 0) {
        char b[16];
        snprintf(b, sizeof(b), "0x%02x", f.esr_ec & 0x3F);
        esr = b;
    }
    const std::string rest = "|" + bug + "|" + panicKindName(f.primary_kind) + "|k=" + joinSorted(kinds, 8) +
                             "|d=" + joinSorted(drivers, 8) + "|s=" + joinSorted(sensors, 12) +
                             "|c=" + joinSorted(codes, 6) + "|w=" + joinSorted(daemons, 4) + "|x=" + esr +
                             "|a=" + (f.assert_text.empty() ? std::string("-") : toLower(squeeze(f.assert_text)));
    std::string extra;
    if (!f.smc_target.empty()) extra += "|t=" + toLower(f.smc_target);
    if (!f.assert_tags.empty()) {
        std::vector<std::string> tags = f.assert_tags;
        std::sort(tags.begin(), tags.end());  // giữ chữ hoa: là mã
        std::string g;
        for (size_t i = 0; i < tags.size(); ++i) g += (i ? "," : "") + tags[i];
        extra += "|g=" + g;
    }
    f.signature = "v1|" + (f.product.empty() ? std::string("UNKNOWN") : f.product) + rest + extra;
    f.signature_generic = "v1|*" + rest + extra;
}

std::string HCCPanicFacts::describe(const PanicFacts &f)
{
    std::string s;
    auto line = [&](const std::string &k, const std::string &v) {
        if (!v.empty()) s += k + ": " + v + "\n";
    };
    line("format", f.format);
    line("bug_type", f.bug_type);
    line("product", f.product);
    line("os_version", f.os_version);
    line("build", f.build);
    if (f.panic_cpu >= 0) line("panic_cpu", std::to_string(f.panic_cpu));
    line("caller_address", f.caller_address);
    line("message", f.message.substr(0, 300));
    line("source_location", f.source_location);
    line("debugger_message", f.debugger_message);
    line("panicked_task", f.panicked_task);
    line("kernel_target", f.kernel_target);
    line("iboot_version", f.iboot_version);
    line("assert_location", f.assert_location);
    line("assert_text", f.assert_text);
    line("smc_target", f.smc_target);
    if (!f.assert_tags.empty()) {
        std::string g;
        for (size_t i = 0; i < f.assert_tags.size(); ++i) g += (i ? "," : "") + f.assert_tags[i];
        line("assert_tags", g);
    }
    line("firmware_rtkit", f.firmware_rtkit);
    line("firmware_client", f.firmware_client);
    line("primary_kind", panicKindName(f.primary_kind));
    for (size_t i = 0; i < f.kinds.size(); ++i) {
        s += "kind: " + std::string(panicKindName(f.kinds[i])) + "  <- L" +
             std::to_string(f.kind_evidence[i].line) + "\n";
    }
    for (const auto &d : f.drivers) {
        s += "driver: " + d.name + " role=" + d.role;
        if (!d.state.empty()) s += " state=" + d.state;
        for (const auto &t : d.tags) s += " tag=" + t;
        s += "  <- L" + std::to_string(d.evidence.line) + "\n";
    }
    for (const auto &x : f.sensors) {
        s += "sensor: " + x.name + " ctx=" + x.context + "  <- L" + std::to_string(x.evidence.line) + "\n";
    }
    for (const auto &t : f.timeouts) {
        s += "timeout: " + t.what + " seconds=" + std::to_string(t.seconds);
        if (t.loops >= 0) s += " loops=" + std::to_string(t.loops);
        if (!t.subject.empty()) s += " subject=" + t.subject;
        s += "\n";
    }
    for (const auto &c : f.codes) {
        s += "code: " + c.label + " raw=" + c.raw + " hex=" + c.hex + " dec=" + c.dec + " bits=[";
        for (size_t i = 0; i < c.bits.size(); ++i) s += (i ? "," : "") + std::to_string(c.bits[i]);
        s += "] ctx=" + c.context + (c.labeled_bitmask ? " bitmask" : "") + "\n";
    }
    for (const auto &r : f.registers) s += "register: " + r.name + "=" + r.raw + "\n";
    for (const auto &a : f.sensor_arrays) {
        s += "sensor_array: " + a.name + " " + std::to_string(a.first_index) + "-" + std::to_string(a.last_index) + " =";
        for (auto v : a.values) s += " " + std::to_string(v);
        s += "  <- L" + std::to_string(a.evidence.line) + "\n";
    }
    for (const auto &m : f.mailbox) {
        s += "mailbox: " + m.dir + " " + m.raw + (m.ascii4.empty() ? std::string() : " ascii=" + m.ascii4) + "\n";
    }
    if (f.esr_ec >= 0) {
        char b[16];
        snprintf(b, sizeof(b), "0x%02X", f.esr_ec);
        s += "esr_ec: " + std::string(b) + " " + f.esr_ec_name + "\n";
    }
    for (int b : f.i2c_buses) s += "i2c_bus: " + std::to_string(b) + "\n";
    for (const auto &x : f.ignored_numbers) s += "ignored: " + x.raw + " (" + x.reason + ")\n";
    for (const auto &n : f.notes) s += "note: " + n + "\n";
    s += "signature: " + f.signature + "\n";
    s += "signature_generic: " + f.signature_generic + "\n";
    return s;
}
