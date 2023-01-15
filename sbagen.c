//
//	SBaGen - Sequenced BinAural Generator
//
//	(c) 1999-2001 Jim Peters <jim@uazu.net>.  All Rights Reserved.
//	For latest version see http://sbagen.sf.net/ or
//	http://www.uazu.net/sbagen/.  Released under the GNU GPL.  Use
//	at your own risk.
//
//	" This program is free software; you can redistribute it and/or modify
//	  it under the terms of the GNU General Public License as published by
//	  the Free Software Foundation, version 2.
//	  
//	  This program is distributed in the hope that it will be useful,
//	  but WITHOUT ANY WARRANTY; without even the implied warranty of
//	  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//	  GNU General Public License for more details. "
//
//	See the file COPYING for details of this license.
//	
//	- - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - 
//
//	Some code fragments in the Win32 audio handling are based on
//	code from PLIB (c) 2001 by Steve Baker, originally released
//	under the LGPL (slDSP.cxx and sl.h).  For the original source,
//	see the PLIB project: http://plib.sf.net

#define VERSION "1.0.9"

#ifndef NO_DEV_DSP
#define DSP		// Define to use /dev/dsp, or comment out if not available
#endif
#ifndef NO_ANSI_TTY
#define ANSI_TTY	// Define to use ANSI sequences to clear/redraw lines
#endif

#include <stdio.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <errno.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <sys/time.h>

#ifdef MINGW
 #include <windows.h>
 #include <mmsystem.h>
#else
 #include <sys/ioctl.h>
 #include <sys/times.h>
 #ifdef DSP
  #include <sys/soundcard.h>
 #endif
#endif

typedef struct Channel Channel;
typedef struct Voice Voice;
typedef struct Period Period;
typedef struct NameDef NameDef;
typedef struct BlockDef BlockDef;
typedef unsigned char uchar;

inline int t_per24(int t0, int t1) ;
inline int t_per0(int t0, int t1) ;
inline int t_mid(int t0, int t1) ;
int main(int argc, char **argv) ;
void status(char *) ;
void dispCurrPer( FILE* ) ;
void init_sin_table() ;
void debug(char *fmt, ...) ;
void * CAlloc(size_t len) ;
char * StrDup(char *str) ;
inline int calcNow() ;
//inline double noise() ;
void loop() ;
void outChunk() ;
void corrVal(int ) ;
int readLine() ;
char * getWord() ;
void badSeq() ;
void readSeqImm() ;
void readSeq(char *fnam) ;
void correctPeriods();
void setup_device(void) ;
void readNameDef();
void readTimeLine();
void setupMidnight();
int voicesEq(Voice *, Voice *);
void error(char *fmt, ...) ;
int sprintTime(char *, int);
int sprintVoice(char *, Voice *, Voice *);
int readTime(char *, int *);
void writeWAV();
void writeOut(char *, int);
void sinc_interpolate(double *, int, int *);
inline int userTime();
#ifdef MINGW
void CALLBACK win32_audio_callback(HWAVEOUT, UINT, DWORD, DWORD, DWORD);
#endif

void 
usage() {
  error("SBaGen - Sequenced BinAural sound Generator, version " VERSION "\n"
	"Copyright (c) 1999-2001 Jim Peters, released under the GNU GPL\n\n"
	"Usage: sbagen [options] seq-file\n"
	"       sbagen [options] -i tone-specs ...\n\n"
	"Options:  -D        Display the full interpreted sequence instead of playing it\n"
	"          -Q        Quiet - don't display running status\n"
	"          -i        Immediate.  Take the remainder of the command line to be\n"
	"                     tone-specifications, and play them continuously\n"
	"          -q mult   Quick.  Run through quickly (real time x `mult') from the\n"
	"                     start time, rather than wait for real time to pass\n"
	"\n"
	"          -r rate   Manually select the output rate (default is 44100 Hz)\n"
	"          -b bits   Select the number bits for output (8 or 16, default 16)\n"
	"          -L time   Select the length of time (hh:mm or hh:mm:ss) to output\n"
	"                     for.  Default is to output forever.\n"
	"          -S        Output from the first tone-set in the sequence (Start),\n"
	"                     instead of working in real-time.  Equivalent to `-q 1'.\n"
	"          -E        Output until the last tone-set in the sequence (End),\n"
	"                     instead of outputting forever.\n"
	"\n"
	"          -o file   Output raw data to the given file instead of /dev/dsp\n"
	"          -O        Output raw data to the standard output\n"
	"          -W        Output a WAV-format file instead of raw data\n"
	"\n"
	"          -R rate   Select rate in Hz that frequency changes are recalculated\n"
	"                     (for file/pipe output only, default is 10Hz)\n"
	"          -F fms    Fade in/out time in ms (default 60000ms, or 1min)\n"
	);
}

#define DEBUG_CHK_UTIME 0	// Check how much user time is being consumed
#define DEBUG_DUMP_WAVES 0	// Dump out wave tables (to plot with gnuplot)
#define DEBUG_DUMP_AMP 0	// Dump output amplitude to stdout per chunk
#define N_CH 8			// Number of channels

struct Voice {
  int typ;			// Voice type: 0 off, 1 binaural, 2 pink noise, 3 bell, 4 spin,
   				//   -1 to -100 wave00 to wave99
  double amp;			// Amplitude level (0-32767)
  double carr;			// Carrier freq (for binaural/bell), width (for spin)
  double res;			// Resonance freq (-ve or +ve) (for binaural/spin)
};

struct Channel {
  Voice v;			// Current voice setting (updated from current period)
  int typ;			// Current type: 0 off, 1 binaural, 2 pink noise, 3 bell, 4 spin,
   				//   -1 to -100 wave00 to wave99
  int amp;			// Current state, according to current type
  int inc1, off1;		//  ::  (for binaural tones, offset + increment into sine 
  int inc2, off2;		//  ::   table * 65536)
};

struct Period {
  Period *nxt, *prv;		// Next/prev in chain
  int tim;			// Start time (end time is ->nxt->tim)
  Voice v0[N_CH], v1[N_CH];	// Start and end voices
  int fi, fo;			// Temporary: Fade-in, fade-out modes
};

struct NameDef {
  NameDef *nxt;
  char *name;			// Name of definition
  BlockDef *blk;		// Non-zero for block definition
  Voice vv[N_CH];		// Voice-set for it (unless a block definition)
};

struct BlockDef {
  BlockDef *nxt;		// Next in chain
  char *lin;			// StrDup'd line
};

#define ST_AMP 0x7FFFF		// Amplitude of wave in sine-table
#define NS_ADJ 12		// Noise is generated internally with amplitude ST_AMP<<NS_ADJ
#define NS_AMP (ST_AMP<<NS_ADJ)
#define ST_SIZ 16384		// Number of elements in sine-table (power of 2)
int *sin_table;
#define AMP_DA(pc) (40.96 * (pc))	// Display value (%age) to ->amp value
#define AMP_AD(amp) ((amp) / 40.96)	// Amplitude value to display %age
int *waves[100];		// Pointers are either 0 or point to a sin_table[]-style array of int

Channel chan[N_CH];		// Current channel states
int now;			// Current time (milliseconds from midnight)
Period *per= 0;			// Current period
NameDef *nlist;			// Full list of name definitions

short *out_buf;			// Output buffer
int out_bsiz;			// Output buffer size (bytes)
int out_blen;			// Output buffer length (samples) (1.0* or 0.5* out_bsiz)
int out_bps;			// Output bytes per sample (2 or 4)
int out_buf_ms;			// Time to output a buffer-ful in ms
int out_buf_lo;			// Time to output a buffer-ful, fine-tuning in ms/0x10000
int out_fd;			// Output file descriptor
int out_rate= 44100;		// Sample rate
int out_mode= 1;		// Output mode: 0 unsigned char[2], 1 short[2], 2 swapped short[2]
int out_prate= 10;		// Rate of parameter change (for file and pipe output only)
int fade_int= 60000;		// Fade interval (ms)
FILE *in;			// Input sequence file
int in_lin;			// Current input line
char buf[4096];			// Buffer for current line
char buf_copy[4096];		// Used to keep unmodified copy of line
char *lin;			// Input line (uses buf[])
char *lin_copy;			// Copy of input line
double spin_carr_max;		// Maximum `carrier' value for spin (really max width in us)

