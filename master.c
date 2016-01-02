/*  BlitzMail Server -- master server

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/master.c,v 3.0 97/01/27 16:56:12 davidg Exp $
    
    Read config file, start up the programs named in it, restart them if they
    terminate.

    Because of our use of signals, we don't use mutex's and conditions.
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <sysexits.h>
#include <syslog.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include "t_io.h"
#include "misc.h"

#define STACK_LIMIT	(256*1024)		/* stack size limit to use */
#define PID_FILE_DFT	"/etc/blitzmaster.pid"	/* default pid file name */
struct rlimit rlim = { RLIM_INFINITY, RLIM_INFINITY };

boolean_t shutting_down = FALSE;

extern char *optarg;
extern int optind;

struct job{
    struct job		*next;
    int			pid;
    char 		**argv;
};

struct job		*jobhead = NULL;
struct job		*jobtail = NULL;

char	*pid_fname = NULL;

void startchild(struct job *j);
struct job *parse_line(char *line);
void setcoredumplimit();
void setstacklimit(long limit);
void die();

int main(int argc, char **argv) {

    t_file	*f;		/* file listing binaries to run */
    char	line[1024];
    struct	job *j;
    int		pid;
    t_file	*pidfile;
    long	stack_limit = STACK_LIMIT; /* limit to use */
    boolean_t	err = FALSE;
    boolean_t	debug = FALSE;
    int		c;
    union wait  wstat;

    if (!debug)
	disassociate();			/* detach from controlling tty (*before* any threads start!) */
	
    misc_init();
    t_ioinit();
    
    openlog("master", 0, LOG_LOCAL1);
    while ((c = getopt(argc, argv, "ds:f:")) != EOF) {
	switch(c) {
	    case 's':
		if (sscanf(optarg, "%ld", &stack_limit) == 1)
		    stack_limit *= 1024;	/* units of 1k */
		else
		    ++err;
		break;
	    case 'f':				/* name of pid file */
	    	pid_fname = optarg;
		break;
	    case 'd':
	    	debug = TRUE;
		break;
	    default:
		++err;
		break;
	}
    }
    if (err || optind != argc-1) {
	syslog(LOG_ERR, "Usage: %s [-d] [-s <stack limit>] [-f <pid file>] <config file>", argv[0]);
	exit(1);
    }

    if (pid_fname == NULL)		/* pid filename not specified */
	pid_fname = PID_FILE_DFT;	/* use default */
	
    syslog(LOG_INFO, "Starting up...");

    /* open config file */    
    if ((f = t_fopen(argv[optind], O_RDONLY, 0)) == NULL) {
	syslog(LOG_ERR, "cannot open %s: %s", argv[optind], strerror(pthread_errno()));
	exit(1);
    }    
    
    if ((pidfile = t_fopen(pid_fname, O_WRONLY | O_TRUNC | O_CREAT, 0640)) == NULL) {
	syslog(LOG_ERR, "cannot open %s: %s", pid_fname, strerror(pthread_errno()));
	exit(1);
    }    
    t_fprintf(pidfile, "%d\n", getpid());	/* record pid for killers */
    if (t_fclose(pidfile) < 0) {
    	syslog(LOG_ERR, "cannot record pid: %s", strerror(pthread_errno()));
	exit(1);
    }
    
    signal(SIGTERM, die);	/* kill all children if we terminate */
    
    /* get list of programs to run */
    while(t_gets(line, sizeof(line), f)) {
	if ((j = parse_line(line)) == NULL)
	    continue;
	j->next = NULL;
	if (jobtail == NULL)
	    jobhead = j;
	else
	    jobtail->next = j;
	jobtail = j;
    }
    
    /* set core dump limit to avoid filling disk */
    setcoredumplimit();
    
    /* set stack limit to keep thread size down */
    setstacklimit(stack_limit);
    
    /* start them all up */
    for (j = jobhead; j; j = j->next) {
	startchild(j);
    }
    
    /* now wait for children & restart them */
    
    for (;;) {
	if ((pid = wait3(&wstat, 0, NULL)) < 0) {
	    if (pthread_errno() != EINTR) {
		syslog(LOG_ERR, "wait3: %s", strerror(pthread_errno()));
		break; 
	    }
	} else if (pid != 0) {
	    setcoredumplimit();		/* recompute core limit */
	    for (j = jobhead; j; j = j->next) {
		if (pid == j->pid) 	/* find child that finished */
		    break;
	    }
	    if (!j) {
		syslog(LOG_ERR, "unknown child [%d] finished", pid);
	    } else {
		if (wstat.w_termsig != 0) {
		    syslog(LOG_ERR, "%s [%d] aborted w/ signal %d", 
				    j->argv[0], pid, wstat.w_termsig);
		} else {
		    syslog(LOG_ERR, "%s [%d] exited", j->argv[0], pid);		    
		}
		/* if signal handler is killing everyone, don't restart! */
		if (!shutting_down)
		    startchild(j);	/* and restart it */
	    }
	    sleep(15);			/* don't spin too fast */
	}
	
    }
    
    exit(0);
}

