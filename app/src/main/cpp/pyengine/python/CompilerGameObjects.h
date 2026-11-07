#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Opaque handles belong exclusively to the destination interpreter. The
 * private compiler never casts/dereferences them as its own PyObjects. All
 * byte inputs are copied by the destination constructors before returning. */
typedef void* PyLoaderPythonHandle;
typedef struct {
    int argcount, nlocals, stacksize, flags, firstlineno;
    PyLoaderPythonHandle code, consts, names, varnames, freevars, cellvars;
    PyLoaderPythonHandle filename, name, lnotab;
} PyLoaderPythonCodeFields;
typedef struct {
    void* context;
    PyLoaderPythonHandle (*bytes)(void*, const char*, size_t);
    PyLoaderPythonHandle (*unicode)(void*, const char*, size_t);
    PyLoaderPythonHandle (*integer)(void*, int64_t);
    PyLoaderPythonHandle (*longDecimal)(void*, const char*);
    PyLoaderPythonHandle (*floating)(void*, double);
    PyLoaderPythonHandle (*singleton)(void*, int); /* None, False, True, Ellipsis */
    PyLoaderPythonHandle (*tuple)(void*, size_t);
    int (*tupleSet)(void*, PyLoaderPythonHandle, size_t, PyLoaderPythonHandle);
    PyLoaderPythonHandle (*frozenSet)(void*, PyLoaderPythonHandle);
    PyLoaderPythonHandle (*code)(void*, const PyLoaderPythonCodeFields*);
    void (*release)(void*, PyLoaderPythonHandle);
} PyLoaderPythonConstructors;
/* Single compiler worker, while holding the destination GIL. tupleSet steals
 * its item on both success/failure; all other handle arguments are borrowed.
 * Result is one owned destination reference. NULL without a private error
 * means a destination API failed: read that interpreter's exception. */
PyLoaderPythonHandle PyLoaderBuildPython3105(const char* bytes, size_t length,
    const PyLoaderPythonConstructors* constructors, char** privateError);
#ifdef __cplusplus
}
#endif
