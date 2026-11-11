# Quản lý tệp trong USB Disk

Mở USB DISK → QUẢN LÝ TỆP khi USB Disk đã tắt. Danh sách hiển thị tối đa
64 mục mỗi trang; TRƯỚC/SAU chuyển trang, LÊN MỘT CẤP về thư mục cha,
MỞ vào thư mục được chọn, BACKUP mở nhanh hcc_backup, LÀM MỚI đọc lại thẻ.
Dung lượng trống và tổng dung lượng lấy từ FAT sau mỗi thao tác.

Chọn một mục → XÓA MỤC CHỌN → kiểm tra tên/đường dẫn → XÓA VĨNH VIỄN.
Xóa thư mục bao gồm toàn bộ nội dung con, kể cả bản .ready/.removed trong
phiên backup đã chọn. Đây là xóa thật để giải phóng dung lượng, không chuyển
vào vùng thu hồi. DỪNG TÁC VỤ dừng tại ranh giới thao tác; những mục đã xóa
không được phục hồi. Lỗi hoặc dừng giữa chừng báo số mục đã xóa và đọc lại
thư mục; không báo thành công giả. Không có thao tác định dạng.

Tác vụ nền không giữ con trỏ LVGL/app. Rời màn hình không làm hỏng UI; tác vụ
đã xác nhận tiếp tục và vẫn giữ quyền thẻ tới khi hoàn tất. Bộ quản lý dùng
cùng SdAccess với USB Disk và backup, từ chối truy cập khi hai tính năng đó
đang giữ thẻ. Các thành phần SD cũ chưa dùng chung khóa hoàn toàn: tránh
đọc/ghi thẻ từ các ứng dụng khác trong khi xóa. Đây chưa phải khóa mọi truy
cập thẻ toàn hệ thống.

Đường dẫn tương đối, cấm xóa gốc thẻ, traversal, ký tự mơ hồ trên FAT,
symlink ở mục/ancestor trên host. Xóa cây duyệt DFS với 24 cấp và đường dẫn
512 byte; vượt giới hạn/lỗi có thể để lại phần cây chưa xóa. Mỗi thao tác
revalidate loại mục, không dùng tên nhập qua shell. Không mount khi cấu hình
BSP tự định dạng lúc mount lỗi được bật. Mỗi trang 64 tên, 255 byte/tên;
phân trang theo thứ tự filesystem, thay đổi từ bên ngoài cần LÀM MỚI.

Kiểm thử host: sh tests/sd_files/run_tests.sh; hỗ trợ HCC_TEST_FLAGS cho
ASan/UBSan. Kiểm tra phân trang 150 tệp, xóa cây, tiếng Việt, hủy giữa chừng,
traversal/gốc/đổi loại/symlink/độ sâu, mount lỗi/không format, task lỗi và khóa
với USB Disk/backup. Firmware build CI xác nhận API LVGL/ESP-IDF; giao diện
và thao tác trên thẻ thật cần kiểm tra trên P4.
