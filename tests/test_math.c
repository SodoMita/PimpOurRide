#include "math3d.h"
#include "vehicle.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int near(float a,float b){return fabsf(a-b)<.001f;}
static int close_hit(Plane p,float x,float y){float cx,cy,w,h;plane_close_rect(p,&cx,&cy,&w,&h);return fabsf(x-cx)<=w*.5f&&fabsf(y-cy)<=h*.5f;}
int main(void){
 Vec3 eye=v3(0,1,2.8f-VEHICLE_COCKPIT_FORWARD);float u,v,d;Plane p=monitor_plane(0);
 assert(plane_hit(p,eye,vnorm(vsub(p.center,eye)),&u,&v,&d,true));assert(near(u,.5)&&near(v,.5));
 for(int i=0;i<9;i++){
  p=monitor_plane(i);assert(plane_hit(p,eye,vnorm(vsub(p.center,eye)),&u,&v,&d,true));assert(near(u,.5)&&near(v,.5));
  Plane deployed=monitor_plane_at(i,1),mid=monitor_plane_at(i,.5f);
  assert(near(deployed.center.x,p.center.x)&&near(deployed.center.y,p.center.y)&&near(deployed.center.z,p.center.z));
  assert(plane_hit(mid,eye,vnorm(vsub(mid.center,eye)),&u,&v,&d,true));assert(near(u,.5)&&near(v,.5));
  if(i==0||i>=7){Plane fixed=monitor_plane_at(i,0);assert(near(fixed.center.x,p.center.x)&&near(fixed.center.y,p.center.y)&&near(fixed.center.z,p.center.z));}
  else{Plane stowed=monitor_plane_at(i,0);assert(fabsf(stowed.center.x-p.center.x)+fabsf(stowed.center.y-p.center.y)+fabsf(stowed.pitch-p.pitch)>.1f);}
 }
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
 Plane base=monitor_plane(0);
 Plane moved=plane_translate(base,eye,.5f,.25f,0);
 assert(near(moved.center.x,base.center.x+.5f)&&near(moved.center.y,base.center.y+.25f)&&near(moved.center.z,base.center.z));
 float before=sqrtf(vdot(vsub(moved.center,eye),vsub(moved.center,eye)));
 moved=plane_translate(moved,eye,0,0,.4f);
 float after=sqrtf(vdot(vsub(moved.center,eye),vsub(moved.center,eye)));
 assert(near(before-after,.4f));
 Plane rotated=plane_rotate((Plane){.yaw=179,.pitch=84,.roll=-179},3,4,-3);
 assert(near(rotated.yaw,-178)&&near(rotated.pitch,85)&&near(rotated.roll,178));
 Plane scaled=plane_uniform_scale(base,base,10);
 assert(near(scaled.width,base.width*2.75f)&&near(scaled.height,base.height*2.75f));
 scaled=plane_uniform_scale(scaled,base,.01f);
 assert(near(scaled.width,base.width*.35f)&&near(scaled.height,base.height*.35f));
 /* Handle operations: a right-edge resize leaves the left edge fixed, a
  * top-edge resize leaves the bottom fixed, uniform scale then retains
  * that resized aspect, and the Z handle moves along the plane normal. */
 Plane resized=plane_resize(base,base,eye,base.width*1.5f,base.height,-1,0);
 assert(near(resized.center.x,base.center.x+base.width*.25f)&&near(resized.center.x-resized.width/2,base.center.x-base.width/2));
 resized=plane_resize(resized,base,eye,resized.width,base.height*2.f,0,-1);
 assert(near(resized.center.y,base.center.y+base.height*.5f)&&near(resized.center.y-resized.height/2,base.center.y-base.height/2));
 float ratio=resized.width/resized.height;
 Plane aspect=plane_uniform_scale(resized,base,1.1f);
 assert(near(aspect.width/aspect.height,ratio));
 /* White-grip resizes request new logical window dimensions: the same
  * 1.5x-by-2x plane above is 1500x1250 from a 1000x625 start. A request
  * clamped by client hints maps back to an anchored plane on the current
  * monitor geometry, and Shift+corner keeps the drag-start aspect. */
 int win_w=0,win_h=0;
 plane_window_pixels(base,resized,1000,625,&win_w,&win_h);
 assert(win_w==1500&&win_h==1250);
 Plane hinted=plane_resize_window_pixels(base,base,eye,1000,625,1150,625,-1,0);
 assert(near(hinted.width,base.width*1.15f)&&near(hinted.height,base.height));
 assert(near(hinted.center.x-hinted.width/2,base.center.x-base.width/2));
 Plane hinted_aspect=plane_resize_window_pixels_aspect(base,base,eye,1000,625,1150,625,-1,0);
 assert(near(hinted_aspect.width,hinted.width)&&near(hinted_aspect.height,base.height*1.15f));
 assert(near(hinted_aspect.width/hinted_aspect.height,base.width/base.height));
 assert(near(hinted_aspect.center.x-hinted_aspect.width/2,base.center.x-base.width/2));
 /* Shift+corner resize: a single factor preserves the drag-start aspect
  * exactly, stays within both base limits, and keeps the opposite corner
  * anchored even when one dimension reaches its limit first. */
 Plane aspect_base=monitor_plane(0);
 Plane start=(Plane){.center={.4f,1.2f,-1.2f},.width=2.40f,.height=1.00f,.yaw=12,.pitch=-18};
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
 Plane axis=plane_translate_local(base,eye,0,0,.5f);
 assert(near(axis.center.z,base.center.z+.5f));
 /* The titlebar X is a fixed-size texture element. Its pick rectangle must
  * follow the glyph in local metres at every monitor height and transform,
  * rather than using a v range that only fits the original .75 m display. */
 const float heights[]={.30f,.50f,.75f,1.50f,2.10f};
 for(unsigned i=0;i<sizeof(heights)/sizeof(heights[0]);i++){
  Plane close_plane=monitor_plane(0);close_plane.height=heights[i];close_plane.center.x+=.37f;close_plane.yaw=13;close_plane.pitch=-9;close_plane.roll=21;
  float cx,cy,w,h;plane_close_rect(close_plane,&cx,&cy,&w,&h);
  assert(close_hit(close_plane,cx,cy));assert(!close_hit(close_plane,cx+w,cy));assert(!close_hit(close_plane,0,0));
  Vec3 cr,cu;plane_axes(close_plane,&cr,&cu,NULL);
  Vec3 close_point=vadd(close_plane.center,vadd(vmul(cr,cx),vmul(cu,cy)));
  assert(plane_hit(close_plane,eye,vnorm(vsub(close_point,eye)),&u,&v,&d,false));
  assert(close_hit(close_plane,(u-.5f)*close_plane.width,(.5f-v)*close_plane.height));
 }
 assert(near(clampf(-100,CD_DRIVER_FOV_MIN,CD_DRIVER_FOV_MAX),1));
 assert(near(clampf(500,CD_DRIVER_FOV_MIN,CD_DRIVER_FOV_MAX),100));
 assert(near(clampf(-100,CD_DRIVER_FOV_MIN,CD_DRIVER_SCROLL_FOV_MAX),1));
 /* Verify telescopic monitor manipulators across all 8 app slots, deployment
  * travel, and freely moved/rotated windows. Car-part base/shoulder anchors
  * stay fixed, reported lengths exactly match the solved geometry, actuator
  * extension stays normalized, and elbow/wrist never cross the screen. */
 for(int i=0;i<8;i++){
  MonitorManipulator m0=monitor_manipulator_at(i,0.f);
  for(int step=0;step<=4;step++){
   float rev=.25f*(float)step;
   MonitorManipulator m=monitor_manipulator_at(i,rev);
   Plane mp=monitor_plane_at(i,rev);
   assert(near(m.base.x,m0.base.x)&&near(m.base.y,m0.base.y)&&near(m.base.z,m0.base.z));
   assert(near(m.shoulder.x,m0.shoulder.x)&&near(m.shoulder.y,m0.shoulder.y)&&near(m.shoulder.z,m0.shoulder.z));
   Vec3 u_vec=vsub(m.elbow,m.shoulder),f_vec=vsub(m.wrist,m.elbow),p_vec=vsub(m.piston_rod,m.piston_base);
   float u_len=sqrtf(vdot(u_vec,u_vec)),f_len=sqrtf(vdot(f_vec,f_vec)),p_len=sqrtf(vdot(p_vec,p_vec));
   assert(near(u_len,m.upper_len)&&near(f_len,m.fore_len));
   assert(p_len>.05f&&p_len<m.upper_len+m.fore_len);
   assert(m.upper_ext>=0.f&&m.upper_ext<=1.f&&m.fore_ext>=0.f&&m.fore_ext<=1.f&&m.piston_ext>=0.f&&m.piston_ext<=1.f);
   Vec3 m_right,m_up,m_norm;plane_axes(mp,&m_right,&m_up,&m_norm);
   assert(vdot(vsub(m.base,mp.center),m_norm)<=-.04f);
   assert(vdot(vsub(m.shoulder,mp.center),m_norm)<=-.04f);
   assert(vdot(vsub(m.elbow,mp.center),m_norm)<=-.04f);
   assert(vdot(vsub(m.wrist,mp.center),m_norm)<=-.04f);
   for(int k=0;k<4;k++){
    Vec3 c_off=vsub(m.claws[k],mp.center);
    float c_dist=sqrtf(vdot(c_off,c_off));
    assert(c_dist>.5f&&c_dist<2.0f);
   }
  }
  /* A free frame/Arrange transform changes telescopic extension, not the
   * fixed mount anchors. This exercises the actual public live-plane API. */
  Plane def=monitor_plane(i);
  MonitorManipulator md=monitor_manipulator_for_plane(i,def);
  Plane dragged=plane_rotate(plane_translate(def,eye,.45f,(i==5||i==6)?-.55f:.55f,.50f),8,-6,10);
  MonitorManipulator mf=monitor_manipulator_for_plane(i,dragged);
  assert(near(mf.base.x,md.base.x)&&near(mf.base.y,md.base.y)&&near(mf.base.z,md.base.z));
  assert(near(mf.shoulder.x,md.shoulder.x)&&near(mf.shoulder.y,md.shoulder.y)&&near(mf.shoulder.z,md.shoulder.z));
  assert(fabsf(mf.upper_ext-md.upper_ext)+fabsf(mf.fore_ext-md.fore_ext)>.02f);
  Vec3 d_right,d_up,d_norm;plane_axes(dragged,&d_right,&d_up,&d_norm);
  assert(vdot(vsub(mf.elbow,dragged.center),d_norm)<=-.04f);
  assert(vdot(vsub(mf.wrist,dragged.center),d_norm)<=-.04f);
  if(i==3){
   /* The renderer's door path supplies a hinged anchor while the live window
    * plane can move independently. Only the car-side joints may follow that
    * anchor; an unchanged window keeps the exact same wrist position. */
   Plane hinged=def;hinged.center.x+=.40f;hinged.yaw+=20;
   MonitorManipulator hm=monitor_manipulator_for_plane_on_mount(i,def,hinged);
   Vec3 anchor_delta=vsub(hm.base,md.base);
   assert(sqrtf(vdot(anchor_delta,anchor_delta))>.01f);
   assert(near(hm.wrist.x,md.wrist.x)&&near(hm.wrist.y,md.wrist.y)&&near(hm.wrist.z,md.wrist.z));
  }
 }
 puts("PASS: output/mount transforms, Arrange resizing, close targets, FOV bounds, back-face rejection, and telescopic manipulator IK");return 0;
}
