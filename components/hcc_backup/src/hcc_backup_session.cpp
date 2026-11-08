#include "hcc_backup_session.hpp"

namespace hcc::backup {
bool valid_relative_backup_path(std::string_view path) {
    if(path.empty()||path.size()>4096||path.front()=='/'||path.front()=='\\') return false;
    std::size_t start=0;
    for(std::size_t i=0;i<=path.size();++i) {
        if(i<path.size() && (path[i]==':'||static_cast<unsigned char>(path[i])<32)) return false;
        if(i==path.size()||path[i]=='/'||path[i]=='\\') {
            const auto part=path.substr(start,i-start);
            if(part.empty()||part=="."||part=="..") return false;
            start=i+1;
        }
    }
    return true;
}
SessionError SessionObserver::acknowledge_version(bool accepted) {
    if(phase_!=Phase::version_decision) return SessionError::wrong_state;
    if(!accepted) { phase_=Phase::failed; outcome_=Outcome::local_error; return SessionError::incompatible_version; }
    phase_=Phase::await_ready; return SessionError::ok;
}
SessionError SessionObserver::mark_operation_started() {
    if(phase_!=Phase::ready) return SessionError::wrong_state;
    phase_=Phase::running; return SessionError::ok;
}
SessionError SessionObserver::complete_request(bool success) {
    if(phase_!=Phase::request_pending) return SessionError::wrong_state;
    phase_=success?Phase::running:Phase::failed;
    pending_=EventKind::unsupported;
    if(!success) outcome_=Outcome::local_error;
    return SessionError::ok;
}
void SessionObserver::transport_failed() {
    if(outcome_==Outcome::none) outcome_=Outcome::local_error;
    phase_=Phase::failed;
}
SessionError SessionObserver::consume(const plist::Document& d,SessionEvent& out) {
    out={}; const auto* root=d.get(d.root);
    if(!root||root->kind!=plist::Kind::array||!root->count) return SessionError::malformed_message;
    const auto name_id=d.at(d.root,0); const auto* name=d.get(name_id);
    if(!name||name->kind!=plist::Kind::string) return SessionError::malformed_message;
    const auto tag=d.string(name_id); out.message=tag;
    if(phase_==Phase::disconnected||phase_==Phase::failed) return SessionError::wrong_state;
    if(tag=="DLMessageDisconnect") { out.kind=EventKind::disconnect; phase_=Phase::disconnected; return SessionError::ok; }
    if(phase_==Phase::completed) return SessionError::wrong_state;
    if(tag=="DLMessageVersionExchange") {
        if(phase_!=Phase::await_version) return SessionError::wrong_state;
        const auto* major=d.get(d.at(d.root,1)); const auto* minor=d.get(d.at(d.root,2));
        if(!major||!minor||major->kind!=plist::Kind::integer||minor->kind!=plist::Kind::integer||major->integer<0||minor->integer<0) return SessionError::malformed_message;
        out.kind=EventKind::version_offer; out.major=major->integer; out.minor=minor->integer;
        phase_=Phase::version_decision; return SessionError::ok;
    }
    if(tag=="DLMessageDeviceReady") {
        if(phase_!=Phase::await_ready) return SessionError::wrong_state;
        phase_=Phase::ready; out.kind=EventKind::device_ready; return SessionError::ok;
    }
    if(phase_!=Phase::ready&&phase_!=Phase::running&&phase_!=Phase::request_pending) return SessionError::wrong_state;
    if(tag=="DLMessageDownloadFiles") {
        if(phase_!=Phase::running) return SessionError::wrong_state;
        const auto files=d.at(d.root,1); const auto* a=d.get(files);
        if(!a||a->kind!=plist::Kind::array) return SessionError::malformed_message;
        for(std::size_t i=0;i<a->count;++i) {
            const auto id=d.at(files,i); const auto* n=d.get(id);
            if(!n||n->kind!=plist::Kind::string) return SessionError::malformed_message;
            if(!valid_relative_backup_path(d.string(id))) return SessionError::invalid_path;
        }
        out.kind=EventKind::download_files; out.payload=files;
        pending_=EventKind::download_files;
        phase_=Phase::request_pending; return SessionError::ok;
    }
    if(tag=="DLMessageCreateDirectory") {
        if(phase_!=Phase::running)return SessionError::wrong_state;
        const auto path=d.at(d.root,1);const auto* node=d.get(path);
        if(!node||node->kind!=plist::Kind::string)return SessionError::malformed_message;
        if(!valid_relative_backup_path(d.string(path)))return SessionError::invalid_path;
        out.kind=EventKind::create_directory;out.payload=path;pending_=out.kind;
        phase_=Phase::request_pending;return SessionError::ok;
    }
    if(tag=="DLMessageGetFreeDiskSpace") {
        if(phase_!=Phase::running) return SessionError::wrong_state;
        out.kind=EventKind::free_disk_space;pending_=out.kind;
        phase_=Phase::request_pending;return SessionError::ok;
    }
    if(tag=="DLContentsOfDirectory") {
        if(phase_!=Phase::running) return SessionError::wrong_state;
        const auto path=d.at(d.root,1); const auto* node=d.get(path);
        if(!node||node->kind!=plist::Kind::string) return SessionError::malformed_message;
        // Empty string lists the backup root; any other path must be relative and
        // traversal-free. payload borrows the request path for the operation driver.
        if(!d.string(path).empty()&&!valid_relative_backup_path(d.string(path))) return SessionError::invalid_path;
        out.kind=EventKind::contents_of_directory;out.payload=path;pending_=out.kind;
        phase_=Phase::request_pending;return SessionError::ok;
    }
    if(tag=="DLMessageUploadFiles") {
        if(phase_!=Phase::running) return SessionError::wrong_state;
        // Known UploadFiles shape has four or more elements. The fourth value
        // is an optional size hint in the reference implementation; bound/type
        // validation of the actual raw transfer remains with the receiver.
        if(root->count<4) return SessionError::malformed_message;
        out.kind=EventKind::upload_files;out.payload=d.root;
        pending_=EventKind::upload_files;phase_=Phase::request_pending;
        return SessionError::ok;
    }
    if(tag=="DLMessageProcessMessage") {
        if(phase_==Phase::request_pending) return SessionError::wrong_state;
        const auto payload=d.at(d.root,1); const auto* obj=d.get(payload);
        if(!obj||obj->kind!=plist::Kind::dict) return SessionError::malformed_message;
        out.payload=payload; out.kind=EventKind::process_message;
        const auto message_name=d.find(payload,"MessageName");
        if(message_name!=plist::none) {
            const auto* n=d.get(message_name);
            if(!n||n->kind!=plist::Kind::string) return SessionError::malformed_message;
            return SessionError::ok; // Named negotiation/control replies are not final results.
        }
        if(phase_==Phase::ready) return SessionError::ok;
        const auto code_id=d.find(payload,"ErrorCode");
        if(code_id!=plist::none) {
            const auto* code=d.get(code_id);
            if(!code||code->kind!=plist::Kind::integer) return SessionError::malformed_message;
            if(phase_!=Phase::running) return SessionError::wrong_state;
            out.kind=EventKind::result; out.result_code=code->integer;
            outcome_=code->integer==0?Outcome::success:Outcome::remote_error;
            phase_=code->integer==0?Phase::completed:Phase::failed;
        }
        return SessionError::ok;
    }
    out.kind=EventKind::unsupported;
    if(tag=="DLMessageMoveFiles"||tag=="DLMessageMoveItems"||tag=="DLMessageRemoveFiles"||
       tag=="DLMessageRemoveItems") return SessionError::unsupported_operation;
    return SessionError::unsupported_message;
}
const char* phase_name(Phase p) {
    switch(p) {
    case Phase::await_version:return "await_version";case Phase::version_decision:return "version_decision";
    case Phase::await_ready:return "await_ready";case Phase::ready:return "ready";
    case Phase::running:return "running";case Phase::request_pending:return "request_pending";
    case Phase::completed:return "completed";case Phase::disconnected:return "disconnected";
    case Phase::failed:return "failed";
    } return "unknown";
}
const char* session_error_name(SessionError e) {
    switch(e) {
    case SessionError::ok:return "ok";case SessionError::malformed_message:return "malformed_message";
    case SessionError::wrong_state:return "wrong_state";case SessionError::unsupported_message:return "unsupported_message";
    case SessionError::unsupported_operation:return "unsupported_operation";case SessionError::invalid_path:return "invalid_path";
    case SessionError::incompatible_version:return "incompatible_version";
    } return "unknown";
}
} // namespace hcc::backup
