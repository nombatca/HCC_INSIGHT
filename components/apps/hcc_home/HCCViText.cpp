#include "HCCViText.hpp"

#include <string>
#include <vector>
#include <cstring>
#include <cctype>

// Bảng nguyên âm tiếng Việt: mỗi hàng = [ngang, sắc, huyền, hỏi, ngã, nặng].
// Telex: s=1(sắc) f=2(huyền) r=3(hỏi) x=4(ngã) j=5(nặng), z=0(xoá dấu).
static const char *kVowelRows[][6] = {
    {"a","á","à","ả","ã","ạ"},
    {"ă","ắ","ằ","ẳ","ẵ","ặ"},
    {"â","ấ","ầ","ẩ","ẫ","ậ"},
    {"e","é","è","ẻ","ẽ","ẹ"},
    {"ê","ế","ề","ể","ễ","ệ"},
    {"i","í","ì","ỉ","ĩ","ị"},
    {"o","ó","ò","ỏ","õ","ọ"},
    {"ô","ố","ồ","ổ","ỗ","ộ"},
    {"ơ","ớ","ờ","ở","ỡ","ợ"},
    {"u","ú","ù","ủ","ũ","ụ"},
    {"ư","ứ","ừ","ử","ữ","ự"},
    {"y","ý","ỳ","ỷ","ỹ","ỵ"},
    {"A","Á","À","Ả","Ã","Ạ"},
    {"Ă","Ắ","Ằ","Ẳ","Ẵ","Ặ"},
    {"Â","Ấ","Ầ","Ẩ","Ẫ","Ậ"},
    {"E","É","È","Ẻ","Ẽ","Ẹ"},
    {"Ê","Ế","Ề","Ể","Ễ","Ệ"},
    {"I","Í","Ì","Ỉ","Ĩ","Ị"},
    {"O","Ó","Ò","Ỏ","Õ","Ọ"},
    {"Ô","Ố","Ồ","Ổ","Ỗ","Ộ"},
    {"Ơ","Ớ","Ờ","Ở","Ỡ","Ợ"},
    {"U","Ú","Ù","Ủ","Ũ","Ụ"},
    {"Ư","Ứ","Ừ","Ử","Ữ","Ự"},
    {"Y","Ý","Ỳ","Ỷ","Ỹ","Ỵ"},
};
static const int kVowelRowCount = sizeof(kVowelRows) / sizeof(kVowelRows[0]);

