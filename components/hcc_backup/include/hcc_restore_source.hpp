#pragma once
#include "hcc_backup_batch.hpp"
#include "hcc_restore_sample_index.hpp"
#include <cstdio>
namespace hcc::backup {
std::uint32_t restore_crc32(std::uint32_t,const std::uint8_t*,std::size_t);
// Caller holds exclusive mount ownership. Only the indexed sample is exposed.
class RestoreFileSource {
public:
    ~RestoreFileSource(){reset();}
    RestoreFileSource()=default;
    RestoreFileSource(const RestoreFileSource&)=delete;
    RestoreFileSource& operator=(const RestoreFileSource&)=delete;
    void reset();
    Error bind(const char*,const RestoreSampleEntry&);
    Source source(){return {this,read};}
private:
    static SourceResult read(void*,std::uint8_t*,std::size_t);
    char path_[352]{};FILE* file_=nullptr;
    std::uint64_t remaining_=0;std::uint32_t expected_=0,crc_=0;
    bool done_=false,failed_=false;
};
class RestoreSampleSource {
public:
    explicit RestoreSampleSource(const RestoreSampleEntry* entries=kRestoreSample,std::size_t count=kRestoreSampleCount):entries_(entries),count_(count){}
    RestoreSampleSource(const RestoreSampleSource&)=delete;
    RestoreSampleSource& operator=(const RestoreSampleSource&)=delete;
    ~RestoreSampleSource(){if(check_)std::fclose(check_);}
    Error begin(const char* trusted_root,std::string_view udid);
    // One bounded read per call. Size+ZIP CRC32 preflight must finish before
    // Restore. CRC32 detects accidental corruption, not malicious tampering.
    Error preflight(std::uint8_t*,std::size_t);
    Error lookup(std::string_view,RestoreFileSource&,bool& found) const;
    Error list(std::string_view,DirectoryListing&) const;
    bool verified() const {return verified_;}
    std::size_t checked() const {return checked_;}
private:
    const RestoreSampleEntry* entries_;std::size_t count_;
    bool relative(std::string_view,std::string_view&) const;
    const RestoreSampleEntry* entry(std::string_view) const;
    char root_[256]{},udid_[41]{};
    FILE* check_=nullptr;std::size_t checked_=0;
    std::uint64_t checked_bytes_=0;std::uint32_t crc_=0;
    bool begun_=false,verified_=false;
};
}
