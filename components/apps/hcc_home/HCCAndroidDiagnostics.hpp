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
    // Starts the phone's camera app on the back then the front camera and checks each opened
    // (about 15 s; the phone screen must be unlocked). parseCameraTest() returns the CAMERA group.
    static const char *cameraTestCommand();
    static Group parseCameraTest(const char *output);
    // Every group is NotTested when the output is missing or truncated.
    static FullResult parse(const char *output);
};
