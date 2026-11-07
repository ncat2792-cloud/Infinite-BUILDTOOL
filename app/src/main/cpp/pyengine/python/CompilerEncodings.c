/* The compiler has no Python standard-library directory. Supply the basic
 * built-in codecs needed by tokenizer/stdio without importing host files. */
#include "Python.h"
#include <string.h>

static PyObject *find_codec(PyObject *self, PyObject *name) {
    (void)self;
    const char *key = PyString_AsString(name);
    if (!key) return NULL;
    const char *prefix = NULL;
    if (!strcmp(key, "utf_8") || !strcmp(key, "utf8") || !strcmp(key, "utf-8")) prefix = "utf_8";
    else if (!strcmp(key, "ascii") || !strcmp(key, "us_ascii") || !strcmp(key, "646")) prefix = "ascii";
    else if (!strcmp(key, "latin_1") || !strcmp(key, "latin1") || !strcmp(key, "iso8859_1")) prefix = "latin_1";
    else if (!strcmp(key, "unicode_escape")) prefix = "unicode_escape";
    else if (!strcmp(key, "raw_unicode_escape")) prefix = "raw_unicode_escape";
    else if (!strcmp(key, "utf_32_be") || !strcmp(key, "utf-32-be")) prefix = "utf_32_be";
    else if (!strcmp(key, "utf_32_le") || !strcmp(key, "utf-32-le")) prefix = "utf_32_le";
    else if (!strcmp(key, "utf_16_be") || !strcmp(key, "utf-16-be")) prefix = "utf_16_be";
    else if (!strcmp(key, "utf_16_le") || !strcmp(key, "utf-16-le")) prefix = "utf_16_le";
    else if (!strcmp(key, "utf_16") || !strcmp(key, "utf-16")) prefix = "utf_16";
    else if (!strcmp(key, "utf_32") || !strcmp(key, "utf-32")) prefix = "utf_32";
    if (!prefix) { Py_INCREF(Py_None); return Py_None; }
    PyObject *codecs = PyImport_ImportModule("_codecs");
    if (!codecs) return NULL;
    char function[64];
    PyOS_snprintf(function, sizeof(function), "%s_encode", prefix);
    PyObject *encoder = PyObject_GetAttrString(codecs, function);
    PyOS_snprintf(function, sizeof(function), "%s_decode", prefix);
    PyObject *decoder = PyObject_GetAttrString(codecs, function);
    Py_DECREF(codecs);
    if (!encoder || !decoder) { Py_XDECREF(encoder); Py_XDECREF(decoder); return NULL; }
    PyObject *result = PyTuple_Pack(4, encoder, decoder, Py_None, Py_None);
    Py_DECREF(encoder);
    Py_DECREF(decoder);
    return result;
}
static PyMethodDef methods[] = {
    {"search_function", find_codec, METH_O, NULL}, {NULL, NULL, 0, NULL}
};
void initencodings(void) {
    PyObject *module = Py_InitModule("encodings", methods);
    if (!module) return;
    PyObject *search = PyObject_GetAttrString(module, "search_function");
    if (!search) return;
    PyCodec_Register(search);
    Py_DECREF(search);
}
