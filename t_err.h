/* thread-friendly error library

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

$Header: /users/davidg/source/blitzserver/RCS/t_err.h,v 3.1 97/10/19 19:11:29 davidg Exp Locker: davidg $
 */

#include "sem.h"

pthread_mutex_t	syslog_lock;		/* protects syslog */
struct sem	log_sem;		/* protects log file */

/* gethostby* are protected by herrno_sem (must serialize access to the static
   state inside those routines.) A separate lock (inet_ntoa_lock) is used for
   inet_ntoa, so a slow DNS lookup doesn't slow down basic IP address formatting. */
   
struct sem 	herrno_sem;		/* protects h_errno (hostname lookup) */
pthread_mutex_t	inet_ntoa_lock;		/* protects inet_ntoa */

void t_errinit(char *ident, int facility);
void t_syslog(int priority, char *mess);
void t_errprint(char *mess);
void t_errprint_l(char *mess, long l);
void t_errprint_ll(char *mess, long l1, long l2);
void t_errprint_s(char *mess, char *s);
void t_errprint_ss(char *mess, char *s1, char *s2);
void t_perror(char *mess);
void t_perror1(char *mess1,char *mess2);
void t_mach_error(char *mess, kern_return_t err);
void log_it (char *s);
void setup_syslog();
