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
Plane monitor_plane(int index){
 static const float angles[]={0,-30,30,-62,62,-94,94,180};float a=angles[index%8];
 return (Plane){.center={4.4f*sinf(a*RAD),1.32f,2.8f-4.4f*cosf(a*RAD)},.width=2.10f,.height=1.3125f,.yaw=-a,.pitch=0};
}
