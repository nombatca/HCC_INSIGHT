#include "HCCSpmiLink.hpp"

namespace HCCSpmiLink {

namespace {

OpenFn s_fn[TARGET_COUNT];
void *s_user[TARGET_COUNT];

}  // namespace

void registerTarget(Target t, OpenFn fn, void *user)
{
    if (t < TARGET_COUNT) {
        s_fn[t] = fn;
        s_user[t] = user;
    }
}

bool available(Target t)
{
    return t < TARGET_COUNT && s_fn[t] != nullptr;
}

bool open(Target t, const Ref &ref)
{
    if (!available(t)) {
        return false;
    }
    s_fn[t](ref, s_user[t]);
    return true;
}

const char *targetName(Target t)
{
    return t == BASEBAND ? "BASEBAND" : t == BOARDVIEW ? "BOARDVIEW" : "?";
}

}  // namespace HCCSpmiLink
