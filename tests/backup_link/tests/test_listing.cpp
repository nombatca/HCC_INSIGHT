// DLContentsOfDirectory storage reconstruction and reply encoding tests.
// Exercises the real StagedUploadTarget filesystem layout (indexed .ready
// records + created-directory namespace) against real temporary files, then
// encodes the reconstructed listing and decodes it with the project parser.
#include "hcc_backup_storage.hpp"
#include "hcc_backup_link.hpp"
#include "hcc_plist.hpp"
#include <cassert>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>
namespace bb=hcc::backup;
namespace pp=hcc::plist;
namespace fs=std::filesystem;

static void upload(bb::StagedUploadTarget& t,const char* dest,const std::string& body){
    auto cb=t.target();
    assert(cb.begin(cb.context,"/device/source",dest)==bb::Error::ok);
    if(!body.empty()) assert(cb.write(cb.context,reinterpret_cast<const std::uint8_t*>(body.data()),body.size()).bytes==body.size());
    assert(cb.finish(cb.context,true)==bb::Error::ok);
}
static const bb::DirectoryEntry* find(const bb::DirectoryListing& l,const char* name){
    for(std::size_t i=0;i<l.count;++i) if(std::strcmp(l.entries[i].name,name)==0) return &l.entries[i];
    return nullptr;
}

// Decode an encoded reply and assert the four-item status envelope plus a dict.
static pp::Document decode(const std::vector<std::uint8_t>& bytes,pp::Node* nodes,std::size_t node_cap,char* text,std::size_t text_cap){
    pp::Document d;
    assert(pp::parse(bytes.data(),bytes.size(),{nodes,node_cap,text,text_cap},d)==pp::Error::ok);
    assert(d.string(d.at(d.root,0))=="DLMessageStatusResponse");
    assert(d.get(d.at(d.root,1))->integer==0);
    assert(d.string(d.at(d.root,2))=="___EmptyParameterString___");
    return d;
}

