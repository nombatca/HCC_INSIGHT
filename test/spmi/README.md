# Test SPMI MONITOR

```
test/spmi/run_tests.sh
```

Chạy trên máy tính, chỉ cần g++ (có ASan / UBSan). Phạm vi kiểm thử:

- Bộ giải mã: vector bit tính tay, parity, gói bị cắt, mất dữ liệu.
- Bộ đệm gói / sóng, kể cả khi tràn.
- File `.spmc` / CSV.
- PMIC analyzer.

Xem `docs/SPMI_MONITOR.md`.

Sóng dùng trong test được sinh bằng `HCCSpmiSynth`. Đây là dữ liệu mô phỏng, không phải kết quả đo.
