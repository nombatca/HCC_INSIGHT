#include "HCCiPhoneDiagnostics.hpp"

#include <ctime>
#include <cstring>

namespace {
uint32_t nowTs() { return static_cast<uint32_t>(time(nullptr)); }
bool nonEmpty(const char *s) { return s != nullptr && s[0] != '\0'; }
}  // namespace

const char *HCCiPhoneDiagnostics::statusText(Status s)
{
    switch (s) {
        case Status::Pass: return "PASS";
        case Status::Fail: return "FAIL";
        case Status::Check: return "CHECK";
        case Status::Unsupported: return "UNSUPPORTED";
        case Status::NotTested:
        default: return "NOT TESTED";
    }
}

bool HCCiPhoneDiagnostics::sessionReady() const
{
    return _t != nullptr && _t->sessionEstablished() && _t->deviceInfoReady();
}

HCCiPhoneDiagnostics::Status HCCiPhoneDiagnostics::rollup(const std::vector<Item> &items)
{
    bool anyFail = false, anyCheck = false, anyPass = false, anyNot = false;
    for (const auto &it : items) {
        switch (it.status) {
            case Status::Fail: anyFail = true; break;
            case Status::Check: anyCheck = true; break;
            case Status::Pass: anyPass = true; break;
            case Status::NotTested: anyNot = true; break;
            case Status::Unsupported: break;
        }
    }
    if (anyFail) return Status::Fail;
    if (anyCheck) return Status::Check;
    if (anyPass) return Status::Pass;
    if (anyNot) return Status::NotTested;
    return Status::Unsupported;
}

// ---- SENSORS: iOS không cấp dữ liệu sensor sống qua Lockdown -> UNSUPPORTED (V1) ----
HCCiPhoneDiagnostics::Group HCCiPhoneDiagnostics::checkSensors()
{
    Group g;
    g.name = "SENSORS";
    const uint32_t ts = nowTs();
    static const char *sensors[] = {"Accelerometer", "Gyroscope", "Proximity",
                                    "Ambient Light", "Thermal/Sensor"};
    for (const char *s : sensors) {
        Item it;
        it.name = s;
        it.status = Status::Unsupported;
        it.source = "Lockdown (khong co sensor API)";
        it.evidence = "iOS khong cung cap du lieu sensor song qua USB/Lockdown. "
                      "V2: diagnostics_relay/IORegistry (chi phat hien hien dien, khong PASS chuc nang).";
        it.ts = ts;
        g.items.push_back(it);
    }
    g.status = rollup(g.items);
    return g;
}

// ---- FACE ID / TRUEDEPTH: chỉ dò lỗi subsystem qua os_trace, KHÔNG bao giờ PASS ----
HCCiPhoneDiagnostics::Group HCCiPhoneDiagnostics::checkFaceID()
{
    Group g;
    g.name = "FACE ID / TRUEDEPTH";
    const uint32_t ts = nowTs();

    // Item chính: trạng thái subsystem từ log.
    Item subsys;
    subsys.name = "Face ID subsystem (log)";
    subsys.source = "os_trace_relay (biometrickitd/Pearl/coreauth)";
    subsys.ts = ts;

    if (!sessionReady()) {
        subsys.status = Status::NotTested;
        subsys.evidence = "Phien Lockdown chua san sang.";
    } else if (!_t->osTraceActive()) {
        subsys.status = Status::NotTested;
        subsys.evidence = "os_trace chua hoat dong tren phien nay.";
    } else {
        const uint32_t matches = _t->osTraceBioMatches();
        const uint32_t errors = _t->osTraceBioErrors();
        char ev[320];
        if (matches == 0) {
            snprintf(ev, sizeof(ev), "No relevant diagnostic evidence (biometrickitd/Pearl/TrueDepth)");
        } else {
            snprintf(ev, sizeof(ev), "log lines=%u, errors=%u; match=%s",
                     (unsigned)matches, (unsigned)errors, _t->osTraceLastBio());
        }
        subsys.evidence = ev;
        if (errors >= 3) {
            subsys.status = Status::Fail;
            subsys.code = "biometric_errors>=3";
        } else if (errors >= 1) {
            subsys.status = Status::Check;
            subsys.code = "biometric_errors";
        } else if (matches > 0) {
            // Có hoạt động subsystem, 0 lỗi -> KHÔNG kết luận PASS (iOS khong cap suc khoe linh kien).
            subsys.status = Status::Unsupported;
        } else {
            subsys.status = Status::NotTested;
            subsys.evidence += " (chua bat duoc log biometric trong cua so os_trace)";
        }
    }
    g.items.push_back(subsys);

    // Item ghi rõ giới hạn: sức khoẻ từng linh kiện không lấy được.
    Item comp;
    comp.name = "Dot Projector / IR Camera / Flood (component)";
    comp.status = Status::Unsupported;
    comp.source = "iOS khong expose suc khoe linh kien Face ID";
    comp.evidence = "Khong co service/khoa hop le cho suc khoe tung linh kien; khong doan PASS. "
                    "Khong truy cap du lieu/template sinh trac.";
    comp.ts = ts;
    g.items.push_back(comp);

    g.status = rollup(g.items);
    return g;
}

