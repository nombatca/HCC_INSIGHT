#include "HCCSpmiAnalyzer.hpp"

#include <algorithm>
#include <map>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace HCCSpmiAnalyzer {

using namespace HCCSpmiDecode;

const char *diffName(DiffType d)
{
    switch (d) {
    case D_SAME: return "GIỐNG";
    case D_DATA: return "KHÁC DỮ LIỆU";
    case D_MISSING: return "THIẾU Ở B";
    case D_EXTRA: return "THỪA Ở B";
    case D_ERR: return "LỖI GIAO TIẾP";
    }
    return "?";
}

namespace {

bool broken(const Txn &t)
{
    return t.kind == K_BAD || t.short_bits;
}

uint32_t key(const Txn &t)
{
    const int cls = isRead(t.kind) ? 1 : isWrite(t.kind) ? 2 : 3;
    // Reg0 Write: dữ liệu nằm trong mã lệnh -> khoá không chứa mã lệnh; lệnh khác (Reset...) khoá theo mã lệnh.
    uint32_t k = ((uint32_t)t.sid << 28) | ((uint32_t)cls << 24) | ((uint32_t)t.kind << 16);
    if (cls == 3) {
        k |= t.cmd;
    } else {
        k |= t.addr;
    }
    return k;
}

bool sameData(const Txn &x, const Txn &y)
{
    if (x.kind == K_REG0_WRITE) {
        return x.cmd == y.cmd;
    }
    return x.len == y.len && memcmp(x.data, y.data, x.len) == 0;
}

}  // namespace

void compare(const Txn *a, uint32_t na, const Txn *b, uint32_t nb, Result &out, int window, uint32_t max_rows)
{
    out = Result();
    for (uint32_t i = 0; i < na; ++i) {
        out.err_a += status(a[i]) == ST_OK || status(a[i]) == ST_UNKNOWN_CMD ? 0 : 1;
    }
    for (uint32_t i = 0; i < nb; ++i) {
        out.err_b += status(b[i]) == ST_OK || status(b[i]) == ST_UNKNOWN_CMD ? 0 : 1;
    }
    uint32_t i = 0, j = 0;
    auto emit = [&](DiffType t, int32_t ia, int32_t ib) {
        if (out.rows.size() >= max_rows) {
            out.limited = true;
            return;
        }
        if (t != D_SAME && out.first_diff < 0) {
            out.first_diff = (int32_t)out.rows.size();
        }
        out.rows.push_back({t, ia, ib});
        switch (t) {
        case D_SAME: ++out.same; break;
        case D_DATA: ++out.data; break;
        case D_MISSING: ++out.missing; break;
        case D_EXTRA: ++out.extra; break;
        case D_ERR: ++out.err; break;
        }
    };
    while ((i < na || j < nb) && !out.limited) {
        if (i < na && j < nb && broken(a[i]) && broken(b[j])) {  // cùng vị trí lỗi ở cả hai
            emit(D_ERR, (int32_t)i++, (int32_t)j++);
            continue;
        }
        if (i < na && broken(a[i])) {
            emit(D_ERR, (int32_t)i++, -1);
            continue;
        }
        if (j < nb && broken(b[j])) {
            emit(D_ERR, -1, (int32_t)j++);
            continue;
        }
        if (i >= na) {
            emit(D_EXTRA, -1, (int32_t)j++);
            continue;
        }
        if (j >= nb) {
            emit(D_MISSING, (int32_t)i++, -1);
            continue;
        }
        const uint32_t ka = key(a[i]), kb = key(b[j]);
        if (ka == kb) {
            if (a[i].parity_err || b[j].parity_err) {
                emit(D_ERR, (int32_t)i, (int32_t)j);
            } else {
                emit(sameData(a[i], b[j]) ? D_SAME : D_DATA, (int32_t)i, (int32_t)j);
            }
            ++i;
            ++j;
            continue;
        }
        // Tìm a[i] ở phía trước trong B (=> B có gói thừa) và b[j] ở phía trước trong A (=> B thiếu gói).
        int fb = -1, fa = -1;
        for (int k = 1; k <= window && j + k < nb; ++k) {
            if (!broken(b[j + k]) && key(b[j + k]) == ka) {
                fb = k;
                break;
            }
        }
        for (int k = 1; k <= window && i + k < na; ++k) {
            if (!broken(a[i + k]) && key(a[i + k]) == kb) {
                fa = k;
                break;
            }
        }
        if (fb > 0 && (fa < 0 || fb <= fa)) {
            emit(D_EXTRA, -1, (int32_t)j++);
        } else if (fa > 0) {
            emit(D_MISSING, (int32_t)i++, -1);
        } else {
            emit(D_MISSING, (int32_t)i++, -1);
            emit(D_EXTRA, -1, (int32_t)j++);
        }
    }
}

