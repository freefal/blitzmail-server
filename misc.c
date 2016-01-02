/*  BlitzMail Server -- misc

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/misc.c,v 3.6 98/10/21 16:08:43 davidg Exp Locker: davidg $

*/
#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/dir.h>
#include <sys/stat.h>
#include <arpa/telnet.h>
#include "t_io.h"
#include "t_err.h"
#include "mbox.h"
#include "misc.h"
#include "config.h"
#include "mess.h"

static char *month_name[12] = { "Jan", "Feb", "Mar", "Apr", 
				"May", "Jun", "Jul", "Aug", 
				"Sep", "Oct", "Nov", "Dec" };

static char *weekday_name[7] = { "Sun", "Mon", "Tue", "Wed", 
				"Thu", "Fri", "Sat" };

boolean_t do_cp1(char *oldname, char *newname);

/* misc_init --

    Initialize global mutexes. Should be called first (before any malloc or anything).
*/

void misc_init() {

    pthread_mutex_init(&global_lock, pthread_mutexattr_default); /* initialize global locks */
    pthread_mutex_init(&malloc_lock, pthread_mutexattr_default);
    pthread_mutex_init(&clock_lock, pthread_mutexattr_default);
    pthread_mutex_init(&dir_lock, pthread_mutexattr_default);
    pthread_mutex_init(&syslog_lock, pthread_mutexattr_default);
    
#ifdef KERBEROS
    sem_init(&krb_sem, "krb_sem");
#endif

    /* create default attribute object */
    if (pthread_attr_create(&generic_attr) < 0) {
        perror("pthread_attr_create");	/* note: not t_perror; not inited yet */
        generic_attr = pthread_attr_default;    /* or just exit?? */
    }
    pthread_attr_setstacksize(&generic_attr, DEFAULT_STACKSIZE);


}
/* add_days --

    Return mactime of a date some days in the future.
*/

u_long add_days(int days) {

    struct tm		*clock;
    u_long		result;		/* time in mac units */
    time_t		secs;
    
    pthread_mutex_lock(&clock_lock);	/* localtime uses static data */
    					
    secs = time((time_t *) NULL);
    clock = localtime(&secs);		/* get current time */
        
    clock->tm_mon++;			/* convert month into range 1-12 */
    
    result = date_to_mactime(clock->tm_year, clock->tm_mon, clock->tm_mday);
    
    result += days*24*60*60;		/* advance n days */
    
    pthread_mutex_unlock(&clock_lock);

    return result;
}
/* add_months --

    Return mactime of a date some months in the future.
*/

u_long add_months(int months) {

    struct tm		*clock;
    u_long		result;		/* time in mac units */
    time_t		secs;
    
    pthread_mutex_lock(&clock_lock);		/* localtime uses static data */
    					
    secs = time((time_t *) NULL);
    clock = localtime(&secs);		/* get current time */
        
    clock->tm_mon++;			/* convert month into range 1-12 */
    clock->tm_mon += months;		/* go forward this far */
    
    while(clock->tm_mon > 12) {		/* and advance year as needed */
	clock->tm_year++;
	clock->tm_mon -= 12;
    }
    
    result = date_to_mactime(clock->tm_year, clock->tm_mon, clock->tm_mday);
    
    pthread_mutex_unlock(&clock_lock);

    return result;
}
/* mactime --

    Return current (local) time in seconds since mac epoch (01 Jan 1904).
*/

u_long mactime() {


    struct tm		*clock;
    u_long		result;		/* time in mac units */
    time_t		secs;
    
    pthread_mutex_lock(&clock_lock);		/* localtime uses static data */
    					
    secs = time((time_t *) NULL);
    clock = localtime(&secs);		/* get current time */
        
    clock->tm_mon++;			/* convert month into range 1-12 */
    
    /* get mactime of start of this day */
    result = date_to_mactime(clock->tm_year, clock->tm_mon, clock->tm_mday);
    
    /* add hours/minutes/seconds to get current mactime */
    result += clock->tm_hour * 60 * 60;
    result += clock->tm_min * 60;
    result += clock->tm_sec;
    
    pthread_mutex_unlock(&clock_lock);

    return result;

}

