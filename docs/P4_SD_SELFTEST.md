# Kiểm tra SD trên P4 thật

Bài thử này xác nhận FAT/SD thực tế trước khi bật luồng nhận dữ liệu iPad.
Chỉ ghi 64 byte mẫu và metadata trong một thư mục HCC mới; không gửi Backup,
Restore hoặc thay đổi MDM trên iPad.

## Nạp firmware

Tắt USB DISK trên board, cắm thẻ SD và dùng terminal ESP-IDF đang làm việc:

```sh
git fetch origin
git switch codex/mdm-on-current-p4
git pull --ff-only
idf.py build flash monitor
```

## Chạy

1. Mở USB HUB trên P4, bấm ngắn MDM rồi rút/cắm lại iPad nếu cần để chạy probe.
2. Chờ log `NEGOTIATION READY protocol=2.1`. Nút hiện thêm dòng `Giữ: kiểm tra SD`.
3. Giữ nút MDM khoảng một giây rồi thả.
4. Chờ màn hình báo `SD: PASS - ghi/chốt/đọc/dọn OK` hoặc thông báo lỗi.

Log mong đợi:

```text
[HCC SDTEST] START version=1 bytes=64
[HCC SDTEST] PASS bytes=64 commit=1 abort=1 session=/sdcard/hcc_backup/rx_...
```

Bài thử tạo `000001.ready/payload.bin` và `identity.bin`, kiểm tra chính xác nội
dung/EOF, sau đó tạo và hủy file thứ hai để kiểm tra dọn thư mục `.part`. File
mẫu đã chốt được giữ lại để đối chiếu. Thẻ cần tối thiểu 1 MiB trống. Worker
riêng chạy thao tác SD; quyền thẻ được nhả sau khi dọn xong.

Nếu lỗi, gửi các dòng `[HCC SDTEST]` và log lỗi SD/FAT gần đó. Các bước lỗi:
`busy`, `storage`, `begin`, `write`, `commit`, `verify`, `abort_cleanup`, `task`.
Nếu không thấy dòng START hoặc dòng hướng dẫn trên nút, ghi lại commit đang
nạp (`git rev-parse --short HEAD`) và trạng thái MDM trên màn hình.

Bấm ngắn MDM sau khi bài thử kết thúc để trở về trạng thái probe. Giữ lâu trên
nút FASTBOOT của Android không chạy bài thử SD; bấm ngắn vẫn dùng FASTBOOT.

Kết quả host/CI không xác nhận SD thật. Cần PASS trên P4 trước khi tích hợp
backup capture vào UI. Các filesystem control chưa hỗ trợ và raw 0x0B vẫn cần
hoàn thiện; PASS SD không có nghĩa đã backup hoặc gỡ MDM.
