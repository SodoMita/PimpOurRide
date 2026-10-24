/* mesh.c - procedural meshes. See mesh.h for the layout contract. */
#include "mesh.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

#define M_PI_F 3.14159265358979323846f

typedef struct {
    float *verts; size_t verts_cap, verts_len;      /* in floats */
    unsigned short *idx; size_t idx_cap, idx_len;
} Builder;

static void b_grow_v(Builder *b, size_t need) {
    if (b->verts_len + need <= b->verts_cap) return;
    b->verts_cap = (b->verts_cap ? b->verts_cap * 2 : 512);
    while (b->verts_len + need > b->verts_cap) b->verts_cap *= 2;
    b->verts = realloc(b->verts, b->verts_cap * sizeof(float));
}
static unsigned short b_vertex(Builder *b, float px, float py, float pz,
                               float nx, float ny, float nz, float u, float v) {
    b_grow_v(b, 8);
    float *d = &b->verts[b->verts_len];
    d[0] = px; d[1] = py; d[2] = pz; d[3] = nx; d[4] = ny; d[5] = nz; d[6] = u; d[7] = v;
    b->verts_len += 8;
    return (unsigned short)(b->verts_len / 8 - 1);
}
static void b_index(Builder *b, unsigned short i) {
    if (b->idx_len == b->idx_cap) {
        b->idx_cap = b->idx_cap ? b->idx_cap * 2 : 512;
        b->idx = realloc(b->idx, b->idx_cap * sizeof(unsigned short));
    }
    b->idx[b->idx_len++] = i;
}
static void b_quad(Builder *b, unsigned short a, unsigned short c,
                   unsigned short d, unsigned short e) {
    /* Two triangles, CCW when viewed from the normal side. */
    b_index(b, a); b_index(b, c); b_index(b, d);
    b_index(b, a); b_index(b, d); b_index(b, e);
}

static MeshData build_box(void) {
    Builder b = { 0 };
    /* Faces: +Z, -Z, +X, -X, +Y, -Y. 24 verts so each face gets flat normals. */
    static const float faces[6][4][3] = {
        { {-0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f, 0.5f}, { 0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f} },
        { { 0.5f,-0.5f,-0.5f}, {-0.5f,-0.5f,-0.5f}, {-0.5f, 0.5f,-0.5f}, { 0.5f, 0.5f,-0.5f} },
        { { 0.5f,-0.5f, 0.5f}, { 0.5f,-0.5f,-0.5f}, { 0.5f, 0.5f,-0.5f}, { 0.5f, 0.5f, 0.5f} },
        { {-0.5f,-0.5f,-0.5f}, {-0.5f,-0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f,-0.5f} },
        { {-0.5f, 0.5f, 0.5f}, { 0.5f, 0.5f, 0.5f}, { 0.5f, 0.5f,-0.5f}, {-0.5f, 0.5f,-0.5f} },
        { {-0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f,-0.5f}, { 0.5f,-0.5f, 0.5f}, {-0.5f,-0.5f, 0.5f} },
    };
    static const float normals[6][3] = {
        {0,0,1}, {0,0,-1}, {1,0,0}, {-1,0,0}, {0,1,0}, {0,-1,0}
    };
    static const float uv[4][2] = { {0,0}, {1,0}, {1,1}, {0,1} };
    for (int f = 0; f < 6; f++) {
        unsigned short v[4];
        for (int i = 0; i < 4; i++)
            v[i] = b_vertex(&b, faces[f][i][0], faces[f][i][1], faces[f][i][2],
                            normals[f][0], normals[f][1], normals[f][2], uv[i][0], uv[i][1]);
        b_quad(&b, v[0], v[1], v[2], v[3]);
    }
    MeshData m = { b.verts, b.verts_len, b.idx, b.idx_len };
    return m;
}

static MeshData build_plane_xz(void) {
    Builder b = { 0 };
    unsigned short v0 = b_vertex(&b, -0.5f, 0, -0.5f, 0, 1, 0, 0, 0);
    unsigned short v1 = b_vertex(&b,  0.5f, 0, -0.5f, 0, 1, 0, 1, 0);
    unsigned short v2 = b_vertex(&b,  0.5f, 0,  0.5f, 0, 1, 0, 1, 1);
    unsigned short v3 = b_vertex(&b, -0.5f, 0,  0.5f, 0, 1, 0, 0, 1);
    b_quad(&b, v0, v1, v2, v3);
    MeshData m = { b.verts, b.verts_len, b.idx, b.idx_len };
    return m;
}

