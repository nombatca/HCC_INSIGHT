#include "hcc_backup_link.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace bb=hcc::backup;namespace pp=hcc::plist;
struct SimulatedService {
    std::vector<std::uint8_t> input,output;std::size_t offset=0;
    bool append(const char* file) {
        std::ifstream in(std::string("tests/fixtures/")+file,std::ios::binary);if(!in)return false;
        const std::vector<std::uint8_t> b{std::istreambuf_iterator<char>(in),{}};
        std::uint8_t header[4];bb::write_be32(header,std::uint32_t(b.size()));
        input.insert(input.end(),header,header+4);input.insert(input.end(),b.begin(),b.end());return true;
    }
    static bb::IoResult read(void* p,std::uint8_t* b,std::size_t n) {
        auto& s=*static_cast<SimulatedService*>(p);
        const auto take=std::min({n,std::size_t(11),s.input.size()-s.offset});
        if(!take)return {0,bb::IoState::would_block};
        std::copy_n(s.input.data()+s.offset,take,b);s.offset+=take;return {take,bb::IoState::ok};
    }
    static bb::IoResult write(void* p,const std::uint8_t* b,std::size_t n) {
        auto& s=*static_cast<SimulatedService*>(p);const auto take=std::min<std::size_t>(8,n);
        s.output.insert(s.output.end(),b,b+take);return {take,bb::IoState::ok};
    }
};
const char* phase_vi(bb::LinkPhase p) {
    switch(p) {
    case bb::LinkPhase::idle:return "Chưa bắt đầu";case bb::LinkPhase::await_version:return "Chờ phiên bản DeviceLink";
    case bb::LinkPhase::send_version:return "Đang gửi phản hồi phiên bản";case bb::LinkPhase::await_ready:return "Chờ DeviceReady";
    case bb::LinkPhase::send_hello:return "Đang gửi Hello";case bb::LinkPhase::await_hello:return "Chờ phản hồi Hello";
    case bb::LinkPhase::ready:return "Thương lượng xong";case bb::LinkPhase::failed:return "Lỗi kết nối";
    }return "Không xác định";
}
int main() {
    std::cout<<"HCC V03 — kết nối dịch vụ mô phỏng, không mở USB hoặc TLS thật\n";
    std::array<std::uint8_t,8192> rx{};std::array<std::uint8_t,512> tx{};
    std::array<pp::Node,256> nodes{};std::array<char,16384> text{};
    bb::BackupLink link({rx.data(),rx.size(),tx.data(),tx.size(),{nodes.data(),nodes.size(),text.data(),text.size()}});
    SimulatedService service;
    if(!service.append("version.bplist")||!service.append("ready.xml")||!service.append("hello.bplist")) {
        std::cerr<<"Chạy ví dụ từ thư mục gốc của gói.\n";return 1;
    }
    if(link.start(0)!=bb::LinkError::ok)return 1;
    auto previous=link.phase();std::cout<<phase_vi(previous)<<"\n";
    for(std::uint64_t now=0;now<10000;++now) {
        const auto e=link.poll({&service,SimulatedService::read,SimulatedService::write},now,4);
        if(link.phase()!=previous) {previous=link.phase();std::cout<<phase_vi(previous)<<"\n";}
        if(e==bb::LinkError::ok) {
            std::cout<<"Phiên bản trong mẫu: "<<link.protocol_version()<<"; nhận "<<link.received_bytes()<<" byte, gửi "<<link.sent_bytes()<<" byte\n";
            std::cout<<"Chỉ kiểm thử thương lượng. Chưa gửi thao tác backup/restore.\n";return 0;
        }
        if(e!=bb::LinkError::yield&&e!=bb::LinkError::would_block) {std::cerr<<bb::link_error_name(e)<<"\n";return 1;}
    }
    return 1;
}
