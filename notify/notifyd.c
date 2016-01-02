/*  Notification server

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.


    The notification server is a clearinghouse for notifications (brief
    messages informing a user of some asynchronous event.)  Notification
    clients acting on behalf of user(s) register their network address
    with us. Entities (like the BlitzMail server) that wish to inform 
    the user of some event call us, and we take care of directing the
    notification to the location(s) that are listening for that user.
    
    Both the registration (when a client announces that it's interested
    in notifications for a given user) and the notification itself are
    presently implemented using ATP transactions.  A single ATP socket
    both accepts registration transactions and sends notification transactions.

    The notification table (the mapping of uid's to notifier address(es))
    is persistent -- once a notifier is registered it remains in the table
    until an attempt to deliver a notification to that address fails.
    -- discard "very old" entries?
    
    The user population is split up among a number of notification servers;
    clients must consult the NOTIFYSERV field of the DND record to
    locate the proper notification server for a particular user.  (The
    DND field gives the object & zone; the type is the known value
    "notify".)  At the moment, there is a one-to-one mapping between
    BlitzMail servers and notification servers (this simplifies the Blitz
    server's job, and also means that the two have the same hours of
    operation etc.), but clients are not to rely on this identity.

    Notifications are typed; possible types include BlitzMail notifications,
    and Bulletin notifications.  The client registration request includes
    a list of types that the client is interested in, but we currently
    ignore it (the client must filter unwanted types).  So far, most clients
    will be accepting all types, so the extra bookkeeping to remember what
    they asked for wouldn't be worthwhile.
    
    The notification begins with a standard header giving type and id.
    The format of the rest of the notification  is type-dependent; the 
    notification server doesn't interpret it.
    
    
    Lock ordering (to avoid deadlock):

    dnd_sem			(seize first)
    stickytab_lock
    notifytab_lock		
    req_lock			(seize last)
    
*/
static char rcsid[] = "$Header: /users/davidg/source/blitzserver/notify/RCS/notifyd.c,v 2.18 98/10/21 17:14:38 davidg Exp Locker: davidg $";

#include "../port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <syslog.h>
#include "../ddp.h"
#include "../t_err.h"
#include "../nbpd/nbpd.h"
#include "../t_io.h"
#include "../misc.h"
#include "../t_dnd.h"
#include "../config.h"
#include "../sem.h"
#include "not_types.h"
#include "notify.h"

/* dnd fields needed for validation */
static char *val_farray[] = { "NAME", "UID",  "PERM",  "DUP", NULL };

void listener(any_t family_);
void notifylisten (any_t zot);
void notify_tcp(any_t state_);
void process_requests(any_t zot);
void atp_periodic(any_t zot);
void writer(any_t zot);
void nbp_reg_xmit(any_t np);
void initialize();
void fatal ();
void not_clear(notifystate *state);
void not_noop(notifystate *state);
void not_notify(notifystate *state);
void not_pass(notifystate *state);
void not_quit(notifystate *state);
void not_user(notifystate *state);
int do_notify(long uid, long typ, long id, notifydat data, long len,
               atpaddr *toaddr, boolean_t sticky);
void notify_one(long typ, long id, notifydat data, long len, notifybuck *bp, int i);
void trel (atphdr *atp, atpaddr *clientaddr);
void tres (atphdr *atp, atpaddr *clientaddr);
boolean_t tickle (ddpbuf *pktp, int pktl, atpaddr *clientaddr);
sta_typ reg_request(req	*reqp, atphdr *atp);
sta_typ clear_request(req *reqp, atphdr *atp);
void new_request(ddpbuf *pktp, int pktl, atphdr *atp, atpaddr *clientaddr);
boolean_t dupreq (atphdr *atp, atpaddr *clientaddr);
void req_link(reqq *q, req *p);
void req_unlink(reqq *q, req *p);
req *req_find(reqq *q, atphdr *atp, atpaddr *clientaddr);
sta_typ update_entry(char *name, long *uid, atpaddr *regaddr, servtab servcode);
void make_entry(long uid, atpaddr *regaddr, long when, servtab servcode);
boolean_t remove_entry(long uid, atpaddr *regaddr);
boolean_t find_entry(long uid, notifybuck **bp, atpaddr *clientaddr, int *i);
boolean_t sticky_find(long uid, stickybuck **bp, int *i);
void sticky_set(long uid, long typ, long id, notifydat data, long len);
void sticky_make(long uid, stickybuck **b, int *i);
void sticky_clear(long uid, long typ);
void read_notifytab();
void write_notifytab();
void read_stickytab();
void write_stickytab();
int atpwrite(atpskt *skt, ddpbuf *bufp, int len, atpaddr *remoteaddr);
int atpread(atpskt *skt, ddpbuf *bufp, atpaddr *source);
u_short next_tid();
void abortsig();
void pkterr(char *msg, atpaddr *clientaddr);
void pkterr_i(char *fmt, int i, atpaddr *clientaddr);

/*
    Main program.

    Register our rpc port with the mach name server.  Start up threads
    to handle the registration socket, aging, and notification sending.
    Main thread will loop handling rpc requests.
*/

