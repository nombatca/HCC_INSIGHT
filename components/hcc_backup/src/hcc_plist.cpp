#include "hcc_plist.hpp"
#include <charconv>
#include <cmath>
#include <cstring>
#include <limits>

namespace hcc::plist {
const Node* Document::get(std::uint32_t i) const { return i < size ? nodes+i : nullptr; }
std::string_view Document::string(std::uint32_t i) const {
    const auto* n=get(i);
    if (!n || n->kind!=Kind::string || n->offset>text_size || n->length>text_size-n->offset) return {};
    return {text+n->offset,n->length};
}
std::uint32_t Document::at(std::uint32_t id,std::size_t index) const {
    const auto* n=get(id); if (!n || n->kind!=Kind::array || index>=n->count) return none;
    auto c=n->first; while (index-- && c!=none) c=nodes[c].next; return c;
}
std::uint32_t Document::find(std::uint32_t id,std::string_view key) const {
    const auto* n=get(id); if (!n || n->kind!=Kind::dict) return none;
    auto c=n->first;
    while (c!=none) { const auto v=nodes[c].next; if (v==none) return none;
        if (string(c)==key) return v;
        c=nodes[v].next; }
    return none;
}
namespace {
bool ws(char c) { return c==' ' || c=='\n' || c=='\r' || c=='\t'; }
bool scalar(std::uint32_t c) {
    return c && c<=0x10ffff && !(c>=0xd800 && c<=0xdfff) &&
           (c>=32 || c==9 || c==10 || c==13) && c!=0xfffe && c!=0xffff;
}
bool utf8(std::string_view s) {
    std::size_t p=0;
    while (p<s.size()) {
        const auto b=std::uint8_t(s[p++]); std::uint32_t cp; unsigned extra;
        if (b<128) { if (!scalar(b)) return false; continue; }
        if (b>=0xc2 && b<=0xdf) { cp=b&31; extra=1; }
        else if (b>=0xe0 && b<=0xef) { cp=b&15; extra=2; }
        else if (b>=0xf0 && b<=0xf4) { cp=b&7; extra=3; }
        else return false;
        if (extra>s.size()-p) return false;
        for (unsigned i=0;i<extra;++i) { const auto v=std::uint8_t(s[p++]); if ((v&0xc0)!=0x80) return false; cp=(cp<<6)|(v&63); }
        if ((extra==1 && cp<0x80)||(extra==2 && cp<0x800)||(extra==3 && cp<0x10000)||!scalar(cp)) return false;
    }
    return true;
}
struct Parser {
    const std::uint8_t* bytes; std::size_t length; Workspace w; Limits limits;
    Error error=Error::ok; std::size_t used=0,chars=0,pos=0;
    std::uint64_t object_count=0,top=0,table=0;
    unsigned offset_width=0,ref_width=0;
    std::uint64_t active[64]{};
    bool fail(Error e=Error::malformed) { if (error==Error::ok) error=e; return false; }
    std::uint32_t node(Kind k) {
        if (used>=w.node_capacity || used>=none) { fail(Error::no_space); return none; }
        const auto n=std::uint32_t(used++); w.nodes[n]=Node{}; w.nodes[n].kind=k; return n;
    }
    bool append(char c) { if (chars>=w.text_capacity) return fail(Error::no_space); w.text[chars++]=c; return true; }
    bool codepoint(std::uint32_t c) {
        if (!scalar(c)) return fail(Error::invalid_text);
        if (c<128) return append(char(c));
        if (c<0x800) return append(char(0xc0|(c>>6))) && append(char(0x80|(c&63)));
        if (c<0x10000) return append(char(0xe0|(c>>12))) && append(char(0x80|((c>>6)&63))) && append(char(0x80|(c&63)));
        return append(char(0xf0|(c>>18))) && append(char(0x80|((c>>12)&63))) && append(char(0x80|((c>>6)&63))) && append(char(0x80|(c&63)));
    }
    std::string_view text(std::uint32_t i) { const auto& n=w.nodes[i]; return {w.text+n.offset,n.length}; }
    bool end_string(std::uint32_t i,std::size_t start) {
        if (chars-start>UINT32_MAX || start>UINT32_MAX) return fail(Error::too_large);
        w.nodes[i].offset=std::uint32_t(start); w.nodes[i].length=std::uint32_t(chars-start);
        if (!utf8({w.text+start,chars-start})) return fail(Error::invalid_text);
        return append(0);
    }
    bool attach(std::uint32_t parent,std::uint32_t child,std::uint32_t& last) {
        if (child==none) return false;
        auto& n=w.nodes[parent];
        if (last==none) n.first=child; else w.nodes[last].next=child;
        last=child; ++n.count; return true;
    }
    bool check_key(std::uint32_t parent,std::uint32_t key) {
        if (key==none || w.nodes[key].kind!=Kind::string) return fail();
        for (auto c=w.nodes[parent].first;c!=none;) {
            if (text(c)==text(key)) return fail(Error::duplicate_key);
            const auto v=w.nodes[c].next; if (v==none) return fail(); c=w.nodes[v].next;
        }
        return true;
    }
    bool range(std::uint64_t p,std::uint64_t n,std::uint64_t end) {
        return p<=end && n<=end-p;
    }
    bool integer_at(std::uint64_t p,unsigned width,std::uint64_t end,std::uint64_t& value) {
        if (!width || width>8 || !range(p,width,end)) return fail();
        value=0; for (unsigned i=0;i<width;++i) value=(value<<8)|bytes[std::size_t(p)+i]; return true;
    }
    bool binary_length(std::uint64_t& p,unsigned lo,std::uint64_t& n) {
        n=lo; if (lo!=15) return true;
        if (p>=table) return fail();
        const auto marker=bytes[p++];
        if ((marker>>4)!=1 || (marker&15)>3) return fail();
        const auto width=1u<<(marker&15);
        if (!integer_at(p,width,table,n)) return false;
        p+=width; return true;
    }
    std::uint32_t binary(std::uint64_t obj,std::uint32_t depth) {
        if (depth>=limits.max_depth || depth>=64) { fail(Error::too_deep); return none; }
        if (obj>=object_count) { fail(); return none; }
        for (std::uint32_t i=0;i<depth;++i) if (active[i]==obj) { fail(Error::cycle); return none; }
        active[depth]=obj;
        std::uint64_t p=0;
        if (!integer_at(table+obj*offset_width,offset_width,length-32,p) || p<8 || p>=table) { fail(); return none; }
        const auto marker=bytes[p++]; const unsigned hi=marker>>4,lo=marker&15;
        auto id=none;
        if (hi==0) {
            if (lo==0) id=node(Kind::null_value);
            else if (lo==8 || lo==9) { id=node(Kind::boolean); if (id!=none) w.nodes[id].boolean=lo==9; }
            else fail(Error::unsupported);
        } else if (hi==1 || hi==2) {
            if (lo>3 || (hi==2 && lo!=2 && lo!=3)) { fail(Error::unsupported); return none; }
            const auto width=1u<<lo; std::uint64_t v=0;
            if (!integer_at(p,width,table,v)) return none;
            id=node(hi==1?Kind::integer:Kind::real); if (id==none) return none;
            if (hi==1) {
                if (width==8) std::memcpy(&w.nodes[id].integer,&v,8); else w.nodes[id].integer=std::int64_t(v);
            } else {
                if (width==4) { const auto v32=std::uint32_t(v); float f; std::memcpy(&f,&v32,4); w.nodes[id].real=f; }
                else std::memcpy(&w.nodes[id].real,&v,8);
                if (!std::isfinite(w.nodes[id].real)) fail();
            }
        } else if (hi==4 || hi==5 || hi==6 || hi==10 || hi==13) {
            std::uint64_t n=0; if (!binary_length(p,lo,n)) return none;
            if (hi==4 || hi==5 || hi==6) {
                const auto width=hi==6?2u:1u;
                if (n>std::numeric_limits<std::size_t>::max()/width || !range(p,n*width,table)) { fail(); return none; }
                id=node(hi==4?Kind::data:Kind::string); if (id==none) return none;
                const auto start=chars;
                for (std::uint64_t j=0;j<n;++j) {
                    if (hi==6) {
                        std::uint32_t c=(std::uint32_t(bytes[p+j*2])<<8)|bytes[p+j*2+1];
                        if (c>=0xd800 && c<=0xdbff) {
                            if (++j>=n) { fail(Error::invalid_text); break; }
                            const auto low=(std::uint32_t(bytes[p+j*2])<<8)|bytes[p+j*2+1];
                            if (low<0xdc00 || low>0xdfff) { fail(Error::invalid_text); break; }
                            c=0x10000+((c-0xd800)<<10)+(low-0xdc00);
                        }
                        if (!codepoint(c)) break;
                    } else {
                        if (hi==5 && bytes[p+j]>=128) { fail(Error::invalid_text); break; }
                        if (!append(char(bytes[p+j]))) break;
                    }
                }
                if (error!=Error::ok) return none;
                if (hi==4) { w.nodes[id].offset=std::uint32_t(start); w.nodes[id].length=std::uint32_t(chars-start); }
                else if (!end_string(id,start)) return none;
            } else {
                const auto factor=hi==13?2u:1u;
                if (n>w.node_capacity || n>UINT32_MAX || !range(p,n*factor*ref_width,table)) { fail(Error::too_large); return none; }
                id=node(hi==13?Kind::dict:Kind::array); if (id==none) return none;
                auto last=none;
                for (std::uint64_t j=0;j<n;++j) {
                    std::uint64_t ref=0; if (!integer_at(p+j*ref_width,ref_width,table,ref)) return none;
                    const auto child=binary(ref,depth+1); if (child==none) return none;
                    if (hi==13 && !check_key(id,child)) return none;
                    if (!attach(id,child,last)) return none;
                    if (hi==13) {
                        if (!integer_at(p+(j+n)*ref_width,ref_width,table,ref)) return none;
                        if (!attach(id,binary(ref,depth+1),last)) return none;
                    }
                }
                if (hi==13) w.nodes[id].count/=2;
            }
        } else fail(Error::unsupported);
        return error==Error::ok?id:none;
    }
    bool begins(std::string_view s) const { return s.size()<=length-pos && !std::memcmp(bytes+pos,s.data(),s.size()); }
    bool skip() {
        for (;;) {
            while (pos<length && ws(char(bytes[pos]))) ++pos;
            if (begins("<!--")) {
                pos+=4; while (pos+3<=length && !begins("-->")) ++pos;
                if (!begins("-->")) return fail();
                pos+=3;
            } else break;
        }
        return true;
    }
    struct Tag { char name[16]{}; bool close=false,empty=false,attrs=false; };
    bool tag(Tag& t) {
        if (!skip() || pos>=length || bytes[pos++]!='<') return fail();
        if (pos<length && bytes[pos]=='/') { t.close=true; ++pos; }
        unsigned k=0;
        while (pos<length && ((bytes[pos]>='a'&&bytes[pos]<='z')||(bytes[pos]>='A'&&bytes[pos]<='Z'))) {
            if (k+1>=sizeof(t.name)) return fail();
            t.name[k++]=char(bytes[pos++]);
        }
        if (!k) return fail();
        bool spaced=false;
        while (pos<length && ws(char(bytes[pos]))) { ++pos; spaced=true; }
        if (pos<length && bytes[pos]!='/' && bytes[pos]!='>') {
            if (!spaced || t.close || std::strcmp(t.name,"plist")) return fail();
            t.attrs=true;
            // Only the standard plist version attribute is accepted.
            if (!begins("version")) return fail();
            pos+=7;
            while (pos<length && ws(char(bytes[pos]))) ++pos;
            if (pos>=length || bytes[pos++]!='=') return fail();
            while (pos<length && ws(char(bytes[pos]))) ++pos;
            if (pos>=length || (bytes[pos]!='\'' && bytes[pos]!='"')) return fail();
            const auto quote=bytes[pos++]; if (!begins("1.0")) return fail(); pos+=3;
            if (pos>=length || bytes[pos++]!=quote) return fail();
            while (pos<length && ws(char(bytes[pos]))) ++pos;
        }
        if (pos<length && bytes[pos]=='/') { if (t.close) return fail(); t.empty=true; ++pos; }
        if (pos>=length || bytes[pos++]!='>') return fail();
        return true;
    }
    bool end_tag(const char* name) { Tag t; return tag(t) && t.close && !std::strcmp(t.name,name) ? true : fail(); }
    bool xml_text(std::uint32_t id,bool empty) {
        const auto start=chars;
        while (!empty && pos<length && bytes[pos]!='<') {
            const auto c=char(bytes[pos++]);
            if (c!='&') { if (!append(c)) return false; continue; }
            const auto begin=pos; while (pos<length && bytes[pos]!=';' && pos-begin<16) ++pos;
            if (pos>=length || bytes[pos]!=';') return fail();
            const std::string_view entity(reinterpret_cast<const char*>(bytes+begin),pos-begin); ++pos;
            std::uint32_t value=0;
            if (entity=="amp") value='&'; else if(entity=="lt") value='<'; else if(entity=="gt") value='>';
            else if(entity=="apos") value='\''; else if(entity=="quot") value='"';
            else if(entity.size()>1 && entity[0]=='#') {
                unsigned base=10; std::size_t j=1;
                if (j<entity.size() && entity[j]=='x') { base=16; ++j; }
                if (j==entity.size()) return fail();
                for (;j<entity.size();++j) {
                    const auto v=entity[j]; unsigned d;
                    if (v>='0'&&v<='9') d=unsigned(v-'0');
                    else if(v>='a'&&v<='f') d=unsigned(v-'a'+10);
                    else if(v>='A'&&v<='F') d=unsigned(v-'A'+10);
                    else return fail();
                    if (d>=base || value>(0x10ffff-d)/base) return fail(Error::invalid_text);
                    value=value*base+d;
                }
            } else return fail();
            if (!codepoint(value)) return false;
        }
        return end_string(id,start);
    }
    bool number(std::uint32_t id,bool real) {
        auto s=text(id); while (!s.empty()&&ws(s.front())) s.remove_prefix(1);
        while (!s.empty()&&ws(s.back())) s.remove_suffix(1);
        if (s.empty()) return fail();
        if (!real) {
            if (s.front()=='+') s.remove_prefix(1);
            std::int64_t value=0; const auto r=std::from_chars(s.data(),s.data()+s.size(),value);
            if (r.ec!=std::errc{} || r.ptr!=s.data()+s.size()) return fail();
            w.nodes[id].kind=Kind::integer; w.nodes[id].integer=value; return true;
        }
        std::size_t i=0; bool negative=false;
        if (s[i]=='-'||s[i]=='+') { negative=s[i]=='-'; if (++i==s.size()) return fail(); }
        double value=0; unsigned digits=0; int exponent=0;
        while(i<s.size()&&s[i]>='0'&&s[i]<='9') { value=value*10+(s[i++]-'0'); ++digits; }
        if(i<s.size()&&s[i]=='.') { ++i; while(i<s.size()&&s[i]>='0'&&s[i]<='9') { value=value*10+(s[i++]-'0'); --exponent; ++digits; } }
        if (!digits) return fail();
        if (i<s.size()&&(s[i]=='e'||s[i]=='E')) {
            ++i; bool neg=false; if(i<s.size()&&(s[i]=='+'||s[i]=='-')) neg=s[i++]=='-';
            const auto begin=i; int exp=0;
            while(i<s.size()&&s[i]>='0'&&s[i]<='9') { if(exp>10000) return fail(); exp=exp*10+(s[i++]-'0'); }
            if(i==begin) return fail();
            exponent+=neg?-exp:exp;
        }
        if(i!=s.size()) return fail();
        value*=std::pow(10.0,exponent); if(negative) value=-value;
        if(!std::isfinite(value)) return fail();
        w.nodes[id].kind=Kind::real; w.nodes[id].real=value; return true;
    }
    std::uint32_t xml(std::uint32_t depth,bool allow_key=false) {
        if (depth>=limits.max_depth || depth>=64) { fail(Error::too_deep); return none; }
        Tag t; if(!tag(t)||t.close||(!allow_key&&!std::strcmp(t.name,"key"))) { fail(); return none; }
        const bool array=!std::strcmp(t.name,"array"),dict=!std::strcmp(t.name,"dict");
        if (array||dict) {
            const auto id=node(array?Kind::array:Kind::dict); if(id==none) return none;
            if(t.empty) return id;
            auto last=none;
            for (;;) {
                if(!skip()) return none;
                if(begins("</")) { if(!end_tag(t.name)) return none; break; }
                if(dict) {
                    const auto saved=pos; Tag key; if(!tag(key)||key.close||std::strcmp(key.name,"key")) { fail(); return none; } pos=saved;
                }
                const auto child=xml(depth+1,dict); if(child==none) return none;
                if(dict&&!check_key(id,child)) return none;
                attach(id,child,last);
                if(dict) { const auto v=xml(depth+1); if(v==none) return none; attach(id,v,last); }
            }
            if(dict) w.nodes[id].count/=2;
            return id;
        }
        const bool string=!std::strcmp(t.name,"string")||!std::strcmp(t.name,"key");
        const bool integer=!std::strcmp(t.name,"integer"),real=!std::strcmp(t.name,"real");
        const bool yes=!std::strcmp(t.name,"true"),no=!std::strcmp(t.name,"false");
        if(yes||no) { if(!t.empty&&!end_tag(t.name)) return none; const auto id=node(Kind::boolean); if(id!=none) w.nodes[id].boolean=yes; return id; }
        if(!string&&!integer&&!real) { fail(Error::unsupported); return none; }
        const auto id=node(Kind::string); if(id==none) return none;
        if(!xml_text(id,t.empty)||(!t.empty&&!end_tag(t.name))) return none;
        if((integer||real)&&!number(id,real)) return none;
        return id;
    }
    std::uint32_t run() {
        if(length>=8&&!std::memcmp(bytes,"bplist00",8)) {
            if(length<40) { fail(); return none; }
            const auto trailer=length-32; offset_width=bytes[trailer+6]; ref_width=bytes[trailer+7];
            if(!integer_at(trailer+8,8,length,object_count)||!integer_at(trailer+16,8,length,top)||!integer_at(trailer+24,8,length,table)) return none;
            if(!object_count || object_count>limits.max_binary_objects || top>=object_count || !offset_width || offset_width>8 || !ref_width || ref_width>8 || table<8 || table>trailer || object_count>(trailer-table)/offset_width) { fail(); return none; }
            return binary(top,0);
        }
        if(length>=3 && bytes[0]==0xef && bytes[1]==0xbb && bytes[2]==0xbf) pos=3;
        if(!skip()) return none;
        if(begins("<?xml")) { pos+=5; while(pos+2<=length&&!begins("?>")) ++pos; if(!begins("?>")) { fail(); return none; } pos+=2; }
        if(!skip()) return none;
        if(begins("<!DOCTYPE")) {
            pos+=9; char quote=0;
            while(pos<length) { const auto c=char(bytes[pos++]); if(!quote&&(c=='['||c=='<')) { fail(Error::unsupported); return none; }
                if(quote) { if(c==quote) quote=0; } else if(c=='\''||c=='"') quote=c; else if(c=='>') break; }
            if(pos==length && bytes[pos-1]!='>') { fail(); return none; }
        }
        Tag root; if(!tag(root)||root.close||root.empty||std::strcmp(root.name,"plist")) { fail(); return none; }
        const auto id=xml(0); if(id==none||!end_tag("plist")||!skip()||pos!=length) { fail(); return none; }
        return id;
    }
};
} // namespace
Error parse(const std::uint8_t* b,std::size_t n,Workspace w,Document& out,Limits limits) {
    out={};
    if(!b||!n||!w.nodes||!w.node_capacity||!w.text||!w.text_capacity||!limits.max_depth||limits.max_depth>64 || w.text_capacity>UINT32_MAX || w.node_capacity>=none) return Error::invalid_argument;
    if(n>limits.max_input) return Error::too_large;
    Parser p{b,n,w,limits}; const auto root=p.run();
    if(p.error!=Error::ok) return p.error;
    if(root==none) return Error::malformed;
    out={w.nodes,p.used,w.text,p.chars,root}; return Error::ok;
}
const char* error_name(Error e) {
    switch(e) {
    case Error::ok:return "ok";case Error::invalid_argument:return "invalid_argument";
    case Error::malformed:return "malformed";case Error::unsupported:return "unsupported";
    case Error::too_large:return "too_large";case Error::too_deep:return "too_deep";
    case Error::no_space:return "no_space";case Error::cycle:return "cycle";
    case Error::duplicate_key:return "duplicate_key";case Error::invalid_text:return "invalid_text";
    } return "unknown";
}
} // namespace hcc::plist