// ---- BASEBAND / CELLULAR: nguồn chính là Lockdown GetValue; log là evidence bổ sung ----
HCCiPhoneDiagnostics::Group HCCiPhoneDiagnostics::checkBaseband()
{
    Group g;
    g.name = "BASEBAND / CELLULAR";
    const uint32_t ts = nowTs();

    if (!sessionReady()) {
        Item it;
        it.name = "Baseband modem";
        it.status = Status::NotTested;
        it.source = "Lockdown session";
        it.evidence = "Phien Lockdown chua san sang (chua pair/session hoac chua doc xong info).";
        it.ts = ts;
        g.items.push_back(it);
        g.status = rollup(g.items);
        return g;
    }

    const char *ver = _t->basebandVersion();
    const char *stat = _t->basebandStatus();
    const char *imei = _t->imei();
    const char *iccid = _t->iccid();
    const char *meid = _t->meid();
    const char *phone = _t->phoneNumber();

    // 1) Baseband được iOS nhận diện (KHÔNG suy ra RF/RFFE/sóng PASS).
    {
        Item it;
        it.name = "Baseband Detection";
        it.source = "Lockdown GetValue (BasebandVersion, BasebandStatus)";
        char ev[240];
        snprintf(ev, sizeof(ev),
                 "PASS = iOS nhan dien baseband/modem + tra du lieu hop le (KHONG suy ra RF/song PASS). "
                 "BasebandVersion='%s', BasebandStatus='%s'",
                 ver ? ver : "", stat ? stat : "");
        it.evidence = ev;
        it.ts = ts;
        if (nonEmpty(ver) && nonEmpty(stat)) {
            it.status = Status::Pass;  // modem có bản version + trạng thái -> được nhận diện
        } else if (nonEmpty(ver)) {
            it.status = Status::Check;
            it.code = "no_baseband_status";
        } else {
            it.status = Status::Unsupported;  // máy Wi-Fi-only / iOS không cung cấp baseband
            it.code = "no_baseband";
        }
        g.items.push_back(it);
    }

    // 2) IMEI (định danh modem).
    {
        Item it;
        it.name = "IMEI";
        it.source = "Lockdown GetValue (InternationalMobileEquipmentIdentity)";
        it.evidence = std::string("IMEI='") + (imei ? imei : "") + "'";
        it.status = nonEmpty(imei) ? Status::Pass : Status::Unsupported;
        it.ts = ts;
        g.items.push_back(it);
    }

    // 3) SIM (ICCID).
    {
        Item it;
        it.name = "SIM (ICCID)";
        it.source = "Lockdown GetValue (IntegratedCircuitCardIdentity)";
        it.evidence = std::string("ICCID='") + (iccid ? iccid : "") + "'";
        if (nonEmpty(iccid)) it.status = Status::Pass;         // SIM có mặt
        else it.status = Status::NotTested;                    // không SIM / không cung cấp (không phải lỗi)
        it.ts = ts;
        g.items.push_back(it);
    }

    // 4) Định danh khác (MEID / số thuê bao) - evidence. Rỗng KHÔNG phải FAIL
    //    (nhiều model/carrier không cung cấp các trường này).
    {
        Item it;
        it.name = "Cellular identity";
        it.source = "Lockdown GetValue (MEID, PhoneNumber)";
        char ev[220];
        snprintf(ev, sizeof(ev), "MEID='%s', PhoneNumber='%s'", meid ? meid : "", phone ? phone : "");
        it.evidence = ev;
        if (nonEmpty(meid) || nonEmpty(phone)) {
            it.status = Status::Pass;
        } else {
            it.status = Status::Unsupported;  // model/carrier khong cung cap - khong phai loi
            it.evidence += " (model/carrier khong cung cap - khong phai loi)";
        }
        it.ts = ts;
        g.items.push_back(it);
    }

    // 5) Log evidence bổ sung (CommCenter/baseband) từ os_trace.
    if (_t->osTraceActive()) {
        Item it;
        it.name = "Baseband log (os_trace)";
        it.source = "os_trace_relay (commcenter/baseband/modem)";
        const uint32_t m = _t->osTraceBasebandMatches();
        const uint32_t e = _t->osTraceBasebandErrors();
        char ev[320];
        if (m == 0) {
            snprintf(ev, sizeof(ev), "No relevant diagnostic evidence (commcenter/baseband/modem/cellular)");
        } else {
            snprintf(ev, sizeof(ev), "log lines=%u, errors=%u; match=%s",
                     (unsigned)m, (unsigned)e, _t->osTraceLastBaseband());
        }
        it.evidence = ev;
        it.ts = ts;
        if (e >= 3) { it.status = Status::Fail; it.code = "bb_errors>=3"; }
        else if (e >= 1) { it.status = Status::Check; it.code = "bb_errors"; }
        else it.status = Status::NotTested;  // không lỗi -> chỉ là evidence, không PASS
        g.items.push_back(it);
    }

    g.status = rollup(g.items);
    return g;
}

