#include "CompilerEntry.h"
#include "Python.h"
#include "marshal.h"
#include "Python3105Bytecode.h"
#include <stdlib.h>
#include <string.h>

static int initialized;

static char *copy_text(const char *text) {
    size_t size = strlen(text) + 1;
    char *result = malloc(size);
    if (result) memcpy(result, text, size);
    return result;
}

static void take_error(char **error) {
    PyObject *type = NULL, *value = NULL, *trace = NULL, *message = NULL;
    PyErr_Fetch(&type, &value, &trace);
    PyErr_NormalizeException(&type, &value, &trace);
    if (value) message = PyObject_Str(value);
    *error = copy_text(message && PyString_Check(message)
        ? PyString_AS_STRING(message) : "Python 2.7 compilation failed");
    Py_XDECREF(message);
    Py_XDECREF(type);
    Py_XDECREF(value);
    Py_XDECREF(trace);
    PyErr_Clear();
}

static int translate_code(PyCodeObject *code, int encode, int depth) {
    if (depth > 200) { PyErr_SetString(PyExc_ValueError, "script nesting exceeds 200 code objects"); return 0; }
    Py_ssize_t size = PyString_GET_SIZE(code->co_code);
    PyObject *translated = PyString_FromStringAndSize(PyString_AS_STRING(code->co_code), size);
    if (!translated) return 0;
    unsigned char *bytes = (unsigned char *)PyString_AS_STRING(translated);
    Py_ssize_t cursor = 0;
    while (cursor < size) {
        int opcode = bytes[cursor];
        int mapped = encode ? PyLoaderEncodeOpcode3105(opcode) : PyLoaderDecodeOpcode3105(opcode);
        int width = opcode >= (encode ? 90 : 93) ? 3 : 1;
        if (mapped < 0 || cursor + width > size) {
            Py_DECREF(translated);
            PyErr_SetString(PyExc_ValueError, "unsupported or truncated Python opcode");
            return 0;
        }
        bytes[cursor] = (unsigned char)mapped;
        cursor += width;
    }
    Py_DecRef(code->co_code);
    code->co_code = translated;
    Py_ssize_t i;
    for (i = 0; i < PyTuple_GET_SIZE(code->co_consts); ++i) {
        PyObject *constant = PyTuple_GET_ITEM(code->co_consts, i);
        if (PyCode_Check(constant) && !translate_code((PyCodeObject *)constant, encode, depth + 1)) return 0;
    }
    return 1;
}

static int compile(const char *source, const char *filename, int target,
                   unsigned char **output, size_t *length, char **error) {
    *output = NULL;
    *length = 0;
    *error = NULL;
    if (!initialized) {
        Py_NoSiteFlag = 1;
        Py_IgnoreEnvironmentFlag = 1;
        Py_DontWriteBytecodeFlag = 1;
        Py_InitializeEx(0);
        if (!Py_IsInitialized()) { *error = copy_text("compiler initialization failed"); return 0; }
        initialized = 1;
    }
    PyCompilerFlags flags = {0};
    PyObject *code = (PyObject *)Py_CompileStringFlags(source, filename, Py_file_input, &flags);
    if (!code) { take_error(error); return 0; }
    if (target && !translate_code((PyCodeObject *)code, 1, 0)) {
        Py_DecRef(code); take_error(error); return 0;
    }
    PyObject *bytes = PyMarshal_WriteObjectToString(code, 2);
    Py_DecRef(code);
    if (!bytes) { take_error(error); return 0; }
    size_t size = (size_t)PyString_GET_SIZE(bytes);
    unsigned char *data = malloc(size);
    if (data) memcpy(data, PyString_AS_STRING(bytes), size);
    Py_DECREF(bytes);
    if (!data) { *error = copy_text("compiler output allocation failed"); return 0; }
    *output = data;
    *length = size;
    return 1;
}

int PyLoaderCompilePython27(const char *source, const char *filename,
                             unsigned char **output, size_t *length, char **error) {
    return compile(source, filename, 0, output, length, error);
}
int PyLoaderCompilePython3105(const char *source, const char *filename,
                               unsigned char **output, size_t *length, char **error) {
    return compile(source, filename, 1, output, length, error);
}
void PyLoaderFinalizePythonCompiler(void) {
    if (initialized) { Py_Finalize(); initialized = 0; }
}

#ifdef PYLOADER_COMPILER_HOST_TEST
int PyLoaderRunCompilerFixture(const char *data, size_t length, int encoded) {
    PyObject *code = PyMarshal_ReadObjectFromString((char *)data, (Py_ssize_t)length);
    if (!code) return 0;
    if (encoded && !translate_code((PyCodeObject *)code, 0, 0)) { Py_DecRef(code); return 0; }
    PyObject *globals = PyDict_New();
    if (!globals) { Py_DecRef(code); return 0; }
    int bound = PyDict_SetItemString(globals, "__builtins__", PyEval_GetBuiltins()) == 0;
    PyObject *result = bound ? PyEval_EvalCode((PyCodeObject *)code, globals, globals) : NULL;
    if (!result) PyErr_Print();
    Py_XDECREF(result);
    Py_DECREF(globals);
    Py_DecRef(code);
    return result != NULL;
}
#endif
