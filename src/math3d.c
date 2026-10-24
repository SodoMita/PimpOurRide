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
 /* The windshield display sits behind the steeply rising cowl, fully below
  * the shared canopy crown instead of piercing the forward roof slope. */
 if(index==0)return (Plane){.center={0,1.05f,-.45f},.width=1.60f,.height=1.0f};
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