int main (int argc,char **argv) {

    pthread_t	thread;
 
    initialize();		/* do our initialization */
    
    /* start threads to handle registration sockets */
    if (pthread_create(&thread, pthread_attr_default,
                   (pthread_startroutine_t) listener, (pthread_addr_t) AF_INET) < 0) {
	t_perror("listener pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);

    if (!m_noappletalk) {
	if (pthread_create(&thread, pthread_attr_default,
			(pthread_startroutine_t) listener, (pthread_addr_t) AF_APPLETALK) < 0) {
	    t_perror("AT listener pthread_create failed");
	    exit(1);
	}
	pthread_detach(&thread);
    }
    
    /* start thread to process registration requests */
    if (pthread_create(&thread, pthread_attr_default,
                   (pthread_startroutine_t) process_requests, (pthread_addr_t) 0) < 0) {
	t_perror("process_requests pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);

    /* start thread to handle TCP notification requests */
    if (pthread_create(&thread, pthread_attr_default,
                   (pthread_startroutine_t) notifylisten, (pthread_addr_t) 0) < 0) {
	t_perror("notifylisten pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);
   
    /* start thread to do ATP retransmissions & timeouts */
    if (pthread_create(&thread, pthread_attr_default,
                   (pthread_startroutine_t) atp_periodic, (pthread_addr_t) 0) < 0) {
	t_perror("atp_periodic pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);

    /* start thread to write out table */
    if (pthread_create(&thread, pthread_attr_default,
                   (pthread_startroutine_t) writer, (pthread_addr_t) AF_INET) < 0) {
	t_perror("writer pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);
    
    /* main thread just naps */
    for (;;) {
    	sleep(999);
    }	
}
/*
    initialize --

	One-time initialization.
*/
void initialize () {

    int i;
    char		logbuf[512];
    
    setup_signals();			/* set up signal handlers for new thread */
    
    misc_init();			/* initialize mutexes */        
    t_ioinit();				/* set up io package */
    t_errinit("notifyd", LOG_LOCAL1);	/* set up error package */
    t_dndinit();			/* and dnd package */

    /* initialize mutex's & conditions */
    pthread_mutex_init(&notifytab_lock, pthread_mutexattr_default);
    pthread_mutex_init(&stickytab_lock, pthread_mutexattr_default);
    pthread_mutex_init(&tid_lock, pthread_mutexattr_default);
    pthread_mutex_init(&req_lock, pthread_mutexattr_default);
    pthread_mutex_init(&herrno_lock, pthread_mutexattr_default);
    pthread_cond_init(&req_wait, pthread_condattr_default);    

    /* initialize data structures */	
    for (i = 0; i < NTABSIZE; i++)
	    notifytab[i] = NULL;

    newreq.head = newreq.tail = NULL;
    oldreq.head = oldreq.tail = NULL;
    rtxreq.head = rtxreq.tail = NULL;

    /* read blitzmail server configuration to get hostname & filenames */
    read_config();
        
    /* don't try to start DDP if appletalk disabled */
    if (!m_noappletalk && !ddpinit())	/* set up ddp package (after config) */
	    fatal();
    	

    pthread_mutex_lock(&notifytab_lock);
    read_notifytab();			/* read saved notifytab */
    pthread_mutex_unlock(&notifytab_lock);
    
    pthread_mutex_lock(&stickytab_lock);
    read_stickytab();			/* and stickytab */
    pthread_mutex_unlock(&stickytab_lock);
        
    if (m_noappletalk) {
	t_sprintf(logbuf, "Notification server up (UDP only).\n");
    } else {
    	pthread_mutex_lock(&inet_ntoa_lock);
	t_sprintf(logbuf, "Notification server up.  Gw %s; net %d; zone %s\n",
		    inet_ntoa(gwaddr.sin_addr), my_atnet, my_atzone);
	pthread_mutex_unlock(&inet_ntoa_lock);
    }
    t_errprint(logbuf);    

}

/* notify --

    Called by not_server when a request to send a notification
    comes in.
*/

kern_return_t notify(port_t reqport, uid_typ uid, long typ,
	long id, notifydat data, long len, boolean_t sticky) {
    
    (void) do_notify(uid, typ, id, data, len, NULL, sticky);
    
    return KERN_SUCCESS;
}

/* notify_clear --

    Called by not_server when a request to clear a notification
    comes in.
*/

kern_return_t notify_clear(port_t reqport, uid_typ uid, long typ) {

    pthread_mutex_lock(&stickytab_lock);    
    sticky_clear(uid, typ); 		/* remove entry from stickytab */
    pthread_mutex_unlock(&stickytab_lock);
    
    return KERN_SUCCESS;
}

/* do_notify --

    Internal routine to send notification.  Called from RPC
    interface ("notify", above), from the TCP interface, and 
    by the server itself (to send sticky notifications.)
        	
    Search notifier table for all entries for the given uid (or,
    for just a particular address if one is specified.)  Construct
    the notification (an ATP request), place it on the retransmit
    queue, and send it.
    
    If "sticky" is set, enter the notification into "stickytab" -
    it will be re-sent to any clients that register for this uid.
    
*/

int do_notify(long uid, long typ, long id, notifydat data, long len,
               atpaddr *toaddr, boolean_t sticky) {
   
    notifybuck *bp;             /* bucket of notifytab */
    int i, j;                   /* temps */
    int hash;			/* bucket number */	
    int	sent = 0;		/* returned: number of clients notification sent to */
    int	reset = FALSE;		/* removing this uid from our tables? */
    atpaddr *remtab = NULL;	/* addresses to remove */
    int remcount = 0;		/* valid entries */
    int remmax = 100;		/* size allocated */

    /* enter sticky notification into table, but there's no such thing
       as a sticky control message */
    if (sticky && typ != NTYPE_CTL) {
	pthread_mutex_lock(&stickytab_lock);
	sticky_set(uid, typ, id, data, len);
	pthread_mutex_unlock(&stickytab_lock);
    }  
    
    /* check for reset message */
    if (typ == NTYPE_CTL && len == NCTL_RESET_LEN &&
        		    bcmp(data, NCTL_RESET, NCTL_RESET_LEN) == 0) {
	reset = TRUE;
	remtab = mallocf(remmax * sizeof(atpaddr));
    }
    
    pthread_mutex_lock(&notifytab_lock);
 
    hash = uid % NTABSIZE;	/* choose the proper list */
   
    /* search table for all entries with correct uid and service type
       (and correct address, if one was specified) */
       
    for (bp = notifytab[hash]; bp; bp = bp->flink) {
	for (i = 0; i < bp->count; i++) {
	    if (bp->entry[i].uid == uid &&
	    (toaddr == NULL || ATP_ADDR_EQ(toaddr, &bp->entry[i].regaddr))) {
		for (j = 0; j < bp->entry[i].servcode.count; ++j) {
		    if (typ == NTYPE_CTL || bp->entry[i].servcode.serv[j] == typ) {
			notify_one(typ, id, data, len, bp, i);
			++sent;	/* found client registered for this type */
			break;	
		    }
		}
		if (reset) {		/* make list of entries for this uid */
		    if (remcount == remmax) {
			remmax += 100;	/* grow as needed */
			remtab = reallocf(remtab, remmax * sizeof(atpaddr));
		    }
		    remtab[remcount++] = bp->entry[i].regaddr;
		}
	    }
	}
    }

       
    if (remtab) {			/* if we're removing this uid */
	for (i = 0; i < remcount; ++i) { /* get rid of all the entries we saw */
	    remove_entry(uid, &remtab[i]);
	}
	t_free(remtab);
    }
    pthread_mutex_unlock(&notifytab_lock);
     
    if (reset) {			/* reset message tosses all sticky notifies */
	pthread_mutex_lock(&stickytab_lock);    
	sticky_clear(uid, -1);		
	pthread_mutex_unlock(&stickytab_lock);    
    }
    
    return sent;
}

/* notify_one --

    Called by "do_notify" to construct & send a single notification.
    
    --> notifytab_lock seized <--
*/

void notify_one(long typ, long id, notifydat data, long len, notifybuck *bp, int i) {

    req		*reqp;			/* notification request */
    atphdr	*atp;			/* within it, ptr to ATP header */
    nothdr	*notp;			/* within it, notification header */
        
    reqp = mallocf(sizeof(req)); 	/* get request buffer */
    
    reqp->clientaddr = bp->entry[i].regaddr;	/* client's address */
    reqp->uid = bp->entry[i].uid;	/* and uid */
    reqp->tid = next_tid();		/* assign a transaction id */		
    reqp->reqtime = reqp->rtxtime = time(NULL);	/* timestamp it */
					    
    /* construct the ATP packet */
    reqp->pkt.ddptype = DDP$ATP; 	/* set the DDP type */
    atp = (atphdr *) &reqp->pkt.ddpdata;
    atp->cmd = ATP_TREQ | ATP_XO;	/* ATP exactly-once request */
    atp->bitseq = 0x01;			/* we expect just 1 packet back */
    putnetshort(atp->tid, reqp->tid);	/* copy transaction id */
    strncpy(atp->userbytes, N_NOTIFY, 4); /* identify this */

    notp = (nothdr *) atp->atpdata;	/* locate notification part of pkt */
    putnetlong(notp->typ, typ);		/* fill it in */
    putnetlong(notp->uid, bp->entry[i].uid);
    putnetlong(notp->id, id);
    bcopy(data, notp->data, len);	/* copy variable-length data */
    
    /* compute length of DDP data */
    reqp->pktl = ATPHDRLEN + NOTHDRLEN + len;
    
    pthread_mutex_lock(&req_lock);	/* seize request queue */
    req_link(&rtxreq, reqp);		/* queue for possible retransmission */
    atpwrite(&regskt, &reqp->pkt, reqp->pktl, &reqp->clientaddr); /* send notification */
    pthread_mutex_unlock(&req_lock);	
}

/* notifylisten --

    Set up a socket listening for notify connections.  When one arrives,
    accept it and spawn a thread to deal with it.
*/

void notifylisten (any_t zot) {

    int			fd;	/* socket we listen on */
    struct sockaddr_in	sin;	/* its addr */
    struct servent	*sp;	/* services entry */
    int			connfd; /* new connection */
    int			len = sizeof(sin); /* addr length */
    int			on = 1;	/* for setsockopt */
    notifystate		*state = NULL; /* connection state */
    pthread_t		thread;
  
    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
      
    pthread_mutex_lock(&herrno_lock);
    if ((sp = getservbyname(NOTIFYPORT, "tcp")) == NULL) {
	t_errprint_s("notifylisten: unknown service: %s\n", NOTIFYPORT);
	exit(1);
    }    
    pthread_mutex_unlock(&herrno_lock);
    
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	t_perror("notifylisten: socket: ");
	exit(1);
    }
    
    /* set REUSEADDR so we won't get an EADDRINUSE if there are connections
       from our previous incarnation lingering in TIME_WAIT */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) < 0)
	t_perror("notifylisten: setsockopt (SO_REUSEADDR)");
	
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = sp->s_port;	/* port we listen on */
    
    if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	t_perror("notifylisten: bind");
	exit(1);
    }

    listen(fd, 5);			/* accept connections (up to 5 in advance) */
        
    for (;;) {				/* now just keep accepting */
    
	if (!state)			/* get state block */
	    state = mallocf(sizeof(notifystate));
	    
	connfd = accept(fd, (struct sockaddr *) &state->remoteaddr, &len);
	
	if (connfd < 0) {
	    t_perror("notifylisten: accept");
	    continue;			/* try again */
	}	

	t_fdopen(&state->conn, connfd);		/* set up t_file for the conn */

	if (pthread_create(&thread, pthread_attr_default,
			(pthread_startroutine_t) notify_tcp, (pthread_addr_t) state) < 0) {
	    t_perror("notify_tcp pthread_create failed");
	    exit(1);
	}			
	pthread_detach(&thread);
		
	state = NULL;			/* it's their state now */
    }
}

/* notify_tcp --

    Thread to handle a single TCP notify connection.
    
*/

void notify_tcp(any_t state_) {

    notifystate	*state;			/* state variables */
    
    state = (notifystate *) state_;
 
    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
   
    state->done = FALSE;
    state->validating = state->validated = FALSE;
    state->dnd = NULL;
    
    t_fprintf(&state->conn, "%d %s Notification server ready.\r\n",
    			    NOT_GREET, m_fullservname);
			    
    while (!state->done) {
    
	t_fflush(&state->conn);		/* write last response */
	
	t_fseek(&state->conn, 0, L_INCR); /* set up to read next command */
	
	if (t_gets(state->comline, sizeof(state->comline), &state->conn) == NULL)
	    break;			/* connection lost */

	if (strncasecmp(state->comline, "CLEAR", 5) == 0)
	    not_clear(state);		
	else if (strncasecmp(state->comline, "NOOP", 4) == 0)
	    not_noop(state);
	else if (strncasecmp(state->comline, "NOTIFY", 6) == 0)
	    not_notify(state);		
	else if (strncasecmp(state->comline, "PASE", 4) == 0)
	    not_pass(state);
	else if (strncasecmp(state->comline, "PASS", 4) == 0)
	    not_pass(state);
	else if (strncasecmp(state->comline, "QUIT", 4) == 0)
	    not_quit(state);	    
	else if (strncasecmp(state->comline, "USER", 4) == 0)
	    not_user(state);	    
	else
	    t_fprintf(&state->conn, "%d Unknown command: %s.\r\n", NOT_BADCMD, state->comline);
	
	/* unless in middle of validating, lose dnd connection */
	if ((!state->validating || state->done) && state->dnd) {
	    t_dndfree(state->dnd);
	    state->dnd = NULL;
	}
    }
    
    t_fflush(&state->conn);		/* flush any last output */
    
    if (state->conn.fd >= 0)  		/* t_fclose would try to free... */
	close(state->conn.fd);		/* ...so close by hand */
        
    return;				/* and end the thread */
    
}
/* not_clear --

    Clear notification.  Command format is:
    
        CLEAR <uid>,<type>	
*/

void not_clear(notifystate *state) {

    char	*p;
    long 	uid;			/* destination uid */
    long	type;			/* notification type */
    
    p = state->comline + strlen("CLEAR");
    if (*p++ != ' ')
	goto BADARG;
    p = strtonum(p, &uid);
     if (*p++ != ',')
	goto BADARG;   
    p = strtonum(p, &type);
     if (*p)
	goto BADARG;    
	
    pthread_mutex_lock(&stickytab_lock);    
    sticky_clear(uid, type);		/* remove sticky notification */
    pthread_mutex_unlock(&stickytab_lock);
 
    t_fflush(&state->conn);
    t_fprintf(&state->conn, "%d Notification cleared.\r\n", NOT_OK);
    return;
    
BADARG:
    t_fflush(&state->conn);
    t_fprintf(&state->conn, "%d Bad argument: %s\r\n", NOT_BADARG, p);
    return;

}

/* not_noop --

    
*/

void not_noop(notifystate *state) {

    t_fflush(&state->conn);		/* get set to write */
    t_fprintf(&state->conn, "%d Nothing.\r\n", NOT_OK);
    
}

/* not_notify --

    Send notification.  Command format is:
    
        NOTIFY <len>,<uid>,<type>,<notification id>,<sticky>
	<len bytes of data>
	
*/

void not_notify(notifystate *state) {

    char	*p;
    long	len;			/* length of variable part of notification */
    long 	uid;			/* destination uid */
    long	type;			/* notification type */
    long	notid;			/* and id */
    long	sticky;			/* sticky notification? */
    notifydat	data;			/* variable-length data */
    int		sent;			/* # of clients notification sent to */
    int		i;
    
    p = state->comline + strlen("NOTIFY");
    if (*p++ != ' ')
	goto BADARG;
    p = strtonum(p, &len);
     if (*p++ != ',')
	goto BADARG;
    p = strtonum(p, &uid);
     if (*p++ != ',')
	goto BADARG;   
    p = strtonum(p, &type);
     if (*p++ != ',')
	goto BADARG;    
    p = strtonum(p, &notid);
     if (*p++ != ',')
	goto BADARG;    
    p = strtonum(p, &sticky);
     if (*p)
	goto BADARG;    
	
    /* verify length (must fit in a packet) */
    if (len > NOTDATAMAX - NOTHDRLEN) {
	for (i = 0; i < len; ++i)	/* eat input before writing */
	    t_getc(&state->conn);
	goto BADARG;
    }
    
    t_fread(&state->conn, data, len);	/* read the notification */

    /* send to all clients */
    sent = do_notify(uid, type, notid, data, len, 0, sticky);
    
    t_fflush(&state->conn);
    t_fprintf(&state->conn, "%d %d\r\n", NOT_OK, sent);
    return;
    
BADARG:
    t_fflush(&state->conn);
    t_fprintf(&state->conn, "%d Bad argument.\r\n", NOT_BADARG);
    return;

}
/* not_pass --

    Complete validation:  send password to DND, check results.
    Note that we accept either plaintext or encrypted pw; t_dndval2
    will figure it out.
*/

void not_pass(notifystate *state) {

    char	*p;
    int		dndstat;
    dndresult	*dndres;
    char	*pw;

    t_fflush(&state->conn);		/* get set to write */

    if (!state->validating) {
	t_fprintf(&state->conn, "%d Specify user first.\r\n", NOT_BADSEQ);
	return;
    }
    
    state->validating = FALSE;		/* no longer awaiting pw */
    
    if (strlen(state->comline) < 6) {
	t_fprintf(&state->conn, "%d Missing password.\r\n", NOT_BADARG);
	return;
    }
    pw = state->comline + 5;		/* skip command & space */
    
    dndstat = t_dndval2(state->dnd, pw, val_farray, &dndres);
					    
    if (dndstat != DND_OK) {		/* did it work? */
	if (dndstat == DND_BADPASS)
	    t_fprintf(&state->conn, "%d Wrong password.\r\n", NOT_BADUSER);
	else
	    t_fprintf(&state->conn, "%d DND not available.\r\n", NOT_NODND);
	return;	
    }
    
    p = t_dndvalue(dndres, "DUP", val_farray);	/* is this user devalidated? */
    if (*p) {
	t_fprintf(&state->conn, "%d No such user.\r\n", NOT_BADUSER);
	return;	   			/* reject them */
    }
    
    /* == check PERM field, if desired == */
    
    state->validated = TRUE;		/* they checked out */

    t_fprintf(&state->conn, "%d User validated.\r\n", NOT_OK);
    
}

/* not_quit --

    Print farewell, mark connection as closing.
*/

void not_quit(notifystate *state) {

    t_fflush(&state->conn);		/* get set to write */
    t_fprintf(&state->conn, "%d Bye now!\r\n", NOT_BYE);
    state->done = TRUE;
    
}

/* not_user --

    Accept user name (or #uid) for validation.
*/

void not_user(notifystate *state) {

    char 	randnum[25];			 
    int		dndstat;   
    char 	*name = state->comline + 4; /* skip cmd */

    t_fflush(&state->conn);		/* get set to write */

    if (state->validating || state->validated) {
	t_fprintf(&state->conn, "%d User already specified.\r\n", NOT_BADSEQ);
	return;
    } 

    /* check name, get random number */
    dndstat = t_dndval1(&state->dnd, name, val_farray, randnum);
    
    if (dndstat == DND_CONTINUE) {	/* so far so good? */
	t_fprintf(&state->conn, "%d %s\r\n", NOT_NEEDPW, randnum); 
	state->validating = TRUE;	/* expect pw next */
    } else if (dndstat == DND_AMBIG || dndstat == DND_PERM || dndstat == DND_NOUSER)
	t_fprintf(&state->conn, "%d Bad user name.\r\n", NOT_BADUSER); 
    else 				/* lost connection etc. */
	t_fprintf(&state->conn, "%d DND not available.\r\n", NOT_NODND); 
    
}
/* nbp_register --

    Send an AppleTalk packet to the NBP socket to establish the
    registration.
*/

void nbp_register(char *object, char *type, ddpsockp ddpsock) {

    nbpinfo *np; 		/* registration packet & control info */
    nbphdr *reghdr;		/* nbp registration header */
    char *p;
    pthread_t	thread;

    np = (nbpinfo *) mallocf(sizeof(nbpinfo));
    
    np->regpkt.ddptype = DDP$NBP_REG;	/* set ddp type */

    reghdr = (nbphdr *) np->regpkt.ddpdata;
    reghdr->typecount = NBP_TYPECOUNT(NBP$REG, 1); /* register; one name per packet */
    reghdr->id = 0;			/* enumerator isn't relevant */

    p = np->regpkt.ddpdata + NBPHDRLEN;	/* next, construct reply tuple */
    p = putnetshort(p, my_atnet);	/* our net (static) */
    *p++ = my_atnode; 			/* our node (static) */
    *p++ = ddpsock->socknum; 		/* socket that owns the name */
    *p++ = 1;				/* alias enumerator */

    /* ok, the name follows */
    *p = strlen(object);		/* nbp object name */
    strcpy(p+1, object);
    p += *p + 1;
    *p = strlen(type);			/* nbp type name */
    strcpy(p+1, type);
    p += *p + 1;
    *p++ = strlen(my_atzone);
    strcpy(p, my_atzone);
    p += *p + 1;

    np->len = p - np->regpkt.ddpdata; 	/* data length */

    np->ddpsock = ddpsock;		/* socket to send packet from */

    if (pthread_create(&thread, pthread_attr_default,
                   (pthread_startroutine_t) nbp_reg_xmit, (pthread_addr_t) np) < 0) {
	t_perror("nbp_reg_xmit pthread_create failed");
	exit(1);
    }
    pthread_detach(&thread);
}

/* nbp_reg_xmit --

    Transmit registration packet to NBP daemon on this host.  The packet is retransmitted
    periodically in case the daemon crashes. No provision is presently made for removing
    a registration; if we wanted to do that, the thing to do would be to set a flag
    in the "nbpinfo" struct which this thread would see.
*/

void nbp_reg_xmit(pthread_addr_t _np) {

    nbpinfo	*np;		/* packet to send & socket to use */
    ataddr	nbpaddr;	/* nbp server */
    
    np = (nbpinfo *) _np;
    
    nbpaddr.at_net = my_atnet;	/* address of this host's NBP server */
    nbpaddr.at_node = my_atnode;
    nbpaddr.at_sock = SOCK$NIS;
    
    for (;;) {
	ddpwrite(np->ddpsock, &np->regpkt, np->len, nbpaddr);
	sleep(60);
    }
}

/* listener --

    Accept registration requests (which are ATP transactions).
    We maintain a list of all outstanding transactions and filter
    duplicates (the client is _required_ to use xo mode).  Duplicate
    requests are ignored if the response isn't ready yet (i.e., the
    client's rtx timer is too fast), else they cause the response to
    be retransmitted.

    Incoming requests are added to the "newreq" queue; the thread
    that owns the DND connection takes requests off this queue and
    processes them (generating and sending the ATP response.)  The
    completed request is saved on the "oldreq" queue, in case
    retransmission is required.

    The same ATP socket is also used for transmitting the notification
    transactions themself; we receive ATP responses from clients
    confirming receipt of the notification. 
    
    Note that two copies of this task are spawned: one for DDP and
    one for UDP.
*/

void listener(any_t family_) {

    int		family = (int) family_;	/* get right type */
    ddpbuf pkt;				/* packet buffer */
    int pktl;				/* and length (w/o DDP header) */
    atphdr *atp;					
    atpaddr clientaddr;			/* address request came from */
    struct sockaddr_in	sin;		/* addr of our UDP port */
    struct servent	*sp;		/* services entry */

    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();

    switch(family) {
    	case AF_APPLETALK:
	    if ((regskt.ddp = ddpopen(0)) == NULL) /* get a socket */	
		fatal();			/* or die trying... */
	
	    /* register ourselves with NBP daemon */
	    nbp_register(m_fullservname, NOTIFYNAME, regskt.ddp);
	    
	    break;
	case AF_INET:
	    pthread_mutex_lock(&herrno_lock);
	    if ((sp = getservbyname(NOTIFYNAME, "udp")) == NULL) {
		t_errprint_s("listen: unknown UDP service: %s\n", NOTIFYNAME);
		fatal();
	    }        
	    if ((regskt.udp = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		t_perror("listen: socket: ");
		fatal();
	    }
	    pthread_mutex_unlock(&herrno_lock);
   	
	    sin.sin_addr.s_addr = INADDR_ANY;
	    sin.sin_port = sp->s_port;	/* port we listen on */
	    
	    if (bind(regskt.udp, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
		t_perror("listen: bind");
		fatal();
	    }
	
	    break;
	default:
	    t_errprint_l("listener: unknown address family: %ld", family);
	    return;
    }

    for (;;) { 				/* loop getting packets */

	/* read next packet */
	clientaddr.family = family; 	/* read from the right socket */
	pktl = atpread(&regskt, &pkt, &clientaddr);

	if (pktl < ATPHDRLEN) {		/* skip bad packets */
	    pkterr("Ignore short ATP packet from", &clientaddr);
	    continue;
	}
	if (pkt.ddptype != DDP$ATP) {
	    pkterr("Ignore non-ATP packet", &clientaddr);
	    continue;
	}

	atp = (atphdr *) &pkt.ddpdata;	/* locate the atp header */
	
	switch(atp->cmd & ATP_CMDMSK) {	/* branch on atp command */
	    case ATP_TREL: 		/* transaction release? */
		trel(atp, &clientaddr); /* process that */
		break;

	    case ATP_TREQ:		/* a request? */
		if (!dupreq(atp, &clientaddr)) { /* filter duplicates */
		    if (atp->cmd & ATP_XO) /* IGNORE non-xo */
			new_request(&pkt, pktl, atp, &clientaddr); 
		    else
			pkterr("Ignore non-XO request", &clientaddr);
		}
	  	break;
		
	    case ATP_TRES:		/* response to our request */
	    	tres(atp, &clientaddr);	/* process it */
		break;
		
	    default: 			/* ignore anything else */
	    	pkterr_i("Unknown ATP cmd %02x", atp->cmd, &clientaddr);
		break;

	}
    } /* for (;;) */
}

/* trel --

    Handle transaction release.  Check our list of completed requests for
    the given transaction, remove it if found.

*/
void trel (atphdr *atp, atpaddr *clientaddr) {

    req 	*p;
    
    pthread_mutex_lock(&req_lock);
    
    
    if (p = req_find(&oldreq, atp, clientaddr)) { /* (sic) */
	req_unlink(&oldreq, p);		/* found it; unqueue */
	free(p);    
    } else
    	;				/* ignore stray trel */
        
    pthread_mutex_unlock(&req_lock);
}

/* tres --

    Handle transaction response.  Check our list of pending notification
    transactions; if found remove the transaction from the retransmit
    queue and send a transaction release.
    
    Check status (first 4 bytes of data; 0 = ok).  If client gave bad status, it means
    notifications for this user are no longer welcome at that addres; remove
    entry from table.
*/

void tres (atphdr *atp, atpaddr *clientaddr) {

    req 	*p;
    bit32	sta;
    
    pthread_mutex_lock(&req_lock);    
    
    if (p = req_find(&rtxreq, atp, clientaddr)) { /* (sic) */
	req_unlink(&rtxreq, p);		/* found it; unqueue */	
	pthread_mutex_unlock(&req_lock);	/* unlock before locking notifytab */

	/* check status */
	sta = getnetlong(atp->atpdata);
	if (sta == NC_NOUSER) {		/* if notification rejected */
	    pthread_mutex_lock(&notifytab_lock);	/* remove them from table */
	    remove_entry(p->uid, &p->clientaddr);	
	    pthread_mutex_unlock(&notifytab_lock);	
	}
	
        atp = (atphdr *) &p->pkt.ddpdata; /* locate request atp header */
        atp->cmd = ATP_TREL;            /* make this a transaction release */
        p->pktl = ATPHDRLEN;      	/* just header */
	atpwrite(&regskt, &p->pkt, p->pktl, &p->clientaddr); /* send TREL */
	free(p);			/* done with this request */    
	

    } else {				/* ignore stray tres */
    	pthread_mutex_unlock(&req_lock);		
	pkterr_i("Stray response TID %d", getnetshort(atp->tid), clientaddr);
    }
    
}

/* dupreq --

    Check for duplicate request.  Check both request queues (pending and
    already processed).  If the request is still pending, this is just
    an over-anxious retransmission (ignore it).  If the request has
    completed, the response packet may have been lost, so it is re-sent.
    In either case, the request timestamp is updated.

*/
boolean_t dupreq (atphdr *atp, atpaddr *clientaddr) {

    req 		*reqp;
					        
    pthread_mutex_lock(&req_lock);
    
    if (reqp = req_find(&newreq, atp, clientaddr)) {	/* (sic) */
	reqp->reqtime = time(NULL);	/* update request time */
	pthread_mutex_unlock(&req_lock);
	return TRUE;			/* ignore duplicate request */
    }

    if (reqp = req_find(&oldreq, atp, clientaddr)) {	/* (sic) */
	reqp->reqtime = time(NULL);	/* update request time */
	atpwrite(&regskt, &reqp->pkt, reqp->pktl, &reqp->clientaddr); /* resend reply */
	pthread_mutex_unlock(&req_lock);
	return TRUE;			/* ignore duplicate request */
    }
    
    pthread_mutex_unlock(&req_lock);
    
    return FALSE;			/* not duplicate */
        
}

/* new_request --

    Queue up a registration request for attention by the dnd thread.
*/

void new_request(ddpbuf *pktp, int pktl, atphdr *atp, atpaddr *clientaddr) {

    req 	*reqp;		/* request info */

    reqp = mallocf(sizeof(req));

    /* copy data into req structure */
    reqp->clientaddr = *clientaddr;	
    reqp->tid = getnetshort(atp->tid);	/* addr/tid */
    reqp->pkt = *pktp;			/* copy entire packet */
    reqp->pktl = pktl;
    reqp->reqtime = time(NULL);		/* stamp the time */

    pthread_mutex_lock(&req_lock);		
    req_link(&newreq, reqp);		/* add request to end of queue */
    pthread_cond_signal(&req_wait); 	/* nudge the thread that serves the queue */
    pthread_mutex_unlock(&req_lock);
    
}

/* req_find --

    Search request queue for request with same client addr & transaction id.
    
    --> req_lock seized <--

*/

req *req_find(reqq *q, atphdr *atp, atpaddr *clientaddr) {

    req		*p;			/* returned: req pointer */
    
    for (p = q->head; p; p = p->flink) {
	if (p->clientaddr.addr == clientaddr->addr
		    && p->clientaddr.family == clientaddr->family
		    && p->tid == getnetshort(atp->tid)) 
	    return p;
    }
    
    return NULL;
}

/* req_link --

    Add request to queue.
    
    --> req_lock seized <--

*/

void req_link(reqq *q, req *p) {

    if (q->tail == NULL)		/* empty? */
	q->head = p;			/* yes- new one is first */
    else 				
	q->tail->flink = p;		/* link from old tail */
    
    p->flink = NULL;			/* new one is last */	
    p->blink = q->tail;			/* link to old tail */
	
    q->tail = p;
}

/* req_unlink --

    Remove request from queue.
    
    --> req_lock seized <--

*/

void req_unlink(reqq *q, req *p) {

    if (p == q->head) 	
	q->head = p->flink;		/* new head */
    else 
	p->blink->flink = p->flink;	/* relink around us */
    
    if (p == q->tail) 
	q->tail = p->blink; 		/* new tail */
    else
	p->flink->blink = p->blink;	/* relink around us */	
	
    p->flink = p->blink = NULL;		/* sppml */
}

/* process_requests --

    Thread to process ATP requests from clients.  Parse the request
    packet, construct an ATP response and send it.
    
    Since duplicate filtration works only on requests that are in
    one of the the queues, leave the request in the newreq
    queue until it's ready to be moved into the oldreq queue.
    
    --> only this thread is allowed to unlink from newreq <--
*/

void process_requests(any_t zot) {
					
    req		*reqp;			/* current request */
    atphdr	*atp;			/* atp request header */
    sta_typ	stat;			/* return status */
    

    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
    	
    for (;;) {
    
	pthread_mutex_lock(&req_lock);		
	
	while(newreq.head == NULL)	/* wait for a request */
	    pthread_cond_wait(&req_wait, &req_lock); 	
    
    	reqp = newreq.head;		/* peek at first one (still linked) */
	
	pthread_mutex_unlock(&req_lock);/* done with the input queue */
	
	atp = (atphdr *) &reqp->pkt.ddpdata; /* locate the atp header */
	
	/* userbytes identify request type */
	if (strncmp(atp->userbytes, N_REGISTER, 4) == 0)
	    stat = reg_request(reqp, atp); /* registration request */
	else if (strncmp(atp->userbytes, N_CLEAR, 4) == 0)
	    stat = clear_request(reqp, atp); /* clear request */
	else
	    stat = N_BADREQ;		/* unknown request type */
	    
	/* rewrite packet to make it a response */	
	
        atp->cmd = ATP_TRES;            /* make this a transaction response */
        atp->bitseq = 0;                /* this is the 0th... */
	atp->cmd |= ATP_EOM;		/* ...and last reply */
        reqp->pktl = ATPHDRLEN + sizeof(sta_typ);       /* header + status longword */
	putnetlong(atp->atpdata, stat); /* copy in net byte order */
	
	/* send the reply */
	atpwrite(&regskt, &reqp->pkt, reqp->pktl, &reqp->clientaddr); 

	pthread_mutex_lock(&req_lock);	/* now, move req to old queue */
	req_unlink(&newreq, reqp);
	req_link(&oldreq, reqp);
	pthread_mutex_unlock(&req_lock);			    
	
    }
}

/* reg_request --

    Handle registration requests.  Parse the request
    packet, check the DND for the given name/uid, construct an
    ATP response and send it.
    
    Any sticky notifications for this uid are re-sent (only to
    the client that just registered, not to all clients for this uid.)
*/

sta_typ reg_request(req	*reqp, atphdr *atp) {

    sta_typ	stat;			/* returned: response status */
    char	name[256];		/* name/uid to register */
    long	uid;			/* uid it resolves to */
    atpaddr	regaddr;		/* address they're registering */
    ataddr	*at_regaddr;		/* appletalk version of it */
    char	*p;
    int		i;
    notif	*sticky;		/* sticky notifications */
    stickybuck	*bp;			/* entry for this uid */
    int		pos;			/* '' */
    servtab	servcode;		/* services they're registering for */
    
    /* request format is:
	    uid/name		-	pstring
	AT:    socket		-	byte
   -or- UDP:   port		-	short
	    serviceCount	-	long
	    serviceCodes	-	<n> longs
    */
    
    /* Do sanity check on packet length */
    if (1 + *((u_char *) atp->atpdata) + 1 + 4 > reqp->pktl - ATPHDRLEN) 
	return N_BADREQ;
    
    p = pstrcpy(name, atp->atpdata); /* get name */
    
    regaddr = reqp->clientaddr;		/* address & family are same used in ATP request */
    if (regaddr.family == AF_APPLETALK) {
    	at_regaddr = (ataddr *) &regaddr.addr;
	at_regaddr->at_sock = *((u_char *) p++); /* AT: they specify socket */
	regaddr.port = 0;		/* set port to 0 in this case */
    } else {
	regaddr.port = getnetshort(p); p += 2;/* INET: pick up 2-byte port # */
	regaddr.port = htons(regaddr.port); /* keep in network byte order */
    }
    
    servcode.count = getnetlong(p); p += 4;	/* get number of service codes */
    
    if (servcode.count * 4 + (p - atp->atpdata) > reqp->pktl - ATPHDRLEN) 
	return N_BADREQ;			/* servcount extends beyond pkt */

    if (servcode.count > SERVMAX) servcode.count = SERVMAX;
    
    for (i = 0; i < servcode.count; ++i) {	/* copy each service code */
	servcode.serv[i] = getnetlong(p); p += 4;
    }
    
    /* create/update table entry */
    stat = update_entry(name, &uid, &regaddr, servcode);	
	
    if (stat == N_OK) {			
	pthread_mutex_lock(&stickytab_lock);
	if (sticky_find(uid, &bp, &pos)) {	/* re-send any sticky notifications */
	    for (sticky = bp->entry[pos].not; sticky; sticky = sticky->flink) {
		(void) do_notify(uid, sticky->typ, sticky->id, /* to this client only */
			       sticky->data, sticky->len, &regaddr, FALSE);
	    }
	}
	pthread_mutex_unlock(&stickytab_lock);
    }
    
    return stat;
}	

/* clear_request --

    Clear "sticky" notification for given uid/service.
    
*/

sta_typ clear_request(req *reqp, atphdr *atp) {

    long	uid;
    long	type;
    				
    /* request format:  <uid (4)> <service (4)> */
    if (reqp->pktl != 4 + 4 + ATPHDRLEN)
	return N_BADREQ;		/* verify length */
	
    uid = getnetlong(atp->atpdata);	/* pick up uid */
    type = getnetlong(atp->atpdata+4);	/* and type */
    
    pthread_mutex_lock(&stickytab_lock);    
    sticky_clear(uid, type);		/* remove sticky notification */
    pthread_mutex_unlock(&stickytab_lock);
    
    return N_OK;			
}

/* update_entry --

    Update/create registration table entry (in response to ATP request.)
    Check with the DND to make sure they're supposed to be talking to us,
    return a bad status if not.  If the "name" is given in uid format,
    we try to optimize by checking our table first (before calling the
    DND).
*/ 

sta_typ update_entry(char *name, long *uid, atpaddr *regaddr, servtab servcode) {

    notifybuck	*bp;			/* notification table bucket */
    int		pos;			/* and offset */
    dndresult	*dndres = NULL;		/* dnd info */
    sta_typ	stat;			/* status from lookup */
    char	notifyserv[256];	/* server hostname */
    char	*p;
    char	buf[256];
    static char *farray[] = { "UID", "NOTIFYSERV", NULL };
    ataddr	*at_regaddr;
					
    if (name[0] == '#') {		/* are they giving a uid? */
	strtonum(name+1, uid);		/* yes - maybe we can bypass DND */
	
	pthread_mutex_lock(&notifytab_lock);	/* get access to the table */
	if (find_entry(*uid, &bp, regaddr, &pos)) {
	    bp->entry[pos].time = time(NULL); 	/* update the timer */
	    bp->entry[pos].servcode = servcode;/* update service list */
	    notifytab_dirty = TRUE;	/* table has changed */
	    pthread_mutex_unlock(&notifytab_lock);
	    return N_OK;		/* nothing more to do */
	}
	
	pthread_mutex_unlock(&notifytab_lock);
    }
    
    /* talk to dnd to resolve name & check NOTIFYSERV value */
    
    stat = t_dndlookup1(name, farray, &dndres);
    
    if (stat == DND_OK) {
	strtonum(t_dndvalue(dndres, "UID", farray), uid);
	strcpy(notifyserv, t_dndvalue(dndres, "NOTIFYSERV", farray));
	if (p = index(notifyserv, '@'))	/* (sic) */
	    *p = 0;			/* chop off the zone */
	if (strcasecmp(notifyserv, m_fullservname) != 0)
	    stat = N_WRONGSERV;		/* we're the wrong server for them */
	else {
	    pthread_mutex_lock(&notifytab_lock);
	    if (find_entry(*uid, &bp, regaddr, &pos)) {
		bp->entry[pos].time = time(NULL);  /* already there; update timer */
		bp->entry[pos].servcode = servcode;/* update service list */
	    } else			/* add them to table */
		make_entry(*uid, regaddr, time(NULL), servcode); 
	    notifytab_dirty = TRUE;	/* table has changed */
	    pthread_mutex_unlock(&notifytab_lock);
	    stat = N_OK;		/* set good status */
	    if (regaddr->family == AF_APPLETALK) {
		at_regaddr = (ataddr *)&regaddr->addr;
		t_sprintf(buf, "New entry %ld/%ld/%ld for uid %ld\n",
			    (long) at_regaddr->at_net,
			    (long) at_regaddr->at_node,
			    (long) at_regaddr->at_sock,
			    *uid);
	    } else {
		pthread_mutex_lock(&inet_ntoa_lock);
		t_sprintf(buf, "New entry %s %ld for uid %ld\n",
			    inet_ntoa(*((struct in_addr *) &regaddr->addr)),
			    (long) ntohs(regaddr->port),
			    *uid);	    
		pthread_mutex_unlock(&inet_ntoa_lock);
	    }
	    t_syslog(LOG_DEBUG, buf);
	}
    } else if (stat == DND_DOWN)
	stat = N_NODND;			/* special bad status if no dnd */
    
    if (dndres)
	free(dndres);
        
    return stat;
}

/* find_entry --

    Search notification table for given uid & address.

    --> notifytab_lock seized! <--
*/

boolean_t find_entry(long uid, notifybuck **bp, atpaddr *clientaddr, int *i) {

    int hash;			/* bucket number */

    if (uid < 0)
	return FALSE;		/* negative uid's are invalid */
	
    hash = uid % NTABSIZE;	/* choose the proper list */

    /* check each bucket in the list */
    for (*bp = notifytab[hash]; *bp; *bp = (*bp)->flink) {
	for (*i = 0; *i < (*bp)->count; *i += 1) {
	    if ((*bp)->entry[*i].uid == uid &&
	         ATP_ADDR_EQ(&(*bp)->entry[*i].regaddr, clientaddr))
			return TRUE; /* uid & address & family match */
	}
    }

    return FALSE;		/* not found */
}

/* make_entry --

    Create notification table entry for given uid & address.

    --> notifytab_lock seized! <--
*/

void make_entry(long uid, atpaddr *regaddr, long when, servtab servcode) {

    int 	hash;		/* bucket number */
    notifybuck  *bp;		/* bucket */
    int		i;		/* and offset */

    hash = uid % NTABSIZE;	/* choose the proper list */

    /* search list for bucket with room */
    for (bp = notifytab[hash]; bp && bp->count == BUCKSIZE; bp = bp->flink) 
    	;
	
    if (!bp) {			/* no partial bucket */
	bp = mallocf(sizeof(notifybuck));
	bp->count = 0;		/* make an empty one */
	if (bp->flink = notifytab[hash]) /* (sic) */ 
	    notifytab[hash]->blink = bp;
	bp->blink = NULL;	
	notifytab[hash] = bp;	/* insert in front */
    }
    
    i = bp->count++;		/* take the next entry */
    bp->entry[i].uid = uid;	/* and fill it in */
    bp->entry[i].regaddr = *regaddr;
    bp->entry[i].time = when;
    bp->entry[i].servcode = servcode;	
    
    notifytab_dirty = TRUE;	/* table has changed */
}

/* remove_entry --

    Remove notification table entry for given uid & address.

    --> notifytab_lock seized! <--
*/

boolean_t remove_entry(long uid, atpaddr *regaddr) {

    notifybuck	*bp;			/* table bucket */
    int		i;			/* and offset */
    int		hash;
    char	buf[256];
    ataddr	*at_regaddr;
    
    if (!find_entry(uid, &bp, regaddr, &i))
	return FALSE;			/* no entry to remove */

    at_regaddr = (ataddr *) &regaddr->addr;
    if (regaddr->family == AF_APPLETALK) {
	t_sprintf(buf, "Remove entry %ld/%ld/%ld for uid %ld\n",
		    (long) at_regaddr->at_net,
		    (long) at_regaddr->at_node,
		    (long) at_regaddr->at_sock,
		    uid);
    } else {
	pthread_mutex_lock(&inet_ntoa_lock);
	t_sprintf(buf, "Remove entry %s %ld for uid %ld\n",
		inet_ntoa(*((struct in_addr *) &regaddr->addr)),
		(long) ntohs(regaddr->port),
		uid);
	pthread_mutex_unlock(&inet_ntoa_lock);
    }  
    t_syslog(LOG_DEBUG, buf);
    
    if (i != bp->count-1) 		/* unless already last */
	bp->entry[i] = bp->entry[bp->count-1]; /* move last in */
	
    if (--bp->count == 0) {		/* if bucket now empty, free it */

	hash = uid % NTABSIZE;		/* choose the proper list */

	if (bp == notifytab[hash]) 
	    notifytab[hash] = bp->flink; /* new head */
	else 
	    bp->blink->flink = bp->flink; /* relink around us */
	
	if (bp->flink) 			/* if not last, fix backlink */
	    bp->flink->blink = bp->blink;	

	free(bp);
    }

    notifytab_dirty = TRUE;	/* table has changed */

    return TRUE;
}

/* sticky_find --

    Search sticky notification table for given uid.

    --> stickytab_lock seized! <--
*/

boolean_t sticky_find(long uid, stickybuck **bp, int *i) {

    int hash;			/* bucket number */

    hash = uid % NTABSIZE;	/* choose the proper list */

    /* check each bucket in the list */
    for (*bp = stickytab[hash]; *bp; *bp = (*bp)->flink) {
	for (*i = 0; *i < (*bp)->count; *i += 1) {
	    if ((*bp)->entry[*i].uid == uid)
		    return TRUE; /* uid matches */
	}
    }

    return FALSE;		/* not found */
}

/* sticky_set --

    Record notification in stickytab.  Search table for entry for this
    user, make one if necessary.  Search user's notification list for
    a sticky notification of this type; replace it if found, else add
    a new one.

    --> stickytab_lock seized! <--
*/

void sticky_set(long uid, long typ, long id, notifydat data, long len) {

    stickybuck		*bp;		/* table bucket */
    int			i;		/* and offset */
    notif		*sticky;	/* sticky notification record */
    
    stickytab_dirty = TRUE;		/* table is changed */

    if (!sticky_find(uid, &bp, &i))	/* locate user's entry */
	sticky_make(uid, &bp, &i);	/* ...or create one */
	
    /* search for existing notification of this type */
    for (sticky = bp->entry[i].not; sticky; sticky = sticky->flink) {
	if (sticky->typ == typ) {	/* found one; rewrite */
	    sticky->id = id;
	    bcopy(data, sticky->data, len);
	    sticky->len = len;
	    return;			/* done */
	}
    }
    
    /* not found; make new one & add to front */
    sticky = mallocf(sizeof(notif));
    sticky->blink = NULL;
    if (sticky->flink = bp->entry[i].not) /* (sic) */
	bp->entry[i].not->blink = sticky;
    bp->entry[i].not = sticky;
	
    sticky->id = id;			/* copy the notification info */
    sticky->typ = typ;
    bcopy(data, sticky->data, len);
    sticky->len = len;

    
}

/* sticky_clear --

    Clear notification from stickytab.  Remove & free the "notif" struct.
    The user entry is NOT removed from the hash table (on the theory that
    we're likely to have to put them back soon anyway).
    
    A negative type clears all entries for the given user.
            
    --> stickytab_lock seized! <--
*/

void sticky_clear(long uid, long typ) {

    stickybuck		*bp;		/* table bucket */
    int			i;		/* and offset */
    notif		*sticky, *next;	/* sticky notification record */

    if (sticky_find(uid, &bp, &i)) {
	for (sticky = bp->entry[i].not; sticky; sticky = next) {
	    next = sticky->flink;
	    if (typ < 0 || sticky->typ == typ) { /* found it */
		if (sticky->flink)
		    sticky->flink->blink = sticky->blink;
		if (sticky->blink)
		    sticky->blink->flink = sticky->flink;
		else
		    bp->entry[i].not = sticky->flink;
		free(sticky);
		stickytab_dirty = TRUE;	/* table has changed */
	    }
	}	
    }
}

/* sticky_make --

    Make new entry in stickytab, returning pointer to it.

    --> stickytab_lock seized! <--
*/

void sticky_make(long uid, stickybuck **bp, int *i) {

    int 	hash;			/* bucket number */

    hash = uid % NTABSIZE;		/* choose the proper list */

    /* search list for bucket with room */
    for (*bp = stickytab[hash]; *bp && (*bp)->count == BUCKSIZE; *bp = (*bp)->flink) 
    	;
	
    if (!*bp) {				/* no partial bucket */
	*bp = mallocf(sizeof(stickybuck));
	(*bp)->count = 0;		/* make an empty one */
	if ((*bp)->flink = stickytab[hash]) /* (sic) */ 
	    stickytab[hash]->blink = *bp;
	(*bp)->blink = NULL;	
	stickytab[hash] = *bp;		/* insert in front */
    }
    
    *i = (*bp)->count++;		/* take the next entry */
    (*bp)->entry[*i].uid = uid;		/* and fill it in */
    (*bp)->entry[*i].not = NULL;	/* no notifications saved yet */
					    
    stickytab_dirty = TRUE;		/* table has changed */
}

/* atp_periodic --

    Thread to handle periodic ATP tasks:  retransmit (and time out)
    notification treqs, and time out tres's that never receive a
    release.
        
    We presently use a very simple-minded fixed retransmission interval:
    notifications are such an infrequent event that it's not clear that
    any type of response time sampling to a given client is likely to
    be valid.  Because of the asynchronous nature of notifications (and
    because there's no sequencing to bring things to a halt if one
    transaction is temporarily delayed) very slow retransmission is viable.
    
    Time out notifications that haven't been delivered within ATP_TIMEOUT.
    Clients that don't respond are also removed from the registration
    table.
    
*/

void atp_periodic(any_t zot) {

    req		*reqp;			/* current request */
    req		*next;			/* next in queue */
    long	now;			/* time */
    struct gone {			/* list of unresponsive clients */
    	long	uid;	
	atpaddr	clientaddr;
    } 		*gonelist;	
    int		gonemax = 100;
    int		gonecount;
 
    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
   
    gonelist = mallocf(gonemax * sizeof(struct gone));
    	
    for (;;) {
    
	sleep(ATP_RTX_CHECK);		/* spin slowly */
	

	now = time(NULL);		/* current time */
	pthread_mutex_lock(&req_lock);	
	
	/* check oldreq for stale responses */
	for (reqp = oldreq.head; reqp; reqp = next) {
	    next = reqp->flink;		/* in case we free */
	    if (now - reqp->reqtime > ATP_TIMEOUT) { /* use phase II var timer? */
		req_unlink(&oldreq, reqp);
		free(reqp);		/* never got trel; discard response */
	    }
	}
	
	gonecount = 0;
	
	/* traverse the rtx queue retransmitting/timing out */
	for (reqp = rtxreq.head; reqp; reqp = next) {
	    next = reqp->flink;		/* in case we free */
	    if (now - reqp->reqtime > ATP_TIMEOUT) {
		if (gonecount == gonemax) {
		    gonemax += 100;	/* grow list if needed */
		    gonelist = reallocf(gonelist, gonemax * sizeof(struct gone));
		}
		gonelist[gonecount].uid = reqp->uid;
		gonelist[gonecount].clientaddr = reqp->clientaddr;
		gonecount++;		/* record unresponsive clients */
		req_unlink(&rtxreq, reqp);
		free(reqp);		/* timed out; discard it */
	    } else if (now - reqp->rtxtime > ATP_RTX_INT) {
		atpwrite(&regskt, &reqp->pkt, reqp->pktl, &reqp->clientaddr);
		reqp->rtxtime = now;	/* update xmit time */
	    }
	}
	
	pthread_mutex_unlock(&req_lock);
	
	/* now purge table of any clients that failed to respond */
	pthread_mutex_lock(&notifytab_lock);
	while(--gonecount >= 0) {
	    remove_entry(gonelist[gonecount].uid, &gonelist[gonecount].clientaddr);	
	}
	pthread_mutex_unlock(&notifytab_lock);
    }
}

/* writer --

    Thread to periodically write out notifytab and stickytab.    
    
*/

void writer(any_t zot) {

    for (;;) {
    	sleep(WRITE_INTERVAL);
	pthread_mutex_lock(&notifytab_lock);
	if (notifytab_dirty) 			/* if changed since last write */
	    write_notifytab();			/* rewrite the file copy */
	pthread_mutex_unlock(&notifytab_lock);

	pthread_mutex_lock(&stickytab_lock);
	if (stickytab_dirty) 			/* same for stickytab */
	    write_stickytab();			
	pthread_mutex_unlock(&stickytab_lock);

    }
   
}

/* write_notifytab --

    Write notifytab to a file, one line per entry:

    Two kinds of entries (AppleTalk & IP):
    
	<uid>,<net>/<node>/<socket>,<time>,<service>...
	<uid>,U<dotted-ip-addr> <udp port>,<time>,<service>...
    
    --> notifytab_lock seized <--
*/

void write_notifytab() {

    int		hash;			/* hash table index */
    notifybuck	*bp;			/* bucket pointer */
    int		i;			/* index in current bucket */
    int		j;			/* service code index */
    t_file	*f;			/* text file */
    char	tempname[FILENAME_MAX];
    ataddr	*at_regaddr;
    
    t_sprintf(tempname, "%s.temp", f_notifytab);		
    
    /* write everything to a new temp file */
    if ((f = t_fopen(tempname, O_WRONLY | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("write_notifytab: cannot open ", tempname);
	return;				/* leave dirty bit set */
    }

    for (hash = 0; hash < NTABSIZE; ++hash) {
	for (bp = notifytab[hash]; bp; bp = bp->flink) {
	    for (i = 0; i < bp->count; i++) {
		t_fprintf(f, "%ld,", bp->entry[i].uid);
		if (bp->entry[i].regaddr.family == AF_APPLETALK) {
		    at_regaddr = (ataddr *) &bp->entry[i].regaddr.addr;
		    t_fprintf(f, "%ld/%ld/%ld,",
			(long) at_regaddr->at_net,
			(long) at_regaddr->at_node,
			(long) at_regaddr->at_sock);
		} else {
		    pthread_mutex_lock(&inet_ntoa_lock);
		    t_fprintf(f, "U%s %ld,",
		    	inet_ntoa(*((struct in_addr *) &bp->entry[i].regaddr.addr)),
			(long) ntohs(bp->entry[i].regaddr.port));
		    pthread_mutex_unlock(&inet_ntoa_lock);
		}
		t_fprintf(f, "%ld", bp->entry[i].time);
		for (j = 0; j < bp->entry[i].servcode.count; ++j) 
		    t_fprintf(f, ",%ld", bp->entry[i].servcode.serv[j]);
		t_putc(f, '\n');
	    }
	}
    }
    
    t_fflush(f);
    
    if (f->t_errno) 			/* see if errors occured during any of that */
	t_perror1("write_notifytab: error writing ", tempname);
    else {				/* ok, replace the old file now */
	if (rename(tempname, f_notifytab) < 0)
	    t_perror1("write_notifytab: rename failed: ", f_notifytab);
	else
	    notifytab_dirty = FALSE;	/* safely written out! */
    }	
    
    t_fclose(f);    
}

/* read_notifytab --

    Initialize notifytab based on file contents.
    	
    --> notifytab_lock seized <--
*/

void read_notifytab() {

    t_file	*f;			/* input file */	
    char	line[256];		/* current entry from it */
    char	*p;			/* pointer into line */
    char	*space;
    long	l;
    long	uid;
    atpaddr	regaddr;		/* address registering now */
    ataddr	*at_regaddr;		/* AT version */
    long	when;
    servtab	servcode;

#define BADLINE	{ t_errprint_s("Ignore bad notifytab line: %s\n", line); continue; }

    if (!f_notifytab) {
	t_errprint("notifyd: Fatal config error: no NOTIFYTAB file defined.\n");
	exit(1);
    }
    
    if ((f = t_fopen(f_notifytab, O_RDONLY | O_CREAT, FILE_ACC)) == NULL) {
	t_perror1("read_notifytab: cannot open ", f_notifytab);
	return;				/* start with empty table */
    }
    
    /* read file line-by-line */
    while (t_gets(line, sizeof(line), f) != NULL) {
	p = strtonum(line, &uid);	/* get uid */
	if (*p++ != ',')
	    BADLINE;
	if (*p == 'U') {		/* UDP entry? */
	    ++p;
	    if ((space = index(p, ' ')) == NULL)
		BADLINE;
	    *space = 0;			/* chop at end of inet address */
	    regaddr.addr = inet_addr(p); /* parse dotted IP addr */
	    p = space + 1;
	    p = strtonum(p, &l); regaddr.port = htons((short) l);		
	    regaddr.family = AF_INET;
	} else {			/* no - default is DDP */
	    at_regaddr = (ataddr *) &regaddr.addr;
	    p = strtonum(p, &l); at_regaddr->at_net = l; /* note: host byte order */
	    if (*p++ != '/')
		BADLINE;
	    p = strtonum(p, &l); at_regaddr->at_node = l;
	    if (*p++ != '/')
		BADLINE;
	    p = strtonum(p, &l); at_regaddr->at_sock = l;
	    regaddr.family = AF_APPLETALK;
	    regaddr.port = 0;
	}
	if (*p++ != ',')
	    BADLINE;
	p = strtonum(p, &when);
	servcode.count = 0;		/* no services yet */
	while (*p == ',' && servcode.count < SERVMAX) {
	    ++p;
	    p = strtonum(p, &servcode.serv[servcode.count++]);
	}
	if (*p++ != 0)
	    BADLINE;	
	    
	/* old-format entries don't have service list, default to mail & bull */
	if (servcode.count == 0) {
	    servcode.serv[servcode.count++] = NTYPE_MAIL;
	    servcode.serv[servcode.count++] = NTYPE_BULL;    
	}
	make_entry(uid, &regaddr, when, servcode);
    }
    
    notifytab_dirty = FALSE;		/* don't need to write yet */

    t_fclose(f);

#undef BADLINE

}

/* write_stickytab --

    Write sticky notifications to file.  A semi-text format is used:
    each entry consists of a line of text, some bytes or (arbitrary)
    data, and final \n.
    
	<uid>,<id>,<type>,<len>\n
	<data>\n
	
	
    --> stickytab_lock seized <--
*/

void write_stickytab() {

    int		hash;			/* hash table index */
    stickybuck	*bp;			/* bucket pointer */
    int		i;			/* index in current bucket */
    notif	*sticky;		/* one notification */
    t_file	*f;	
    char	tempfile[FILENAME_MAX];	
    
    t_sprintf(tempfile, "%s.temp", f_stickytab);
    
    /* write everything to a new temp file */
    if ((f = t_fopen(tempfile, O_WRONLY | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("write_stickytab: cannot open ", tempfile);
	return;				/* leave dirty bit set */
    }

    for (hash = 0; hash < NTABSIZE; ++hash) {
	for (bp = stickytab[hash]; bp; bp = bp->flink) {
	    for (i = 0; i < bp->count; i++) {
	    	for (sticky = bp->entry[i].not; sticky; sticky = sticky->flink) {
		    t_fprintf(f, "%ld,%ld,%ld,%ld\n", bp->entry[i].uid,
		    		sticky->id, sticky->typ, sticky->len);
		    t_fwrite(f, sticky->data, sticky->len);
		    t_putc(f, '\n'); 
		}
	    }
	}
    }
    
    t_fflush(f);
    
    if (f->t_errno) 			/* see if errors occured during any of that */
	t_perror1("write_stickytab: error writing ", tempfile);
    else {				/* ok, replace the old file now */
	if (rename(tempfile, f_stickytab) < 0)
	    t_perror1("write_stickytab: rename failed: ", f_stickytab);
	else
	    stickytab_dirty = FALSE;	/* safely written out! */
    }	
    
    t_fclose(f);    
}

/* read_stickytab --

    Initialize stickytab based on file contents.
	
    --> stickytab_lock seized <--
*/

void read_stickytab() {

    t_file	*f;			/* input file */	
    char	line[NOTDATAMAX];	/* current entry from it */
    char	*p;			/* pointer into line */
    long	uid;
    long	id;
    long	typ;
    long	len;

#define BADLINE	{ t_errprint_s("Ignore bad stickytab line: %s\n", line); continue; }

    if (!f_stickytab) {
	t_errprint("notifyd: Fatal config error: no STICKYTAB file defined.\n");
	exit(1);
    }

    if ((f = t_fopen(f_stickytab, O_RDONLY | O_CREAT, FILE_ACC)) == NULL) {
	t_perror1("read_stickytab: cannot open ", f_stickytab);
	return;				/* start with empty table */
    }
    
    /* read file line-by-line */
    while (t_gets(line, sizeof(line), f) != NULL) {
	p = strtonum(line, &uid);	/* get uid */
	if (*p++ != ',')
	    BADLINE;
	p = strtonum(p, &id); 
	if (*p++ != ',')
	    BADLINE;
	p = strtonum(p, &typ);
	if (*p++ != ',')
	    BADLINE;
	p = strtonum(p, &len);
	if (*p)
	    BADLINE;
	t_fread(f, line, len);		/* read notification data */
	if (t_getc(f) != '\n')		/* should be followed by end of line */
	    BADLINE;
	sticky_set(uid, typ, id, line, len);	
    }
    
    stickytab_dirty = FALSE;		/* don't need to write yet */

    t_fclose(f);
}

/*
   fatal --

	Fatal error exit.  Save dump??
*/
void fatal () {

	exit(1);
}

/* next_tid --

    Generate next ATP transaction id (16 bit unsigned, wrap at top.)
    Use a "long" for the arithmetic (overflow paranoia.)
    
    The value is returned in host byte order.
*/

u_short next_tid() {
    
    u_short	result;
    
    pthread_mutex_lock(&tid_lock);
    tid = (tid + 1) & 0xFFFF;	/* 16 bit unsigned increment */
    result = tid;
    pthread_mutex_unlock(&tid_lock);
    
    return result;
}

/* atpread --

    Read ATP packet from DDP or UDP (according to "source.family").
    
    The "ddpbuf" structure is used in both cases, although the DDP
    header is meaningless in the UDP case.  
*/

int atpread(atpskt *skt, ddpbuf *bufp, atpaddr *source) {

    int cc; 		/* length received */ 
    struct sockaddr_in fromaddr;	
    int			fromlen;

    switch(source->family) {
    	case AF_INET:
	    for (;;) {			/* until good packet read */
		fromlen = sizeof(fromaddr);
		cc = recvfrom(skt->udp, (char *) &bufp->ddptype, DDPDATAMAX+1, 0,
			    (struct sockaddr *) &fromaddr, &fromlen);
		if (cc < ATPHDRLEN + 1) {
		    t_errprint("atpread: short packet ignored");
		    continue;
		}
		/* fill in source addr/port */
		source->port = fromaddr.sin_port;
		source->addr = fromaddr.sin_addr.s_addr;
		return cc - 1;		/* DDP type byte doesn't count */
	    }
	    break;
	case AF_APPLETALK:
	    return ddpread(skt->ddp, bufp, (ataddr *) &source->addr);
	    break;
	default:
	    t_errprint_l("atpread: Invalid address family: %ld", source->family);
	    return -1;
	    break;
    }
	
}

/* atpwrite --

    Write ATP packet to DDP or UDP (according to "remoteaddr.family").
    
    The "ddpbuf" structure is used in both cases, although the DDP
    header is meaningless in the UDP case.  
*/

int atpwrite(atpskt *skt, ddpbuf *bufp, int len, atpaddr *remoteaddr) {

    struct sockaddr_in 	udpdest; 	/* udp destination address */
    int			s;

    switch(remoteaddr->family) {
    	case AF_INET:
	    bufp->ddptype = DDP$ATP;	/* set DDP type */
	    /* set up UDP address */
	    udpdest.sin_family = AF_INET;
	    udpdest.sin_port = remoteaddr->port;
	    udpdest.sin_addr.s_addr = remoteaddr->addr;
	    
	    s = sendto(skt->udp, (char *) &bufp->ddptype, len+1, 0,
	    			(struct sockaddr *)&udpdest, sizeof(udpdest));
	    if (s < 0)
		t_perror("atpwrite: sendto");
	    return s;
	    
	    break;
	case AF_APPLETALK:
	    if (m_noappletalk)		/* AT off, but entry still in table? */
		return 0;		/* fail quietly */
	    return ddpwrite(skt->ddp, bufp, len, *((ataddr *) &remoteaddr->addr));
	    break;
	default:
	    t_errprint_l("atpwrite: Invalid address family: %ld", remoteaddr-> family);
	    return -1;
	    break;
    }
}

/* pkterr --
    pkterr_i --

    Format & log error message complaining about bad packet.
*/

void pkterr(char *msg, atpaddr *clientaddr) {

    char buf[256];
    ataddr *at_clientaddr = (ataddr *) &clientaddr->addr;
    
    if (clientaddr->family == AF_APPLETALK) {
	t_sprintf(buf, "%s from %ld/%ld/%ld\n",
		    msg,
		    (long) at_clientaddr->at_net,
		    (long) at_clientaddr->at_node,
		    (long) at_clientaddr->at_sock);
    } else {
	pthread_mutex_lock(&inet_ntoa_lock);
	t_sprintf(buf, "%s from %s %ld\n",
		msg,
		inet_ntoa(*((struct in_addr *) &clientaddr->addr)),
		(long) ntohs(clientaddr->port));
	pthread_mutex_unlock(&inet_ntoa_lock);
    }
    t_syslog(LOG_DEBUG, buf);
}

void pkterr_i(char *fmt, int i, atpaddr *clientaddr) {

    char buf[256];
    
    t_sprintf(buf, fmt, i);
    pkterr(buf, clientaddr);

}

