#include "hcc_backup_batch.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <cstring>
#include <iostream>
#include <vector>
namespace bb=hcc::backup;
struct Source {
    std::vector<std::uint8_t> data;std::size_t offset=0;bool fail=false;
    static bb::SourceResult read(void* ctx,std::uint8_t* out,std::size_t cap) {
        auto& s=*static_cast<Source*>(ctx);
        if(s.fail)return {0,false,bb::Error::io_error};
        const auto n=std::min(cap,s.data.size()-s.offset);
        if(n) std::memcpy(out,s.data.data()+s.offset,n);
        s.offset+=n;return {n,s.offset==s.data.size(),bb::Error::ok};
    }
    bb::Source io(){return {this,read};}
};
struct Wire {
    std::vector<std::uint8_t> data;std::size_t calls=0;
    const std::uint8_t* retry=nullptr;std::size_t length=0;
    static bb::IoResult write(void* ctx,const std::uint8_t* p,std::size_t n) {
        auto& w=*static_cast<Wire*>(ctx);++w.calls;
        if(w.retry) {assert(p==w.retry&&n==w.length);w.retry=nullptr;}
        else {w.retry=p;w.length=n;return {0,bb::IoState::would_block};}
        const auto take=std::min(n,std::size_t(7));
        w.data.insert(w.data.end(),p,p+take);return {take,bb::IoState::ok};
    }
};
int main(){
    std::array<std::uint8_t,4100> names{};std::array<std::uint8_t,19> scratch{};
    Source a{{1,2,3,4}},b{{}},c{std::vector<std::uint8_t>(100,42)};
    bb::DownloadFile files[]={{"backup/a",a.io()},{"backup/empty",b.io()},{"backup/c",c.io()}};
    bb::DownloadBatchSender batch(names.data(),names.size(),scratch.data(),scratch.size());Wire wire;
    assert(batch.start(files,3)==bb::Error::ok);
    assert(batch.start(files,3)==bb::Error::busy);
    for(unsigned i=0;i<10000&&!batch.done();++i){
        const auto before=wire.calls;const auto e=batch.pump({&wire,Wire::write},2);
        assert(e==bb::Error::ok||e==bb::Error::would_block);assert(wire.calls-before<=2);
    }
    assert(batch.done()&&batch.completed_files()==3);
    std::size_t pos=0;
    for(unsigned i=0;i<3;++i){
        const auto len=bb::read_be32(wire.data.data()+pos);pos+=4;
        assert(len==files[i].path.size());
        assert(std::memcmp(wire.data.data()+pos,files[i].path.data(),len)==0);pos+=len;
        std::vector<std::uint8_t> actual;
        for(;;){const auto n=bb::read_be32(wire.data.data()+pos);pos+=4;
            assert(n>=1);const auto tag=wire.data[pos++];
            if(tag==0){assert(n==1);break;}
            assert(tag==0x0c);actual.insert(actual.end(),wire.data.begin()+pos,wire.data.begin()+pos+n-1);pos+=n-1;
        }
        assert(actual==(i==0?a.data:i==1?b.data:c.data));
    }
    assert(bb::read_be32(wire.data.data()+pos)==0);pos+=4;
    const auto status_size=bb::read_be32(wire.data.data()+pos);pos+=4;
    assert(status_size==wire.data.size()-pos);
    hcc::plist::Node nodes[32];char text[512];hcc::plist::Document doc;
    assert(hcc::plist::parse(wire.data.data()+pos,status_size,{nodes,32,text,512},doc)==hcc::plist::Error::ok);
    assert(doc.string(doc.at(doc.root,0))=="DLMessageStatusResponse");
    assert(doc.get(doc.at(doc.root,1))->integer==0);
    // Invalid final path must reject the entire list before any source is read.
    bb::DownloadBatchSender invalid(names.data(),names.size(),scratch.data(),scratch.size());
    files[2].path="../escape";a.offset=0;
    assert(invalid.start(files,3)==bb::Error::invalid_argument&&a.offset==0);
    Source broken{{},0,true};bb::DownloadFile bad{"backup/bad",broken.io()};
    bb::DownloadBatchSender failed(names.data(),names.size(),scratch.data(),scratch.size());Wire failure;
    assert(failed.start(&bad,1)==bb::Error::ok);
    bb::Error e=bb::Error::ok;
    for(unsigned i=0;i<1000;++i){e=failed.pump({&failure,Wire::write});if(e!=bb::Error::would_block)break;}
    assert(e==bb::Error::io_error&&!failed.done()&&failed.completed_files()==0);
    assert(failure.data.size()==4+bad.path.size()); // No file-end, batch-end or status success.
    assert(failed.pump({&failure,Wire::write})==bb::Error::io_error);
    bb::DownloadBatchSender empty(names.data(),names.size(),scratch.data(),scratch.size());Wire ew;
    assert(empty.start(nullptr,0)==bb::Error::ok);
    for(unsigned i=0;i<1000&&!empty.done();++i)empty.pump({&ew,Wire::write},1);
    assert(empty.done()&&bb::read_be32(ew.data.data())==0);
    bb::DownloadBatchSender closed(names.data(),names.size(),scratch.data(),scratch.size());
    assert(closed.start(nullptr,0)==bb::Error::ok);
    auto close=[](void*,const std::uint8_t*,std::size_t n)->bb::IoResult{return {n,bb::IoState::closed};};
    assert(closed.pump({nullptr,close})==bb::Error::closed&&!closed.done());
    bb::DownloadBatchSender over(names.data(),names.size(),scratch.data(),scratch.size());
    assert(over.start(nullptr,0)==bb::Error::ok);
    assert(over.pump({nullptr,close},0)==bb::Error::invalid_argument);
    auto oversized=[](void*,const std::uint8_t*,std::size_t n)->bb::IoResult{return {n+1,bb::IoState::ok};};
    assert(over.pump({nullptr,oversized})==bb::Error::io_error&&!over.done());
    // Fresh-backup response: every requested file is absent (null source) -> each
    // gets a CODE_ERROR_LOCAL frame and the final status is a DeviceLink multi-status.
    bb::DownloadFile absent[]={{"Status.plist",{}},{"Manifest.db",{}}};
    bb::DownloadBatchSender nf(names.data(),names.size(),scratch.data(),scratch.size());Wire nw;
    assert(nf.start(absent,2)==bb::Error::ok);
    for(unsigned i=0;i<10000&&!nf.done();++i){const auto e=nf.pump({&nw,Wire::write},2);assert(e==bb::Error::ok||e==bb::Error::would_block);}
    assert(nf.done()&&nf.completed_files()==2);
    std::size_t np=0;
    for(unsigned i=0;i<2;++i){
        const auto plen=bb::read_be32(nw.data.data()+np);np+=4;
        assert(plen==absent[i].path.size()&&std::memcmp(nw.data.data()+np,absent[i].path.data(),plen)==0);np+=plen;
        const auto elen=bb::read_be32(nw.data.data()+np);np+=4;
        assert(elen>=1&&nw.data[np]==0x06);np+=elen; // CODE_ERROR_LOCAL + reason bytes
    }
    assert(bb::read_be32(nw.data.data()+np)==0);np+=4; // batch terminator
    const auto nslen=bb::read_be32(nw.data.data()+np);np+=4;
    hcc::plist::Node nns[32];char ntx[512];hcc::plist::Document nd;
    assert(hcc::plist::parse(nw.data.data()+np,nslen,{nns,32,ntx,512},nd)==hcc::plist::Error::ok);
    assert(nd.string(nd.at(nd.root,0))=="DLMessageStatusResponse"&&nd.get(nd.at(nd.root,1))->integer==-13);
    std::cout<<"Download batch: ordered files, empty file/list, budgets, retries, invalid paths, source failure and absent-file multi-status PASS\n";
}
