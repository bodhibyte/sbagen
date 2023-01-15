//
//	OGG decoding using Tremor (libvorbisidec)
//

#include "libs/ivorbiscodec.h"
#include "libs/ivorbisfile.h"

extern FILE *mix_in;
extern void *Alloc(size_t);
extern void error(char *fmt, ...);
extern int out_rate, out_rate_def;

int ogg_read(int *dst, int dlen);

static OggVorbis_File oggfile;
static short *ogg_buf0, *ogg_buf1, *ogg_rd, *ogg_end;

void 
ogg_init() {
   vorbis_info *vi;
   int len= 2048;

   // Setup buffer so that we can be consistent in our calls to ov_read
   ogg_buf0= ALLOC_ARR(len, short);
   ogg_buf1= ogg_buf0 + len;
   ogg_rd= ogg_end= ogg_buf0;

   // Setup OGG decoder
   if (0 > ov_open(mix_in, &oggfile, NULL, 0) < 0) 
      error("Input does not appear to be an Ogg bitstream");

   // Pick up sampling rate and override default if -r no used
   vi= ov_info(&oggfile, -1);
   if (out_rate_def) out_rate= vi->rate;
   out_rate_def= 0;

   inbuf_start(ogg_read, 256*1024);	// 1024K buffer: 3s@44.1kHz
}

void 
ogg_term() {
   ov_clear(&oggfile);
   free(ogg_buf0);
}

int 
ogg_read(int *dst, int dlen) {
   int *dst0= dst;
   int *dst1= dst + dlen;

   while (dst < dst1) {
      int rv, sect;

      // Copy data from buffer
      if (ogg_rd != ogg_end) {
	 while (ogg_rd != ogg_end && dst != dst1)
	    *dst++= *ogg_rd++ << 4;
	 continue;
      }

      // Refill buffer
      rv= ov_read(&oggfile, (char*)ogg_buf0, (ogg_buf1-ogg_buf0)*sizeof(short), &sect);
      //debug("ov_read %d/%d", rv, (ogg_buf1-ogg_buf0)*sizeof(short));
      if (rv < 0) {
	 warn("Recoverable error in Ogg stream  ");
	 continue;
      }
      if (rv == 0) 	// EOF
	 return dst-dst0;
      if (rv & 3)
	 error("UNEXPECTED: ov_read() returned a partial sample count: %d", rv);
      ogg_rd= ogg_buf0;
      ogg_end= ogg_buf0 + (rv/2);
   }
   return dst-dst0;
}

// END //
