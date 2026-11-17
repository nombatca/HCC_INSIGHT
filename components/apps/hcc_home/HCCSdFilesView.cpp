#include "HCCSdFilesView.hpp"
#include "HCCSdFiles.hpp"
#include "vn_fonts.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
namespace HCCSdFilesView {
namespace {
struct View {
    lv_obj_t *root=nullptr,*info=nullptr,*path_label=nullptr,*list=nullptr,*selection=nullptr;
    lv_obj_t *up=nullptr,*refresh=nullptr,*open=nullptr,*remove=nullptr,*remove_label=nullptr,*prev=nullptr,*next=nullptr,*shortcut=nullptr,*modal=nullptr;
    lv_timer_t* timer=nullptr;
    char path[HCCSdFileSystem::path_size]{},chosen[256]{},notice[192]{};
    std::size_t offset=0;bool directory=false,supported=false,awaiting=false,was_busy=false;
} view;
void disabled(lv_obj_t* object,bool value){if(value)lv_obj_add_state(object,LV_STATE_DISABLED);else lv_obj_clear_state(object,LV_STATE_DISABLED);}
lv_obj_t* label(lv_obj_t* parent,const char* text,int x,int y,int width,const lv_font_t* font){
    auto* object=lv_label_create(parent);lv_label_set_text(object,text);lv_obj_set_pos(object,x,y);
    lv_obj_set_width(object,width);lv_label_set_long_mode(object,LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_font(object,font,0);lv_obj_set_style_text_color(object,lv_color_hex(0xCFE3EF),0);return object;
}
lv_obj_t* button(lv_obj_t* parent,const char* text,int width,lv_event_cb_t callback,void* user=nullptr){
    auto* object=lv_btn_create(parent);lv_obj_set_size(object,width,48);lv_obj_set_style_radius(object,10,0);
    lv_obj_set_style_bg_color(object,lv_color_hex(0x2478D0),0);lv_obj_add_event_cb(object,callback,LV_EVENT_CLICKED,user);
    auto* caption=lv_label_create(object);lv_label_set_text(caption,text);lv_obj_center(caption);
    lv_obj_set_style_text_font(caption,&lv_font_be_vietnam_16,0);return object;
}
void updateControls(){
    const bool busy=HCCSdFiles::busy();
    disabled(view.up,busy||!view.path[0]);disabled(view.refresh,busy);disabled(view.shortcut,busy);
    disabled(view.prev,busy||!view.offset);
    disabled(view.next,busy||!HCCSdFiles::result().page.more); // Result stable only when idle; short circuit while busy.
    disabled(view.open,busy||!view.chosen[0]||!view.directory||!view.supported);
    disabled(view.remove,!busy&&(!view.chosen[0]||!view.supported));
    lv_label_set_text(view.remove_label,busy?"DỪNG TÁC VỤ":"XÓA MỤC CHỌN");
}
void requestList(){
    view.chosen[0]=0;view.supported=false;
    lv_label_set_text(view.selection,view.notice[0]?view.notice:"Chọn một mục để mở hoặc xóa.");
    lv_label_set_text(view.path_label,view.path[0]?view.path:"/ (gốc thẻ SD)");
    lv_obj_clean(view.list);
    view.awaiting=HCCSdFiles::requestList(view.path,view.offset);
    if(!view.awaiting)lv_label_set_text(view.info,"Đang có tác vụ thẻ nhớ. Chờ hoàn tất rồi làm mới.");
    updateControls();
}
bool selectedPath(char* out,std::size_t size){
    if(!view.chosen[0])return false;
    const int n=std::snprintf(out,size,"%s%s%s",view.path,view.path[0]?"/":"",view.chosen);
    return n>0&&static_cast<std::size_t>(n)<size;
}
void onChoose(lv_event_t* event){
    if(HCCSdFiles::busy()||view.modal)return;
    const auto index=reinterpret_cast<std::uintptr_t>(lv_event_get_user_data(event))-1;
    const auto& result=HCCSdFiles::result();if(index>=result.page.count)return;
    const auto& entry=result.page.entries[index];std::strcpy(view.chosen,entry.name);
    view.directory=entry.directory;view.supported=entry.supported;
    char text[360];std::snprintf(text,sizeof(text),"%s: %s%s",entry.directory?"Thư mục":"Tệp",entry.name,entry.supported?"":" (không hỗ trợ)");
    lv_label_set_text(view.selection,text);updateControls();
}
void onOpen(lv_event_t*){
    if(HCCSdFiles::busy()||view.modal||!view.directory||!view.supported)return;
    char path[sizeof(view.path)];if(!selectedPath(path,sizeof(path))){lv_label_set_text(view.selection,"Đường dẫn quá dài.");return;}
    std::strcpy(view.path,path);view.offset=0;view.notice[0]=0;requestList();
}
void onUp(lv_event_t*){
    if(HCCSdFiles::busy()||view.modal)return;
    if(auto* slash=std::strrchr(view.path,'/'))*slash=0;else view.path[0]=0;
    view.offset=0;view.notice[0]=0;requestList();
}
void onRefresh(lv_event_t*){if(!HCCSdFiles::busy()&&!view.modal){view.notice[0]=0;requestList();}}
void onBackup(lv_event_t*){if(!HCCSdFiles::busy()&&!view.modal){std::strcpy(view.path,"hcc_backup");view.offset=0;view.notice[0]=0;requestList();}}
void onPage(lv_event_t* event){
    if(HCCSdFiles::busy()||view.modal)return;
    const bool next=lv_event_get_user_data(event)!=nullptr;
    if(next){if(!HCCSdFiles::result().page.more)return;view.offset+=HCCSdFileSystem::page_size;}
    else if(view.offset)view.offset-=HCCSdFileSystem::page_size;
    view.notice[0]=0;requestList();
}
void onCancelDialog(lv_event_t*){if(view.modal){lv_obj_del(view.modal);view.modal=nullptr;}}
void onConfirm(lv_event_t*){
    if(HCCSdFiles::busy()){onCancelDialog(nullptr);return;}
    char path[sizeof(view.path)];const bool valid=selectedPath(path,sizeof(path));onCancelDialog(nullptr);
    if(!valid){lv_label_set_text(view.selection,"Đường dẫn quá dài.");return;}
    view.awaiting=HCCSdFiles::requestDelete(path,view.directory);
    if(!view.awaiting)lv_label_set_text(view.selection,"Chưa thể xóa: thẻ nhớ đang bận.");
    updateControls();
}
void onRemove(lv_event_t*){
    if(HCCSdFiles::busy()){HCCSdFiles::cancel();return;}
    if(view.modal||!view.chosen[0]||!view.supported)return;
    view.modal=lv_obj_create(view.root);lv_obj_set_size(view.modal,LV_PCT(100),LV_PCT(100));lv_obj_center(view.modal);
    lv_obj_set_style_bg_color(view.modal,lv_color_hex(0x101820),0);lv_obj_clear_flag(view.modal,LV_OBJ_FLAG_SCROLLABLE);
    auto* box=lv_obj_create(view.modal);lv_obj_set_size(box,820,320);lv_obj_center(box);
    lv_obj_set_style_bg_color(box,lv_color_hex(0x1F3242),0);lv_obj_clear_flag(box,LV_OBJ_FLAG_SCROLLABLE);
    label(box,"XÓA VĨNH VIỄN KHỎI THẺ SD?",16,12,760,&lv_font_be_vietnam_22);
    char text[850];std::snprintf(text,sizeof(text),"%s%s%s\n%s",view.path,view.path[0]?"/":"",view.chosen,
        view.directory?"Toàn bộ tệp và thư mục con sẽ bị xóa. Không thể hoàn tác.":"Tệp sẽ bị xóa. Không thể hoàn tác.");
    label(box,text,16,60,760,&lv_font_be_vietnam_16);
    auto* cancel=button(box,"HỦY",180,onCancelDialog);lv_obj_align(cancel,LV_ALIGN_BOTTOM_LEFT,16,-8);
    auto* confirm=button(box,"XÓA VĨNH VIỄN",240,onConfirm);lv_obj_align(confirm,LV_ALIGN_BOTTOM_RIGHT,-16,-8);
    lv_obj_set_style_bg_color(confirm,lv_color_hex(0xC0392B),0);
}
void render(){
    const auto& result=HCCSdFiles::result();
    if(result.deletion){
        std::snprintf(view.notice,sizeof(view.notice),"%s",result.message);
        // Refresh from disk even after a partial delete/cancel/error.
        view.offset=0;requestList();return;
    }
    char info[256];
    if(!result.success)std::snprintf(info,sizeof(info),"%s",result.message);
    else if(result.space_valid)std::snprintf(info,sizeof(info),"Trống %.1f MB / %.1f MB  |  Trang %u  |  %u mục%s",result.free/1048576.0,result.total/1048576.0,
        static_cast<unsigned>(view.offset/HCCSdFileSystem::page_size+1),static_cast<unsigned>(result.page.count),result.page.more?" (còn tiếp)":"");
    else std::snprintf(info,sizeof(info),"%u mục. Chưa đọc được dung lượng trống.",static_cast<unsigned>(result.page.count));
    lv_label_set_text(view.info,info);lv_obj_clean(view.list);
    if(!result.page.count)label(view.list,result.success?"Thư mục trống.":"Không có danh sách tệp.",8,8,860,&lv_font_be_vietnam_18);
    int y=0;
    for(std::size_t i=0;i<result.page.count;++i){
        const auto& entry=result.page.entries[i];char text[360];
        if(entry.directory)std::snprintf(text,sizeof(text),"[THƯ MỤC]  %s",entry.name);
        else std::snprintf(text,sizeof(text),"%s  |  %.1f KB%s",entry.name,entry.bytes/1024.0,entry.supported?"":"  [không hỗ trợ]");
        auto* row=button(view.list,"",880,onChoose,reinterpret_cast<void*>(i+1));lv_obj_set_pos(row,0,y);lv_obj_set_height(row,52);
        lv_obj_set_style_bg_color(row,lv_color_hex(0x1F3242),0);
        auto* caption=lv_obj_get_child(row,0);lv_label_set_text(caption,text);lv_obj_set_width(caption,850);
        lv_label_set_long_mode(caption,LV_LABEL_LONG_DOT);lv_obj_center(caption);y+=58;
    }
    updateControls();
}
void onTimer(lv_timer_t*){
    if(!view.root)return;
    const bool busy=HCCSdFiles::busy();
    if(busy){
        char text[160];std::snprintf(text,sizeof(text),"Đang xử lý thẻ nhớ... Đã xóa %u mục. Có thể bấm DỪNG TÁC VỤ.",HCCSdFiles::progress());
        lv_label_set_text(view.info,text);
    }else if(view.awaiting&&HCCSdFiles::state()==HCCSdFiles::State::done){view.awaiting=false;render();}
    if(busy!=view.was_busy){view.was_busy=busy;updateControls();}
}
void onDeleted(lv_event_t*){if(view.timer)lv_timer_del(view.timer);view.timer=nullptr;view.root=nullptr;}
}
void show(lv_obj_t* parent,lv_event_cb_t back,void* user){
    if(view.root)lv_obj_del(view.root);
    view=View{};view.root=lv_obj_create(parent);lv_obj_set_size(view.root,LV_PCT(100),LV_PCT(100));lv_obj_center(view.root);
    lv_obj_clear_flag(view.root,LV_OBJ_FLAG_SCROLLABLE);lv_obj_set_style_pad_all(view.root,0,0);lv_obj_set_style_border_width(view.root,0,0);
    lv_obj_set_style_bg_color(view.root,lv_color_hex(0x101820),0);
    label(view.root,"QUẢN LÝ TỆP TRÊN THẺ SD",28,14,700,&lv_font_be_vietnam_28);
    view.info=label(view.root,"",28,56,950,&lv_font_be_vietnam_16);
    view.path_label=label(view.root,"/",28,86,680,&lv_font_be_vietnam_16);lv_label_set_long_mode(view.path_label,LV_LABEL_LONG_DOT);
    view.prev=button(view.root,"TRƯỚC",100,onPage);lv_obj_set_pos(view.prev,744,78);lv_obj_set_height(view.prev,34);
    view.next=button(view.root,"SAU",100,onPage,reinterpret_cast<void*>(1));lv_obj_set_pos(view.next,862,78);lv_obj_set_height(view.next,34);
    lv_obj_update_layout(parent);const int height=std::max(460,static_cast<int>(lv_obj_get_height(parent)));
    view.list=lv_obj_create(view.root);lv_obj_set_pos(view.list,28,126);lv_obj_set_size(view.list,940,height-256);
    lv_obj_set_style_bg_color(view.list,lv_color_hex(0x06243A),0);lv_obj_set_scroll_dir(view.list,LV_DIR_VER);
    view.selection=label(view.root,"",28,0,940,&lv_font_be_vietnam_16);lv_obj_align(view.selection,LV_ALIGN_BOTTOM_LEFT,28,-82);
    auto* back_button=button(view.root,"USB DISK",140,back,user);lv_obj_align(back_button,LV_ALIGN_BOTTOM_LEFT,28,-16);
    view.up=button(view.root,"LÊN MỘT CẤP",160,onUp);lv_obj_align(view.up,LV_ALIGN_BOTTOM_LEFT,180,-16);
    view.refresh=button(view.root,"LÀM MỚI",140,onRefresh);lv_obj_align(view.refresh,LV_ALIGN_BOTTOM_LEFT,352,-16);
    view.open=button(view.root,"MỞ",100,onOpen);lv_obj_align(view.open,LV_ALIGN_BOTTOM_LEFT,504,-16);
    view.shortcut=button(view.root,"BACKUP",130,onBackup);lv_obj_align(view.shortcut,LV_ALIGN_BOTTOM_LEFT,616,-16);
    view.remove=button(view.root,"XÓA MỤC CHỌN",210,onRemove);lv_obj_align(view.remove,LV_ALIGN_BOTTOM_LEFT,758,-16);
    lv_obj_set_style_bg_color(view.remove,lv_color_hex(0xC0392B),0);view.remove_label=lv_obj_get_child(view.remove,0);
    view.timer=lv_timer_create(onTimer,250,nullptr);lv_obj_add_event_cb(view.root,onDeleted,LV_EVENT_DELETE,nullptr);
    requestList();
}
}
