# SPMI MONITOR / PMIC ANALYZER

Màn SPMI có hai chế độ riêng:

| Chế độ | Màn | Việc làm | Có phát lệnh lên bus? |
|---|---|---|---|
| SCANNER | `HCCSpmiView` (giữ nguyên) | Hỏi SID 0-15 bằng bit-bang (`HCCRffe::startSpmiScan`) | CÓ (Register Read) |
| MONITOR | `HCCSpmiMonitorView` | Nghe thụ động SCLK / SDATA, giải mã, lưu, phân tích lại | KHÔNG |
| PMIC ANALYZER | `HCCSpmiAnalyzerView` | So sánh capture main tốt / main lỗi | Không dùng bus |

Đi vào: HCC INSIGHT → SPMI (SCANNER) → nút **MONITOR >** → nút **ANALYZE**.

## 1. Kiến trúc

```
 Nguồn (Capture Driver)            Lõi (HCCSpmiMonitor, task nhân 0)              Giao diện (LVGL, nhân 1)
 ┌──────────────────────┐   Feed   ┌─────────────────────────────────────┐  đọc  ┌────────────────────────┐
 │ PARLIO RX (P4)        │────────▶│ nhận vai dây SCLK/SDATA              │◀──────│ HCCSpmiMonitorView     │
 │ DEMO (mô phỏng)       │ samples │ HCCSpmiDecode::Decoder (SSC, khung,  │ timer │  sóng / bảng / chi tiết│
 │ FILE (.spmc / .csv)   │ idle    │   parity, bus park, thời điểm)       │ 100ms │ HCCSpmiAnalyzerView    │
 │ FPGA/STM32 (chưa có)  │ lost    │ PacketBuf (150 000 gói, PSRAM)       │       │  (task so sánh nền)    │
 └──────────────────────┘ packet  │ RawStore (8 MB sóng thô, PSRAM)      │       └────────────────────────┘
      HCCSpmiSource.hpp            │ thống kê, phát hiện mất dữ liệu      │
                                   │ HCCSpmiCapFile: lưu .spmc + .csv     │
                                   └─────────────────────────────────────┘
```

| File | Vai trò | Phụ thuộc ESP-IDF |
|---|---|---|
| `HCCSpmiDecode.*` | Giải mã SPMI từ mẫu 2 bit; `parse()` theo bảng lệnh; trạng thái gói | không |
| `HCCSpmiSynth.*` | Tạo sóng SPMI. Chỉ dùng cho test và nguồn DEMO | không |
| `HCCSpmiBuffers.*` | `PacketBuf` (1 luồng ghi / nhiều luồng đọc, không khoá) và `RawStore` (sóng thô, bỏ khoảng nghỉ) | không |
| `HCCSpmiCapFile.*` | File `.spmc` (gói + sóng), xuất / nhập CSV | không (stdio) |
| `HCCSpmiAnalyzer.*` | Dóng hàng A/B, thanh ghi cuối, mapping PMIC do người dùng cung cấp | không |
| `HCCSpmiSource.hpp` | Giao diện Capture Driver (`Source`, `Feed`) | không |
| `HCCSpmiSourceParlio.cpp` | Nguồn PARLIO RX | có |
| `HCCSpmiSources.cpp` | Nguồn DEMO / FILE / EXTERNAL | FreeRTOS (`vTaskDelay`) |
| `HCCSpmiMonitor.*` | Task thu, thống kê, trạng thái, lưu SD, cờ xác nhận phần cứng (NVS) | có |
| `HCCSpmiMonitorView.*`, `HCCSpmiAnalyzerView.*` | Giao diện LVGL | LVGL |
| `HCCSpmiLink.*` | Điểm nối BASEBAND / BOARDVIEW (module đích tự đăng ký) | không |

Module cũ vẫn giữ nguyên:

- `HCCRffe` (SCANNER) và `HCCSpmiView`: chỉ đổi nút "NGHE BUS" thành "MONITOR >".
- `HCCSpmiLog` / `HCCSpmiLogView` (màn BẮT DỮ LIỆU SPMI cũ): vẫn biên dịch nhưng không còn nút mở, vì MONITOR làm được mọi việc của màn này. Có thể xoá sau khi MONITOR đã chạy ổn trên phần cứng.

**Phiên bản phần mềm:** dự án dùng **LVGL 8.4** (bắt buộc với esp-brookesia 0.4), ESP-IDF 5.5 và BSP `esp32_p4_wifi6_touch_lcd_7b` 3.0.0 nằm sẵn trong repo. Không nâng lên LVGL 9 vì sẽ làm hỏng toàn bộ giao diện Brookesia. Phần lõi (decoder, bộ đệm, file, analyzer, source) không phụ thuộc LVGL, nên nếu sau này chuyển sang LVGL 9 thì chỉ phải sửa hai file View.

## 2. Khả năng phần cứng hiện tại

