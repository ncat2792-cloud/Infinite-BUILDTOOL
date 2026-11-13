#include <jni.h>
#include "pyengine/PyEngineBridge.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <link.h>
#include <mutex>
#include <thread>
#include <unistd.h>

#include "main.h"
#include "log_control.h"
#include "tp/TpModule.h"
#include "tp/BuildPacketReceiveHook.h"
#include "tp/MinecraftUpdateHook.h"
#include "tp/LoopbackPacketSenderCapture.h"
#include "build_import/BuildProjectionRenderer.h"
#include "build_import/MapAnvilUiCloseBridge.h"
#include "build_import/MapChestUiCloseBridge.h"

#define LOG_TAG "BuildToolsNative"

std::atomic<uintptr_t> Main::baseAddress{0};

namespace {
struct MinecraftExecutableAddressQuery {
    uintptr_t module_base = 0;
    uintptr_t address = 0;
    size_t required_size = 0;
    bool module_found = false;
    bool executable = false;
};

bool isMinecraftModulePath(const char* path) {
    if (!path) return false;
    const char* file_name = std::strrchr(path, '/');
    file_name = file_name ? file_name + 1 : path;
    return std::strcmp(file_name, "libminecraftpe.so") == 0 ||
           std::strcmp(file_name, "libminecraftpe.so (deleted)") == 0;
}

int findMinecraftExecutableAddress(dl_phdr_info* info, size_t, void* opaque) {
    auto* query = static_cast<MinecraftExecutableAddressQuery*>(opaque);
    if (!query || static_cast<uintptr_t>(info->dlpi_addr) != query->module_base ||
        !isMinecraftModulePath(info->dlpi_name)) {
        return 0;
    }
    query->module_found = true;
    for (ElfW(Half) index = 0; index < info->dlpi_phnum; ++index) {
        const ElfW(Phdr)& header = info->dlpi_phdr[index];
        if (header.p_type != PT_LOAD || (header.p_flags & PF_X) == 0) continue;
        if (query->module_base > std::numeric_limits<uintptr_t>::max() -
                                     static_cast<uintptr_t>(header.p_vaddr)) {
            continue;
        }
        const uintptr_t start = query->module_base + static_cast<uintptr_t>(header.p_vaddr);
        if (start > std::numeric_limits<uintptr_t>::max() -
                        static_cast<uintptr_t>(header.p_memsz)) {
            continue;
        }
        const uintptr_t end = start + static_cast<uintptr_t>(header.p_memsz);
        if (query->address >= start && query->address <= end &&
            query->required_size <= static_cast<size_t>(end - query->address)) {
            query->executable = true;
            return 1;
        }
    }
    return 1;
}

std::atomic<bool> hook_initialization_requested{false};
std::atomic<bool> hooks_initialized{false};
std::mutex hook_initialization_mutex;

// 定义在下方；initializeHooksIfReady 需要它。
uintptr_t getModuleBase(const char* name);

void initializeHooksIfReady() {
    std::lock_guard<std::mutex> lock(hook_initialization_mutex);
    if (!hook_initialization_requested.load()) return;
    const uintptr_t base_address = Main::getBaseAddress();
    if (base_address == 0) return;

    // 第二道闸：即便 Main 里已经写过一个基址，这里再独立取一次。
    // LSPatch 重新打包后同一文件可能有多个映射，两个来源对不上时 getModuleBase
    // 返回 0；此时绝不能让下面任何一条 Dobby 安装继续跑。
    const uintptr_t verified_base = getModuleBase("libminecraftpe.so");
    if (verified_base == 0 || verified_base != base_address) {
        static bool logged = false;
        if (!logged) {
            logged = true;
            LOGE("base address not verified (cached=0x%lx verified=0x%lx); hooks skipped",
                 (unsigned long) base_address, (unsigned long) verified_base);
        }
        return;
    }

    if (!InitLoopbackPacketSenderCapture(base_address)) {
        LOGE("Command sender capture is not ready; retrying on next request");
    }
    if (!hooks_initialized.exchange(true)) {
        LOGI("Minecraft library ready; initializing building tool hooks");
    }
    if (!BuildPacketReceiveHook::isReceiveHookReady() && !BuildPacketReceiveHook::init(base_address)) {
        LOGE("Command acknowledgement hook is not ready");
    }
    if (!InitMinecraftUpdateHook(base_address)) {
        LOGE("Game tick hook is not ready");
    }
    if (!build_import::InitBuildProjectionHook(base_address)) {
        LOGE("Projection render hook is not ready");
    }
    // Resolve and verify the 3.10.5 Python bytecode ABI once the game image is
    // mapped. Buildings-tool features that execute Python (structure import,
    // projection printer, inventory transfer) are unavailable until this
    // succeeds, so it is attempted on every hook pass until verification holds.
    if (!py_engine::Install()) {
        LOGE("Python engine not ready: %s", py_engine::Status().c_str());
    }
}

// ── 模块基址 ────────────────────────────────────────────────────────
//
// 只用 /proc/self/maps，不用 dl_iterate_phdr。
//
// 原因：LSPatch 补丁后的进程里，dl_iterate_phdr 回调拿到的 dlpi_name 可能
// 指向临时映射，回调返回后字符串就失效了。之前基址正是从这种悬空指针里读
// 出来的，于是「基址 + RVA」算出的地址带着 0xff 高字节、甚至是奇数 ——
// 崩溃现场 lr/pc 全是 0xff 开头、最低位为 9，就是这条路的证据。
//
// /proc/self/maps 是内核给的文本快照，不依赖任何回调期指针，LSPatch 下稳定。
// 取每行起始地址里「属于该文件、且映射从文件偏移 0 开始」的那一条，
// 它才是映像基址；单纯字符串匹配会命中中间的段。

struct MapsImage {
    uintptr_t base = 0;
    bool found = false;
};

bool isMinecraftImagePath(const char* path) {
    if (!path) return false;
    const char* name = std::strrchr(path, '/');
    name = name ? name + 1 : path;
    return std::strcmp(name, "libminecraftpe.so") == 0 ||
           std::strcmp(name, "libminecraftpe.so (deleted)") == 0;
}

// 解析一行 maps：start-end perms offset dev inode path
bool parseMapsLine(const char* line, MapsImage* image) {
    if (!line || !image) return false;
    unsigned long long start = 0, end = 0, offset = 0;
    char perms[8] = {0};
    if (std::sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &offset) != 4) return false;
    // 只认「从文件头开始的映射」：那是 ELF 装载基址。
    if (offset != 0) return false;
    const char* path = std::strchr(line, '/');
    if (!isMinecraftImagePath(path)) return false;
    image->base = static_cast<uintptr_t>(start);
    image->found = true;
    return true;
}