/* date_to_mactime --

   Convert a given month/day/year to seconds since the Mac epoch.
   
   Note that the "years" parameter is years since 1900 (like the tm_year
   value returned by localtime())
*/

u_long date_to_mactime(int years, int months, int days) {

    long		result;


    static int daytab[12] = {
              0,       		/* days in year before Jan 1 */
              31,       	/* days in year before Feb 1 */
              31+28,    	/* days in year before Mar 1 */
              31+28+31, 	/* etc */
              31+28+31+30,
              31+28+31+30+31,
              31+28+31+30+31+30,
              31+28+31+30+31+30+31,
              31+28+31+30+31+30+31+31,
              31+28+31+30+31+30+31+31+30,
              31+28+31+30+31+30+31+31+30+31,
              31+28+31+30+31+30+31+31+30+31+30};


    result = (days-1) + years * 365; 	/* convert years to days; jan 1 is day 0 */
    result += (years-1)/4; 		/* add one day per leap-year _before_ this year */
    					/* (1905 had one leap year before it, etc.) */
    result += daytab[months-1]; 	/* get days in year before first of this month */
    if (months > 2 && (years % 4) == 0)	/* if after February & leap year */
	result++; 			/* account for Feb 29 of a leap year */
  
    result -= 4*365; 			/* start at 1 Jan 1904 instead of 1 Jan 1900 */
    result *= 24*60*60; 		/* convert to seconds */
    
    return(result);

}

/* date_time --

    Generate date and time in MM/DD/YY and HH:MM:SS formats.  Local time is used.
*/

void date_time(char datestr[9], char timestr[9]) {

    struct tm	*clock;
    time_t	secs;
    
    pthread_mutex_lock(&clock_lock);		/* localtime uses static data */
    
    secs = time((time_t *) NULL);
    clock = localtime(&secs);		/* get current time */
    
    clock->tm_mon++;			/* convert month to range 1-12 */

    datestr[0] = '0' + clock->tm_mon / 10; /* MM */
    datestr[1] = '0' + clock->tm_mon % 10;
    datestr[2] = '/';
    datestr[3] = '0' + clock->tm_mday / 10; /* DD */
    datestr[4] = '0' + clock->tm_mday % 10;
    datestr[5] = '/';
    datestr[6] = '0' + (clock->tm_year / 10) % 10; /* YY */
    datestr[7] = '0' + clock->tm_year % 10;
    datestr[8] = 0;
    
    timestr[0] = '0' + clock->tm_hour / 10; /* HH */
    timestr[1] = '0' + clock->tm_hour % 10;
    timestr[2] = ':';
    timestr[3] = '0' + clock->tm_min / 10; /* MM */
    timestr[4] = '0' + clock->tm_min % 10;
    timestr[5] = ':';
    timestr[6] = '0' + clock->tm_sec / 10; /* SS */
    timestr[7] = '0' + clock->tm_sec % 10;
    timestr[8] = 0;

    pthread_mutex_unlock(&clock_lock);    
    
    return;
}
/* escname --
    
    Names may include characters that are either illegal (8-th bit set)
    or inconvenient ('*',' ','/','.') in filenames).  Convert these to \nnn (octal).
*/

void escname(char *in, char *out) {
    
    u_char 	c;		/* unsigned, since we're checking top bit */
    
    while((c = *in++) != 0) {
	if (c == '*' || c == ' ' || c == '.' 
	    || c == '/' || c == '\\' || c >= 0200) {
		*out++ = '\\';
		*out++ = '0' + ((c >> 6) & 3);
		*out++ = '0' + ((c >> 3) & 7);
		*out++ = '0' + (c & 7);
	    } else 
		*out++ = c;
    }
    
    *out++ = 0;
}
/* unescname --
    
    Convert \nnn representation back to real life.
*/

