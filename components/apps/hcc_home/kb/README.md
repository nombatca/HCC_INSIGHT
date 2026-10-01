# Knowledge Base V1: phân tích panic iPhone

Code: `HCCPanicKBv1.hpp/.cpp` (schema, validator, matcher, migrate) và `HCCPanicKBStore.hpp/.cpp`
(đọc flash/SD). V1 **chưa nối vào giao diện / ANALYZE**: đó là việc của BƯỚC 3.

## Nơi lưu

| Nơi | Đường dẫn | Nội dung |
|---|---|---|
| Firmware | `kb/hcc_kb_default.json` (nhúng bằng `EMBED_TXTFILES`) | KB mặc định |
| SD | `kb/v1/user/*.json` | KB người dùng: cùng định dạng, thêm hoặc ghi đè theo `id` |
| SD | `kb/v1/cases/*.json` | Case sửa chữa `hcc.case.v1` (VERIFIED / REPAIR_CONFIRMED / DRAFT) |
| SD (cũ) | `kb/verified/*.json`, `kb/repair_confirmed/*.json` | Case của HCCPanicKB cũ. Chỉ đọc, được migrate trong bộ nhớ, không xoá |

## Mức bằng chứng

| Mức | Điều kiện (validator kiểm tra) |
|---|---|
| `DOCUMENTED` | Có `sources` và **mọi** nguồn có type `apple_source`, `apple_doc` hoặc `standard` |
| `INFERRED` | Bắt buộc có `reasoning`. Nguồn cộng đồng (`community`, `research`, `hcc_observation`) chỉ được dùng ở mức này |
| `VERIFIED` | **Chỉ** đến từ case sửa thực tế. Rule có level VERIFIED bị từ chối |
| `UNKNOWN` | Chưa đủ bằng chứng. `value` = `"UNKNOWN"` |

Rule vi phạm sẽ bị bỏ, lỗi được ghi vào `kb.errors`. Driver rule phải có `requires_facts >= 2`: chỉ được
nêu `components` khi có ít nhất 2 dữ kiện độc lập cùng nhóm.

## Các loại record (`hcc.kb.v1`)

- `sources[]`: `id`, `type`, `title`, `ref`, `note`
- `groups[]`: `id` (9 nhóm), `name`, `kinds[]` (tên PanicKind), `tags[]` (tag tên driver)
- `kind_rules[]`: `id`, `kind`, `group`, `mechanism`, `hardware_area`, `checks[]`
- `driver_rules[]`: `id`, `group`, `match{exact,prefix,contains}`, `subsystem`, `hardware_area`,
  `bus_rail`, `components`, `requires_facts`
- `sensor_rules[]`: `id`, `name`, `models[]` (rỗng = mọi model), `meaning`, `location`
- `code_rules[]`: `id`, `label` (được dùng `*`), `models[]`, `meaning`, `bits[]{bit, meaning}`
- `unknowns[]`: `id`, `what` (driver|sensor|code|assert_text|mailbox_ascii|kernel_target),
  `value` (được dùng `*`), `reason`, `seen_in`

Mỗi field (`mechanism`, `subsystem`, ...) có dạng `{"value", "level", "sources": [], "reasoning"}`.

## Thêm một VERIFIED CASE

1. Copy `test/panic/kb/example_verified_case.json` vào SD thành `kb/v1/cases/<id>.json`.
2. Lấy `model`, `ios`, `panic_type`, `primary_kind`, `signature`, `signature_generic`, `drivers`,
   `sensors`, `codes` từ panic của máy đó. Có hai cách:
   - `test/panic/run_tests.sh --dump <file.ips>` (dòng `signature:` / `signature_generic:`);
   - hoặc dùng `HCCKB::caseFromFacts()` (BƯỚC 3 sẽ có nút tạo sẵn).
3. Điền phần sửa chữa: `hardware_area`, `components[]` (`name`, `ref`, `action`:
   replaced/repaired/reworked/cleaned/other), `bus_rail[]`, `measurements[]` (`point`, `type`,
   `before`, `after`, `unit`), `repair_action`, `result` (`panic_resolved`, `observed_days`), `notes`,
   `date`, `evidence` (`panic_file`, `panic_excerpt`, `attachments`).
4. `status: "VERIFIED"` chỉ được giữ nếu có `signature`, `model`, `repair_action` và
   `result.panic_resolved = true`. Thiếu một trong các điều kiện đó thì case tự hạ xuống `REPAIR_CONFIRMED`
   kèm cảnh báo. Dữ liệu vẫn giữ nguyên.