uintptr_t getModuleBase(const char* /*name*/) {
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) {
        LOGE("cannot open /proc/self/maps");
        return 0;
    }
    char line[1024];
    uintptr_t base = 0;
    int candidates = 0;
    while (std::fgets(line, sizeof(line), maps)) {
        MapsImage image;
        if (!parseMapsLine(line, &image)) continue;
        ++candidates;
        if (base == 0) base = image.base;
    }
    std::fclose(maps);
    if (base == 0) {
        LOGE("libminecraftpe.so is not mapped yet");
        return 0;
    }
    if (candidates > 1) {
        // 同一映像被映射两次时无法判断哪个是链接器在用的，宁可不装 hook。
        LOGE("libminecraftpe.so has %d file-offset-0 mappings; refusing to guess base",
             candidates);
        return 0;
    }
    LOGI("module base resolved from maps: 0x%lx", (unsigned long)base);
    return base;
}

// 判断 [address, address+size) 是否整段落在某个可执行映射里。
// 用 /proc/self/maps，不依赖链接器回调。
bool isExecutableMapping(uintptr_t address, size_t size) {
    if (address == 0 || size == 0) return false;
    FILE* maps = std::fopen("/proc/self/maps", "r");
    if (!maps) return false;
    char line[1024];
    bool ok = false;
    while (std::fgets(line, sizeof(line), maps)) {
        unsigned long long start = 0, end = 0, offset = 0;
        char perms[8] = {0};
        if (std::sscanf(line, "%llx-%llx %7s %llx", &start, &end, perms, &offset) != 4) continue;
        if (perms[2] != 'x') continue;  // 第 3 位是 'x' 才是可执行
        if (address < start) continue;
        if (address + size > end) continue;
        ok = true;
        break;
    }
    std::fclose(maps);
    return ok;
}

void waitForMinecraft() {
    for (int attempt = 0; attempt < 100; ++attempt) {
        const uintptr_t base = getModuleBase("libminecraftpe.so");
        if (base != 0) {
            Main::setBaseAddress(base);
            initializeHooksIfReady();
            return;
        }
        usleep(500000);
    }
    LOGE("Minecraft library was not found");
}