void unescname(char *in, char *out) {
    
    char 	c;
    
    while((c = *in++) != 0) {
	if (c == '\\') {
	    c = (*in++ - '0') << 6;
	    c |= (*in++ - '0') <<3;
	    c |= (*in++ - '0');
	}
	*out++ = c;
    }
    
    *out++ = 0;
}

/* get_date --

    Get date & time in RFC822 format:  8 Oct 88 12:30:10 EDT
*/

void get_date(char *t) {

    struct tm	*clock;
    time_t	secs;
    
    pthread_mutex_lock(&clock_lock);		/* localtime uses static data */
    
    secs = time((time_t *) NULL);
    clock = localtime(&secs);		/* get current time */
    
    strftime(t, 32, "%d %b %y %H:%M:%S %Z", clock);
    
    pthread_mutex_unlock(&clock_lock);    
    
    return;

}
/* parse_date --
	
    Convert RFC822 date to MM/DD/YY and HH:MM:SS. (and mactime)
    
    [Sun, ]8 Oct 88 12:30:10 EDT
*/

boolean_t parse_date(char *p, char datestr[9], char timestr[9], u_long *out) {

    long 	month;
    long	day;
    long	year;
    long	hour;
    long	min;
    long 	sec;
    int		i;
    
    /* skip optional weekday name */
    for (i = 0; i < 7; ++i) {
	if (strncasecmp(p, weekday_name[i], 3) == 0) {
	    p += 3;
	    if (*p == ',')
		++p;
	    if (*p == ' ')
		++p;
	    break;
	}
    }
    
    p = strtonum(p, &day);
    if (*p == ' ' || *p == '-')		/* allow 3-Apr-91 */
	++p;
    else
	return FALSE;
	
    for (month = 0; month < 12; ++month) {
	if (strncasecmp(p, month_name[month], 3) == 0)
	    break;
    }
    
    if (++month > 12)			/* Jan == 1 */
	return FALSE;			/* no match */
    p += 3;				/* skip month name */
    if (*p == ' ' || *p == '-')		
	++p;
    else
	return FALSE;

    p = strtonum(p, &year);
    if (*p++ != ' ')
	return FALSE;
    if (year > 1900)
	year -= 1900;
	
    if (index(p, ':') == NULL) {	/* allow ugly HHMM format */
        if (!isdigit(*p)) return FALSE;
	hour = (*p++ - '0') * 10;
        if (!isdigit(*p)) return FALSE;
	hour += (*p++ - '0');
        if (!isdigit(*p)) return FALSE;
	min = (*p++ - '0') * 10;
        if (!isdigit(*p)) return FALSE;
	min += (*p++ - '0');
	sec = 0;	
    } else {
	p = strtonum(p, &hour);
	if (*p++ != ':')
	    return FALSE;
	
	p = strtonum(p, &min);
	if (*p++ == ':')			/* seconds are optional */
	    p = strtonum(p, &sec);
	else
	    sec = 0;
    }
    
    datestr[0] = '0' + month / 10; /* MM */
    datestr[1] = '0' + month % 10;
    datestr[2] = '/';
    datestr[3] = '0' + day / 10; /* DD */
    datestr[4] = '0' + day % 10;
    datestr[5] = '/';
    datestr[6] = '0' + (year / 10) % 10; /* YY */
    datestr[7] = '0' + year % 10;
    datestr[8] = 0;
    
    timestr[0] = '0' + hour / 10; /* HH */
    timestr[1] = '0' + hour % 10;
    timestr[2] = ':';
    timestr[3] = '0' + min / 10; /* MM */
    timestr[4] = '0' + min % 10;
    timestr[5] = ':';
    timestr[6] = '0' + sec / 10; /* SS */
    timestr[7] = '0' + sec % 10;
    timestr[8] = 0;

    if (out)			/* caller wants mactime result? */
    	*out = date_to_mactime(year, month, day); /* no sense parsing twice... */
    	
    return TRUE;
    
}
/* parse_expdate --
	
    Parse DND "expires" field (8-OCT-88), return mactime.
    
*/

