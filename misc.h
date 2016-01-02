/*  Mach BlitzMail Server -- misc definitions
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/misc.h,v 3.6 98/10/21 16:08:48 davidg Exp Locker: davidg $
*/

#ifndef _MISC_H
#define _MISC_H

#include "sem.h"

/* typedefs for places where we require a particular number of bits */
#ifndef NUMLEN
#if defined(NeXT) || defined(__alpha)
typedef u_int	u_bit32;
typedef int	bit32;
typedef u_short u_bit16;
typedef short	bit16;
typedef char	bit8;
typedef u_char	u_bit8;
#define NUMLEN 32			/* long enough for decimal ascii(long) */
#endif
#endif

struct machost {			/* machost header (endian sensitive...) */
    u_char	vers;			/* file version */
    char 	type[3];		/* and identifier */
    u_bit32	space;			/* disk space used */
    char	finder_info[16];	/* finder info */
    u_char	fname_len;		/* length of filename */
};					/* followed by filename, datalen & data fork */
typedef struct machost machost;
#define MACHOST_VERS 0xFE		/* version we know & love */
#define MACHOST_HDR_LEN 25		/* byte length (avoid padding) */

struct forkinfo {			/* locate data/resource forks in encl */
    long	datastart;
    long	datalen;
    long 	rsrcstart;
    long	rsrclen;
};
typedef struct forkinfo forkinfo;

/*    ----      Access Permissions ---     */

#define	FILE_ACC	0640
#define DIR_ACC		0750

#define DIRBUF_SIZE	8192 	/* buffer size for reading directories */

extern char server_vers[];

pthread_mutex_t global_lock;	/* lock protecting misc globals */
pthread_mutex_t malloc_lock;	/* lock protecting malloc */
pthread_mutex_t dir_lock;	/* lock protecting readdir etc. */
pthread_attr_t  generic_attr;   /* thread attributes */
#define DEFAULT_STACKSIZE 32768

struct {			/* counters of allocated things */
	long	total;		/* all blocks currently allocated */
	long	summbuck;	/* summary buckets */
	long	mbox;		/* mbox structs */
	long	obufs;		/* obufs */
	long	login_blocks;	/* recent_login blocks */
	long	preftab;	/* pref hash table */
	long	prefentry;	/* individual pref entry */
	long	mltab;		/* mailing list hash table */
	long	mlentry;	/* individual mailing list entry */
} malloc_stats;

#ifdef KERBEROS
struct sem krb_sem;
#endif

volatile boolean_t server_shutdown; /* server currently closing down? */

char up_date[9], up_time[9];	/* date/time we started up */

void misc_init();
u_long add_days(int days);
u_long add_months(int months);
void date_time(char datestr[9], char timestr[9]);
boolean_t parse_date(char *p, char datestr[9], char timestr[9], u_long *out);
boolean_t parse_expdate(char *p, u_long *out);
u_long mactime(void);
u_long date_to_mactime(int years, int months, int days);
void get_date(char *t);
void *mallocf(size_t byteSize);
void *reallocf(void *old, size_t byteSize);
void escname(char *in, char *out);
void unescname(char *in, char *out);
void t_free(void *p);
boolean_t check_temp_space(long l);
boolean_t do_rm(char *dirname);
boolean_t do_cp(char *old, char *new);
boolean_t isblankstr(char *in);
bit32 getnetlong (char *in);
u_short getnetshort (char *in);
char *putnetlong (char *out, bit32 in);
char *putnetshort (char *out, u_short in);
char *strtonum(char *in, long *out);
char *strtouns(char *in, u_long *out);
char *numtostr(long in, char *out);
char *unstostr(u_long in, char *out);
char *strncpy_and_quote(char *to, char *from, int max);
char *pstrcpy(char *to, char *from);
char *strqcpy (char *to, char *from);
void strtrimcpy(char *out, char *in);
void ucase(char *to, char *from);
void lcase(char *to, char *from);
char *unquote(char *out, char *in);
char *strcasematch(char *str, char *substr);
void telnet_strip(char *out, char *in);
void disassociate();
char *strwcpy (char *to, char *from);
void setup_signals();
#endif
