#include "CompilerGameObjects.h"
#include "Python.h"
#include "marshal.h"
#include <stdlib.h>
#include <string.h>

typedef PyLoaderPythonHandle Handle;
typedef struct { const PyLoaderPythonConstructors* api; size_t nodes; } Builder;
static Handle build(Builder*, PyObject*, int);
static void release(Builder* b, Handle h) { if (h) b->api->release(b->api->context, h); }

static Handle sequence(Builder* b, PyObject* value, int depth) {
    const Py_ssize_t count = PySequence_Size(value);
    if (count < 0) return NULL;
    Handle tuple = b->api->tuple(b->api->context, (size_t)count);
    if (!tuple) return NULL;
    for (Py_ssize_t i = 0; i < count; ++i) {
        PyObject* child = PySequence_GetItem(value, i);
        Handle handle = child ? build(b, child, depth + 1) : NULL;
        Py_XDECREF(child);
        if (!handle) { release(b, tuple); return NULL; }
        if (b->api->tupleSet(b->api->context, tuple, (size_t)i, handle) < 0) {
            release(b, tuple); return NULL;
        }
    }
    return tuple;
}
static Handle code(Builder* b, PyCodeObject* co, int depth) {
    PyObject* values[] = {co->co_code, co->co_consts, co->co_names, co->co_varnames,
        co->co_freevars, co->co_cellvars, co->co_filename, co->co_name, co->co_lnotab};
    Handle handles[9] = {0}, result = NULL;
    for (size_t i = 0; i < 9; ++i) {
        handles[i] = build(b, values[i], depth + 1);
        if (!handles[i]) goto done;
    }
    PyLoaderPythonCodeFields fields = {
        co->co_argcount, co->co_nlocals, co->co_stacksize, co->co_flags, co->co_firstlineno,
        handles[0], handles[1], handles[2], handles[3], handles[4], handles[5],
        handles[6], handles[7], handles[8]
    };
    result = b->api->code(b->api->context, &fields);
done:
    for (size_t i = 0; i < 9; ++i) release(b, handles[i]);
    return result;
}
static Handle build(Builder* b, PyObject* value, int depth) {
    const PyLoaderPythonConstructors* a = b->api;
    if (depth > 200 || ++b->nodes > 1000000) {
        PyErr_SetString(PyExc_ValueError, "compiled constants exceed the construction budget"); return NULL;
    }
    if (value == Py_None) return a->singleton(a->context, 0);
    if (value == Py_False) return a->singleton(a->context, 1);
    if (value == Py_True) return a->singleton(a->context, 2);
    if (value == Py_Ellipsis) return a->singleton(a->context, 3);
    if (PyString_Check(value)) return a->bytes(a->context, PyString_AS_STRING(value), (size_t)PyString_GET_SIZE(value));
    if (PyUnicode_Check(value)) {
        PyObject* text = PyUnicode_AsUTF8String(value);
        if (!text) return NULL;
        Handle result = a->unicode(a->context, PyString_AS_STRING(text), (size_t)PyString_GET_SIZE(text));
        Py_DECREF(text); return result;
    }
    if (PyInt_Check(value)) return a->integer(a->context, (int64_t)PyInt_AS_LONG(value));
    if (PyLong_Check(value)) {
        PyObject* decimal = PyObject_Str(value);
        if (!decimal) return NULL;
        Handle result = a->longDecimal(a->context, PyString_AS_STRING(decimal));
        Py_DECREF(decimal); return result;
    }
    if (PyFloat_Check(value)) return a->floating(a->context, PyFloat_AS_DOUBLE(value));
    if (PyTuple_Check(value)) return sequence(b, value, depth);
    if (PyFrozenSet_Check(value)) {
        PyObject* items = PySequence_Tuple(value);
        if (!items) return NULL;
        Handle tuple = sequence(b, items, depth);
        Py_DECREF(items);
        if (!tuple) return NULL;
        Handle result = a->frozenSet(a->context, tuple); release(b, tuple); return result;
    }
    if (PyCode_Check(value)) return code(b, (PyCodeObject*)value, depth);
    PyErr_SetString(PyExc_ValueError, "constant type is unavailable in the reviewed game Python runtime");
    return NULL;
}
Handle PyLoaderBuildPython3105(const char* bytes, size_t length,
    const PyLoaderPythonConstructors* api, char** privateError) {
    *privateError = NULL;
    if (!api || !bytes || !length || length > 16U * 1024U * 1024U || bytes[0] != 'c') return NULL;
    PyObject* root = PyMarshal_ReadObjectFromString((char*)bytes, (Py_ssize_t)length);
    Builder builder = {api, 0};
    Handle result = root && PyCode_Check(root) ? build(&builder, root, 0) : NULL;
    Py_XDECREF(root);
    if (!result && PyErr_Occurred()) {
        PyObject *type = NULL, *value = NULL, *trace = NULL;
        PyErr_Fetch(&type, &value, &trace);
        PyErr_NormalizeException(&type, &value, &trace);
        PyObject* message = value ? PyObject_Str(value) : NULL;
        const char* text = message ? PyString_AsString(message) : NULL;
        if (!text) text = "private compiled-object reconstruction failed";
        const size_t size = strlen(text) + 1;
        *privateError = malloc(size);
        if (*privateError) memcpy(*privateError, text, size);
        Py_XDECREF(message); Py_XDECREF(type); Py_XDECREF(value); Py_XDECREF(trace); PyErr_Clear();
    }
    return result;
}