boolean_t parse_expdate(char *p, u_long *out) {

    long 	month;
    long	day;
    long	year;
        
    p = strtonum(p, &day);
    if (*p == ' ' || *p == '-')		/* allow 3-Apr-91 */
	++p;
    else
	return FALSE;
	
    for (month = 0; month < 12; ++month) {
	if (strncasecmp(p, month_name[month], 3) == 0)
	    break;
    }
    
    if (++month > 12)			/* Jan == 1 */
	return FALSE;			/* no match */
    p += 3;				/* skip month name */
    if (*p == ' ' || *p == '-')		
	++p;
    else
	return FALSE;

    p = strtonum(p, &year);
    
    if (*p && !isspace(*p))
	return FALSE;
    if (year > 1900)
	year -= 1900;
	
    *out = date_to_mactime(year, month, day);	/* return result as mactime */

    return TRUE;
    
}
/* t_free --
	thread-safe free
*/


void t_free(void *p) {

    pthread_mutex_lock(&malloc_lock);
    free(p);
    pthread_mutex_unlock(&malloc_lock);    

    pthread_mutex_lock(&global_lock);
    --malloc_stats.total;
    pthread_mutex_unlock(&global_lock);

}

/* mallocf --
	Thread-safe malloc-or-fail
*/


void *mallocf(size_t byteSize) {

    void 		*result;
    
    pthread_mutex_lock(&malloc_lock);	/* malloc probably isn't reentrant */
    
    result = malloc(byteSize);
    
    if (result == NULL) {
	pthread_mutex_unlock(&malloc_lock);
	t_perror("malloc failed");
	abortsig();		
    }
    
    pthread_mutex_unlock(&malloc_lock);
    
    pthread_mutex_lock(&global_lock);
    ++malloc_stats.total;
    pthread_mutex_unlock(&global_lock);

    return result;
}

/* reallocf --
	realloc-or-fail
*/


void *reallocf(void *old, size_t byteSize) {

    void 		*result;

    pthread_mutex_lock(&malloc_lock);	/* probably isn't reentrant */
    
    result = realloc(old, byteSize);
    
    if (result == NULL) {
	pthread_mutex_unlock(&malloc_lock);
	t_perror("realloc failed");
	abortsig();		
    }

    pthread_mutex_unlock(&malloc_lock);	
    
    return result;
}
/* check_temp_space --

    Check to see if temp space has room for message of a given length.
    
*/
boolean_t check_temp_space(long l) {

    struct statfs	buf;
    long		avail;	/* free disk space */

    if (STATFS("/tmp", &buf, sizeof(buf)) < 0) {
	t_perror("check_temp_space: cannot statfs /tmp");
    } else {
	/* normalize block size to make sure we don't run
	    out of bits for very large disks */
	avail = buf.f_bavail * (buf.f_bsize / 128);
	if (l / 128 > avail * 0.4) {	/* don't let 1 mess take more than 40% */
	    return FALSE;
	}
    }
    return TRUE;
}

/* do_cp --

    Copy (recursively) a directory.
*/