| Hạng mục | Thực tế | Kết luận |
|---|---|---|
| Mức logic | GPIO P4 trên board là 3,3 V (VIH ≈ 0,75 × VDD ≈ 2,5 V). Bus SPMI chạy 1,2 V hoặc 1,8 V | Nối thẳng là ngoài thông số, không tin cậy |
| Mạch BSS138 của RFFE | Hai chiều, có điện trở kéo lên ở cả hai phía. Sườn lên phụ thuộc RC (vài trăm ns) | Không dùng cho SPMI: điện trở kéo làm thay đổi bus (SPMI nghỉ ở mức 0) và mạch quá chậm |
| Bộ thu PARLIO RX | 2 dây, 20 / 40 / 80 MHz, DMA vòng 16 × 4032 B | Đủ cho SCLK ≤ fs/4 (≤ 20 MHz ở 80 MHz, nên ≤ 13 MHz). Chưa kiểm chứng trên máy thật |
| SPMI tốc độ cao (tới 26 MHz) | Ở 80 MHz chỉ được ~3 mẫu cho mỗi chu kỳ | Không đủ: cần bộ thu FPGA |

Vì vậy nguồn **PARLIO bị khoá (HARDWARE NOT SUPPORTED)** cho đến khi người dùng bấm "ĐÃ LẮP MẠCH ĐÚNG - CHO PHÉP". Lựa chọn này lưu trong NVS và có thể thu hồi. Firmware không tự đo được điện áp hay mạch đã lắp, nên không thể tự xác nhận thay người dùng.

### Mạch thu đề xuất (cần bổ sung)

- **Bộ chuyển mức một chiều**, đi từ main sang P4, trở kháng vào cao. Ví dụ SN74AXC2T45 hoặc 2 × SN74LVC1T45 với DIR cố định A→B. VCCA lấy từ VIO 1,2 / 1,8 V của main, VCCB lấy 3,3 V của board. Không có điện trở kéo ở phía bus.
- Phía P4 có điện trở nối tiếp 47-100 Ω. Dây đo ngắn (< 10 cm), GND chung sát điểm đo.
- Chân mặc định dùng chung với RFFE: SCLK = GPIO2, SDATA = GPIO3 (đặt trong menuconfig). Nối ngược hai dây vẫn được, vì firmware tự nhận dây nào là SCLK theo số lần đổi mức.
- Bus nhanh hơn ~13-20 MHz: dùng bộ thu ngoài (FPGA, ví dụ iCE40 / Tang Nano; hoặc STM32H7 có timer + DMA), gửi về qua `HCCSpmiSource::external()`. Giao diện nguồn đã có sẵn, driver thì chưa.

## 3. Giải mã

Giải mã theo khung SPMI:

- **SSC:** SDATA lên rồi xuống trong khi SCLK ở mức thấp. Mốc thời gian của gói là cạnh xuống SDATA của SSC.
- **Bit:** lấy SDATA tại cạnh xuống của SCLK.
- **Khung lệnh:** SID 4 bit + lệnh 8 bit + parity lẻ.
- **Khung địa chỉ / dữ liệu:** 8 bit + parity lẻ.
- **Lệnh đọc:** có 1 chu kỳ bus park trước dữ liệu do slave gửi.

Lớp lệnh được giải mã:

| Lệnh | Mã |
|---|---|
| Extended Register Write | 0x00-0x0F |
| Reset / Sleep / Shutdown / Wakeup | 0x10-0x13 |
| Extended Register Read | 0x20-0x2F |
| Extended Register Write Long | 0x30-0x37 |
| Extended Register Read Long | 0x38-0x3F |
| Register Write | 0x40-0x5F |
| Register Read | 0x60-0x7F |
| Register 0 Write | 0x80-0xFF |

Mã lệnh khác hiện là "LỆNH KHÁC": chỉ ghi mã lệnh, không đoán nội dung.

Cột STATUS: `OK`, `SAI PARITY` (khung địa chỉ / dữ liệu), `THIẾU BIT` (gói bị cắt), `LỖI LỆNH` (khung lệnh sai parity hoặc ngắn hơn 13 bit), `LỆNH KHÁC`. Dấu `!` đứng trước nghĩa là ngay trước gói đó có dữ liệu bị mất.

Mất dữ liệu luôn được báo, không bị giấu:

- Hàng đợi DMA tràn hoặc bộ giải mã chạy chậm: tăng `MẤT`, gói kế tiếp mang cờ `F_LOST_BEFORE`, trạng thái **OVERFLOW**. Thời gian của các gói sau vẫn đúng vì số mẫu bị mất được cộng bù.
- Bộ đệm gói đầy (150 000 gói): dừng thu, đếm số gói bị bỏ, trạng thái **OVERFLOW**.
- Bộ nhớ sóng đầy (8 MB): gói vẫn được giải mã; phần sóng sau thời điểm đó hiện "không lưu".
- SCLK lớn hơn 1/4 tần số lấy mẫu: hiện cảnh báo "giải mã không tin cậy".

## 4. File trên thẻ SD (`/sdcard/spmi/`)

