#include "math3d.h"
#include <math.h>
#define RAD (3.14159265358979323846f/180.f)
Vec3 v3(float x,float y,float z){return (Vec3){x,y,z};}
Vec3 vadd(Vec3 a,Vec3 b){return v3(a.x+b.x,a.y+b.y,a.z+b.z);}
Vec3 vsub(Vec3 a,Vec3 b){return v3(a.x-b.x,a.y-b.y,a.z-b.z);}
Vec3 vmul(Vec3 a,float k){return v3(a.x*k,a.y*k,a.z*k);}
float vdot(Vec3 a,Vec3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}
Vec3 vcross(Vec3 a,Vec3 b){return v3(a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x);}
Vec3 vnorm(Vec3 v){float n=sqrtf(vdot(v,v));return n>1e-9f?vmul(v,1.f/n):v3(0,0,0);}
Vec3 rotate_y(Vec3 v,float deg){float c=cosf(deg*RAD),s=sinf(deg*RAD);return v3(c*v.x+s*v.z,v.y,-s*v.x+c*v.z);}
Vec3 rotate_x(Vec3 v,float deg){float c=cosf(deg*RAD),s=sinf(deg*RAD);return v3(v.x,c*v.y-s*v.z,s*v.y+c*v.z);}
Vec3 rotate_z(Vec3 v,float deg){float c=cosf(deg*RAD),s=sinf(deg*RAD);return v3(c*v.x-s*v.y,s*v.x+c*v.y,v.z);}
float clampf(float v,float lo,float hi){return fminf(hi,fmaxf(lo,v));}
Vec3 camera_ray(float x,float y,int w,int h,float fov,float yaw,float pitch){
 float t=tanf(fov*RAD*.5f);Vec3 d=v3((2*x/w-1)*((float)w/h)*t,(1-2*y/h)*t,-1);
 return vnorm(rotate_y(rotate_x(d,pitch),yaw));
}
bool plane_hit(Plane p,Vec3 origin,Vec3 ray,float *u,float *v,float *distance,bool clip){
 Vec3 right=rotate_y(v3(1,0,0),p.yaw),up=rotate_y(rotate_x(v3(0,1,0),p.pitch),p.yaw);
 Vec3 normal=vcross(right,up);float den=vdot(ray,normal);
 if(den>=-1e-6f)return false;
 float t=vdot(vsub(p.center,origin),normal)/den;if(t<=.01f)return false;
 Vec3 q=vsub(vadd(origin,vmul(ray,t)),p.center);
 *u=vdot(q,right)/p.width+.5f;*v=.5f-vdot(q,up)/p.height;*distance=t;
 return !clip||(*u>=0&&*u<=1&&*v>=0&&*v<=1);
}
MonitorMount monitor_mount(int index){
 if(index==8)return MOUNT_CLUSTER;
 static const MonitorMount mounts[8]={MOUNT_WINDSHIELD,MOUNT_DASH_SLIDE,MOUNT_DASH_SLIDE,MOUNT_DOOR_SLIDE,MOUNT_DOOR_SLIDE,MOUNT_ROOF_FLIP,MOUNT_ROOF_FLIP,MOUNT_REAR_FIXED};
 return mounts[index&7];
}
const char *monitor_mount_name(int index){
 static const char *names[]={"WINDSHIELD / FIXED","DASH / SLIDE-UP","DOOR / SLIDE-OUT","ROOF / FLIP-DOWN","REAR DECK / FIXED","INSTRUMENT PANEL"};
 return names[monitor_mount(index)];
}
Plane monitor_plane(int index){
 if(index==8)return (Plane){.center={-.94f,.43f,.28f},.width=1.46f,.height=.59f,.pitch=-12};
 if(index==5)return (Plane){.center={-3.0f,1.75f,.20f},.width=2.10f,.height=1.3125f,.yaw=45};
 if(index==6)return (Plane){.center={ 3.0f,1.75f,.20f},.width=2.10f,.height=1.3125f,.yaw=-45};
 static const float angles[]={0,-30,30,-62,62,0,0,180};float a=angles[index&7];
 return (Plane){.center={4.4f*sinf(a*RAD),1.32f,2.8f-4.4f*cosf(a*RAD)},.width=2.10f,.height=1.3125f,.yaw=-a,.pitch=0};
}
Plane monitor_plane_at(int index,float reveal){
 Plane deployed=monitor_plane(index),stowed=deployed;MonitorMount mount=monitor_mount(index);
 if(mount==MOUNT_WINDSHIELD||mount==MOUNT_REAR_FIXED||mount==MOUNT_CLUSTER)return deployed;
 if(mount==MOUNT_DASH_SLIDE){stowed.center.y-=.95f;stowed.center.z+=.18f;}
 else if(mount==MOUNT_DOOR_SLIDE){stowed.center.x+=index==3?-.85f:.85f;stowed.center.y-=.15f;stowed.center.z+=.25f;}
 else if(mount==MOUNT_ROOF_FLIP){stowed.center.y+=.50f;stowed.pitch=78;}
 float t=clampf(reveal,0,1);t=t*t*(3-2*t); /* smooth mechanical travel, not a linear pop */
 deployed.center=vadd(stowed.center,vmul(vsub(deployed.center,stowed.center),t));
 deployed.pitch=stowed.pitch+(deployed.pitch-stowed.pitch)*t;
 return deployed;
}
/* Stationary chassis hardpoints and 2-link inverse-kinematic parameters for the
 * eight articulated mechanical manipulators holding monitors CD-01 .. CD-08.
 * Every base, shoulder, elbow, and wrist sits in the back half-space behind
 * its monitor plane (vdot(joint - p.center, normal) <= -0.04) and bends outward
 * into the visible inter-monitor or outer-cockpit bays so the driver sees the
 * articulated manipulator linkage without any strut ever crossing in front of
 * a monitor screen or the center console. */
