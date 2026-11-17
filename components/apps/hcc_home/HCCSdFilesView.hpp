#pragma once
#include "lvgl.h"
namespace HCCSdFilesView {
// UI owns only LVGL objects. Worker results never hold screen/app pointers.
void show(lv_obj_t* parent,lv_event_cb_t back,void* user);
}
