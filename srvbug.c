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

udb *user_alloc();
void free_user(udb *user);

int main(int argc, char **argv) {

    int			fd;	/* socket we listen on */
    struct sockaddr_in	sin;	/* its addr */
    udb			*user = NULL;	/* new connection */
    int			connfd; /* its file */
    int			len = sizeof(sin); /* addr length */
    int			on = 1;	/* for setsockopt */
    char		logbuf[2*MAX_STR];
    boolean_t		worry;	/* user worry threshold exceeded? */
    static t_file	listen_f; /* t_file for listening socket */
    pthread_t thread;		/* thread var */


    misc_init();                                /* set up global locks */
    t_ioinit();
    t_errinit("srvtest", LOG_LOCAL1);
    t_dndinit();                /* and dnd package */

    read_config();              /* read configuration file */

    user_init();       

    signal(SIGPIPE, SIG_IGN);		/* don't terminate if connections lost */

    sin.sin_family = AF_INET;	
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = htons(1234);	/* port we listen on */


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
    	

    for (;;) {			/* now just keep accepting */

	if (!user)
	    user = user_alloc();	/* get user data block */
		
	connfd = accept(fd, (struct sockaddr *) &user->remoteaddr, &len);
	
	if (connfd < 0) {
	    t_perror("listener: accept: ");
	    sleep(5);			/* spin slowly */
	    continue;			/* try again */
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
	pthread_mutex_unlock(&global_lock);
	
	if (pthread_create(&thread, pthread_attr_default,
		       (pthread_startroutine_t) user_cmd, (pthread_addr_t) user) < 0) {
	    t_perror("listener: pthread_create");
	    free_user(user);		/* couldn't start thread; clean up & back out */
	} else {
	    pthread_detach(&thread);
	}
	user = NULL;		/* it's theirs now, we'll get another */
    }
}

/* user_cmd --

    Thread to read and process commands for a single client.

    Note that the mailbox pointer (user->mb) is valid only when
    the user is validated.
*/

any_t xuser_cmd(any_t user_) {

    udb		*user;			/* user data block */
    mbox	*mb;			/* and mailbox */
    int		cmdnum;			/* index into command table */

    user = (udb *) user_;

    setup_signals();			/* set up signal handlers for new thread */


    /* if not forced shutdown, read next command */
    while(!user->shutdown) {

	/* before each command, adjust timeout based on current
	   user load */
	pthread_mutex_lock(&global_lock);
	if (user->validating)			/* if signing on */
	    user->conn.timeout = 1;		/* time out very fast */
	else {
	    user->conn.timeout = u_timeout;	/* timeout (minutes) */
	    if (u_num > u_worry)		/* if beyond threshold */
		user->conn.timeout -= (u_num - u_worry); /* time out more agressively */
	}
	if (user->conn.timeout <= 0)
	    user->conn.timeout = 1;		/* min value (0=no timeout) */
	else
	    user->conn.timeout *= 60;		/* value in seconds */
	pthread_mutex_unlock(&global_lock);
	
	if (!user->conn.urgent)
	    user->comp = t_gets(user->comline, MAX_STR, &user->conn);
	
	/* handle breaks */
	if (user->conn.urgent) {
	    t_skipurg(&user->conn);	/* consume the urgent data */
	    t_fflush(&user->conn);
	    print(user, BLITZ_BREAKACK); /* acknowledge it */
	} else if (user->comp == NULL) { /* EOF --> connection gone */
	    break;				
	} else {			/* have a Blitz command */
	    /* flag connection as being ready for output (strict alternation, remember)
	       to avoid another pass through t_select before responding */
	    user->conn.can |= SEL_WRITE;
	
	    t_fflush(&user->conn);
	    t_fprintf(&user->conn, "010 Ok\r\n");
	}
	
	if (user->conn.fd >= 0) {
	    t_fflush(&user->conn);	/* flush output, if connected */
	    t_fseek(&user->conn, 0, SEEK_CUR); /* seek to set up to read again */
	}
	
    }

    free_user(user);		/* clean up & free udb */

    return 0;
}