#define NS_BIT 10
int ns_tbl[1<<NS_BIT];
int ns_off= 0;

int fast_tim0= -1;		// First time mentioned in the sequence file (for -q and -S option)
int fast_tim1= -1;		// Last time mentioned in the sequence file (for -E option)
int fast_mult= 0;		// 0 to sync to clock (adjusting as necessary), or else sync to
				//  output rate, with the multiplier indicated
int byte_count= -1;		// Number of bytes left to output, or -1 if unlimited
int tty_erase;			// Chars to erase from current line (for ESC[K emulation)

int opt_D;
int opt_Q;
int opt_i;
int opt_S;
int opt_E;
int opt_W;
int opt_O;
int opt_L= -1;			// Length in ms, or -1
char *opt_o;			// File name to output to, or 0

#ifdef MINGW
#define BUFFER_COUNT 8
#define BUFFER_SIZE 8192*4
HWAVEOUT aud_handle;
WAVEHDR *aud_head[BUFFER_COUNT];
int aud_current;		// Current header
int aud_cnt;			// Number of headers in use
#endif


//
//	Time-keeping functions
//

#define H24 (86400000)			// 24 hours
#define H12 (43200000)			// 12 hours

inline int t_per24(int t0, int t1) {		// Length of period starting at t0, ending at t1.
  int td= t1 - t0;				// NB for t0==t1 this gives 24 hours, *NOT 0*
  return td > 0 ? td : td + H24;
}
inline int t_per0(int t0, int t1) {		// Length of period starting at t0, ending at t1.
  int td= t1 - t0;				// NB for t0==t1 this gives 0 hours
  return td >= 0 ? td : td + H24;
}
inline int t_mid(int t0, int t1) {		// Midpoint of period from t0 to t1
  return ((t1 < t0) ? (H24 + t0 + t1) / 2 : (t0 + t1) / 2) % H24;
}

//
//	M A I N
//

int 
main(int argc, char **argv) {
  int val;
  char dmy;

  argc--; argv++;
  init_sin_table();
  setupMidnight();

  // Scan options
  while (argc > 0 && argv[0][0] == '-' && argv[0][1]) {
    char *p= 1 + *argv++; argc--;
    while (*p) switch (*p++) {
     case 'D': opt_D= 1; break;
     case 'Q': opt_Q= 1; break;
     case 'i': opt_i= 1; break;
     case 'E': opt_E= 1; break;
     case 'S': opt_S= 1;
       if (!fast_mult) fast_mult= 1; 		// Don't try to sync with real time
       break;
     case 'O': opt_O= 1;
       if (!fast_mult) fast_mult= 1; 		// Don't try to sync with real time
       break;
     case 'W': opt_W= 1;
       if (!fast_mult) fast_mult= 1; 		// Don't try to sync with real time
       break;
     case 'q': 
       opt_S= 1;
       if (argc-- < 1 || 1 != sscanf(*argv++, "%d %c", &fast_mult, &dmy)) usage();
       if (fast_mult < 1) fast_mult= 1;
       break;
     case 'r':
       if (argc-- < 1 || 1 != sscanf(*argv++, "%d %c", &out_rate, &dmy)) usage();
       break;
     case 'b':
       if (argc-- < 1 || 1 != sscanf(*argv++, "%d %c", &val, &dmy)) usage();
       if (val != 8 && val != 16) usage();
       out_mode= (val == 8) ? 0 : 1;
       break;
     case 'o':
       if (argc-- < 1) usage();
       opt_o= *argv++;
       if (!fast_mult) fast_mult= 1;		// Don't try to sync with real time
       break;
     case 'L':
       if (argc-- < 1 || 0 == (val= readTime(*argv, &opt_L)) || 
	   1 == sscanf(*argv++ + val, " %c", &dmy)) 
	 usage();
       break;
     case 'R':
       if (argc-- < 1 || 1 != sscanf(*argv++, "%d %c", &out_prate, &dmy)) usage();
       break;
     case 'F':
       if (argc-- < 1 || 1 != sscanf(*argv++, "%d %c", &fade_int, &dmy)) usage();
       break;
     default:
       usage(); break;
    }
  }

  if (argc < 1) usage();

  if (opt_W && !opt_o && !opt_O)
    error("Use -o or -O with the -W option");
  if (opt_W && opt_L < 0 && !opt_E)
    error("Use -L or -E with -W option to give the length of the WAV file");

  if (opt_i) {
    // Immediate mode
    char *p= buf;
    p += sprintf(p, "immediate:");
    while (argc-- > 0) p += sprintf(p, " %s", *argv++);
    readSeqImm();
  }
  else {
    // Sequenced mode
    if (argc != 1) usage();
    readSeq(argv[0]);
  }

  loop();
  return 0;
}

//
//	Update a status line
//

void 
status(char *err) {
  int a;
  int nch= N_CH;
  char *p= buf, *p0, *p1;

  if (opt_Q) return;

#ifdef ANSI_TTY
  if (tty_erase) p += sprintf(p, "\033[K");
#endif

  p0= p;		// Start of line
  *p++= ' '; *p++= ' ';
  p += sprintTime(p, now);
  while (nch > 1 && chan[nch-1].v.typ == 0) nch--;
  for (a= 0; a<nch; a++)
    p += sprintVoice(p, &chan[a].v, 0);
  if (err) p += sprintf(p, " %s", err);
  p1= p;		// End of line

#ifndef ANSI_TTY
  while (tty_erase > p-p0) *p++= ' ';
#endif

  tty_erase= p1-p0;		// Characters that will need erasing
  fprintf(stderr, "%s\r", buf);
  fflush(stderr);
}

void 				// Display current period details
dispCurrPer(FILE *fp) {
  int a;
  Voice *v0, *v1;
  char *p0, *p1;
  int len0, len1;
  int nch= N_CH;

  if (opt_Q) return;

  p0= buf;
  p1= buf_copy;
  
  p0 += sprintf(p0, "* ");
  p0 += sprintTime(p0, per->tim);
  p1 += sprintf(p1, "  ");	
  p1 += sprintTime(p1, per->nxt->tim);

  v0= per->v0; v1= per->v1;
  while (nch > 1 && v0[nch-1].typ == 0) nch--;
  for (a= 0; a<nch; a++, v0++, v1++) {
    p0 += len0= sprintVoice(p0, v0, 0);
    p1 += len1= sprintVoice(p1, v1, v0);
    while (len0 < len1) { *p0++= ' '; len0++; }
    while (len1 < len0) { *p1++= ' '; len1++; }
  }
  *p0= 0; *p1= 0;
  fprintf(fp, "%s\n%s\n", buf, buf_copy);
  fflush(fp);
}

int
sprintTime(char *p, int tim) {
  return sprintf(p, "%02d:%02d:%02d",
		 tim % 86400000 / 3600000,
		 tim % 3600000 / 60000,
		 tim % 60000 / 1000);
}

int
sprintVoice(char *p, Voice *vp, Voice *dup) {
   switch (vp->typ) {
    case 0:
       return sprintf(p, " -");
    case 1:
       if (dup && vp->carr == dup->carr && vp->res == dup->res && vp->amp == dup->amp)
	  return sprintf(p, "  ::");
       return sprintf(p, " %.2f%+.2f/%.2f", vp->carr, vp->res, AMP_AD(vp->amp));
    case 2:
       if (dup && vp->amp == dup->amp)
	  return sprintf(p, "  ::");
       return sprintf(p, " pink/%.2f", AMP_AD(vp->amp));
    case 3:
       if (dup && vp->carr == dup->carr && vp->amp == dup->amp)
	  return sprintf(p, "  ::");
       return sprintf(p, " bell%+.2f/%.2f", vp->carr, AMP_AD(vp->amp));
    case 4:
       if (dup && vp->carr == dup->carr && vp->res == dup->res && vp->amp == dup->amp)
	  return sprintf(p, "  ::");
       return sprintf(p, " spin:%.2f%+.2f/%.2f", vp->carr, vp->res, AMP_AD(vp->amp));
    default:
       if (vp->typ < -100 || vp->typ > -1)
	  return sprintf(p, " ERROR");
       if (dup && vp->typ == dup->typ && 
	   vp->carr == dup->carr && vp->res == dup->res && vp->amp == dup->amp)
	  return sprintf(p, "  ::");
       return sprintf(p, " wave%02d:%.2f%+.2f/%.2f", -1-vp->typ, vp->carr, vp->res, AMP_AD(vp->amp));
   }
}

