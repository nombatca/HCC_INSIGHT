#include "hcc_plist.hpp"
#include "hcc_backup_session.hpp"
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>
#include <vector>

namespace pp=hcc::plist;
namespace bb=hcc::backup;
int main() {
    std::cout<<"HCC V02 — mô phỏng bằng file mẫu, không kết nối thiết bị\n";
    std::array<pp::Node,256> nodes{};
    std::array<char,16384> text{};
    bb::SessionObserver session;
    for(const auto* name:{"version.bplist","ready.xml","hello.bplist","download.xml","result_ok.bplist","disconnect.xml"}) {
        std::ifstream in(std::string("tests/fixtures/")+name,std::ios::binary);
        if(!in) { std::cerr<<"Chạy ví dụ từ thư mục gốc của gói.\n"; return 1; }
        const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in),{}};
        pp::Document doc;
        const auto parsed=pp::parse(bytes.data(),bytes.size(),{nodes.data(),nodes.size(),text.data(),text.size()},doc);
        if(parsed!=pp::Error::ok) { std::cerr<<"Lỗi plist: "<<pp::error_name(parsed)<<"\n"; return 1; }
        bb::SessionEvent event;
        const auto accepted=session.consume(doc,event);
        if(accepted!=bb::SessionError::ok) { std::cerr<<"Lỗi trạng thái: "<<bb::session_error_name(accepted)<<"\n"; return 1; }
        switch(event.kind) {
        case bb::EventKind::version_offer:
            std::cout<<"Nhận đề nghị phiên bản "<<event.major<<"."<<event.minor<<"\n";
            // Demo acknowledgment only: no real reply is sent.
            session.acknowledge_version(true); break;
        case bb::EventKind::device_ready:
            std::cout<<"Mẫu DeviceReady; bắt đầu thao tác MÔ PHỎNG\n";
            session.mark_operation_started(); break;
        case bb::EventKind::download_files:
            std::cout<<"Danh sách yêu cầu: "<<doc.get(event.payload)->count<<" file\n";
            for(std::size_t i=0;i<doc.get(event.payload)->count;++i) std::cout<<"  "<<doc.string(doc.at(event.payload,i))<<"\n";
            // Demo acknowledgment only: no file is transmitted.
            session.complete_request(true); break;
        case bb::EventKind::result: std::cout<<"Kết quả trong mẫu: "<<event.result_code<<"\n"; break;
        case bb::EventKind::disconnect: std::cout<<"Nhận ngắt kết nối trong mẫu\n"; break;
        default: std::cout<<"Chuyển tiếp thông điệp điều khiển: "<<event.message<<"\n"; break;
        }
    }
    std::cout<<"Hoàn tất mô phỏng. Kết quả này không phải kiểm thử với iPad.\n";
}
