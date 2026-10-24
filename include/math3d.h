#ifndef CD_MATH3D_H
#define CD_MATH3D_H
#include <stdbool.h>
typedef struct { float x,y,z; } Vec3;
typedef struct { Vec3 center; float width,height,yaw,pitch; } Plane;
Vec3 v3(float x,float y,float z);
Vec3 vadd(Vec3 a,Vec3 b);
Vec3 vsub(Vec3 a,Vec3 b);
Vec3 vmul(Vec3 a,float k);
float vdot(Vec3 a,Vec3 b);
Vec3 vcross(Vec3 a,Vec3 b);
Vec3 vnorm(Vec3 v);
Vec3 rotate_y(Vec3 v,float deg);
Vec3 rotate_x(Vec3 v,float deg);
Vec3 camera_ray(float x,float y,int w,int h,float fov,float yaw,float pitch);
bool plane_hit(Plane p,Vec3 origin,Vec3 ray,float *u,float *v,float *distance,bool clip);
Plane monitor_plane(int index);
float clampf(float v,float lo,float hi);
#endif