static MeshData build_plane_xy(void) {
    Builder b = { 0 };
    unsigned short v0 = b_vertex(&b, -0.5f, -0.5f, 0, 0, 0, 1, 0, 0);
    unsigned short v1 = b_vertex(&b,  0.5f, -0.5f, 0, 0, 0, 1, 1, 0);
    unsigned short v2 = b_vertex(&b,  0.5f,  0.5f, 0, 0, 0, 1, 1, 1);
    unsigned short v3 = b_vertex(&b, -0.5f,  0.5f, 0, 0, 0, 1, 0, 1);
    b_quad(&b, v0, v1, v2, v3);
    MeshData m = { b.verts, b.verts_len, b.idx, b.idx_len };
    return m;
}

static MeshData build_cylinder(void) {
    enum { SLICES = 20 };
    Builder b = { 0 };
    /* Side wall: smooth normals. Caps: separate rings with flat normals. */
    unsigned short side[SLICES + 1][2];
    for (int i = 0; i <= SLICES; i++) {
        float a = (float)i / SLICES * 2.0f * M_PI_F;
        float cx = cosf(a) * 0.5f, cz = sinf(a) * 0.5f;
        side[i][0] = b_vertex(&b, cx, -0.5f, cz, cosf(a), 0, sinf(a), (float)i / SLICES, 0);
        side[i][1] = b_vertex(&b, cx,  0.5f, cz, cosf(a), 0, sinf(a), (float)i / SLICES, 1);
    }
    for (int i = 0; i < SLICES; i++)
        b_quad(&b, side[i][0], side[i + 1][0], side[i + 1][1], side[i][1]);
    for (int cap = 0; cap < 2; cap++) {
        float y = cap ? 0.5f : -0.5f, ny = cap ? 1.0f : -1.0f;
        unsigned short center = b_vertex(&b, 0, y, 0, 0, ny, 0, 0.5f, 0.5f);
        unsigned short ring[SLICES + 1];
        for (int i = 0; i <= SLICES; i++) {
            float a = (float)i / SLICES * 2.0f * M_PI_F;
            ring[i] = b_vertex(&b, cosf(a) * 0.5f, y, sinf(a) * 0.5f, 0, ny, 0,
                               0.5f + cosf(a) * 0.5f, 0.5f + sinf(a) * 0.5f);
        }
        for (int i = 0; i < SLICES; i++) {
            if (cap) { b_index(&b, center); b_index(&b, ring[i]); b_index(&b, ring[i + 1]); }
            else     { b_index(&b, center); b_index(&b, ring[i + 1]); b_index(&b, ring[i]); }
        }
    }
    MeshData m = { b.verts, b.verts_len, b.idx, b.idx_len };
    return m;
}

static MeshData build_sphere(void) {
    enum { STACKS = 14, SLICES = 20 };
    Builder b = { 0 };
    unsigned short grid[STACKS + 1][SLICES + 1];
    for (int y = 0; y <= STACKS; y++) {
        float phi = (float)y / STACKS * M_PI_F; /* 0..pi, top to bottom */
        for (int x = 0; x <= SLICES; x++) {
            float theta = (float)x / SLICES * 2.0f * M_PI_F;
            float nx = sinf(phi) * cosf(theta), ny = cosf(phi), nz = sinf(phi) * sinf(theta);
            grid[y][x] = b_vertex(&b, nx * 0.5f, ny * 0.5f, nz * 0.5f, nx, ny, nz,
                                  (float)x / SLICES, (float)y / STACKS);
        }
    }
    for (int y = 0; y < STACKS; y++)
        for (int x = 0; x < SLICES; x++)
            b_quad(&b, grid[y][x], grid[y][x + 1], grid[y + 1][x + 1], grid[y + 1][x]);
    MeshData m = { b.verts, b.verts_len, b.idx, b.idx_len };
    return m;
}

static MeshData g_meshes[MESH_COUNT];
static int g_built = 0;

const MeshData *mesh_get(enum MeshId id) {
    if (!g_built) {
        g_meshes[MESH_BOX] = build_box();
        g_meshes[MESH_PLANE_XZ] = build_plane_xz();
        g_meshes[MESH_PLANE_XY] = build_plane_xy();
        g_meshes[MESH_CYLINDER] = build_cylinder();
        g_meshes[MESH_SPHERE] = build_sphere();
        g_built = 1;
    }
    if (id < 0 || id >= MESH_COUNT) id = MESH_BOX;
    return &g_meshes[id];
}
