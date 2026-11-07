#pragma once
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Call only on the single compiler worker. Returned buffers use malloc/free;
 * they contain bytes/text, never PyObject or game-owned allocations. */
int PyLoaderCompilePython27(const char *source, const char *filename,
                             unsigned char **output, size_t *length,
                             char **error);
int PyLoaderCompilePython3105(const char *source, const char *filename,
                               unsigned char **output, size_t *length,
                               char **error);
void PyLoaderFinalizePythonCompiler(void);
#ifdef __cplusplus
}
#endif
