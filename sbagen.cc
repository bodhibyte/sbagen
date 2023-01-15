//
//	SBaGen - Sequenced BinAural Generator
//
//	(c) 1999 Jim Peters <jim@aguazul.demon.co.uk>.  All Rights Reserved.
//	For latest version see <http://www.aguazul.demon.co.uk/bagen/>.
//	Released under the GNU GPL.  Use at your own risk.
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
//	If you really don't have a copy of the GNU GPL, I'll send you one.
//	

#define VERSION "1.0.1"

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
#include <sys/ioctl.h>
#include <time.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/soundcard.h>

typedef struct Channel Channel;
typedef struct Voice Voice;
typedef struct Period Period;
typedef struct NameDef NameDef;
typedef struct BlockDef BlockDef;

inline int t_per24(int t0, int t1) ;
inline int t_per0(int t0, int t1) ;
inline int t_mid(int t0, int t1) ;
int main(int argc, char **argv) ;
void status(char *) ;
void dispCurrPer(int ) ;
void init_sin_table() ;
void debug(char *fmt, ...) ;
void * CAlloc(size_t len) ;
char * StrDup(char *str) ;
inline int calcNow() ;
//inline double noise() ;
void loop(int ) ;
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
void setupMidn();
int voicesEq(Voice *, Voice *);
void error(char *fmt, ...) ;

void 
usage() {
  error("SBaGen - Sequenced BinAural sound Generator, version " VERSION "\n"
	"Copyright (c) 1999 Jim Peters, released under the GNU GPL\n\n"
	"Usage: sbagen [options] seq-file\n"
	"       sbagen [options] -i tone-specs ...\n\n"
	"Options:  -v        Display the full interpreted sequence before playing\n"
	"          -q mult   Quick.  Run through quickly (real time x `mult') from the\n"
	"                     start time, rather than wait for real time to pass\n"
	"          -r rate   Manually select the output rate (default is 44100 Hz)\n"
	"          -b bits   Select the number bits for output (8 or 16, default 16)\n"
	"          -i        Immediate.  Take the remainder of the command line to be\n"
	"                     tone-specifications, and play them continuously"
	);
}

int const DEBUG_CHK_UTIME= 0;	// Check how much user time is being consumed
int const N_CH= 8;		// Number of channels

struct Voice {
  int typ;			// Voice type: 0 off, 1 binaural, 2 pink noise
  double amp;			// Amplitude level (0-32767)
  double carr;			// Carrier freq (for binaural)
  double res;			// Resonance freq (-ve or +ve) (for binaural)
};

struct Channel {
  Voice v;			// Current voice setting (updated from current period)
  int typ;			// Current type: 0 off, 1 binaural, 2 pink noise
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

Channel chan[N_CH];		// Current channel states
int now;			// Current time (milliseconds from midnight)
Period *per= 0;			// Current period
NameDef *nlist;			// Full list of name definitions

short *out_buf;			// Output buffer
int out_bsiz;			// Output buffer size (bytes)
int out_blen;			// Output buffer length (samples)
int out_buf_ms;			// Time to output a buffer-ful in ms
int out_fd;			// Output file descriptor
int out_rate= 44100;		// Sample rate
int out_mode= 0;		// Output mode: 0 short[2], 1 unsigned char[2]
FILE *in;			// Input sequence file
int in_lin;			// Current input line
char buf[4096];			// Buffer for current line
char buf_copy[4096];		// Used to keep unmodified copy of line
char *lin;			// Input line (uses buf[])
char *lin_copy;			// Copy of input line

#define NS_BIT 10
int ns_tbl[1<<NS_BIT];
int ns_off= 0;

int fast_tim0= -1;		// First time mentioned in the sequence file (for -q option)

int opt_v;
int opt_i;

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
  int mult= 0;		// Multiple if going fast
  int val;
  char dmy;

  argc--; argv++;
  init_sin_table();
  setupMidn();

  // Scan options
  while (argc > 0 && argv[0][0] == '-') {
    char *p= 1 + *argv++; argc--;
    while (*p) switch (*p++) {
     case 'v': opt_v= 1; break;
     case 'i': opt_i= 1; break;
     case 'q': 
       if (argc-- < 1 || 1 != sscanf(*argv++, "%d %c", &mult, &dmy)) usage();
       if (mult < 1) mult= 1;
       break;
     case 'r':
       if (argc-- < 1 || 1 != sscanf(*argv++, "%d %c", &out_rate, &dmy)) usage();
       break;
     case 'b':
       if (argc-- < 1 || 1 != sscanf(*argv++, "%d %c", &val, &dmy)) usage();
       if (val != 8 && val != 16) usage();
       out_mode= (val == 8) ? 1 : 0;
       break;
     default:
       usage(); break;
    }
  }

  if (argc < 1) usage();

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

  loop(mult);
}

//
//	Update a status line
//

