#ifndef INFINITE_BUILD_PY_ENGINE_BRIDGE_H
#define INFINITE_BUILD_PY_ENGINE_BRIDGE_H

#include <cstdint>
#include <memory>
#include <string>

// Bridge between the building tool's existing PythonUtils call sites and the
// 3.10.5 script engine.
//
// The shipped 3.9.5 code executed user code through the game's own
// PyRun_StringFlags entry point. That entry point no longer exists in the
// 3.10.5 client, so every building-tool feature that ran embedded Python
// (import, projection printer, inventory transfer, native world access) went
// dark at the same time. This bridge keeps the old synchronous, string-in /
// string-out contract while the actual work is performed by the script engine:
// the source is compiled by the private CPython 2.7 compiler bundled with the
// module and the resulting bytecode is evaluated on the game thread.
//
// The engine is entered from the local-player tick. Callers that are already
// running on that thread evaluate immediately; callers on another thread park
// until the tick delivers their result, exactly like the previous synchronous
// game API did. A call therefore must never be issued while holding a lock the
// tick callback needs.
namespace py_engine {

// True once the game image has been identified and the 3.10.5 Python API
// addresses have been verified. Until then every execute call fails.
bool Install() noexcept;

// Current one-line description of the engine state, for logs and the UI.
std::string Status();

// Runs `source` in the engine's __main__ module and captures the value of the
// trailing `expression`. Both must be valid Python 2.7 source. `output`
// receives the evaluated expression as UTF-8 text.
bool Execute(const std::string& source, const std::string& expression,
             std::string* output);

// Runs `source` without an expression. Equivalent to Execute(source, "",
// output) and used where only success or failure matters.
bool ExecuteSource(const std::string& source);

}  // namespace py_engine

#endif  // INFINITE_BUILD_PY_ENGINE_BRIDGE_H