static const Vec3 manip_base[8]={
 {-0.78f, 2.32f,-1.78f}, /* CD-01: upper-left windshield header gantry */
 {-3.32f, 0.24f,-0.94f}, /* CD-02: outer-left dashboard wing turret */
 { 3.32f, 0.24f,-0.94f}, /* CD-03: outer-right dashboard wing turret */
 {-5.05f, 0.48f, 0.86f}, /* CD-04: left door inner sill turret */
 { 5.05f, 0.48f, 0.86f}, /* CD-05: right door inner sill turret */
 {-3.28f, 2.39f,-0.12f}, /* CD-06: left roof cantrail hinge turret */
 { 3.28f, 2.39f,-0.12f}, /* CD-07: right roof cantrail hinge turret */
 { 0.00f, 0.56f, 7.52f}  /* CD-08: rear deck bulkhead pedestal */
};
static const Vec3 manip_shoulder[8]={
 {-0.92f, 2.16f,-1.82f},
 {-3.22f, 0.42f,-1.00f},
 { 3.22f, 0.42f,-1.00f},
 {-5.12f, 0.64f, 0.84f},
 { 5.12f, 0.64f, 0.84f},
 {-3.24f, 2.28f,-0.08f},
 { 3.24f, 2.28f,-0.08f},
 { 0.00f, 0.72f, 7.46f}
};
static const float manip_upper_len[8]={.64f,.42f,.42f,.94f,.94f,.48f,.48f,.52f};
static const float manip_fore_len[8] ={1.18f,.88f,.88f,.90f,.90f,.44f,.44f,.48f};
static const Vec3 manip_bend_hint[8]={
 {-0.95f,-0.35f,-0.18f},
 { 0.65f,-0.72f,-0.35f},
 {-0.65f,-0.72f,-0.35f},
 {-0.65f,-0.55f,-0.52f},
 { 0.65f,-0.55f,-0.52f},
 {-0.72f, 0.45f,-0.52f},
 { 0.72f, 0.45f,-0.52f},
 { 0.55f,-0.75f, 0.36f}
};
static const float manip_wrist_lat[8] ={0.f,-.32f,.32f,0.f,0.f,0.f,0.f,0.f};
static const float manip_wrist_vert[8]={0.f,-.12f,-.12f,0.f,0.f,0.f,0.f,0.f};
MonitorManipulator monitor_manipulator_at(int index,float reveal){
 int slot=index<0?0:(index>7?7:index);
 Plane p=monitor_plane_at(slot,reveal);
 Vec3 right=rotate_y(v3(1,0,0),p.yaw),up=rotate_y(rotate_x(v3(0,1,0),p.pitch),p.yaw),normal=vcross(right,up);
 MonitorManipulator m={
  .base=manip_base[slot],.shoulder=manip_shoulder[slot],
  .wrist=vadd(vadd(vadd(p.center,vmul(right,manip_wrist_lat[slot])),vmul(up,manip_wrist_vert[slot])),vmul(normal,-.09f)),
  .upper_len=manip_upper_len[slot],.fore_len=manip_fore_len[slot]
 };
 Vec3 delta=vsub(m.wrist,m.shoulder);
 float dist=sqrtf(vdot(delta,delta));
 Vec3 dir=dist>1e-6f?vmul(delta,1.f/dist):v3(0,0,-1);
 Vec3 bend=manip_bend_hint[slot];
 Vec3 perp_raw=vsub(bend,vmul(dir,vdot(bend,dir)));
 float plen=sqrtf(vdot(perp_raw,perp_raw));
 Vec3 perp=plen>1e-6f?vmul(perp_raw,1.f/plen):vnorm(vcross(dir,fabsf(dir.y)>.9f?v3(1,0,0):v3(0,1,0)));
 float ul=m.upper_len,fl=m.fore_len;
 float d=clampf(dist,fabsf(ul-fl)+1e-4f,ul+fl-1e-4f);
 float a=(ul*ul-fl*fl+d*d)/(2.f*d);
 float h=sqrtf(fmaxf(0.f,ul*ul-a*a));
 m.elbow=vadd(m.shoulder,vadd(vmul(dir,a),vmul(perp,h)));
 m.piston_base=vadd(m.base,vmul(vsub(m.shoulder,m.base),.35f));
 m.piston_rod=vadd(m.shoulder,vmul(vsub(m.elbow,m.shoulder),.62f));
 float w=p.width,hp=p.height;
 Vec3 back=vmul(normal,-.025f);
 m.claws[0]=vadd(vadd(p.center,back),vadd(vmul(right,-w*.38f),vmul(up, hp*.5f+.20f)));
 m.claws[1]=vadd(vadd(p.center,back),vadd(vmul(right, w*.38f),vmul(up, hp*.5f+.20f)));
 m.claws[2]=vadd(vadd(p.center,back),vadd(vmul(right,-w*.38f),vmul(up,-hp*.5f-.05f)));
 m.claws[3]=vadd(vadd(p.center,back),vadd(vmul(right, w*.38f),vmul(up,-hp*.5f-.05f)));
 return m;
}
