#ifndef CHROMEDRIVE_FBDEV_PIXFMT_H
#define CHROMEDRIVE_FBDEV_PIXFMT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Pure Linux fbdev pixel/geometry helpers. This header deliberately does not
 * include <linux/fb.h>: backend_fbdev.c copies the four bitfields into plain
 * integers, so format conversion and page math stay unit-testable without a
 * framebuffer device, EGL, or SDL. */

typedef struct {
 int bits_per_pixel;
 int bytes_per_pixel;
 unsigned r_offset,r_length,g_offset,g_length,b_offset,b_length,a_offset,a_length;
 uint32_t r_lut[256],g_lut[256],b_lut[256],a_lut[256];
} CdFbFormat;

typedef struct {
 int width,height;
 int bits_per_pixel,bytes_per_pixel;
 unsigned line_length;
 size_t smem_len;
 int yres_virtual;
 int page_count;
 size_t page_bytes;
 bool pan_possible;
} CdFbGeometry;

bool cd_fb_format_init(CdFbFormat *fmt,int bits_per_pixel,
 unsigned r_offset,unsigned r_length,unsigned g_offset,unsigned g_length,
 unsigned b_offset,unsigned b_length,unsigned a_offset,unsigned a_length);
uint32_t cd_fb_pack_pixel(const CdFbFormat *fmt,uint8_t r,uint8_t g,uint8_t b,uint8_t a);

/* Convert a tightly packed GL_RGBA/GL_UNSIGNED_BYTE image (4 source bytes per
 * pixel) into fbdev's packed truecolor format. Source and destination strides
 * are independent; bytes in dst beyond width*bytes_per_pixel are untouched.
 * bottom_up=true flips OpenGL's bottom-up rows into fbdev's top-down order. */
void cd_fb_convert_rgba(const CdFbFormat *fmt,const uint8_t *src,size_t src_stride,
 int width,int height,bool bottom_up,uint8_t *dst,size_t dst_stride);

bool cd_fb_geometry_init(CdFbGeometry *geo,int width,int height,int bits_per_pixel,
  unsigned line_length,size_t smem_len,int yres_virtual);
size_t cd_fb_visible_offset(size_t line_length,int xoffset,int yoffset,int bytes_per_pixel);
size_t cd_fb_page_offset(size_t line_length,int xoffset,int page,int yres,int bytes_per_pixel);

/* Pure mode-string parser (no <linux/fb.h> dependency). Parses "WIDTHxHEIGHT[@BPP]".
 * BPP must be 16, 24, or 32. Returns true on success, fills out width/height/bpp
 * (bpp = 0 if not specified). Returns false on any syntax error, overflow,
 * zero/negative values, extra characters, or invalid BPP. */
bool cd_fb_mode_parse(const char *str,int *out_width,int *out_height,int *out_bpp);

#endif
