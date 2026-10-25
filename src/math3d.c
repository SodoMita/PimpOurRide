#include "math3d.h"
#include "vehicle.h"
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
/* The close glyph is part of ui_monitor_label()'s fixed 1200x76 titlebar
 * texture, not a normalized sub-rectangle of the application. Keep its hit
 * target in plane-local metres: v-based tests drift away from the glyph as
 * soon as a monitor is resized, while local coordinates remain valid after
 * every translate/rotate/scale operation. The generous target surrounds the
 * visible 18 px X without covering the application below the titlebar. */
void plane_close_rect(Plane p,float *cx,float *cy,float *w,float *h){
 if(cx)*cx=p.width*(1159.f/1200.f-.5f);
 if(cy)*cy=p.height*.5f+.011f+.137f*(1.f-39.f/76.f);
 if(w)*w=fmaxf(.12f,p.width*.04f);
 if(h)*h=.11f;
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
Plane plane_resize_aspect(Plane p,Plane base,Vec3 eye,float width,float height,int fixed_x,int fixed_y){
 if(base.width<=0.f||base.height<=0.f||p.width<=0.f||p.height<=0.f)return p;
 /* Shift+corner resize: one factor for both axes keeps the drag-start
  * aspect exact even when one dimension reaches its independent limit. */
 float factor=fmaxf(width/p.width,height/p.height);
 float lo=fmaxf(base.width*.35f/p.width,base.height*.35f/p.height);
 float hi=fminf(base.width*2.75f/p.width,base.height*2.75f/p.height);
 if(lo>hi)return p;
 factor=clampf(factor,lo,hi);
 float new_w=p.width*factor,new_h=p.height*factor;
 float dw=new_w-p.width,dh=new_h-p.height;
 Vec3 right,up;plane_axes(p,&right,&up,NULL);
 if(fixed_x)p.center=vadd(p.center,vmul(right,-(float)fixed_x*dw*.5f));
 if(fixed_y)p.center=vadd(p.center,vmul(up,-(float)fixed_y*dh*.5f));
 p.width=new_w;p.height=new_h;
 return plane_clamp_center(p,eye);
}
static int plane_dim_pixels(float start_dim,float new_dim,int start_px){
 if(start_px<=0)return 1;
 if(start_dim<=0.f||new_dim<=0.f)return start_px;
 int px=(int)lrintf((float)start_px*new_dim/start_dim);
 return px<1?1:px;
}
void plane_window_pixels(Plane start,Plane resized,int start_w,int start_h,int *out_w,int *out_h){
 /* Convert a white-grip plane resize into logical window pixels using the
  * density at drag start. The magenta uniform-scale handle is the separate
  * operation that changes that density; a border resize asks the app for
  * different dimensions instead of only stretching its old buffer. */
 if(out_w)*out_w=plane_dim_pixels(start.width,resized.width,start_w);
 if(out_h)*out_h=plane_dim_pixels(start.height,resized.height,start_h);
}
static Plane plane_resize_window_pixels_impl(Plane start,Plane base,Vec3 eye,int start_w,int start_h,int win_w,int win_h,int fixed_x,int fixed_y,bool keep_aspect){
 if(start.width<=0.f||start.height<=0.f||start_w<=0||start_h<=0||win_w<=0||win_h<=0)return start;
 float w=start.width*(float)win_w/(float)start_w;
 float h=start.height*(float)win_h/(float)start_h;
 return keep_aspect?plane_resize_aspect(start,base,eye,w,h,fixed_x,fixed_y)
                   :plane_resize(start,base,eye,w,h,fixed_x,fixed_y);
}
Plane plane_resize_window_pixels(Plane start,Plane base,Vec3 eye,int start_w,int start_h,int win_w,int win_h,int fixed_x,int fixed_y){
 return plane_resize_window_pixels_impl(start,base,eye,start_w,start_h,win_w,win_h,fixed_x,fixed_y,false);
}
Plane plane_resize_window_pixels_aspect(Plane start,Plane base,Vec3 eye,int start_w,int start_h,int win_w,int win_h,int fixed_x,int fixed_y){
 return plane_resize_window_pixels_impl(start,base,eye,start_w,start_h,win_w,win_h,fixed_x,fixed_y,true);
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
Plane windshield_hud_plane(void){
 /* The lower, narrower windshield at the forward cockpit position requires a
  * shorter projection than the old rearward layout. Preserve its original
  * 2.55:0.62 aspect and keep every corner inside the real canopy. */
 return (Plane){.center={0,.58f,-1.08f-VEHICLE_COCKPIT_FORWARD},.width=1.90f,.height=.46f,.pitch=-8};
}
Plane monitor_plane(int index){
 if(index==8)return (Plane){.center={-.94f,.43f,.28f-VEHICLE_COCKPIT_FORWARD},.width=1.46f,.height=.59f,.pitch=-12};
 /* Every deployed output is packaged inside the same hull/canopy envelope as
  * the physical cockpit and shares its forward occupant offset. Door outputs
  * are then registered to the physical hinge by scene_update_mounts(). */
 if(index==0)return (Plane){.center={0,.68f,-.45f-VEHICLE_COCKPIT_FORWARD},.width=1.20f,.height=.75f};
 if(index==1)return (Plane){.center={-1.12f,.52f,-.30f-VEHICLE_COCKPIT_FORWARD},.width=.90f,.height=.5625f,.yaw=12,.pitch=-12};
 if(index==2)return (Plane){.center={ 1.12f,.52f,-.30f-VEHICLE_COCKPIT_FORWARD},.width=.90f,.height=.5625f,.yaw=-12,.pitch=-12};
 if(index==3)return (Plane){.center={-1.40f,.48f,.75f-VEHICLE_COCKPIT_FORWARD},.width=.80f,.height=.50f,.yaw=35,.pitch=-8};
 if(index==4)return (Plane){.center={ 1.40f,.48f,.75f-VEHICLE_COCKPIT_FORWARD},.width=.80f,.height=.50f,.yaw=-35,.pitch=-8};
 if(index==5)return (Plane){.center={-.92f,.82f,.95f-VEHICLE_COCKPIT_FORWARD},.width=.90f,.height=.5625f,.yaw=18};
 if(index==6)return (Plane){.center={ .92f,.82f,.95f-VEHICLE_COCKPIT_FORWARD},.width=.90f,.height=.5625f,.yaw=-18};
 return (Plane){.center={0,.55f,3.65f-VEHICLE_COCKPIT_FORWARD},.width=1.12f,.height=.70f,.yaw=180,.pitch=-12};
}
Plane monitor_plane_at(int index,float reveal){
 Plane deployed=monitor_plane(index),stowed=deployed;MonitorMount mount=monitor_mount(index);
 if(mount==MOUNT_WINDSHIELD||mount==MOUNT_REAR_FIXED||mount==MOUNT_CLUSTER)return deployed;
 if(mount==MOUNT_DASH_SLIDE){stowed.center.y-=.95f;stowed.center.z+=.18f;}
 else if(mount==MOUNT_DOOR_SLIDE){stowed.center.x+=index==3?-.20f:.20f;stowed.center.y-=.45f;stowed.center.z+=.10f;}
 else if(mount==MOUNT_ROOF_FLIP){stowed.center.y+=.10f;stowed.pitch=60;}
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
/* The manipulator was authored around the original 2.10 m monitor bays. Map
 * its local hardpoints into the current, smaller car-part planes instead of
 * leaving world-space arms outside the concept car. This retains the branch's
 * joint layout and fixed link lengths while sharing the exact current mount
 * positions, orientations and cockpit-forward offset. */
static Plane manipulator_legacy_plane(int slot){
 if(slot==5||slot==6)return (Plane){.center={slot==5?-3.0f:3.0f,1.75f,.20f},.width=2.10f,.height=1.3125f,.yaw=slot==5?45:-45};
 static const float angles[8]={0,-30,30,-62,62,0,0,180};
 float a=angles[slot];
 return (Plane){.center={4.4f*sinf(a*RAD),1.32f,2.8f-4.4f*cosf(a*RAD)},.width=2.10f,.height=1.3125f,.yaw=-a};
}
static Vec3 manipulator_map_point(Vec3 point,Plane old_plane,Plane new_plane,float scale){
 Vec3 old_r,old_u,old_n,new_r,new_u,new_n;
 plane_axes(old_plane,&old_r,&old_u,&old_n);plane_axes(new_plane,&new_r,&new_u,&new_n);
 Vec3 d=vsub(point,old_plane.center);
 return vadd(new_plane.center,vadd(vmul(new_r,vdot(d,old_r)*scale),
  vadd(vmul(new_u,vdot(d,old_u)*scale),vmul(new_n,vdot(d,old_n)*scale))));
}
static Vec3 manipulator_map_direction(Vec3 direction,Plane old_plane,Plane new_plane){
 Vec3 old_r,old_u,old_n,new_r,new_u,new_n;
 plane_axes(old_plane,&old_r,&old_u,&old_n);plane_axes(new_plane,&new_r,&new_u,&new_n);
 return vadd(vmul(new_r,vdot(direction,old_r)),
  vadd(vmul(new_u,vdot(direction,old_u)),vmul(new_n,vdot(direction,old_n))));
}
MonitorManipulator monitor_manipulator_at(int index,float reveal){
 int slot=index<0?0:(index>7?7:index);
 Plane p=monitor_plane_at(slot,reveal),deployed=monitor_plane(slot);
 Plane legacy=manipulator_legacy_plane(slot);
 float scale=deployed.width/legacy.width;
 Vec3 right,up,normal;plane_axes(p,&right,&up,&normal);
 MonitorManipulator m={
  .base=manipulator_map_point(manip_base[slot],legacy,deployed,scale),
  .shoulder=manipulator_map_point(manip_shoulder[slot],legacy,deployed,scale),
  .wrist=vadd(vadd(vadd(p.center,vmul(right,manip_wrist_lat[slot]*scale)),vmul(up,manip_wrist_vert[slot]*scale)),vmul(normal,-.05f)),
  .upper_len=manip_upper_len[slot]*scale,.fore_len=manip_fore_len[slot]*scale
 };
 /* The deeper stowed dash position needs a little more fixed-link reach than
  * a straight scale of the legacy arms provides. Do not clamp the IK distance:
  * that would silently stretch the forearm and detach it from the wrist. */
 if(monitor_mount(slot)==MOUNT_DASH_SLIDE){
  m.upper_len=fmaxf(m.upper_len,.26f);m.fore_len=fmaxf(m.fore_len,.46f);
 }
 Vec3 delta=vsub(m.wrist,m.shoulder);
 float dist=sqrtf(vdot(delta,delta));
 Vec3 dir=dist>1e-6f?vmul(delta,1.f/dist):v3(0,0,-1);
 Vec3 bend=manipulator_map_direction(manip_bend_hint[slot],legacy,deployed);
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
 m.claws[2]=vadd(vadd(p.center,back),vadd(vmul(right,-w*.38f),vmul(up,-hp*.5f-.20f)));
 m.claws[3]=vadd(vadd(p.center,back),vadd(vmul(right, w*.38f),vmul(up,-hp*.5f-.20f)));
 return m;
}
