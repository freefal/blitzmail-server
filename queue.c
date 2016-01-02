/*  BlitzMail Server -- queue routines

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/queue.c,v 3.6 98/10/21 16:09:27 davidg Exp Locker: davidg $

    Process queued messages.  There's an independent queue for each
    message destination (one for each peer server, including the
    local server.)  Each queue is processed by a single thread; when
    a new message is added to the queue "wake_queuethread" is called
    to inform (and possibly wake up) the thread.  
        
    The spool directory (with its control files) is the permanent
    record of the state of the queue, although an in-memory list
    is also kept by the queue thread.  The queue thread is unblocked
    periodically to retry old messages in the queue (mostly the only
    reason they'll still be there is if the server at the other end
    is down.)
    
    The control & data files are number using "qid"s, which are
    distinct from message ids.  The summary info in the control
    file indicates the corresponding message id (a messageid is
    assigned at the time the message is entered into the queue).
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <sysexits.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "mess.h"
#include "queue.h"
#include "deliver.h"
#include "client.h"
#include "smtp.h"
any_t inqueue_read(any_t hostnum_);
any_t outqueue_read(any_t hostnum_);
int sendsmtp_one(int hostnum, t_file **conn, qent *cur);
void hopcount_check(fileinfo *head, recip *rlist);
int sendout_one(int hostnum, t_file **conn, qent *cur);
void rewrite_qfile(char *sender, char *summstr, recip *rlist, char *fname);
void bounce_822(char *sender, recip *badrecips, t_file *mess, summinfo *summ);

/* queue_init --

    Set up queue routines; must be called first (but after config read).
    
    Note that the outgoing smtp queue is host number -1 (to hide it from
    most loops that deal with all peers).
*/
void queue_init () {

    int		i;
    
    /* leave room for a -1st entry in each of these */
    q_lock = (pthread_mutex_t *) mallocf((m_servcount+1) * sizeof(pthread_mutex_t));
    ++q_lock;
    q_wait = (pthread_cond_t *) mallocf((m_servcount+1) * sizeof(pthread_cond_t));
    ++q_wait;
    q_head = (qent **) mallocf((m_servcount+1) * sizeof(qent *));
    ++q_head;
    q_tail = (qent **) mallocf((m_servcount+1) * sizeof(qent *));
    ++q_tail;
    
    next_q_id = 1;			/* increased if queues have messages already */
    
    for (i = -1; i < m_servcount; ++i) {
	pthread_mutex_init(&q_lock[i], pthread_mutexattr_default);
	pthread_cond_init(&q_wait[i], pthread_condattr_default);
	q_head[i] = q_tail[i] = NULL;
	queue_startup(i);
    }

    sem_init(&not_sem, "not_sem");	/* set up to talk w/ notification server */
    not_f = NULL;			/* not there yet... */
      
}

/* queue_startup --

    Initialize one queue.  Read the spool directory to find out what
    messages and control files are there (ignore any unmatched ones).
    
    Fork thread to handle the queue.
*/

void queue_startup(int hostnum) {

    char		fname[MESS_NAMELEN];
    int			i, j;
    DIR			*dirf;			/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    struct q {					/* for sorting queue */
    	boolean_t	ctl;			/* control file seen? */
	boolean_t	msg;			/* message file seen? */
	long		qid;			/* qid */
    };
    long		qid;
    struct q		*qlist;			/* array of sorted qids */
    int			qcount, qmax;		/* current & max length */
    char		*end;			/* end of scanned number */
    boolean_t		control;		/* control file? */
    qent		*new, *prev;		/* to construct list */
    pthread_t		thread;			/* created thread */
    
    if (!m_server[hostnum])			/* this queue not configured? */
	return;					/* (smtp) */
	
    qcount = 0; qmax = 100;
    qlist = (struct q *) mallocf(qmax * sizeof(struct q));
    
    relocate_time = mactime();			/* time of most recent transfer (assuming the worst) */
    
    /* read spool directory for this host, get sorted list of queue ids */
    
    t_sprintf(fname, "%s%s%s", m_spoolfs_name, SPOOL_DIR, m_server[hostnum]);

    if (mkdir(fname, DIR_ACC) < 0) {		/* create it if not there yet */
	if (pthread_errno() != EEXIST)
	    t_perror1("queue_startup: cannot create ", fname);    
    }

    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    dirf = opendir(fname);
    pthread_mutex_unlock(&dir_lock);
    
    if (dirf == NULL) {
	t_perror1("queue_startup: cannot open ", fname);
    } else {

	while ((dirp = readdir(dirf)) != NULL) {	/* read entire directory */
		
	    /* skip dot-files */
	    if (dirp->d_name[0] != '.') {
		if (dirp->d_name[strlen(dirp->d_name) - 1] == 'C') {
		    control = TRUE;		/* this is control file */
		    dirp->d_name[strlen(dirp->d_name) - 1] = 0;
		} else
		    control = FALSE;
		
		end = strtonum(dirp->d_name, &qid);
		
		if (*end != 0) 		/* ignore non-numeric filenames */
		    continue;
		    
		/* insert new id into sorted list */    
		if (qcount == qmax) { 
		    qmax += 100;	/* need to grow list */
		    qlist = reallocf(qlist, qmax * sizeof(struct q));
		}
		for (i = 0; i < qcount; i++) {
		    if (qid <= qlist[i].qid)
			break;
		}
	    
		if (i < qcount) {	/* match, or need to slide others out of way */
		    if (qid == qlist[i].qid) { /* match? */
			if (control)
			    qlist[i].ctl = TRUE; /* control file seen */
			else
			    qlist[i].msg = TRUE; /* message file seen */
			continue;		/* don't insert new entry */
		    }
		    for (j = qcount; j > i; --j)
			qlist[j] = qlist[j-1];
		} 
	    
		/* room made; add the new entry */
		qlist[i].qid = qid;
		if (control) {
		    qlist[i].ctl = TRUE; /* control file seen */
		    qlist[i].msg = FALSE;
		} else {
		    qlist[i].msg = TRUE; /* message file seen */
		    qlist[i].ctl = FALSE;
		}
		qcount++;			/* one more total */
	    }
	}
	
	closedir(dirf);
    }
    
    /* now construct linked list of messages for which both ctl & msg file found */

    pthread_mutex_lock(&q_lock[hostnum]);
    
    prev = NULL;
    for (i = 0; i < qcount; ++i) {
	if (!qlist[i].ctl) {			/* control file missing */
	    queue_fname(fname, hostnum, qlist[i].qid); /* name of data file */
	    t_errprint_s("Remove stray spool file %s", fname);
	    (void) unlink(fname);
	    continue;
	}
	if (!qlist[i].msg) {			/* data file missing */
	    queue_fname(fname, hostnum, qlist[i].qid); /* name of data file */
	    strcat(fname, "C");			/* of control file */
	    t_errprint_s("Remove stray control file %s", fname);
	    (void) unlink(fname);
	    continue;
	}
	new = (qent *) mallocf(sizeof(qent));
	new->qid = qlist[i].qid;
		
	new->next = NULL;			/* new one is last */
	if (prev)
	    prev->next = new;
	else
	    q_head[hostnum] = new;		/* first one... */
	prev = new;
    }
    if (prev) {
	q_tail[hostnum] = prev;
	pthread_mutex_lock(&global_lock);
	if (prev->qid >= next_q_id)		/* make sure any new qids */
	    next_q_id = prev->qid + 1;		/* are larger than ones used so far */
	pthread_mutex_unlock(&global_lock);
    }
    
    pthread_mutex_unlock(&q_lock[hostnum]);
    if (hostnum == m_thisserv) {			/* for this server? */
	if (pthread_create(&thread, generic_attr,
			(pthread_startroutine_t) inqueue_read, (pthread_addr_t) hostnum) < 0) {
	    t_perror("queue_startup: inqueue_read pthread_create");
	    exit(1);
	}
	pthread_detach(&thread);
    } else {
	if (pthread_create(&thread, generic_attr,
			(pthread_startroutine_t) outqueue_read, (pthread_addr_t) hostnum) < 0) {
	    t_perror("queue_startup: outqueue_read pthread_create");
	    exit(1);
	}
	pthread_detach(&thread);
    }
    t_free(qlist);
   
}
/* wake_queuethread --

    Add message to end of queue, signal thread that services the queue.
*/

