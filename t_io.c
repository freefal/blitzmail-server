/*
	BlitzMail Server
	
	Copyright (c) 1994 by the Trustees of Dartmouth College; 
	see the file 'Copyright' in the distribution for conditions of use.

	Thread-safe replacement for basic stdio routines.
	
	$Header: /users/davidg/source/blitzserver/RCS/t_io.c,v 3.3 98/02/03 14:20:42 davidg Exp Locker: davidg $
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <varargs.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/dir.h>
#include <sys/time.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <arpa/telnet.h>
#include "t_io.h"
#include "t_err.h"
#include "misc.h"

#ifdef T_SELECT
pthread_addr_t t_select(pthread_addr_t zot);
#endif

void got_sigio();
void got_sigurg();
void doshutdown();

/* t_ioinit --

    Initialize io package (mutexs & conditions).  Start select thread and install
    signal handlers.

*/

void t_ioinit() {

    pthread_t	thread;
    
    pthread_mutex_init(&sel_lock, pthread_mutexattr_default);
    pthread_cond_init(&timeout_wait, pthread_condattr_default);
    
    t_fdmap = (t_file **) mallocf(getdtablesize() * sizeof(t_file *));
    
#ifdef T_SELECT
    /* start up thread to watch for input */
    if (pthread_create(&thread, generic_attr,
    	           (pthread_startroutine_t) t_select, (pthread_addr_t) 0) < 0) {
	t_perror("t_ioinit: t_select pthread_create");
	exit(1);
    }
    pthread_detach(&thread);
#endif

    /* start up thread to time out idle connections */
    if (pthread_create(&thread, generic_attr,
    	           (pthread_startroutine_t) t_reaper, (pthread_addr_t) 0) < 0) {
	t_perror("t_ioinit: t_reaper pthread_create");
	exit(1);
    }
    pthread_detach(&thread);
    
    /* find out how many open files are possible */
    maxfds = getdtablesize();
    
    max_used_fd = 0;
}

/* t_fdopen --
    
    Set up caller-supplied t_file to use an already-open file descriptor.
*/

void t_fdopen(t_file *f, int fd) {

    
    f->fd = fd;			/* copy underlying file number */
    f->t_errno = 0;		/* no errors so far */
    f->ptr = f->buf;		/* buffer is empty */
    f->count = 0;
    f->writing = FALSE;
    f->select = FALSE;
    f->urgent = FALSE;
    f->telnet = FALSE;
    f->tel.state = TS_DATA;
    f->tel.interrupt = FALSE;
    f->want = f->can = 0;
    f->timeout = 0;
    f->iotime = time(NULL);
#ifdef T_SELECT
    pthread_cond_init(&f->wait, pthread_condattr_default);
#endif
    t_sprintf(f->name, "#%d", fd);

    pthread_mutex_lock(&sel_lock);	/* sync w/ t_select */  
    t_fdmap[fd] = f;			/* fd->t_file map (for t_select) */
    if (fd > max_used_fd)
	max_used_fd = fd;		/* remember largest fd yet seen */
    pthread_mutex_unlock(&sel_lock);
   
}
/* t_fopen --

    Allocate and initialize file buffer structure, open file.
*/

t_file *t_fopen(char *name, int acc, int mode) {

    t_file	*f;			/* returned: file structure */
    int		fd;
    int		retries = 0;
    
    /* first, make sure file is available */
    do {			/* retry too many files errors */
	fd = open(name, acc, mode);
	if (fd < 0) {		/* open failed */
	    if (pthread_errno() != EMFILE)
		return NULL;	/* generic error - don't retry */
	    else {		/* too many files - delay & retry */
		t_errprint_s("Too many open files opening %s; retrying", name);
		if (++retries > 60) {
		    t_errprint("Too many retries; quitting!");
		    abort();	/* generate a dump */
		}
		sleep(15);	/* delay, hoping some files will free up */
	    }
	}
    } while (fd < 0);
	        
    f = (t_file *) mallocf(sizeof(t_file));

    f->fd = fd;			/* copy underlying file number */
    f->t_errno = 0;		/* no errors so far */
    f->ptr = f->buf;		/* buffer is empty */
    f->count = 0;
    f->writing = FALSE;
    f->select = FALSE;
    f->urgent = FALSE;
    f->telnet = FALSE;
    f->tel.state = TS_DATA;
    f->tel.interrupt = FALSE;
    f->timeout = 0;
    f->iotime = time(NULL);
    f->want = f->can = 0;
#ifdef T_SELECT
    pthread_cond_init(&f->wait, pthread_condattr_default);
#endif
    
    strcpy(f->name, name);	/* save name, for debugging */
    
    pthread_mutex_lock(&sel_lock);	/* sync w/ t_select */
    t_fdmap[fd] = f;			/* set fd -> t_file mapping, for t_select */
    if (fd > max_used_fd)
	max_used_fd = fd;		/* remember largest fd yet seen */
    pthread_mutex_unlock(&sel_lock);
    
    return f;
}