void 
init_sin_table() {
  int a;
  int *arr= (int*)CAlloc(ST_SIZ * sizeof(int));

  for (a= 0; a<ST_SIZ; a++)
    arr[a]= (int)(ST_AMP * sin((a * 3.14159265358979323846 * 2) / ST_SIZ));

  sin_table= arr;
}

void 
error(char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  exit(1);
}

void 
debug(char *fmt, ...) {
  va_list ap; va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
}

void *
CAlloc(size_t len) {
  void *p= calloc(1, len);
  if (!p) error("Out of memory");
  return p;
}

char *
StrDup(char *str) {
  char *rv= strdup(str);
  if (!rv) error("Out of memory");
  return rv;
}

static int time_midnight;

#ifndef MINGW
// Precalculate midnight to accelerate calcNow()
void 
setupMidnight() {
  struct tm *tt;
  time_t tim= time(0);
  tt= localtime(&tim);
  tt->tm_sec= 0;
  tt->tm_min= 0;
  tt->tm_hour= 0;
  time_midnight= mktime(tt);
}  

inline int  
calcNow() {
  struct timeval tv;
  if (0 != gettimeofday(&tv, 0)) error("Can't get current time");
  return ((tv.tv_sec - time_midnight) * 1000 + tv.tv_usec / 1000) % H24;
}
#else
void setupMidnight() { }

inline int  
calcNow() {
  SYSTEMTIME st;
  GetLocalTime(&st);
  return st.wMilliseconds + 1000*st.wSecond + 60000*st.wMinute + 3600000*st.wHour;
}
#endif

#if DEBUG_CHK_UTIME
inline int 
userTime() {
  struct tms buf;
  times(&buf);
  return buf.tms_utime;
}
#endif

//
//	Simple random number generator.  Generates a repeating
//	sequence of 65536 odd numbers in the range -65535->65535.
//
//	Based on ZX Spectrum random number generator:
//	  seed= (seed+1) * 75 % 65537 - 1
//

#define RAND_MULT 75

static int seed= 2;

//inline int qrand() {
//  return (seed= seed * 75 % 131074) - 65535;
//}

//
//	Generate next sample for simulated pink noise, with same
//	scaling as the sin_table[].  This version uses an inlined
//	random number generator, and smooths the lower frequency bands
//	as well.
//

#define NS_BANDS 9
typedef struct Noise Noise;
struct Noise {
  int val;		// Current output value
  int inc;		// Increment
};
Noise ntbl[NS_BANDS];
int nt_off;
int noise_buf[256];
uchar noise_off= 0;

static inline int 
noise2() {
  int tot;
  int off= nt_off++;
  int cnt= 1;
  Noise *ns= ntbl;
  Noise *ns1= ntbl + NS_BANDS;

  tot= ((seed= seed * RAND_MULT % 131074) - 65535) * (NS_AMP / 65535 / (NS_BANDS + 1));

  while ((cnt & off) && ns < ns1) {
    int val= ((seed= seed * RAND_MULT % 131074) - 65535) * (NS_AMP / 65535 / (NS_BANDS + 1));
    tot += ns->val += ns->inc= (val - ns->val) / (cnt += cnt);
    ns++;
  }

  while (ns < ns1) {
    tot += (ns->val += ns->inc);
    ns++;
  }

  return noise_buf[noise_off++]= (tot >> NS_ADJ);
}

//	//
//	//	Generate next sample for simulated pink noise, scaled the same
//	//	as the sin_table[].  This version uses a library random number
//	//	generator, and no smoothing.
//	//
//	
//	inline double 
//	noise() {
//	  int tot= 0;
//	  int bit= ~0;
//	  int a;
//	  int off;
//	
//	  ns_tbl[ns_off]= (rand() - (RAND_MAX / 2)) / (NS_BIT + 1);
//	  off= ns_off;
//	  for (a= 0; a<=NS_BIT; a++, bit <<= 1) {
//	    off &= bit;
//	    tot += ns_tbl[off];
//	  }
//	  ns_off= (ns_off + 1) & ((1<<NS_BIT) - 1);
//	
//	  return tot * (ST_AMP / (RAND_MAX * 0.5));
//	}

//
//	Play loop
//

void 
loop() {	
  int c, cnt;
  int err;		// Error to add to `now' until next cnt==0
  int fast= fast_mult != 0;
  int vfast= fast_mult > 20;		// Very fast - update status line often
  int utime= 0;
  int now_lo= 0;			// Low-order 16 bits of `now' (fractional)
  int err_lo= 0;
  int ms_inc;

  setup_device();
  spin_carr_max= 127.0 / 1E-6 / out_rate;
  cnt= 1 + 1999 / out_buf_ms;	// Update every 2 seconds or so
  now= opt_S ? fast_tim0 : calcNow();
  err= fast ? out_buf_ms * (fast_mult - 1) : 0;
  if (opt_L)
    byte_count= out_bps * (int)(opt_L * 0.001 * out_rate);
  if (opt_E)
    byte_count= out_bps * (int)(t_per0(now, fast_tim1) * 0.001 * out_rate /
				(fast ? fast_mult : 1));
  if (opt_W)
    writeWAV();

  if (!opt_Q) fprintf(stderr, "\n");
  corrVal(0);		// Get into correct period
  dispCurrPer(stderr);	// Display
  status(0);
  
  while (1) {
    for (c= 0; c < cnt; c++) {
      corrVal(1);
      outChunk();
      ms_inc= out_buf_ms + err;
      now_lo += out_buf_lo + err_lo;
      if (now_lo >= 0x10000) { ms_inc += now_lo >> 16; now_lo &= 0xFFFF; }
      now += ms_inc;
      if (now > H24) now -= H24;
      if (vfast && (c&1)) status(0);
    }

    if (fast) {
      if (!vfast) status(0);
    }
    else {
      // Synchronize with real clock, gently over the next second or so
      char buf[32];
      int diff= calcNow() - now;
      if (abs(diff) > H12) diff= 0;
      sprintf(buf, "(%d)", diff); 

      err_lo= diff * 0x10000 / cnt;
      err= err_lo >> 16;
      err_lo &= 0xFFFF;

      if (DEBUG_CHK_UTIME) {
	int prev= utime;
	utime= userTime();
	sprintf(buf, "%d ticks", utime-prev);		// Replaces standard message
      }
      status(buf);
    }
  }
}

//
//	Output a chunk of sound (a buffer-ful), then return
//
//	Note: Optimised for 16-bit output.  Eight-bit output is
//	slower, but then it probably won't have to run at as high a
//	sample rate.
//