void 
status(char *err) {
  int a;
  int nch= N_CH;

  while (nch > 1 && chan[nch-1].v.typ == 0) nch--;

  printf("\033[K  %02d:%02d:%02d", 
	 now % 86400000 / 3600000,
	 now % 3600000 / 60000,
	 now % 60000 / 1000);
  for (a= 0; a<nch; a++) switch (chan[a].v.typ) {
   default:
     printf(" -"); break;
   case 1:
     printf(" %.2f%+.2f/%.2f", chan[a].v.carr, chan[a].v.res, 
	    AMP_AD(chan[a].v.amp)); break;
   case 2:
     printf(" pink/%.2f", AMP_AD(chan[a].v.amp)); break;
  }
  if (err) printf(" %s", err);
  printf("\r");
  fflush(stdout);
  printf("\033[K");
}

void 				// Display current period details
dispCurrPer() {
  int a;
  Voice *v0, *v1;
  char *p0, *p1;
  int len0, len1;
  int nch= N_CH;


  p0= buf;
  p1= buf_copy;
  
  p0 += sprintf(p0, "* %02d:%02d:%02d",
		per->tim % 86400000 / 3600000,
		per->tim % 3600000 / 60000,
		per->tim % 60000 / 1000);
  p1 += sprintf(p1, "  %02d:%02d:%02d", 
		per->nxt->tim % 86400000 / 3600000,
		per->nxt->tim % 3600000 / 60000,
		per->nxt->tim % 60000 / 1000);
  v0= per->v0; v1= per->v1;
  while (nch > 1 && v0[nch-1].typ == 0) nch--;
  for (a= 0; a<nch; a++, v0++, v1++) switch (v0->typ) {
   default:
     p0 += sprintf(p0, " -");
     p1 += sprintf(p1, " -");
     break;
   case 1:
     p0 += len0= sprintf(p0, " %.2f%+.2f/%.2f", v0->carr, v0->res, AMP_AD(v0->amp));
     if (v0->carr != v1->carr || v0->res != v1->res || v0->amp != v1->amp)
       p1 += len1= sprintf(p1, " %.2f%+.2f/%.2f", v1->carr, v1->res, AMP_AD(v1->amp));
     else 
       p1 += len1= sprintf(p1, "  ::");
     while (len0 < len1) { *p0++= ' '; len0++; }
     while (len1 < len0) { *p1++= ' '; len1++; }
     break;
   case 2:
     p0 += len0= sprintf(p0, " pink/%.2f", AMP_AD(v0->amp));
     if (v0->amp != v1->amp)
       p1 += len1= sprintf(p1, " pink/%.2f", AMP_AD(v1->amp));
     else 
       p1 += len1= sprintf(p1, "  ::");
     while (len0 < len1) { *p0++= ' '; len0++; }
     while (len1 < len0) { *p1++= ' '; len1++; }
     break;
  }
  *p0= 0; *p1= 0;
  printf("%s\n%s\n", buf, buf_copy);
  fflush(stdout);
}

