/* Exact rendered-surface picking and cursor-contact regression.
 *
 * scene.c is included so the test uses the production camera, draw-layer
 * offsets and picker. Function/data sections + --gc-sections discard OpenGL
 * rendering; the retained code links the real Wayland surface traversal and
 * vehicle/math transforms. Projection below is an independent glFrustum-style
 * calculation, not a call back into the picking ray. */
#include "../src/scene.c"
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do{if(!(cond)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);failures++;}}while(0)
static int close_enough(float a,float b,float tolerance){return fabsf(a-b)<=tolerance;}
static void vec_close(Vec3 a,Vec3 b,float tolerance){
 CHECK(close_enough(a.x,b.x,tolerance));CHECK(close_enough(a.y,b.y,tolerance));CHECK(close_enough(a.z,b.z,tolerance));
}
static void project_rendered(Server *s,SceneCamera camera,Vec3 local,float *x,float *y){
 Vec3 world=vehicle_local_to_world(&s->vehicle,local),relative=vsub(world,camera.eye);
 float cx=vdot(relative,camera.right),cy=vdot(relative,camera.up),cz=vdot(relative,camera.forward);
 float tangent=tanf(s->fov*RAD*.5f);CHECK(cz>.05f);
 *x=(cx/cz/(((float)s->width/s->height)*tangent)+1)*s->width*.5f;
 *y=(1-cy/cz/tangent)*s->height*.5f;
}
static void setup(Server *s,Surface *root){
 memset(s,0,sizeof(*s));memset(root,0,sizeof(*root));wl_list_init(&s->surfaces);
 s->width=1600;s->height=1000;s->fov=65;s->pitch=-6;s->camera_mode=VEHICLE_CAMERA_DRIVER;
 s->eye=v3(0,.86f,2.8f-VEHICLE_COCKPIT_FORWARD);vehicle_init(&s->vehicle);
 s->vehicle.position=v3(4,1,-7);s->vehicle.yaw=23;s->vehicle.pitch=5;s->vehicle.roll=-7;
 root->server=s;root->mapped=true;root->texture=1;root->slot=0;root->width=1000;root->height=625;
 wl_list_insert(&s->surfaces,&root->link);s->monitors[0].surface=root;
 /* Keep unrelated cockpit targets behind the eye in this focused test. */
 for(int i=0;i<6;i++)s->buttons[i].plane.center=v3(0,0,100);
 s->cog.center=s->quit_button.center=s->hud_button.center=v3(0,0,100);
}
static void check_hit(const char *label,Hit hit,Surface *surface,float x,float y,Vec3 point,Vec3 normal,Server *s,SceneCamera camera,float screen_x,float screen_y){
 float error=sqrtf((hit.x-x)*(hit.x-x)+(hit.y-y)*(hit.y-y));
 printf("%-24s client=(%.3f,%.3f) expected=(%.3f,%.3f) error=%.4f px\n",label,hit.x,hit.y,x,y,error);
 CHECK(hit.kind==HIT_APP);CHECK(hit.surface==surface);CHECK(hit.has_contact);
 CHECK(close_enough(hit.x,x,.03f));CHECK(close_enough(hit.y,y,.03f));
 vec_close(hit.point,point,.003f);vec_close(hit.normal,normal,.0001f);
 float projected_x=0,projected_y=0;project_rendered(s,camera,hit.point,&projected_x,&projected_y);
 CHECK(close_enough(projected_x,screen_x,.03f));CHECK(close_enough(projected_y,screen_y,.03f));
}
static Vec3 root_pixel_point(Plane p,float root_x,float root_y,int root_w,int root_h,float z){
 Vec3 right,up,normal;plane_axes(p,&right,&up,&normal);
 float scale=fminf(p.width/root_w,p.height/root_h);
 float lx=(root_x-root_w*.5f)*scale,ly=(root_y-root_h*.5f)*scale;
 return vadd(p.center,vadd(vadd(vmul(right,lx),vmul(up,-ly)),vmul(normal,z)));
}
int main(void){
 const struct {float yaw,pitch,roll,sx,sy;} cases[]={
  {0,0,0,1,1},{65,-45,20,1,1},{-70,60,-30,1.35f,.80f},{80,-75,55,.65f,1.25f},
 };
 for(unsigned i=0;i<sizeof(cases)/sizeof(cases[0]);i++){
  Server s;Surface root;setup(&s,&root);Plane p=monitor_plane(0);
  p.yaw=cases[i].yaw;p.pitch=cases[i].pitch;p.roll=cases[i].roll;
  p.width*=cases[i].sx;p.height*=cases[i].sy;s.monitors[0].plane=p;
  float u=.73f,v=.33f,k=monitor_iso_scale(p,0),z=APP_SURFACE_Z0*k;
  Vec3 right,up,normal;plane_axes(p,&right,&up,&normal);
  Vec3 point=vadd(p.center,vadd(vadd(vmul(right,(u-.5f)*p.width),vmul(up,(.5f-v)*p.height)),vmul(normal,z)));
  SceneCamera camera=scene_camera(&s);float screen_x,screen_y;project_rendered(&s,camera,point,&screen_x,&screen_y);
  float scale=fminf(p.width/root.width,p.height/root.height);
  float expected_x=(u-.5f)*p.width/scale+root.width*.5f;
  float expected_y=(v-.5f)*p.height/scale+root.height*.5f;
  char label[64];snprintf(label,sizeof(label),"root case %u direct",i);
  check_hit(label,scene_pick_surface(&s,&root,screen_x,screen_y),&root,expected_x,expected_y,point,normal,&s,camera,screen_x,screen_y);
  snprintf(label,sizeof(label),"root case %u generic",i);
  check_hit(label,scene_pick(&s,screen_x,screen_y,false),&root,expected_x,expected_y,point,normal,&s,camera,screen_x,screen_y);
 }
 /* Root is layer 0 and this child is layer 1 in the exact renderer traversal.
  * The extra .0007*k offset is small, but at a steep angle it is still part
  * of the contact and must not be dropped. */
 Server s;Surface root,child;setup(&s,&root);memset(&child,0,sizeof(child));
 child.server=&s;child.parent=&root;child.mapped=true;child.texture=1;child.slot=-1;
 child.x=100;child.y=50;child.width=400;child.height=250;wl_list_insert(&s.surfaces,&child.link);
 Plane p=monitor_plane(0);p.yaw=65;p.pitch=-45;p.roll=20;s.monitors[0].plane=p;
 Vec3 normal;plane_axes(p,NULL,NULL,&normal);float k=monitor_iso_scale(p,0),child_z=(APP_SURFACE_Z0+APP_SURFACE_ZSTEP)*k;
 SceneCamera camera=scene_camera(&s);
 Vec3 point=root_pixel_point(p,320,180,root.width,root.height,child_z);float screen_x,screen_y;
 project_rendered(&s,camera,point,&screen_x,&screen_y);
 check_hit("subsurface direct",scene_pick_surface(&s,&child,screen_x,screen_y),&child,220,130,point,normal,&s,camera,screen_x,screen_y);
 check_hit("subsurface generic",scene_pick(&s,screen_x,screen_y,false),&child,220,130,point,normal,&s,camera,screen_x,screen_y);
 /* Implicit grabs retain their original surface and allow coordinates beyond
  * its quad. The contact still lies on that child's rendered layer plane. */
 point=root_pixel_point(p,600,450,root.width,root.height,child_z);project_rendered(&s,camera,point,&screen_x,&screen_y);
 check_hit("subsurface unbounded",scene_pick_surface(&s,&child,screen_x,screen_y),&child,500,400,point,normal,&s,camera,screen_x,screen_y);
 if(failures){fprintf(stderr,"%d cursor/contact checks failed\n",failures);return 1;}
 puts("PASS: rendered root/subsurface picking, steep-angle client coordinates, exact contact/normal, and grab continuity");return 0;
}