void 
outChunk() {
   int off= 0;
   
   while (off < out_blen) {
      int ns= noise2();		// Use same pink noise source for everything
      int tot1, tot2;		// Left and right channels
      int val, a;
      Channel *ch;
      int *tab;
      
      tot1= tot2= 0;
      
      ch= &chan[0];
      for (a= 0; a<N_CH; a++, ch++) switch (ch->typ) {
       case 0:
	  break;
       case 1:	// Binaural tones
	  ch->off1 += ch->inc1;
	  ch->off1 &= (ST_SIZ << 16) - 1;
	  tot1 += ch->amp * sin_table[ch->off1 >> 16];
	  ch->off2 += ch->inc2;
	  ch->off2 &= (ST_SIZ << 16) - 1;
	  tot2 += ch->amp * sin_table[ch->off2 >> 16];
	  break;
       case 2:	// Pink noise
	  val= ns * ch->amp;
	  tot1 += val;
	  tot2 += val;
	  break;
       case 3:	// Bell
	  if (ch->off2) {
	     ch->off1 += ch->inc1;
	     ch->off1 &= (ST_SIZ << 16) - 1;
	     val= ch->off2 * sin_table[ch->off1 >> 16];
	     tot1 += val; tot2 += val;
	     if (--ch->inc2 < 0) {
		ch->inc2= out_rate/20;
		ch->off2 -= 1 + ch->off2 / 12;	// Knock off 10% each 50 ms
	     }
	  }
	  break;
       case 4:	// Spinning pink noise
	  ch->off1 += ch->inc1;
	  ch->off1 &= (ST_SIZ << 16) - 1;
	  val= (ch->inc2 * sin_table[ch->off1 >> 16]) >> 24;
	  tot1 += ch->amp * noise_buf[(uchar)(noise_off+128+val)];
	  tot2 += ch->amp * noise_buf[(uchar)(noise_off+128-val)];
	  break;
       default:	// Waveform-based binaural tones
	  tab= waves[-1 - ch->typ];
	  ch->off1 += ch->inc1;
	  ch->off1 &= (ST_SIZ << 16) - 1;
	  tot1 += ch->amp * tab[ch->off1 >> 16];
	  ch->off2 += ch->inc2;
	  ch->off2 &= (ST_SIZ << 16) - 1;
	  tot2 += ch->amp * tab[ch->off2 >> 16];
	  break;
      }
      
      out_buf[off++]= tot1 >> 16;
      out_buf[off++]= tot2 >> 16;
  }

  // Generate debugging amplitude output
  if (DEBUG_DUMP_AMP) {
    short *sp= out_buf;
    short *end= out_buf + out_blen;
    int max= 0;
    while (sp < end) {
       int val= (int)sp[0] + (int)sp[1]; sp += 2;
       if (val < 0) val= -val;
       if (val > max) max= val;
    }
    max /= 328;
    while (max-- > 0) putc('#', stdout);
    printf("\n"); fflush(stdout);
  }

  // Rewrite buffer for 8-bit mode
  if (out_mode == 0) {
    short *sp= out_buf;
    short *end= out_buf + out_blen;
    char *cp= (char*)out_buf;
    while (sp < end) *cp++= (*sp++ >> 8) + 128;
  }

  // Rewrite buffer for 16-bit byte-swapping
  if (out_mode == 2) {
    char *cp= (char*)out_buf;
    char *end= (char*)(out_buf + out_blen);
    while (cp < end) { char tmp= *cp++; cp[-1]= cp[0]; *cp++= tmp; }
  }

  // Check and update the byte count if necessary
  if (byte_count > 0) {
    if (byte_count <= out_bsiz) {
      writeOut((char*)out_buf, byte_count);
      exit(0);		// All done
    }
    else {
      writeOut((char*)out_buf, out_bsiz);
      byte_count -= out_bsiz;
    }
  }
  else
    writeOut((char*)out_buf, out_bsiz);
} 

void 
writeOut(char *buf, int siz) {
  int rv;

#ifdef MINGW
  if (out_fd == -9999) {
     // Win32 output: write it to a header and send it off
     MMRESULT rv;

     //debug_win32_buffer_status();

     //while (aud_cnt == BUFFER_COUNT) {
     //while (aud_head[aud_current]->dwFlags & WHDR_INQUEUE) {
     while (!(aud_head[aud_current]->dwFlags & WHDR_DONE)) {
	//debug("SLEEP %d", out_buf_ms / 2 + 1);
	Sleep(out_buf_ms / 2 + 1);
	//debug_win32_buffer_status();
     }

     memcpy(aud_head[aud_current]->lpData, buf, siz);
     aud_head[aud_current]->dwBufferLength= (DWORD)siz;

     //debug("Output buffer %d", aud_current);
     rv= waveOutWrite(aud_handle, aud_head[aud_current], sizeof(WAVEHDR));

     if (rv != MMSYSERR_NOERROR) {
        char buf[255];
        waveOutGetErrorText(rv, buf, sizeof(buf)-1);
        error("Error writing a fragment to the audio device:\n  %s", buf);
     }
   
     aud_cnt++; 
     aud_current++;
     aud_current %= BUFFER_COUNT;

     return;
  }
#endif

  while (-1 != (rv= write(out_fd, buf, siz))) {
    if (0 == (siz -= rv)) return;
    buf += rv;
  }
  error("Output error");
}

//
//	Correct values and types according to current period, and
//	current time
//

void 
corrVal(int running) {
  int a;
  int t0= per->tim;
  int t1= per->nxt->tim;
  Channel *ch;
  Voice *v0, *v1;
  double rat0, rat1;
  double amp, carr, res;
  int trigger= 0;

  while ((now >= t0) ^ (now >= t1) ^ (t1 > t0)) {
    per= per->nxt;
    t0= per->tim;
    t1= per->nxt->tim;
    if (running) {
      if (tty_erase) {
#ifdef ANSI_TTY	
	fprintf(stderr, "\033[K");
#else
	fprintf(stderr, "%*s\r", tty_erase, ""); 
	tty_erase= 0;
#endif
      }
      dispCurrPer(stderr); status(0);
    }
    trigger= 1;		// Trigger bells or whatever
  }

  rat1= t_per0(t0, now) / (double)t_per24(t0, t1);
  rat0= 1 - rat1;

  ch= &chan[0];
  v0= &per->v0[0];
  v1= &per->v1[0];

  for (a= 0; a<N_CH; a++, ch++, v0++, v1++) {
     if (ch->v.typ != v0->typ) {
	switch (ch->v.typ= ch->typ= v0->typ) {
	 case 1:
	    ch->off1= ch->off2= 0; break;
	 case 2:
	    break;
	 case 3:
	    ch->off1= ch->off2= 0; break;
	 case 4:
	    ch->off1= ch->off2= 0; break;
	 default:
	    ch->off1= ch->off2= 0; break;
	}
     }
    
    switch (ch->v.typ) {
     case 1:
       amp= rat0 * v0->amp + rat1 * v1->amp;
       carr= rat0 * v0->carr + rat1 * v1->carr;
       res= rat0 * v0->res + rat1 * v1->res;
       ch->v.amp= amp;
       ch->v.carr= carr;
       ch->v.res= res;
       ch->amp= (int)amp;
       ch->inc1= (int)((carr + res/2) / out_rate * ST_SIZ * 65536);
       ch->inc2= (int)((carr - res/2) / out_rate * ST_SIZ * 65536);
       break;
     case 2:
       ch->amp= (int)(ch->v.amp= rat0 * v0->amp + rat1 * v1->amp);
       break;
     case 3:
       amp= v0->amp;		// No need to slide, as bell only rings briefly
       carr= v0->carr;
       ch->amp= ch->v.amp= amp;
       ch->v.carr= carr;
       ch->inc1= (int)(carr / out_rate * ST_SIZ * 65536);
       if (trigger) {		// Trigger the bell only on entering the period
	 ch->off2= ch->amp;
	 ch->inc2= out_rate/20;
       }
       break;
     case 4:
       amp= rat0 * v0->amp + rat1 * v1->amp;
       carr= rat0 * v0->carr + rat1 * v1->carr;
       res= rat0 * v0->res + rat1 * v1->res;
       if (carr > spin_carr_max) carr= spin_carr_max;		// Clipping sweep width
       if (carr < -spin_carr_max) carr= -spin_carr_max;
       ch->v.amp= amp;
       ch->v.carr= carr;
       ch->v.res= res;
       ch->amp= (int)amp;
       ch->inc1= (int)(res / out_rate * ST_SIZ * 65536);
       ch->inc2= (int)(carr * 1E-6 * out_rate * (1<<24) / ST_AMP);
       break;
     default:		// Waveform based binaural
       amp= rat0 * v0->amp + rat1 * v1->amp;
       carr= rat0 * v0->carr + rat1 * v1->carr;
       res= rat0 * v0->res + rat1 * v1->res;
       ch->v.amp= amp;
       ch->v.carr= carr;
       ch->v.res= res;
       ch->amp= (int)amp;
       ch->inc1= (int)((carr + res/2) / out_rate * ST_SIZ * 65536);
       ch->inc2= (int)((carr - res/2) / out_rate * ST_SIZ * 65536);
       if (ch->inc1 > ch->inc2) 
         ch->inc2= -ch->inc2;
       else 
         ch->inc1= -ch->inc1;
       break;
    }
  }
}       
      