boolean_t do_cp(char *old, char *new) {

    DIR			*dirf;			/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    char		*newname;		/* new file/dir name */
    char		*oldname;		/* old file/dir name */
    boolean_t		ok = TRUE;		/* returned: error seen? */
    struct stat		statbuf;		/* stat() info */
    
    /* create destination directory, if not yet there */
    if (mkdir(new, DIR_ACC) < 0) {
	if (pthread_errno() != EEXIST) {
	    t_perror1("do_cp: cannot create ", new);
	    return FALSE;
	}
    }

    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    dirf = opendir(old);
    pthread_mutex_unlock(&dir_lock);
    
    if (dirf == NULL) {
	t_perror1("do_cp: cannot open ", old);
	return FALSE;
    }
    
    oldname = mallocf(FILENAME_MAX+1);
    newname = mallocf(FILENAME_MAX+1);
    
    /* read directory, giving up on first error */
    while (ok && (dirp = readdir(dirf)) != NULL) {
	if (strcmp(dirp->d_name, ".") == 0 || strcmp(dirp->d_name, "..") == 0)
	    continue;				/* skip '.' and '..'  */

	/* generate full pathnames (old & new) */
	t_sprintf(oldname, "%s/%s", old, dirp->d_name);
	t_sprintf(newname, "%s/%s", new, dirp->d_name);
	    
	/* is this entry file or dir? */
	if (stat(oldname, &statbuf) < 0) {
	    t_perror1("do_cp: cannot stat ", oldname);
	    ok = FALSE;
	    break;
	}
	if ((statbuf.st_mode & S_IFMT) == S_IFDIR) { /* dir - recurse */
	    if (!do_cp(oldname, newname))
		ok = FALSE;
	} else {				/* file - copy */
	    if (!do_cp1(oldname, newname))
		ok = FALSE;
	}
    }
    
    closedir(dirf);
    t_free(oldname);		
    t_free(newname);
    
    return ok;
}

/* copy a file */

boolean_t do_cp1(char *oldname, char *newname) {

    t_file	*oldf = NULL, *newf = NULL;
    boolean_t	ok = TRUE;
    char 	*buf = NULL;
    int		readlen, writelen;
    int		buflen = 8192; 		/* use a healthy-sized buffer */
    
    buf = mallocf(buflen);
    
    /* open source & destination files */
    newf = t_fopen(newname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC);
    if (newf == NULL) {
	t_perror1("do_cp1: cannot open/create ", newname);
	ok = FALSE;
    } else {
	oldf = t_fopen(oldname, O_RDONLY, 0);
	if (oldf == NULL) {
	    t_perror1("do_cp1: cannot open ", oldname);
	    ok = FALSE;
	}
    }
    
    while(ok) {
	readlen = read(oldf->fd, buf, buflen);	/* read a buffer full */
	if (readlen < 0) {
	    t_perror1("do_cp1: error reading ", oldname);
	    ok = FALSE;
	    break;
	}
	writelen = write(newf->fd, buf, readlen);
	if (writelen < readlen)	{	/* write error? */
	    t_perror1("do_cp1: error writing ", newname);
	    ok = FALSE;
	    break;
	}
	if (readlen < buflen) 
	    break;			/* end of file */
    }
    
    if (buf) t_free(buf);
    if (oldf) t_fclose(oldf);
    if (newf) t_fclose(newf);
    
    return ok;

}
/* do_rm --

    Remove (recursively) a directory.
*/

boolean_t do_rm(char *dirname) {

    DIR			*dirf;			/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    char		*subname;		/* subdirectory pathname */
    boolean_t		ok = TRUE;		/* returned: error seen? */
    struct stat		statbuf;		/* stat() info */

    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    dirf = opendir(dirname);
    pthread_mutex_unlock(&dir_lock);
    
    if (dirf == NULL) {
	t_perror1("do_rm: cannot open ", dirname);
	return FALSE;
    }

    subname = mallocf(FILENAME_MAX+1);
    
    while ((dirp = readdir(dirf)) != NULL) {	/* read entire directory */
	if (strcmp(dirp->d_name, ".") == 0 || strcmp(dirp->d_name, "..") == 0)
	    continue;				/* skip '.' and '..'  */
	    
	t_sprintf(subname, "%s/%s", dirname, dirp->d_name);
	if (stat(subname, &statbuf) < 0) {
	    t_perror1("do_rm: cannot stat ", subname);
	    ok = FALSE;
	} else {
	    if ((statbuf.st_mode & S_IFMT) == S_IFDIR) { /* dir - recurse */
		if (!do_rm(subname))
		    ok = FALSE;
	    } else {				/* file - unlink */
		if (unlink(subname) < 0) {
		    t_perror1("do_rm: cannot unlink ", subname);
		    ok = FALSE;
		}
	    }
	}
    }
    
    closedir(dirf);
    if (rmdir(dirname) < 0) {
	t_perror1("do_rm: cannot rmdir ", dirname);
	ok = FALSE;
    }
    t_free(subname);
    
    return ok;
}
/* getnetlong --
    getnetshort --

    Copy 16 or 32-bit value from network buffer to host var.  This operation is
    mostly trivial, but for two portability considerations:
    
      - The value in the buffer may not be properly aligned.
      - We may use a different byte ordering from the network.
      
*/

