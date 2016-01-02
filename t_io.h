/*
	Thread-safe (sub)standard io routines.

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/t_io.h,v 3.1 97/10/19 19:11:55 davidg Exp Locker: davidg $
*/

/* conditionally compile code to use separate t_select thread */
#ifdef __NeXT__
#define T_SELECT
#endif

#ifndef EOF
#define EOF (-1)
#endif

#define T_BUFSIZ	1024

struct t_file {
	int		fd;		/* the file */
	int		t_errno;	/* last error (if any) */
	int		count;		/* read: valid chars; write: room left */
	boolean_t 	writing; 	/* buffer state */
	boolean_t	urgent;		/* urgent data present on socket? */
	boolean_t	select;		/* should t_select check this fd? */
	boolean_t	telnet;		/* watch for telnet commands? */
	struct {
	    int		state;		/* telnet parsing state */
	    boolean_t	interrupt;	/* interrupt seen */
	} tel;
	int		want;		/* select: conditions to check for */
	int		can;		/* select: conditions that exist now */
#ifdef T_SELECT
	pthread_cond_t 	wait;		/* wait here for select */
#endif
	long		iotime;		/* time of last io */
	int		timeout;	/* idle timeout (seconds) */
	u_char		*ptr;		/* current position in buffer */
	u_char		buf[T_BUFSIZ];
	char		name[FILENAME_MAX]; /* debbuging: name */
};

typedef struct t_file t_file;

#define SEL_READ	001		/* bits for want/can */
#define SEL_WRITE	002
#define SEL_URG		004
#define SEL_CLOSE  	010

#define TS_DATA		0		/* values for tel.state */
#define TS_IAC		1
#define TS_WILL		2
#define TS_WONT		3
#define TS_DO		4
#define TS_DONT		5
#define TS_CR		6		/* seen cr */

/* The t_fdmap table maps os-level file descriptor numbers to t_file
   pointers.  This is very handy in debugging -- all routines that open
   file descriptors of any sort should use t_fopen or t_fdopen (even if they
   aren't going to do stream io on the file), so all active descriptors 
   are recorded in the table. */
   
t_file **t_fdmap;			/* map fd to t_file for t_select */

pthread_mutex_t		sel_lock;	/* protects sel_* and t_fdmap */

int maxfds;				/* result of getdtablesize() */
int max_used_fd;			/* biggest fd seen by t_fopen */
pthread_cond_t timeout_wait;		/* t_reaper waits here */

#define t_getc(f) ((f)->telnet ? t_telgetc(f) : (--(f)->count >= 0 ?\
	 (int)(*(unsigned char *)(f)->ptr++) : t_fillbuf(f)))
#define t_putc(f,c) (--(f)->count >= 0 ? *(f)->ptr++ = (c) : t_flshbuf(f, c))

void t_fdopen(t_file *f, int fd);
t_file *t_fopen(char *name, int acc, int mode);
int t_fclose(t_file *f);
int t_closefd(int fd);
char *t_gets(char *s, int len, t_file *f);
char *t_gets_eol(char *s, int len, t_file *f,boolean_t *eol);
int t_telgetc(t_file *f);
void t_puts(t_file *f, char *p);
void t_putnum(t_file *f, long l);
int t_fillbuf(t_file * f);
int t_fflush(t_file *f);
int t_flshbuf(t_file *f, int c);
int t_fread(t_file *f, char *buf, int len);
long t_fseek(t_file *f, long offset, long whence);
int t_fwrite(t_file *f, char *p, int len);
void t_ungetc(t_file *f, int c);
void t_sprintf();
void t_fprintf();
void t_ioinit();
boolean_t t_selwait(t_file *f, int bits);
void t_skipurg(t_file *f);
any_t t_reaper(any_t zot);
