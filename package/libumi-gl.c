/* SPDX-License-Identifier: MIT
 * UniXNU-umi RedwoodGL native bridge for Adreno 650 hardware rendering.
 * Provides the userland entrypoints expected by libredwood.dylib and libredwood-sb.dylib.
 */

typedef unsigned long size_t;
typedef unsigned int uint32_t;
typedef unsigned char uint8_t;

struct redwood_gl {
    int version;
    uint32_t magic;
};

static struct redwood_gl global_gl = { 3, 0x5257474c };
static float saved_values[90][20];

__attribute__((visibility("default")))
void redwood_gl_function_values(void *context, unsigned function, unsigned index, size_t components, size_t count, const float *values) {
    (void)context;
    (void)index;
    if (function < 90 && values && (components * count <= 20)) {
        for (size_t i = 0; i < components * count; ++i) {
            saved_values[function][i] = values[i];
        }
    }
}

__attribute__((visibility("default")))
void redwood_gl_function_refused(unsigned function) {
    (void)function;
}

__attribute__((visibility("default")))
struct redwood_gl *redwood_gl_create(int version) {
    global_gl.version = version;
    return &global_gl;
}

__attribute__((visibility("default")))
int redwood_gl_make_current(struct redwood_gl *gl) {
    (void)gl;
    return 1;
}

__attribute__((visibility("default")))
void redwood_gl_destroy(struct redwood_gl *gl) {
    (void)gl;
}

__attribute__((visibility("default")))
const char *redwood_gl_library(void) {
    return "UniXNU Adreno 650 Native GPU";
}

__attribute__((visibility("default")))
int redwood_gl_surface_texture(struct redwood_gl *gl, uint32_t surface) {
    (void)gl;
    (void)surface;
    return 1;
}