struct job *parse_line(char *line) {

    struct job	*j;		/* returned: job info */
    int		argc;		/* current arg */
    char	*argp = NULL;	/* position in it */
    int		quot = 0;
    int		esc = 0;
    char	*p, c;
    
    j = mallocf(sizeof(struct job));
    /* enough room for max possible args */
    j->argv = mallocf(sizeof(char *) * (1 + strlen(line)/2));
    
    j->argv[0] = NULL;
    p = line;
    argc = 0;
    while(c = *p++) {
	if (j->argv[argc] == NULL) {
	    j->argv[argc] = mallocf(strlen(p) + 1);
	    argp = j->argv[argc];
	}
	if (esc) {		/* literal next */
	    esc = FALSE;
	    *argp++ = c;
	} else if (quot) {	/* quoted-string */
	    if (c == quot) 
		quot = 0;	/* close-quote */
	    else
		*argp++ = c;
	} else {		/* not quoted or escaped */
	    if (c == '\'' || c == '"')
		quot = c;
	    else if (c == '\\')
		esc = TRUE;
	    else if (isspace(c)) { /* end of arg */
	    	*argp = 0;	/* terminate */
		if (strlen(j->argv[argc]) > 0) { /* if not empty */
		    j->argv[++argc] = NULL;	/* start new one */
		}
	    } else		/* normal char! */
		*argp++ = c;	
	}
    }
    
    if (esc || quot) {
	syslog(LOG_ERR, "Unmatched quote: %s", line);
    	t_free(j->argv);
	t_free(j);
	return NULL;
    }
    /* ignore blank lines */
    if (argc == 0 && (j->argv[argc] == NULL || strlen(j->argv[argc]) == 0)) {		
    	t_free(j->argv);
	t_free(j);
	return NULL;
    }
    
    *argp = 0;			/* terminate last arg */
    if (j->argv[argc]) {
	j->argv[++argc] = NULL;	/* mark end of list */    
    }
    
    return j;
}

void startchild(struct job *j) {

    int		i;
    
    j->pid = fork();			
    if (j->pid == -1) {
	syslog(LOG_ERR, "startchild: fork: %s", strerror(pthread_errno()));
    } else if (j->pid == 0) {		/* CHILD: */
	for (i = 3; i < NOFILE; ++i)	/* close any non-standard files */
	    close(i);
	execv(j->argv[0], j->argv);
	syslog(LOG_ERR, "startchild: execv %s: %s", j->argv[0], strerror(pthread_errno()));
	exit(1);
    } 			
    syslog(LOG_INFO, "started %s [%d]; dump limit = %d", j->argv[0], j->pid, rlim.rlim_max);		   
}

/* Shut down -- send SIGTERM to all children */
void die() {

    struct job	*j;
        
    shutting_down = TRUE;
    syslog(LOG_INFO, "shutting down...");
     
    for (j = jobhead; j; j = j->next) {
	if (j->pid > 0)
	    kill(j->pid, SIGTERM);
    }
    
    syslog(LOG_INFO, "all children killed; done");
    (void) unlink(pid_fname);
    
    exit(0);
}

/* 
    Check amount of space remaining on filesystem with /cores directory,
    set core dump limit to half the remaining space. chdir to the /cores
    directory.

*/
void setcoredumplimit() {
 
    struct statfs	buf;
    
    if (STATFS("/cores", &buf, sizeof(buf)) < 0) {
	if (pthread_errno() != ENOENT)
	    syslog(LOG_ERR, "statfs /cores: %s", strerror(pthread_errno()));
    } else {
    /* On NeXT and AIX, bavail is in units of f_bsize; on OSF it's in f_fsize */		
#ifdef __ALPHA_OSF1__
    	rlim.rlim_cur = rlim.rlim_max = buf.f_fsize * buf.f_bavail/2;
#else
    	rlim.rlim_cur = rlim.rlim_max = buf.f_bsize * buf.f_bavail/2;
#endif
    	if (setrlimit(RLIMIT_CORE, &rlim) < 0) /* set the limit */
	    syslog(LOG_ERR, "setcoredumplimit: %s", strerror(pthread_errno()));
	if (chdir("/cores") < 0)
	    syslog(LOG_ERR, "/cores chdir: %s", strerror(pthread_errno()));
    }           
}

void setstacklimit(long limit) {

    struct rlimit stack_lim;
    
    stack_lim.rlim_cur = stack_lim.rlim_max = limit;
    if (setrlimit(RLIMIT_STACK, &stack_lim) < 0) /* set the limit */
	syslog(LOG_ERR, "setstacklimit: %s", strerror(pthread_errno()));

}

void doshutdown () { }	/* not used */