- `mon_NNN.spmc` (`mon_NNN_DEMO.spmc` nếu là dữ liệu DEMO): định dạng nhị phân little-endian, mô tả trong `HCCSpmiCapFile.hpp`. Gồm header (cờ DEMO, tràn, sample_hz…), các gói 40 byte và sóng thô.
- `mon_NNN.csv`: bảng gói. Dòng `#` đầu file ghi nguồn và tốc độ lấy mẫu, kèm dòng `# DEMO …` nếu là dữ liệu mô phỏng.
- **OPEN (phân tích lại):**
  - Với `.spmc` có đủ sóng và capture gốc không mất dữ liệu: giải mã lại toàn bộ từ sóng bằng bộ giải mã hiện tại.
  - Với các trường hợp còn lại: nạp các gói đã lưu (giữ cờ mất dữ liệu).
  - Với `.csv` (cả `spmi_NNN.csv` của màn cũ): chỉ có gói, không có sóng.
- `pmic_map.csv`: mapping thanh ghi PMIC do **người dùng** cung cấp. Firmware không kèm mapping nào.

  ```
  # sid,dia_chi,mask,gia_tri,nhan,nguon
  1,0x13,0x0F,0x0F,TEN_TU_DAT,datasheet / tài liệu đã xác minh (trang ...)
  ```

  Dòng không có cột nguồn bị loại. Tab RAIL chỉ báo "LỆNH GHI KHỚP / KHÁC / KHÔNG THẤY LỆNH GHI", **không** kết luận rail đã có điện áp. Muốn biết điện áp phải đo thực tế.

## 5. PMIC ANALYZER

- **Nguồn A (main tốt) và B (main lỗi):** capture hiện tại hoặc file `.spmc` / `.csv`.
- **TRÌNH TỰ / CHỈ KHÁC BIỆT:** dóng hàng theo khoá (SID, lớp lệnh, địa chỉ), đánh dấu GIỐNG / KHÁC DỮ LIỆU / THIẾU Ở B / THỪA Ở B / LỖI GIAO TIẾP. Phương pháp dóng hàng là heuristic tham lam, nhìn trước 64 gói; với chuỗi lệnh lặp dày có thể dóng lệch, nên kết quả là gợi ý để xem lại.
- **THANH GHI CUỐI:** giá trị ghi cuối cùng theo (SID, địa chỉ), chỉ tính các gói không lỗi.
- **RAIL (MAPPING):** xem mục 4.
- **BASEBAND / BOARDVIEW:** các nút đang bị khoá ("chưa nối"). Module đích gọi `HCCSpmiLink::registerTarget()` thì nút sẽ mở được, và nhận (SID, địa chỉ) của gói đang chọn hoặc của khác biệt đầu tiên.

## 6. Kiểm thử

- `test/spmi/run_tests.sh` chạy trên máy tính, cần g++ và dùng ASan / UBSan. Kết quả hiện tại: 171 kiểm tra đạt, 0 lỗi. Nội dung kiểm thử:
  - Vector bit tính tay cho Register Write / Read, Extended Read Long, Register 0 Write, Wakeup.
  - Sai parity ở khung lệnh và khung dữ liệu; gói bị cắt; chuỗi ngắn hơn 13 bit; bảng lệnh.
  - Giải mã từ sóng, chia chunk 1 / 3 / 7 / 64 byte, đảo dây, đo mốc thời gian SSC.
  - Mất dữ liệu (`lost`) và khoảng nghỉ (`idle`): gói kế tiếp mang cờ, không sinh gói giả.
  - PacketBuf đầy (dropped) và RawStore đầy (truncated, cột NODATA).
  - Lưu / đọc `.spmc`, giải mã lại từ sóng trong file, CSV xuất / nhập, file hỏng.
  - Analyzer (thiếu / thừa / khác / lỗi, gộp lỗi cùng vị trí), thanh ghi cuối, mapping thiếu nguồn bị loại.
- Dữ liệu sóng trong test và nguồn DEMO được sinh bằng `HCCSpmiSynth`. Đây là **dữ liệu mô phỏng**, không phải kết quả đo. Mọi chỗ hiển thị và lưu đều có nhãn DEMO.

## 7. Chưa kiểm chứng trên phần cứng

- PARLIO RX ở 80 MHz với 2 dây: cách đóng gói bit thực tế (bit0 = dây 0, LSB trước) và khả năng giải mã theo kịp mà không tràn khi bus bận liên tục.
- Mạch chuyển mức và tín hiệu SPMI thật của main (1,2 / 1,8 V, tần số SCLK thực tế).
- Việc nhận vai dây tự động trên tín hiệu thật.
- Ý nghĩa các bit dư sau khung (ACK/NACK của một số lệnh SPMI 2.0): hiện chỉ hiển thị giá trị thô, không diễn giải.
- Hiệu năng vẽ sóng khi thu nhỏ hết cỡ (tối đa ~4 triệu mẫu mỗi lần vẽ).