bit32 getnetlong (char *in) {

    bit32	out;
    
    bcopy(in, (char *) &out, 4);/* copy & align 4 bytes */
    return ntohl(out);		/* convert to host byte ordering */
}

u_short getnetshort (char *in) {

    u_short	out;
    
    bcopy(in, (char *) &out, 2);/* copy & align 2 bytes */
    return ntohs(out);		/* convert to host byte ordering */
    
}

/* putnetlong --
    putnetshort --

    The inverse of "getnetlong|short"; copies value to buffer in network byte order.
    
    Returns pointer past end of output value.
*/

char *putnetlong (char *out, bit32 in) {

    bit32	l;
    
    l = htonl(in);		/* convert to network byte order */
    bcopy((char *) &l, out, 4);	/* copy out */
    
    return out + 4;		/* return pointer past end */
}

char *putnetshort (char *out, u_short in) {

    u_short	s;
    
    s = htons(in);		/* convert to network byte order */
    bcopy((char *) &s, out, 2);	/* copy out*/
    
    return out + 2;		/* return pointer past end */
}
/* isblankstr --

    Is string entirely whitespace?
    
*/

boolean_t isblankstr(char *in) {

    for(; *in; ++in) {
	if (!(isascii(*in) && isspace(*in)))
	    return FALSE;
    }
    return TRUE;
}
/* numtostr --

    Convert longint to decimal ascii.  Returns pointer to end of output.
    
*/

char *numtostr(long in, char *out) {

    char	stack[NUMLEN], *stackp;	/* holds reversed result */

    if (in == 0) {
	strcpy(out, "0");
	++out;
    } else {
	if (in < 0) {
	    *out++ = '-';
	    in = -in;
	}
	stackp = stack; 
	*stackp++ = 0;			/* sentinel */	
	while (in > 0) {		/* generate digits right-to-left */
	    *stackp++ = '0' + in % 10;
	    in /= 10;
	}
	while (*out++ = *--stackp)
		;			/* copy & reverse */
	--out;				/* point at the terminator */
    }
    return out;				/* return pointer to end */
}

/* unstostr --

    Convert unsigned longint to decimal ascii.  Returns pointer to end of output.
    
*/

char *unstostr(u_long in, char *out) {

    char	stack[NUMLEN], *stackp;	/* holds reversed result */

    if (in == 0) {
	strcpy(out, "0");
	++out;
    } else {
	stackp = stack; 
	*stackp++ = 0;			/* sentinel */	
	while (in > 0) {		/* generate digits right-to-left */
	    *stackp++ = '0' + in % 10;
	    in /= 10;
	}
	while (*out++ = *--stackp)
		;			/* copy & reverse */
	--out;				/* point at the terminator */
    }
    return out;				/* return pointer to end */
}

/* strtonum --

    Convert ascii to long int.  Returns pointer to 1st char after the number.
*/

