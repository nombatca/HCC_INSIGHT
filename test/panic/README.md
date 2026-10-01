# Test Panic Parser v2 (host)

Kiểm tra `HCCPanicParser` + `HCCPanicFacts` trên máy tính (không cần board).

```bash
# Cần g++ và ESP-IDF (lấy cJSON): IDF_PATH mặc định ~/esp/esp-idf
test/panic/run_tests.sh                       # chạy mọi mẫu
test/panic/run_tests.sh --dump file.ips       # in toàn bộ fact của một file .ips
```

Thư mục `test/` nằm ngoài `components/` nên không được build vào firmware.

## Mẫu

- `samples/*.ips`: mẫu **tổng hợp** do `make_samples.py` sinh ra. Chúng **không phải
  panic thật**. Mỗi mẫu dựng lại một định dạng đã biết để kiểm tra cơ chế tách fact.
  Nguồn của từng mẫu ghi trong `samples/SOURCES.txt`.
- `samples/real/*.ips`: panic **thật**. Runner tự chạy thư mục này.
  - `busy_timeout_smc_charger_iphone13_4`: iPhone13,4, iOS 26.5.2. Đây là bản chép từ màn hình
    nên có dòng bị ngắt giữa chừng và entry cuối bị cắt.
  - `smc_bsc_failure_fragment`: đoạn payload SMC PANIC (T8140), thiếu header và product.
  - Cả hai đã xoá `crashReporterKey`.

### Thêm một panic .ips thật

1. Chép file vào `test/panic/samples/real/<tên>.ips`.
2. Chạy `test/panic/run_tests.sh --dump test/panic/samples/real/<tên>.ips` và đọc kỹ fact.
3. Tạo `<tên>.expect` cùng thư mục, mỗi dòng một kiểm tra:
   - `has: <dòng>`: kết quả phải chứa đúng dòng đó (chép từ `--dump` sau khi đã kiểm tra).
   - `lacks: <chuỗi>`: kết quả không được chứa chuỗi đó.
   - `same_signature: <tên mẫu khác>`: hai panic cùng lỗi phải có cùng signature.
   - `# ...`: chú thích.
4. Mọi mẫu, kể cả mẫu không có `.expect`, đều qua các kiểm tra chung:
   - không mã lỗi nào là địa chỉ, caller, thanh ghi hay `(x,y)` của IOService;
   - mọi fact đều có evidence;
   - signature không chứa địa chỉ và ổn định giữa các lần parse.

Panic thật có thể chứa ECID / serial. Hãy xoá các trường đó trước khi commit.

## Nguồn định dạng

| Định dạng | Nguồn | Mức |
|---|---|---|
| `panic(cpu N caller 0x…): <thông điệp>` | xnu `osfmk/kern/debug.c` | DOCUMENTED |
| `Debugger message`, `Memory ID`, `OS version`, `Kernel version`, `iBoot version`, `secure boot?`, `roots installed`, `Paniclog version`, `Panicked task/thread` | xnu `osfmk/arm/model_dep.c` | DOCUMENTED |
| `busy timeout[N], (Ns): 'name' (x,y), …`; `(x,y)` = `__state[0..1]` của IOService | xnu `iokit/Kernel/IOService.cpp` | DOCUMENTED |
| `@file:line` sau thông điệp | xnu `osfmk/kern/debug.h` (`PANIC_LOCATION`) | DOCUMENTED |
| `Kernel data abort.` …, register dump `x0: … esr: … far: …` | xnu `osfmk/arm64/sleh.c` | DOCUMENTED |
| ESR_ELx.EC (bit 31:26) | ARM Architecture Reference Manual | DOCUMENTED |
| `userspace watchdog timeout … checkins from <daemon>`, `Missing sensor(s): …`, `watchdog timeout … watchdogd` | log thật trên iFixit / Apple Community | OBSERVED |
| `SMC PANIC - ASSERT: <file>:<line>: <v>, <chữ>, …`, `S.sensor array A - B is v0, v1, …`, `F.sensor array …` | panic thật `samples/real/smc_bsc_failure_fragment.ips` | OBSERVED (ý nghĩa phần tử/bit: UNKNOWN) |
| Dump firmware `RTKit-… - Client: …`, mailbox `0x… [TX] …` (4 byte cao in được ASCII) | panic thật như trên | OBSERVED (ý nghĩa khoá: UNKNOWN) |
| `RELEASE_ARM64_Txxxx` trong kernel version | panic thật | OBSERVED (tên cấu hình kernel) |
| SMC / AOP / SEP / ANS / baseband / PMU / i2c (phần còn lại) | phần mềm đóng của Apple; chỉ nhận theo chữ | UNKNOWN chi tiết |

Mã nguồn xnu: https://github.com/apple-oss-distributions/xnu
