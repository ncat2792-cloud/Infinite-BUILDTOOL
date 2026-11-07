/* Private compiler runtime configuration: no site, filesystem module path,
 * extension loader, user scripts, signal handlers or game module imports. */
#include "Python.h"
#include "importdl.h"

extern void PyMarshal_Init(void);
extern void initimp(void);
extern void init_ast(void);
extern void init_codecs(void);
extern void init_weakref(void);
extern void initgc(void);
extern void _PyWarnings_Init(void);
extern void initencodings(void);
struct _inittab _PyImport_Inittab[] = {
    {"marshal", PyMarshal_Init}, {"imp", initimp}, {"_ast", init_ast},
    {"_codecs", init_codecs}, {"encodings", initencodings},
    {"_weakref", init_weakref}, {"gc", initgc},
    {"__main__", NULL}, {"__builtin__", NULL}, {"sys", NULL},
    {"exceptions", NULL}, {"_warnings", _PyWarnings_Init}, {NULL, NULL}
};
static struct _frozen empty_frozen[] = {{NULL, NULL, 0}};
struct _frozen *PyImport_FrozenModules = empty_frozen;
char *Py_GetPath(void) { return ""; }
char *Py_GetPrefix(void) { return ""; }
char *Py_GetExecPrefix(void) { return ""; }
char *Py_GetProgramFullPath(void) { return "pyloader-python-compiler"; }
const char *Py_GetBuildInfo(void) { return "PyLoader private Python 2.7.13 compiler"; }
const char *_Py_hgversion(void) { return ""; }
const char *_Py_hgidentifier(void) { return "pyloader-private"; }