void wake_queuethread(int hostnum, long qid) {

    qent	*new;
    
    new = (qent *) mallocf(sizeof(qent));
    new->qid = qid;
    new->next = NULL;
    
    /* add to end of queue */
    pthread_mutex_lock(&q_lock[hostnum]);
    if (q_tail[hostnum]) 
	q_tail[hostnum]->next = new;
    else
	q_head[hostnum] = new;
    q_tail[hostnum] = new;
    pthread_mutex_unlock(&q_lock[hostnum]);
    
    pthread_cond_signal(&q_wait[hostnum]);	/* wake owner thread */
}

/* inqueue_read --

    Thread to handle this server's incoming queue.  If queue has anything in it, 
    process appropriately, then wait until signalled again.  Other threads may append 
    to the end of the queue, but this is the only one that may delete.
    
    Usually our job is to call localdeliver to stuff the message into the appropriate
    mailboxes, but if there's forwarding involved, we need to turn around and feed
    the message back through the delivery process.  (With distributed servers, it's
    no longer possible to deal with forwarding at address resolution time.)  
        
    Since we're delivering either into mailboxes our other queues on the local server,
    there's never any need to defer delivery; we always clean out the queue.

    For cloned enclosures, forwarding is disabled (by setting the F_NOFWD flag
    in the control file); the clone is to be delivered locally, regardless
    of the forwarding address.
*/