char *strtonum(char *in, long *out) {

    boolean_t	minus = FALSE;
     
    while(isascii(*in) && isspace(*in))	/* skip leading whitespace */
	in++;
	
    if (*in == '+')			/* check for optional sign */
	in++;
    else if (*in == '-') {
    	in++;
	minus = TRUE;
    }
    
    *out = 0;
    while(isdigit(*in)) 		/* now loop until nondigit */
	*out = *out * 10 + (*in++ - '0');
	
    if (minus)
	*out = -(*out);			/* factor the sign back in */

    return in;				/* return pointer past # */
}

/* strtouns --

    Convert ascii to unsigned long.  Returns pointer to 1st char after the number.
*/

char *strtouns(char *in, u_long *out) {
     
    while(isascii(*in) && isspace(*in))	/* skip leading whitespace */
	in++;
	
    *out = 0;
    while(isdigit(*in)) 		/* loop until nondigit */
	*out = *out * 10 + (*in++ - '0');
	    
    return in;				/* return pointer past # */
}
/* pstrcpy --

    Copy "pascal string" (first char == length) to ordinary string.
    Returns pointer to first char after end of source.
    
*/

char *pstrcpy(char *to, char *from) {

    int		l;		/* length */
    
    l = *((u_char *) from);	/* get length byte (watch out for sign!) */
    ++from;			/* and skip it */
    
    for ( ; l > 0; --l)		/* copy the rest */
	*to++ = *from++;
	
    *to++ = 0;			/* terminate */
    
    return from;
}

/* strncpy_and_quote --

    Copy a string, adding quoting.  Quotes within the string are doubled.
    No more than 'max' chars are to be copied.  Opening and closing quotes
    and a terminating null are always produced, so the smallest useful
    value for max is 3.
    
    Returns pointer to destination terminator.
*/

char *strncpy_and_quote(char *to, char *from, int max) {

    max -= 3;				/* allow for quotes & null */
    
    *to++ = '"';
    if (from) {				/* allow null from string */
	while(*from && --max > 0) {	/* stop when not room for doubled quote */
	    if ((*to++ = *from++) == '"')
		*to++ = '"';		/* double quotes */
	}
    }
    *to++ = '"';
    *to = 0;
    
    return to;
}
/* strwcpy --

    Copy word (space-terminated).  
    
    Returns pointer to first char after word.
*/

char *strwcpy (char *to, char *from) {

    while (*from && !(isascii(*from) && isspace(*from)))
	*to++ = *from++;
    *to++ = 0;
    
    return from;
}
/* strqcpy --

    Copy quoted string up through and including the closing quote.  
    
    Returns pointer to first char after closing quote in source.
*/

char *strqcpy (char *to, char *from) {

    if (*from != '"') {			/* isn't quoted; just copy */
	strcpy(to, from);
	return from + strlen(from);
    }
    
    *to++ = *from++;			/* copy initial quote */
    
    for (;;) {				/* until non-doubled quote is copied */
	if (*from == 0)	{		/* yikes! no closing quote */
	    *to++ = '"';		/* intuit one */
	    break;
	}
   	if ((*to++ = *from++) == '"') {	
	    if (*from == '"')		/* quote; is it the end? */
	    	*to++ = *from++;	/* no; copy 2nd half of pair */
	    else
	    	break;			/* yes; *from == first char after quoted str */
	}
    }
    
    *to++ = 0;
    
    return from;
}

/* strtrimcpy --

    Copy, trimming spaces from either end
*/

void strtrimcpy(char *out, char *in) {

    char 	*oldout = out;
    int		spacecount = 0;
    char	c;
    
    while(c = *in++) {			/* sic */
    	if (c == ' ')
	    spacecount++;		/* save up spaces */
	else {	
	    if (out == oldout) 		/* eat leading spaces */
		spacecount = 0;
	    for( ; spacecount; --spacecount)
		    *out++ = ' ';
	    *out++ = c;
	}
    }
    *out++ = 0;
}

/* ucase -

   Copy & uppercase.
   
*/

void ucase(char *to, char *from) {

    while(*to++ = ((isascii(*from) && islower(*from)) ? toupper(*from) : *from))
	from++;
    
}