//
//	Setup audio device
//

void 
setup_device(void) {

  // Handle output to files and pipes
  if (opt_O || opt_o) {
    if (opt_O)
      out_fd= 1;		// stdout
    else {
      FILE *out;		// Need to create a stream to set binary mode for DOS
      if (!(out= fopen(opt_o, "wb")))
	error("Can't open \"%s\", errno %d", opt_o, errno);
      out_fd= fileno(out);
    }
    out_blen= out_rate * 2 / out_prate;		// 10 fragments a second by default
    while (out_blen & (out_blen-1)) out_blen &= out_blen-1;		// Make power of two
    out_bsiz= out_blen * (out_mode ? 2 : 1);
    out_bps= out_mode ? 4 : 2;
    out_buf= (short*)CAlloc(out_blen * sizeof(short));
    out_buf_lo= (int)(0x10000 * 1000.0 * 0.5 * out_blen / out_rate);
    out_buf_ms= out_buf_lo >> 16;
    out_buf_lo &= 0xFFFF;

    if (!opt_Q && !opt_W)		// Informational message for opt_W is written later
      fprintf(stderr, 
	      "Outputting %d-bit raw audio data at %d Hz with %d-sample blocks, %d ms per block\n",
	      out_mode ? 16 : 8, out_rate, out_blen/2, out_buf_ms);
    return;
  }

#ifdef DSP
  // Normal /dev/dsp output
  {
    int stereo, rate, fragsize, numfrags, enc;
    int afmt_req, afmt;
    int test= 1;
    audio_buf_info info;
    if (0 > (out_fd= open("/dev/dsp", O_WRONLY)))
      error("Can't open /dev/dsp, errno %d", errno);
    
    afmt= afmt_req= ((out_mode == 0) ? AFMT_U8 : 
		     ((char*)&test)[0] ? AFMT_S16_LE : AFMT_S16_BE);
    stereo= 1;
    rate= out_rate;
    fragsize= 14;
    numfrags= 4;	
    
    enc= (numfrags<<16) | fragsize;
    
    if (0 > ioctl(out_fd, SNDCTL_DSP_SETFRAGMENT, &enc) ||
	0 > ioctl(out_fd, SNDCTL_DSP_SAMPLESIZE, &afmt) ||
	0 > ioctl(out_fd, SNDCTL_DSP_STEREO, &stereo) ||
	0 > ioctl(out_fd, SNDCTL_DSP_SPEED, &rate))
      error("Can't configure /dev/dsp, errno %d", errno);
    
    if (afmt != afmt_req) 
      error("Can't open device in %d-bit mode", out_mode ? 16 : 8);
    if (!stereo)
      error("Can't open device in stereo");
    
    out_rate= rate;
    
    if (-1 == ioctl(out_fd, SNDCTL_DSP_GETOSPACE, &info))
      error("Can't get audio buffer info, errno %d", errno);
    out_bsiz= info.fragsize;
    out_blen= out_mode ? out_bsiz/2 : out_bsiz;
    out_bps= out_mode ? 4 : 2;
    out_buf= (short*)CAlloc(out_blen * sizeof(short));
    out_buf_lo= (int)(0x10000 * 1000.0 * 0.5 * out_blen / out_rate);
    out_buf_ms= out_buf_lo >> 16;
    out_buf_lo &= 0xFFFF;
    
    if (!opt_Q)
      fprintf(stderr, 
	      "Outputting %d-bit audio at %d Hz with %d %d-sample fragments, %d ms per fragment\n",
	      out_mode ? 16 : 8, out_rate, info.fragstotal, out_blen/2, out_buf_ms);
  }
#else // DSP
#ifdef MINGW
  // Output using Win32 calls
  {
     MMRESULT rv;
     WAVEFORMATEX fmt;
     int a;

     fmt.wFormatTag= WAVE_FORMAT_PCM;           
     fmt.nChannels= 2;
     fmt.nSamplesPerSec= out_rate;
     fmt.wBitsPerSample= out_mode ? 16 : 8;
     fmt.nBlockAlign= 1;
     fmt.nAvgBytesPerSec= out_rate * 2 * (out_mode ? 2 : 1);
     fmt.cbSize= 0;
     aud_handle= NULL;

     // if (MMSYSERR_NOERROR != 
     //    waveOutOpen(&aud_handle, WAVE_MAPPER, &fmt, 0, 
     //                0L, WAVE_FORMAT_QUERY))
     //    error("Windows is rejecting our audio request (%d-bit stereo, %dHz)",
     //          out_mode ? 16 : 8, out_rate);

     if (MMSYSERR_NOERROR != 
	 (rv= waveOutOpen(&aud_handle, WAVE_MAPPER, 
			  (WAVEFORMATEX*)&fmt, (DWORD)win32_audio_callback, 
			  (DWORD)0, CALLBACK_FUNCTION))) {
	char buf[255];
	waveOutGetErrorText(rv, buf, sizeof(buf)-1);
	error("Can't open audio device (%d-bit stereo, %dHz):\n  %s",
               out_mode ? 16 : 8, out_rate, buf);
     }

     if (fmt.nChannels != 2)
	error("Can't open audio device in stereo");
     if (fmt.wBitsPerSample != (out_mode ? 16 : 8))
	error("Can't open audio device in %d-bit mode", out_mode ? 16 : 8);
     
     aud_current= 0;
     aud_cnt= 0;

     for (a= 0; a<BUFFER_COUNT; a++) {
	char *p= CAlloc(sizeof(WAVEHDR) + BUFFER_SIZE);
	WAVEHDR *w= aud_head[a]= (WAVEHDR*)p;

	w->lpData= (LPSTR)p + sizeof(WAVEHDR);
	w->dwBufferLength= (DWORD)BUFFER_SIZE;
	w->dwBytesRecorded= 0L;
	w->dwUser= 0;
	w->dwFlags= 0;
	w->dwLoops= 0;
	w->lpNext= 0;
	w->reserved= 0;

	rv= waveOutPrepareHeader(aud_handle, w, sizeof(WAVEHDR));
	if (rv != MMSYSERR_NOERROR) {
	   char buf[255];
	   waveOutGetErrorText(rv, buf, sizeof(buf)-1);
	   error("Can't setup a wave header %d:\n  %s", a, buf);
	}
	w->dwFlags |= WHDR_DONE;
     }     
     
     out_rate= fmt.nSamplesPerSec;
     out_bsiz= BUFFER_SIZE;
     out_blen= out_mode ? out_bsiz/2 : out_bsiz;
     out_bps= out_mode ? 4 : 2;
     out_buf= (short*)CAlloc(out_blen * sizeof(short));
     out_buf_lo= (int)(0x10000 * 1000.0 * 0.5 * out_blen / out_rate);
     out_buf_ms= out_buf_lo >> 16;
     out_buf_lo &= 0xFFFF;
     out_fd= -9999;

     if (!opt_Q)
	fprintf(stderr, 
		"Outputting %d-bit audio at %d Hz with %d %d-sample fragments, %d ms per fragment\n",
		out_mode ? 16 : 8, out_rate, BUFFER_COUNT, out_blen/2, out_buf_ms);
  }
#else // MINGW
  error("Direct output to soundcard not supported on this platform.\n"
	"Use -o or -O to write raw data, or -Wo or -WO to write a WAV file.");
#endif // MINGW
#endif // DSP
}

//
//	Audio callback for Win32
//

