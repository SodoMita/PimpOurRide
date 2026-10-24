#include "math3d.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int near(float a,float b){return fabsf(a-b)<.001f;}
int main(void){
 Vec3 eye=v3(0,1,2.8f);float u,v,d;Plane p=monitor_plane(0);
 assert(plane_hit(p,eye,vnorm(vsub(p.center,eye)),&u,&v,&d,true));assert(near(u,.5)&&near(v,.5));
 for(int i=0;i<8;i++){p=monitor_plane(i);assert(plane_hit(p,eye,vnorm(vsub(p.center,eye)),&u,&v,&d,true));assert(near(u,.5)&&near(v,.5));}
 p=monitor_plane(0);assert(!plane_hit(p,eye,v3(0,0,1),&u,&v,&d,true));assert(!plane_hit(p,eye,v3(1,0,0),&u,&v,&d,true));
 Vec3 r=camera_ray(800,500,1600,1000,55,0,0);assert(near(r.x,0)&&near(r.y,0)&&near(r.z,-1));
 r=camera_ray(800,500,1600,1000,55,90,0);assert(near(r.x,-1)&&near(r.z,0));
 p=(Plane){.center={.8f,0,.6f},.width=1.4f,.height=.7f,.yaw=-4,.pitch=-28};
 assert(plane_hit(p,eye,vnorm(vsub(p.center,eye)),&u,&v,&d,true));assert(near(u,.5)&&near(v,.5));
 Vec3 corner=vadd(p.center,vadd(vmul(rotate_y(v3(1,0,0),p.yaw),p.width*.49f),vmul(rotate_y(rotate_x(v3(0,1,0),p.pitch),p.yaw),p.height*.49f)));
 assert(plane_hit(p,eye,vnorm(vsub(corner,eye)),&u,&v,&d,true));assert(near(u,.99)&&near(v,.01));
 assert(!plane_hit(p,eye,vnorm(vsub(vadd(corner,v3(2,0,0)),eye)),&u,&v,&d,true));
 /* Roll is around the plane normal: at +90 degrees the local right axis
  * points world-up and local up points world-left. */
 p=monitor_plane(0);p.roll=90;
 Vec3 right,up,normal;plane_axes(p,&right,&up,&normal);
 assert(near(right.x,0)&&near(right.y,1)&&near(right.z,0));
 assert(near(up.x,-1)&&near(up.y,0)&&near(up.z,0));
 assert(near(normal.x,0)&&near(normal.y,0)&&near(normal.z,1));
 Vec3 rolled_point=vadd(p.center,vadd(vmul(right,p.width*.25f),vmul(up,p.height*.25f)));
 assert(plane_hit(p,eye,vnorm(vsub(rolled_point,eye)),&u,&v,&d,true));assert(near(u,.75)&&near(v,.25));
 /* Arrange transforms: local translation, eye-depth, wrapped/clamped
  * rotation, and aspect-preserving uniform scale with limits. */
 Plane moved=plane_translate(monitor_plane(0),eye,.5f,.25f,0);
 assert(near(moved.center.x,.5f)&&near(moved.center.y,1.57f)&&near(moved.center.z,-1.6f));
 float before=sqrtf(vdot(vsub(moved.center,eye),vsub(moved.center,eye)));
 moved=plane_translate(moved,eye,0,0,.4f);
 float after=sqrtf(vdot(vsub(moved.center,eye),vsub(moved.center,eye)));
 assert(near(before-after,.4f));
 Plane rotated=plane_rotate((Plane){.yaw=179,.pitch=84,.roll=-179},3,4,-3);
 assert(near(rotated.yaw,-178)&&near(rotated.pitch,85)&&near(rotated.roll,178));
 Plane scaled=plane_uniform_scale(monitor_plane(0),monitor_plane(0),10);
 assert(near(scaled.width,2.10f*2.75f)&&near(scaled.height,1.3125f*2.75f));
 scaled=plane_uniform_scale(scaled,monitor_plane(0),.01f);
 assert(near(scaled.width,2.10f*.35f)&&near(scaled.height,1.3125f*.35f));
 puts("PASS: monitor ray picking, all eight screen transforms, camera axes, tilted/rolled planes, arrange transforms, back-face rejection");return 0;
}
