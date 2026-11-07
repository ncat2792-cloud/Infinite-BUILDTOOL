#ifndef BUILD_TOOL_PYTHON_UTILS_H
#define BUILD_TOOL_PYTHON_UTILS_H

#include "string"
#include "FunctionUtils.h"
#include "FunctionsAddress.h"
#include "../pyengine/PyEngineBridge.h"
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>

typedef
enum { PyGILState_LOCKED1, PyGILState_UNLOCKED1 }
        PyGILState_STATE1;
class PyObject1;
class PyCompilerFlags1;
constexpr auto Py_file_input1 = 257;


class PythonUtils{
public:

// ---------------------------------------------------------------------------
// 3.10.5 execution bridge.
//
// The shipped 3.9.5 client exposed PyRun_StringFlags through
// FunctionsAddress::PythonUtils_*. Those absolute RVAs are meaningless in the
// 3.10.5 client, and the entry point itself is gone. Every caller below keeps
// its original signature and semantics, but the work is now handed to
// py_engine::Execute, which compiles the source with the module's private
// CPython 2.7 compiler and evaluates the result through the 3.10.5 bytecode
// ABI that the engine verifies by genuine code signatures at startup.
//
// The environment-variable result channel is preserved because the game may
// hide PyString_AsString from RTLD_DEFAULT; PyEngineBridge performs the same
// hex round-trip internally.
// ---------------------------------------------------------------------------

static bool PyEvalUtf8(const std::string& statements, const std::string& expression,
                       std::string* output) {
    if (!output) return false;
    output->clear();
    return py_engine::Execute(statements, expression, output);
}

static bool PyExecChecked(std::string& str, bool noMessage = false) {
    (void)noMessage;
    return py_engine::ExecuteSource(str);
}

// Executes trusted, internally generated source directly. Callers must expose
// their detailed result via their own return channel; a true result only means
// the source itself did not raise an uncaught exception.
static bool PyExecDirectChecked(const std::string& str) {
    return py_engine::ExecuteSource(str);
}

// The engine owns the GIL pairing internally, so the former lock/ensure/release
// helpers are no longer part of the execution path. They are kept as no-ops so
// that diagnostic code referring to them still links.
static void setExecLocked(bool isLock = false) { (void)isLock; }

private:
static std::mutex& executionMutex() {
    static std::mutex mutex;
    return mutex;
}

};


#endif //BUILD_TOOL_PYTHON_UTILS_H