#if MINGW
void CALLBACK
win32_audio_callback(HWAVEOUT hand, UINT uMsg,     
		     DWORD dwInstance, DWORD dwParam1, DWORD dwParam2) {
   switch (uMsg) {
    case WOM_CLOSE:
       break;
    case WOM_OPEN:
       break;
    case WOM_DONE:
       aud_cnt--;
       //debug("Buffer done (cnt==%d)", aud_cnt);
       //debug_win32_buffer_status();
       break;
   }
}

int debug_win32_buffer_status() {
   char tmp[80];
   char *p= tmp;
   int a;
   for (a= 0; a<BUFFER_COUNT; a++) {
      *p++= (aud_head[a]->dwFlags & WHDR_INQUEUE) ? 'I' : '-';
      *p++= (aud_head[a]->dwFlags & WHDR_DONE) ? 'D' : '-';
      *p++= ' ';
   }
   p[-1]= 0;
   debug(tmp);
}
#endif

//
//	Write a WAV header, and setup out_mode if byte-swapping is
//	required.  `byte_count' should have been set up by this point.
//

#define addU4(xx) { int a= xx; *p++= a; *p++= (a >>= 8); *p++= (a >>= 8); *p++= (a >>= 8); }
#define addStr(xx) { char *q= xx; *p++= *q++; *p++= *q++; *p++= *q++; *p++= *q++; }

void 
writeWAV() {
  char buf[44], *p= buf;
  addStr("RIFF");
  addU4(byte_count + 36);
  addStr("WAVE");
  addStr("fmt ");
  addU4(16);
  addU4(0x00020001);
  addU4(out_rate);
  addU4(out_rate * out_bps);
  addU4(0x0004 + 0x10000*(out_bps*4));	// 2,4 -> 8,16 - always assume stereo
  addStr("data");
  addU4(byte_count);
  writeOut(buf, 44);

  if (out_mode == 1) {
    short test= 0x0011;
    if (!*(char*)&test) out_mode= 2;		// Turn on byte-swapping
  }

  if (!opt_Q)
    fprintf(stderr, 
	    "Outputting %d-bit WAV data at %d Hz, file size %d bytes\n",
	    out_mode ? 16 : 8, out_rate, byte_count + 44);
}

//
//	Read a line, discarding blank lines and comments.  Rets: Another line ?
//   

int 
readLine() {
  char *p;
  lin= buf;

  while (1) {
    if (!fgets(lin, sizeof(buf), in)) {
      if (feof(in)) return 0;
      error("Read error on sequence file");
    }

    in_lin++;
    
    while (isspace(*lin)) lin++;
    p= strchr(lin, '#');
    p= p ? p : strchr(lin, 0);
    while (p > lin && isspace(p[-1])) p--;
    if (p != lin) break;
  }
  *p= 0;
  lin_copy= buf_copy;
  strcpy(lin_copy, lin);
  return 1;
}

//
//	Get next word at `*lin', moving lin onwards, or return 0
//

char *
getWord() {
  char *rv, *end;
  while (isspace(*lin)) lin++;
  if (!*lin) return 0;

  rv= lin;
  while (*lin && !isspace(*lin)) lin++;
  end= lin;
  if (*lin) lin++;
  *end= 0;

  return rv;
}

//
//	Bad sequence file
//

void 
badSeq() {
  error("Bad sequence file content at line: %d\n  %s", in_lin, lin_copy);
}

//
//	Generate a list of Period structures, based on a single input
//	line in the buf[] array.
//

void 
readSeqImm() {
  in_lin= 0;
  lin= buf; lin_copy= buf_copy;
  strcpy(lin_copy, lin);
  readNameDef();

  lin= buf; lin_copy= buf_copy;
  strcpy(lin, "00:00 immediate");
  strcpy(lin_copy, lin);
  readTimeLine();

  correctPeriods();
}

//
//	Read the sequence file, and generate a list of Period structures
//

void 
readSeq(char *fnam) {
  // Setup a `now' value to use for NOW in the sequence file
  now= calcNow();	

  in= (0 == strcmp("-", fnam)) ? stdin : fopen(fnam, "r");
  if (!in) error("Can't open sequence file");
  
  in_lin= 0;
  
  while (readLine()) {
    // Check to see if it fits the form of <name>:<white-space>
    char *p= lin;
    if (!isalpha(*p)) 
      p= 0;
    else {
      while (isalnum(*p) || *p == '_' || *p == '-') p++;
      if (*p++ != ':' || !isspace(*p)) 
	p= 0;
    }
    
    if (p)
      readNameDef();
    else 
      readTimeLine();
  }
  
  correctPeriods();
}


//
//	Fill in all the correct information for the Periods, assuming
//	they have just been loaded using readTimeLine()
//


void 
correctPeriods() {
  // Get times all correct
  {
    Period *pp= per;
    do {
      if (pp->fi == -2) {
	pp->tim= pp->nxt->tim;
	pp->fi= -1;
      }

      pp= pp->nxt;
    } while (pp != per);
  }

  // Make sure that the transitional periods each have enough time
  {
    Period *pp= per;
    do {
      if (pp->fi == -1) {
	int per= t_per0(pp->tim, pp->nxt->tim);
	if (per < fade_int) {
	  int adj= (fade_int - per) / 2, adj0, adj1;
	  adj0= t_per0(pp->prv->tim, pp->tim);
	  adj0= (adj < adj0) ? adj : adj0;
	  adj1= t_per0(pp->nxt->tim, pp->nxt->nxt->tim);
	  adj1= (adj < adj1) ? adj : adj1;
	  pp->tim= (pp->tim - adj0 + H24) % H24;
	  pp->nxt->tim= (pp->nxt->tim + adj1) % H24;
	}
      }

      pp= pp->nxt;
    } while (pp != per);
  }

  // Fill in all the voice arrays, and sort out details of
  // transitional periods
  {
    Period *pp= per;
    do {
      if (pp->fi < 0) {
	int fo, fi;
	int a;
	int midpt= 0;

	Period *qq= (Period*)CAlloc(sizeof(*qq));
	qq->prv= pp; qq->nxt= pp->nxt;
	qq->prv->nxt= qq->nxt->prv= qq;

	qq->tim= t_mid(pp->tim, qq->nxt->tim);

	memcpy(pp->v0, pp->prv->v1, sizeof(pp->v0));
	memcpy(qq->v1, qq->nxt->v0, sizeof(qq->v1));

	// Special handling for bells
	for (a= 0; a<N_CH; a++) {
	  if (pp->v0[a].typ == 3 && pp->fi != -3)
	    pp->v0[a].typ= 0;

	  if (qq->v1[a].typ == 3 && pp->fi == -3)
	    qq->v1[a].typ= 0;
	}
	      
	fo= pp->prv->fo;
	fi= qq->nxt->fi;

	// Special handling for -> slides:
	//   always slide, and stretch slide if possible
	if (pp->fi == -3) {
	  fo= fi= 2;		// Force slides for ->
	  for (a= 0; a<N_CH; a++) {
	    Voice *vp= &pp->v0[a];
	    Voice *vq= &qq->v1[a];
	    if (vp->typ == 0 && vq->typ != 0 && vq->typ != 3) {
	      memcpy(vp, vq, sizeof(*vp)); vp->amp= 0;
	    }
	    else if (vp->typ != 0 && vq->typ == 0) {
	      memcpy(vq, vp, sizeof(*vq)); vq->amp= 0;
	    }
	  }
	}

	memcpy(pp->v1, pp->v0, sizeof(pp->v1));
	memcpy(qq->v0, qq->v1, sizeof(qq->v0));

	for (a= 0; a<N_CH; a++) {
	  Voice *vp= &pp->v1[a];
	  Voice *vq= &qq->v0[a];
	  if ((fo == 0 || fi == 0) ||		// Fade in/out to silence
	      (vp->typ != vq->typ) ||		// Different types
	      ((fo == 1 || fi == 1) &&		// Fade thru, but different pitches
	       (vp->typ == 1 || vp->typ < 0) && 
	       (vp->carr != vq->carr || vp->res != vq->res))
	      ) {
	    vp->amp= vq->amp= 0;		// To silence
	    midpt= 1;				// Definitely need the mid-point

	    if (vq->typ == 3) {	 		// Special handling for bells
	      vq->amp= qq->v1[a].amp; 
	      qq->nxt->v0[a].typ= qq->nxt->v1[a].typ= 0;
	    }
	  }
	  else if (vp->typ == 3) {		// Else smooth transition - for bells not so smooth
	    qq->v0[a].typ= qq->v1[a].typ= 0;
	  }
	  else {				// Else smooth transition
	    vp->amp= vq->amp= (vp->amp + vq->amp) / 2;
	    if (vp->typ == 1 || vp->typ == 4 || vp->typ < 0) {
	      vp->carr= vq->carr= (vp->carr + vq->carr) / 2;
	      vp->res= vq->res= (vp->res + vq->res) / 2;
	    }
	  }
	}

	// If we don't really need the mid-point, then get rid of it
	if (!midpt) {
	  memcpy(pp->v1, qq->v1, sizeof(pp->v1));
	  qq->prv->nxt= qq->nxt;
	  qq->nxt->prv= qq->prv;
	  free(qq);
	}
	else pp= qq;
      }

      pp= pp->nxt;
    } while (pp != per);
  }

  // Clear out zero length sections, and duplicate sections
  {
    Period *pp;
    while (per != per->nxt) {
      pp= per;
      do {
	if (voicesEq(pp->v0, pp->v1) &&
	    voicesEq(pp->v0, pp->nxt->v0) &&
	    voicesEq(pp->v0, pp->nxt->v1))
	  pp->nxt->tim= pp->tim;

	if (pp->tim == pp->nxt->tim) {
	  if (per == pp) per= per->prv;
	  pp->prv->nxt= pp->nxt;
	  pp->nxt->prv= pp->prv;
	  free(pp);
	  pp= 0;
	  break;
	}
	pp= pp->nxt;
      } while (pp != per);
      if (pp) break;
    }
  }

  // Make sure that the total is 24 hours only (not more !)
  if (per->nxt != per) {
    int tot= 0;
    Period *pp= per;
    
    do {
      tot += t_per0(pp->tim, pp->nxt->tim);
      pp= pp->nxt;
    } while (pp != per);

    if (tot > H24) {
      fprintf(stderr, 
	      "Total time is greater than 24 hours.  Probably two times are\n"
	      "out of order.  Suspicious intervals are:\n\n");
      pp= per;
      do {
	if (t_per0(pp->tim, pp->nxt->tim) >= H12) 
	  fprintf(stderr, "  %02d:%02d:%02d -> %02d:%02d:%02d\n",
		  pp->tim % 86400000 / 3600000,
		  pp->tim % 3600000 / 60000,
		  pp->tim % 60000 / 1000,
		  pp->nxt->tim % 86400000 / 3600000,
		  pp->nxt->tim % 3600000 / 60000,
		  pp->nxt->tim % 60000 / 1000);
	pp= pp->nxt;
      } while (pp != per);
      error("\nCheck the sequence around these times and try again");
    }
  }

  // Print the whole lot out
  if (opt_D) {
    Period *pp;
    if (per->nxt != per)
      while (per->prv->tim < per->tim) per= per->nxt;

    pp= per;
    do {
      dispCurrPer(stdout);
      per= per->nxt;
    } while (per != pp);
    printf("\n");

    exit(0);		// All done
  }  
}