any_t inqueue_read(any_t hostnum_) {

    int		hostnum;		/* our host number */
    qent	*cur;			/* current queue entry */
    t_file	*f;			/* control file */
    char	sender[MAX_ADDR_LEN];	/* sender's address */
    char	fname[MESS_NAMELEN];
    char	tmpname[MESS_NAMELEN];
    char	s[SUMMBUCK_LEN];
    char	*p;
    summinfo	summ;			/* summary info */
    long	uid;			/* recip uid */
    char	fsname[MESS_NAMELEN];	/* filesystem mailbox lives on */
    u_long	timestamp;		/* resolution timestamp */
    long  	flags;			/* recipient flags */
    boolean_t	hextext;		/* binhex text enclosures? */
    recip	*rlist;			/* recipients after forwarding check */
    recip	*r, *tempr;
    char	name[MAX_ADDR_LEN];	/* recip name */
    char	addr[MAX_ADDR_LEN];	/* and addr */
    char 	*namep;			/* pointer into name */
    int		recipcount;		/* count for resolve */
    messinfo	mi;			/* message info: */
    fileinfo	head;			/* header */
    fileinfo	text;			/* text */
    enclinfo	*encl;			/* enclosure list */
    long	mtype;			/* message type */
    int		i;
    boolean_t	resend;			/* message needs resending? */
    boolean_t	sent;
    boolean_t	must_resolve;		/* must re-check dnd? */
    
#define CHECK_COMMA(x)	if (*(x)++ != ',') { t_errprint_s("inqueue_read: bad ctl file: %s",\
fname); t_fclose(f); goto unlink_it; }
   
    hostnum = (int) hostnum_;
    
    setup_signals();			/* set up new thread environment */
    setup_syslog();

    for (;;) {
	pthread_mutex_lock(&q_lock[hostnum]);
	while(q_head[hostnum] == NULL)
	    pthread_cond_wait(&q_wait[hostnum], &q_lock[hostnum]);
	    
	cur = q_head[hostnum];
	pthread_mutex_unlock(&q_lock[hostnum]);

	queue_fname(fname, hostnum, cur->qid);		/* name of data file */
	strcat(fname, "C");				/* of control file */
	sent = FALSE;
	
	if ((f = t_fopen(fname, O_RDONLY, 0)) == NULL) {
	    t_perror1("inqueue_read: cannot open ", fname);
	    goto unlink_it;
	}
	
	t_gets(sender, sizeof(sender), f);		/* get sender's address */
	if (t_gets(s, sizeof(s), f) == NULL) {		/* get summary info */
	    t_fclose(f);
	    t_errprint_s("inqueue_read: incomplete control file: %s", fname);
	    goto unlink_it;
	}
	if (!summ_parse(s, &summ, fname, FALSE))	/* parse (unpacked form) */
	    goto unlink_it;
    	date_time(summ.date, summ.time);		/* update current date & time */
	summ.expire = pick_expire(&summ);		/* choose expiration date */
	summ.read = FALSE;
	
	rlist = NULL;
	
	/* <uid>,<blitzfs>,<resolve timestamp>,<name> */	
	while(t_gets(s, sizeof(s), f)) {		/* for each recipient */
	    p = strtonum(s, &uid);		
	    CHECK_COMMA(p);

            for(namep = fsname; *p && *p != ','; )	/* filesystem name */
		*namep++ = *p++;
	    *namep++ = 0;
	    CHECK_COMMA(p);

	    p = strtouns(p, &timestamp); 
	    CHECK_COMMA(p);
	
            for(namep = name; *p && *p != ','; )	/* recip name (if re-resolve needed) */
		*namep++ = *p++;
	    *namep++ = 0;
	    CHECK_COMMA(p);
			
	    p = strtonum(p, &flags);		/* get flag bits */

	    recipcount = 0;
	    
	    must_resolve = FALSE;		/* assume we don't have to ask DND again */
	    
	    /* if resolved more than a day ago, or we've moved users since
	       then, must re-resolve (5 min slop for clock drift).
	       Special uids are immune from this check (since they aren't
	       really in the dnd in the first place.) */
	    if ((mactime() - timestamp > 24*60*60 
	    		|| timestamp < 5*60 + relocate_time) 
		    && uid > 0) {
		must_resolve = TRUE;
	    }	
	    
	    /* If fs name is missing (new user) or invalid (queue file copied from another server),
	       be sure to consult DND (very bad news if we try to assign them a new fs!) */
	    if (uid > 0 && fs_match(fsname) == -1)
		must_resolve = TRUE;
		
	    if (must_resolve) {
		addhost(name, addr, m_hostname); /* resolve to blitz box always */
	    	sresolve(addr, &r, 0, NULL, NULL, &recipcount);
	    } else {			/* don't need to re-resolve */
		r = alloc_recip(&recipcount);	/* construct recip by hand */
		strcpy(r->name, name);
		r->addr[0] = 0;
		r->id = uid;
		r->local = TRUE;
		r->blitzserv = m_thisserv;
		strcpy(r->blitzfs, fsname);
		r->stat = RECIP_OK;
		if (uid > 0 && !(flags & F_NOFWD))	/* check fwding only for real users */
		    check_forward(&r, 0, &recipcount); 
	    }
	    		
	    /* add to our existing recip list */
	    if (!rlist) {			
		rlist = r;
	    } else {
		tempr = rlist->next;
		rlist->next = r->next; 		/* end of old -> beginning of new */
		r->next = tempr; 		/* end of new -> beginning of old */
		rlist = r;			/* new tail */
	    }
	}				/* end of reading recips from control file */

	t_fclose(f);

	/* now open & parse the message file, setting up messinfo,
	   header fileinfo, text fileinfo, and enclosure list */
	   
	queue_fname(fname, hostnum, cur->qid);
	
	if (!mess_open(fname, &head, &text, &encl, &mi.finfo.len, &mtype)) {
	    t_perror1("inqueue_read: cannot open ", fname);
	    goto unlink_it;
	}	
	
	/* set up messinfo describing file in queue */
	mi.messid = summ.messid;		/* assign message id */
	for (i = 0; i < m_filesys_count; ++i)
	    mi.present[i] = FALSE;		/* haven't saved copy anywhere yet */
	if (m_spool_filesys != -1)		/* if spool filesys has mailboxes */
	    mi.present[m_spool_filesys] = TRUE; /* don't need to copy */
	strcpy(mi.finfo.fname, fname);
	mi.finfo.offset = 0;			/* (length set above) */
	mi.finfo.temp = FALSE;			/* don't unlink */
	
	/* see if message is in a forwarding loop */
	hopcount_check(&head, rlist);

	/* send bounces for anything that failed to resolve (e.g., bad forwarding) */
	do_bounces(sender, rlist, &head, &text, &summ);
	
	/* send vacations (both for local folks & those that forward) */
	do_vacations(rlist, &head, &summ);	
	
	resend = FALSE;
	
	/* now deliver to boxes on this server */
	for (r = rlist->next ;; r = r->next) {	
	    if (!r->nosend && r->stat == RECIP_OK) {
		if (r->local && r->blitzserv == m_thisserv) {
		    localdeliver(sender, r, &mi, &head, &text, encl, &summ);
		    r->nosend = TRUE;	/* don't need to deal further */
		} else			/* valid recip not here */
		    resend = TRUE;	/* must re-send this message */
	    }
	    if (r == rlist)
		break;
	}
	
	/* note that blitzdeliver will assign a new qid */
	if (resend) {			/* if nonlocal recips cropped up, re-send */
	    hextext = flags & F_HEXTEXT; /* binhex text enclosures? */
	    internet(sender, rlist, NULL, &head, &text, encl, &summ, hextext);
	    blitzdeliver(sender, rlist, NULL, &mi, &head, &text, encl, &summ, hextext);
	}
	clean_encl_list(&encl);		/* clean up enclosure list */
	/* (don't need finfoclose since not using temp file) */
	mess_done(&mi);			/* remove mess_deliver temps */
	
	sent = TRUE;
	
unlink_it:	/* here on missing control/data file */

	free_recips(&rlist);		/* clean up recip list */
	
	if (!sent) {			/* if error, move files to temp dir for debugging */
	    t_errprint("Moving bad control file & mess to mtmp directory");
	    queue_fname(fname, hostnum, cur->qid);
	    mess_tmpname(tmpname, m_spool_filesys, cur->qid);
	    strcat(tmpname, "bad");
	    if (rename(fname, tmpname) < 0)
		t_perror1("inqueue_read: rename ", fname);
	    strcat(fname, "C"); strcat(tmpname, "C");
	    if (rename(fname, tmpname) < 0)
		t_perror1("inqueue_read: rename ", fname);
	} 				/* try to unlink too, in case of error on rename */
	queue_fname(fname, hostnum, cur->qid);
	(void) unlink(fname);		/* unlink message file */
	strcat(fname, "C");
	(void) unlink(fname);		/* and control file */
		
	pthread_mutex_lock(&q_lock[hostnum]);
	if (q_head[hostnum] != cur)
	    abortsig();			/* somebody messed with the queue! */
	q_head[hostnum] = cur->next;
	if (q_head[hostnum] == NULL)
	    q_tail[hostnum] = NULL;
	pthread_mutex_unlock(&q_lock[hostnum]);
	t_free(cur);
	
    }					/* end of queue */

    return 0;				/* notreached */
}

