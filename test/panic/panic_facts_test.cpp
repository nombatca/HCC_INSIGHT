// Test host cho Panic Parser v2 (HCCPanicParser + HCCPanicFacts).
//
//   panic_facts_test <samples_dir>        chạy mọi *.ips có file .expect cùng tên + kiểm tra chung
//   panic_facts_test --dump <file.ips>    in toàn bộ fact (dùng để viết .expect cho .ips thật)
//
// File .expect: mỗi dòng một kiểm tra trên kết quả HCCPanicFacts::describe():
//   has: <dòng>       describe() phải chứa đúng dòng này
//   lacks: <chuỗi>    describe() không được chứa chuỗi này
//   same_signature: <tên mẫu khác>   signature phải bằng signature của mẫu đó
//   # ...             chú thích
#include "HCCPanicParser.hpp"

#include <dirent.h>

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

static std::string readAll(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static int g_fail = 0;
static int g_checks = 0;

static void check(bool ok, const std::string &sample, const std::string &what)
{
    g_checks++;
    if (!ok) {
        g_fail++;
        printf("  FAIL [%s] %s\n", sample.c_str(), what.c_str());
    }
}

// Kiểm tra chung cho mọi mẫu (kể cả .ips thật không có .expect).
static void invariants(const std::string &name, const ParsedPanic &p)
{
    const PanicFacts &f = p.facts;
    check(f.valid, name, "facts.valid");
    check(f.signature.rfind("v1|", 0) == 0, name, "signature bắt đầu bằng v1|");
    // Không mã lỗi nào trùng số đã bị loại (địa chỉ, caller, trạng thái IOService, thanh ghi).
    for (const auto &c : f.codes) {
        for (const auto &ig : f.ignored_numbers) {
            check(c.raw != ig.raw, name, "code " + c.raw + " trùng số bị loại (" + ig.reason + ")");
        }
        for (const auto &r : f.registers) {
            check(c.raw != r.raw, name, "code " + c.raw + " là thanh ghi " + r.name);
        }
        check(!c.evidence.excerpt.empty(), name, "code " + c.raw + " thiếu evidence");
        check(c.raw.size() <= 12, name, "code " + c.raw + " dài như địa chỉ");
    }
    // Mọi fact có evidence.
    for (const auto &d : f.drivers) check(!d.evidence.excerpt.empty(), name, "driver " + d.name + " thiếu evidence");
    for (const auto &s : f.sensors) check(!s.evidence.excerpt.empty(), name, "sensor " + s.name + " thiếu evidence");
    for (const auto &t : f.timeouts) check(!t.evidence.excerpt.empty(), name, "timeout " + t.what + " thiếu evidence");
    for (const auto &e : f.kind_evidence) check(!e.excerpt.empty(), name, "kind thiếu evidence");
    check(f.kinds.size() == f.kind_evidence.size(), name, "kinds/kind_evidence lệch");
    // error_codes cũ (CODE HEX gợi ý, input AI) không còn địa chỉ / trạng thái.
    for (const auto &ec : p.error_codes) {
        for (const auto &ig : f.ignored_numbers) {
            check(ec.rfind(ig.raw + " ", 0) != 0, name, "error_codes chứa số bị loại " + ig.raw);
        }
    }
    // Signature không chứa địa chỉ kernel / caller.
    check(f.signature.find("0xffff") == std::string::npos, name, "signature chứa địa chỉ kernel");
    if (!f.caller_address.empty()) {
        check(f.signature.find(f.caller_address) == std::string::npos, name, "signature chứa caller");
    }
}

static std::vector<std::string> listIps(const std::string &dir)
{
    std::vector<std::string> out;
    DIR *d = opendir(dir.c_str());
    if (!d) return out;
    while (dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n.size() > 4 && n.compare(n.size() - 4, 4, ".ips") == 0) out.push_back(n.substr(0, n.size() - 4));
    }
    closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

int main(int argc, char **argv)
{
    if (argc == 3 && std::string(argv[1]) == "--dump") {
        ParsedPanic p = HCCPanicParser::parse(readAll(argv[2]));
        fputs(HCCPanicFacts::describe(p.facts).c_str(), stdout);
        printf("legacy.model: %s\nlegacy.panic_type: %s\nlegacy.signature: %s\n", p.model.c_str(),
               p.panic_type.c_str(), p.signature.c_str());
        for (const auto &c : p.error_codes) printf("legacy.error_code: %s\n", c.c_str());
        return 0;
    }
    if (argc < 2) {
        fprintf(stderr, "usage: %s <samples_dir> [more dirs...] | --dump <file.ips>\n", argv[0]);
        return 2;
    }

    std::map<std::string, ParsedPanic> parsed;
    std::map<std::string, std::string> described;
    std::vector<std::pair<std::string, std::string>> order;  // name, dir
    for (int a = 1; a < argc; ++a) {
        for (const auto &n : listIps(argv[a])) {
            ParsedPanic p = HCCPanicParser::parse(readAll(std::string(argv[a]) + "/" + n + ".ips"));
            parsed[n] = p;
            described[n] = HCCPanicFacts::describe(p.facts);
            order.push_back({n, argv[a]});
        }
    }

    int samples = 0;
    for (const auto &it : order) {
        const std::string &n = it.first;
        samples++;
        const int before = g_fail;
        invariants(n, parsed[n]);
        // Parse 2 lần phải ra cùng signature (không phụ thuộc trạng thái).
        ParsedPanic again = HCCPanicParser::parse(readAll(it.second + "/" + n + ".ips"));
        check(again.facts.signature == parsed[n].facts.signature, n, "signature đổi giữa 2 lần parse");

        std::ifstream ex(it.second + "/" + n + ".expect");
        std::string line;
        const std::string &d = described[n];
        while (std::getline(ex, line)) {
            if (line.empty() || line[0] == '#') continue;
            if (line.rfind("has: ", 0) == 0) {
                std::string want = line.substr(5);
                check(("\n" + d).find("\n" + want + "\n") != std::string::npos, n, "thiếu dòng: " + want);
            } else if (line.rfind("lacks: ", 0) == 0) {
                std::string bad = line.substr(7);
                check(d.find(bad) == std::string::npos, n, "không được có: " + bad);
            } else if (line.rfind("same_signature: ", 0) == 0) {
                std::string other = line.substr(16);
                check(parsed.count(other) && parsed[other].facts.signature == parsed[n].facts.signature, n,
                      "signature khác " + other);
            } else {
                check(false, n, "dòng .expect không hiểu: " + line);
            }
        }
        printf("%s %s\n", g_fail == before ? "PASS" : "FAIL", n.c_str());
        if (g_fail != before) printf("---- describe(%s) ----\n%s----\n", n.c_str(), d.c_str());
    }
    printf("\n%d mẫu, %d kiểm tra, %d lỗi\n", samples, g_checks, g_fail);
    return g_fail == 0 && samples > 0 ? 0 : 1;
}
