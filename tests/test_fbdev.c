#include "fbdev_pixfmt.h"
#include "evdev_util.h"

#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(cond) do { if(!(cond)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#cond);failures++;} } while(0)

#if defined(__BYTE_ORDER__)&&__BYTE_ORDER__==__ORDER_BIG_ENDIAN__
#define CD_TEST_BIG_ENDIAN 1
#else
#define CD_TEST_BIG_ENDIAN 0
#endif

static void test_rejects(void){
 CdFbFormat fmt;
 CHECK(!cd_fb_format_init(&fmt,8,0,8,0,8,0,8,0,0));
 CHECK(!cd_fb_format_init(&fmt,15,10,5,5,5,0,5,0,0));
 CHECK(!cd_fb_format_init(&fmt,32,25,8,8,8,0,8,0,0)); /* red overruns 32 bits */
 CHECK(!cd_fb_format_init(&fmt,16,11,0,5,6,0,5,0,0)); /* missing red */
 CHECK(!cd_fb_format_init(&fmt,32,16,8,18,8,0,8,0,0)); /* red/green overlap */
 CHECK(!cd_fb_format_init(&fmt,32,16,8,8,8,0,8,24,9)); /* alpha longer than 8 */
 CHECK(cd_fb_format_init(&fmt,32,16,8,8,8,0,8,24,8));
}

static void test_packing(void){
 CdFbFormat rgb565,rgb555,xrgb,bgr24,rgb24;
 CHECK(cd_fb_format_init(&rgb565,16,11,5,5,6,0,5,0,0));
 CHECK(cd_fb_pack_pixel(&rgb565,255,0,0,255)==0xf800);
 CHECK(cd_fb_pack_pixel(&rgb565,0,255,0,255)==0x07e0);
 CHECK(cd_fb_pack_pixel(&rgb565,0,0,255,255)==0x001f);
 CHECK(cd_fb_pack_pixel(&rgb565,255,255,255,255)==0xffff);
 CHECK(cd_fb_pack_pixel(&rgb565,128,128,128,255)==0x8410);

 CHECK(cd_fb_format_init(&rgb555,16,10,5,5,5,0,5,0,0));
 CHECK(cd_fb_pack_pixel(&rgb555,255,0,0,255)==0x7c00);
 CHECK(cd_fb_pack_pixel(&rgb555,0,255,0,255)==0x03e0);
 CHECK(cd_fb_pack_pixel(&rgb555,0,0,255,255)==0x001f);

 CHECK(cd_fb_format_init(&xrgb,32,16,8,8,8,0,8,24,8));
 CHECK(cd_fb_pack_pixel(&xrgb,0x12,0x34,0x56,0x78)==0x78123456u);

 CHECK(cd_fb_format_init(&bgr24,24,16,8,8,8,0,8,0,0));
 CHECK(cd_fb_pack_pixel(&bgr24,0xaa,0xbb,0xcc,0xff)==0xaabbccu);
 CHECK(cd_fb_format_init(&rgb24,24,0,8,8,8,16,8,0,0));
 CHECK(cd_fb_pack_pixel(&rgb24,0xaa,0xbb,0xcc,0xff)==0xccbbaau);
}