/* t_fclose --
	
    Write buffer (if necessary), close file, free storage.
*/

int t_fclose(t_file *f) {
	
    int		err = 0;		/* returned */
    
    /* flush buffer, if we were writing */
    if (t_fflush(f) < 0)
	err = EOF;
    
    if (f->fd >= 0) {			/* close underlying file */
	t_closefd(f->fd);
    }

#ifdef T_SELECT    
    pthread_cond_destroy(&f->wait);	/* clean up condition var */
#endif

    t_free((char *) f);
    
    return err;
}

/* t_closefd --
	
    Close file and remove it from t_fdmap.  If t_select uses the
    file, we must wait for it to say ok.
*/

int t_closefd(int fd) {
	
    t_file	*f;
        
    f = t_fdmap[fd];			/* locate file structure */
    
    pthread_mutex_lock(&sel_lock);	/* sync with t_select */
#ifdef T_SELECT
    if (f->select) {			/* is it looking at this fd? */
	f->want |= SEL_CLOSE;		/* we want to close the file */
	while (!(f->can & SEL_CLOSE)) { /* wait until we get permission */
	    pthread_cond_wait(&f->wait, &sel_lock);
	}
    }
#endif
    t_fdmap[fd] = NULL;			/* remove ourselves from t_select's view */
    pthread_mutex_unlock(&sel_lock);
            
    return close(fd);
}
/* t_fflush --
	
    If writing and buffer is non-empty, write the buffer.
    Note that it may take several writes to get the whole buffer
    out, if we've been flow-controlled.
    
    Set state to writing, set initialize ptr and count.
    
*/

int t_fflush(t_file *f) {

    int 	len;		/* length to write */
    int		err = 0;	/* returned */
    int		written;
    int l;
    
    /* if valid output in buffer, write it */
    if (f->writing && (len = f->ptr - f->buf) > 0 ) {
	/* keep writing until entire buffer sent */
	for (written = 0; written < len; ) {
	    if (!t_selwait(f, SEL_WRITE)) {
		err = EOF;	/* urgent data or pending disconnect */
		break;
	    }
	    
	    l = write(f->fd, f->buf+written, len-written);
	    if (l < 0) {
		f->t_errno = pthread_errno();
		err = EOF;
		break;
	    } else
		written += l;
	}
    }
    
    f->writing = TRUE; 		/* entering writing state */
    f->ptr = f->buf;		/* set up full buffer */
    f->count = sizeof(f->buf);
    
    return err;
}

/* t_fillbuf --

    Read buffer, set ptr and count.  Return (and advance past) first
    char of buffer; return EOF if nothing there.
*/

int t_fillbuf(t_file * f) {

    if (f->tel.interrupt)	/* interrupt pending; don't read more */
	return EOF;
		
    if (f->writing)		/* must flush before switching modes */
	return EOF;
 
    if (!t_selwait(f, SEL_READ))	
	return EOF;		/* urgent data or disconnect */
	 
    f->count = read(f->fd, f->buf, sizeof(f->buf));	/* read a buffer */
    
    if (f->count < 0) {
	f->t_errno = pthread_errno();
	return EOF;
    }
    
    f->ptr = f->buf;	/* start at beginning */
    
    if (--f->count < 0) /* empty? */
	return EOF;		
			    
    return *f->ptr++;	/* return first char */
}

/* t_flshbuf --
	
    Flush buffer and put a char in the new buffer (used by putc macro).
	
*/

