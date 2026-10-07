#include "HCCNewsParse.hpp"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

namespace HCCNewsParse {

namespace {

void appendUtf8(std::string &out, uint32_t cp)
{
    if (cp < 0x80) {
        out.push_back((char)cp);
    } else if (cp < 0x800) {
        out.push_back((char)(0xC0 | (cp >> 6)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out.push_back((char)(0xE0 | (cp >> 12)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp < 0x110000) {
        out.push_back((char)(0xF0 | (cp >> 18)));
        out.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back((char)(0x80 | (cp & 0x3F)));
    }
}

struct NamedEntity {
    const char *name;
    uint32_t cp;
};

const NamedEntity NAMED[] = {
    {"amp", '&'},     {"lt", '<'},       {"gt", '>'},       {"quot", '"'},     {"apos", '\''},
    {"nbsp", ' '},    {"ndash", '-'},    {"mdash", '-'},    {"hellip", 0x2026}, {"ldquo", '"'},
    {"rdquo", '"'},   {"lsquo", '\''},   {"rsquo", '\''},   {"laquo", '"'},    {"raquo", '"'},
    {"bull", '-'},    {"middot", '-'},   {"copy", 0xA9},    {"deg", 0xB0},     {"times", 'x'},
    {"agrave", 0xE0}, {"aacute", 0xE1},  {"acirc", 0xE2},   {"atilde", 0xE3},  {"egrave", 0xE8},
    {"eacute", 0xE9}, {"ecirc", 0xEA},   {"igrave", 0xEC},  {"iacute", 0xED},  {"ograve", 0xF2},
    {"oacute", 0xF3}, {"ocirc", 0xF4},   {"otilde", 0xF5},  {"ugrave", 0xF9},  {"uacute", 0xFA},
    {"yacute", 0xFD}, {"Agrave", 0xC0},  {"Aacute", 0xC1},  {"Acirc", 0xC2},   {"Atilde", 0xC3},
    {"Egrave", 0xC8}, {"Eacute", 0xC9},  {"Ecirc", 0xCA},   {"Igrave", 0xCC},  {"Iacute", 0xCD},
    {"Ograve", 0xD2}, {"Oacute", 0xD3},  {"Ocirc", 0xD4},   {"Otilde", 0xD5},  {"Ugrave", 0xD9},
    {"Uacute", 0xDA}, {"Yacute", 0xDD},
};

// Ký tự font tiếng Việt có thể không có -> đổi sang ký tự tương đương.
uint32_t mapCodepoint(uint32_t cp)
{
    switch (cp) {
    case 0xA0: case 0x2002: case 0x2003: case 0x2009: case 0x200A: case 0x202F:
        return ' ';
    case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: case 0xAD:
        return 0;  // bỏ
    case 0x2018: case 0x2019: case 0x201A: case 0x2032:
        return '\'';
    case 0x201C: case 0x201D: case 0x201E: case 0xAB: case 0xBB: case 0x2033:
        return '"';
    case 0x2010: case 0x2011: case 0x2012: case 0x2013: case 0x2014: case 0x2015: case 0x2022: case 0xB7:
    case 0x2212:
        return '-';
    default:
        return cp;
    }
}

void appendMapped(std::string &out, uint32_t cp)
{
    if (cp == 0x2026) {
        out += "...";
        return;
    }
    cp = mapCodepoint(cp);
    if (cp != 0) {
        appendUtf8(out, cp);
    }
}

// Đọc một ký tự UTF-8 ở s[i]; trả độ dài (1 nếu byte lỗi).
int utf8At(const std::string &s, size_t i, uint32_t *cp)
{
    const unsigned char c = (unsigned char)s[i];
    int n = 1;
    uint32_t v = c;
    if (c >= 0xF0 && c < 0xF8) {
        n = 4;
        v = c & 0x07;
    } else if (c >= 0xE0) {
        n = 3;
        v = c & 0x0F;
    } else if (c >= 0xC0) {
        n = 2;
        v = c & 0x1F;
    } else {
        *cp = c;
        return 1;
    }
    if (i + n > s.size()) {
        *cp = c;
        return 1;
    }
    for (int k = 1; k < n; ++k) {
        const unsigned char cc = (unsigned char)s[i + k];
        if ((cc & 0xC0) != 0x80) {
            *cp = c;
            return 1;
        }
        v = (v << 6) | (cc & 0x3F);
    }
    *cp = v;
    return n;
}

bool isSpace(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

std::string collapse(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    bool sp = true;
    for (char c : s) {
        if (isSpace(c)) {
            if (!sp) {
                out.push_back(' ');
                sp = true;
            }
        } else {
            out.push_back(c);
            sp = false;
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::string lower(std::string s)
{
    for (char &c : s) {
        c = (char)tolower((unsigned char)c);
    }
    return s;
}

// Tìm không phân biệt hoa thường.
size_t ifind(const char *hay, size_t len, const char *needle, size_t from)
{
    const size_t n = strlen(needle);
    if (n == 0 || len < n) {
        return std::string::npos;
    }
    for (size_t i = from; i + n <= len; ++i) {
        if (strncasecmp(hay + i, needle, n) == 0) {
            return i;
        }
    }
    return std::string::npos;
}

// Giá trị thuộc tính name trong chuỗi thuộc tính (attrs = phần giữa tên thẻ và '>').
std::string attr(const std::string &attrs, const char *name)
{
    const size_t n = strlen(name);
    size_t i = 0;
    while (i < attrs.size()) {
        while (i < attrs.size() && (isSpace(attrs[i]) || attrs[i] == '/')) {
            ++i;
        }
        const size_t ks = i;
        while (i < attrs.size() && !isSpace(attrs[i]) && attrs[i] != '=' && attrs[i] != '>' && attrs[i] != '/') {
            ++i;
        }
        const size_t ke = i;
        while (i < attrs.size() && isSpace(attrs[i])) {
            ++i;
        }
        std::string val;
        if (i < attrs.size() && attrs[i] == '=') {
            ++i;
            while (i < attrs.size() && isSpace(attrs[i])) {
                ++i;
            }
            if (i < attrs.size() && (attrs[i] == '"' || attrs[i] == '\'')) {
                const char q = attrs[i++];
                const size_t vs = i;
                while (i < attrs.size() && attrs[i] != q) {
                    ++i;
                }
                val = attrs.substr(vs, i - vs);
                if (i < attrs.size()) {
                    ++i;
                }
            } else {
                const size_t vs = i;
                while (i < attrs.size() && !isSpace(attrs[i]) && attrs[i] != '>') {
                    ++i;
                }
                val = attrs.substr(vs, i - vs);
            }
        }
        if (ke - ks == n && strncasecmp(attrs.c_str() + ks, name, n) == 0) {
            return decodeEntities(val);
        }
        if (ke == ks) {
            ++i;  // ký tự lạ: bỏ qua để không lặp vô hạn
        }
    }
    return std::string();
}

// Nội dung thẻ <tag ...>...</tag> đầu tiên trong [s, s+len). attrs nhận phần thuộc tính.
bool tagContent(const char *s, size_t len, const char *tag, std::string *content, std::string *attrs = nullptr)
{
    const std::string open = std::string("<") + tag;
    size_t pos = 0;
    while (true) {
        pos = ifind(s, len, open.c_str(), pos);
        if (pos == std::string::npos) {
            return false;
        }
        const char after = pos + open.size() < len ? s[pos + open.size()] : '\0';
        if (after == '>' || isSpace(after) || after == '/') {
            break;
        }
        pos += open.size();
    }
    const char *gt = (const char *)memchr(s + pos, '>', len - pos);
    if (gt == nullptr) {
        return false;
    }
    const size_t a0 = pos + open.size();
    const size_t a1 = gt - s;
    if (attrs) {
        *attrs = std::string(s + a0, a1 - a0);
    }
    if (a1 > a0 && s[a1 - 1] == '/') {  // <tag ... />
        content->clear();
        return true;
    }
    const std::string close = std::string("</") + tag;
    const size_t c = ifind(s, len, close.c_str(), a1 + 1);
    const size_t end = c == std::string::npos ? len : c;
    *content = std::string(s + a1 + 1, end - a1 - 1);
    return true;
}

// Bỏ <![CDATA[ ]]> (giữ nội dung).
std::string stripCdata(const std::string &s)
{
    if (s.find("<![CDATA[") == std::string::npos) {
        return s;
    }
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        const size_t a = s.find("<![CDATA[", i);
        if (a == std::string::npos) {
            out.append(s, i, std::string::npos);
            break;
        }
        out.append(s, i, a - i);
        const size_t b = s.find("]]>", a + 9);
        if (b == std::string::npos) {
            out.append(s, a + 9, std::string::npos);
            break;
        }
        out.append(s, a + 9, b - a - 9);
        i = b + 3;
    }
    return out;
}

bool usableImageUrl(const std::string &u)
{
    if (u.empty() || u.compare(0, 5, "data:") == 0 || u.compare(0, 11, "javascript:") == 0) {
        return false;
    }
    const std::string l = lower(u);
    const size_t q = l.find_first_of("?#");
    const std::string path = l.substr(0, q);
    return !(path.size() > 4 && (path.compare(path.size() - 4, 4, ".svg") == 0 ||
                                 path.compare(path.size() - 4, 4, ".gif") == 0));
}

std::string imgUrlFromAttrs(const std::string &attrs)
{
    static const char *const KEYS[] = {"data-src", "data-original", "data-lazy-src", "data-srcset", "src"};
    for (const char *k : KEYS) {
        std::string v = attr(attrs, k);
        if (strcmp(k, "data-srcset") == 0 && !v.empty()) {
            const size_t sp = v.find_first_of(" ,");  // lấy URL đầu tiên của srcset
            v = v.substr(0, sp);
        }
        v = collapse(v);
        if (usableImageUrl(v)) {
            return v;
        }
    }
    return std::string();
}

}  // namespace

std::string decodeEntities(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        if (c == '&') {
            const size_t semi = s.find(';', i + 1);
            if (semi != std::string::npos && semi - i <= 10) {
                const std::string ent = s.substr(i + 1, semi - i - 1);
                uint32_t cp = 0;
                bool ok = false;
                if (!ent.empty() && ent[0] == '#') {
                    char *endp = nullptr;
                    if (ent.size() > 1 && (ent[1] == 'x' || ent[1] == 'X')) {
                        cp = (uint32_t)strtoul(ent.c_str() + 2, &endp, 16);
                    } else {
                        cp = (uint32_t)strtoul(ent.c_str() + 1, &endp, 10);
                    }
                    ok = endp && *endp == '\0' && cp > 0 && cp < 0x110000;
                } else {
                    for (const NamedEntity &e : NAMED) {
                        if (ent == e.name) {
                            cp = e.cp;
                            ok = true;
                            break;
                        }
                    }
                }
                if (ok) {
                    appendMapped(out, cp);
                    i = semi + 1;
                    continue;
                }
            }
            out.push_back('&');
            ++i;
            continue;
        }
        if ((unsigned char)c >= 0x80) {
            uint32_t cp;
            const int n = utf8At(s, i, &cp);
            if (n > 1) {
                appendMapped(out, cp);
            }
            // byte UTF-8 lỗi: bỏ
            i += n;
            continue;
        }
        out.push_back(c);
        ++i;
    }
    return out;
}

std::string htmlToText(const std::string &s)
{
    std::string t;
    t.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] == '<') {
            const size_t gt = s.find('>', i + 1);
            if (gt == std::string::npos) {
                break;
            }
            if (strncasecmp(s.c_str() + i, "<br", 3) == 0 || strncasecmp(s.c_str() + i, "</p", 3) == 0) {
                t.push_back(' ');
            }
            i = gt + 1;
            continue;
        }
        t.push_back(s[i++]);
    }
    return collapse(decodeEntities(t));
}

std::string resolveUrl(const std::string &base, const std::string &href_in)
{
    const std::string href = collapse(href_in);
    if (href.empty()) {
        return std::string();
    }
    if (href.compare(0, 7, "http://") == 0 || href.compare(0, 8, "https://") == 0) {
        return href;
    }
    std::string scheme = "https";
    std::string rest = base;
    const size_t sp = base.find("://");
    if (sp != std::string::npos) {
        scheme = base.substr(0, sp);
        rest = base.substr(sp + 3);
    }
    if (href.compare(0, 2, "//") == 0) {
        return scheme + ":" + href;
    }
    const size_t slash = rest.find('/');
    const std::string host = slash == std::string::npos ? rest : rest.substr(0, slash);
    if (href[0] == '/') {
        return scheme + "://" + host + href;
    }
    std::string path = slash == std::string::npos ? "/" : rest.substr(slash);
    path = path.substr(0, path.find_first_of("?#"));
    const size_t last = path.find_last_of('/');
    return scheme + "://" + host + path.substr(0, last + 1) + href;
}

// ==================== RSS ====================

std::vector<Item> parseRss(const char *xml, size_t len, size_t max_items)
{
    std::vector<Item> items;
    const bool atom = ifind(xml, len, "<entry", 0) != std::string::npos && ifind(xml, len, "<item", 0) == std::string::npos;
    const char *open = atom ? "<entry" : "<item";
    const char *close = atom ? "</entry>" : "</item>";
    size_t pos = 0;
    while (items.size() < max_items) {
        size_t a = ifind(xml, len, open, pos);
        if (a == std::string::npos) {
            break;
        }
        const char after = a + strlen(open) < len ? xml[a + strlen(open)] : '\0';
        if (after != '>' && !isSpace(after)) {
            pos = a + 1;
            continue;
        }
        size_t b = ifind(xml, len, close, a);
        if (b == std::string::npos) {
            b = len;
        }
        const char *s = xml + a;
        const size_t n = b - a;
        pos = b + 1;

        Item it;
        std::string v, at;
        if (tagContent(s, n, "title", &v)) {
            it.title = htmlToText(stripCdata(v));
        }
        if (atom) {
            if (tagContent(s, n, "link", &v, &at)) {
                it.link = attr(at, "href");
            }
        } else if (tagContent(s, n, "link", &v)) {
            it.link = collapse(decodeEntities(stripCdata(v)));
        }
        if (it.link.empty() && tagContent(s, n, "guid", &v)) {
            const std::string g = collapse(decodeEntities(stripCdata(v)));
            if (g.compare(0, 4, "http") == 0) {
                it.link = g;
            }
        }
        std::string desc;
        if (tagContent(s, n, "description", &v) || tagContent(s, n, "summary", &v) ||
            tagContent(s, n, "content", &v)) {
            const bool cdata = v.find("<![CDATA[") != std::string::npos;
            desc = stripCdata(v);
            if (!cdata && desc.find("&lt;") != std::string::npos) {
                desc = decodeEntities(desc);  // HTML bị mã hoá thực thể
            }
        }
        // Ảnh: <img> trong mô tả, rồi enclosure / media:content / media:thumbnail.
        size_t ip = ifind(desc.c_str(), desc.size(), "<img", 0);
        if (ip != std::string::npos) {
            const size_t gt = desc.find('>', ip);
            it.image = imgUrlFromAttrs(desc.substr(ip + 4, (gt == std::string::npos ? desc.size() : gt) - ip - 4));
        }
        static const char *const MEDIA[] = {"enclosure", "media:content", "media:thumbnail"};
        for (const char *m : MEDIA) {
            if (!it.image.empty()) {
                break;
            }
            if (tagContent(s, n, m, &v, &at)) {
                const std::string type = lower(attr(at, "type"));
                const std::string u = attr(at, "url");
                if ((type.empty() || type.compare(0, 6, "image/") == 0) && usableImageUrl(u)) {
                    it.image = u;
                }
            }
        }
        it.summary = htmlToText(desc);
        if (it.summary.size() > 600) {
            size_t cut = 600;
            while (cut > 0 && ((unsigned char)it.summary[cut] & 0xC0) == 0x80) {
                --cut;  // không cắt giữa ký tự UTF-8
            }
            it.summary = it.summary.substr(0, cut) + "...";
        }
        if (tagContent(s, n, "pubDate", &v) || tagContent(s, n, "updated", &v) || tagContent(s, n, "published", &v)) {
            it.date = collapse(stripCdata(v));
        }
        if (!it.title.empty() && !it.link.empty()) {
            it.link = resolveUrl(it.link, it.link);
            items.push_back(std::move(it));
        }
    }
    return items;
}

// ==================== Bài báo ====================

namespace {

struct Region {
    size_t start;
    size_t end;
};

// Tìm phần tử có thuộc tính chứa marker; trả [vị trí '<' của thẻ mở, vị trí sau thẻ đóng tương ứng).
bool findRegion(const char *h, size_t len, const char *marker, Region *r)
{
    const size_t m = ifind(h, len, marker, 0);
    if (m == std::string::npos) {
        return false;
    }
    size_t lt = m;
    while (lt > 0 && h[lt] != '<') {
        --lt;
    }
    if (h[lt] != '<') {
        return false;
    }
    size_t te = lt + 1;
    while (te < len && (isalnum((unsigned char)h[te]) || h[te] == '-')) {
        ++te;
    }
    const std::string tag = lower(std::string(h + lt + 1, te - lt - 1));
    if (tag.empty()) {
        return false;
    }
    const std::string open = "<" + tag;
    const std::string close = "</" + tag;
    int depth = 0;
    size_t i = lt;
    while (i < len) {
        const size_t o = ifind(h, len, open.c_str(), i);
        const size_t c = ifind(h, len, close.c_str(), i);
        if (c == std::string::npos) {
            i = len;
            break;
        }
        if (o != std::string::npos && o < c) {
            const char after = o + open.size() < len ? h[o + open.size()] : '\0';
            if (after == '>' || isSpace(after)) {
                ++depth;
            }
            i = o + open.size();
        } else {
            --depth;
            i = c + close.size();
            if (depth <= 0) {
                const char *gt = (const char *)memchr(h + i, '>', len - i);
                i = gt ? (size_t)(gt - h) + 1 : len;
                break;
            }
        }
    }
    r->start = lt;
    r->end = i;
    return true;
}

bool skipBox(const std::string &attrs)
{
    const std::string a = lower(attrs);
    static const char *const BAD[] = {"related", "tinlienquan", "box-tin", "advert", "banner", "social", "share",
                                      "comment", "box_comment", "box-comment", "author-info", "tags", "box-relate",
                                      "relatednews", "vcsortableinpreviewmode", "list-news", "footer", "header"};
    for (const char *b : BAD) {
        if (a.find(b) != std::string::npos) {
            return true;
        }
    }
    return false;
}

struct Builder {
    std::vector<Block> out;
    std::string text;
    int heading = 0;  // đang trong <hN>
    int caption = 0;  // đang trong <figcaption>
    size_t max_blocks;
    std::string base;

    void flush()
    {
        const std::string t = collapse(decodeEntities(text));
        text.clear();
        if (!t.empty() && out.size() < max_blocks) {
            out.push_back({heading ? Block::Heading : caption ? Block::Caption : Block::Para, t, std::string()});
        }
    }
    void image(const std::string &u)
    {
        flush();
        if (u.empty() || out.size() >= max_blocks) {
            return;
        }
        const std::string abs = resolveUrl(base, u);
        for (const Block &b : out) {
            if (b.type == Block::Image && b.url == abs) {
                return;  // ảnh lặp (noscript, srcset)
            }
        }
        out.push_back({Block::Image, std::string(), abs});
    }
};

bool isBlockTag(const std::string &t)
{
    static const char *const TAGS[] = {"p", "div", "br", "li", "ul", "ol", "section", "article", "table", "tr",
                                       "td", "blockquote", "figure", "h1", "h2", "h3", "h4", "h5", "h6", "header",
                                       "footer", "aside"};
    for (const char *x : TAGS) {
        if (t == x) {
            return true;
        }
    }
    return false;
}

// Bỏ qua tới thẻ đóng tương ứng của tag (đếm lồng nhau) bắt đầu từ i (ngay sau thẻ mở).
size_t skipElement(const char *h, size_t end, const std::string &tag, size_t i)
{
    const std::string open = "<" + tag;
    const std::string close = "</" + tag;
    int depth = 1;
    while (i < end && depth > 0) {
        const size_t o = ifind(h, end, open.c_str(), i);
        const size_t c = ifind(h, end, close.c_str(), i);
        if (c == std::string::npos) {
            return end;
        }
        if (o != std::string::npos && o < c) {
            const char after = o + open.size() < end ? h[o + open.size()] : '\0';
            if (after == '>' || isSpace(after)) {
                ++depth;
            }
            i = o + open.size();
        } else {
            --depth;
            i = c + close.size();
        }
    }
    const char *gt = (const char *)memchr(h + i, '>', end > i ? end - i : 0);
    return gt ? (size_t)(gt - h) + 1 : end;
}

void walk(const char *h, size_t start, size_t end, Builder &b)
{
    size_t i = start;
    while (i < end && b.out.size() < b.max_blocks) {
        if (h[i] != '<') {
            const char *lt = (const char *)memchr(h + i, '<', end - i);
            const size_t e = lt ? (size_t)(lt - h) : end;
            b.text.append(h + i, e - i);
            i = e;
            continue;
        }
        if (i + 4 <= end && strncmp(h + i, "<!--", 4) == 0) {
            const size_t c = ifind(h, end, "-->", i + 4);
            i = c == std::string::npos ? end : c + 3;
            continue;
        }
        const char *gtp = (const char *)memchr(h + i, '>', end - i);
        if (gtp == nullptr) {
            break;
        }
        const size_t gt = gtp - h;
        const bool closing = h[i + 1] == '/';
        size_t ns = i + (closing ? 2 : 1);
        size_t ne = ns;
        while (ne < gt && (isalnum((unsigned char)h[ne]) || h[ne] == ':' || h[ne] == '-')) {
            ++ne;
        }
        const std::string tag = lower(std::string(h + ns, ne - ns));
        const std::string attrs(h + ne, gt - ne);
        i = gt + 1;
        if (tag.empty()) {
            continue;
        }
        if (!closing && (tag == "script" || tag == "style" || tag == "noscript" || tag == "iframe" ||
                         tag == "svg" || tag == "button" || tag == "form" || tag == "video" || tag == "template")) {
            if (attrs.empty() || attrs.back() != '/') {
                i = skipElement(h, end, tag, i);
            }
            continue;
        }
        if (!closing && (tag == "div" || tag == "aside" || tag == "section" || tag == "table" || tag == "ul" ||
                         tag == "footer" || tag == "nav") && skipBox(attrs)) {
            b.flush();
            i = skipElement(h, end, tag, i);
            continue;
        }
        if (tag == "img" && !closing) {
            const std::string w = attr(attrs, "width");
            if (!w.empty() && atoi(w.c_str()) > 0 && atoi(w.c_str()) < 80) {
                continue;  // biểu tượng nhỏ
            }
            b.image(imgUrlFromAttrs(attrs));
            continue;
        }
        if (tag.size() == 2 && tag[0] == 'h' && tag[1] >= '1' && tag[1] <= '6') {
            b.flush();
            b.heading = closing ? (b.heading > 0 ? b.heading - 1 : 0) : b.heading + 1;
            continue;
        }
        if (tag == "figcaption") {
            b.flush();
            b.caption = closing ? (b.caption > 0 ? b.caption - 1 : 0) : b.caption + 1;
            continue;
        }
        if (isBlockTag(tag)) {
            if (tag == "br" && (b.heading || b.caption)) {
                b.text.push_back(' ');
                continue;
            }
            b.flush();
            continue;
        }
        // thẻ inline (a, strong, em, span ...): giữ chữ
        b.text.push_back(' ');
    }
    b.flush();
}

int paraCount(const std::vector<Block> &v)
{
    int n = 0;
    for (const Block &b : v) {
        n += (b.type == Block::Para && b.text.size() >= 40) ? 1 : 0;
    }
    return n;
}

}  // namespace

std::vector<Block> parseArticle(const char *html, size_t len, const std::string &page_url, size_t max_blocks)
{
    // Khung nội dung của các báo (thứ tự ưu tiên).
    static const char *const MARKERS[] = {
        "fck_detail",               // vnexpress
        "detail-cmain",             // tuoitre / thanhnien (cả sapo + nội dung)
        "detail-content",           // tuoitre / thanhnien
        "singular-content",         // dantri
        "dt-news__content",         // dantri (giao diện cũ)
        "e-magazine__body",         // dantri emagazine
        "maincontent",              // vietnamnet
        "content-detail",           //
        "article-content",          //
        "article__body",            //
        "entry-content",            // wordpress
        "itemprop=\"articleBody\"", //
        "<article",                 //
    };
    Builder best;
    best.max_blocks = max_blocks;
    best.base = page_url;
    for (const char *m : MARKERS) {
        Region r;
        if (!findRegion(html, len, m, &r)) {
            continue;
        }
        Builder b;
        b.max_blocks = max_blocks;
        b.base = page_url;
        walk(html, r.start, r.end, b);
        if (paraCount(b.out) >= 2) {
            return b.out;
        }
        if (paraCount(b.out) > paraCount(best.out)) {
            best = b;
        }
    }
    // Dự phòng: cả trang, giữ đoạn văn dài và ảnh nằm giữa các đoạn đó.
    size_t body = ifind(html, len, "<body", 0);
    Builder all;
    all.max_blocks = 4000;
    all.base = page_url;
    walk(html, body == std::string::npos ? 0 : body, len, all);
    int first = -1, last = -1;
    for (size_t k = 0; k < all.out.size(); ++k) {
        const Block &b = all.out[k];
        if ((b.type == Block::Para && b.text.size() >= 80)) {
            if (first < 0) {
                first = (int)k;
            }
            last = (int)k;
        }
    }
    std::vector<Block> out;
    if (first >= 0) {
        for (int k = first; k <= last && out.size() < max_blocks; ++k) {
            const Block &b = all.out[k];
            if (b.type == Block::Image || b.type == Block::Caption || b.type == Block::Heading ||
                (b.type == Block::Para && b.text.size() >= 40)) {
                out.push_back(b);
            }
        }
    }
    if (paraCount(out) > paraCount(best.out)) {
        return out;
    }
    return best.out;
}

}  // namespace HCCNewsParse