namespace {

int cpLen(unsigned char c)
{
    if (c < 0x80) return 1;
    if ((c >> 5) == 0x6) return 2;
    if ((c >> 4) == 0xE) return 3;
    if ((c >> 3) == 0x1E) return 4;
    return 1;
}

std::vector<std::string> toCps(const std::string &s)
{
    std::vector<std::string> v;
    size_t i = 0;
    while (i < s.size()) {
        int n = cpLen((unsigned char)s[i]);
        if (i + n > s.size()) n = 1;
        v.push_back(s.substr(i, n));
        i += n;
    }
    return v;
}

// Tìm (hàng) chứa codepoint 'v' ở bất kỳ cột nào; trả -1 nếu không phải nguyên âm.
int vowelRowOf(const std::string &v)
{
    for (int r = 0; r < kVowelRowCount; ++r) {
        for (int c = 0; c < 6; ++c) {
            if (v == kVowelRows[r][c]) return r;
        }
    }
    return -1;
}

// Áp dấu 'idx' cho nguyên âm 'v' (giữ hình dạng ă/â/ê/ô/ơ/ư). "" nếu không phải nguyên âm.
std::string applyTone(const std::string &v, int idx)
{
    int r = vowelRowOf(v);
    if (r < 0) return std::string();
    return kVowelRows[r][idx];
}

// Xử lý Telex trên "từ hiện tại" (đã gồm phím vừa gõ ở cuối).
// Trả true + out (thay cả từ) nếu có biến đổi.
bool telexTail(const std::string &word, std::string &out)
{
    std::vector<std::string> cps = toCps(word);
    if (cps.size() < 2) return false;

    const std::string &last = cps.back();
    if (last.size() != 1) return false;  // phím kích hoạt phải là ASCII
    char key = (char)tolower((unsigned char)last[0]);

    std::vector<std::string> base(cps.begin(), cps.end() - 1);

    auto join = [](const std::vector<std::string> &v) {
        std::string s;
        for (auto &x : v) s += x;
        return s;
    };

    // Dấu thanh: s f r x j z
    int tone = -1;
    switch (key) {
        case 's': tone = 1; break;
        case 'f': tone = 2; break;
        case 'r': tone = 3; break;
        case 'x': tone = 4; break;
        case 'j': tone = 5; break;
        case 'z': tone = 0; break;
        default: break;
    }
    if (tone >= 0) {
        // Tìm nguyên âm cuối trong base để đặt dấu.
        for (int i = (int)base.size() - 1; i >= 0; --i) {
            if (vowelRowOf(base[i]) >= 0) {
                std::string toned = applyTone(base[i], tone);
                if (toned.empty()) return false;
                if (toned == base[i]) return false;  // không đổi (vd đã đúng)
                base[i] = toned;
                out = join(base);
                return true;
            }
        }
        return false;  // không có nguyên âm -> giữ nguyên (chữ literal)
    }

    // Biến đổi hình dạng: aa->â ee->ê oo->ô aw->ă ow->ơ uw->ư dd->đ
    const std::string &prev = base.back();
    std::string rep;
    if (key == 'a' && (prev == "a" || prev == "A")) rep = (prev == "A") ? "Â" : "â";
    else if (key == 'e' && (prev == "e" || prev == "E")) rep = (prev == "E") ? "Ê" : "ê";
    else if (key == 'o' && (prev == "o" || prev == "O")) rep = (prev == "O") ? "Ô" : "ô";
    else if (key == 'd' && (prev == "d" || prev == "D")) rep = (prev == "D") ? "Đ" : "đ";
    else if (key == 'w') {
        if (prev == "a") rep = "ă"; else if (prev == "A") rep = "Ă";
        else if (prev == "o") rep = "ơ"; else if (prev == "O") rep = "Ơ";
        else if (prev == "u") rep = "ư"; else if (prev == "U") rep = "Ư";
    }
    if (rep.empty()) return false;

    base.back() = rep;
    out = join(base);
    return true;
}

// Nhãn đánh dấu ô nhập tiếng Việt (đặt vào user_data của textarea).
static const void *VI_TAG = (const void *)"HCC_VI";

// Sự kiện của keyboard: chạy SAU handler mặc định (đã chèn ký tự).
void telexKbCb(lv_event_t *e)
{
    lv_obj_t *kb = lv_event_get_target(e);
    lv_obj_t *ta = lv_keyboard_get_textarea(kb);
    if (!ta) return;
    // Chỉ gõ tiếng Việt cho ô đã đánh dấu (bỏ qua ô ASCII).
    if (lv_obj_get_user_data(ta) != (void *)VI_TAG) return;

    const char *txt = lv_textarea_get_text(ta);
    if (!txt || !txt[0]) return;
    std::string s = txt;

    // Lấy "từ hiện tại": phần sau dấu cách/xuống dòng cuối cùng.
    size_t start = s.find_last_of(" \n\t");
    std::string word = (start == std::string::npos) ? s : s.substr(start + 1);
    if (word.size() < 2) return;

    std::string out;
    if (!telexTail(word, out)) return;

    // Thay cả từ: xoá số codepoint của từ cũ rồi chèn từ mới.
    size_t old_cp = toCps(word).size();
    for (size_t i = 0; i < old_cp; ++i) lv_textarea_del_char(ta);
    lv_textarea_add_text(ta, out.c_str());
}

}  // namespace

void HCCViText::enableTelex(lv_obj_t *keyboard)
{
    if (!keyboard) return;
    lv_obj_add_event_cb(keyboard, telexKbCb, LV_EVENT_VALUE_CHANGED, nullptr);
}

void HCCViText::markVietnamese(lv_obj_t *textarea)
{
    if (textarea) lv_obj_set_user_data(textarea, (void *)VI_TAG);
}

lv_obj_t *HCCViText::createKeyboard(lv_obj_t *parent, lv_obj_t *textarea)
{
    lv_obj_t *kb = lv_keyboard_create(parent);
    lv_obj_set_size(kb, LV_PCT(100), 320);
    lv_obj_align(kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    // Font lớn -> phím to, dễ bấm.
    lv_obj_set_style_text_font(kb, &lv_font_be_vietnam_22, 0);
    if (textarea) lv_keyboard_set_textarea(kb, textarea);
    enableTelex(kb);
    return kb;
}