int t_flshbuf(t_file *f, int c) {

    if (t_fflush(f) < 0) {		/* flush, enter writing state */
	f->t_errno = pthread_errno();
	return EOF;
    }
	    
    *f->ptr++ = c; f->count--; 		/* put the char */
    
    return c;
}

/* t_fread --

    Buffered binary read. Returns EOF and sets t_errno on error.
*/

int t_fread(t_file *f, char *buf, int len) {

    int		cc = 0;
    int		c;
    int		old_errno;
    
    old_errno = f->t_errno;		/* distinguish old errors from new */
    f->t_errno = 0;
    
    while(++cc <= len && (c = t_getc(f)) != EOF)
    	*buf++ = c;			/* don't compare char to EOF! */
	
    if (f->t_errno)			/* if trouble, return eof */
	 return EOF;
	 
    f->t_errno = old_errno;		/* errors are sticky */
    
    return --cc;			/* return length actually read */
    
}

/*t_fseek --

    Seek; empty buffer; set state to reading.
    SEEK_CUR isn't implemented correctly (it doesn't take the length
    remaining in the buffer into account.)
*/

long t_fseek(t_file *f, long offset, long whence) {

    long	pos;
    
    (void) t_fflush(f);			/* flush buffer if writing */
    
    pos = lseek(f->fd, offset, whence);	/* do the seek */
    
    f->ptr = f->buf;			/* set up to read */
    f->count = 0;
    f->writing = FALSE;
    
    return pos;
}

/*t_fwrite --

    Write chunk to file. Returns length written.
*/
int t_fwrite(t_file *f, char *p,  int len) {

    int		chunklen;	/* length this time */
    int		written = 0;	/* returned: running total */
    int		err;		/* return from t_fflush */
    
    while (len > 0) {
    	chunklen = (len > f->count) ? f->count : len; /* as much as fits */
	bcopy(p, f->ptr, chunklen); /* move into buffer */
	f->ptr += chunklen; f->count -= chunklen; /* update buffer state */
	p += chunklen; len -= chunklen; /* and amount to go */
	if (len > 0) {			/* too much for one buffer? */
	    if ((err = t_fflush(f)) < 0) /* flush & do another */
		return err;
	}
	written += chunklen;
    }
    return written;		/* return amount written */
}

/* t_gets --
    
    Get chars until EOL or buffer fills.  The EOL is replaced with a null.
    (i.e., the way fgets _should_ work...)
    
    The EOL sequence may be either \r, \n, or \r\n; we have to deal with
    all 3 at one time or another (mac files, unix files, network connections).
    
    Return buffer pointer, or null on eof.
*/

char *t_gets(char *s, int len, t_file *f) {
	
    int 	c = 0;
    char	*ss = s;
    
    while(--len > 0) {			/* don't overrun buffer */
	c = t_getc(f);
	if (c == '\r') {		/* bare CR or CRLF */
	    c = t_getc(f);		/* peek at next char */		
	    if (c != '\n')		/* eat LF after CR */
		t_ungetc(f, c);		/* oops, put it back */
	    break;			/* stop at CR or CRLF */
	}
	if (c == '\n' || c == EOF)
		break;			/* stop at newline or end of file */
	*ss++ = c;				
    }
    
    *ss = '\0';				/* terminate the string */
    
    if (c == EOF && !*s)		/* indicate eof, if needed */
	return NULL;
	
    return s;				/* return unmodified string ptr */
}

/*^L t_gets_eol --

    Same as t_gets, but additional parameter tells caller whether
    or not an end-of-line sequence (as opposed to a full buffer)
    ended the read.

    Return buffer pointer, or null on eof.
*/

