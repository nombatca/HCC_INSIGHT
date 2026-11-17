#pragma once
#include "hcc_sd_access.hpp"
namespace HCCSdAccess {
// Shared between new backup capture and USB DISK. Legacy SD users do not yet
// participate, so this is not a project-wide filesystem lock.
inline hcc::backup::SdAccess gate;
}