/* hopcount_check --

    Scan message header, counting Received: lines.  If message has been
    forwarded too many times, mark recipients with the RECIP_LOOP bad
    status.

*/

void hopcount_check(fileinfo *head, recip *rlist) {

    t_file	*f;			/* header file */
    long	remaining;		/* length left to read */
    int		hopcount = 0;		/* number of received: lines seen */
    char	*s;			/* current header line */
    recip	*r;			/* current recip */
    
    if ((f = t_fopen(head->fname, O_RDONLY, 0)) == NULL) {
	t_perror1("hopcount_check: cannot open ",head->fname);
	return;
    }
    
    t_fseek(f, head->offset, SEEK_SET);	/* start at beginning of header */
    
    /* count "Received:" lines in header */
    remaining = head->len;
    while((s = getheadline(f, &remaining, TRUE)) != NULL) { /* (sic) */
	if (strncasecmp(s, "Received:", strlen("Received:")) == 0)
	    ++hopcount;
	t_free(s);
    }
    t_fclose(f);
    
    /* if too many, mark recip(s) with bad status */
    
    if (hopcount > SMTP_HOP_MAX) {
	for (r = rlist->next ;; r = r->next) {
	    if (!r->nosend && r->stat == RECIP_OK)
		r->stat = RECIP_LOOP;	/* mark only ones we would have sent to */
	    if (r == rlist)		/* end of circular list */
		break;
	}
    }
}

/* outqueue_read -- 

    Handle outgoing queue for a single destination server.  The server may be either
    a peer or our SMTP relay host.   For a blitz peer, both the control and
    message files are transmitted to the other end; the message is transmitted in
    its constituent pieces (header, text, enclosures) to make it easier for systems
    with alignment restrictions.
    
    The protocol is in some ways more like a file transfer than the usual mail transfer
    protocol (although still on top of SMTP) -- the other end is not given a chance to
    reject individual recipients; the whole message is transferred and the other end
    can send back bounces if it likes.  (If the other end is a peer, bounces will
    be very uncommon since we used the same DND to generate the addresses in the
    first place; a bad forwarding address is about the only thing that should lead
    to a bounce from them.)
    
    By default, the connection to the other end is kept open continuously (because there
    will be a small number of peer servers with a relatively rate of traffic, this
    makes more sense than setting up & tearing down the connection every time.)

*/
any_t outqueue_read(any_t hostnum_) {

#define MIN_SLEEP	60		/* minimum connect retry interval */
#define MAX_SLEEP	16*60		/* backoff until reaching this point */

    int		hostnum;		/* host we're responsible for */
    t_file	*conn = NULL;		/* connection to them */
    qent	*cur;			/* current queue entry */
    unsigned	sleep_time = MIN_SLEEP;	/* connection retry delay */
    char	fname[MESS_NAMELEN];
    int		stat;			/* status of one mess */
    
    hostnum = (int) hostnum_;

    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
   
    /* note that this is a strict queue:  the entry at the head is always the
       one being operated on */
       
    for (;;) {
	pthread_mutex_lock(&q_lock[hostnum]); /* wait for something to appear in queue */
	while(q_head[hostnum] == NULL)
	    pthread_cond_wait(&q_wait[hostnum], &q_lock[hostnum]);
	    
	cur = q_head[hostnum];
	pthread_mutex_unlock(&q_lock[hostnum]);	
	    
	if (cur == NULL) {		/* no job ready... */
	    if (conn && (conn->want & SEL_CLOSE)) { /* ...but has connection timed out? */
		t_fclose(conn);		/* yes - get rid of it */
		conn = NULL;
	    }
	    continue;			/* no job; back to sleep */
	}
	
	if (hostnum == SMTP_HOSTNUM) {	/* the smtp queue? */
	    stat = sendsmtp_one(hostnum, &conn, cur);	/* outgoing smtp */
	    if (m_smtpdisconnect && conn) {	/* disconnect after every message? */
		t_fprintf(conn, "QUIT\r\n"); /* yes - say goodbye */
					/* (don't care if they respond) */
    		t_fclose(conn);		/* close */
		conn = NULL;
	    }	    			
	} else
	    stat = sendout_one(hostnum, &conn, cur); /* process outgoing blitz */
	
	if (stat == Q_RETRY) {		/* transient error? */
	    if (conn) {			/* if connnected... */
		t_fclose(conn);		/* close, in case out of sync */
		conn = NULL;
	    }
	    sleep(sleep_time);		/* delay before retrying */
	    if (sleep_time < MAX_SLEEP)
		sleep_time *= 2;	/* exponential backoff */
	    
	} else { 			/* done w/ this message */
	    if (stat == Q_OK) 
		sleep_time = MIN_SLEEP;	/* reset retry interval */
	    else  if (conn) { 		/* if connnected... */
		t_fclose(conn);	/* close, in case out of sync */
		conn = NULL;
	    }

	    queue_fname(fname, hostnum, cur->qid);
	    (void) unlink(fname);	/* unlink message file */
	    strcat(fname, "C");
	    (void) unlink(fname);	/* and control file */

	    pthread_mutex_lock(&q_lock[hostnum]); /* and remove entry from queue */
	    if (q_head[hostnum] != cur)
		abortsig();		/* somebody messed with the queue! */
	    q_head[hostnum] = cur->next;
	    if (q_head[hostnum] == NULL)
		q_tail[hostnum] = NULL;
	    t_free(cur);		/* done with queue entry */
	    pthread_mutex_unlock(&q_lock[hostnum]);
	}
    }
}

