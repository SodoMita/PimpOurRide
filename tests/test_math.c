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
 /* Handle operations: a right-edge resize leaves the left edge fixed, a
  * top-edge resize leaves the bottom fixed, uniform scale then retains
  * that resized aspect, and the Z handle moves along the plane normal. */
 Plane resized=plane_resize(monitor_plane(0),monitor_plane(0),eye,3.15f,1.3125f,-1,0);
 assert(near(resized.center.x,.525f)&&near(resized.center.x-resized.width/2,-1.05f));
 resized=plane_resize(resized,monitor_plane(0),eye,resized.width,2.625f,0,-1);
 assert(near(resized.center.y,1.97625f)&&near(resized.center.y-resized.height/2,.66375f));
 float ratio=resized.width/resized.height;
 Plane aspect=plane_uniform_scale(resized,monitor_plane(0),1.1f);
 assert(near(aspect.width/aspect.height,ratio));
 /* Shift+corner resize: a single factor preserves the drag-start aspect
  * exactly, stays within both base limits, and keeps the opposite corner
  * anchored even when one dimension reaches its limit first. */
 Plane aspect_base=monitor_plane(0);
 Plane start=(Plane){.center={.4f,1.2f,-1.2f},.width=3.15f,.height=1.3125f,.yaw=12,.pitch=-18};
 float start_aspect=start.width/start.height;
 Plane grew=plane_resize_aspect(start,aspect_base,eye,start.width*1.2f,start.height*1.2f,-1,1);
 assert(near(grew.width/start.width,1.2f)&&near(grew.height/start.height,1.2f));
 assert(near(grew.width/grew.height,start_aspect));
 Vec3 gr,gu;plane_axes(start,&gr,&gu,NULL);
 Vec3 fixed_before=vadd(vsub(start.center,vmul(gr,start.width*.5f)),vmul(gu,start.height*.5f));
 Vec3 fixed_after=vadd(vsub(grew.center,vmul(gr,grew.width*.5f)),vmul(gu,grew.height*.5f));
 assert(near(fixed_after.x,fixed_before.x)&&near(fixed_after.y,fixed_before.y)&&near(fixed_after.z,fixed_before.z));
 Plane shrank=plane_resize_aspect(start,aspect_base,eye,start.width*.6f,start.height*.6f,1,-1);
 assert(near(shrank.width/start.width,.6f)&&near(shrank.height/start.height,.6f));
 assert(near(shrank.width/shrank.height,start_aspect));
 Plane limited=plane_resize_aspect(start,aspect_base,eye,start.width*3.f,start.height*3.f,-1,1);
 assert(near(limited.width,aspect_base.width*2.75f));
 assert(near(limited.width/limited.height,start_aspect));
 assert(limited.height<aspect_base.height*2.75f);
 fixed_before=vadd(vsub(start.center,vmul(gr,start.width*.5f)),vmul(gu,start.height*.5f));
 fixed_after=vadd(vsub(limited.center,vmul(gr,limited.width*.5f)),vmul(gu,limited.height*.5f));
 assert(near(fixed_after.x,fixed_before.x)&&near(fixed_after.y,fixed_before.y)&&near(fixed_after.z,fixed_before.z));
 Plane axis=plane_translate_local(monitor_plane(0),eye,0,0,.5f);
 assert(near(axis.center.z,-1.1f));
 puts("PASS: monitor ray picking, all eight screen transforms, camera axes, tilted/rolled planes, arrange/handle transforms, aspect-locked resize, back-face rejection");return 0;
}
