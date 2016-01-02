/*

    Compute messid.  Search all mailboxes to find largest messid currently in use.
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/computemessid.c,v 3.1 97/10/19 18:57:59 davidg Exp Locker: davidg $
    
*/
#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <syslog.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "mess.h"

int	finished = 0;
pthread_cond_t finish_wait;
long	new_messid = 100;	/* minimum initial messid */

any_t checkfs(any_t _fs);
void readmessdir(long uid, long fs);

void doshutdown() {}

int main (int argc, char **argv) {

    int		i;
    int		messid_f;
    char	buf[16];
    pthread_t	thread;
    int			sock;	/* blitzmail server port socket */
    struct sockaddr_in	sin;	/* its addr */
    struct servent	*sp;	/* services entry */
    int			on = 1;	/* for setsockopt */
    	
    misc_init();				/* set up global locks */
    pthread_cond_init(&finish_wait, pthread_condattr_default);
    
    t_ioinit();
    t_errinit("computemessid", LOG_LOCAL1);	/* initialize error package */
    t_dndinit();		/* and dnd package */
    
    read_config();		/* read configuration file */

    if (!f_messid) {
	fprintf(stderr, "Bad config file: messid file not defined!\n");
	exit(1);
    }

    if ((messid_f = open(f_messid, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) < 0) {
	fprintf(stderr, "Cannot open "); perror(f_messid);
	exit(1);
    }
            
    /* verify that server isn't running -- try to bind to its address */
 
     if ((sp = getservbyname(BLITZ_SERV, "tcp")) == NULL) {
	fprintf(stderr, "unknown service: %s", BLITZ_SERV);
	exit(1);
    }
    
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	perror("socket: ");
	exit(1);
    }   
    
    /* set REUSEADDR so we won't get an EADDRINUSE if there are connections
       lingering in TIME_WAIT */
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) < 0)
	perror("setsockopt (SO_REUSEADDR)");

    sin.sin_family = AF_INET;	
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = sp->s_port;	/* blitz server port */
    
    if (bind(sock, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	if (pthread_errno() == EADDRINUSE) {
	    fprintf(stderr, "\n###  BlitzMail server is running!  ###\n");
	    fprintf(stderr, " (Must kill it before running %s.)\n\n" , argv[0]);
	} else
	    perror("bind");
	exit(1);
    }
    
    /* leave socket open to keep server from starting up while we're running */
    
    pthread_mutex_lock(&syslog_lock);    
    fprintf(stderr, "**\n** Filesystems configured:\n**\n");
    for (i = 0; i < m_filesys_count; ++i) {
	fprintf(stderr, "    %s\n", m_filesys[i]);
	if (pthread_create(&thread, generic_attr,
			(pthread_startroutine_t) checkfs, (pthread_addr_t) i) < 0) {
	    t_perror("pthread_create");
	    exit(1);
	}
	pthread_detach(&thread);
    }
    fprintf(stderr, "\nComputing...");
    pthread_mutex_unlock(&syslog_lock); 
    
    /* wait for all threads to finish */
    pthread_mutex_lock(&global_lock);
    while(finished < m_filesys_count)
	pthread_cond_wait(&finish_wait, &global_lock);

    ++new_messid;				/* use next available messid */
	
    fprintf(stderr, "\n\n** \n** New messid will be: %ld\n", new_messid);
    
    t_sprintf(buf, "%ld\n", new_messid);
    if (write(messid_f, buf, strlen(buf)) < 0 || fsync(messid_f) < 0) {
	t_perror("panic! cannot record new messid");
	exit(1);
    }

    fprintf(stderr, "** %s:  Done.\n**\n", argv[0]);
    
    close(sock);			/* server can run now */
    
    exit(0);
}

/* check one filesystem's boxes */

any_t checkfs(any_t _fs) {

    long		fs;			/* filesystem to check */
    char		fname[MBOX_NAMELEN];	/* name of box dir on that fs */
    DIR			*dirf;			/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    long		uid;			/* one box */
    char		*end;			/* end of uid str */

    fs = (long) _fs;		
    
    /* open box directory */

    t_sprintf(fname, "%s%s", m_filesys[fs], BOX_DIR);
    fname[strlen(fname) - 1] = 0;		/* chop trailing '/' */
    if (mkdir(fname, DIR_ACC) < 0 && pthread_errno() != EEXIST) {
	t_perror1("checkfs: cannot create ", fname);
	exit(1);    
    }
    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    dirf = opendir(fname);
    pthread_mutex_unlock(&dir_lock);

    if (dirf == NULL) {
	t_perror1("checkfs: cannot open ", fname);
	exit(1);
    } 

    while ((dirp = readdir(dirf)) != NULL) {	/* read entire directory */
	/* skip dot-files */
	if (dirp->d_name[0] != '.') {
	    end = strtonum(dirp->d_name, &uid);
	    if (*end == 0)	/* ignore non-numeric filenames */
		readmessdir(uid, fs);	
	}
    }
    
    closedir(dirf);
        
    pthread_mutex_lock(&global_lock);	/* count threads that have finished */
    ++finished;
    pthread_mutex_unlock(&global_lock);
    pthread_cond_signal(&finish_wait);	/* wake main thread */
    
    return 0;				/* thread fades away */
}

void readmessdir(long uid, long fs) {

    char		messdir[FILENAME_MAX];
    DIR			*dirf;			/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    long		messid;			/* current message file id */	
    char		*delim;
    
    t_sprintf(messdir, "%s%s%ld%s", m_filesys[fs], BOX_DIR, uid, MESS_DIR);

    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    dirf = opendir(messdir);
    pthread_mutex_unlock(&dir_lock);

    if (dirf == NULL) {
	if (pthread_errno() != ENOENT) {	/* if no messages yet, dir may not exist */
	    t_perror1("readmessdir: cannot open ", messdir);
	    exit(1);
	}
	return;
    }

    while ((dirp = readdir(dirf)) != NULL) {	/* read entire directory */
	    
	/* skip dot-files & non-numeric names */
	delim = strtonum(dirp->d_name, &messid);
	
	if (*delim == 0) { /* ignore names with non-digits */
	    pthread_mutex_lock(&global_lock);
	    if (messid > new_messid)
		new_messid = messid; /* larger id seen */
	    pthread_mutex_unlock(&global_lock);
	}
    }
    
    closedir(dirf); 
}