// ---- BATTERY / POWER: dữ liệu thật từ diagnostics_relay All -> GasGauge ----
HCCiPhoneDiagnostics::Group HCCiPhoneDiagnostics::checkBattery()
{
    Group g;
    g.name = "BATTERY / POWER";
    const uint32_t ts = nowTs();

    if (!sessionReady()) {
        Item it;
        it.name = "Gas Gauge";
        it.status = Status::NotTested;
        it.source = "diagnostics_relay (All)";
        it.evidence = "Phien Lockdown chua san sang.";
        it.ts = ts;
        g.items.push_back(it);
        g.status = rollup(g.items);
        return g;
    }
    if (!_t->batteryAvailable()) {
        Item it;
        it.name = "Gas Gauge";
        it.status = Status::NotTested;
        it.source = "diagnostics_relay (All -> Diagnostics.GasGauge)";
        it.evidence = "Chua nhan duoc GasGauge (diagnostics_relay chua chay xong tren phien nay).";
        it.ts = ts;
        g.items.push_back(it);
        g.status = rollup(g.items);
        return g;
    }

    const int32_t cyc = _t->batteryCycleCount();
    const int32_t design = _t->batteryDesignCapacity();
    const int32_t fcc = _t->batteryFullChargeCapacity();

    // 1) Pin/Gas gauge được nhận diện (IC gas gauge phản hồi qua iOS).
    {
        Item it;
        it.name = "Battery Detection (GasGauge)";
        it.source = "diagnostics_relay All -> Diagnostics.GasGauge";
        char ev[220];
        snprintf(ev, sizeof(ev),
                 "PASS = iOS doc duoc gas gauge (pin nhan dien). CycleCount=%d, DesignCapacity=%d mAh, FullChargeCapacity=%d",
                 (int)cyc, (int)design, (int)fcc);
        it.evidence = ev;
        it.status = Status::Pass;
        it.ts = ts;
        g.items.push_back(it);
    }

    // 2) Chu kỳ sạc (evidence độ chai; KHÔNG kết luận thay pin).
    {
        Item it;
        it.name = "Battery cycle count";
        it.source = "diagnostics_relay All -> GasGauge.CycleCount";
        char ev[160];
        snprintf(ev, sizeof(ev), "CycleCount=%d", (int)cyc);
        it.evidence = ev;
        if (cyc < 0) {
            it.status = Status::NotTested;
        } else if (cyc >= 1000) {
            it.status = Status::Check;  // chu ky cao -> can kiem tra do chai
            it.code = "high_cycle";
        } else {
            it.status = Status::Pass;
        }
        it.ts = ts;
        g.items.push_back(it);
    }

    g.status = rollup(g.items);
    return g;
}

HCCiPhoneDiagnostics::FullResult HCCiPhoneDiagnostics::runFullCheck()
{
    FullResult r;
    r.sensors = checkSensors();
    r.faceid = checkFaceID();
    r.baseband = checkBaseband();
    r.battery = checkBattery();
    return r;
}