char *t_gets_eol(char *s, int len, t_file *f, boolean_t *eol) {

    int         c = 0;
    char        *ss = s;

    *eol = FALSE;			/* no CR/LF yet */

    while(--len > 0) {                  /* don't overrun buffer */
        c = t_getc(f);
        if (c == '\r') {                /* bare CR or CRLF */
	    *eol = TRUE;
            c = t_getc(f);              /* peek at next char */
            if (c != '\n')              /* eat LF after CR */
                t_ungetc(f, c);         /* oops, put it back */
            break;                      /* stop at CR or CRLF */
        }
        if (c == '\n' || c == EOF)
                break;                  /* stop at newline or end of file */
        *ss++ = c;
    }

    *ss = '\0';                         /* terminate the string */

    if (c == EOF && !*s)                /* indicate eof, if needed */
        return NULL;

    if (c == '\n')			/* newline terminated input */
	*eol = TRUE;
    return s;                           /* return unmodified string ptr */
}
/* t_ungetc --

    Put back a char just read. 
*/

void t_ungetc(t_file *f, int c) {

    if (f->writing)
	   return;		/* not reading */
	   
    *--(f)->ptr = (char) c;
    ++f->count;
    f->tel.state = TS_DATA;	/* don't screw up telnet state machine */
}
/* t_puts --
	
    Copy string to file.
*/

void t_puts(t_file *f, char *p) {

    while(*p)
	t_putc(f, *p++);
}

/* t_putnum --

    Copy (decimal) number to file.
    
*/

void t_putnum(t_file *f, long l) {

    char	buf[NUMLEN];
    
    numtostr(l, buf);
    t_puts(f, buf);
}

/* t_putuns --

    Copy unsigned number to file.
    
*/

void t_putuns(t_file *f, u_long u) {

    char	buf[NUMLEN];
    
    unstostr(u, buf);
    t_puts(f, buf);
}

/* t_sprintf --

    This is a limited version of the full sprintf:  no field widths,
    only %s, and %d formats.
*/

void t_sprintf(s, fmt, va_alist) 
char *s;
char *fmt;
va_dcl
{

    va_list	ap;			/* for varargs */
    char	c;
    boolean_t	esc = FALSE;		/* last char a '%'? */
    boolean_t	wantlong = FALSE;	/* %ld? */
    long	l;
    u_long	u;
					   
    va_start(ap);			/* setup varargs */
    
    while (c = *fmt++) {		/* (sic) */
	if (c == '%' && !esc)
	    esc = TRUE;			/* %% is literal */
	else if (!esc)
	    *s++ = c;			/* copy literal char */
	else {				/* handle escape */
	    switch(c) {
	    	case 'l':
		    wantlong = TRUE;	/* next int will be long */
		    break;		/* esc still set */
		case 'd':		/* int or long? */
		    l = (wantlong ? va_arg(ap, long) : va_arg(ap, int));
		    s = numtostr(l, s);
		    esc = wantlong = FALSE;
		    break;
		case 'u':		/* unsigned int or long? */
		    u = (wantlong ? va_arg(ap, u_long) : va_arg(ap, u_int));
		    s = unstostr(u, s);
		    esc = wantlong = FALSE;
		    break;
		case 's':
		    strcpy(s, va_arg(ap, char *));
		    s += strlen(s);
		    esc = wantlong = FALSE;
		    break;
		default:
		    *s++ = c;
		    esc = wantlong = FALSE;
	    }
	}
    }
    
    *s = 0;
    
    va_end(ap);
}

/* t_fprintf --

    Should really use a common subroutine with t_sprintf, but that's
    difficult with varargs...
*/

