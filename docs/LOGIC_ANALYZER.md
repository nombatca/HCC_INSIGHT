# LOGIC ANALYZER 8 kênh

Đường vào: màn chính HCC INSIGHT → nút **LOGIC ANALYZER**.

## Phần cứng

- **Bộ thu:** PARLIO RX của ESP32-P4, rộng 8 bit, lấy mẫu liên tục, DMA đổ vào bộ đệm vòng trong RAM nội (16 × 4032 B), rồi được chép sang PSRAM.
- **Chân mặc định (menuconfig > HCC LOGIC ANALYZER):**

  | D0 | D1 | D2 | D3 | D4 | D5 | D6 | D7 |
  |---|---|---|---|---|---|---|---|
  | GPIO20 | GPIO21 | GPIO22 | GPIO23 | GPIO26 | GPIO27 | GPIO47 | GPIO48 |

  **Cần đối chiếu với chân thực tế trên header của board** và đổi trong menuconfig nếu khác. Firmware từ chối các chân đã có việc trên board:

  | Chân | Đang dùng cho |
  |---|---|
  | 7, 8 | I2C cảm ứng |
  | 9–13 | Âm thanh |
  | 14–19, 54 | SDIO Wi-Fi C6 |
  | 24, 25 | USB Serial/JTAG |
  | 32, 33 | LCD |
  | 37, 38 | UART0 |
  | 39–44 | Thẻ SD |
  | 53 | Ampli |

  Firmware cũng từ chối khi hai kênh trùng một chân. Nếu một kênh dùng chung chân với RFFE/SPMI, chức năng đó sẽ tạm bị khoá trong lúc thu.
- **Mức điện áp vào:** 0 / 3,3 V. Không cấp điện áp cao hơn 3,3 V vào chân.
  - Tín hiệu 1,8 V / 1,2 V: cần mạch chuyển mức một chiều.
  - Tín hiệu 5 V: cần mạch phân áp hoặc mạch đệm.
  - Luôn nối GND chung.
- **Chỉ nghe:** chân được cấu hình làm đầu vào, tắt điện trở kéo lên / kéo xuống.

## Thông số

| Mục | Giá trị |
|---|---|
| Tần số lấy mẫu | 100 kHz … 80 MHz (chia từ PLL 160 MHz) |
| Độ sâu | 256K / 1M / 2M / 4M / 8M mẫu (PSRAM). Ví dụ 2M mẫu ở 10 MHz = 0,2 s |
| Trigger | NGAY, cạnh lên / xuống / bất kỳ của một kênh, MẪU 8 bit (X / 0 / 1) |
| Trước điểm kích | 0–90 % |
| Giải mã (2 bộ cùng lúc) | I2C, UART 8N1 (baud tự đoán hoặc chọn), SPI mode 0–3, SPMI |
| Đo | Tần số, chu kỳ, duty, xung ngắn nhất của kênh đang chọn; con trỏ A / B đo khoảng thời gian B−A và 1/(B−A) |
| File | `/sdcard/logic/la_NNN.lab` (mở lại được) + `.vcd` (mở bằng PulseView / GTKWave) |

**Băng thông thực tế:** tín hiệu cần ít nhất 4 mẫu mỗi chu kỳ để giải mã ổn định. Ở 80 MHz lấy mẫu, xem được tín hiệu tới khoảng 20 MHz. Dùng cho I2C, UART, SPI vài MHz thì đặt tần số lấy mẫu 1–20 MHz là đủ.

**Mất dữ liệu (40–80 MHz):** firmware báo rõ khi xảy ra, không bỏ qua.
- **Trước điểm kích:** phần đã thu bị bỏ (vì không còn liên tục) và thu lại từ đầu.
- **Sau điểm kích:** dừng ngay, hiện **OVERFLOW**, chỉ giữ phần dữ liệu liên tục đến chỗ bị mất.

## Nguồn DEMO

CÀI ĐẶT → Nguồn → **DEMO** tạo dữ liệu **mô phỏng**: I2C trên D0/D1, UART trên D2, SPI trên D3–D6, xung vuông 10 kHz trên D7. Dùng để thử giao diện và bộ giải mã khi không có que đo. Dữ liệu này luôn có nhãn DEMO trên màn và trong tên file (`_DEMO`).

## Mã nguồn

| File | Nội dung |
|---|---|
| `HCCLogicCore.*` | Trigger, bảng tóm tắt để vẽ nhanh (mipmap), đo, bộ tạo DEMO. Không phụ thuộc ESP-IDF |
| `HCCLogicDecode.*` | Giải mã I2C / UART / SPI / SPMI. Không phụ thuộc ESP-IDF |
| `HCCLogicFile.*` | `.lab` / `.vcd` |
| `HCCLogic.*` | Engine thu PARLIO (task chạy trên nhân 0) |
| `HCCLogicView.*` | Giao diện |

Test: `test/logic/run_tests.sh`, gồm 69 kiểm tra.

## Chưa kiểm chứng trên máy thật

- PARLIO 8 bit chạy thật: đúng thứ tự bit (bit k = Dk) hay không, và tần số tối đa thu được mà không tràn.
- Các chân mặc định có thực sự ra header của board 7B hay không.
