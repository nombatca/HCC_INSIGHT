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
backup capture vào UI. Các filesystem control còn thiếu và raw 0x0B cần xác nhận thêm trên iPad; PASS SD không có nghĩa đã backup hoặc gỡ MDM.

## Kết quả thực tế đã nhận — 08/11/2026

Người dùng đã chạy PASS ba lần trên P4, session `rx_000002`, `rx_000003`,
`rx_000004`: mỗi lần `bytes=64 commit=1 abort=1`. Đây là xác nhận bài thử
SD nhỏ trên phần cứng; chưa xác nhận truyền backup lớn hoặc raw 0x0B.

Bộ điều phối prototype hiện xử lý thêm `DLMessageGetFreeDiskSpace`. Phản hồi
là ngân sách nhận hiệu dụng: nhỏ hơn giữa dung lượng FAT còn trống và quota
payload còn lại. Metadata cũng dùng SD nên con số này không bảo đảm toàn bộ
quota sẽ ghi được; lỗi ghi vẫn phải dừng. Chỉ trả trạng thái request về running
sau khi phản hồi được gửi đủ. Tính năng này chưa nối vào nút MDM.

## Bản tích hợp mới: bước thử nhận dữ liệu

Sau khi cập nhật đúng nhánh và build/nạp thành công, chờ protocol 2.1, giữ MDM
để chạy SD test. Sau PASS, nút đổi thành `Giữ: nhận backup`; giữ lần nữa để chạy
capture thử nghiệm có giới hạn. Chờ log `[HCC MDM] BACKUP started`, các dòng
`[HCC BACKUP]`, rồi `BACKUP complete` hoặc `BACKUP stopped`. Dừng/lỗi có thể giữ
file đã chốt để chẩn đoán; không có nghĩa backup hoàn chỉnh hoặc đã gỡ MDM.

Mỗi phiên chỉ chạy một lượt. Rút/cắm và thương lượng lại trước lượt tiếp theo.
Rút thiết bị khi đang nhận phải hủy file tạm và nhả quyền SD. Khi gửi log kết quả,
ưu tiên các dòng `[HCC MDM]`, `[HCC BACKUP]`, SD/FAT và lỗi; không cần gửi lại
IMEI, serial, ICCID hay số điện thoại. Không dùng erase-flash.

Nếu sau DownloadFiles chưa có bước tiếp, bản có watchdog sẽ in `[HCC BACKUP] wait`
khoảng mỗi 5 giây. Giữ nguyên lượt đang chạy ít nhất 70 giây sau phản hồi cuối,
không bấm MDM/rút cáp/thoát monitor trong khoảng đó. Gửi các dòng `wait`, `process`,
`stop` và `watchdog stopped`/`BACKUP stopped`. Không cần gửi log gesture/Wi-Fi.
Nếu cả dòng `wait` cũng không xuất hiện thì chưa thể xác nhận owner task còn
polling; cần đối chiếu đúng commit đã build và log từ đầu lượt.