void t_fprintf(f, fmt, va_alist) 
t_file	*f;
char 	*fmt;
va_dcl
{

    va_list	ap;			/* for varargs */
    char	c;
    boolean_t	esc = FALSE;		/* last char a '%'? */
    boolean_t	wantlong = FALSE;	/* %ld? */
    long	l;
    u_long	u;
					   
    va_start(ap);			/* setup varargs */
    
    while (c = *fmt++) {		/* (sic) */
	if (c == '%' && !esc)
	    esc = TRUE;			/* %% is literal */
	else if (!esc)
	    t_putc(f, c);		/* copy literal char */
	else {				/* handle escape */
	    switch(c) {
	    	case 'l':
		    wantlong = TRUE;	/* next int will be long */
		    break;		/* esc still set */
		case 'd':		/* int or long? */
		    l = (wantlong ? va_arg(ap, long) : va_arg(ap, int));
		    t_putnum(f, l);
		    esc = wantlong = FALSE;
		    break;
		case 'u':		/* unsigned int or long? */
		    u = (wantlong ? va_arg(ap, u_long) : va_arg(ap, u_int));
		    t_putuns(f, u);
		    esc = wantlong = FALSE;
		    break;
		case 's':
		    t_puts(f, va_arg(ap, char *));
		    esc = wantlong = FALSE;
		    break;
		case 'c':
		    c = va_arg(ap, int); /* char (passed as int) */
		    t_putc(f, c);
		    esc = wantlong = FALSE;
		    break;
		default:
		    t_putc(f, c);
		    esc = wantlong = FALSE;
	    }
	}
    }
    
    va_end(ap);
}
/* t_telgetc --

    Return next input char, after filtering out telnet commands.  f->tel.state
    records what state we're in: TS_DATA is for parsing normal chars, other
    states are for the various twisty little passages of telnet negotations.
    
    We essentially don't care about any options; we ignore all DONTs and WONTs,
    and reject all DOs and WILLs.  This lets us avoid remembering the setting
    of every single option, while keeping out of option loops.
    
    Any responses are written here directly (bypassing the buffer, since a
    single buffer is used for input and output).
    
    In the event of an interrupt or break command, we want to return EOF, but
    only after parsing any other telnet sequences in the buffer (this is
    accomplished by having t_fillbuf check the interrupt bit).
    
    As always with Telnet, handling end-of-line sequences is a bitch.  Because
    these routines use a single buffer (output destroys input), the eol handling
    must(unfortunately) be buffer-boundary sensitive.  Depending on what kind of
    client we're talking to, a line of input may be terminated by a CR-LF (or CR-NUL,
    we don't care which), or the client may send only the CR, beginning the _next_
    buffer of input with the following NUL.  
    
    Things to watch out for are:  
      - after seeing a CR at the end of the buffer, don't hang trying to 
        read the NUL before considering the input line to be complete.
      - if the buffer ends with a CRLF, don't read just the CR, enter
        TS_CR, and destroy the LF by generating output
	
    The solution to all of this is:  when a CR is seen, (a) map it to a \n,
    and (b) if the buffer is empty enter TS_CR, else consume the LF or NUL.
    
    NOTE: at present, telnet option handling is NOT compatible with watching
    for urgent data with t_selwait: for a given file you can set either f->select
    or f->telnet, but not both!
*/

int t_telgetc(t_file *f) {

    int		c;
    char	buf[3];			/* for option negotiations */
    
    for (;;) {				/* until we get a data char */
	/* pick up next char, filling buffer if necessary */
	if (--f->count >= 0)
	    c = (int)(*(unsigned char *)f->ptr++);
	else 
	    c = t_fillbuf(f);
	
	if (c == EOF)			/* connection gone? */
	    return c;
	    
	switch(f->tel.state) {		/* enter state machine */
	    case TS_CR:			/* saw CR; eat LF or NUL */
		f->tel.state = TS_DATA;	/* back to normal now */
		if (c == 0 || c == '\n')
		    break;
		/* FALL THROUGH */
	    case TS_DATA:		/* ordinary data */
		if (c == IAC)		/* escape char? */
		    f->tel.state = TS_IAC;
		else if (c == DM)
		    ;			/* data mark - ignore */
		else {
		    if (c == '\r') {	/* CR-LF or CR-NUL? */
		    	c = '\n';	/* don't hang trying to read next char */
			if (f->count == 0) { /* CR ends buffer */
			    f->tel.state = TS_CR; /* eat first char of next buffer */
			} else if (f->ptr[1] == '\n' || f->ptr[1] == 0) {
			    ++f->ptr;	/* skip LF or NUL now */
			    --f->count;
			} 
		    } 
		    /* work around bug in TCP/KSP gateway that causes it to
		       send CR<telnet commands>NUL by eating stray nulls. */
		    if (c == 0)
			break;				    
		    return c;		/* return ordinary char */
		}
		break;
	    case TS_IAC:		/* last char was IAC */
	    	f->tel.state = TS_DATA;	/* assume next byte is plaintext */
		switch(c) {
		    case WILL:
		    	f->tel.state = TS_WILL;
			break;
		    case WONT:
		    	f->tel.state = TS_WONT;
			break;
		    case DO:
		    	f->tel.state = TS_DO;
			break;
		    case DONT:
		    	f->tel.state = TS_DONT;
			break;
		    case IP:		/* interrupt! */
		    case BREAK:
			f->tel.interrupt = TRUE;
			break;
		    case IAC:
			return c;	/* IAC-IAC */
		    default:
		    	break;		/* ignore others */
		}
		break;
	    case TS_DONT:
	    case TS_WONT:
		f->tel.state = TS_DATA;	/* ignore DONTs and WONTs */
		break;
	    case TS_DO:
		f->tel.state = TS_DATA;
		if (c == TELOPT_ECHO)	/* he's confirming echo drive */
		    break;
		if (c == TELOPT_SGA)	/* .. */
		    break;
	    	buf[0] = IAC; buf[1] = WONT; buf[2] = c;
		if (write(f->fd, buf, sizeof(buf)) < 0) {
		    f->t_errno = pthread_errno();
		    return EOF;
		}
		break;
	    case TS_WILL:
		f->tel.state = TS_DATA;
	    	buf[0] = IAC; buf[1] = DONT; buf[2] = c;
		if (write(f->fd, buf, sizeof(buf)) < 0) {
		    f->t_errno = pthread_errno();
		    return EOF;
		}
		break;
	}
    }
}


