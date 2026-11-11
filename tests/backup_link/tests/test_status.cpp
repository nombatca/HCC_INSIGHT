#include "hcc_backup_link.hpp"
#include <algorithm>
#include <array>
#include <cassert>
#include <fstream>
#include <iostream>
#include <vector>
namespace bb=hcc::backup;
namespace pp=hcc::plist;
struct Wire {
    std::vector<std::uint8_t> bytes;
    const std::uint8_t* retry=nullptr;
    std::size_t retry_size=0;
    bool block=true;
    static bb::IoResult write(void* ctx,const std::uint8_t* p,std::size_t n) {
        auto& w=*static_cast<Wire*>(ctx);
        if(w.block) {w.retry=p;w.retry_size=n;w.block=false;return {0,bb::IoState::would_block};}
        assert(p==w.retry && n==w.retry_size);
        const auto take=std::min(n,std::size_t(3));
        w.bytes.insert(w.bytes.end(),p,p+take);w.block=true;
        return {take,bb::IoState::ok};
    }
};
int main() {
    std::array<std::uint8_t,512> output{};
    std::array<pp::Node,32> nodes{};std::array<char,1024> text{};
    for(auto code:{INT64_MIN,std::int64_t(-1),std::int64_t(0),INT64_MAX}) {
        for(bool dict:{false,true}) {
            bb::StatusReplyWriter writer(output.data(),output.size());Wire wire;
            assert(writer.begin(code,nullptr,dict)==bb::Error::ok);
            assert(writer.begin(0)==bb::Error::busy);
            for(unsigned i=0;!writer.done()&&i<1000;++i)
                assert(writer.pump({&wire,Wire::write})!=bb::Error::io_error);
            assert(writer.done());assert(bb::read_be32(wire.bytes.data())==wire.bytes.size()-4);
            pp::Document doc;
            assert(pp::parse(wire.bytes.data()+4,wire.bytes.size()-4,{nodes.data(),nodes.size(),text.data(),text.size()},doc)==pp::Error::ok);
            assert(doc.get(doc.root)->count==4);
            assert(doc.string(doc.at(doc.root,0))=="DLMessageStatusResponse");
            assert(doc.get(doc.at(doc.root,1))->integer==code);
            assert(doc.string(doc.at(doc.root,2))=="___EmptyParameterString___");
            if(dict) assert(doc.get(doc.at(doc.root,3))->kind==pp::Kind::dict);
            else assert(doc.string(doc.at(doc.root,3))=="___EmptyParameterString___");
            if(code==-1&&dict) {std::ofstream f("build/status.bplist",std::ios::binary);f.write(reinterpret_cast<const char*>(wire.bytes.data()+4),wire.bytes.size()-4);}
        }
    }
    for(auto bytes:{std::uint64_t(0),std::uint64_t(255),std::uint64_t(65536),std::uint64_t(1)<<40,std::uint64_t(INT64_MAX)}) {
        bb::StatusReplyWriter free_writer(output.data(),output.size());Wire wire;
        assert(free_writer.begin_free_space(bytes)==bb::Error::ok);
        assert(free_writer.begin_free_space(0)==bb::Error::busy);
        for(unsigned i=0;!free_writer.done()&&i<1000;++i)free_writer.pump({&wire,Wire::write});
        assert(free_writer.done());pp::Document doc;
        assert(pp::parse(wire.bytes.data()+4,wire.bytes.size()-4,{nodes.data(),nodes.size(),text.data(),text.size()},doc)==pp::Error::ok);
        assert(doc.get(doc.at(doc.root,1))->integer==0);
        assert(doc.get(doc.at(doc.root,3))->kind==pp::Kind::integer);
        assert(doc.get(doc.at(doc.root,3))->integer==static_cast<std::int64_t>(bytes));
        if(bytes==(std::uint64_t(1)<<40)){std::ofstream f("build/free_space.bplist",std::ios::binary);f.write(reinterpret_cast<const char*>(wire.bytes.data()+4),wire.bytes.size()-4);}
    }
    std::size_t n=99;
    assert(bb::encode_free_space_response(UINT64_MAX,output.data(),output.size(),n)==bb::LinkError::invalid_argument&&n==0);
    assert(bb::encode_free_space_response(1,output.data(),8,n)==bb::LinkError::too_large&&n==0);
    assert(bb::encode_status_response(0,"\xc3\xa0",true,output.data(),output.size(),n)==bb::LinkError::invalid_argument && n==0);
    assert(bb::encode_status_response(0,"failure",true,output.data(),8,n)==bb::LinkError::too_large && n==0);
    bb::StatusReplyWriter writer(output.data(),output.size());
    assert(writer.begin(-1,"failure")==bb::Error::ok);
    auto closed=[](void*,const std::uint8_t*,std::size_t n)->bb::IoResult{return {n,bb::IoState::closed};};
    assert(writer.pump({nullptr,closed})==bb::Error::closed && !writer.done());
    assert(writer.pump({nullptr,Wire::write})==bb::Error::closed);
    assert(writer.begin(0)==bb::Error::ok);
    auto zero=[](void*,const std::uint8_t*,std::size_t)->bb::IoResult{return {0,bb::IoState::ok};};
    assert(writer.pump({nullptr,zero})==bb::Error::no_progress);
    assert(writer.begin(0)==bb::Error::ok);
    auto oversized=[](void*,const std::uint8_t*,std::size_t n)->bb::IoResult{return {n+1,bb::IoState::ok};};
    assert(writer.pump({nullptr,oversized})==bb::Error::io_error);
    std::cout<<"Status replies: codec, partial writes, stable TLS retries and failures PASS\n";
}
