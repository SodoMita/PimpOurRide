#include "math3d.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
static int near(float a,float b){return fabsf(a-b)<.001f;}
int main(void){
 Vec3 eye=v3(0,1,2.8f);float u,v,d;Plane p=monitor_plane(0);
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
 Plane axis=plane_translate_local(monitor_plane(0),eye,0,0,.5f);
 assert(near(axis.center.z,-1.1f));
 /* Verify 2-link telescopic linear-actuator monitor manipulators across all 8
  * app slots, deployment travel (reveal = 0..1), and freely moved/arranged
  * windows: stationary car base/shoulder, exact telescopic link lengths,
  * back-half-space safety, and dynamic linear actuator expansion. */
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
    assert(c_dist>.5f&&c_dist<2.5f);
   }
  }
  /* Freely moved/arranged window: moving the window away from its car mount
   * must expand the telescopic linear actuators while keeping base/shoulder
   * anchored to the car and elbow/wrist behind the moved plane. */
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
 }
 puts("PASS: nine output transforms, fixed/sliding/flip mounts, animated-plane picking, camera axes, tilted/rolled planes, arrange/handle transforms, telescopic manipulator IK");return 0;
}
