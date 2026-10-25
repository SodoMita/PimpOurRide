#include "math3d.h"
#include <math.h>
#include <stddef.h>
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
void plane_axes(Plane p,Vec3 *right,Vec3 *up,Vec3 *normal){
 /* Match scene.c plane_begin(): OpenGL applies yaw, then pitch, then the
  * in-plane roll call, so a local basis vector is Rz first, Rx second, Ry
  * last. Roll therefore rotates the window around its own normal. */
 Vec3 r=rotate_y(rotate_x(rotate_z(v3(1,0,0),p.roll),p.pitch),p.yaw);
 Vec3 u=rotate_y(rotate_x(rotate_z(v3(0,1,0),p.roll),p.pitch),p.yaw);
 if(right)*right=r;
 if(up)*up=u;
 if(normal)*normal=vcross(r,u);
}
bool plane_hit(Plane p,Vec3 origin,Vec3 ray,float *u,float *v,float *distance,bool clip){
 Vec3 right,up,normal;plane_axes(p,&right,&up,&normal);
 float den=vdot(ray,normal);
 if(den>=-1e-6f)return false;
 float t=vdot(vsub(p.center,origin),normal)/den;if(t<=.01f)return false;
 Vec3 q=vsub(vadd(origin,vmul(ray,t)),p.center);
 *u=vdot(q,right)/p.width+.5f;*v=.5f-vdot(q,up)/p.height;*distance=t;
 return !clip||(*u>=0&&*u<=1&&*v>=0&&*v<=1);
}
static float wrap_degrees(float a){
 while(a>180.f)a-=360.f;
 while(a<=-180.f)a+=360.f;
 return a;
}
static Plane plane_clamp_center(Plane p,Vec3 eye){
 /* Keep an arranged display inside the garage volume and out of the
  * driver's face. The default ring extends behind the eye (slot 8), hence
  * the generous Z range. */
 p.center.x=clampf(p.center.x,-8.f,8.f);
 p.center.y=clampf(p.center.y,.20f,4.f);
 p.center.z=clampf(p.center.z,-9.f,9.f);
 Vec3 rel=vsub(p.center,eye);float dist=sqrtf(vdot(rel,rel));
 if(dist<.8f&&dist>1e-6f)p.center=vadd(eye,vmul(rel,.8f/dist));
 else if(dist>12.f)p.center=vadd(eye,vmul(rel,12.f/dist));
 return p;
}
Plane plane_translate_local(Plane p,Vec3 eye,float right_amount,float up_amount,float normal_amount){
 Vec3 right,up,normal;plane_axes(p,&right,&up,&normal);
 p.center=vadd(p.center,vadd(vadd(vmul(right,right_amount),vmul(up,up_amount)),vmul(normal,normal_amount)));
 return plane_clamp_center(p,eye);
}
Plane plane_translate(Plane p,Vec3 eye,float right_amount,float up_amount,float toward_eye){
 p=plane_translate_local(p,eye,right_amount,up_amount,0);
 if(toward_eye!=0.f){
  Vec3 to_eye=vnorm(vsub(eye,p.center));
  p.center=vadd(p.center,vmul(to_eye,toward_eye));
  p=plane_clamp_center(p,eye);
 }
 return p;
}
Plane plane_rotate(Plane p,float yaw,float pitch,float roll){
 p.yaw=wrap_degrees(p.yaw+yaw);
 p.pitch=clampf(p.pitch+pitch,-85.f,85.f);
 p.roll=wrap_degrees(p.roll+roll);
 return p;
}
Plane plane_uniform_scale(Plane p,Plane base,float factor){
 if(base.width<=0.f||base.height<=0.f||p.width<=0.f||p.height<=0.f||factor<=0.f)return p;
 /* Preserve the plane's current aspect (which an edge resize may have
  * changed), while keeping both relative dimensions inside the limits. */
 float sx=p.width/base.width,sy=p.height/base.height;
 float lo=.35f/fminf(sx,sy),hi=2.75f/fmaxf(sx,sy);
 if(lo>hi)return p;
 float f=clampf(factor,lo,hi);
 p.width*=f;p.height*=f;
 return p;
}
Plane plane_resize(Plane p,Plane base,Vec3 eye,float width,float height,int fixed_x,int fixed_y){
 if(base.width<=0.f||base.height<=0.f)return p;
 float new_w=clampf(width,base.width*.35f,base.width*2.75f);
 float new_h=clampf(height,base.height*.35f,base.height*2.75f);
 float dw=new_w-p.width,dh=new_h-p.height;
 Vec3 right,up;plane_axes(p,&right,&up,NULL);
 /* fixed_x/fixed_y identify the stationary edge: -1 = left/bottom,
  * +1 = right/top, 0 = centre. Moving the centre by half the size
  * change in the opposite direction keeps that edge anchored, exactly
  * like dragging a normal window border. */
 if(fixed_x)p.center=vadd(p.center,vmul(right,-(float)fixed_x*dw*.5f));
 if(fixed_y)p.center=vadd(p.center,vmul(up,-(float)fixed_y*dh*.5f));
 p.width=new_w;p.height=new_h;
 return plane_clamp_center(p,eye);
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
/* Stationary chassis hardpoints and 2-link telescopic linear-actuator
 * kinematics for the eight mechanical manipulators holding monitors CD-01..08.
 * Each manipulator is anchored to a car-mounted turret below (slots 0..4, 7)
 * or above (roof slots 5..6) the screen's bezel and extends/retracts its
 * multi-stage telescopic linear actuators dynamically as the window Plane p
 * is deployed, dragged by its frame, or rotated/resized in 3D Arrange mode. */
static const Vec3 manip_base[8]={
 {-1.22f, 0.56f,-1.74f}, /* CD-01: forward cowl bulkhead turret in left inter-bay */
 {-3.24f, 0.52f,-0.82f}, /* CD-02: left dashboard outer cowl turret */
 { 3.24f, 0.52f,-0.82f}, /* CD-03: right dashboard outer cowl turret */
 {-5.05f, 0.44f, 0.88f}, /* CD-04: left door inner sill turret */
 { 5.05f, 0.44f, 0.88f}, /* CD-05: right door inner sill turret */
 {-3.28f, 2.42f,-0.14f}, /* CD-06: left roof cantrail hinge turret */
 { 3.28f, 2.42f,-0.14f}, /* CD-07: right roof cantrail hinge turret */
 { 0.38f, 0.44f, 7.52f}  /* CD-08: rear deck bulkhead pedestal */
};
static const Vec3 manip_shoulder[8]={
 {-1.18f, 0.68f,-1.70f},
 {-3.12f, 0.62f,-0.86f},
 { 3.12f, 0.62f,-0.86f},
 {-4.96f, 0.56f, 0.84f},
 { 4.96f, 0.56f, 0.84f},
 {-3.22f, 2.34f,-0.10f},
 { 3.22f, 2.34f,-0.10f},
 { 0.34f, 0.56f, 7.46f}
};
static const float manip_wrist_lat[8]={-0.46f,0.18f,-0.18f,0.10f,-0.10f,0.0f,0.0f,0.06f};
static const Vec3 manip_bend_hint[8]={
 {-0.65f, 0.25f,-0.35f},
 { 0.45f,-0.42f,-0.45f},
 {-0.45f,-0.42f,-0.45f},
 {-0.55f,-0.25f,-0.45f},
 { 0.55f,-0.25f,-0.45f},
 {-0.55f, 0.35f,-0.45f},
 { 0.55f, 0.35f,-0.45f},
 {-0.55f,-0.20f, 0.45f}
};
MonitorManipulator monitor_manipulator_for_plane(int index,Plane p){
 int slot=index<0?0:(index>7?7:index);
 Vec3 right,up,normal;plane_axes(p,&right,&up,&normal);
 MonitorManipulator m={.base=manip_base[slot],.shoulder=manip_shoulder[slot]};
 Vec3 wrist_local=slot==0
  ?vadd(vmul(right,manip_wrist_lat[slot]*p.width),vmul(up,p.height*.08f))
  :(slot==5||slot==6)
   ?vadd(vmul(right,manip_wrist_lat[slot]*p.width),vmul(up,p.height*.32f))
   :vadd(vmul(right,manip_wrist_lat[slot]*p.width),vmul(up,-p.height*.48f));
 m.wrist=vadd(p.center,vadd(wrist_local,vmul(normal,-.085f)));
 Vec3 delta=vsub(m.wrist,m.shoulder);
 float dist=sqrtf(vdot(delta,delta));
 Vec3 dir=dist>1e-6f?vmul(delta,1.f/dist):v3(0,0,-1);
 Vec3 bend=vadd(manip_bend_hint[slot],vmul(normal,-.45f));
 Vec3 perp_raw=vsub(bend,vmul(dir,vdot(bend,dir)));
 float plen=sqrtf(vdot(perp_raw,perp_raw));
 Vec3 perp=plen>1e-6f?vmul(perp_raw,1.f/plen):vnorm(vcross(dir,fabsf(dir.y)>.9f?v3(1,0,0):v3(0,1,0)));
 const float u_min=.14f,u_max=2.60f,f_min=.14f,f_max=2.60f;
 float ul=clampf(dist*.62f,u_min,u_max);
 float fl=clampf(dist*.62f,f_min,f_max);
 float d=clampf(dist,fabsf(ul-fl)+1e-4f,ul+fl-1e-4f);
 float a=(ul*ul-fl*fl+d*d)/(2.f*d);
 float h=sqrtf(fmaxf(0.f,ul*ul-a*a));
 m.elbow=vadd(m.shoulder,vadd(vmul(dir,a),vmul(perp,h)));
 float en=vdot(vsub(m.elbow,p.center),normal);
 if(en>-.06f)m.elbow=vadd(m.elbow,vmul(normal,-.06f-en));
 Vec3 u_vec=vsub(m.elbow,m.shoulder),f_vec=vsub(m.wrist,m.elbow);
 m.upper_len=sqrtf(vdot(u_vec,u_vec));
 m.fore_len=sqrtf(vdot(f_vec,f_vec));
 m.upper_ext=clampf((m.upper_len-u_min)/(u_max-u_min),0.f,1.f);
 m.fore_ext=clampf((m.fore_len-f_min)/(f_max-f_min),0.f,1.f);
 m.piston_base=vadd(m.base,vmul(perp,-.055f));
 m.piston_rod=vadd(vadd(m.shoulder,vmul(u_vec,.56f)),vmul(perp,-.042f));
 Vec3 p_vec=vsub(m.piston_rod,m.piston_base);
 float p_len=sqrtf(vdot(p_vec,p_vec));
 m.piston_ext=clampf((p_len-.18f)/1.40f,0.f,1.f);
 float w=p.width,hp=p.height;
 Vec3 back=vmul(normal,-.025f);
 m.claws[0]=vadd(vadd(p.center,back),vadd(vmul(right,-w*.36f),vmul(up, hp*.5f+.20f)));
 m.claws[1]=vadd(vadd(p.center,back),vadd(vmul(right, w*.36f),vmul(up, hp*.5f+.20f)));
 m.claws[2]=vadd(vadd(p.center,back),vadd(vmul(right,-w*.36f),vmul(up,-hp*.5f-.05f)));
 m.claws[3]=vadd(vadd(p.center,back),vadd(vmul(right, w*.36f),vmul(up,-hp*.5f-.05f)));
 return m;
}
MonitorManipulator monitor_manipulator_at(int index,float reveal){
 int slot=index<0?0:(index>7?7:index);
 return monitor_manipulator_for_plane(slot,monitor_plane_at(slot,reveal));
}