int 
voicesEq(Voice *v0, Voice *v1) {
  int a= N_CH;

  while (a-- > 0) {
    if (v0->typ != v1->typ) return 0;
    switch (v0->typ) {
     case 1:
     case 4:
     default:
       if (v0->amp != v1->amp ||
	   v0->carr != v1->carr ||
	   v0->res != v1->res)
	 return 0;
       break;
     case 2:
       if (v0->amp != v1->amp)
	 return 0;
       break;
     case 3:
       if (v0->amp != v1->amp ||
	   v0->carr != v1->carr)
	 return 0;
       break;
    }
    v0++; v1++;
  }
  return 1;
}

//
//	Read a name definition
//

void 
readNameDef() {
  char *p, *q;
  NameDef *nd;
  int ch;

  if (!(p= getWord())) badSeq();

  q= strchr(p, 0) - 1;
  if (*q != ':') badSeq();
  *q= 0;
  for (q= p; *q; q++) if (!isalnum(*q) && *q != '-' && *q != '_') 
    error("Bad name \"%s\" in definition, line %d:\n  %s", p, in_lin, lin_copy);

  // Waveform definition ?
  if (0 == memcmp(p, "wave", 4) && 
      isdigit(p[4]) &&
      isdigit(p[5]) &&
      !p[6]) {
     int ii= (p[4] - '0') * 10 + (p[5] - '0');
     int siz= ST_SIZ * sizeof(int);
     int *arr= (int*)CAlloc(siz);
     double *dp0= (double*)arr;
     double *dp1= (double*)(siz + (char*)arr);
     double *dp= dp0;
     double dmax= 0, dmin= 1;
     int np;

     if (waves[ii])
	error("Waveform %02d already defined, line %d:\n  %s",
	      ii, in_lin, lin_copy);
     waves[ii]= arr;
     
     while ((p= getWord())) {
	double dd;
	char dmy;
	if (1 != sscanf(p, "%lf %c", &dd, &dmy)) 
	   error("Expecting floating-point numbers on this waveform definition line, line %d:\n  %s",
		 in_lin, lin_copy);
	if (dp >= dp1)
	   error("Too many samples on line (maximum %d), line %d:\n  %s",
		 dp1-dp0, in_lin, lin_copy);
	*dp++= dd;
	if (dmax < dmin) dmin= dmax= dd;
	else {
	   if (dd > dmax) dmax= dd;
	   if (dd < dmin) dmin= dd;
	}
     }
     dp1= dp;
     np= dp1 - dp0;
     if (np < 2) 
	error("Expecting at least two samples in the waveform, line %d:\n  %s",
	      in_lin, lin_copy);

     // Adjust to range 0-1
     for (dp= dp0; dp < dp1; dp++)
	*dp= (*dp - dmin) / (dmax - dmin);

     sinc_interpolate(dp0, np, arr);
     
     if (DEBUG_DUMP_WAVES) {
	int a;
	printf("Dumping wave%02d:\n", ii);
	for (a= 0; a<ST_SIZ; a++)
	   printf("%d %g\n", a, arr[a] * 1.0 / ST_AMP);
     }
     return;
  } 

  // Must be block or tone-set, then, so put into a NameDef
  nd= (NameDef*)CAlloc(sizeof(NameDef));
  nd->name= StrDup(p);

  // Block definition ?
  if (*lin == '{') {
    BlockDef *bd, **prvp;
    if (!(p= getWord()) || 
	0 != strcmp(p, "{") || 
	0 != (p= getWord()))
      badSeq();

    prvp= &nd->blk;
    
    while (readLine()) {
      if (*lin == '}') {
	if (!(p= getWord()) || 
	    0 != strcmp(p, "}") || 
	    0 != (p= getWord()))
	  badSeq();
	if (!nd->blk) error("Empty blocks not permitted, line %d:\n  %s", in_lin, lin_copy);
	nd->nxt= nlist; nlist= nd;
	return;
      }
      
      if (*lin != '+') 
	error("All lines in the block must have relative time, line %d:\n  %s",
	      in_lin, lin_copy);
      
      bd= (BlockDef*) CAlloc(sizeof(*bd));
      *prvp= bd; prvp= &bd->nxt;
      bd->lin= StrDup(lin);
    }
    
    // Hit EOF before }
    error("End-of-file within block definition (missing '}')");
  }

  // Normal line-definition
  for (ch= 0; ch < N_CH && (p= getWord()); ch++) {
    char dmy;
    double amp, carr, res;
    int wave;

    // Interpret word into Voice nd->vv[ch]
    if (0 == strcmp(p, "-")) continue;
    if (1 == sscanf(p, "pink/%lf %c", &amp, &dmy)) {
      nd->vv[ch].typ= 2;
      nd->vv[ch].amp= AMP_DA(amp);
      continue;
    }
    if (2 == sscanf(p, "bell%lf/%lf %c", &carr, &amp, &dmy)) {
      nd->vv[ch].typ= 3;
      nd->vv[ch].carr= carr;
      nd->vv[ch].amp= AMP_DA(amp);
      continue;
    }
    if (4 == sscanf(p, "wave%d:%lf%lf/%lf %c", &wave, &carr, &res, &amp, &dmy)) {
       if (wave < 0 || wave >= 100)
	  error("Only wave00 to wave99 is permitted at line: %d\n  %s", in_lin, lin_copy);
       if (!waves[wave])
	  error("Waveform %02d has not been defined, line: %d\n  %s", wave, in_lin, lin_copy);
       nd->vv[ch].typ= -1-wave;
       nd->vv[ch].carr= carr;
       nd->vv[ch].res= res;
       nd->vv[ch].amp= AMP_DA(amp);	
       continue;
    }
    if (3 == sscanf(p, "%lf%lf/%lf %c", &carr, &res, &amp, &dmy)) {
      nd->vv[ch].typ= 1;
      nd->vv[ch].carr= carr;
      nd->vv[ch].res= res;
      nd->vv[ch].amp= AMP_DA(amp);	
      continue;
    }
    if (3 == sscanf(p, "spin:%lf%lf/%lf %c", &carr, &res, &amp, &dmy)) {
      nd->vv[ch].typ= 4;
      nd->vv[ch].carr= carr;
      nd->vv[ch].res= res;
      nd->vv[ch].amp= AMP_DA(amp);	
      continue;
    }
    badSeq();
  }
  nd->nxt= nlist; nlist= nd;
}  

