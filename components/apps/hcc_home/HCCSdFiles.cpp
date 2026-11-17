#include "HCCSdFiles.hpp"
#include "HCCSdAccess.hpp"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include <atomic>
#include <cstdio>
#include <cstring>
namespace HCCSdFiles {
namespace {
std::atomic<State> current{State::idle};
std::atomic<bool> stopped{false};
std::atomic<std::uint32_t> removed_count{0};
Result output;
void clearResult(){
    output.page.count=0;output.page.more=false;output.message[0]=0;
    output.total=0;output.free=0;output.space_valid=false;
    output.deletion=false;output.success=false;output.removed=0;
}
char requested[HCCSdFileSystem::path_size]{};
std::size_t page_offset=0;
bool deleting=false,expected_directory=false;
bool isCancelled(void*){return stopped.load();}
void onProgress(void*,std::uint32_t count){removed_count.store(count);if(count%16==0)vTaskDelay(1);}
void worker(void*){
    using namespace HCCSdFileSystem;
    clearResult();output.deletion=deleting;
    hcc::backup::SdLease lease(HCCSdAccess::gate,hcc::backup::SdOwner::file_manager);
    if(!lease.acquire()){
        std::snprintf(output.message,sizeof(output.message),"Thẻ đang bận: tắt USB Disk hoặc chờ backup kết thúc.");
    }else{
        bool mounted=bsp_sdcard!=nullptr;
        if(!mounted){
#ifndef CONFIG_BSP_SD_FORMAT_ON_MOUNT_FAIL
            const auto e=bsp_sdcard_mount();mounted=(e==ESP_OK||e==ESP_ERR_INVALID_STATE)&&bsp_sdcard!=nullptr;
#endif
        }
        if(!mounted){
            std::snprintf(output.message,sizeof(output.message),"Không gắn được thẻ SD. Không định dạng thẻ.");
        }else{
            const auto error=deleting?remove(BSP_SD_MOUNT_POINT,requested,expected_directory,
                                            {nullptr,isCancelled,onProgress},output.removed)
                                     :list(BSP_SD_MOUNT_POINT,requested,page_offset,output.page,{nullptr,isCancelled,nullptr});
            output.success=error==Error::ok;
            if(deleting){
                const char* detail=error==Error::ok?"Hoàn tất":error==Error::cancelled?"Đã dừng; các mục đã xóa không được khôi phục":"Dừng vì lỗi SD, đường dẫn hoặc mục không được hỗ trợ";
                std::snprintf(output.message,sizeof(output.message),"%s. Đã xóa %u mục.",detail,output.removed);
            }else if(error!=Error::ok){
                std::snprintf(output.message,sizeof(output.message),"%s",error==Error::cancelled?"Đã dừng đọc thư mục.":"Không đọc được thư mục. Kiểm tra thẻ SD và đường dẫn.");
            }
            output.space_valid=esp_vfs_fat_info(BSP_SD_MOUNT_POINT,&output.total,&output.free)==ESP_OK&&output.free<=output.total;
        }
        lease.release(); // Publish completion only after SD is available again.
    }
    current.store(State::done);vTaskDelete(nullptr);
}
bool request(const char* path,bool remove,bool directory,std::size_t offset){
    if(!path||std::strlen(path)>=sizeof(requested))return false;
    auto previous=current.load();
    if(previous==State::busy||!current.compare_exchange_strong(previous,State::busy))return false;
    std::strcpy(requested,path);deleting=remove;expected_directory=directory;page_offset=offset;
    removed_count.store(0);stopped.store(false);
    if(xTaskCreatePinnedToCore(worker,"hcc_sd_files",8192,nullptr,3,nullptr,0)!=pdPASS){
        clearResult();std::snprintf(output.message,sizeof(output.message),"Không tạo được tác vụ quản lý thẻ.");
        current.store(State::done);
    }
    return true;
}
}
State state(){return current.load();}
bool busy(){return state()==State::busy;}
std::uint32_t progress(){return removed_count.load();}
void cancel(){stopped.store(true);}
const Result& result(){return output;}
bool requestList(const char* path,std::size_t offset){return request(path,false,false,offset);}
bool requestDelete(const char* path,bool directory){return path&&path[0]&&request(path,true,directory,0);}
}