static void test_convert_flip_stride(void){
 /* Memory row 0 is the OpenGL bottom row: blue, white. Row 1 is red, green. */
 static const uint8_t src[2*8]={
  0,0,255,255,   255,255,255,255,
  255,0,0,255,   0,255,0,255
 };

 CdFbFormat xrgb;
 CHECK(cd_fb_format_init(&xrgb,32,16,8,8,8,0,8,24,8));
 uint8_t dst32[2*12];
 memset(dst32,0xaa,sizeof(dst32));
 cd_fb_convert_rgba(&xrgb,src,8,2,2,true,dst32,12);
 if(CD_TEST_BIG_ENDIAN){
  static const uint8_t row0[8]={255,255,0,0, 255,0,255,0};
  static const uint8_t row1[8]={255,0,0,255, 255,255,255,255};
  CHECK(!memcmp(dst32,row0,sizeof(row0)));
  CHECK(!memcmp(dst32+12,row1,sizeof(row1)));
 }else{
  static const uint8_t row0[8]={0,0,255,255, 0,255,0,255};
  static const uint8_t row1[8]={255,0,0,255, 255,255,255,255};
  CHECK(!memcmp(dst32,row0,sizeof(row0)));
  CHECK(!memcmp(dst32+12,row1,sizeof(row1)));
 }
 for(int i=8;i<12;i++){CHECK(dst32[i]==0xaa);CHECK(dst32[12+i]==0xaa);}

 CdFbFormat rgb565;
 CHECK(cd_fb_format_init(&rgb565,16,11,5,5,6,0,5,0,0));
 uint8_t dst16[2*6];
 memset(dst16,0x5a,sizeof(dst16));
 cd_fb_convert_rgba(&rgb565,src,8,2,2,true,dst16,6);
 if(CD_TEST_BIG_ENDIAN){
  CHECK(dst16[0]==0xf8&&dst16[1]==0x00);
  CHECK(dst16[2]==0x07&&dst16[3]==0xe0);
  CHECK(dst16[6]==0x00&&dst16[7]==0x1f);
 }else{
  CHECK(dst16[0]==0x00&&dst16[1]==0xf8);
  CHECK(dst16[2]==0xe0&&dst16[3]==0x07);
  CHECK(dst16[6]==0x1f&&dst16[7]==0x00);
 }
 CHECK(dst16[8]==0xff&&dst16[9]==0xff);
 CHECK(dst16[4]==0x5a&&dst16[10]==0x5a);

 CdFbFormat bgr24;
 CHECK(cd_fb_format_init(&bgr24,24,16,8,8,8,0,8,0,0));
 uint8_t dst24[2*8];
 memset(dst24,0xaa,sizeof(dst24));
 cd_fb_convert_rgba(&bgr24,src,8,2,2,true,dst24,8);
 if(CD_TEST_BIG_ENDIAN){
  static const uint8_t row0[6]={255,0,0, 0,255,0};
  static const uint8_t row1[6]={0,0,255, 255,255,255};
  CHECK(!memcmp(dst24,row0,sizeof(row0)));
  CHECK(!memcmp(dst24+8,row1,sizeof(row1)));
 }else{
  static const uint8_t row0[6]={0,0,255, 0,255,0};
  static const uint8_t row1[6]={255,0,0, 255,255,255};
  CHECK(!memcmp(dst24,row0,sizeof(row0)));
  CHECK(!memcmp(dst24+8,row1,sizeof(row1)));
 }
 for(int i=6;i<8;i++){CHECK(dst24[i]==0xaa);CHECK(dst24[8+i]==0xaa);}

 CdFbFormat rgb24;
 CHECK(cd_fb_format_init(&rgb24,24,0,8,8,8,16,8,0,0));
 uint8_t dst24rgb[1*6];
 memset(dst24rgb,0xaa,sizeof(dst24rgb));
 cd_fb_convert_rgba(&rgb24,src+8,8,2,1,false,dst24rgb,8);
 if(CD_TEST_BIG_ENDIAN){
  static const uint8_t want[6]={0,0,255, 0,255,0};
  CHECK(!memcmp(dst24rgb,want,sizeof(want)));
 }else{
  static const uint8_t want[6]={255,0,0, 0,255,0};
  CHECK(!memcmp(dst24rgb,want,sizeof(want)));
 }
}

static void test_geometry(void){
 CdFbGeometry geo;
 CHECK(cd_fb_geometry_init(&geo,1920,1080,32,7680,(size_t)7680*2160,2160));
 CHECK(geo.page_count==2);
 CHECK(geo.page_bytes==(size_t)7680*1080);
 CHECK(geo.bytes_per_pixel==4);
 CHECK(geo.pan_possible);

 CHECK(cd_fb_geometry_init(&geo,800,600,16,1600,(size_t)1600*600,600));
 CHECK(geo.page_count==1);
 CHECK(!geo.pan_possible);

 CHECK(!cd_fb_geometry_init(&geo,800,600,32,100,(size_t)100*600,600));
 CHECK(!cd_fb_geometry_init(&geo,800,600,16,1600,(size_t)1600*599,600));
 CHECK(!cd_fb_geometry_init(&geo,800,600,16,1600,(size_t)1600*600,599));
 CHECK(!cd_fb_geometry_init(&geo,800,600,8,800,(size_t)800*600,600));
}

