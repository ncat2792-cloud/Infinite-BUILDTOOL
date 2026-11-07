#include "ScriptRuntime.h"
#include "PythonScriptAbi3105.h"
#include "python/CompilerEntry.h"
#include "python/CompilerGameObjects.h"
#include "common/Signatures.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>

namespace local_python {
namespace {
using Clock=std::chrono::steady_clock;
constexpr char kRoot[]="/storage/emulated/0/Android/data/com.netease.x19/files/netease";
constexpr size_t kMaxSource=4U*1024U*1024U;
constexpr size_t kMaxMarshal=16U*1024U*1024U;
enum class Phase {Idle,WaitingWorld,Compiling,WaitingExecution,Running,Succeeded,Failed};
std::atomic<uint64_t> epoch{1};
std::atomic<bool> watching{false};

struct Api {
    abi::Targets addresses{};
    using Object=void;
    int ensure() const {return reinterpret_cast<int(*)()>(addresses.gilEnsure)();}
    void release(int state) const {reinterpret_cast<void(*)(int)>(addresses.gilRelease)(state);}
    void decref(Object* object) const {if(object)reinterpret_cast<void(*)(Object*)>(addresses.decRef)(object);}
    PyLoaderPythonConstructors constructors() const {
        return {const_cast<Api*>(this),
            [](void* c,const char* p,size_t n)->Object* {
                return reinterpret_cast<Object*(*)(const char*,intptr_t)>(static_cast<Api*>(c)->addresses.stringNew)(p,intptr_t(n));
            },
            [](void* c,const char* p,size_t n)->Object* {
                return reinterpret_cast<Object*(*)(const char*,intptr_t,const char*)>(static_cast<Api*>(c)->addresses.unicodeNew)(p,intptr_t(n),nullptr);
            },
            [](void* c,int64_t value)->Object* {
                return reinterpret_cast<Object*(*)(int64_t)>(static_cast<Api*>(c)->addresses.intNew)(value);
            },
            [](void* c,const char* value)->Object* {
                return reinterpret_cast<Object*(*)(const char*,char**,int)>(static_cast<Api*>(c)->addresses.longNew)(value,nullptr,10);
            },
            [](void* c,double value)->Object* {
                return reinterpret_cast<Object*(*)(double)>(static_cast<Api*>(c)->addresses.floatNew)(value);
            },
            [](void* c,int tag)->Object* {
                const auto& a=static_cast<Api*>(c)->addresses;
                const char* names[]={"None","False","True","Ellipsis"};
                Object* module=reinterpret_cast<Object*(*)(const char*)>(a.addModule)("__builtin__");
                return module && tag>=0 && tag<4 ? reinterpret_cast<Object*(*)(Object*,const char*)>(a.getAttr)(module,names[tag]) : nullptr;
            },
            [](void* c,size_t n)->Object* {
                return reinterpret_cast<Object*(*)(intptr_t)>(static_cast<Api*>(c)->addresses.tupleNew)(intptr_t(n));
            },
            [](void* c,Object* tuple,size_t index,Object* value)->int {
                return reinterpret_cast<int(*)(Object*,intptr_t,Object*)>(static_cast<Api*>(c)->addresses.tupleSet)(tuple,intptr_t(index),value);
            },
            [](void* c,Object* values)->Object* {
                return reinterpret_cast<Object*(*)(Object*)>(static_cast<Api*>(c)->addresses.frozenNew)(values);
            },
            [](void* c,const PyLoaderPythonCodeFields* f)->Object* {
                using NewCode=Object*(*)(int,int,int,int,Object*,Object*,Object*,Object*,Object*,Object*,Object*,Object*,int,Object*);
                return reinterpret_cast<NewCode>(static_cast<Api*>(c)->addresses.codeNew)(
                    f->argcount,f->nlocals,f->stacksize,f->flags,f->code,f->consts,f->names,f->varnames,
                    f->freevars,f->cellvars,f->filename,f->name,f->firstlineno,f->lnotab);
            },
            [](void* c,Object* object){static_cast<Api*>(c)->decref(object);}
        };
    }
    Object* load(const std::string& bytes) const {
        const auto api=constructors();
        char* rawError=nullptr;
        Object* result=PyLoaderBuildPython3105(bytes.data(),bytes.size(),&api,&rawError);
        std::unique_ptr<char,decltype(&free)> error(rawError,&free);
        if(!result && error)throw std::runtime_error(error.get());
        return result;
    }
    Object* module() const {return reinterpret_cast<Object*(*)(const char*)>(addresses.addModule)("__main__");}
    Object* dict(Object* module) const {return reinterpret_cast<Object*(*)(Object*)>(addresses.moduleDict)(module);}
    Object* eval(Object* code,Object* globals) const {
        return reinterpret_cast<Object*(*)(Object*,Object*,Object*)>(addresses.evalCode)(code,globals,globals);
    }
    void error(char* output,size_t capacity,const char* stage) const {
        Object *type=nullptr,*value=nullptr,*trace=nullptr;
        reinterpret_cast<void(*)(Object**,Object**,Object**)>(addresses.errorFetch)(&type,&value,&trace);
        if(type)reinterpret_cast<void(*)(Object**,Object**,Object**)>(addresses.errorNormalize)(&type,&value,&trace);
        Object* text=value ? reinterpret_cast<Object*(*)(Object*)>(addresses.objectStr)(value) : nullptr;
        const char* bytes=text ? reinterpret_cast<const char*(*)(Object*)>(addresses.stringData)(text) : nullptr;
        if(!bytes)reinterpret_cast<void(*)()>(addresses.errorClear)();
        Object* typeName=type ? reinterpret_cast<Object*(*)(Object*,const char*)>(addresses.getAttr)(type,"__name__") : nullptr;
        const char* name=typeName ? reinterpret_cast<const char*(*)(Object*)>(addresses.stringData)(typeName) : nullptr;
        std::snprintf(output,capacity,"%s: %s%s%s",stage,name?name:"",
            name&&bytes?": ":"",bytes?bytes:(name?"":"native API returned NULL without a Python exception"));
        decref(typeName);decref(text);decref(type);decref(value);decref(trace);
        reinterpret_cast<void(*)()>(addresses.errorClear)();
    }
    bool ready() const {
        uint8_t initialized=0;
        return script_host::Read(addresses.initialized,&initialized,1) && initialized==1;
    }
    bool initialize() {
        if(addresses.codeNew) return true;
        const uintptr_t base=script_host::Base();
        if(!PyEngineAddress::ReadyFor(base)) return false;
        const auto code=[base](uintptr_t offset,uintptr_t* address) {
            if(!offset || offset>UINTPTR_MAX-base || !PyEngineAddress::IsResolvedAddress(base,base+offset))return false;
            return script_host::CodeOffset(base,offset,4,address);
        };
        const auto data=[base](uintptr_t offset,size_t size,uintptr_t* address) {
            if(!offset || offset>UINTPTR_MAX-base || size>UINTPTR_MAX-(base+offset) ||
               !PyEngineAddress::IsResolvedAddress(base,base+offset))return false;
            *address=base+offset;return true;
        };
        return abi::Verify(base,&addresses,code,data,[](uintptr_t address,void* output,size_t size) {
            return script_host::Read(address,output,size);
        });
    }
};

struct GilScope {
    const Api& api;
    int token;
    explicit GilScope(const Api& value):api(value),token(api.ensure()){}
    ~GilScope(){api.release(token);}
};

struct Job {
    uint64_t epoch=0;
    uintptr_t actor=0,dimension=0;
    bool context=false;
    Api::Object *setup=nullptr,*code=nullptr;
    std::atomic<bool> done{false};
    bool success=false;
    std::array<char,4096> error{};
};
struct State {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread worker;
    bool stopping=false,active=false,pending=false;
    Phase phase=Phase::Idle;
    uint64_t serial=0;
    uint64_t queuedEpoch=0;
    std::string path,name,reason,detail;
    Job* prepared=nullptr; // borrowed only until its matching done receipt
    Job* waiting=nullptr;
    Api api;
    ~State();
};
State& state(){static State value;return value;}

std::string quote(const std::string& text) {
    static constexpr char hex[]="0123456789abcdef";
    std::string result="\"";
    for(unsigned char c:text){
        if(c=='\"'||c=='\\'){result+='\\';result+=char(c);}
        else if(c<32){result+="\\u00";result+=hex[c>>4];result+=hex[c&15];}
        else result+=char(c);
    }
    result+='\"';return result;
}
std::string pythonLiteral(const std::string& text) {
    static constexpr char hex[]="0123456789abcdef";
    std::string result="'";
    for(unsigned char c:text){result+="\\x";result+=hex[c>>4];result+=hex[c&15];}
    result+='\'';return result;
}
std::string readSource(const std::string& path,std::string* canonical) {
    char* rawRoot=realpath(kRoot,nullptr);
    if(!rawRoot)throw std::runtime_error("script directory is unavailable");
    std::string root(rawRoot);free(rawRoot);
    char* rawPath=realpath(path.c_str(),nullptr);
    if(!rawPath)throw std::runtime_error("script file is unavailable");
    std::string resolved(rawPath);free(rawPath);
    if(resolved.size()<=root.size()+1 || resolved.compare(0,root.size()+1,root+"/") ||
       resolved.size()<3 || resolved[resolved.size()-3]!='.' ||
       (resolved[resolved.size()-2]!='p' && resolved[resolved.size()-2]!='P') ||
       (resolved.back()!='y' && resolved.back()!='Y'))
        throw std::runtime_error("select a .py file inside the netease script directory");
    const int fd=open(resolved.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
    if(fd<0)throw std::runtime_error("cannot open script");
    struct File {int fd;~File(){close(fd);}} file{fd};
    struct stat info{};
    if(fstat(fd,&info) || !S_ISREG(info.st_mode) || info.st_size<=0 || uint64_t(info.st_size)>kMaxSource)
        throw std::runtime_error("script must be a nonempty regular file of at most 4 MiB");
    std::string source(size_t(info.st_size),'\0');
    size_t position=0;
    while(position<source.size()){
        const ssize_t count=read(fd,source.data()+position,source.size()-position);
        if(count<0 && errno==EINTR)continue;
        if(count<=0)throw std::runtime_error("script changed or could not be read completely");
        position+=size_t(count);
    }
    if(source.find('\0')!=std::string::npos)throw std::runtime_error("script contains a NUL byte");
    *canonical=std::move(resolved);return source;
}
std::string compile(const std::string& source,const std::string& filename) {
    unsigned char* raw=nullptr;size_t length=0;char* error=nullptr;
    const bool ok=PyLoaderCompilePython3105(source.c_str(),filename.c_str(),&raw,&length,&error)!=0;
    std::unique_ptr<unsigned char,decltype(&free)> bytes(raw,&free);
    std::unique_ptr<char,decltype(&free)> message(error,&free);
    if(!ok)throw std::runtime_error(message ? message.get() : "Python compilation failed");
    if(!length || length>kMaxMarshal || raw[0]!='c')throw std::runtime_error("invalid compiled code payload");
    return std::string(reinterpret_cast<const char*>(raw),length);
}

bool current(const Job& job) {
    return epoch.load(std::memory_order_acquire)==job.epoch;
}
void finish(State& s,Phase phase,const std::string& reason,const std::string& detail) {
    std::lock_guard<std::mutex> lock(s.mutex);
    s.prepared=nullptr;s.waiting=nullptr;s.phase=phase;s.reason=reason;
    s.detail=detail.substr(0,4095);s.active=false;watching.store(false,std::memory_order_release);
}

void workerLoop() {
    State& s=state();
    for(;;){
        std::string path;
        Job job;
        {
            std::unique_lock<std::mutex> lock(s.mutex);
            s.wake.wait(lock,[&]{return s.stopping||s.pending;});
            if(s.stopping)break;
            s.pending=false;path=s.path;
            job.epoch=s.queuedEpoch;
            
            lock.unlock();
            const bool tickReady=script_host::Ready() && current(job) && script_host::EnsureTickObserver();
            lock.lock();
            if(!tickReady){
                lock.unlock();finish(s,Phase::Failed,"tick_unavailable","the game update observer is unavailable");continue;
            }
            s.waiting=&job;
            const auto deadline=Clock::now()+std::chrono::seconds(3);
            while(!s.stopping && !job.context && current(job) && Clock::now()<deadline)
                s.wake.wait_for(lock,std::chrono::milliseconds(100));
            s.waiting=nullptr;
            if(s.stopping)break;
            if(!job.context || !current(job)){
                lock.unlock();finish(s,Phase::Failed,"world_unavailable","enter the current world before loading a script");continue;
            }
            s.phase=Phase::Compiling;
        }
        try {
            if(!script_host::Ready() || !current(job))
                throw std::runtime_error("game hook is not initialized");
            if(!s.api.initialize())throw std::runtime_error("the reviewed 3.10.5 Python bytecode ABI is unavailable");
            std::string filename;
            const std::string source=readSource(path,&filename);
            const std::string directory=filename.substr(0,filename.rfind('/'));
            const std::string setup="__file__ = "+pythonLiteral(filename)+"\n__package__ = None\n"+
                "if "+pythonLiteral(directory)+" not in __import__('sys').path:\n"+
                " __import__('sys').path.insert(0, "+pythonLiteral(directory)+")\n";
            const std::string setupBytes=compile(setup,"<PyLoader script setup>");
            const std::string codeBytes=compile(source,filename);
            if(!current(job) || !script_host::Ready())
                throw std::runtime_error("script request was cancelled or world changed");
            if(!s.api.ready())throw std::runtime_error("the game Python interpreter is not initialized");
            {
                const GilScope gil(s.api);
                job.setup=s.api.load(setupBytes);
                if(job.setup)job.code=s.api.load(codeBytes);
                if(!job.setup||!job.code)s.api.error(job.error.data(),job.error.size(),job.setup?"construct_script":"construct_setup");
            }
            if(!job.setup||!job.code)throw std::runtime_error(job.error.data());
            {
                std::unique_lock<std::mutex> lock(s.mutex);
                if(s.stopping||!current(job))throw std::runtime_error("script request was cancelled");
                s.prepared=&job;s.phase=Phase::WaitingExecution;
                const auto deadline=Clock::now()+std::chrono::seconds(30);
                while(!job.done.load(std::memory_order_acquire)){
                    // Once the game borrows the job, its done receipt owns
                    // retirement. Never destroy code underneath an evaluation.
                    if(s.phase!=Phase::Running && (s.stopping||!current(job)||Clock::now()>=deadline)){
                        s.prepared=nullptr;throw std::runtime_error("script execution was cancelled or game ticks timed out");
                    }
                    s.wake.wait_for(lock,std::chrono::milliseconds(100));
                }
                s.prepared=nullptr;
            }
            {
                const GilScope gil(s.api);
                s.api.decref(job.setup);s.api.decref(job.code);job.setup=nullptr;job.code=nullptr;
            }
            finish(s,job.success?Phase::Succeeded:Phase::Failed,
                job.success?"":"runtime_error",job.success?"":job.error.data());
        } catch(const std::exception& error) {
            if(job.setup||job.code){
                const GilScope gil(s.api);s.api.decref(job.setup);s.api.decref(job.code);
            }
            finish(s,Phase::Failed,"script_failed",error.what());
        } catch(...) {
            if(job.setup||job.code){
                const GilScope gil(s.api);s.api.decref(job.setup);s.api.decref(job.code);
            }
            finish(s,Phase::Failed,"script_failed","unexpected native script error");
        }
    }
    PyLoaderFinalizePythonCompiler();
}
State::~State(){
    {std::lock_guard<std::mutex> lock(mutex);stopping=true;}
    wake.notify_all();if(worker.joinable())worker.join();
}
} // namespace

bool SubmitPath(const std::string& path) {
    State& s=state();
    std::lock_guard<std::mutex> lock(s.mutex);
    if(s.active||s.stopping)return false;
    if(!script_host::Operational()){
        s.phase=Phase::Failed;s.reason="not_ready";s.detail="game hook is not initialized";return false;
    }
    if(path.empty()||path.size()>4096||path.find('\0')!=std::string::npos)return false;
    if(!s.worker.joinable())s.worker=std::thread(workerLoop);
    s.path=path;s.name=path.substr(path.rfind('/')==std::string::npos?0:path.rfind('/')+1);
    s.reason.clear();s.detail.clear();s.phase=Phase::WaitingWorld;s.active=true;s.pending=true;++s.serial;
    s.queuedEpoch=epoch.load(std::memory_order_acquire);
    
    watching.store(true,std::memory_order_release);s.wake.notify_all();return true;
}
std::string StatusJson() {
    State& s=state();std::lock_guard<std::mutex> lock(s.mutex);
    return "{\"serial\":"+std::to_string(s.serial)+",\"phase\":"+std::to_string(int(s.phase))+
        ",\"active\":"+(s.active?"true":"false")+",\"name\":"+quote(s.name)+
        ",\"reason\":"+quote(s.reason)+",\"detail\":"+quote(s.detail)+"}";
}
void OnLocalPlayerTick(void* actor,uintptr_t dimension) noexcept {
    if(!watching.load(std::memory_order_acquire)||!actor||!dimension||
       !script_host::Operational())return;
    State& s=state();
    std::unique_lock<std::mutex> lock(s.mutex,std::try_to_lock);
    if(!lock.owns_lock())return;
    if(s.waiting && current(*s.waiting)){
        s.waiting->actor=reinterpret_cast<uintptr_t>(actor);s.waiting->dimension=dimension;s.waiting->context=true;
        s.wake.notify_all();
    }
    Job* job=s.prepared;
    if(!job || s.phase!=Phase::WaitingExecution || !current(*job) ||
       job->actor!=reinterpret_cast<uintptr_t>(actor)||job->dimension!=dimension)return;
    s.phase=Phase::Running;
    lock.unlock();
    try {
        const GilScope gil(s.api);
        auto* module=s.api.module();
        auto* globals=module?s.api.dict(module):nullptr;
        auto* result=globals?s.api.eval(job->setup,globals):nullptr;
        const char* stage=!module?"main_module":!globals?"module_globals":"setup_execution";
        if(result){s.api.decref(result);stage="script_execution";result=s.api.eval(job->code,globals);}
        job->success=result!=nullptr;
        if(!result)s.api.error(job->error.data(),job->error.size(),stage);
        s.api.decref(result);
    } catch(...) {
        job->success=false;
        constexpr char message[]="unexpected native Python evaluation error";
        std::memcpy(job->error.data(),message,sizeof(message));
    }
    job->done.store(true,std::memory_order_release);s.wake.notify_all();
}
void OnSessionBoundary() noexcept {
    epoch.fetch_add(1,std::memory_order_acq_rel);
    if(watching.load(std::memory_order_acquire))state().wake.notify_all();
}
std::string QuoteStatus(const std::string& value){return quote(value);}
void Shutdown() noexcept {
    watching.store(false,std::memory_order_release);
    State& s=state();
    {std::lock_guard<std::mutex> lock(s.mutex);s.stopping=true;}
    s.wake.notify_all();if(s.worker.joinable())s.worker.join();
}
} // namespace local_python
