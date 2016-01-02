/*	BlitzMail Server -- main program.
	
    Copyright (c) 1994, by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    David Gelhar
    
    Do initialization, then fork the basic threads that are always running
    (waiting for users, SMTP connections, etc.)
    
*/

#if !defined(lint)
static char rcsid[] = "$Header: /users/davidg/source/blitzserver/RCS/blitzserv.c,v 3.6 98/10/21 15:57:29 davidg Exp Locker: davidg $";

static char copyright[] = "Copyright 1996 by the Trustees of Dartmouth College; \
see the file 'Copyright' for conditions of use";

#endif

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <fcntl.h>
#include <syslog.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "client.h"
#include "config.h"
#include "control.h"
#include "mess.h"
#include "deliver.h"
#include "queue.h"
#include "smtp.h"
#include "cty.h"
#include "ddp.h"
#include "cryptutil.h"

any_t listener(any_t zot);
int poplistener(any_t zot);
int poppassdlistener(any_t zot);
any_t shutdownpoll(any_t zot);
void shutdownsig();
void doshutdown();

/* main program --

    Note that we do NOT disassociate from the controlling terminal -
    "master" wants to know if we terminate; it disassociates for us. 
*/

int main (int argc, char **argv) {
        
    pthread_t thread;		/* thread var */

    setup_signals();		/* standard setup for synchronous signals */
    
    signal(SIGQUIT, abortsig);	/* generate clean dump on QUIT signal */
    
    misc_init();		/* initialize mutexes */	
    t_ioinit();			/* initialize io routines */
        
    t_errinit("blitzserv", LOG_LOCAL1);	/* initialize error package */
    t_dndinit();		/* and dnd package */

    read_config();		/* read configuration file */
    
    /* don't try to start DDP if appletalk disabled */
    if (!m_noappletalk && !ddpinit())	/* start ddp package */
	exit(1);    
    
    
    mess_init();		/* initialize message code */
    mbox_init();		/* initialize mailbox code */
    user_init();		/* initialize client code */
    smtp_init();		/* initialize smtp functions */
        
    date_time(up_date, up_time);/* date & time of startup */
    
    signal(SIGTERM, shutdownsig); /* to shut down cleanly (_after_ inits) */
    
    queue_init();		/* now, start up queue threads */

    /* start thread to handle udp status requests */
    if (pthread_create(&thread, generic_attr,
                   (pthread_startroutine_t) ctl_udplisten, (pthread_addr_t) 0) < 0) {
	t_perror("ctl_udplisten pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);
    
    /* start up thread to listen for users */
    if (pthread_create(&thread, generic_attr,
                   (pthread_startroutine_t) listener, (pthread_addr_t) 0) < 0) {
	t_perror("listener pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);
    
    /* start up thread to listen for POP users */
    if (pthread_create(&thread, generic_attr,
                   (pthread_startroutine_t) poplistener, (pthread_addr_t) 0) < 0) {
	t_perror("poplistener pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);
    
    /* start up thread to listen for POP password changers */
    if (pthread_create(&thread, generic_attr,
                   (pthread_startroutine_t) poppassdlistener, (pthread_addr_t) 0) < 0){
	t_perror("pospassdlistener pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);
    
    /* start up thread to listen for smtp connections */
    if (pthread_create(&thread, generic_attr,
                   (pthread_startroutine_t) smtplisten, (pthread_addr_t) 0) < 0) {
	t_perror("smtplisten pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);
   
    /* start up thread to listen for cty connections */
    if (pthread_create(&thread, generic_attr,
                   (pthread_startroutine_t) ctylisten, (pthread_addr_t) 0) < 0) {
	t_perror("ctylisten pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);

    /* start up thread to watch for shutdown signal */
    if (pthread_create(&thread, generic_attr,
                   (pthread_startroutine_t) shutdownpoll, (pthread_addr_t) 0) < 0) {
	t_perror("shutdownpoll pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);
       
    /* main thread will write changes periodically */
    (void) mbox_writer((any_t) 0);
        
    return 0;			/* notreached */
}

/* listener --

    Set up a socket listening for client connections.  When one arrives,
    accept it and spawn a thread to deal with it.
*/

any_t listener(any_t zot) {

    int			fd;	/* socket we listen on */
    struct sockaddr_in	sin;	/* its addr */
    struct servent	*sp;	/* services entry */
    udb			*user = NULL;	/* new connection */
    int			connfd; /* its file */
    int			len = sizeof(sin); /* addr length */
    int			on = 1;	/* for setsockopt */
    char		logbuf[2*MAX_STR];
    boolean_t		worry;	/* user worry threshold exceeded? */
    static t_file	listen_f; /* t_file for listening socket */
    pthread_t thread;		/* thread var */

    setup_signals();                  /* set up signal handlers for new thread */
    setup_syslog();

    sem_seize(&herrno_sem);		/* serialize access to getservbyname */
           
    if ((sp = getservbyname(BLITZ_SERV, "tcp")) == NULL) {
	t_errprint_s("listener: unknown service: %s", BLITZ_SERV);
	exit(1);
    }
    sin.sin_family = AF_INET;	
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = sp->s_port;	/* port we listen on */
    sp = NULL;			/* sppml */
    sem_release(&herrno_sem);    
    
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	t_perror("listener: socket: ");
	exit(1);
    }   
    
    /* set REUSEADDR so we won't get an EADDRINUSE if there are connections
       from our previous incarnation lingering in TIME_WAIT */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) < 0)
	t_perror("listener: setsockopt (SO_REUSEADDR)");
    
    if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	t_perror("listener: bind");
	exit(1);
    }

    listen(fd, 5);		/* accept connections (up to 5 in advance) */
 
    t_fdopen(&listen_f, fd);	/* set up (useless) t_file for the socket... */
    strcpy(listen_f.name, "listener"); /* ...just to get it entered in t_fdmap */
    	
    t_sprintf(logbuf, "BlitzMail server %s up (%s).", m_server[m_thisserv], server_vers);
    t_errprint(logbuf);
    log_it(logbuf);
    
    if (m_dndresolver) {		/* are we responsible for DND redirection */
	t_sprintf(logbuf, "Redirecting mail for DND psuedo-host %s", m_dndhost[0]);
	log_it(logbuf);
	t_errprint(logbuf);
    }
    
    for (;;) {			/* now just keep accepting */
    
	if (!user)
	    user = user_alloc();	/* get user data block */
	
	/* if too many connections already, don't accept any more */
	pthread_mutex_lock(&global_lock);	
	while (u_num >= u_max) 		/* too many users? */
	    pthread_cond_wait(&usermax_wait, &global_lock); 
	pthread_mutex_unlock(&global_lock);	
	
	connfd = accept(fd, (struct sockaddr *) &user->remoteaddr, &len);
	
	if (connfd < 0) {
	    t_perror("listener: accept: ");
	    sleep(5);			/* spin slowly */
	    continue;			/* try again */
	}
	if (server_shutdown) {		/* if in process of shutting down */
	    return 0;			/* don't accept any more users */
	}    
	
	/* enable periodic tickles */
	if (setsockopt(connfd, SOL_SOCKET, SO_KEEPALIVE, (char *) &on, sizeof(on)) < 0)
	    t_perror("listener: setsockopt (SO_KEEPALIVE)");
	/* non-blocking io mode */
	if (ioctl(connfd, FIONBIO, (char *) &on) < 0)
	    t_perror("listener: ioctl (FIONBIO)");
	
	t_fdopen(&user->conn, connfd); 	/* set up t_file for connection */
	strcpy(user->conn.name, "Client (not logged in)");
	user->conn.select = TRUE;	/* t_select should check this fd */
	
	pthread_mutex_lock(&global_lock);	
	user->next = u_head;		/* add to front of user list */
	u_head = user;
	if (++u_num > u_hwm)		/* count number of active users */
	    u_hwm = u_num;  		/* new high water mark? */ 
	worry = u_num > u_worry;	/* getting close to user limit? */
	pthread_mutex_unlock(&global_lock);
    
	if (worry) {			/* lots of users... */
	    udb *u;			/* ...so recompute all timeouts */
	    for (u = u_head; u; u = u->next) {
		if (!u->validating)	/* short timer during signon */
		    u->conn.timeout = 60 * (u_timeout - (u_num - u_worry));
	    }
	    pthread_cond_signal(&timeout_wait); /* and see if some can time out now */
	}
	
	if (pthread_create(&thread, generic_attr,
		       (pthread_startroutine_t) user_cmd, (pthread_addr_t) user) < 0) {
	    t_perror("listener: pthread_create"); 
	    free_user(user);		/* couldn't start thread; clean up & back out */
	    sleep(5);
	} else {
	    pthread_detach(&thread);
	}
	user = NULL;		/* it's theirs now, we'll get another */
    }
}

/* poplistener --

    Set up a socket listening for POP client connections.  When one arrives,
    accept it and spawn a thread to deal with it.
*/

int poplistener(any_t zot) {

    int			fd;	/* socket we listen on */
    struct sockaddr_in	sin;	/* its addr */
    struct servent	*sp;	/* services entry */
    udb			*user = NULL;	/* new connection */
    int			connfd; /* its file */
    int			len = sizeof(sin); /* addr length */
    int			on = 1;	/* for setsockopt */
    char		logbuf[2*MAX_STR];
    boolean_t		worry;	/* user worry threshold exceeded? */
    static t_file	listen_f; /* t_file for listening socket */
    pthread_t thread;		/* thread var */

    setup_signals();		/* set up signal handlers for new thread */
    setup_syslog();
    
    sem_seize(&herrno_sem);		/* serialize access to getservbyname */
           
    if ((sp = getservbyname(POP_SERV, "tcp")) == NULL) {
	t_errprint_s("poplistener: unknown service: %s, POP support disabled", POP_SERV);
	sem_release(&herrno_sem);
	return 0;
    }
    sin.sin_family = AF_INET;	
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = sp->s_port;	/* port we listen on */
    sp = NULL;			/* sppml */
    sem_release(&herrno_sem);    
    
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	t_perror("poplistener: socket: ");
	return 0;
    }   
    
    /* set REUSEADDR so we won't get an EADDRINUSE if there are connections
       from our previous incarnation lingering in TIME_WAIT */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) < 0)
	t_perror("poplistener: setsockopt (SO_REUSEADDR)");
    
    if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	t_perror("poplistener: bind");
	return 0;
    }

    listen(fd, 5);		/* accept connections (up to 5 in advance) */
 
    t_fdopen(&listen_f, fd);	/* set up (useless) t_file for the socket... */
    strcpy(listen_f.name, "poplistener"); /* ...just to get it entered in t_fdmap */
    	
    t_sprintf(logbuf, "BlitzMail POP server %s up.", m_server[m_thisserv]);
    t_errprint(logbuf);
    log_it(logbuf);
    
    for (;;) {			/* now just keep accepting */
    
	if (!user)
	    user = user_alloc();	/* get user data block */
	
	/* if too many connections already, don't accept any more */
	pthread_mutex_lock(&global_lock);	
	while (u_num >= u_max) 		/* too many users? */
	    pthread_cond_wait(&usermax_wait, &global_lock); 
	pthread_mutex_unlock(&global_lock);	
	
	connfd = accept(fd, (struct sockaddr *) &user->remoteaddr, &len);
	
	if (connfd < 0) {
	    t_perror("poplistener: accept: ");
	    sleep(5);			/* spin slowly */
	    continue;			/* try again */
	}
	if (server_shutdown) {		/* if in process of shutting down */
	    return 0;			/* don't accept any more users */
	}    
	
	/* enable periodic tickles */
	if (setsockopt(connfd, SOL_SOCKET, SO_KEEPALIVE, (char *) &on, sizeof(on)) < 0)
	    t_perror("poplistener: setsockopt (SO_KEEPALIVE)");
	/* non-blocking io mode */
	if (ioctl(connfd, FIONBIO, (char *) &on) < 0)
	    t_perror("poplistener: ioctl (FIONBIO)");
	
	t_fdopen(&user->conn, connfd); 	/* set up t_file for connection */
	strcpy(user->conn.name, "POP Client (not logged in)");
	user->conn.select = TRUE;	/* t_select should check this fd */
	
	pthread_mutex_lock(&global_lock);	
	user->next = u_head;		/* add to front of user list */
	u_head = user;
	if (++u_num > u_hwm)		/* count number of active users */
	    u_hwm = u_num;  		/* new high water mark? */ 
	worry = u_num > u_worry;	/* getting close to user limit? */
	pthread_mutex_unlock(&global_lock);
    
	if (worry) {			/* lots of users... */
	    udb *u;			/* ...so recompute all timeouts */
	    for (u = u_head; u; u = u->next) {
		if (!u->validating)	/* short timer during signon */
		    u->conn.timeout = 60 * (u_timeout - (u_num - u_worry));
	    }
	    pthread_cond_signal(&timeout_wait); /* and see if some can time out now */
	}
	
	user->pop = TRUE;		/* mark as POP session */


	if (pthread_create(&thread, generic_attr,
		       (pthread_startroutine_t) popuser_cmd, (pthread_addr_t) user) < 0) {
	    t_perror("poplistener: pthread_create"); 
	    free_user(user);		/* couldn't start thread; clean up & back out */
	    sleep(5);
	} else {
	    pthread_detach(&thread);
	}
	user = NULL;		/* it's theirs now, we'll get another */
    }
}

/* poppassdlistener --

    Set up a socket listening for POP password-changing client connections.  
    When one arrives, accept it and spawn a thread to deal with it.
*/

int poppassdlistener(any_t zot) {

    int			fd;	/* socket we listen on */
    struct sockaddr_in	sin;	/* its addr */
    struct servent	*sp;	/* services entry */
    struct sockaddr_in 	remoteaddr;	/* client address */
    t_file		*conn = NULL;	/* client connection */
    int			connfd;	/* socket */
    int			len = sizeof(sin); /* addr length */
    int			on = 1;	/* for setsockopt */
    char		logbuf[2*MAX_STR];
    static t_file	listen_f; /* t_file for listening socket */
    pthread_t thread;		/* thread var */

    setup_signals();		/* set up signal handlers for new thread */
    setup_syslog();

    sem_seize(&herrno_sem);		/* serialize access to getservbyname */
           
    if ((sp = getservbyname(POPPASSD_SERV, "tcp")) == NULL) {
	t_errprint_s("poppassdlistener: unknown service: %s", POPPASSD_SERV);
	sem_release(&herrno_sem);
	return 0;
    }
    sin.sin_family = AF_INET;	
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = sp->s_port;	/* port we listen on */
    sp = NULL;			/* sppml */
    sem_release(&herrno_sem);    
    
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	t_perror("poppassdlistener: socket: ");
	return 0;
    }   
    
    /* set REUSEADDR so we won't get an EADDRINUSE if there are connections
       from our previous incarnation lingering in TIME_WAIT */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) < 0)
	t_perror("poppassdlistener: setsockopt (SO_REUSEADDR)");
    
    if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	t_perror("poppassdlistener: bind");
	return 0;
    }

    listen(fd, 5);		/* accept connections (up to 5 in advance) */
 
    t_fdopen(&listen_f, fd);	/* set up (useless) t_file for the socket... */
    strcpy(listen_f.name, "poppassdlistener"); /* ...just to get it entered in t_fdmap */
    	
    t_sprintf(logbuf, "BlitzMail POP Password Changing server %s up.", m_server[m_thisserv]);
    t_errprint(logbuf);
    log_it(logbuf);
    
    for (;;) {			/* now just keep accepting */
    
    	if (!conn) {			/* need to allocate a connection block? */
	    conn = (t_file *) mallocf(sizeof(t_file));
	    conn->fd = -1;
	}
    
	/* if too many user connections already, don't accept password changers */
	pthread_mutex_lock(&global_lock);	
	while (u_num >= u_max) 		/* too many users? */
	    pthread_cond_wait(&usermax_wait, &global_lock); 
	pthread_mutex_unlock(&global_lock);	
	
	connfd = accept(fd, (struct sockaddr *) &remoteaddr, &len);
	
	if (connfd < 0) {
	    t_perror("poppassdlistener: accept: ");
	    sleep(5);			/* spin slowly */
	    continue;			/* try again */
	}
	if (server_shutdown) {		/* if in process of shutting down */
	    return 0;			/* don't accept any more clients */
	}    
	
	/* enable periodic tickles */
	if (setsockopt(connfd, SOL_SOCKET, SO_KEEPALIVE, (char *) &on, sizeof(on)) < 0)
	    t_perror("poppassdlistener: setsockopt (SO_KEEPALIVE)");
	/* non-blocking io mode */
	if (ioctl(connfd, FIONBIO, (char *) &on) < 0)
	    t_perror("poppassdlistener: ioctl (FIONBIO)");
	
	t_fdopen(conn, connfd); 	/* set up t_file for connection */
	strcpy(conn->name, "POP Password Client");
	conn->select = TRUE;	/* t_select should check this fd */

	if (pthread_create(&thread, generic_attr,
		       (pthread_startroutine_t) poppassd, (pthread_addr_t) conn) < 0) {
	    t_perror("poppassd: pthread_create"); 
	    t_closefd(conn->fd);		/* close file & remove from fd table */  
	    t_free(conn);			/* free connection block */  
	    sleep(5); 
	} else {
	    pthread_detach(&thread);
	}
	
	conn = NULL;		/* it's theirs now, we'll get another */
    }
}

/* shutdownsig --
    shutdownpoll -- 
    doshutdown --
    
    When a SIGTERM is received, the signal handler "shutdownsig" is invoked; its
    only function is to set the "server_shutdown" flag.  (NOTE:  it is NOT safe for
    a signal handler to perform any operation of a condition or mutex!)
    
    When someone (shutdownpoll) notices that the shutdown flag is set, doshutdown is
    called to write out all mailbox changes and exit.	
*/

void shutdownsig() {

    server_shutdown = TRUE;
}

any_t shutdownpoll(any_t zot) {

    setup_syslog();

    for (;;) {
    	sleep(1);			/* check flag once a second */
	if (server_shutdown)
	    doshutdown();		/* never returns */
    }

}

void doshutdown() {

    t_errprint("Shutting down....");
    mbox_dowrite(FALSE);
    t_errprint("...shutdown complete; exiting");
    
    exit(0);

}

