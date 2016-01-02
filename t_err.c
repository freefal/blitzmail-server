/*  BlitzMail Server -- error library

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/t_err.c,v 3.1 97/10/19 19:11:15 davidg Exp Locker: davidg $
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "t_err.h"
#include "t_io.h"
#include "mbox.h"
#include "misc.h"
#include "config.h"

static char syslog_ident[128];
static int syslog_facility;
/* t_errinit --

    Initialize error package.  Set up mutex protecting the 
    gethostbyname routine. (syslog_lock initialized in misc_init)
*/

void t_errinit (char *ident, int facility) {

    pthread_mutex_init(&inet_ntoa_lock, pthread_mutexattr_default);
    sem_init(&herrno_sem, "herrno_sem");
    sem_init(&log_sem, "log_sem");
    
    /* record ident & facility globally */
    strncpy(syslog_ident, ident, sizeof(syslog_ident)-1);
    syslog_ident[sizeof(syslog_ident)-1] = 0;
    syslog_facility = facility;
    setup_syslog();
}

/* setup_syslog --

    Beginning with DU4.0, each thread needs to call "openlog" individually,
    since each has its own syslog context. (This probably means that syslog
    is thread-safe there, but we keep the locking for compatability with
    older OS's.)
*/
    
void setup_syslog() {
    pthread_mutex_lock(&syslog_lock);
    openlog(syslog_ident, 0, syslog_facility);	/* set default id string & facility code */
    pthread_mutex_unlock(&syslog_lock);
}

/* t_perror --

    Thread-safe perror.  Uses pthread_errno, protects syslog w/ mutex.
*/

void t_perror(char *mess) {
    
    pthread_mutex_lock(&syslog_lock);
    syslog(LOG_ERR, "%s: %s\n", mess, strerror(pthread_errno()));
    pthread_mutex_unlock(&syslog_lock);
}

/* t_perror --

    Thread-safe perror.  w/ additional string parameter.
*/

void t_perror1(char *mess1,char *mess2) {
    
    pthread_mutex_lock(&syslog_lock);
    syslog(LOG_ERR, "%s%s: %s\n", mess1, mess2, strerror(pthread_errno()));
    pthread_mutex_unlock(&syslog_lock);
}


/* t_mach_error --

    Thread-safe mach_error.
*/

#ifndef __POWER_AIX__ /* This routine doesn't seem to get called, but mach_error_string isn't defined */
void t_mach_error(char *mess, kern_return_t err) {
        
    pthread_mutex_lock(&syslog_lock);
    syslog(LOG_ERR, "%s: %s\n", mess, mach_error_string(err));
    pthread_mutex_unlock(&syslog_lock);

}
#endif
/* t_syslog --
    
    Thread-safe version of syslog.
*/

void t_syslog(int priority, char *mess) {
    
    pthread_mutex_lock(&syslog_lock);
    syslog(priority, "%s", mess);
    pthread_mutex_unlock(&syslog_lock);
}

/* t_errprint --
    
    Simplified syslog (always log at LOG_ERR).
*/

void t_errprint(char *mess) {
    
    pthread_mutex_lock(&syslog_lock);
    syslog(LOG_ERR, "%s", mess);
    pthread_mutex_unlock(&syslog_lock);
}

/* t_errprint_l --
    
    Print error with long arg.
*/

void t_errprint_l(char *mess, long l) {

    pthread_mutex_lock(&syslog_lock);
    syslog(LOG_ERR, mess, l);
    pthread_mutex_unlock(&syslog_lock);
}

/* t_errprint_ll --
    
    Print error with 2 long args.
*/

void t_errprint_ll(char *mess, long l1, long l2) {

    pthread_mutex_lock(&syslog_lock);
    syslog(LOG_ERR, mess, l1, l2);
    pthread_mutex_unlock(&syslog_lock);
}

/* t_errprint_s --
    
    Print error with string arg.
*/

void t_errprint_s(char *mess, char *s) {
    
    pthread_mutex_lock(&syslog_lock);
    syslog(LOG_ERR, mess, s);
    pthread_mutex_unlock(&syslog_lock);
}

/* t_errprint_ss --
    
    Print error with 2 string args.
*/

void t_errprint_ss(char *mess, char *s1, char *s2) {
    
    pthread_mutex_lock(&syslog_lock);
    syslog(LOG_ERR, mess, s1, s2);
    pthread_mutex_unlock(&syslog_lock);
}

/* log_it --

    Add timestamp, and write line to log file.  
    
    So far, we're managing this file ourself (instead of using syslog) because
    of the heavy volume.  This may not really be necessary; timing tests should
    be done.
    
    Because opening the log file is very expensive (compared to writing), the
    file is kept open constantly.  To ensure that we switch over to the new
    day's logfile, reopen the file when the date changes, or we're less than
    10 minutes after midnight.
*/

void log_it (char *s) {

    char 		datestr[9]; 
    char 		timestr[9];
    static t_file 	*log_file = NULL;	/* open log file */
    static char		lastdate[9] = "xxxxxxxx";		
       
    if (!f_logfile || strlen(f_logfile) == 0)
	return;				/* logging disabled */

    date_time(datestr, timestr);
	
    sem_seize(&log_sem);	
    /* new day is starting; close & reopen */
    if (strcmp(lastdate, datestr) != 0 || strncmp(timestr, "00:0",4) == 0) {
	if (log_file) {
	    t_fclose(log_file);
	    log_file = NULL;
	}
    }
    strcpy(lastdate, datestr);
    if (log_file == NULL) {
	if ((log_file = t_fopen(f_logfile, O_APPEND | O_CREAT | O_WRONLY, 0600)) == NULL) 
	    t_perror("logfile open");
    }	
    if (log_file) {
	t_fprintf(log_file, "%s %s %s\n", datestr, timestr, s);
	t_fflush(log_file);
    }
    
    sem_release(&log_sem);	
    
}