static void test_offsets(void){
  CHECK(cd_fb_visible_offset(20,2,3,2)==64);
  CHECK(cd_fb_page_offset(20,2,1,3,2)==64);
  CHECK(cd_fb_page_offset(20,0,2,3,4)==120);
  CHECK(cd_fb_visible_offset(100,5,7,3)==715);
  CHECK(cd_fb_page_offset(100,0,1,10,3)==1000);
}

static void test_mode_parse(void){
  int w,h,bpp;
  CHECK(cd_fb_mode_parse("800x600",&w,&h,&bpp));
  CHECK(w==800&&h==600&&bpp==0);
  CHECK(cd_fb_mode_parse("1920x1080@32",&w,&h,&bpp));
  CHECK(w==1920&&h==1080&&bpp==32);
  CHECK(cd_fb_mode_parse("1280x720@16",&w,&h,&bpp));
  CHECK(w==1280&&h==720&&bpp==16);
  CHECK(cd_fb_mode_parse("640x400@24",&w,&h,&bpp));
  CHECK(w==640&&h==400&&bpp==24);

  CHECK(!cd_fb_mode_parse("",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("x600",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("0x600",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x0",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("-1x600",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x-1",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("7681x600",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x4321",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x600@",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x600@8",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x600@15",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x600@33",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x600@32x",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x600@32garbage",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse(" 800x600",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x600 ",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800X600",&w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("800x600@", &w,&h,&bpp));
  CHECK(!cd_fb_mode_parse("1000000x600",&w,&h,&bpp));
}

static void test_evdev_scale_abs(void){
  CHECK(evdev_util_scale_abs(0, 0, 100, 800) == 0);
  CHECK(evdev_util_scale_abs(100, 0, 100, 800) == 799);
  CHECK(evdev_util_scale_abs(50, 0, 100, 800) == 399);
  CHECK(evdev_util_scale_abs(-10, 0, 100, 800) == 0);
  CHECK(evdev_util_scale_abs(110, 0, 100, 800) == 799);
  CHECK(evdev_util_scale_abs(500, 0, 1000, 1920) == 959);
  CHECK(evdev_util_scale_abs(0, 100, 100, 800) == 0); /* degenerate */
  CHECK(evdev_util_scale_abs(0, 200, 100, 800) == 0); /* degenerate */
  CHECK(evdev_util_scale_abs(0, 0, 100, 0) == 0); /* degenerate */
  CHECK(evdev_util_scale_abs(0, 0, 100, -1) == 0); /* degenerate */
}

static void test_evdev_touch_id_pool(void){
  evdev_util_touch_id_reset();
  int id0 = evdev_util_touch_id_alloc(); CHECK(id0 == 0);
  int id1 = evdev_util_touch_id_alloc(); CHECK(id1 == 1);
  int id2 = evdev_util_touch_id_alloc(); CHECK(id2 == 2);
  evdev_util_touch_id_release(1);
  int id3 = evdev_util_touch_id_alloc(); CHECK(id3 == 1); /* reuse lowest free */
  evdev_util_touch_id_release(0);
  evdev_util_touch_id_release(2);
  evdev_util_touch_id_release(1); /* double release safe */
  for(int i=0;i<10;i++){
    int id = evdev_util_touch_id_alloc();
    CHECK(id == i);
  }
  int id_exhaust = evdev_util_touch_id_alloc(); CHECK(id_exhaust == -1);
  evdev_util_touch_id_release(5);
  int id_reuse = evdev_util_touch_id_alloc(); CHECK(id_reuse == 5);
  evdev_util_touch_id_reset();
  int id_after_reset = evdev_util_touch_id_alloc(); CHECK(id_after_reset == 0);
}

int main(void){
  test_rejects();
  test_packing();
  test_convert_flip_stride();
  test_geometry();
  test_offsets();
  test_mode_parse();
  test_evdev_scale_abs();
  test_evdev_touch_id_pool();
  if(failures)return 1;
  puts("PASS: fbdev pixel formats (16/24/32bpp packing, LUT quantization, row flip, independent strides/padding, geometry, page offsets, mode parsing) + evdev_util (abs scaling, touch-id pool)");
  return 0;
}
