#include "fbdev_pixfmt.h"

#include <string.h>

static bool field_ok(unsigned offset,unsigned length,int bits_per_pixel,bool required){
 if(required&&length==0)return false;
 if(length>8)return false;
 if(length&&offset+length>(unsigned)bits_per_pixel)return false;
 return true;
}
static bool fields_overlap(unsigned a_offset,unsigned a_length,unsigned b_offset,unsigned b_length){
 if(!a_length||!b_length)return false;
 return a_offset<b_offset+b_length&&b_offset<a_offset+a_length;
}
static void fill_lut(uint32_t lut[256],unsigned offset,unsigned length){
 if(!length){memset(lut,0,256*sizeof(lut[0]));return;}
 uint32_t max=(1u<<length)-1u;
 for(int i=0;i<256;i++){
  uint32_t q=(uint32_t)((i*(int)max+127)/255);
  lut[i]=q<<offset;
 }
}
static void store_pixel(uint8_t *dst,uint32_t pixel,int bytes){
#if defined(__BYTE_ORDER__)&&__BYTE_ORDER__==__ORDER_BIG_ENDIAN__
 for(int b=0;b<bytes;b++)dst[b]=(uint8_t)(pixel>>(8*(bytes-1-b)));
#else
 for(int b=0;b<bytes;b++)dst[b]=(uint8_t)(pixel>>(8*b));
#endif
}

bool cd_fb_format_init(CdFbFormat *fmt,int bits_per_pixel,
 unsigned r_offset,unsigned r_length,unsigned g_offset,unsigned g_length,
 unsigned b_offset,unsigned b_length,unsigned a_offset,unsigned a_length){
 if(!fmt)return false;
 memset(fmt,0,sizeof(*fmt));
 if(bits_per_pixel!=16&&bits_per_pixel!=24&&bits_per_pixel!=32)return false;
 if(!field_ok(r_offset,r_length,bits_per_pixel,true))return false;
 if(!field_ok(g_offset,g_length,bits_per_pixel,true))return false;
 if(!field_ok(b_offset,b_length,bits_per_pixel,true))return false;
 if(!field_ok(a_offset,a_length,bits_per_pixel,false))return false;
 if(fields_overlap(r_offset,r_length,g_offset,g_length))return false;
 if(fields_overlap(r_offset,r_length,b_offset,b_length))return false;
 if(fields_overlap(g_offset,g_length,b_offset,b_length))return false;
 if(fields_overlap(a_offset,a_length,r_offset,r_length))return false;
 if(fields_overlap(a_offset,a_length,g_offset,g_length))return false;
 if(fields_overlap(a_offset,a_length,b_offset,b_length))return false;
 fmt->bits_per_pixel=bits_per_pixel;
 fmt->bytes_per_pixel=bits_per_pixel/8;
 fmt->r_offset=r_offset;fmt->r_length=r_length;
 fmt->g_offset=g_offset;fmt->g_length=g_length;
 fmt->b_offset=b_offset;fmt->b_length=b_length;
 fmt->a_offset=a_offset;fmt->a_length=a_length;
 fill_lut(fmt->r_lut,r_offset,r_length);
 fill_lut(fmt->g_lut,g_offset,g_length);
 fill_lut(fmt->b_lut,b_offset,b_length);
 fill_lut(fmt->a_lut,a_offset,a_length);
 return true;
}

uint32_t cd_fb_pack_pixel(const CdFbFormat *fmt,uint8_t r,uint8_t g,uint8_t b,uint8_t a){
 if(!fmt)return 0;
 return fmt->r_lut[r]|fmt->g_lut[g]|fmt->b_lut[b]|fmt->a_lut[a];
}

void cd_fb_convert_rgba(const CdFbFormat *fmt,const uint8_t *src,size_t src_stride,
 int width,int height,bool bottom_up,uint8_t *dst,size_t dst_stride){
 if(!fmt||!src||!dst||width<=0||height<=0)return;
 int bytes=fmt->bytes_per_pixel;
 if(bytes<=0)return;
 if(src_stride<(size_t)width*4||dst_stride<(size_t)width*(size_t)bytes)return;
 for(int y=0;y<height;y++){
  int sy=bottom_up?height-1-y:y;
  const uint8_t *srow=src+(size_t)sy*src_stride;
  uint8_t *drow=dst+(size_t)y*dst_stride;
  for(int x=0;x<width;x++){
   const uint8_t *sp=srow+(size_t)x*4;
   uint32_t px=cd_fb_pack_pixel(fmt,sp[0],sp[1],sp[2],sp[3]);
   store_pixel(drow+(size_t)x*(size_t)bytes,px,bytes);
  }
 }
}

bool cd_fb_geometry_init(CdFbGeometry *geo,int width,int height,int bits_per_pixel,
 unsigned line_length,size_t smem_len,int yres_virtual){
 if(!geo)return false;
 memset(geo,0,sizeof(*geo));
 if(width<=0||height<=0||yres_virtual<height)return false;
 if(bits_per_pixel!=16&&bits_per_pixel!=24&&bits_per_pixel!=32)return false;
 int bytes=bits_per_pixel/8;
 if((size_t)line_length<(size_t)width*(size_t)bytes)return false;
 size_t page_bytes=(size_t)line_length*(size_t)height;
 if(smem_len<page_bytes)return false;
 int pages=yres_virtual/height;
 if(pages<1)return false;
 geo->width=width;
 geo->height=height;
 geo->bits_per_pixel=bits_per_pixel;
 geo->bytes_per_pixel=bytes;
 geo->line_length=line_length;
 geo->smem_len=smem_len;
 geo->yres_virtual=yres_virtual;
 geo->page_count=pages;
 geo->page_bytes=page_bytes;
 geo->pan_possible=pages>=2&&page_bytes<=SIZE_MAX/2&&smem_len>=page_bytes*2;
 return true;
}

size_t cd_fb_visible_offset(size_t line_length,int xoffset,int yoffset,int bytes_per_pixel){
 if(xoffset<0)xoffset=0;
 if(yoffset<0)yoffset=0;
 if(bytes_per_pixel<0)bytes_per_pixel=0;
 return (size_t)yoffset*line_length+(size_t)xoffset*(size_t)bytes_per_pixel;
}
size_t cd_fb_page_offset(size_t line_length,int xoffset,int page,int yres,int bytes_per_pixel){
 if(page<0)page=0;
 if(yres<0)yres=0;
 return cd_fb_visible_offset(line_length,xoffset,page*yres,bytes_per_pixel);
}
