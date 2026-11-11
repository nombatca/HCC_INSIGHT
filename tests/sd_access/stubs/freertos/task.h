#pragma once
#include <cstdint>
int xTaskCreatePinnedToCore(void(*)(void*),const char*,std::uint32_t,void*,unsigned,void*,int);
void vTaskDelete(void*);

void vTaskDelay(std::uint32_t);