/* t_skipurg --

    Skip urgent (out-of-band) data present on socket.  Note that the SO_OOBINLINE
    option should be set, so we can read the out-of-band data with an ordinary
    read.  Use SIOCATMARK to tell if we have reached the out-of-band data, then
    read once more to get the urgent data (and clear the urgent indication from
    the socket.)
*/

void t_skipurg(t_file *f) {

    int		mark;		/* at mark? */	
    int		len;

    pthread_mutex_lock(&sel_lock);    
    f->urgent = FALSE; 		/* clear the indication in t_file */
    f->can &= ~SEL_URG;
    pthread_mutex_unlock(&sel_lock);
        
    t_fseek(f, 0, SEEK_END);	/* enter reading state */
    
    /* go until urgent data read... */
    while (!(f->want & SEL_CLOSE)) {  /* ...or connection is closed */
	if (ioctl(f->fd, SIOCATMARK, &mark) < 0) {
	    t_perror("t_skipurg: SIOCATMARK");
	    f->t_errno = pthread_errno();
	    return;
	}
	    
	/* read & discard */
	if ((len = read(f->fd, f->buf, sizeof(f->buf))) < 0) {
	    f->t_errno = pthread_errno();
	    return;
	}
	
	/* done if we just read the urgent data, or EOF */
	if (mark || len == 0)	
	    break;
    }
}
#ifdef T_SELECT
/* t_select -- [NeXT version]

    Thread to do a select() to check for pending urgent data on any of our
    files.  Two sets of status bits are associated with the file:  the thread
    that owns the file sets bits in the "want" set, and this thread sets bits
    in the "can" set.  While waiting for the "can" bit it wants, the file owner
    blocks on the "wait"  condition.  In addition to the read, write, and except
    bits used by the select() itself, there's also the "close" bit.  When the 
    file owner wants to close the file, it sets the close bit in "want" and 
    until we set the one in "can".  This ensures that the file isn't closed 
    while we're trying to do a select() on it.  Other threads (not the owner)
    that want to cause a disconnect can also do want |= close (asking the owner
    to please close.)
    
    A relatively short timeout value is set on the select(); it's a compromise
    between reasonable responsiveness and cpu-friendliness. The reason we don't
    want to just block forever is that files need to be added to the descriptor
    set from time to time. On systems that allow large numbers of open files, checking
    every value up to maxfds can consume a significant amount of time (assuming we're
    not actually using nearly that many), so t_fopen (and t_fdopen) remember the largest
    fd that they've seen yet -- no need to check any higher than that.
    
    On OSF/1 systems, it's more CPU-friendly to have an independent select() for
    each file, so this code is used only on the NeXT. (NeXTs get a timeout table
    overflow if you have a select() for each thread; they must use this version).
*/

