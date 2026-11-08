#include "hcc_plist.hpp"
#include "hcc_backup_session.hpp"
#include "hcc_backup_stream.hpp"
#include <array>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace pp=hcc::plist;
namespace bb=hcc::backup;
#define CHECK(x) do { if (!(x)) { std::cerr<<"FAIL line "<<__LINE__<<": " #x "\n"; std::abort(); } } while(false)
std::vector<std::uint8_t> fixture(const std::string& name) {
    std::ifstream in("tests/fixtures/"+name,std::ios::binary); CHECK(bool(in));
    return {std::istreambuf_iterator<char>(in),{}};
}
struct Memory {
    std::array<pp::Node,512> nodes{};
    std::array<char,32768> text{};
    pp::Document doc;
    pp::Workspace workspace() { return {nodes.data(),nodes.size(),text.data(),text.size()}; }
    pp::Error parse(const std::vector<std::uint8_t>& b,pp::Limits limits={}) {
        return pp::parse(b.data(),b.size(),workspace(),doc,limits);
    }
    pp::Error xml(const std::string& body) {
        const auto text="<plist version=\"1.0\">"+body+"</plist>";
        return pp::parse(reinterpret_cast<const std::uint8_t*>(text.data()),text.size(),workspace(),doc);
    }
};
void equivalent_types() {
    for (const auto ext:{"xml","bplist"}) {
        Memory m; CHECK(m.parse(fixture(std::string("types.")+ext))==pp::Error::ok);
        const auto& d=m.doc; const auto root=d.root;
        CHECK(d.get(root)->kind==pp::Kind::dict && d.get(root)->count==8);
        CHECK(d.string(d.find(root,"text"))=="Tiếng Việt <&> 😀");
        CHECK(d.get(d.find(root,"bool"))->boolean);
        CHECK(d.get(d.find(root,"negative"))->integer==-19);
        CHECK(d.get(d.find(root,"int"))->integer==255);
        CHECK(std::abs(d.get(d.find(root,"float"))->real-2.1)<1e-12);
        const auto repeat=d.find(root,"repeat");
        CHECK(d.string(d.at(repeat,0))=="same" && d.string(d.at(repeat,1))=="same");
        CHECK(d.string(d.find(root,"long")).size()==64);
        CHECK(d.get(d.find(root,"empty"))->count==0);
        CHECK(d.find(root,"not_here")==pp::none);
    }
}
void binary_scalars() {
    Memory m; CHECK(m.parse(fixture("binary_types.bplist"))==pp::Error::ok);
    const auto& d=m.doc;
    CHECK(d.get(d.find(d.root,"min"))->integer==INT64_MIN);
    CHECK(d.get(d.find(d.root,"max"))->integer==INT64_MAX);
    CHECK(d.get(d.find(d.root,"null"))->kind==pp::Kind::null_value);
    const auto* data=d.get(d.find(d.root,"data"));
    CHECK(data->kind==pp::Kind::data && data->length==3);
    CHECK(std::uint8_t(d.text[data->offset])==0 && std::uint8_t(d.text[data->offset+2])==255);
}
void xml_entities_and_rejections() {
    Memory m;
    CHECK(m.xml("<string>&#x1F600;&#7871;&amp;&lt;&gt;&apos;&quot;</string>")==pp::Error::ok);
    CHECK(m.doc.string(m.doc.root)=="😀ế&<>'\"");
    CHECK(m.xml("<dict><key/><string/></dict>")==pp::Error::ok);
    CHECK(m.xml("<key>bad root</key>")!=pp::Error::ok);
    CHECK(m.xml("<dict><key>x</key><key>bad value</key></dict>")!=pp::Error::ok);
    for(const auto s:{"<string>&unknown;</string>","<string>&#0;</string>","<string>&#xD800;</string>",
                      "<integer>9223372036854775808</integer>","<integer>1x</integer>",
                      "<real>NaN</real>","<real>1e9999</real>","<array><true/></dict>",
                      "<dict><string>not key</string><true/></dict>"}) {
        CHECK(m.xml(s)!=pp::Error::ok); CHECK(m.doc.root==pp::none);
    }
    CHECK(m.xml("<date>2026-11-07T00:00:00Z</date>")==pp::Error::unsupported);
    CHECK(m.xml("<data>AA==</data>")==pp::Error::unsupported);
    const std::string dtd="<!DOCTYPE plist [<!ENTITY x SYSTEM 'file:///not-opened'>]><plist><string>&x;</string></plist>";
    CHECK(pp::parse(reinterpret_cast<const std::uint8_t*>(dtd.data()),dtd.size(),m.workspace(),m.doc)==pp::Error::unsupported);
    CHECK(m.xml(std::string("<string>\xc0\xaf</string>"))==pp::Error::invalid_text);
}
void binary_malformed_and_shared_refs() {
    Memory m;
    CHECK(m.parse(fixture("cycle.bplist"))==pp::Error::cycle);
    CHECK(m.parse(fixture("duplicate.bplist"))==pp::Error::duplicate_key);
    CHECK(m.parse(fixture("bad_surrogate.bplist"))==pp::Error::invalid_text);
    CHECK(m.parse(fixture("bad_ascii.bplist"))==pp::Error::invalid_text);
    auto b=fixture("download.bplist");
    for(std::size_t n=1;n<b.size();++n) {
        const auto e=pp::parse(b.data(),n,m.workspace(),m.doc);
        CHECK(e!=pp::Error::ok); CHECK(m.doc.root==pp::none);
    }
    b[b.size()-26]=0; CHECK(m.parse(b)!=pp::Error::ok);
}
void limits_and_duplicate_keys() {
    Memory m; CHECK(m.xml("<dict><key>x</key><true/><key>x</key><false/></dict>")==pp::Error::duplicate_key);
    auto b=fixture("types.xml"); pp::Limits small; small.max_input=10;
    CHECK(m.parse(b,small)==pp::Error::too_large);
    std::array<pp::Node,1> nodes{};
    CHECK(pp::parse(b.data(),b.size(),{nodes.data(),1,m.text.data(),m.text.size()},m.doc)==pp::Error::no_space);
    CHECK(pp::parse(b.data(),b.size(),{m.nodes.data(),m.nodes.size(),m.text.data(),2},m.doc)==pp::Error::no_space);
    std::string deep="<plist>";
    for(unsigned i=0;i<12;++i) deep+="<array>";
    deep+="<true/>"; for(unsigned i=0;i<12;++i) deep+="</array>"; deep+="</plist>";
    pp::Limits depth; depth.max_depth=8;
    CHECK(pp::parse(reinterpret_cast<const std::uint8_t*>(deep.data()),deep.size(),m.workspace(),m.doc,depth)==pp::Error::too_deep);
}
bb::SessionError message(Memory& m,bb::SessionObserver& s,const std::string& name,bb::SessionEvent& e) {
    CHECK(m.parse(fixture(name))==pp::Error::ok); return s.consume(m.doc,e);
}
void ready(Memory& m,bb::SessionObserver& s) {
    bb::SessionEvent e;
    CHECK(message(m,s,"version.bplist",e)==bb::SessionError::ok && e.kind==bb::EventKind::version_offer);
    CHECK(e.major==400 && e.minor==0);
    CHECK(s.acknowledge_version(true)==bb::SessionError::ok);
    CHECK(message(m,s,"ready.xml",e)==bb::SessionError::ok && s.phase()==bb::Phase::ready);
}
void state_machine_complete_operation() {
    Memory m; bb::SessionObserver s; bb::SessionEvent e; ready(m,s);
    CHECK(message(m,s,"hello.bplist",e)==bb::SessionError::ok && e.kind==bb::EventKind::process_message);
    CHECK(s.outcome()==bb::Outcome::none && s.phase()==bb::Phase::ready);
    CHECK(s.mark_operation_started()==bb::SessionError::ok);
    CHECK(message(m,s,"hello.xml",e)==bb::SessionError::ok && s.phase()==bb::Phase::running);
    CHECK(message(m,s,"download.bplist",e)==bb::SessionError::ok);
    CHECK(e.kind==bb::EventKind::download_files && m.doc.get(e.payload)->count==2);
    CHECK(s.phase()==bb::Phase::request_pending);
    CHECK(message(m,s,"result_ok.xml",e)==bb::SessionError::wrong_state);
    CHECK(s.complete_request(true)==bb::SessionError::ok);
    CHECK(message(m,s,"result_ok.bplist",e)==bb::SessionError::ok);
    CHECK(e.kind==bb::EventKind::result && s.outcome()==bb::Outcome::success && s.phase()==bb::Phase::completed);
    CHECK(message(m,s,"disconnect.xml",e)==bb::SessionError::ok);
    CHECK(s.phase()==bb::Phase::disconnected && s.outcome()==bb::Outcome::success);
}
void state_machine_failures_and_disconnect() {
    Memory m; bb::SessionObserver s; bb::SessionEvent e;
    CHECK(message(m,s,"ready.xml",e)==bb::SessionError::wrong_state);
    CHECK(message(m,s,"version.xml",e)==bb::SessionError::ok);
    CHECK(message(m,s,"ready.xml",e)==bb::SessionError::wrong_state);
    CHECK(s.acknowledge_version(false)==bb::SessionError::incompatible_version);
    CHECK(s.outcome()==bb::Outcome::local_error);
    s.reset(); ready(m,s); CHECK(s.mark_operation_started()==bb::SessionError::ok);
    CHECK(message(m,s,"result_error.bplist",e)==bb::SessionError::ok);
    CHECK(e.result_code==5 && s.outcome()==bb::Outcome::remote_error);
    s.reset(); ready(m,s); CHECK(s.mark_operation_started()==bb::SessionError::ok);
    CHECK(message(m,s,"disconnect.bplist",e)==bb::SessionError::ok);
    CHECK(s.outcome()==bb::Outcome::none); // Disconnect alone is never success.
    s.reset(); ready(m,s); CHECK(s.mark_operation_started()==bb::SessionError::ok);
    CHECK(message(m,s,"download.xml",e)==bb::SessionError::ok);
    CHECK(s.complete_request(false)==bb::SessionError::ok && s.outcome()==bb::Outcome::local_error);
}
void request_validation_and_unsupported() {
    for(const auto p:{"../x","/absolute","C:\\x","a/../x","a//x","a/","a/./b","\\\\server\\x",""}) CHECK(!bb::valid_relative_backup_path(p));
    CHECK(bb::valid_relative_backup_path("sample/aa/hash"));
    CHECK(bb::valid_relative_backup_path("sample\\aa\\hash"));
    Memory m; bb::SessionObserver s; bb::SessionEvent e; ready(m,s);
    CHECK(s.mark_operation_started()==bb::SessionError::ok);
    CHECK(m.xml("<array><string>DLMessageDownloadFiles</string><array><string>../escape</string></array></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::invalid_path && s.phase()==bb::Phase::running);
    CHECK(m.xml("<array><string>DLMessageRemoveFiles</string><array/></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::unsupported_operation && s.phase()==bb::Phase::running);
    CHECK(m.xml("<array><string>DLMessageProcessMessage</string><dict><key>ErrorCode</key><string>0</string></dict></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::malformed_message && s.outcome()==bb::Outcome::none);
    CHECK(m.xml("<array><string>DLMessageUploadFiles</string><integer>0</integer></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::malformed_message && s.phase()==bb::Phase::running);
    CHECK(m.xml("<array><string>DLMessageUploadFiles</string><integer>0</integer><integer>0</integer><integer>123</integer></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::ok && e.kind==bb::EventKind::upload_files);
    CHECK(s.pending_request()==bb::EventKind::upload_files);
    CHECK(s.consume(m.doc,e)==bb::SessionError::wrong_state);
    CHECK(s.complete_request(true)==bb::SessionError::ok && s.pending_request()==bb::EventKind::unsupported);
}
void free_space_request_lifecycle() {
    Memory m;bb::SessionObserver s;bb::SessionEvent e;
    CHECK(m.parse(fixture("version.xml"))==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::ok);
    CHECK(s.acknowledge_version(true)==bb::SessionError::ok);
    CHECK(m.parse(fixture("ready.xml"))==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::ok);
    CHECK(m.xml("<array><string>DLMessageGetFreeDiskSpace</string></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::wrong_state);
    CHECK(s.mark_operation_started()==bb::SessionError::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::ok&&e.kind==bb::EventKind::free_disk_space);
    CHECK(s.pending_request()==bb::EventKind::free_disk_space);
    CHECK(s.consume(m.doc,e)==bb::SessionError::wrong_state);
    CHECK(s.complete_request(true)==bb::SessionError::ok&&s.phase()==bb::Phase::running);
    CHECK(s.outcome()==bb::Outcome::none);
}
void directory_request_lifecycle() {
    Memory m;bb::SessionObserver s;bb::SessionEvent e;
    CHECK(m.parse(fixture("version.xml"))==pp::Error::ok&&s.consume(m.doc,e)==bb::SessionError::ok);
    CHECK(s.acknowledge_version(true)==bb::SessionError::ok);
    CHECK(m.parse(fixture("ready.xml"))==pp::Error::ok&&s.consume(m.doc,e)==bb::SessionError::ok);
    CHECK(m.xml("<array><string>DLMessageCreateDirectory</string><string>backup/a</string></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::wrong_state);
    CHECK(s.mark_operation_started()==bb::SessionError::ok);
    for(const auto* body:{"<array><string>DLMessageCreateDirectory</string></array>","<array><string>DLMessageCreateDirectory</string><integer>1</integer></array>"}){
        CHECK(m.xml(body)==pp::Error::ok&&s.consume(m.doc,e)==bb::SessionError::malformed_message);
    }
    CHECK(m.xml("<array><string>DLMessageCreateDirectory</string><string>../escape</string></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::invalid_path&&s.phase()==bb::Phase::running);
    CHECK(m.xml("<array><string>DLMessageCreateDirectory</string><string>backup/a</string></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::ok&&e.kind==bb::EventKind::create_directory);
    CHECK(m.doc.string(e.payload)=="backup/a"&&s.pending_request()==bb::EventKind::create_directory);
    CHECK(s.consume(m.doc,e)==bb::SessionError::wrong_state);
    CHECK(s.complete_request(true)==bb::SessionError::ok&&s.phase()==bb::Phase::running&&s.outcome()==bb::Outcome::none);
}
void contents_request_lifecycle() {
    Memory m;bb::SessionObserver s;bb::SessionEvent e;
    CHECK(m.parse(fixture("version.xml"))==pp::Error::ok&&s.consume(m.doc,e)==bb::SessionError::ok);
    CHECK(s.acknowledge_version(true)==bb::SessionError::ok);
    CHECK(m.parse(fixture("ready.xml"))==pp::Error::ok&&s.consume(m.doc,e)==bb::SessionError::ok);
    // Rejected before the operation is marked running.
    CHECK(m.xml("<array><string>DLContentsOfDirectory</string><string>backup</string></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::wrong_state);
    CHECK(s.mark_operation_started()==bb::SessionError::ok);
    // Missing/typed path is malformed.
    for(const auto* body:{"<array><string>DLContentsOfDirectory</string></array>","<array><string>DLContentsOfDirectory</string><integer>1</integer></array>"}){
        CHECK(m.xml(body)==pp::Error::ok&&s.consume(m.doc,e)==bb::SessionError::malformed_message);
    }
    // Traversal is rejected without leaving the running phase.
    CHECK(m.xml("<array><string>DLContentsOfDirectory</string><string>../escape</string></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::invalid_path&&s.phase()==bb::Phase::running);
    // A valid relative path becomes a pending contents request carrying the path.
    CHECK(m.xml("<array><string>DLContentsOfDirectory</string><string>backup/sub</string></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::ok&&e.kind==bb::EventKind::contents_of_directory);
    CHECK(m.doc.string(e.payload)=="backup/sub"&&s.pending_request()==bb::EventKind::contents_of_directory);
    CHECK(s.consume(m.doc,e)==bb::SessionError::wrong_state);
    CHECK(s.complete_request(true)==bb::SessionError::ok&&s.phase()==bb::Phase::running&&s.outcome()==bb::Outcome::none);
    // An empty path lists the backup root and is accepted.
    CHECK(m.xml("<array><string>DLContentsOfDirectory</string><string></string></array>")==pp::Error::ok);
    CHECK(s.consume(m.doc,e)==bb::SessionError::ok&&e.kind==bb::EventKind::contents_of_directory);
    CHECK(m.doc.string(e.payload).empty()&&s.pending_request()==bb::EventKind::contents_of_directory);
    CHECK(s.complete_request(true)==bb::SessionError::ok&&s.phase()==bb::Phase::running);
}
void framed_session_one_byte_at_a_time() {
    std::vector<std::uint8_t> wire;
    for(const auto name:{"version.xml","ready.bplist","hello.xml","download.bplist","result_ok.xml","disconnect.bplist"}) {
        const auto b=fixture(name); std::array<std::uint8_t,4> header{};
        bb::write_be32(header.data(),std::uint32_t(b.size()));
        wire.insert(wire.end(),header.begin(),header.end()); wire.insert(wire.end(),b.begin(),b.end());
    }
    std::array<std::uint8_t,8192> frame{}; bb::FrameDecoder decoder(frame.data(),frame.size());
    Memory m; bb::SessionObserver session; std::size_t frames=0;
    for(const auto byte:wire) {
        std::size_t n=0; CHECK(decoder.feed(&byte,1,n)==bb::Error::ok && n==1);
        if(!decoder.ready()) continue;
        CHECK(pp::parse(decoder.data(),decoder.size(),m.workspace(),m.doc)==pp::Error::ok);
        bb::SessionEvent e; CHECK(session.consume(m.doc,e)==bb::SessionError::ok); ++frames;
        if(e.kind==bb::EventKind::version_offer) CHECK(session.acknowledge_version(true)==bb::SessionError::ok);
        if(e.kind==bb::EventKind::device_ready) CHECK(session.mark_operation_started()==bb::SessionError::ok);
        if(e.kind==bb::EventKind::download_files) CHECK(session.complete_request(true)==bb::SessionError::ok);
        decoder.next_frame();
    }
    CHECK(frames==6 && session.outcome()==bb::Outcome::success);
}
void deterministic_mutation_corpus() {
    const auto seed=fixture("types.bplist"); Memory m; std::uint32_t random=0x12345678;
    for(unsigned iteration=0;iteration<4000;++iteration) {
        auto b=seed;
        for(unsigned j=0;j<1+(iteration%5);++j) {
            random^=random<<13; random^=random>>17; random^=random<<5;
            b[random%b.size()]^=std::uint8_t(1u<<(random%8));
        }
        const auto e=m.parse(b);
        if(e==pp::Error::ok) {
            CHECK(m.doc.root<m.doc.size && m.doc.size<=m.nodes.size());
            for(std::size_t i=0;i<m.doc.size;++i) {
                const auto& n=m.doc.nodes[i];
                CHECK(n.next==pp::none || n.next<m.doc.size);
                CHECK(n.first==pp::none || n.first<m.doc.size);
                CHECK(n.offset<=m.doc.text_size && n.length<=m.doc.text_size-n.offset);
            }
        } else CHECK(m.doc.root==pp::none);
    }
}
int main() {
    const struct { const char* name;void(*run)(); } tests[]={
        {"XML and binary equivalent typed values",equivalent_types},
        {"binary data, null, signed integer bounds",binary_scalars},
        {"XML Unicode/entities and explicit unsupported forms",xml_entities_and_rejections},
        {"binary cycles, duplicate keys and truncated files",binary_malformed_and_shared_refs},
        {"input, depth, arena and text budgets",limits_and_duplicate_keys},
        {"session request/result lifecycle",state_machine_complete_operation},
        {"negotiation failure and disconnect is not success",state_machine_failures_and_disconnect},
        {"request types, relative paths and unsupported operations",request_validation_and_unsupported},
        {"free-space request requires running and explicit acknowledgement",free_space_request_lifecycle},
        {"directory request validation and lifecycle",directory_request_lifecycle},
        {"contents-of-directory request validation and lifecycle",contents_request_lifecycle},
        {"framed mixed XML/binary session fragmented by byte",framed_session_one_byte_at_a_time},
        {"4000 deterministic binary-plist mutations",deterministic_mutation_corpus},
    };
    for(const auto& t:tests) { t.run(); std::cout<<"PASS: "<<t.name<<"\n"; }
    std::cout<<sizeof(tests)/sizeof(tests[0])<<" protocol test groups passed; no device connection performed.\n";
}
