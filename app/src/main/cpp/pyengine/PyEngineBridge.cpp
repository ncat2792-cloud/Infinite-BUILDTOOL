#include "PyEngineBridge.h"

#include "common/PyEngineAddress.h"
#include "common/Signatures.h"
#include "python/CompilerEntry.h"
#include "python/CompilerGameObjects.h"
#include "scripts/PythonScriptAbi3105.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <link.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace py_engine {
namespace {

using Object = void;

// The engine evaluates the compiled bytecode through the game's own
// PyEval_EvalCode. Reusing the verified ABI descriptor from the script runtime
// keeps a single definition of every address, so a client update only has to be
// re-audited in one place.
struct Api {
    local_python::abi::Targets addresses{};
    bool verified = false;

    int ensure() const { return reinterpret_cast<int (*)()>(addresses.gilEnsure)(); }
    void release(int state) const { reinterpret_cast<void (*)(int)>(addresses.gilRelease)(state); }
    void decref(Object* object) const {
        if (object) reinterpret_cast<void (*)(Object*)>(addresses.decRef)(object);
    }

    PyLoaderPythonConstructors constructors() const {
        return {const_cast<Api*>(this),
            [](void* c, const char* p, size_t n) -> Object* {
                return reinterpret_cast<Object* (*)(const char*, intptr_t)>(
                    static_cast<Api*>(c)->addresses.stringNew)(p, intptr_t(n));
            },
            [](void* c, const char* p, size_t n) -> Object* {
                return reinterpret_cast<Object* (*)(const char*, intptr_t, const char*)>(
                    static_cast<Api*>(c)->addresses.unicodeNew)(p, intptr_t(n), nullptr);
            },
            [](void* c, int64_t value) -> Object* {
                return reinterpret_cast<Object* (*)(int64_t)>(
                    static_cast<Api*>(c)->addresses.intNew)(value);
            },
            [](void* c, const char* value) -> Object* {
                return reinterpret_cast<Object* (*)(const char*, char**, int)>(
                    static_cast<Api*>(c)->addresses.longNew)(value, nullptr, 10);
            },
            [](void* c, double value) -> Object* {
                return reinterpret_cast<Object* (*)(double)>(
                    static_cast<Api*>(c)->addresses.floatNew)(value);
            },
            [](void* c, int tag) -> Object* {
                const auto& a = static_cast<Api*>(c)->addresses;
                const char* names[] = {"None", "False", "True", "Ellipsis"};
                Object* module = reinterpret_cast<Object* (*)(const char*)>(a.addModule)("__builtin__");
                return module && tag >= 0 && tag < 4
                    ? reinterpret_cast<Object* (*)(Object*, const char*)>(a.getAttr)(module, names[tag])
                    : nullptr;
            },
            [](void* c, size_t n) -> Object* {
                return reinterpret_cast<Object* (*)(intptr_t)>(
                    static_cast<Api*>(c)->addresses.tupleNew)(intptr_t(n));
            },
            [](void* c, Object* tuple, size_t index, Object* value) -> int {
                return reinterpret_cast<int (*)(Object*, intptr_t, Object*)>(
                    static_cast<Api*>(c)->addresses.tupleSet)(tuple, intptr_t(index), value);
            },
            [](void* c, Object* values) -> Object* {
                return reinterpret_cast<Object* (*)(Object*)>(
                    static_cast<Api*>(c)->addresses.frozenNew)(values);
            },
            [](void* c, const PyLoaderPythonCodeFields* f) -> Object* {
                using NewCode = Object* (*)(int, int, int, int, Object*, Object*, Object*, Object*,
                                            Object*, Object*, Object*, Object*, int, Object*);
                return reinterpret_cast<NewCode>(static_cast<Api*>(c)->addresses.codeNew)(
                    f->argcount, f->nlocals, f->stacksize, f->flags, f->code, f->consts, f->names,
                    f->varnames, f->freevars, f->cellvars, f->filename, f->name, f->firstlineno,
                    f->lnotab);
            },
            [](void* c, Object* object) { static_cast<Api*>(c)->decref(object); }};
    }

    Object* load(const std::string& bytes) const {
        const auto ctor = constructors();
        char* raw_error = nullptr;
        Object* result = PyLoaderBuildPython3105(bytes.data(), bytes.size(), &ctor, &raw_error);
        std::unique_ptr<char, decltype(&free)> error(raw_error, &free);
        if (!result) {
            throw std::runtime_error(error ? error.get() : "bytecode construction failed");
        }
        return result;
    }

