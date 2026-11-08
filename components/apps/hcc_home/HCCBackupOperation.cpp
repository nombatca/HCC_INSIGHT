#include "HCCBackupOperation.hpp"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "esp_vfs_fat.h"
#include <algorithm>
namespace bb=hcc::backup;
HCCBackupError HCCBackupOperation::fail(HCCBackupError e){
    reply_.reset();upload_.reset();if(start_)start_->cancel();if(observer_)observer_->transport_failed();
    phase_=HCCBackupPhase::failed;return error_=e;
}
void HCCBackupOperation::cancel(){
    if(phase_==HCCBackupPhase::starting||phase_==HCCBackupPhase::waiting||phase_==HCCBackupPhase::receiving||phase_==HCCBackupPhase::replying){
        fail(HCCBackupError::cancelled);phase_=HCCBackupPhase::cancelled;
    }
}
HCCBackupError HCCBackupOperation::start(std::string_view udid,std::uint64_t now,HCCBackupConfig config){
    if(phase_!=HCCBackupPhase::idle)return HCCBackupError::busy;
    observer_=link_.session_observer();
    if(!observer_||observer_->phase()!=bb::Phase::ready||link_.control_frame_pending()||!scratch_||!capacity_||capacity_>bb::kObservedChunkSize||!output_||output_capacity_<4||
       !config.minimum_free_bytes||!config.idle_timeout_ms||!config.total_timeout_ms||!config.limits.max_files||config.limits.max_files>1024||
       !config.limits.max_frame_payload||!config.limits.max_file_bytes||!config.limits.max_batch_bytes)return HCCBackupError::invalid_argument;
    std::size_t encoded=0;
    if(bb::encode_backup_request(udid,config.force_full,output_+4,output_capacity_-4,encoded)!=bb::LinkError::ok)return HCCBackupError::invalid_argument;
    // No request bytes or running state until mount/space preflight succeeds.
    const auto storage=capture_.open(config.minimum_free_bytes);
    if(storage!=bb::Error::ok){phase_=HCCBackupPhase::failed;return error_=storage==bb::Error::busy?HCCBackupError::busy:storage==bb::Error::too_large?HCCBackupError::quota:HCCBackupError::storage;}
    start_.emplace(*observer_,output_,output_capacity_);
    if(start_->begin(udid,config.force_full)!=bb::Error::ok){phase_=HCCBackupPhase::failed;return error_=HCCBackupError::transfer;}
    config_=config;began_=last_now_=progress_=now;phase_=HCCBackupPhase::starting;return HCCBackupError::ok;
}
bb::IoResult HCCBackupOperation::Progress::read(void* p,std::uint8_t* b,std::size_t n){auto& s=*static_cast<Progress*>(p);auto r=s.io.read(s.io.context,b,n);if(r.bytes)s.operation->progress_=s.now;return r;}
bb::IoResult HCCBackupOperation::Progress::write(void* p,const std::uint8_t* b,std::size_t n){auto& s=*static_cast<Progress*>(p);auto r=s.io.write(s.io.context,b,n);if(r.bytes)s.operation->progress_=s.now;return r;}
HCCBackupError HCCBackupOperation::poll(bb::Duplex io,std::uint64_t now,std::size_t budget){
    if(phase_==HCCBackupPhase::succeeded)return HCCBackupError::ok;
    if(phase_==HCCBackupPhase::failed||phase_==HCCBackupPhase::cancelled)return error_;
    if(phase_==HCCBackupPhase::idle||!io.read||!io.write||!budget)return HCCBackupError::invalid_argument;
    if(now<last_now_)return fail(HCCBackupError::clock);
    last_now_=now;
    if(now-began_>=config_.total_timeout_ms||now-progress_>=config_.idle_timeout_ms)return fail(HCCBackupError::timeout);
    Progress context{this,io,now};const bb::Duplex tracked{&context,Progress::read,Progress::write};
    if(phase_==HCCBackupPhase::starting){
        const auto e=start_->pump({tracked.context,tracked.write});
        if(e!=bb::Error::ok&&e!=bb::Error::would_block)return fail(HCCBackupError::transfer);
        if(start_->phase()==bb::BackupStartPhase::sent)phase_=HCCBackupPhase::waiting;
        return HCCBackupError::would_block;
    }
    if(phase_==HCCBackupPhase::replying){
        const auto result=reply_->pump({tracked.context,tracked.write});
        if(result!=bb::Error::ok&&result!=bb::Error::would_block)return fail(HCCBackupError::transfer);
        if(reply_->done()){
            if(observer_->complete_request(true)!=bb::SessionError::ok)return fail(HCCBackupError::transfer);
            reply_.reset();phase_=HCCBackupPhase::waiting;
        }
        return HCCBackupError::would_block;
    }
    if(phase_==HCCBackupPhase::receiving){
        const auto e=upload_->pump(tracked,budget);
        if(e==bb::Error::would_block)return HCCBackupError::would_block;
        if(e!=bb::Error::ok)return fail(e==bb::Error::too_large?HCCBackupError::quota:HCCBackupError::transfer);
        files_+=upload_->completed_files();bytes_+=upload_->completed_payload_bytes();upload_.reset();
        phase_=HCCBackupPhase::waiting;return HCCBackupError::would_block;
    }
    hcc::plist::Document doc;bb::SessionEvent event;bool available=false;
    const auto e=link_.receive_event(tracked,now,doc,event,available,budget);
    if(e==bb::LinkError::would_block||e==bb::LinkError::yield)return HCCBackupError::would_block;
    if(e!=bb::LinkError::ok)return fail(e==bb::LinkError::timeout?HCCBackupError::timeout:
        e==bb::LinkError::unsupported_operation?HCCBackupError::unsupported:
        e==bb::LinkError::too_large?HCCBackupError::quota:HCCBackupError::transfer);
    if(!available)return HCCBackupError::would_block;
    if(event.kind==bb::EventKind::create_directory){
        reply_.emplace(output_,output_capacity_);
        // Validate response storage before mutating the capture namespace.
        if(reply_->begin(0,nullptr,false)!=bb::Error::ok)return fail(HCCBackupError::transfer);
        const auto result=capture_.createDirectory(doc.string(event.payload));
        if(result!=bb::Error::ok)return fail(result==bb::Error::too_large?HCCBackupError::quota:HCCBackupError::storage);
        phase_=HCCBackupPhase::replying;return HCCBackupError::would_block;
    }
    if(event.kind==bb::EventKind::free_disk_space){
        std::uint64_t total=0,available_bytes=0;
        if(esp_vfs_fat_info(BSP_SD_MOUNT_POINT,&total,&available_bytes)!=ESP_OK||available_bytes>total)
            return fail(HCCBackupError::storage);
        // Advertise the effective capture budget, capped by the remaining quota.
        const auto remaining=config_.limits.max_batch_bytes-bytes_;
        reply_.emplace(output_,output_capacity_);
        if(reply_->begin_free_space(std::min(available_bytes,remaining))!=bb::Error::ok)return fail(HCCBackupError::transfer);
        phase_=HCCBackupPhase::replying;return HCCBackupError::would_block;
    }
    if(event.kind==bb::EventKind::contents_of_directory){
        // Caller must provision listing storage; otherwise directory listing is refused.
        if(!contents_.entries||!contents_.entry_capacity||
           contents_.offset_capacity<bb::contents_object_count(contents_.entry_capacity))
            return fail(HCCBackupError::unsupported);
        bb::DirectoryListing listing{contents_.entries,contents_.entry_capacity,0};
        // Too many children or any storage/metadata error fails the operation; the
        // listing is never silently truncated or replaced with an empty reply.
        const auto result=capture_.listDirectory(doc.string(event.payload),listing);
        if(result!=bb::Error::ok)return fail(result==bb::Error::too_large?HCCBackupError::quota:HCCBackupError::storage);
        reply_.emplace(output_,output_capacity_);
        if(reply_->begin_contents(listing,contents_.offsets,contents_.offset_capacity)!=bb::Error::ok)
            return fail(HCCBackupError::transfer);
        phase_=HCCBackupPhase::replying;return HCCBackupError::would_block;
    }
    if(event.kind==bb::EventKind::upload_files){
        if(files_>=config_.limits.max_files||bytes_>=config_.limits.max_batch_bytes)return fail(HCCBackupError::quota);
        auto limits=config_.limits;limits.max_files-=files_;limits.max_batch_bytes-=bytes_;
        upload_.emplace(*observer_,scratch_,capacity_,output_,output_capacity_);
        if(upload_->start(capture_.target(),limits)!=bb::Error::ok)return fail(HCCBackupError::transfer);
        phase_=HCCBackupPhase::receiving;return HCCBackupError::would_block;
    }
    if(event.kind==bb::EventKind::result){
        remote_code_=event.result_code;
        if(remote_code_)return fail(HCCBackupError::remote);
        phase_=HCCBackupPhase::succeeded;return HCCBackupError::ok;
    }
    if(event.kind==bb::EventKind::disconnect)return fail(HCCBackupError::disconnected);
    if(event.kind==bb::EventKind::process_message)return HCCBackupError::would_block;
    return fail(HCCBackupError::unsupported);
}
