# Nút MDM — kiểm tra dịch vụ mobilebackup2

Thay nút DFU trong USB HUB bằng MDM, giữ nguyên bố cục. Phát hiện chế độ DFU của thiết bị vẫn được giữ.

## Sử dụng

1. Mở USB HUB, bấm MDM. Màn hình yêu cầu rút/cắm lại thiết bị. Có thể bấm khi chưa cắm thiết bị rồi cắm vào.
2. Mở khóa iPhone/iPad, chấp nhận Tin cậy nếu máy yêu cầu.
3. Trong lần kết nối đó, sau pair/StartSession và đọc thông tin máy, HCC mở com.apple.mobilebackup2 thay cho dịch vụ diagnostics_relay đang dùng TCP #2.
4. HCC dùng cổng và EnableServiceSSL thực tế từ StartService. TLS, nếu cần, là phiên riêng của dịch vụ, dùng lớp kết nối hiện có.
5. HCC thương lượng DeviceLink và Hello mobilebackup2; màn hình báo đang kết nối, đang thương lượng, sẵn sàng hoặc thất bại.

Thông báo sẵn sàng CHỈ xác nhận thương lượng dịch vụ. Chưa kiểm tra có MDM hay không, chưa xóa profile, chưa gửi yêu cầu backup/restore, không xác nhận thiết bị được giải phóng khỏi Apple Business Manager.

Chế độ chỉ áp dụng cho một lần kết nối. Khi rút/cắm lại sau lần kiểm tra, chế độ chẩn đoán mặc định trở lại. Muốn thử lại sau lỗi: bấm MDM rồi rút/cắm lại. Không thay dịch vụ giữa một phiên TLS đang chạy.

## Thay đổi kỹ thuật

- components/hcc_backup chứa core C++17 V03 và được apps khai báo dependency rõ ràng. Test/demo đặt ngoài apps để không bị GLOB_RECURSE đưa vào firmware.
- HCCMdmProbe.inc được include một lần sau ServiceTlsContext; giữ phần nối riêng để kiểm thử bằng header HCCUSBTransport thật với SDK/I/O stub trên host.
- Yêu cầu từ UI dùng atomic state; chỉ task USB mở kết nối, cấp phát và xử lý giao thức. UI chỉ đọc trạng thái và lên lịch lại qua cơ chế có sẵn.
- Chỉ chuyển dữ liệu vào parser MDM ở socket mobilebackup2 sau StartSession. Socket notification dùng trong bước pair vẫn dùng bộ xử lý cũ.
- Trường hợp SSL: BackupLink trực tiếp sở hữu ssl_read/write, buffer retry giữ nguyên; nhánh đọc os_trace không cùng tiêu thụ stream. Trường hợp không SSL: nhận qua vùng chờ 4 KiB hiện có; TX có loại riêng để tăng TCP sequence sau callback hoàn tất.
- Control frame tối đa 8 KiB, text arena 8 KiB, 128 plist nodes, output 1 KiB. Context cấp trong PSRAM, thu hồi khi reset socket/rút cáp. Kích thước host đo được 24.040 byte; chưa đo ABI/RAM trên ESP32-P4.
- Timeout không tiến triển trong thương lượng 5 giây; deadline mở dịch vụ/thương lượng 30 giây tính từ StartService. Chờ người dùng xác nhận Trust trước StartService chưa có deadline mới.
- Task USB có thời gian chờ hữu hạn trong lúc kiểm tra để timeout không phụ thuộc việc có gói đến.

## Kiểm chứng ngày 07/11/2026

Chạy từ gốc repo:

```sh
sh tests/backup_link/run_tests.sh
sh tests/mdm_probe/run_tests.sh
```

27 nhóm test core + 2 demo đạt trên Linux/GCC. 5 nhóm test phần nối firmware đạt: yêu cầu lặp, plaintext, TLS giả lập có partial I/O/WANT và cùng con trỏ retry, không đọc quá Hello, timeout, thiếu PSRAM, buffer quá lớn, lỗi gửi, TLS đóng và thu hồi context.

5 nhóm phần nối cũng đạt với ASan/UBSan và -fno-exceptions -fno-rtti. LeakSanitizer không chạy (detect_leaks=0); bộ cấp phát giả có kiểm tra cân bằng cấp/giải phóng. Các phép thử dùng header thật và HCCMdmProbe.inc thật; không build toàn bộ HCCUSBTransport.cpp, HCCHome.cpp hoặc ESP-IDF.

Chưa có ESP-IDF/toolchain trong môi trường này; chưa build firmware, chưa thử USB/TLS thật hoặc iPad. Cần build bằng môi trường ESP-IDF đang dùng và lấy log serial trước khi kết luận tương thích.

## Nguồn đối chiếu

- https://github.com/libimobiledevice/libimobiledevice/blob/master/src/device_link_service.c
- https://github.com/libimobiledevice/libimobiledevice/blob/master/src/mobilebackup2.c
- https://github.com/opensource-apple/CF/blob/master/CFBinaryPList.c
- https://mbed-tls.readthedocs.io/projects/api/en/v3.6.3/api/file/ssl_8h/

Core được viết riêng, fixture là dữ liệu tổng hợp; không đưa payload của 4uKey vào repo.

## Build trên GitHub Actions

Workflow `.github/workflows/hcc-build.yml` chạy kiểm thử host trước, sau đó dùng
action chính thức của Espressif build ESP32-P4 với ESP-IDF v5.5.5, phiên bản đang
dùng cho P4. Lần build đầu với v5.5 đã dừng ở dependency solver với MissingKconfigError
ESP_HOSTED_HOST_USB_CHERRYUSB_MANIFEST; kiểm thử host trên GitHub đã đạt. Bản cập nhật
chuyển SDK CI sang v5.5.5 và đang chờ kiểm chứng build sạch.

Sau khi áp dụng bản vá lên repo:

1. Commit/push lên main, hoặc mở pull request. Workflow chạy tự động.
2. Hoặc vào Actions → HCC INSIGHT build → Run workflow khi workflow đã có trên
   nhánh mặc định.
3. Nếu build thành công, tải artifact HCC_INSIGHT-esp32p4-<commit>, rồi giải nén
   HCC_INSIGHT_firmware.zip bên trong.
4. Gói giữ nguyên các ảnh trong flash_files của flasher_args.json, offset,
   flash_settings và cấu trúc thư mục. Không nạp app .bin đơn lẻ vào địa chỉ 0.
5. Nếu build lỗi, mở bước lỗi và tải artifact build-diagnostics nếu có.

Thời gian giới hạn: host 10 phút, firmware 45 phút. Artifact firmware lưu 14 ngày,
log lỗi 7 ngày. Đây là các giá trị cấu hình của workflow.

`tools/package_firmware.py` chỉ đóng gói các file được ESP-IDF khai báo, bổ sung
SHA256SUMS và hướng dẫn. Ba test đóng gói đã đạt trên host: ảnh/offset/checksum,
thiếu ảnh không ghi đè bản cũ, và từ chối đường dẫn thoát thư mục build. Cấu trúc
YAML cũng đã kiểm tra cục bộ; chưa chạy GitHub Actions hoặc build firmware.

Bản vá này áp dụng từ commit 50e5b3a7cc3158bb74bb836e6eae58d8b68122df. Nếu đã áp
dụng bản vá MDM trước đó, không áp lại bản vá tổng hợp lên cùng những thay đổi đó.