std::vector<RegRow> finalRegs(const Txn *a, uint32_t na, const Txn *b, uint32_t nb)
{
    std::map<uint32_t, RegRow> m;
    auto scan = [&](const Txn *t, uint32_t n, bool isA) {
        for (uint32_t i = 0; i < n; ++i) {
            const Txn &x = t[i];
            if (!isWrite(x.kind) || status(x) != ST_OK) {
                continue;
            }
            const int cnt = x.kind == K_REG0_WRITE ? 1 : x.len;
            for (int k = 0; k < cnt; ++k) {
                const uint16_t addr = (uint16_t)(x.addr + k);
                const uint8_t v = x.kind == K_REG0_WRITE ? (x.cmd & 0x7F) : x.data[k];
                const uint32_t key = ((uint32_t)x.sid << 16) | addr;
                RegRow &r = m[key];
                r.sid = x.sid;
                r.addr = addr;
                if (isA) {
                    r.has_a = true;
                    r.last_a = v;
                    ++r.writes_a;
                } else {
                    r.has_b = true;
                    r.last_b = v;
                    ++r.writes_b;
                }
            }
        }
    };
    scan(a, na, true);
    scan(b, nb, false);
    std::vector<RegRow> v;
    v.reserve(m.size());
    for (auto &kv : m) {
        v.push_back(kv.second);
    }
    return v;
}

namespace {

bool parseNum(const char *s, uint32_t &v)
{
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    if (*s == '\0') {
        return false;
    }
    char *end = nullptr;
    v = (uint32_t)strtoul(s, &end, 0);
    while (end && (*end == ' ' || *end == '\t')) {
        ++end;
    }
    return end && *end == '\0';
}

std::string trim(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        ++s;
    }
    std::string r(s);
    while (!r.empty() && (r.back() == ' ' || r.back() == '\t' || r.back() == '\r')) {
        r.pop_back();
    }
    return r;
}

}  // namespace

int parseMap(const char *text, PmicMap &map)
{
    map = PmicMap();
    map.loaded = true;
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        std::string line = e ? std::string(p, e - p) : std::string(p);
        p = e ? e + 1 : p + strlen(p);
        std::string t = trim(line.c_str());
        if (t.empty() || t[0] == '#') {
            continue;
        }
        if (t.compare(0, 3, "sid") == 0) {
            continue;  // dòng tiêu đề
        }
        std::vector<std::string> f;
        size_t s = 0;
        while (true) {
            const size_t c = t.find(',', s);
            f.push_back(trim(t.substr(s, c == std::string::npos ? std::string::npos : c - s).c_str()));
            if (c == std::string::npos) {
                break;
            }
            s = c + 1;
        }
        uint32_t sid, addr, mask, val;
        if (f.size() < 6 || !parseNum(f[0].c_str(), sid) || !parseNum(f[1].c_str(), addr) ||
            !parseNum(f[2].c_str(), mask) || !parseNum(f[3].c_str(), val) || sid > 15 || addr > 0xFFFF ||
            mask > 0xFF || val > 0xFF || f[4].empty() || f[5].empty()) {
            ++map.rejected;
            continue;
        }
        // Cột nguồn có thể chứa dấu phẩy: ghép phần còn lại.
        std::string src = f[5];
        for (size_t k = 6; k < f.size(); ++k) {
            src += "," + f[k];
        }
        map.entries.push_back({(uint8_t)sid, (uint16_t)addr, (uint8_t)mask, (uint8_t)val, f[4], src});
    }
    return (int)map.entries.size();
}

bool loadMap(const char *path, PmicMap &map)
{
    map = PmicMap();
    FILE *f = fopen(path, "rb");
    if (f == nullptr) {
        return false;
    }
    std::string text;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0 && text.size() < 256 * 1024) {
        text.append(buf, n);
    }
    fclose(f);
    parseMap(text.c_str(), map);
    return true;
}

const char *mapStateName(MapState s)
{
    switch (s) {
    case M_NO_WRITE: return "KHÔNG THẤY LỆNH GHI";
    case M_MATCH: return "LỆNH GHI KHỚP";
    case M_DIFF: return "LỆNH GHI KHÁC";
    }
    return "?";
}

MapState applyMap(const MapEntry &e, const Txn *t, uint32_t n, uint8_t *last_value)
{
    bool found = false;
    uint8_t last = 0;
    for (uint32_t i = 0; i < n; ++i) {
        const Txn &x = t[i];
        if (x.sid != e.sid || !isWrite(x.kind) || status(x) != ST_OK) {
            continue;
        }
        if (x.kind == K_REG0_WRITE) {
            if (e.addr == 0) {
                found = true;
                last = x.cmd & 0x7F;
            }
            continue;
        }
        if (e.addr >= x.addr && e.addr < x.addr + x.len) {
            found = true;
            last = x.data[e.addr - x.addr];
        }
    }
    if (last_value) {
        *last_value = last;
    }
    if (!found) {
        return M_NO_WRITE;
    }
    return ((last & e.mask) == (e.value & e.mask)) ? M_MATCH : M_DIFF;
}

}  // namespace HCCSpmiAnalyzer