void ensureBuildToolsHooks(JNIEnv*, jclass) {
    if (Main::getBaseAddress() == 0) {
        Main::setBaseAddress(getModuleBase("libminecraftpe.so"));
    }
    hook_initialization_requested.store(true);
    initializeHooksIfReady();
}

jlong takePendingMapAnvilUiCloseRequest(JNIEnv*, jclass) {
    return static_cast<jlong>(build_import::TakePendingMapAnvilUiCloseRequest());
}
jboolean isMapAnvilUiCloseStillSafe(JNIEnv*, jclass, jlong ticket) {
    return build_import::IsMapAnvilUiCloseStillSafe(static_cast<uint64_t>(ticket))
        ? JNI_TRUE : JNI_FALSE;
}
void markMapAnvilUiCloseDispatched(JNIEnv*, jclass, jlong ticket) {
    build_import::MarkMapAnvilUiCloseDispatched(static_cast<uint64_t>(ticket));
}
jlong takePendingMapChestUiCloseRequest(JNIEnv*, jclass) {
    return static_cast<jlong>(build_import::TakePendingMapChestUiCloseRequest());
}
jboolean isMapChestUiCloseStillSafe(JNIEnv*, jclass, jlong ticket) {
    return build_import::IsMapChestUiCloseStillSafe(static_cast<uint64_t>(ticket))
        ? JNI_TRUE : JNI_FALSE;
}
void markMapChestUiCloseDispatched(JNIEnv*, jclass, jlong ticket) {
    build_import::MarkMapChestUiCloseDispatched(static_cast<uint64_t>(ticket));
}

const JNINativeMethod native_methods[] = {
    {"ensureBuildToolsHooks", "()V", reinterpret_cast<void*>(ensureBuildToolsHooks)},
    {"takePendingMapAnvilUiCloseRequest", "()J",
     reinterpret_cast<void*>(takePendingMapAnvilUiCloseRequest)},
    {"isMapAnvilUiCloseStillSafe", "(J)Z",
     reinterpret_cast<void*>(isMapAnvilUiCloseStillSafe)},
    {"markMapAnvilUiCloseDispatched", "(J)V",
     reinterpret_cast<void*>(markMapAnvilUiCloseDispatched)},
    {"takePendingMapChestUiCloseRequest", "()J",
     reinterpret_cast<void*>(takePendingMapChestUiCloseRequest)},
    {"isMapChestUiCloseStillSafe", "(J)Z",
     reinterpret_cast<void*>(isMapChestUiCloseStillSafe)},
    {"markMapChestUiCloseDispatched", "(J)V",
     reinterpret_cast<void*>(markMapChestUiCloseDispatched)},
};
}  // namespace

bool ResolveMinecraftExecutableOffset(uintptr_t base_address, uintptr_t offset,
                                      size_t required_size, uintptr_t* address) {
    if (!address) return false;
    *address = 0;
    if (!base_address || !offset || !required_size ||
        base_address > std::numeric_limits<uintptr_t>::max() - offset) {
        return false;
    }
    const uintptr_t target = base_address + offset;
    if ((target & 0x3U) != 0) return false;

    // 也用 maps 判断可执行性，理由同 getModuleBase：dl_iterate_phdr 在
    // LSPatch 下的回调指针不可靠，而这里是每次装 hook 都必经的路径。
    if (!isExecutableMapping(target, required_size)) return false;
    *address = target;
    return true;
}

bool EnsureBuildProjectionHooksReady() {
    std::lock_guard<std::mutex> lock(hook_initialization_mutex);
    const uintptr_t base_address = Main::getBaseAddress();
    if (base_address == 0) return false;
    return InitMinecraftUpdateHook(base_address) &&
           build_import::InitBuildProjectionHook(base_address);
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    JNIEnv* env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6) != JNI_OK) {
        return JNI_ERR;
    }
    jclass clazz = env->FindClass("com/vdl/kong520/NativeCore");
    if (!clazz) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }
    const jint registered = env->RegisterNatives(
        clazz, native_methods, sizeof(native_methods) / sizeof(native_methods[0]));
    env->DeleteLocalRef(clazz);
    if (registered != JNI_OK) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        return JNI_ERR;
    }
    std::thread(waitForMinecraft).detach();
    return JNI_VERSION_1_6;
}
