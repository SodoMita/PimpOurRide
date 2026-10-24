/*
 * mesh.h - procedural unit meshes.
 *
 * All geometry is generated in code (no model files to ship/load): a unit
 * cube, two unit planes, a unit cylinder and a unit sphere, each centered
 * on the origin with extent 1. scene.c scales them via node transforms, so
 * "a pillar" is a cube scaled (1.2, 8.5, 1.0) and "a neon tube" is a
 * cylinder scaled (0.07, 3.4, 0.07). The renderer uploads each mesh once
 * into a VAO/VBO/IBO and reuses it for every node - important on mobile
 * GPUs where VAO switches are cheap but buffer uploads are not.
 *
 * Vertex layout is interleaved and fixed for every mesh:
 *   float pos[3], normal[3], uv[2]   (stride 8 floats)
 * The shader locations (0,1,2) are bound to this order in renderer.c.
 */
#ifndef GARAGE3D_MESH_H
#define GARAGE3D_MESH_H

#include <stddef.h>

enum MeshId {
    MESH_BOX = 0,   /* unit cube, 24 verts (flat normals per face) */
    MESH_PLANE_XZ,  /* unit quad in XZ, normal +Y (floors/ceilings) */
    MESH_PLANE_XY,  /* unit quad in XY, normal +Z (walls, city, mist) */
    MESH_CYLINDER,  /* radius 0.5, height 1, axis +Y, capped */
    MESH_SPHERE,    /* radius 0.5 UV sphere */
    MESH_COUNT
};

typedef struct {
    const float *vertices;    /* interleaved, see above */
    size_t vertex_floats;
    const unsigned short *indices;
    size_t index_count;
} MeshData;

#define MESH_VERTEX_STRIDE_FLOATS 8

/* Builds (once, lazily) and returns CPU-side mesh data. No GL here, so the
 * unit tests can validate index bounds without a context. */
const MeshData *mesh_get(enum MeshId id);

#endif