    Object* module() const {
        return reinterpret_cast<Object* (*)(const char*)>(addresses.addModule)("__main__");
    }
    Object* dict(Object* module) const {
        return module ? reinterpret_cast<Object* (*)(Object*)>(addresses.moduleDict)(module) : nullptr;
    }
    Object* eval(Object* code, Object* globals) const {
        return reinterpret_cast<Object* (*)(Object*, Object*, Object*)>(
            addresses.evalCode)(code, globals, globals);
    }
    // Converts the pending Python exception into readable text and clears it.
    void describeError(char* output, size_t capacity, const char* stage) const {
        Object *type = nullptr, *value = nullptr, *trace = nullptr;
        reinterpret_cast<void (*)(Object**, Object**, Object**)>(addresses.errorFetch)(
            &type, &value, &trace);
        if (type) {
            reinterpret_cast<void (*)(Object**, Object**, Object**)>(addresses.errorNormalize)(
                &type, &value, &trace);
        }
        Object* text = value
            ? reinterpret_cast<Object* (*)(Object*)>(addresses.objectStr)(value) : nullptr;
        const char* bytes = text
            ? reinterpret_cast<const char* (*)(Object*)>(addresses.stringData)(text) : nullptr;
        Object* type_name = type
            ? reinterpret_cast<Object* (*)(Object*, const char*)>(addresses.getAttr)(type, "__name__")
            : nullptr;
        const char* name = type_name
            ? reinterpret_cast<const char* (*)(Object*)>(addresses.stringData)(type_name) : nullptr;
        std::snprintf(output, capacity, "%s: %s%s%s", stage, name ? name : "",
                      (name && bytes) ? ": " : "",
                      bytes ? bytes : (name ? "" : "Python API returned NULL without an exception"));
        decref(type_name);
        decref(text);
        decref(type);
        decref(value);
        decref(trace);
        reinterpret_cast<void (*)()>(addresses.errorClear)();
    }
    bool interpreterReady() const {
        uint8_t initialized = 0;
        return script_host::Read(addresses.initialized, &initialized, 1) && initialized == 1;
    }
};

struct GilScope {
    const Api& api;
    int token;
    explicit GilScope(const Api& value) : api(value), token(api.ensure()) {}
    ~GilScope() { api.release(token); }
};

std::mutex& stateMutex() {
    static std::mutex mutex;
    return mutex;
}

Api& api() {
    static Api value;
    return value;
}

std::string& statusText() {
    static std::string value = "等待游戏原生库";
    return value;
}

// Serialises whole Execute calls. The private compiler keeps one worker state,
// and the game's __main__ dictionary must not be mutated by two callers at once.
std::mutex& executionMutex() {
    static std::mutex mutex;
    return mutex;
}

void setStatus(const std::string& text) {
    std::lock_guard<std::mutex> lock(stateMutex());
    statusText() = text;
}

int collectImage(dl_phdr_info* info, size_t, void* data) {
    if (!info->dlpi_name || !std::strstr(info->dlpi_name, "libminecraftpe.so")) return 0;
    auto& image = *static_cast<script_host::Image*>(data);
    image.base = info->dlpi_addr;
    for (size_t i = 0; i < info->dlpi_phnum; ++i) {
        const auto& header = info->dlpi_phdr[i];
        if (header.p_vaddr > UINTPTR_MAX - image.base) continue;
        if (header.p_memsz > UINTPTR_MAX - image.base - header.p_vaddr) continue;
        const uintptr_t start = image.base + header.p_vaddr;
        if (header.p_type == PT_LOAD && (header.p_flags & PF_R)) {
            arm64_signature::CodeRange range{reinterpret_cast<const uint8_t*>(start),
                                             size_t(header.p_memsz), start};
            if (header.p_flags & PF_X) {
                range.size = header.p_filesz;
                image.code.push_back(range);
            } else {
                image.data.push_back(range);
            }
            if (header.p_flags & PF_W) image.writable.push_back(range);
        }
        if (header.p_type == PT_NOTE) {
            size_t cursor = 0;
            while (cursor + 12 <= header.p_filesz) {
                uint32_t note[3];
                if (!script_host::Read(start + cursor, note, 12)) break;
                cursor += 12;
                const size_t name_size = (size_t(note[0]) + 3) & ~size_t(3);
                const size_t desc_size = (size_t(note[1]) + 3) & ~size_t(3);
                if (name_size > header.p_filesz - cursor ||
                    desc_size > header.p_filesz - cursor - name_size) {
                    break;
                }
                char name[4]{};
                if (note[0] == 4 && note[2] == 3 && script_host::Read(start + cursor, name, 4) &&
                    !std::memcmp(name, "GNU", 4) && note[1] <= 64) {
                    image.buildId.resize(note[1]);
                    if (!script_host::Read(start + cursor + name_size, image.buildId.data(),
                                           image.buildId.size())) {
                        image.buildId.clear();
                    }
                }
                cursor += name_size + desc_size;
            }
        }
    }
    return 1;
}

// Compiles `source` with the private CPython 2.7 compiler and returns the
// marshal payload produced by the 3.10.5 opcode converter.
std::string compileSource(const std::string& source, const char* filename) {
    unsigned char* raw = nullptr;
    size_t length = 0;
    char* error = nullptr;
    const bool ok = PyLoaderCompilePython3105(source.c_str(), filename, &raw, &length, &error) != 0;
    std::unique_ptr<unsigned char, decltype(&free)> bytes(raw, &free);
    std::unique_ptr<char, decltype(&free)> message(error, &free);
    if (!ok) throw std::runtime_error(message ? message.get() : "Python compilation failed");
    // A code object marshal stream always starts with the TYPE_CODE marker.
    if (!length || raw[0] != 'c') throw std::runtime_error("invalid compiled code payload");
    return std::string(reinterpret_cast<const char*>(raw), length);
}

// Source text is passed to the engine as a hex-escaped Python 2.7 string
// literal so that arbitrary bytes, quotes and newlines survive round-tripping.
std::string pythonByteLiteral(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string literal;
    literal.reserve(value.size() * 4 + 2);
    literal.push_back('\'');
    for (unsigned char ch : value) {
        literal += "\\x";
        literal.push_back(hex[(ch >> 4) & 0x0f]);
        literal.push_back(hex[ch & 0x0f]);
    }
    literal.push_back('\'');
    return literal;
}

std::string hexEncode(const std::string& value) {
    static constexpr char hex[] = "0123456789abcdef";
    std::string encoded;
    encoded.reserve(value.size() * 2);
    for (unsigned char ch : value) {
        encoded.push_back(hex[(ch >> 4) & 0x0f]);
        encoded.push_back(hex[ch & 0x0f]);
    }
    return encoded;
}

int hexValue(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

bool decodeHex(const char* encoded, size_t length, std::string* output) {
    if (length % 2 != 0) return false;
    output->clear();
    output->reserve(length / 2);
    for (size_t i = 0; i < length; i += 2) {
        const int high = hexValue(encoded[i]);
        const int low = hexValue(encoded[i + 1]);
        if (high < 0 || low < 0) return false;
        output->push_back(char((high << 4) | low));
    }
    return true;
}

bool resolveApi(std::string* reason) {
    const uintptr_t base = script_host::Base();
    if (!base) {
        *reason = "游戏原生库尚未就绪";
        return false;
    }
    if (!PyEngineAddress::ReadyFor(base)) {
        *reason = "Python API 地址尚未发布";
        return false;
    }
    const auto code = [base](uintptr_t offset, uintptr_t* address) {
        if (!offset || offset > UINTPTR_MAX - base) return false;
        if (!PyEngineAddress::IsResolvedAddress(base, base + offset)) return false;
        return script_host::CodeOffset(base, offset, 4, address);
    };
    const auto data = [base](uintptr_t offset, size_t size, uintptr_t* address) {
        if (!offset || offset > UINTPTR_MAX - base) return false;
        if (size > UINTPTR_MAX - (base + offset)) return false;
        if (!PyEngineAddress::IsResolvedAddress(base, base + offset)) return false;
        *address = base + offset;
        return true;
    };
    const bool verified = local_python::abi::Verify(
        base, &api().addresses, code, data,
        [](uintptr_t address, void* output, size_t size) {
            return script_host::Read(address, output, size);
        });
    if (!verified) {
        *reason = "3.10.5 Python 字节码 ABI 校验未通过";
        return false;
    }
    api().verified = true;
    return true;
}

}  // namespace

bool Install() noexcept {
    std::lock_guard<std::mutex> lock(stateMutex());
    if (api().verified) return true;
    // Failures are retried: the module native library may be loaded before the
    // game image has finished mapping.
    script_host::Image image;
    dl_iterate_phdr(collectImage, &image);
    if (!image.base) {
        statusText() = "等待游戏原生库";
        return false;
    }
    std::string error;
    if (!script_host::Resolve(image, &error)) {
        statusText() = error.empty() ? "游戏版本签名不匹配" : error;
        return false;
    }
    if (!resolveApi(&error)) {
        statusText() = error;
        return false;
    }
    statusText() = "Python 引擎就绪";
    return true;
}

std::string Status() {
    std::lock_guard<std::mutex> lock(stateMutex());
    return statusText();
}

bool Execute(const std::string& source, const std::string& expression, std::string* output) {
    std::string ignored;
    std::string& value_out = output ? *output : ignored;
    value_out.clear();

    {
        std::lock_guard<std::mutex> lock(stateMutex());
        std::string reason;
        if (!api().verified && !resolveApi(&reason)) {
            if (!reason.empty()) statusText() = reason;
            return false;
        }
    }
    Api& engine = api();
    if (!engine.interpreterReady()) {
        setStatus("游戏 Python 解释器尚未初始化");
        return false;
    }

    std::lock_guard<std::mutex> serialise(executionMutex());

    // Results travel back through the environment, exactly as the previous
    // PyRun_StringFlags bridge did: the game may hide PyString_AsString from
    // RTLD_DEFAULT, so the value is hex-encoded by Python itself and the status
    // flag is a separate variable. Nothing is left pending on the game thread.
    constexpr const char* kStatusEnv = "INFINITECZ_ENGINE_STATUS";
    constexpr const char* kValueEnv = "INFINITECZ_ENGINE_VALUE";
    setenv(kStatusEnv, "0", 1);
    setenv(kValueEnv, "", 1);

    std::string wrapper;
    wrapper.reserve(source.size() * 4 + expression.size() * 4 + 2048);
    wrapper +=
        "def __infinitecz_engine_run():\n"
        "  import os, sys, binascii\n"
        "  __infinitecz_status = '0'\n"
        "  __infinitecz_value = ''\n"
        "  try:\n"
        "    exec compile(" + pythonByteLiteral(source) +
        ", '<infinitecz-engine-setup>', 'exec') in globals(), globals()\n";
    if (!expression.empty()) {
        wrapper +=
            "    __infinitecz_result = eval(compile(" + pythonByteLiteral(expression) +
            ", '<infinitecz-engine-eval>', 'eval'), globals(), globals())\n"
            "    try:\n"
            "      if isinstance(__infinitecz_result, unicode):\n"
            "        __infinitecz_result = __infinitecz_result.encode('utf-8')\n"
            "      else:\n"
            "        __infinitecz_result = str(__infinitecz_result)\n"
            "    except NameError:\n"
            "      __infinitecz_result = str(__infinitecz_result)\n"
            "    __infinitecz_value = binascii.hexlify(__infinitecz_result)\n";
    }
    wrapper +=
        "    __infinitecz_status = '1'\n"
        "  except BaseException:\n"
        "    try:\n"
        "      import traceback\n"
        "      traceback.print_exc()\n"
        "    except BaseException:\n"
        "      pass\n"
        "  finally:\n"
        "    try:\n"
        "      sys.exc_clear()\n"
        "    except BaseException:\n"
        "      pass\n"
        "    try:\n"
        "      os.environ['" + std::string(kStatusEnv) + "'] = __infinitecz_status\n"
        "      os.environ['" + std::string(kValueEnv) + "'] = __infinitecz_value\n"
        "    except BaseException:\n"
        "      pass\n"
        "__infinitecz_engine_run()\n"
        "try:\n"
        "  del __infinitecz_engine_run\n"
        "except BaseException:\n"
        "  pass\n";

    bool ran = false;
    try {
        const std::string bytes = compileSource(wrapper, "<infinitecz-engine>");
        const GilScope gil(engine);
        // A small explicit guard is used instead of a unique_ptr with a lambda
        // deleter: a capturing lambda cannot decay to a function pointer, and
        // the code object must be released under the GIL this scope holds.
        struct CodeGuard {
            const Api& owner;
            Object* value;
            ~CodeGuard() { owner.decref(value); }
        };
        const CodeGuard code_guard{engine, engine.load(bytes)};
        Object* const code = code_guard.value;
        if (!code) {
            setStatus("无法构造 Python 代码对象");
            return false;
        }
        Object* module = engine.module();
        Object* globals = engine.dict(module);
        if (!module || !globals) {
            setStatus("无法取得游戏 __main__ 模块");
            return false;
        }
        Object* result = engine.eval(code, globals);
        if (!result) {
            std::array<char, 512> message{};
            engine.describeError(message.data(), message.size(), "engine_execution");
            setStatus(message.data());
            return false;
        }
        engine.decref(result);
        ran = true;
    } catch (const std::exception& error) {
        setStatus(error.what());
        return false;
    }

    const char* status = std::getenv(kStatusEnv);
    const char* encoded = std::getenv(kValueEnv);
    const bool succeeded = ran && status && status[0] == '1' && status[1] == '\0';
    if (succeeded && encoded) {
        decodeHex(encoded, std::strlen(encoded), &value_out);
    }
    return succeeded;
}

bool ExecuteSource(const std::string& source) {
    return Execute(source, std::string(), nullptr);
}

}  // namespace py_engine
