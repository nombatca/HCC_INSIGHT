#include "HCCBackupOperation.hpp"
#include "bsp/esp32_p4_wifi6_touch_lcd_7b.h"
#include "esp_vfs_fat.h"
#include <algorithm>
#include <cstring>
namespace bb=hcc::backup;
HCCBackupError HCCBackupOperation::fail(HCCBackupError e){
#ifdef ESP_PLATFORM
    printf("[HCC BACKUP] stop phase=%d reason=%d\n",static_cast<int>(phase_),static_cast<int>(e));
#endif
    move_doc_={};move_key_=hcc::plist::none;
    download_.reset();reply_.reset();upload_.reset();if(start_)start_->cancel();if(observer_)observer_->transport_failed();
    phase_=HCCBackupPhase::failed;return error_=e;
}
void HCCBackupOperation::cancel(){
    if(phase_==HCCBackupPhase::starting||phase_==HCCBackupPhase::waiting||phase_==HCCBackupPhase::receiving||phase_==HCCBackupPhase::replying||phase_==HCCBackupPhase::downloading||phase_==HCCBackupPhase::moving){
        fail(HCCBackupError::cancelled);phase_=HCCBackupPhase::cancelled;
    }
}
HCCBackupError HCCBackupOperation::start(std::string_view udid,std::uint64_t now,HCCBackupConfig config){
    if(phase_!=HCCBackupPhase::idle)return HCCBackupError::busy;
    observer_=link_.session_observer();
    if(!observer_||observer_->phase()!=bb::Phase::ready||link_.control_frame_pending()||!scratch_||!capacity_||capacity_>bb::kObservedChunkSize||!output_||output_capacity_<4||
       (config.use_sd_capacity&&config.reserve_bytes==UINT64_MAX)||!config.minimum_free_bytes||!config.idle_timeout_ms||!config.total_timeout_ms||!config.limits.max_files||config.limits.max_files>1024||
       !config.limits.max_frame_payload||!config.limits.max_file_bytes||!config.limits.max_batch_bytes)return HCCBackupError::invalid_argument;
    std::size_t encoded=0;
    if(bb::encode_backup_request(udid,config.force_full,output_+4,output_capacity_-4,encoded)!=bb::LinkError::ok)return HCCBackupError::invalid_argument;
    // No request bytes or running state until mount/space preflight succeeds.
    const auto storage=capture_.open(config.use_sd_capacity?std::max(config.minimum_free_bytes,config.reserve_bytes+1):config.minimum_free_bytes);
    if(storage!=bb::Error::ok){phase_=HCCBackupPhase::failed;return error_=storage==bb::Error::busy?HCCBackupError::busy:storage==bb::Error::too_large?HCCBackupError::quota:HCCBackupError::storage;}
    if(config.use_sd_capacity){
        std::uint64_t total=0,available=0;
        if(esp_vfs_fat_info(BSP_SD_MOUNT_POINT,&total,&available)!=ESP_OK||available>total){
            phase_=HCCBackupPhase::failed;return error_=HCCBackupError::storage;
        }
        if(available<=config.reserve_bytes){phase_=HCCBackupPhase::failed;return error_=HCCBackupError::quota;}
        config.limits.max_batch_bytes=available-config.reserve_bytes;
#ifdef ESP_PLATFORM
        printf("[HCC BACKUP] space free=%llu reserve=%llu payload_budget=%llu\n",
               static_cast<unsigned long long>(available),static_cast<unsigned long long>(config.reserve_bytes),
               static_cast<unsigned long long>(config.limits.max_batch_bytes));
#endif
    }
    start_.emplace(*observer_,output_,output_capacity_);
    if(start_->begin(udid,config.force_full)!=bb::Error::ok){phase_=HCCBackupPhase::failed;return error_=HCCBackupError::transfer;}
    config_=config;began_=last_now_=progress_=now;phase_=HCCBackupPhase::starting;return HCCBackupError::ok;
}
bb::IoResult HCCBackupOperation::Progress::read(void* p,std::uint8_t* b,std::size_t n){auto& s=*static_cast<Progress*>(p);auto r=s.io.read(s.io.context,b,n);if(r.bytes)s.operation->progress_=s.now;return r;}
bb::IoResult HCCBackupOperation::Progress::write(void* p,const std::uint8_t* b,std::size_t n){auto& s=*static_cast<Progress*>(p);auto r=s.io.write(s.io.context,b,n);if(r.bytes)s.operation->progress_=s.now;return r;}
HCCBackupError HCCBackupOperation::checkDeadline(std::uint64_t now){
    if(phase_==HCCBackupPhase::succeeded)return HCCBackupError::ok;
    if(phase_==HCCBackupPhase::failed||phase_==HCCBackupPhase::cancelled)return error_;
    if(phase_==HCCBackupPhase::idle)return HCCBackupError::invalid_argument;
    if(now<last_now_)return fail(HCCBackupError::clock);
    last_now_=now;
    if(now-began_>=config_.total_timeout_ms||now-progress_>=config_.idle_timeout_ms){
#ifdef ESP_PLATFORM
        printf("[HCC BACKUP] timeout kind=%s elapsed_ms=%llu idle_ms=%llu total_limit_ms=%llu idle_limit_ms=%llu\n",
               now-began_>=config_.total_timeout_ms?"total":"idle",
               static_cast<unsigned long long>(now-began_),static_cast<unsigned long long>(now-progress_),
               static_cast<unsigned long long>(config_.total_timeout_ms),static_cast<unsigned long long>(config_.idle_timeout_ms));
#endif
        return fail(HCCBackupError::timeout);
    }
    return HCCBackupError::would_block;
}
HCCBackupError HCCBackupOperation::poll(bb::Duplex io,std::uint64_t now,std::size_t budget){
    if(phase_==HCCBackupPhase::succeeded)return HCCBackupError::ok;
    if(phase_==HCCBackupPhase::failed||phase_==HCCBackupPhase::cancelled)return error_;
    if(phase_==HCCBackupPhase::idle||!io.read||!io.write||!budget)return HCCBackupError::invalid_argument;
    const auto deadline=checkDeadline(now);
    if(deadline!=HCCBackupError::would_block)return deadline;
    Progress context{this,io,now};const bb::Duplex tracked{&context,Progress::read,Progress::write};
    if(phase_==HCCBackupPhase::moving){
        bb::Error moved=bb::Error::ok;
        if(move_key_!=hcc::plist::none){
            const auto value=move_doc_.get(move_key_)->next;
            moved=capture_.moveFile(move_doc_.string(move_key_),move_doc_.string(value),*contents_.scratch);
            if(moved==bb::Error::would_block){progress_=now;return HCCBackupError::would_block;}
            if(moved==bb::Error::ok){++moves_done_;move_key_=move_doc_.get(value)->next;progress_=now;}
        }
#ifdef ESP_PLATFORM
        if(moved!=bb::Error::ok||move_key_==hcc::plist::none||moves_done_%8==0)
            printf("[HCC BACKUP] MoveItems progress=%u result=%s\n",moves_done_,bb::error_name(moved));
#endif
        if(moved!=bb::Error::ok||move_key_==hcc::plist::none){
            if(moved==bb::Error::ok){reply_.emplace(output_,output_capacity_);if(reply_->begin(0,nullptr,true)!=bb::Error::ok)return fail(HCCBackupError::transfer);}
            move_doc_={};phase_=HCCBackupPhase::replying;
        }
        return HCCBackupError::would_block;
    }
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
    if(phase_==HCCBackupPhase::downloading){
        const auto e=download_->pump({tracked.context,tracked.write},budget);
        if(e!=bb::Error::ok&&e!=bb::Error::would_block)return fail(HCCBackupError::transfer);
        if(download_->done()){
#ifdef ESP_PLATFORM
            printf("[HCC BACKUP] DownloadFiles reply complete attempted=%u; waiting for next control\n",static_cast<unsigned>(download_->completed_files()));
#endif
            if(observer_->complete_request(true)!=bb::SessionError::ok)return fail(HCCBackupError::transfer);
            download_.reset();phase_=HCCBackupPhase::waiting;
        }
        return HCCBackupError::would_block;
    }
    hcc::plist::Document doc;bb::SessionEvent event;bool available=false;
    const auto e=link_.receive_event(tracked,now,doc,event,available,budget);
    if(e==bb::LinkError::would_block||e==bb::LinkError::yield)return HCCBackupError::would_block;
    if(e!=bb::LinkError::ok){
#ifdef ESP_PLATFORM
        const auto rejected=link_.rejected_message();
        printf("[HCC BACKUP] control receive failed error=%s command=%.*s files=%u bytes=%llu\n",
               bb::link_error_name(e),static_cast<int>(rejected.size()),rejected.data(),
               static_cast<unsigned>(files_),static_cast<unsigned long long>(bytes_));
#endif
        return fail(e==bb::LinkError::timeout?HCCBackupError::timeout:
            e==bb::LinkError::unsupported_operation?HCCBackupError::unsupported:
            e==bb::LinkError::too_large?HCCBackupError::quota:HCCBackupError::transfer);
    }
    if(!available)return HCCBackupError::would_block;
#ifdef ESP_PLATFORM
    // Diagnostic: show every control message the device sends during a backup so
    // unhandled requests (e.g. DownloadFiles) are visible instead of a silent stall.
    printf("[HCC BACKUP] event kind=%d msg=%.*s\n",static_cast<int>(event.kind),
           static_cast<int>(event.message.size()),event.message.data());
    if(event.kind==bb::EventKind::process_message||event.kind==bb::EventKind::result){
        char details[448];
        bb::format_process_message_diagnostic(doc,event.payload,details,sizeof(details));
        printf("[HCC BACKUP] process %s\n",details);
    }
#endif
    if(event.kind==bb::EventKind::create_directory){
        reply_.emplace(output_,output_capacity_);
        // Validate response storage before mutating the capture namespace.
        if(reply_->begin(0,nullptr,false)!=bb::Error::ok)return fail(HCCBackupError::transfer);
        const auto result=capture_.createDirectory(doc.string(event.payload));
        if(result!=bb::Error::ok)return fail(result==bb::Error::too_large?HCCBackupError::quota:HCCBackupError::storage);
        phase_=HCCBackupPhase::replying;return HCCBackupError::would_block;
    }
    if(event.kind==bb::EventKind::move_files){
        if(!contents_.scratch)return fail(HCCBackupError::unsupported);
        reply_.emplace(output_,output_capacity_);
        // Preflight the larger error envelope before changing any metadata.
        if(reply_->begin(-1,"Move unavailable in capture",true)!=bb::Error::ok)return fail(HCCBackupError::transfer);
        move_doc_=doc;move_key_=doc.get(event.payload)->first;moves_done_=0;
        phase_=HCCBackupPhase::moving;return HCCBackupError::would_block;
    }
    if(event.kind==bb::EventKind::purge_disk_space){
        // Match idevicebackup2: refuse host purging with an explicit status,
        // then allow the peer to decide whether to continue or return an error.
        // No capture records or earlier sessions are removed.
        reply_.emplace(output_,output_capacity_);
        if(reply_->begin(-1,"Operation not supported",true)!=bb::Error::ok)
            return fail(HCCBackupError::transfer);
        phase_=HCCBackupPhase::replying;return HCCBackupError::would_block;
    }
    if(event.kind==bb::EventKind::free_disk_space){
        std::uint64_t total=0,available_bytes=0;
        if(esp_vfs_fat_info(BSP_SD_MOUNT_POINT,&total,&available_bytes)!=ESP_OK||available_bytes>total)
            return fail(HCCBackupError::storage);
        // Advertise the effective capture budget, capped by the remaining quota.
        const auto remaining=config_.limits.max_batch_bytes-bytes_;
        reply_.emplace(output_,output_capacity_);
        const auto usable=config_.use_sd_capacity?(available_bytes>config_.reserve_bytes?available_bytes-config_.reserve_bytes:0):available_bytes;
#ifdef ESP_PLATFORM
        printf("[HCC BACKUP] free-space actual=%llu advertised=%llu remaining_budget=%llu\n",
               static_cast<unsigned long long>(available_bytes),static_cast<unsigned long long>(std::min(usable,remaining)),
               static_cast<unsigned long long>(remaining));
#endif
        if(reply_->begin_free_space(std::min(usable,remaining))!=bb::Error::ok)return fail(HCCBackupError::transfer);
        phase_=HCCBackupPhase::replying;return HCCBackupError::would_block;
    }
    if(event.kind==bb::EventKind::contents_of_directory){
        // Caller must provision listing storage; otherwise directory listing is refused.
        if(!contents_.entries||!contents_.entry_capacity||!contents_.scratch||
           contents_.offset_capacity<bb::contents_object_count(contents_.entry_capacity))
            return fail(HCCBackupError::unsupported);
        bb::DirectoryListing listing{contents_.entries,contents_.entry_capacity,0};
        // Too many children or any storage/metadata error fails the operation; the
        // listing is never silently truncated or replaced with an empty reply.
        const auto result=capture_.listDirectory(doc.string(event.payload),listing,*contents_.scratch);
        if(result!=bb::Error::ok)return fail(result==bb::Error::too_large?HCCBackupError::quota:HCCBackupError::storage);
        reply_.emplace(output_,output_capacity_);
        if(reply_->begin_contents(listing,contents_.offsets,contents_.offset_capacity)!=bb::Error::ok)
            return fail(HCCBackupError::transfer);
        phase_=HCCBackupPhase::replying;return HCCBackupError::would_block;
    }
    if(event.kind==bb::EventKind::download_files){
        // Fresh-backup response: every requested file is reported absent so the
        // device proceeds to send a full backup. Serving existing capture files
        // (incremental backup) is future work. Paths are copied because the plist
        // document is reused by the next receive.
        const auto files_node=event.payload;const auto* arr=doc.get(files_node);
        if(arr==nullptr)return fail(HCCBackupError::transfer);
        // Only the initial empty capture can truthfully report every file absent.
        // Existing-record download lookup is not implemented; never invent ENOENT.
        if(arr->count&&capture_.committedFiles()!=0)return fail(HCCBackupError::unsupported);
        std::size_t n=0,used=0;
        for(std::size_t i=0;i<arr->count;++i){
            const auto path=doc.string(doc.at(files_node,i));
            if(n>=kMaxDownloadFiles||used+path.size()>sizeof(download_arena_))return fail(HCCBackupError::quota);
            std::memcpy(download_arena_+used,path.data(),path.size());
            download_entries_[n]={std::string_view(download_arena_+used,path.size()),{nullptr,nullptr}};
            used+=path.size();++n;
        }
        download_.emplace(output_,output_capacity_,scratch_,capacity_);
        if(download_->start(download_entries_,n)!=bb::Error::ok)return fail(HCCBackupError::transfer);
        phase_=HCCBackupPhase::downloading;return HCCBackupError::would_block;
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
