/*
 * HCCRffe: đọc thanh ghi thiết bị MIPI RFFE (bit-bang SCLK / SDATA) và so với dữ liệu tham chiếu
 * (bảng JC BUS2MM4 trong firmware hoặc file trên thẻ SD).
 *
 * Chân SCLK / SDATA đặt trong menuconfig (HCC RFFE), mặc định -1 = chưa nối: SCAN chỉ báo chưa cấu hình.
 * RFFE chạy mức 1,8 V; GPIO của board là 3,3 V nên phải qua mạch chuyển mức hai chiều.
 *
 * Dữ liệu tham chiếu thêm (do người dùng cung cấp):
 *   /sdcard/rffe/<MODEL>/<DATA>.csv
 *   mỗi dòng: usid,reg,expected[,mask[,tên[,ic]]]   (số hex 0x.. hoặc thập phân; dòng '#' là ghi chú)
 *   ví dụ:    0x5,0x00,0x38,0xFF,REG0,<tên IC theo nguồn của bạn>
 *   Cột ic (tùy chọn) gắn USID với IC RFFE của model; firmware chỉ hiển thị, không tự suy ra IC.
 * Bảng JC BUS2MM4 nhúng sẵn (rffe_jc_database.h); file /sdcard/rffe/JC_BUS2MM4_iPhone_RFFE_USID.csv nếu có sẽ thay thế
 *   (model_id,type_code,rffe_line,device_part,usid_dec,usid_hex): SELECT MODEL = model_id,
 *   RFFE DATA = rffe_line; mỗi linh kiện kiểm 0x1F & 0x0F == USID. USID 0 (broadcast) không quét.
 *
 * SCAN: bus phải ở mức 0 khi nghỉ (không thì dừng). Hỏi USID 1..15 (0 là broadcast); USID chỉ được nhận khi
 * thanh ghi 0x1F đúng parity, bit[3:0] == USID đang hỏi và đọc lại hai lần cùng giá trị (không nối gì -> 0 USID).
 * Lưu ý board P4 WIFI6: GPIO14..19 và 54 là SDIO / reset của chip Wi-Fi C6 - firmware từ chối các chân này.
 * USID có trong dữ liệu tham chiếu: đọc đúng các thanh ghi đó (địa chỉ > 0x1F dùng Extended Register Read) và so (giá trị & mask) == (chuẩn & mask). USID không có tham
 * chiếu: đọc thô 0x00..0x1F, không kết luận.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

namespace HCCRffe {

struct RefEntry {
    uint8_t usid;
    uint16_t reg;
    uint8_t expected;
    uint8_t mask;
    char name[24];
    char ic[24];     // tên IC theo dữ liệu tham chiếu ("" nếu file không ghi)
};

struct RegResult {
    uint16_t reg;
    uint8_t value;
    bool read_ok;    // bit chẵn lẻ đúng
    bool has_ref;
    uint8_t expected;
    uint8_t mask;
    char name[24];
};

struct UsidResult {
    uint8_t usid;
    bool responded;
    bool in_ref;     // USID có trong dữ liệu tham chiếu
    char ic[24];     // IC của USID theo dữ liệu tham chiếu ("" nếu không có)
    uint8_t id_regs[3];  // giá trị thô 0x1D, 0x1E, 0x1F
    bool id_ok[3];
    int pass;
    int fail;
    int no_read;
    std::vector<RegResult> regs;
};

struct Report {
    bool scanned = false;      // false: chỉ là bản xem trước dữ liệu tham chiếu
    char message[96] = "";
    std::vector<UsidResult> usids;
};

bool configured(void);
// nullptr khi chân hợp lệ; ngược lại là lý do (chưa đặt chân, trùng chân, trùng SDIO Wi-Fi).
const char *configError(void);
// true nếu model lấy từ file JC BUS2MM4 (dữ liệu chỉ có USID theo đường RFFE, kiểm bằng thanh ghi 0x1F).
bool isJcModel(const char *model);
const char *jcFileName(void);
// Nguồn bảng JC đang dùng: "firmware" (rffe_jc_database.h) hoặc "thẻ SD" (file CSV ghi đè).
const char *jcSource(void);
int sclkPin(void);
int sdataPin(void);

// Danh sách cho lv_dropdown (ngăn cách '\n'). Trả số mục, -1 nếu không đọc được thẻ / thư mục.
// Model = các model trong bảng JC (firmware / thẻ) và thư mục /sdcard/rffe/<MODEL>/.
int listModels(char *out, size_t out_size);
// Model JC: danh sách đường RFFE của model; model thư mục: các file .csv.
int listData(const char *model, char *out, size_t out_size);

// Nạp file CSV tham chiếu. Trả false + err khi lỗi; dòng sai cú pháp bị bỏ qua (đếm vào *skipped).
bool loadReference(const char *model, const char *file, std::vector<RefEntry> &out, int *skipped,
                   char *err, size_t err_size);

// Bảng xem trước (chưa scan) dựng từ dữ liệu tham chiếu.
void buildPreview(const std::vector<RefEntry> &ref, Report &out);

// Quét ở task nền. Trả false nếu đang quét.
bool startScan(const std::vector<RefEntry> &ref);
bool busy(void);
// Tăng mỗi lần quét xong; report() chỉ đọc khi !busy().
uint32_t reportSerial(void);
const Report &report(void);

}  // namespace HCCRffe
