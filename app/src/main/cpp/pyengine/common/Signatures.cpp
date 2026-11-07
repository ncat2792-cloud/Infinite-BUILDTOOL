#include "Signatures.h"
#include "PyEngineAddress.h"
#include <algorithm>
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#include <atomic>
#include <cstring>
#include <array>
namespace script_host {
namespace {
using arm64_signature::Signature;
enum class Kind{Direct,Branch,Data,Verified};
struct Bytes{const uint8_t* data=nullptr;size_t size=0;};
struct Rule{uintptr_t key;Kind kind;Signature first,second,body;size_t branch,a,b,c,d,size,align;bool writable;Bytes bytes;};
struct Binding{uintptr_t key;uintptr_t* field;};
#include "Signatures.inc"
std::atomic<uintptr_t> published{0};
std::vector<uintptr_t> members;
std::vector<arm64_signature::CodeRange> codeRanges;
const uint8_t* at(const std::vector<arm64_signature::CodeRange>& ranges,uintptr_t address,size_t size){
    for(const auto& r:ranges)if(address>=r.address && address-r.address<=r.size && size<=r.size-(address-r.address))return r.data+address-r.address;
    return nullptr;
}
const uint8_t* readable(const Image& i,uintptr_t a,size_t n){const auto* p=at(i.code,a,n);return p?p:at(i.data,a,n);}
bool matches(const Image& i,uintptr_t a,const Signature& s){
    if(!s.count)return true;
    const auto* p=at(i.code,a,s.count*4);if(!p)return false;
    for(size_t n=0;n<s.count;++n){uint32_t w;std::memcpy(&w,p+n*4,4);if((w&s.instructions[n].mask)!=s.instructions[n].value)return false;}
    return true;
}
bool reference(const Image& i,uintptr_t owner,size_t a,size_t b,uintptr_t* result){
    const auto* p=at(i.code,owner+a*4,4);const auto* q=at(i.code,owner+b*4,4);if(!p||!q)return false;
    uint32_t x,y;std::memcpy(&x,p,4);std::memcpy(&y,q,4);
    return arm64_signature::DecodeAdrpReference(owner+a*4,x,y,result);
}
}
bool Resolve(const Image& i,std::string* error){
    if(published.load())return published.load()==i.base;
    static constexpr uint8_t expected[]={0xa4,0x94,0xde,0x85,0x05,0x9e,0x92,0xab,0xc0,0x1d,0x63,0x43,0x03,0x92,0x39,0x46,0x56,0x1a,0x8a,0x8b};
    if(!i.base || i.buildId.size()!=sizeof(expected) || std::memcmp(expected,i.buildId.data(),sizeof(expected))){*error="仅支持已验证的 3.10.5 ARM64";return false;}
    std::vector<Signature> patterns;for(const auto& r:rules){patterns.push_back(r.first);if(r.second.count)patterns.push_back(r.second);}
    arm64_signature::ScanIndex index;const bool indexed=index.Build(i.code.data(),i.code.size(),patterns.data(),patterns.size());
    std::vector<std::pair<uintptr_t,uintptr_t>> resolved;
    for(const auto& r:rules){
        const auto hit=arm64_signature::Scan(i.code.data(),i.code.size(),r.first,indexed?&index:nullptr);
        uintptr_t value=hit.address;
        bool valid=hit.status==arm64_signature::ScanStatus::Unique;
        if(valid && r.kind==Kind::Branch){
            const auto* p=at(i.code,value+r.branch*4,4);uint32_t w=0;if(p)std::memcpy(&w,p,4);
            const int64_t delta=int64_t(int32_t(w<<6)>>6)*4;
            valid=p && (w&0xfc000000U)==0x94000000U;
            value=uintptr_t(int64_t(value+r.branch*4)+delta);
        }
        if(valid && (r.kind==Kind::Data||r.kind==Kind::Verified)){
            valid=reference(i,hit.address,r.a,r.c,&value);
            if(valid && r.second.count){const auto other=arm64_signature::Scan(i.code.data(),i.code.size(),r.second,indexed?&index:nullptr);uintptr_t second=0;
                valid=other.status==arm64_signature::ScanStatus::Unique && reference(i,other.address,r.b,r.d,&second) && second==value;}
            valid=valid && value && !(value&(r.align-1)) && (r.writable?at(i.writable,value,r.size):readable(i,value,r.size));
            if(valid && r.bytes.size){const auto* p=readable(i,value,r.bytes.size);valid=p&&!std::memcmp(p,r.bytes.data,r.bytes.size);}
        }else valid=valid && !(value&3U) && at(i.code,value,4) && matches(i,value,r.body);
        if(!valid){*error="特征识别失败，key="+std::to_string(r.key);return false;}
        resolved.emplace_back(r.key,value);
    }
    std::vector<uintptr_t> accepted;
    for(const auto& b:bindings){auto it=std::find_if(resolved.begin(),resolved.end(),[&](auto p){return p.first==b.key;});if(it==resolved.end())return false;*b.field=it->second-i.base;accepted.push_back(it->second);}
    std::sort(accepted.begin(),accepted.end());members=std::move(accepted);codeRanges=i.code;
    published.store(i.base,std::memory_order_release);return true;
}
bool Ready() noexcept{return published.load(std::memory_order_acquire)!=0;}
uintptr_t Base() noexcept{return published.load(std::memory_order_acquire);}
bool CodeOffset(uintptr_t base,uintptr_t offset,size_t size,uintptr_t* result){
    if(!base||base!=Base()||offset>UINTPTR_MAX-base||!at(codeRanges,base+offset,size))return false;*result=base+offset;return true;
}
bool Member(uintptr_t base,uintptr_t address) noexcept{return base==Base()&&std::binary_search(members.begin(),members.end(),address);}
// Safe read of a possibly-invalid address. The bytes are pushed through a pipe
// instead of being dereferenced directly, so a bad pointer fails the call
// rather than faulting the game process.
bool Read(uintptr_t address,void* output,size_t size) noexcept{
    if(!address||!output||!size||size>4096||size>UINTPTR_MAX-address)return false;
    struct Channel{int fd[2]{-1,-1};~Channel(){reset();}void reset(){for(int& f:fd){if(f>=0)::close(f);f=-1;}}};
    static thread_local Channel channel;
    if(channel.fd[0]<0 && pipe2(channel.fd,O_CLOEXEC|O_NONBLOCK))return false;
    ssize_t written;
    do{written=write(channel.fd[1],reinterpret_cast<const void*>(address),size);}while(written<0&&errno==EINTR);
    if(written!=ssize_t(size)){channel.reset();return false;}
    ssize_t received;
    do{received=read(channel.fd[0],output,size);}while(received<0&&errno==EINTR);
    if(received!=ssize_t(size)){channel.reset();return false;}
    return true;
}
}
bool PyEngineAddress::ReadyFor(uintptr_t b) noexcept{return b&&b==script_host::Base();}
bool PyEngineAddress::IsResolvedAddress(uintptr_t b,uintptr_t a) noexcept{return script_host::Member(b,a);}