pthread_addr_t t_select(pthread_addr_t zot) {

    fd_set 		readfds, writefds, exceptfds;
    int			fd;		/* current fd */
    t_file		*f;		/* corresponding file struct */
    struct timeval	poll = { 0, 10000 }; /* for select - 10ms */
    int			topfd = 0;	/* highest fd to check */

    for (;;) {				/* and never cease */
		   
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_ZERO(&exceptfds);

	pthread_mutex_lock(&sel_lock);		/* get access to globals */
    	
	/* use global map to locate all t_file structs; construct
	   bitmaps indicating what we're interested in */
	for (fd = 0; fd <= max_used_fd; ++fd)  {
	    if ((f = t_fdmap[fd]) && f->select) {
		topfd = fd;
	    	if (server_shutdown)	/* shutdown in progress */
		    f->want |= SEL_CLOSE; /* force disconnect */
		if (f->want & SEL_CLOSE) {
		    f->can |= SEL_CLOSE; /* give permission to close */
		    pthread_cond_signal(&f->wait);
		} else {
		    if (f->want & SEL_READ)
			FD_SET(fd, &readfds);
		    if (f->want & SEL_WRITE)
			FD_SET(fd, &writefds);
		    if (f->want & SEL_URG)
			FD_SET(fd, &exceptfds);
		}
	    }
	}  
	
	pthread_mutex_unlock(&sel_lock);
		    
	/* check all those files (time out after poll interval) */
	if (select(topfd+1, &readfds, &writefds, &exceptfds, &poll) < 0) {
	    t_perror("t_select: select");
	    continue;
	}     

	pthread_mutex_lock(&sel_lock);			/* get globals again */

	/* check results and set "can" bits.  As we do so, clear the
	   corresponding "want" bits so we don't spin if the client
	   thread is distracted */
	   
	for (fd = 0; fd <= topfd; ++fd)  {
	    if ((f = t_fdmap[fd]) && f->select) {
		if (FD_ISSET(fd, &readfds)) {
		    f->can |= SEL_READ;
		    f->want &= ~SEL_READ;
		    pthread_cond_signal(&f->wait);
		} 		
		if (FD_ISSET(fd, &writefds)) {
		    f->can |= SEL_WRITE;
		    f->want &= ~SEL_WRITE;
		    pthread_cond_signal(&f->wait);		    
		}		
		if (FD_ISSET(fd, &exceptfds)) {
		    f->can |= SEL_URG;
		    f->want &= ~SEL_URG;
		    pthread_cond_signal(&f->wait);
		} 
	    }
	}	
	pthread_mutex_unlock(&sel_lock);
    }

}

/* t_selwait -- [NeXT version]

    Wake the t_select thread, and wait until it indicates that our file
    is ready to [read,write] (or urgent data arrives or a disconnect is
    pending.)
    
    Returns TRUE if it's now ok to read/write.  Sets f->urgent (and
    returns FALSE) if urgent data is present.
    
    Note: FIONBIO (non-blocking io) option should be enabled on the file;
    select() may indicate that it's safe to write, but if you try to write
    (e.g.) 1024 bytes when the window will only allow 512, the write()
    will block unless non-blocking io is enabled.
*/

boolean_t t_selwait(t_file *f, int bits) {
        
    boolean_t 		ok;		/* returned: ok to read/write */
    
    if (!f->select)			
	return TRUE;			/* no waiting needed */
	
    pthread_mutex_lock(&sel_lock);	/* get t_select globals */
    
    bits |= SEL_URG;			/* SEL_READ or SEL_WRITE plus SEL_URG */
    f->want |= bits;		
    
    /* now wait until something happens */
    while (!(f->can & bits) && !(f->want & SEL_CLOSE)) {
	pthread_cond_wait(&f->wait, &sel_lock); /* wait for t_select to respond */
    }
      
    if (f->can & SEL_URG) {		/* urgent data present? */
	f->urgent = TRUE;
	ok = FALSE;			/* break seen */
    } else if (f->want & SEL_CLOSE) {
	ok = FALSE;			/* disconnect pending */
    } else {
	ok = TRUE;			/* ok to read/write */
	f->iotime = time(NULL);		/* so reset timer */
    }  

    f->want &= ~bits;			/* no need to keep polling this file */
    f->can &= ~bits;			/* must do another select next time */
    pthread_mutex_unlock(&sel_lock);
    
    return ok;
 
}
#else
/* t_selwait -- [non-NeXT version]

    Do a select() on a single file; wait until the file is ready 
    to [read,write] (or urgent data arrives or a disconnect is pending.)
    
    The select times out every few seconds, at which point we check if some
    other thread wants us to close the file (or if the server is shutting down).
    
    Returns TRUE if it's now ok to read/write.  Sets f->urgent (and
    returns FALSE) if urgent data is present.
*/

