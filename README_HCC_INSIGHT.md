# HCC INSIGHT - Phase 1

This patch adds the first HCC INSIGHT dashboard as a normal ESP-Brookesia PhoneApp.

It intentionally does not modify the Waveshare BSP, display/touch initialization, shared I2C bus, Settings app, or managed components.

Files:
- `components/apps/hcc_home/HCCHome.cpp`
- `components/apps/hcc_home/HCCHome.hpp`
- `components/apps/apps.h` (add HCCHome include)
- `main/main.cpp` (install HCCHome)

The dashboard is only the foundation. Module actions are not wired yet; this keeps the current working firmware functions unchanged while we build HCC modules one at a time.
