/* Out-of-process font coverage worker for the compositor.
 *
 * argv[1] is the ChromeDrive install root and argv[2] an already-resolved
 * language tag. The parent never waits for this process: fontconfig/Cairo may
 * block here without holding startup, Wayland input, or managed VT release.
 * Exit 0 means covered/English, 3 means verified missing glyphs, 2 is a usage
 * or internal error. Detailed diagnostics stay on inherited stderr. */
#include "fontprobe.h"
#include "i18n.h"

#include <stdio.h>
#include <string.h>

int main(int argc,char **argv){
 if(argc!=3){
  fprintf(stderr,"chromedrive-fontprobe: internal usage: ROOT LANGUAGE\n");
  return 2;
 }
 const char *now=i18n_set_language(argv[1],argv[2]);
 if(!strcmp(now,"en")){i18n_free();return 0;}
 const char *family=i18n_font();if(!family)family="DejaVu Sans";
 char first_bad[8]="";
 int missing=cd_font_probe_catalog("DejaVu Sans",first_bad,sizeof(first_bad));
 if(missing>0){
  cd_font_report_missing(now,family,missing,first_bad);
  i18n_free();
  return 3;
 }
 char samples[12][8];int n=i18n_sample_glyphs(samples,12);
 fprintf(stderr,"[font] %s: %d sample glyphs covered by '%s' (asynchronous)\n",now,n,family);
 i18n_free();
 return 0;
}
