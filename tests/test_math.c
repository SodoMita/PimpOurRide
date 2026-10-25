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
 /* Verify 2-link inverse-kinematic monitor manipulators across all 8 app slots
  * and deployment travel (reveal = 0, 0.5, 1.0): fixed chassis base/shoulder,
  * exact link lengths |elbow-shoulder|==upper_len and |wrist-elbow|==fore_len,
  * non-degenerate hydraulic piston span, and bezel claw attachment. */
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
   /* Every manipulator joint must stay in the back half-space behind its
    * monitor plane so no strut ever crosses in front of the screen. */
   Vec3 m_right=rotate_y(v3(1,0,0),mp.yaw),m_up=rotate_y(rotate_x(v3(0,1,0),mp.pitch),mp.yaw),m_norm=vcross(m_right,m_up);
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
 }
 puts("PASS: nine output transforms, fixed/sliding/flip mounts, animated-plane picking, camera axes, cockpit buttons, back-face rejection, manipulator IK");return 0;
}