/* sendout_one --

    Transfer a message to the other server.  Returns Q_OK if the message has
    been sent, Q_ABORT if it failed (and should not be retried), and Q_RETRY
    if the message should be retried later.
    
    Communication failures (e.g., other server not responding) are about the
    only type of error that should be retried (especially true since the
    queue is processed in strict order; a stuck message is big trouble).
*/

int sendout_one(int hostnum, t_file **conn, qent *cur) {

    t_file	*f;			/* control/data file */
    fileinfo	head;			/* header */
    fileinfo	text;			/* text */
    enclinfo	*encl;			/* enclosure list */
    enclinfo	*ep;
    long	mtype;			/* message type */
    char	buf[MAX_STR];
    char	fname[MESS_NAMELEN];
    int		i;
    long	messid;

    if (!*conn) { 			/* need to connect to other server? */
	if ((*conn = serv_connect(m_server[hostnum])) == NULL)
	    return Q_RETRY;		/* can't connect; retry later */
    }

    queue_fname(fname, hostnum, cur->qid); /* open the data file */  
    if (!mess_open(fname, &head, &text, &encl, NULL, &mtype)) {
	t_perror1("sendout_one: cannot open ", fname);
	return Q_ABORT;			/* bad; blow it away */
    }		    
    
 
    t_puts(*conn, "XBTZ\r\n");		/* here it comes... */
    if (!checkresponse(*conn, buf, SMTP_BLITZON)) {/* ...are they ready? */
	if (buf[0] == SMTP_RETRY)	/* transient error? */
	    return Q_RETRY;		/* yes - try again later */
	if (strlen(buf) == 0)		/* other server down? */
	    ;				/* don't bother logging */
	else
	    t_errprint_ss("Protocol error connecting to %s: %s", m_server[hostnum], buf);
	return Q_RETRY;			/* don't destroy the message */	
    }
    
    t_puts(*conn, "BLTZ");		/* identify format of following chunks */
    
    strcat(fname, "C");			/* open the control file now */
    if ((f = t_fopen(fname, O_RDONLY, 0)) == NULL) {
	t_perror1("sendout_one: cannot open ", fname);
	clean_encl_list(&encl);
	return Q_ABORT;			/* not all there; unlink */
    }
    
    /* read control file to determine messid */
    t_gets(buf, sizeof(buf), f); 	/* sender addr */
    t_gets(buf, sizeof(buf), f);	/* summary */
    strtonum(buf, &messid);		/* get the messageid */

    /* send message to other server piece-by-piece */
    xmit_file(*conn, f, "CTRL");	/* send control info first */
    t_fclose(f);			/* done w/ control file */
    
    xmit_block(*conn, &head, "HEAD");
    xmit_block(*conn, &text, "TEXT");
    for (ep = encl; ep; ep = ep->next) {
	xmit_encl(*conn, ep);	    
    }
    
    for (i = 0; i < 4; ++i)		/* indicate end of message */
	t_putc(*conn, 0);		/* with a zero-length "block" */

    clean_encl_list(&encl);		/* clean up enclosures */
    /* (don't need to finfoclose head & text; they aren't temp files) */

    if(!checkresponse(*conn, buf, SMTP_OK)) { /* did they accept it? */
	if (buf[0] == SMTP_RETRY || strlen(buf) == 0)	
	    return Q_RETRY;		/* transient error - try again later */
	t_errprint_ss("Protocol error sending to %s: %s", m_server[hostnum], buf);
	return Q_RETRY;			/* don't destroy the message */	
    }
    
    t_sprintf(buf, "Message %ld forwarded to server %s", messid,
			m_server[hostnum]);
    log_it(buf);
    
    return Q_OK;			/* message delivered */
}

/* sendsmtp_one --

    Send an outgoing SMTP message.  Returns Q_OK if the message has
    been sent, Q_ABORT if it failed (and should not be retried), and Q_RETRY
    if the message should be retried later.
    
    Note that the SMTP response line from the remote server is stashed in the
    "name" field of the recipient node.
    
*/

