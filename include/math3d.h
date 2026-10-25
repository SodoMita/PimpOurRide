#ifndef CD_MATH3D_H
#define CD_MATH3D_H
#include <stdbool.h>
#define CD_DRIVER_FOV_MIN 1.0f
#define CD_DRIVER_FOV_MAX 100.0f
#define CD_DRIVER_SCROLL_FOV_MAX 75.0f
typedef struct { float x,y,z; } Vec3;
typedef struct { Vec3 center; float width,height,yaw,pitch,roll; } Plane;
Vec3 v3(float x,float y,float z);
Vec3 vadd(Vec3 a,Vec3 b);
Vec3 vsub(Vec3 a,Vec3 b);
Vec3 vmul(Vec3 a,float k);
float vdot(Vec3 a,Vec3 b);
Vec3 vcross(Vec3 a,Vec3 b);
Vec3 vnorm(Vec3 v);
Vec3 rotate_y(Vec3 v,float deg);
Vec3 rotate_x(Vec3 v,float deg);
Vec3 rotate_z(Vec3 v,float deg);
Vec3 camera_ray(float x,float y,int w,int h,float fov,float yaw,float pitch);
void plane_axes(Plane p,Vec3 *right,Vec3 *up,Vec3 *normal);
bool plane_hit(Plane p,Vec3 origin,Vec3 ray,float *u,float *v,float *distance,bool clip);
void plane_close_rect(Plane p,float *cx,float *cy,float *w,float *h);
Plane plane_translate(Plane p,Vec3 eye,float right,float up,float toward_eye);
Plane plane_translate_local(Plane p,Vec3 eye,float right,float up,float normal);
Plane plane_rotate(Plane p,float yaw,float pitch,float roll);
Plane plane_uniform_scale(Plane p,Plane base,float factor);
Plane plane_resize(Plane p,Plane base,Vec3 eye,float width,float height,int fixed_x,int fixed_y);
Plane plane_resize_aspect(Plane p,Plane base,Vec3 eye,float width,float height,int fixed_x,int fixed_y);
void plane_window_pixels(Plane start,Plane resized,int start_w,int start_h,int *out_w,int *out_h);
Plane plane_resize_window_pixels(Plane start,Plane base,Vec3 eye,int start_w,int start_h,int win_w,int win_h,int fixed_x,int fixed_y);
Plane plane_resize_window_pixels_aspect(Plane start,Plane base,Vec3 eye,int start_w,int start_h,int win_w,int win_h,int fixed_x,int fixed_y);
typedef enum {MOUNT_WINDSHIELD,MOUNT_DASH_SLIDE,MOUNT_DOOR_SLIDE,MOUNT_ROOF_FLIP,MOUNT_REAR_FIXED,MOUNT_CLUSTER} MonitorMount;
typedef struct {
 Vec3 base,shoulder,elbow,wrist;
 Vec3 piston_base,piston_rod;
 Vec3 claws[4];
 float upper_len,fore_len;
 float upper_ext,fore_ext,piston_ext;
} MonitorManipulator;
Plane monitor_plane(int index); /* fully deployed transform */
Plane windshield_hud_plane(void); /* fitted projection on the shared windshield */
Plane monitor_plane_at(int index,float reveal); /* shared by drawing and picking */
MonitorManipulator monitor_manipulator_for_plane(int index,Plane p);
MonitorManipulator monitor_manipulator_at(int index,float reveal);
MonitorMount monitor_mount(int index);
const char *monitor_mount_name(int index);
float clampf(float v,float lo,float hi);
#endif