//
//	Bad time
//

void 
badTime(char *tim) {
  error("Badly constructed time \"%s\", line %d:\n  %s", tim, in_lin, lin_copy);
}

//
//	Read a time-line of either type
//

void 
readTimeLine() {
  char *p, *tim_p;
  int nn;
  int fo, fi;
  Period *pp;
  NameDef *nd;
  static int last_abs_time= -1;
  int tim, rtim;

  if (!(p= getWord())) badSeq();
  tim_p= p;
  
  // Read the time represented
  tim= -1;
  if (0 == memcmp(p, "NOW", 3)) {
    last_abs_time= tim= now;
    p += 3;
  }

  while (*p) {
    if (*p == '+') {
      if (tim < 0) {
	if (last_abs_time < 0) 
	  error("Relative time without previous absolute time, line %d:\n  %s", in_lin, lin_copy);
	tim= last_abs_time;
      }
      p++;
    }
    else if (tim != -1) badTime(tim_p);

    if (0 == (nn= readTime(p, &rtim))) badTime(tim_p);
    p += nn;

    if (tim == -1) 
      last_abs_time= tim= rtim;
    else 
      tim= (tim + rtim) % H24;
  }

  if (fast_tim0 < 0) fast_tim0= tim;		// First time
  fast_tim1= tim;				// Last time
      
  if (!(p= getWord())) badSeq();
      
  fi= fo= 1;
  if (!isalpha(*p)) {
    switch (p[0]) {
     case '<': fi= 0; break;
     case '-': fi= 1; break;
     case '=': fi= 2; break;
     default: badSeq();
    }
    switch (p[1]) {
     case '>': fo= 0; break;
     case '-': fo= 1; break;
     case '=': fo= 2; break;
     default: badSeq();
    }
    if (p[2]) badSeq();

    if (!(p= getWord())) badSeq();
  }
      
  for (nd= nlist; nd && 0 != strcmp(p, nd->name); nd= nd->nxt) ;
  if (!nd) error("Name \"%s\" not defined, line %d:\n  %s", p, in_lin, lin_copy);

  // Check for block name-def
  if (nd->blk) {
    char *prep= StrDup(tim_p);		// Put this at the start of each line
    BlockDef *bd= nd->blk;

    while (bd) {
      lin= buf; lin_copy= buf_copy;
      sprintf(lin, "%s%s", prep, bd->lin);
      strcpy(lin_copy, lin);
      readTimeLine();		// This may recurse, and that's why we're StrDuping the string
      bd= bd->nxt;
    }
    free(prep);
    return;
  }
      
  // Normal name-def
  pp= (Period*)CAlloc(sizeof(*pp));
  pp->tim= tim;
  pp->fi= fi;
  pp->fo= fo;
      
  memcpy(pp->v0, nd->vv, N_CH * sizeof(Voice));
  memcpy(pp->v1, nd->vv, N_CH * sizeof(Voice));

  if (!per)
    per= pp->nxt= pp->prv= pp;
  else {
    pp->nxt= per; pp->prv= per->prv;
    pp->prv->nxt= pp->nxt->prv= pp;
  }

  // Automatically add a transitional period
  pp= (Period*)CAlloc(sizeof(*pp));
  pp->fi= -2;		// Unspecified transition
  pp->nxt= per; pp->prv= per->prv;
  pp->prv->nxt= pp->nxt->prv= pp;

  if (0 != (p= getWord())) {
    if (0 != strcmp(p, "->")) badSeq();
    pp->fi= -3;		// Special `->' transition
    pp->tim= tim;
  }
}

int
readTime(char *p, int *timp) {		// Rets chars consumed, or 0 error
  int nn, hh, mm, ss;

  if (3 > sscanf(p, "%2d:%2d:%2d%n", &hh, &mm, &ss, &nn)) {
    ss= 0;
    if (2 > sscanf(p, "%2d:%2d%n", &hh, &mm, &nn)) return 0;
  }

  if (hh < 0 || hh >= 24 ||
      mm < 0 || mm >= 60 ||
      ss < 0 || ss >= 60) return 0;

  *timp= ((hh * 60 + mm) * 60 + ss) * 1000;
  return nn;
}

//
//	Takes a set of points and repeats them twice, inverting the
//	second set, and then interpolates them using a periodic sinc
//	function (see http://www-ccrma.stanford.edu/~jos/resample/)
//	and writes them to arr[] in the same format as the sin_table[].
//

void sinc_interpolate(double *dp, int np, int *arr) {
   double *sinc;	// Temporary sinc-table
   double *out;		// Temporary output table
   int a, b;
   double dmax, dmin;
   double adj, off;

   // Generate a modified periodic sin(x)/x function to be used for
   // each of the points.  Really this should be sin(x)/x modified
   // by the sum of an endless series.  However, this doesn't
   // converge very quickly, so to save time I'm approximating this
   // series by 1-4*t*t where t ranges from 0 to 0.5 over the first
   // half of the periodic cycle.  If you do the maths, this is at
   // most 5% out.  This will have to do - it's smooth, and I don't
   // know enough maths to make this series converge quicker.
   sinc= CAlloc(ST_SIZ * sizeof(double));
   sinc[0]= 1.0;
   for (a= ST_SIZ/2; a>0; a--) {
      double tt= a * 1.0 / ST_SIZ;
      double t2= tt*tt;
      double adj= 1 - 4 * t2;
      double xx= 2 * np * 3.14159265358979323846 * tt;
      double vv= adj * sin(xx) / xx;
      sinc[a]= vv;
      sinc[ST_SIZ-a]= vv;
   }
   
   // Build waveform into buffer
   out= CAlloc(ST_SIZ * sizeof(double));
   for (b= 0; b<np; b++) {
      int off= b * ST_SIZ / np / 2;
      double val= dp[b];
      for (a= 0; a<ST_SIZ; a++) {
	 out[(a + off)&(ST_SIZ-1)] += sinc[a] * val;
	 out[(a + off + ST_SIZ/2)&(ST_SIZ-1)] -= sinc[a] * val;
      }
   }

   // Look for maximum for normalization
   dmax= dmin= 0;
   for (a= 0; a<ST_SIZ; a++) {
      if (out[a] > dmax) dmax= out[a];
      if (out[a] < dmin) dmin= out[a];
   }

   // Write out to output buffer
   off= -0.5 * (dmax + dmin);
   adj= ST_AMP / ((dmax - dmin) / 2);
   for (a= 0; a<ST_SIZ; a++)
      arr[a]= (int)((out[a] + off) * adj);

   free(sinc);
   free(out);
}

// END //