int sendsmtp_one(int hostnum, t_file **conn, qent *cur) {

    t_file	*f;			/* control/data file */
    char	sender[MAX_STR];	/* sender address */
    char	summstr[SUMMBUCK_LEN];	/* summary info in string form */
    summinfo	summ;			/* in broken-down form */
    char	fname[MESS_NAMELEN];
    char	buf[MAX_STR];
    recip	*rlist = NULL;		/* recipient list */
    recip	*r;			/* one recip */
    recip	*badrecips = NULL;	/* recipents that failed */
    recip	*retryrecips = NULL; 	/* recipients that must be retried */
    int		recipcount = 0;
    int		goodrecip = 0;		/* recips accepted by other end */
    boolean_t	ok = TRUE;
    
    if (!*conn) { 			/* need to connect to other server? */
	if ((*conn = serv_connect(m_server[hostnum])) == NULL)
	    return Q_RETRY;		/* can't connect; retry later */
    }
    
    queue_fname(fname, hostnum, cur->qid);  
    strcat(fname, "C");			/* open the control file */
    if ((f = t_fopen(fname, O_RDONLY, 0)) == NULL) {
	t_perror1("sendsmtp_one: cannot open ", fname);
	return Q_ABORT;			/* not all there; unlink */
    }
    t_gets(sender, sizeof(sender), f); 	/* sender addr */
    t_gets(summstr, sizeof(summstr), f);/* summary */
    if (!summ_parse(summstr, &summ, fname, FALSE)) { /* parse it */
	t_fclose(f);
	return Q_ABORT;			/* bogus summary info */
    }

    /* now get each recipient */
    r = alloc_recip(&recipcount);	/* get a temp recipient */
    while(t_gets(buf, sizeof(buf), f)) {
	 strcpy(r->addr, buf);		/* pick up address */
	 strcpy(r->name, "");		/* status ok so far */
	 copy_recip(r, &rlist);		/* copy recipient node into list */
    }
    t_free(r);				/* free temp */
    if (rlist == NULL) {		/* should be something */
	t_errprint_l("sendsmtp_one: no recips for messid %ld?", summ.messid);
	return Q_ABORT;
    }    
    
    t_fclose(f);			/* done reading control file */
    queue_fname(fname, hostnum, cur->qid);  /* now, open data file */
    if ((f = t_fopen(fname, O_RDONLY, 0)) == NULL) {
	t_perror1("sendsmtp_one: cannot open ", fname);
	free_recips(&rlist);
	return Q_ABORT;			/* not all there; unlink */
    }

    t_fprintf(*conn, "MAIL FROM:<%s>\r\n", sender);
    ok = checkresponse(*conn, buf, SMTP_OK);

    if (ok) {	
	for (r = rlist->next ;; r = r->next) {
	    /* send each recipient addr */
	    t_fprintf(*conn, "RCPT TO:<%s>\r\n", r->addr);
	    if (!checkresponse(*conn, buf, SMTP_OK)) {
		if (strlen(buf) == 0)		/* lost connection? */
		    t_sprintf(r->name, "%d Lost connection to SMTP host", SMTP_SHUTDOWN);
		else
		    strcpy(r->name, buf);	/* keep status here */
	    } else
		++goodrecip;			/* count # of valid recips */
	    if (r == rlist)			/* end of circular list */
		break;
	}
    }
    if (ok && goodrecip > 0) {		/* iff at least one valid recip */
	t_fprintf(*conn, "DATA\r\n");	/* send message data now */
	ok = checkresponse(*conn, buf, SMTP_SENDON);
	if (ok) {
	    while(t_gets(buf, sizeof(buf), f)) { /* read each line */
		if (buf[0] == '.')
		    t_putc(*conn, '.');		 /* escape initial dots */
		t_fprintf(*conn, "%s\r\n", buf); /* use CRLF terminators */
	    }
	    t_fprintf(*conn, ".\r\n");	/* end of message */
	    ok = checkresponse(*conn, buf, SMTP_OK);
	} 
    }

    /* if "ok" is false, we encountered an error applying to all recips */
    if (!ok) {				/* error status still in "buf" */
	if (strlen(buf) == 0)		/* fake status for lost connection */
	    t_sprintf(buf, "%d", SMTP_SHUTDOWN);
	for (r = rlist->next ;; r = r->next) {
	    strcpy(r->name, buf);	/* fill in status for every recip */
	    if (r == rlist)		/* end of circular list */
		break;
	}	
    }

    /* Walk through the recipient list locating recipients that failed
       or need to be retried later.  For failed recipients, the bounce
       message is sent now (and that's the end of it).  If there are
       retry recipients, the control file must be rewritten (including
       just them). */

    for (r = rlist->next ;; r = r->next) {
	if (strlen(r->name) == 0) {	/* no error status? */
	    t_sprintf(buf, "Sent message %ld to %s", summ.messid, r->addr);
	    log_it(buf);
	}
	else if (r->name[0] == SMTP_RETRY)
	    copy_recip(r, &retryrecips);/* retryable error */
	else
	    copy_recip(r, &badrecips);	/* unrecoverable error */
	if (r == rlist)			/* end of circular list */
	    break;
    }	
    free_recips(&rlist);		/* done with master list */
    
    /* if there's any doubt about what state the connection might be left in,
       force a new connection for the next message */
    if (badrecips || retryrecips)	
	ok = FALSE;
	
    if (badrecips) {			/* send any bounces */
    	bounce_822(sender, badrecips, f, &summ);
	free_recips(&badrecips);
    }
    t_fclose(f);			/* done with data */
    
    if (retryrecips) {			/* retry needed? */
        queue_fname(fname, hostnum, cur->qid);  
	strcat(fname, "C");		/* control file name */
	rewrite_qfile(sender, summstr, retryrecips, fname); /* rewrite control file */
	free_recips(&retryrecips);
	return Q_RETRY;			/* message remains in queue */
    } else if (ok) {
	++m_sent_internet;		/* statistics: count internet messages sent */
	return Q_OK;			/* message dealt with */
    } else				/* if any errors at all... */
	return Q_ABORT;			/* ...should reconnect next time */
    
}

/* bounce_822 -- 

    Bounce an RFC822-format message.  To do this correctly, we need to
    copy the message stripping \n's (and noting where the header & text
    begin).  The SMTP error status is in recip.name.

*/