boolean_t t_selwait(t_file *f, int bits) {

    fd_set 		readfds, writefds, exceptfds;
    struct timeval	poll = { 5, 0 }; /* for select - 5sec */
    boolean_t 		ok;		/* returned: ok to read/write */
    int                 nf;             /* number of files found by select */

    if (!f->select)			
	return TRUE;			/* no waiting needed */

    bits |= SEL_URG;			/* SEL_READ or SEL_WRITE plus SEL_URG */
    f->want |= bits;		

    while (!(f->can & bits) && !(f->want & SEL_CLOSE)) {
		   
	FD_ZERO(&readfds);
	FD_ZERO(&writefds);
	FD_ZERO(&exceptfds);
	
	if (f->want & SEL_READ)	
	    FD_SET(f->fd, &readfds);
	if (f->want & SEL_WRITE)
	    FD_SET(f->fd, &writefds);
	if (f->want & SEL_URG)
	    FD_SET(f->fd, &exceptfds);
			    
	/* check this one file (time out after poll interval) */
	nf = select(f->fd+1, &readfds, &writefds, &exceptfds, &poll); 
	if (nf < 1) {			/* select timeout or error */
		if (nf < 0)
			t_perror("t_selwait: select");
		continue;		/* redo the select */
	}     
	if (server_shutdown) 		/* closing down? */
	    f->want |= SEL_CLOSE;
	    
	/* check results and set "can" bits. */
	   
	if (FD_ISSET(f->fd, &readfds)) {
	    f->can |= SEL_READ;
	    f->want &= ~SEL_READ;
	} 		
	if (FD_ISSET(f->fd, &writefds)) {
	    f->can |= SEL_WRITE;
	    f->want &= ~SEL_WRITE;
	}		
	if (FD_ISSET(f->fd, &exceptfds)) {
	    f->can |= SEL_URG;
	    f->want &= ~SEL_URG;
	} 
    }

    /* select found something; what was it? */
    
    if (f->can & SEL_URG) {		/* urgent data present? */
	f->urgent = TRUE;
	ok = FALSE;			/* break seen */
    } else if (f->want & SEL_CLOSE) {
	ok = FALSE;			/* disconnect pending */
    } else {
	ok = TRUE;			/* ok to read/write */
	f->iotime = time(NULL);		/* so reset timer */
    }  

    f->want &= ~bits;			/* no need to keep polling this file */
    f->can &= ~bits;			/* must do another select next time */
    
    return ok;

}
#endif
/* reaper --

    Thread to time out idle connections (to keep us from running out of file
    descriptors.)  mbox_writer wakes us up periodically.  If the number of
    active users exceeds u_worry, we are awakened immediately.
    
    All files governed by t_select are eligible for timeouts if their
    "timeout" field is non-zero.  Higher-level code is responsible for
    adjusting the timeout as desired (e.g., to implement the policy of
    shorter timeouts when there are more users) 
*/

any_t t_reaper(any_t zot) {

    int		fd;			/* current file descriptor */
    t_file	*f;			/* corresponding struct */
    long	now;			/* current time */

    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
    
    for (;;) {
	pthread_mutex_lock(&sel_lock);	/* sync w/ t_select */
	pthread_cond_wait(&timeout_wait, &sel_lock); /* wait until awakened */


	/* check each file with a timeout value set */
	now = time(NULL);
	for (fd = 0; fd <= max_used_fd; ++fd)  {
	    if ((f = t_fdmap[fd]) && f->select && f->timeout) {
		if (now - f->iotime > f->timeout) {
		    f->want |= SEL_CLOSE; /* ask connection thread to close */
#ifdef T_SELECT
		    pthread_cond_signal(&f->wait); /* nudge it */
#endif
		}
	    }	
	}
	pthread_mutex_unlock(&sel_lock); 
    }
}
