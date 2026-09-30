#pragma once

#include "HCCiPhoneDiagnostics.hpp"

// Android hardware check over ADB (no LVGL, no USB): command() is run as one ADB shell job,
// parse() turns its output into the same four groups the HARDWARE CHECK screen shows for
// iPhone (sensors, biometrics, baseband/SIM, battery).
//
// Runtime evidence (what the sensor HAL / services report) is compared with what the phone's
// static configuration declares (`pm list features`): declared but missing means the part did
// not come up, which is the useful repair signal. Nothing here proves a part works end to end.
class HCCAndroidDiagnostics {
public:
    using Status = HCCiPhoneDiagnostics::Status;
    using Item = HCCiPhoneDiagnostics::Item;
    using Group = HCCiPhoneDiagnostics::Group;
    using FullResult = HCCiPhoneDiagnostics::FullResult;

    static const char *command();

    // ---- Guided camera test (TEST CAMERA) ----
    // One step per camera: front first, then back main, ultra-wide, tele..., macro/other.
    struct CameraStep {
        std::string title;       // "CAMERA TELE (~3x)  (ID 3, 8.0 MP)"
        std::string instruction; // what the technician does / looks at
        int launch_facing = -1;  // 1 = open the camera app on the front camera, 0 = back, -1 = none
        bool front = false;      // the step needs a front camera open (else a back one)
        std::vector<std::string> facing_ids; // camera IDs on that side, to check what the app opened
        double zoom = 0;         // lens to select in the camera app (0.5, 3...); 0 = leave as is
    };
    // A button found in the camera app's UI (uiautomator dump), to tap with `input tap`.
    struct UiTap {
        bool found = false;
        int x = 0;
        int y = 0;
        std::string label; // its text / description, shown to the technician
    };
    // Shell fragment: camera IDs open now (##O) and the camera app's buttons (##U).
    static const char *cameraProbeCommand();
    static UiTap findSwitchCameraButton(const char *probe_output);
    // zoom < 1: the ultra-wide button (smallest value); else the value closest to zoom.
    static UiTap findZoomButton(const char *probe_output, double zoom);
    struct CameraStepResult {
        std::string title;
        int verdict = -1;   // 1 = technician says OK, 0 = faulty, -1 = skipped
        std::string opened; // camera IDs the camera service had open at check time
        std::string errors; // camera error log lines since the step started
    };
    // Declared camera features + camera service list (with focal lengths).
    static const char *cameraListCommand();
    static std::vector<CameraStep> cameraSteps(const char *list_output);
    // Output of a step check (sections ##O open cameras, ##Y camera errors).
    static void parseCameraCheck(const char *check_output, std::string *opened, std::string *errors);
    // CAMERA card: one item per step (camera errors turn an OK into FAIL), then the camera list.
    static Group cameraTestGroup(const char *list_output, const std::vector<CameraStepResult> &results);
    // Every group is NotTested when the output is missing or truncated.
    static FullResult parse(const char *output);
};
