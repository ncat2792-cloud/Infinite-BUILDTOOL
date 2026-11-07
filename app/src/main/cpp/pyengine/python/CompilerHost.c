/* Local compiler fixture. Never linked into the Android module. */
#include "CompilerEntry.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int PyLoaderRunCompilerFixture(const char *, size_t, int);
int main(int argc, char **argv) {
    if (argc < 3 || argc > 5) return 2;
    int target = 0, execute = 1;
    for (int i = 3; i < argc; ++i) {
        if (!strcmp(argv[i], "--target3105")) target = 1;
        else if (!strcmp(argv[i], "--compile-only")) execute = 0;
        else return 2;
    }
    FILE *input = fopen(argv[1], "rb");
    if (!input) return 3;
    if (fseek(input, 0, SEEK_END)) return 4;
    long size = ftell(input);
    if (size < 0 || size > 4 * 1024 * 1024) return 4;
    rewind(input);
    char *source = calloc((size_t)size + 1, 1);
    if (!source || fread(source, 1, (size_t)size, input) != (size_t)size) return 5;
    fclose(input);
    unsigned char *bytes = NULL;
    size_t length = 0;
    char *error = NULL;
    int ok = target ? PyLoaderCompilePython3105(source, argv[1], &bytes, &length, &error)
                    : PyLoaderCompilePython27(source, argv[1], &bytes, &length, &error);
    free(source);
    if (!ok) { fprintf(stderr, "%s\n", error ? error : "compile failed"); free(error); return 6; }
    FILE *output = fopen(argv[2], "wb");
    if (!output) return 7;
    int result = fwrite(bytes, 1, length, output) == length ? 0 : 8;
    if (fclose(output)) result = 8;
    if (execute && !PyLoaderRunCompilerFixture((const char *)bytes, length, target)) result = 9;
    free(bytes);
    return result;
}