void 
init_sin_table() {
  int a;
  int *arr= (int*)CAlloc(ST_SIZ * sizeof(int));

  for (a= 0; a<ST_SIZ; a++)
    arr[a]= (int)(ST_AMP * sin((a * M_PI * 2) / ST_SIZ));

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
  vfprintf(stdout, fmt, ap);
  printf("\n");
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

void 
setupMidn() {
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

inline int 
userTime() {
  struct tms buf;
  times(&buf);
  return buf.tms_utime;
}

//
//	Simple random number generator.  Generates a repeating
//	sequence of 65536 odd numbers in the range -65535->65535.
//
//	Based on ZX Spectrum random number generator:
//	  seed= (seed+1) * 75 % 65537 - 1
//
//	Except we've changed to 3220 instead of 75
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

  return tot >> NS_ADJ;
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
loop(int fast_mult) {	
  int c, cnt;
  int err;		// Error to add to `now' until next cnt==0
  int fast= fast_mult != 0;
  int utime= 0;

  setup_device();
  cnt= 1 + 1999 / out_buf_ms;	// Update every 2 seconds or so
  now= fast ? fast_tim0 : calcNow();
  err= fast ? out_buf_ms * (fast_mult - 1) : 0;

  printf("\n");
  corrVal(0);		// Get into correct period
  dispCurrPer();	// Display
  status(0);
  
  while (1) {
    for (c= 0; c < cnt; c++) {
      corrVal(1);
      outChunk();
      now += out_buf_ms + err;
      if (now > H24) now -= H24;
      if (fast && (c&1)) status(0);
    }

    if (!fast) {
      char buf[32];
      err= calcNow() - now;		// Make sure that `now' keeps in sync with real
      if (abs(err) > H12) err= 0;	//  clock, but don't go changing it suddenly
      sprintf(buf, "(%d)", err); 
      err /= cnt;

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

    tot1= tot2= 0;

    ch= &chan[0];
    for (a= 0; a<N_CH; a++, ch++) switch (ch->typ) {
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
    }
    
    out_buf[off++]= tot1 >> 16;
    out_buf[off++]= tot2 >> 16;
  }

  // Rewrite buffer for 8-bit mode
  if (out_mode) {
    short *sp= out_buf;
    short *end= out_buf + out_blen;
    char *cp= (char*)out_buf;
    while (sp < end) *cp++= (*sp++ >> 8) + 128;
  }

  if (out_bsiz != write(out_fd, out_buf, out_bsiz)) 
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

  while ((now >= t0) ^ (now >= t1) ^ (t1 > t0)) {
    per= per->nxt;
    t0= per->tim;
    t1= per->nxt->tim;
    if (running) { dispCurrPer(); status(0); }
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
    }
  }
}       
      
//
//	Setup audio device
//

void 
setup_device(void) {
  int stereo, rate, fragsize, numfrags, enc;
  int afmt_req, afmt;
  audio_buf_info info;
  int test= 1;

  if (0 > (out_fd= open("/dev/dsp", O_WRONLY)))
    error("Can't open /dev/dsp, errno %d", errno);
  
  afmt= afmt_req= (out_mode ? AFMT_U8 : 
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
    error("Can't open device in %d-bit mode", out_mode ? 8 : 16);
  if (!stereo)
    error("Can't open device in stereo");
    
  out_rate= rate;

  if (-1 == ioctl(out_fd, SNDCTL_DSP_GETOSPACE, &info))
    error("Can't get audio buffer info, errno %d", errno);
  out_bsiz= info.fragsize;
  out_blen= out_mode ? out_bsiz : out_bsiz / 2;
  out_buf= (short*)CAlloc(out_blen * sizeof(short));
  out_buf_ms= (int)(1000.0 * 0.5 * out_blen / out_rate);

  printf("Outputting %d-bit audio at %d Hz with %d %d-sample fragments, %d ms per fragment\n",
	 out_mode ? 8 : 16, out_rate, info.fragstotal, out_blen/2, out_buf_ms);
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

  in= fopen(fnam, "r");
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
	if (per < 60000) {
	  int adj= (60000 - per) / 2, adj0, adj1;
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

	fo= pp->prv->fo;
	fi= qq->nxt->fi;

	// Special handling for -> slides:
	//   always slide, and stretch slide if possible
	if (pp->fi == -3) {
	  fo= fi= 2;		// Force slides for ->
	  for (a= 0; a<N_CH; a++) {
	    Voice *vp= &pp->v0[a];
	    Voice *vq= &qq->v1[a];
	    if (vp->typ == 0 && vq->typ != 0) {
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
	       vp->typ == 1 && 
	       (vp->carr != vq->carr || vp->res != vq->res))
	      ) {
	    vp->amp= vq->amp= 0;		// To silence
	    midpt= 1;				// Definately need the mid-point
	  }
	  else {				// Else smooth transition
	    vp->amp= vq->amp= (vp->amp + vq->amp) / 2;
	    if (vp->typ == 1) {
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
  if (opt_v) {
    Period *pp;
    if (per->nxt != per)
      while (per->prv->tim < per->tim) per= per->nxt;

    pp= per;
    do {
      dispCurrPer();
      per= per->nxt;
    } while (per != pp);
    printf("\n");
  }  
}

int 
voicesEq(Voice *v0, Voice *v1) {
  int a= N_CH;

  while (a-- > 0) {
    if (v0->typ != v1->typ) return 0;
    switch (v0->typ) {
     case 1:
       if (v0->amp != v1->amp ||
	   v0->carr != v1->carr ||
	   v0->res != v1->res)
	 return 0;
       break;
     case 2:
       if (v0->amp != v1->amp)
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
  for (ch= 0; ch < 8 && (p= getWord()); ch++) {
    char dmy;
    double amp, carr, res;

    // Interpret word into Voice nd->vv[ch]
    if (0 == strcmp(p, "-")) continue;
    if (1 == sscanf(p, "pink/%lf %c", &amp, &dmy)) {
      nd->vv[ch].typ= 2;
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
  int hh, mm, ss;
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
    
    if (3 > sscanf(p, "%2d:%2d:%2d%n", &hh, &mm, &ss, &nn)) {
      ss= 0;
      if (2 > sscanf(p, "%2d:%2d%n", &hh, &mm, &nn)) badTime(tim_p);
    }
    p += nn;

    if (hh < 0 || hh >= 24 ||
	mm < 0 || mm >= 60 ||
	ss < 0 || ss >= 60) badTime(tim_p);
    rtim= ((hh * 60 + mm) * 60 + ss) * 1000;

    if (tim == -1) 
      last_abs_time= tim= rtim;
    else 
      tim= (tim + rtim) % H24;
  }

  if (fast_tim0 < 0) fast_tim0= tim;
      
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

// END //