int main(){
    char tmpl[]="/tmp/hcc-listing-XXXXXX"; assert(mkdtemp(tmpl));
    const fs::path parent(tmpl);
    bb::DirectoryEntry storage[64]; bb::DirectoryListing list{storage,64,0};
    static bb::ListingScratch scratch;

    // Reconstructed tree combining nested file destinations and a created directory.
    {
        bb::StagedUploadTarget t; assert(t.open_session(tmpl)==bb::Error::ok);
        upload(t,"backup/Info.plist","0123456789");          // 10-byte regular
        upload(t,"backup/Manifest.db","");                    // empty regular
        upload(t,"backup/Snapshot/data.bin","abc");           // implies Snapshot/ dir
        assert(t.create_directory("backup/emptydir")==bb::Error::ok);

        assert(t.list_directory("backup",list,scratch)==bb::Error::ok&&list.count==4);
        const auto* info=find(list,"Info.plist"); assert(info&&info->type==bb::EntryType::regular&&info->size==10);
        const auto* man=find(list,"Manifest.db"); assert(man&&man->type==bb::EntryType::regular&&man->size==0);
        const auto* snap=find(list,"Snapshot"); assert(snap&&snap->type==bb::EntryType::directory&&snap->size==0);
        const auto* empty=find(list,"emptydir"); assert(empty&&empty->type==bb::EntryType::directory);
        assert(!find(list,"payload.bin")&&!find(list,"identity.bin")); // internal names never leak

        // Nested directory lists only its own file.
        assert(t.list_directory("backup/Snapshot",list,scratch)==bb::Error::ok&&list.count==1);
        assert(find(list,"data.bin")&&find(list,"data.bin")->type==bb::EntryType::regular&&find(list,"data.bin")->size==3);

        // Root collapses every source into the single top directory.
        assert(t.list_directory("",list,scratch)==bb::Error::ok&&list.count==1);
        assert(find(list,"backup")&&find(list,"backup")->type==bb::EntryType::directory);

        // A directory with no children is a valid empty listing, not an error.
        assert(t.list_directory("backup/does/not/exist",list,scratch)==bb::Error::ok&&list.count==0);

        // Traversal is rejected outright.
        assert(t.list_directory("../escape",list,scratch)==bb::Error::invalid_argument);

        // Overflow fails loudly rather than truncating.
        bb::DirectoryEntry tiny_store[2]; bb::DirectoryListing tiny{tiny_store,2,0};
        assert(t.list_directory("backup",tiny,scratch)==bb::Error::too_large);
    }
    std::cout<<"PASS: reconstructed listing, nested/root/empty, traversal and overflow\n";

    // Encode a listing and decode it back with the project parser (date supported).
    {
        bb::StagedUploadTarget t; assert(t.open_session(tmpl)==bb::Error::ok);
        upload(t,"set/alpha.txt","hello");
        assert(t.create_directory("set/beta")==bb::Error::ok);
        assert(t.list_directory("set",list,scratch)==bb::Error::ok&&list.count==2);

        std::uint32_t offsets[bb::contents_object_count(64)];
        std::vector<std::uint8_t> out(4096); std::size_t written=0;
        assert(bb::encode_contents_of_directory_response(list,offsets,bb::contents_object_count(64),out.data(),out.size(),written)==bb::LinkError::ok);
        out.resize(written);
        pp::Node nodes[256]; char text[4096];
        auto d=decode(out,nodes,256,text,sizeof(text));
        const auto dict=d.at(d.root,3); assert(d.get(dict)->kind==pp::Kind::dict&&d.get(dict)->count==2);
        const auto alpha=d.find(dict,"alpha.txt"); assert(alpha!=pp::none);
        assert(d.string(d.find(alpha,"DLFileType"))=="DLFileTypeRegular");
        assert(d.get(d.find(alpha,"DLFileSize"))->integer==5);
        assert(d.get(d.find(alpha,"DLFileModificationDate"))->kind==pp::Kind::real); // date decoded as real
        const auto beta=d.find(dict,"beta"); assert(beta!=pp::none);
        assert(d.string(d.find(beta,"DLFileType"))=="DLFileTypeDirectory");
        assert(d.get(d.find(beta,"DLFileSize"))->integer==0);

        // Empty listing encodes an empty dictionary with status 0.
        bb::DirectoryListing empty{storage,64,0};
        std::vector<std::uint8_t> out2(256); std::size_t w2=0;
        assert(bb::encode_contents_of_directory_response(empty,offsets,bb::contents_object_count(64),out2.data(),out2.size(),w2)==bb::LinkError::ok);
        out2.resize(w2);
        auto d2=decode(out2,nodes,256,text,sizeof(text));
        assert(d2.get(d2.at(d2.root,3))->kind==pp::Kind::dict&&d2.get(d2.at(d2.root,3))->count==0);
    }
    std::cout<<"PASS: encoded reply decodes to the expected status envelope and file dictionary\n";

    // A destination that is both a file and a directory prefix is ambiguous.
    {
        bb::StagedUploadTarget t; assert(t.open_session(tmpl)==bb::Error::ok);
        upload(t,"dir/x","leaf");         // x is a file
        upload(t,"dir/x/y","nested");     // ... and also a directory prefix
        assert(t.list_directory("dir",list,scratch)==bb::Error::io_error);
    }
    std::cout<<"PASS: file/directory name collision fails instead of guessing\n";

    // Corrupt metadata fails the listing; it is never silently dropped.
    {
        bb::StagedUploadTarget t; assert(t.open_session(tmpl)==bb::Error::ok);
        upload(t,"c/keep","data");
        std::ofstream(fs::path(t.session_path())/"000001.ready/identity.bin",std::ios::binary|std::ios::trunc).write("HCC",3);
        assert(t.list_directory("c",list,scratch)==bb::Error::io_error);
    }
    std::cout<<"PASS: corrupt identity metadata is a loud error, never a dropped entry\n";

    // A symlink planted in the namespace is rejected (host symlink semantics).
    {
        bb::StagedUploadTarget t; assert(t.open_session(tmpl)==bb::Error::ok);
        assert(t.create_directory("s/real")==bb::Error::ok);
        fs::create_directory_symlink(parent,fs::path(t.session_path())/"directories/s/link");
        assert(t.list_directory("s",list,scratch)==bb::Error::io_error);
    }
    std::cout<<"PASS: namespace symlink entries are rejected\n";

    fs::remove_all(parent);
    std::cout<<"All DLContentsOfDirectory listing tests passed; no device connection performed.\n";
    return 0;
}