/* lcase -

   Copy & lowercase.
   
*/

void lcase(char *to, char *from) {

    while(*to++ = ((isascii(*from) && isupper(*from)) ? tolower(*from) : *from))
	from++;
    
}
/* unquote --

    Copy quoted string, removing quotes.  Return pointer past
    final quote of source.
*/

char *unquote(char *out, char *in) {
   
    char	c;
    
    if (*in != '"') {		/* if not quoted, do nothing */
    	strcpy(out, in);
	return in + strlen(in);
    }

    ++in;				/* skip initial quote */
    
    for (;;) {				/* until non-doubled quote is copied */
	c = *in++;
   	if (c == '"') {			/* quote; is it the end? */
	    if (*in == '"')
		*out++ = *in++;		/* no; copy 2nd half of pair */
	    else
		break;			/* yes; *in == char after quoted str */   
	} else if (!c) {		/* missing final quote */
	    --in;			/* oh well; deal */
	    break;
	} else
	    *out++ = c;			/* copy ordinary char */
    }
    
    *out++ = 0;
    
    return in;    
}

/* strcasematch -

   Case-blind string search.
   
*/

char *strcasematch(char *str, char *substr) {

    int		len;
    
    len = strlen(substr);
    
    while(strlen(str) >= len) {
	if (strncasecmp(str, substr, len) == 0)
	    return str;			/* match: return location */
	++str;				/* advance & try again */
    }
    
    return NULL;			/* not found */

}

/* telnet_strip --

    Copy string, stripping telnet commands.
*/

void telnet_strip(char *out, char *in) {

    u_char	c;
    boolean_t 	esc = FALSE;
    
    while (c = *in++) {		/* sic */
	if (esc) {		/* processing command? */
	    switch(c) {
		case IAC:
		    *out++ = c;
		    break;	/* escaped IAC char */
		case DO:
		case DONT:
		case WILL:
		case WONT:
		    ++in;	/* options are 2 bytes */
		    break;
		default:
		    break;
	    }
	    esc = FALSE;	/* done with command */
	} else {
	    if (c == IAC)	/* begin command */
		esc = TRUE;
	    else
		*out++ = c;	/* copy ordinary char */
	}
    }

    *out = 0;
}

/* disassociate --

    Disassociate us from the controlling terminal.  Close standard
    files and fork (continuing in the child.)  Do a TIOCNOTTY on
    /dev/tty to remove our association with the controlling terminal.
    
    Must be called before any threads are started (since we fork).
*/

void disassociate() {

    int		i;
    
    for (i = 0; i < 3; ++i)
	close(i);			/* close standard files */
	
    if (fork())				/* start a child (run in background) */
	exit(0);			/* parent exits, child continues */
	
    open("/", O_RDONLY);		/* put something on stdin */
    dup2(0,1);				/* and stdout */
    
    open("/dev/console", O_WRONLY);	/* stderr goes to console */
    
    if ((i = open("/dev/tty", O_RDWR)) >= 0) {
	ioctl(i, TIOCNOTTY, 0);		/* ditch the controlling terminal */
	close(i);
    }
    
}
/* setup_signals --

    Set up signal handling environment for a new thread. Asynchronous signals
    like SIGQUIT and SIGTERM are handled globally, but things like SIGPIPE
    and SIGBUS need to have each thread specify how to handle them (under
    OSF/1; NeXT does these globally too).
    
    The two main things we need to do here are: ignore SIGPIPE (we never want
    to terminate just because a connection goes down), handle core-dump-causing
    signals like SIGSEGV by calling abortsig() to generate a useful dump &
    traceback. (This last shouldn't be necessary, but default behavior under
    OSF/1 produces a useless core file).
*/

void setup_signals() {

    signal(SIGPIPE, SIG_IGN);	/* don't terminate if connections lost */
    signal(SIGBUS, abortsig);	/* generate traceback & core dump */
    signal(SIGSEGV, abortsig);	/* .. */

}