void bounce_822(char *sender, recip *badrecips, t_file *mess, summinfo *summ) {

    fileinfo	head;			/* frames message header */
    fileinfo	text;			/* and text */
    boolean_t	inhead = TRUE;		/* start out in header */
    char	buf[MAX_STR];		/* one line of file */
    t_file	*out;			/* reformatted message */
    recip	*r;
    
    temp_finfo(&head);			/* get temp file name */
    text = head;			/* use same file for text */
    text.temp = FALSE;			/* don't try to unlink it twice */
    head.offset = text.offset = 0;
    head.len = text.len = 0;
    if ((out = t_fopen(head.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("bounce_822: cannot open ", head.fname);
	return;
    }
    
    t_fseek(mess, 0, 0);		/* rewind to beginning */
    
    while (t_gets(buf, sizeof(buf), mess)) { /* read through file */
	t_fprintf(out, "%s\r", buf);	/* copy to output, w/ mac terminator */
	if (inhead) {
	    if (strlen(buf) == 0) {	/* blank line ends header */
		inhead = FALSE;
		text.offset = head.len + 1; /* remainder is text */
	    } else			/* note: blank line is discarded */
		head.len += strlen(buf) + 1; /* more header data */
	} else
	    text.len += strlen(buf) + 1; /* more text data */
    }
    
    t_fclose(out);			/* don't need to keep open */

    for (r = badrecips->next ;; r = r->next) { /* error status is in r->name */
	bad_mail(sender, r->addr, &head, &text, summ, r->name);
	if (r == badrecips)		/* end of circular list */
	    break;
    }	
        
    finfoclose(&head);			/* clean up temp file */
    					/* don't do text; it's same file! */
}
/* rewrite_qfile -- 

    Rewrite queue control file with new (smaller?) recipient list.

*/

void rewrite_qfile(char *sender, char *summstr, recip *rlist, char *fname) {

    t_file	*f;			/* new file */
    char	tempname[MAX_STR];	/* temp filename */
    recip	*r;
    
    t_sprintf(tempname, "%s.temp", fname); /* generate temp file name */
    
    if ((f = t_fopen(tempname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("rewrite_qfile: cannot create ", tempname);
	return;				
    }

    t_fprintf(f, "%s\r\n", sender);	/* bounce address */
    t_fprintf(f, "%s\r\n", summstr);	/* pass that along also */

    for (r = rlist->next ;; r = r->next) { /* now add all recips */
	t_fprintf(f, "%s\r\n", r->addr); /* one recip per line */
	if (r == rlist)			/* end of circular list */
	    break;
    }	
    if (t_fclose(f) != 0) {
	t_perror1("rewrite_qfile: close ", tempname);
    } else {
	if (rename(tempname, fname) < 0)
	    t_perror1("rewrite_qfile: rename ", tempname);
    }
}

/* serv_connect -- 

    Connect to SMTP port on another server, exchange greetings.
    Returns an open connection, or NULL.

*/
t_file *serv_connect(char *hostname) {

    t_file	*f;			/* returned: open connection */
    int		sock;			/* the socket */
    struct hostent *host;	    	/* host entry for other server */
    struct servent *sp;			/* service entry */
    struct sockaddr_in sin;		/* server's address */
    int		on = 1;                 /* for setsockopt */    
    char	buf[MAX_STR];
    
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	t_perror("serv_connect: socket");
	return NULL;
    }
    
    sem_seize(&herrno_sem);		/* serialize access to gethostbyname */
    	
    /* if "name" begins with digit, treat as ip addr */
    if (isdigit(hostname[0])) {
	sin.sin_addr.s_addr = inet_addr(hostname);
	if (sin.sin_addr.s_addr == -1) {
	    t_errprint_s("serv_connect: ill-formed ip addr: %s", hostname);
	    sem_release(&herrno_sem);
	    return NULL;	
	}
    } else {
	if ((host = gethostbyname(hostname)) == NULL) {
	    t_sprintf(buf, "serv_connect: gethostbyname for %s fails: %d", 
	    		hostname, h_errno);
	    t_errprint(buf);
	    sem_release(&herrno_sem);
	    return NULL;
	}
	bcopy(host->h_addr, (char *)&sin.sin_addr, host->h_length);
    }
    
    sin.sin_family = AF_INET;
    
    if ((sp = getservbyname(SMTPPORT, "tcp")) == NULL) {
	t_perror1("serv_connect: getservbyname: ", SMTPPORT);
	sem_release(&herrno_sem);
	return NULL;
    }
    sin.sin_port = sp->s_port;	/* get port from services entry */    
    sp = NULL;			/* sppml */

    sem_release(&herrno_sem);	/* don't hold other people up while we connect */
    
    /* try to connect; may fail if other server is down */
    if (connect(sock, (struct sockaddr *)&sin, sizeof (sin)) < 0) {
	/* If they are merely down; log to file but not to console */
	if (pthread_errno() == ETIMEDOUT) {
	    t_sprintf(buf, "serv_connect: connection attempt to %s timed out",
	    			 hostname);
	    log_it(buf);
	} else if (pthread_errno() == ECONNREFUSED) {
	    t_sprintf(buf, "serv_connect: connection to %s refused",
	    			 hostname);
	    log_it(buf);	
	} else 
	    t_perror1("serv_connect: connecting to ", hostname);
	close(sock);
	return NULL;
    }

    /* enable periodic tickles */
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &on, sizeof(on)) < 0)
	t_perror("serv_connect: setsockopt (SO_KEEPALIVE)");
    /* non-blocking io mode */
    if (ioctl(sock, FIONBIO, (char *) &on) < 0)
	t_perror("serv_connect: ioctl (FIONBIO)");
	        
    /* connection established; try for greeting message */
    
    f = (t_file *) mallocf(sizeof(t_file));
    t_fdopen(f, sock);			/* associate file w/ socket */
    
    f->select = TRUE;			/* t_select should check this file */
    f->timeout = smtp_timeout * 60;	/* set timeout interval (seconds) */
    
    /* record name as debugging aid */
    t_sprintf(f->name, "SMTP to %s", hostname);

    if (!checkresponse(f, buf, SMTP_GREET)) {
	if (strlen(buf) == 0)
	    t_errprint_s("No SMTP response from %s.\n", hostname);
	else
	    t_errprint_ss("Unexpected greeting from %s:%s\n", hostname, buf);
	t_fclose(f);
	return NULL;
    }
    
    /* identify ourselves */
    t_sprintf(buf, "HELO %s\r\n", m_fullservname);
    t_puts(f, buf);
    
    if (!checkresponse(f, buf, SMTP_OK)) {
	t_fclose(f);
	return NULL;
    }
    
    return f;				/* all set!  return the connection */
}

/* checkresponse -- 

    Read response, compare status code against what we expect.
    Reads one line, then flushes any other input (puts file back into
    writing state.)

*/
boolean_t checkresponse(t_file *f, char *buf, int expect) {

    t_fseek(f, 0, SEEK_CUR);		/* make sure we're reading */
    t_gets(buf, MAX_STR, f);		/* get response */
    t_fflush(f);			/* set up to write again */
    
    return atoi(buf) == expect;
}

/* xmit_block -- 

    Transmit a piece of message (header, text, single enclosure).

    Transmission format is:
    
          +-------------------+
	  |       <len>       |  (4) byte length of block (incl header)
	  +-------------------+
	  |       <type>      |  (4) block type ("HEAD", etc.)
	  +-------------------+
	  |  block contents   |
	  |       | **********|  <- 0-3 padding bytes (mbz)
	  +-------------------+
	  
    The block length is a 32-bit binary value (msb first).
    Each block begins on a 4-byte boundary, so there are 0-3
    bytes of padding at the end of each block (the length
    field allows the receiver to locate & remove the padding).
*/
void xmit_block(t_file *conn, fileinfo *finfo, char *type) {

    u_bit32	len;
    
    len = finfo->len + BLOCK_HDLEN;		 
    t_putc(conn, (len >> 24) & 0xFF);	/* 32 bits, msb first */
    t_putc(conn, (len >> 16) & 0xFF);
    t_putc(conn, (len >>  8) & 0xFF);
    t_putc(conn, len & 0xFF);
    
    t_puts(conn, type);			/* send the type */
    
    finfocopy(conn, finfo);		/* and the data itself */
    
    while (len++ % 4)			/* the padding */
	t_putc(conn, 0);
	
    return;
}

/* send an already-open file */

void xmit_file(t_file *conn, t_file *f, char *type) {

    u_bit32	len;
    u_bit32	i;
    
    len = t_fseek(f, 0, SEEK_END) + BLOCK_HDLEN; 	/* compute len */
    t_fseek(f, 0, SEEK_SET);		/* then start at the beginning */
     
    t_putc(conn, (len >> 24) & 0xFF);	/* 32 bits, msb first */
    t_putc(conn, (len >> 16) & 0xFF);
    t_putc(conn, (len >>  8) & 0xFF);
    t_putc(conn, len & 0xFF);
    
    t_puts(conn, type);			/* send the type */

    for(i = 0; i < len - BLOCK_HDLEN; ++i) /* copy it */
	t_putc(conn, t_getc(f));
	
    while (len++ % 4)			/* add the padding */
	t_putc(conn, 0);
    
}

/* Transmit an enclosure.  Note that the enclosure header (in "enclhead" format)
is transmitted before the enclosure itself, so the length sent is longer than
than ep->finfo.len. */

void xmit_encl(t_file *conn, enclinfo *ep) {

    u_bit32	len;
    enclhead	eh;		/* enclhead in standard format */

    eh.encllen = htonl(ep->finfo.len);	/* create standard encl header */
    eh.typelen = htonl(strlen(ep->type));
    strcpy(eh.type, ep->type);		/* note: length < ENCLSTR_LEN */
    eh.namelen = htonl(strlen(ep->name));
    strcpy(eh.name, ep->name);		/* note: length < ENCLSTR_LEN */

    len = ep->finfo.len + EHEAD_LEN + BLOCK_HDLEN; /* total length we'll send */
    
    t_putc(conn, (len >> 24) & 0xFF);	/* 32 bits, msb first */
    t_putc(conn, (len >> 16) & 0xFF);
    t_putc(conn, (len >>  8) & 0xFF);
    t_putc(conn, len & 0xFF);
    
    t_puts(conn, "ENCL");	/* send the type */
    
    t_fwrite(conn, (char *) &eh, EHEAD_LEN); /* encl header */
    finfocopy(conn, &ep->finfo);	/* and the encl itself */
 
    while (len++ % 4)			/* add the padding */
	t_putc(conn, 0);
    
}

/* queue_fname -- 

    Generate pathname of file queued in spool directory for a given host.
*/

void queue_fname(char *fname, int hostnum, long qid) {

    t_sprintf(fname, "%s%s%s/%ld", m_spoolfs_name, SPOOL_DIR, m_server[hostnum], qid);

}

/* next_qid -- 

    Queue ids are sequential integers; the same sequence space is used for
    all queues.  Queue ids must be distinct from message ids because a message
    may be in a queue more than once (as a result of forwarding).  Note that
    (unlike message ids) queue ids are transient:  at startup, the next qid
    is initialized to one higher than the largest id currently in use; if the
    queues are empty at startup queue ids end up being reused.
*/

long next_qid () {

    long 	qid;
    
    pthread_mutex_lock(&global_lock);
    
    qid = next_q_id++;

    pthread_mutex_unlock(&global_lock);
    
    return qid;
}



