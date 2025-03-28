/**
 * Dr.Jit's builtin PTX files are quite large (~2MB for each targeted SM version).
 *
 * This file compresses them with LZ4 following a build. This compressed version can
 * then be checked into Git and included in executables
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <lz4hc.h>
#include <xxh3.h>

char *read_file(const char *fname, size_t *size_out) {
    FILE *f = fopen(fname, "r");
    if (!f) {
        fprintf(stderr, "Could not open '%s'!\n", fname);
        exit(EXIT_FAILURE);
    }

    fseek(f, 0, SEEK_END);
    size_t size = ftell(f);
    char *buf = malloc(size + 1);
    buf[size] = '\0';
    fseek(f, 0, SEEK_SET);

    if (fread(buf, size, 1, f) != 1) {
        fprintf(stderr, "Could not read '%s'!\n", fname);
        exit(EXIT_FAILURE);
    }

    fclose(f);
    *size_out = size;
    return buf;
}

void pack(FILE *f, const char *id, const char *source_fname, const char *dst_fname, const void *dict, size_t dict_size) {
    LZ4_streamHC_t *stream = LZ4_createStreamHC();
    size_t source_size;
    void *source = read_file(source_fname, &source_size);

    int buf_size = LZ4_compressBound(source_size);
    char *buf = (char *) malloc(buf_size);

    LZ4_resetStreamHC_fast(stream, LZ4HC_CLEVEL_MAX);
    if (dict)
        LZ4_loadDictHC(stream, dict, dict_size);

    size_t compressed_size = LZ4_compress_HC_continue(stream, source, buf, source_size, buf_size);

    fprintf(f, "extern const char %s[];\n", id);
    fprintf(f, "static const size_t %s_size_uncompressed = %zu;\n", id, source_size);
    fprintf(f, "static const size_t %s_size_compressed = %zu;\n", id, compressed_size);

    FILE *f2 = fopen(dst_fname, "wb");
    if (!f2) {
        fprintf(stderr, "Could not open '%s'!", dst_fname);
        exit(EXIT_FAILURE);
    }
    fwrite(buf, 1, compressed_size, f2);
    fclose(f2);
    free(buf);
    free(source);
    LZ4_freeStreamHC(stream);
    fprintf(f, "\n");
}

int main(int argc, char **argv) {
    FILE *f = fopen("kernels.h", "w");
    if (!f) {
        fprintf(stderr, "Could not open 'kernels.h'!");
        exit(EXIT_FAILURE);
    }

    size_t kernels_dict_size;
    char *kernels_dict = read_file("kernels_dict", &kernels_dict_size);

    fprintf(f, "// This file was automatically generated by \"pack\"\n\n");
    fprintf(f, "#pragma once\n\n");
    fprintf(f, "#include <stdlib.h>\n\n");
    fprintf(f, "#if defined(__GNUC__)\n");
    fprintf(f, "#  pragma GCC diagnostic push\n");
    fprintf(f, "#  pragma GCC diagnostic ignored \"-Wunused-variable\"\n");
    fprintf(f, "#endif\n\n");

    fprintf(f, "#ifdef __cplusplus\n");
    fprintf(f, "extern \"C\" {\n");
    fprintf(f, "#endif\n\n");

    pack(f, "kernels_dict", "kernels_dict",    "kernels_dict.lz4", NULL, 0);
    pack(f, "kernels_50", "kernels_50.ptx",  "kernels_50.lz4", kernels_dict, kernels_dict_size);
    pack(f, "kernels_70", "kernels_70.ptx",  "kernels_70.lz4", kernels_dict, kernels_dict_size);

    size_t kernels_70_size;
    char *kernels_70 = read_file("kernels_70.ptx", &kernels_70_size);

    fprintf(f, "static const char *kernels_list =");
    char *ptr = kernels_70;
    while (ptr) {
        ptr = strstr(ptr, ".entry ");
        if (!ptr)
            break;
        ptr += 7;
        char *next = strstr(ptr, "(");
        if (!next)
            break;
        fprintf(f, "\n    \"");
        fwrite(ptr, next-ptr, 1, f);
        fprintf(f, ",\"");
        ptr = next;
    }

    fprintf(f, ";\n\n");
    fprintf(f, "#ifdef __cplusplus\n");
    fprintf(f, "}\n");
    fprintf(f, "#endif\n\n");

    fprintf(f, "#if defined(__GNUC__)\n");
    fprintf(f, "#  pragma GCC diagnostic pop\n");
    fprintf(f, "#endif\n\n");
    free(kernels_dict);
    free(kernels_70);
    fclose(f);

    return EXIT_SUCCESS;
}

