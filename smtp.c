/*  BlitzMail Server -- smtp server

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/smtp.c,v 3.6 98/10/21 16:10:23 davidg Exp Locker: davidg $
    
    Handle incoming SMTP connections.  In addition to standard SMTP
    transactions, this is also where incoming blitz-format transfers
    arrive.
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "smtp.h"
#include "client.h"
#include "mess.h"
#include "deliver.h"
#include "queue.h"
#include "binhex.h"

any_t smtp_serv(any_t smtp_);
void smtp_data(smtpstate *smtp);
void smtp_ehlo(smtpstate *smtp);
void smtp_expn(smtpstate *smtp);
void smtp_filtermatch(smtpstate *smtp);
void smtp_helo(smtpstate *smtp);
void smtp_help(smtpstate *smtp);
void smtp_mail(smtpstate *smtp);
void smtp_noop(smtpstate *smtp);
void smtp_quit(smtpstate *smtp);
void smtp_rcpt(smtpstate *smtp);
void smtp_rset(smtpstate *smtp);
void smtp_vrfy(smtpstate *smtp);
void smtp_xbtz(smtpstate *smtp);
void smtp_xfer(smtpstate *smtp);
long recv_block(t_file *conn, fileinfo *finfo, char *want, char *got, t_file *f);
long recv_encl(t_file *conn, enclinfo **ep);
void xfer_done(smtpstate *smtp, mbox *mb);
void recv_fold(smtpstate *smtp, mbox *mb);
void recv_list(smtpstate *smtp, mbox *mb);
void recv_mess(smtpstate *smtp, mbox *mb);
void recv_pref(smtpstate *smtp, mbox *mb);
void recv_vaca(smtpstate *smtp, mbox *mb);
boolean_t recv_xfermess(smtpstate *smtp, summinfo *summ, int fs, char *xfername);
void read_smtp_filters();

struct respstat {
	int	stat;
	char	*msg;
};

static struct respstat resptab[] = {	/* map RECIP_?? -> SMTP status */
    { SMTP_OK, "Recipient ok" },		/* RECIP_OK */
    { SMTP_BADRECIP, "Ambiguous user name" },	/* RECIP_AMBIG */
    { SMTP_BADRECIP, "No such user" },		/* RECIP_BADADDR */
    { SMTP_BADRECIP, "Mailing list access denied" },	/* RECIP_NO_SEND */
    { SMTP_NODND, "Name directory (DND) not available"}, /* RECIP_NO_DND */
    { SMTP_BADRECIP, "Forwarding loop detected"	} /* RECIP_LOOP */
};

/*^L smtp_init --

    Initialize SMTP state; before other threads spawned.
*/

void smtp_init() {

    /* set up conditions & semaphores */
    pthread_cond_init(&smtpmax_wait, pthread_condattr_default);
    sem_init(&smtp_filt_sem, "smtp_filt_sem");

    sem_seize(&smtp_filt_sem);
    read_smtp_filters();	/* read filter rules */
    sem_release(&smtp_filt_sem);
}

/* smtplisten --

    Set up a socket listening for smtp connections.  When one arrives,
    accept it and spawn a thread to deal with it.
*/

any_t smtplisten (any_t zot) {

    int			fd;	/* socket we listen on */
    struct sockaddr_in	sin;	/* its addr */
    struct servent	*sp;	/* services entry */
    int			connfd; /* new connection */
    int			len = sizeof(sin); /* addr length */
    int			on = 1;	/* for setsockopt */
    smtpstate		*smtp = NULL; /* connection state */
    static t_file	listen_f; /* t_file for listening socket */
    pthread_t		thread;	/* thread var */   
    static char	 	logbuf[MAX_STR];
    
    setup_signals();		/* set up signal handlers for new thread */
    setup_syslog();
        
    sem_seize(&herrno_sem);		/* serialize access to getservbyname */
        
    if ((sp = getservbyname(SMTPPORT, "tcp")) == NULL) {
	t_errprint_s("smtplisten: unknown service: %s", SMTPPORT);
	exit(1);
    }    
	
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = sp->s_port;	/* port we listen on */
    sp = NULL;			/* sppml */

    sem_release(&herrno_sem);
    
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	t_perror("smtplisten: socket: ");
	exit(1);
    }
    
    /* set REUSEADDR so we won't get an EADDRINUSE if there are connections
       from our previous incarnation lingering in TIME_WAIT */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) < 0)
	t_perror("smtplisten: setsockopt (SO_REUSEADDR)");
    
    if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	t_perror("smtplisten: bind");
	exit(1);
    }

    listen(fd, 5);			/* accept connections (up to 5 in advance) */

    t_fdopen(&listen_f, fd);		/* set up (useless) t_file for the socket... */
    strcpy(listen_f.name, "smtplisten"); /* ...just to get it entered in t_fdmap */
        
    for (;;) {				/* now just keep accepting */
    
	if (!smtp)			/* get state block */
	    smtp = (smtpstate *) mallocf(sizeof(smtpstate));

	/* if too many connections already, don't accept any more */
	pthread_mutex_lock(&global_lock);	
	while (smtp_num >= smtp_max) 	/* too many smtp connections? */
	    pthread_cond_wait(&smtpmax_wait, &global_lock); 
	pthread_mutex_unlock(&global_lock);	
	    
	connfd = accept(fd, (struct sockaddr *) &smtp->remoteaddr, &len);
	
	if (connfd < 0) {
	    t_perror("smtplisten: accept");
	    sleep(5);
	    continue;			/* try again */
	}
	
	if (smtp_debug) {
	    t_sprintf(logbuf, "[%d] connection accepted (currently %d incoming SMTP connections)\n", 
	    	smtp, smtp_num+1);
	    log_it(logbuf);
	}
	
	/* enable periodic tickles */	
	if (setsockopt(connfd, SOL_SOCKET, SO_KEEPALIVE, (char *) &on, sizeof(on)) < 0)
	    t_perror("smtplisten: setsockopt (SO_KEEPALIVE)");
	/* non-blocking io mode */
	if (ioctl(connfd, FIONBIO, (char *) &on) < 0)
	    t_perror("smtplisten: ioctl (FIONBIO)");
	    
	t_fdopen(&smtp->conn, connfd);		/* set up t_file for the conn */
	strcpy(smtp->conn.name, "Incoming SMTP"); /* set up name (debugging) */
	smtp->conn.select = TRUE;	/* t_select should check this fd */
	
	pthread_mutex_lock(&global_lock);
	++smtp_num;			/* one more connection */
	pthread_mutex_unlock(&global_lock);
	
	if (smtp_num >= smtp_max)
	    t_errprint("All SMTP connections in use...\n");
	
	if (pthread_create(&thread, generic_attr, 
		(pthread_startroutine_t) smtp_serv, (pthread_addr_t) smtp) < 0) {
	    t_perror("smtplisten: pthread_create");
	    if (t_closefd(smtp->conn.fd) < 0)	/* couldn't create thread, close conn */
		t_perror("smtplisten: close");
	    t_free(smtp);
	    pthread_mutex_lock(&global_lock);
	    --smtp_num;			/* connection failed; decrement count */
	    pthread_mutex_unlock(&global_lock);
	    sleep(5);
	} else {
	    pthread_detach(&thread);
	}
	
	smtp = NULL;			/* it's their state now */
    }
}

/* smtp_serv --

    Thread to handle a single SMTP connection.
    
*/

any_t smtp_serv(any_t smtp_) {

    smtpstate	*smtp;			/* state variables */
    struct hostent *hinfo;		/* remote host info */
    char	*logbuf = NULL;		/* for debugging log */
    
    smtp = (smtpstate *) smtp_;

    logbuf = mallocf(MAX_STR + SMTP_CMD_MAX);

    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
    
    smtp->reciplist = NULL;		/* no recips */
    smtp->recipcount = 0;
    smtp->mail = FALSE;			/* initial state */
    smtp->done = FALSE;
    smtp->peer = -1;
    
    /* set a timeout to get rid of connections from dead hosts */
    smtp->conn.timeout = smtp_timeout * 60; /* (seconds) */
    
    if (smtp_debug) {
    	t_sprintf(logbuf, "[%d] starting thread", smtp);
	log_it(logbuf);
    }
    
    /* try a reverse lookup to turn addr into hostname */

    sem_seize(&herrno_sem);		/* get access to gethostby[*] */
    hinfo = gethostbyaddr((char *) &smtp->remoteaddr.sin_addr, sizeof(smtp->remoteaddr.sin_addr), AF_INET);
    if (hinfo)
	strcpy(smtp->remotehost, hinfo->h_name); /* use name if possible */
    else {				/* just have addr; use that */
    	pthread_mutex_lock(&inet_ntoa_lock);	/* get access to inet_ntoa */
    	strcpy(smtp->remotehost, inet_ntoa(smtp->remoteaddr.sin_addr));
    	pthread_mutex_unlock(&inet_ntoa_lock);
    }
    sem_release(&herrno_sem);
    
    if (smtp_debug && logbuf) {
	t_sprintf(logbuf, "[%d] remote host is %s", smtp, smtp->remotehost);
	log_it(logbuf);
    }
    
    if (server_shutdown) {		/* if closed now, don't even greet */
	t_fprintf(&smtp->conn, "%d %s shutting down now.\r\n", SMTP_SHUTDOWN, 
			m_fullservname);
	smtp->done = TRUE;			
    } else {
	smtp_filtermatch(smtp);		/* check for source-address-based filtering */
	if (smtp->filterlevel == FILT_REJECT) {
	    t_fprintf(&smtp->conn, "%d Connection from your address denied.\r\n",
			 smtp->filt_errcode);
	    pthread_mutex_lock(&inet_ntoa_lock);
	    t_sprintf(logbuf, "Refuse connection from %s [%s]",
			 smtp->remotehost, inet_ntoa(smtp->remoteaddr.sin_addr));
	    pthread_mutex_unlock(&inet_ntoa_lock);
	    log_it(logbuf);
	    smtp->done = TRUE;
	} else {
	    t_fprintf(&smtp->conn, "%d %s server ready (%s).\r\n", SMTP_GREET,
			m_fullservname, server_vers);
	}
    }

    while (!smtp->done) {		/* loop processing commands */
    
	if (server_shutdown) {		/* if we're now closing */
	    t_fprintf(&smtp->conn, "%d %s shutting down now.\r\n", SMTP_SHUTDOWN, 
			    m_fullservname);
	    break;
	}
	
	t_fflush(&smtp->conn);		/* write last response */
	
	t_fseek(&smtp->conn, 0, SEEK_CUR); /* set up to read next command */
	
	if (t_gets(smtp->comline, sizeof(smtp->comline), &smtp->conn) == NULL)
	    break;			/* connection lost */
	
	t_fflush(&smtp->conn);		/* get set to write */
	
	if (smtp_debug && logbuf) {
	    t_sprintf(logbuf, "[%d] cmd: %s", smtp, smtp->comline);
	    log_it(logbuf);
	}	    
	
	if (strncasecmp(smtp->comline, "DATA", 4) == 0)
	    smtp_data(smtp);
	else if (strncasecmp(smtp->comline, "EHLO", 4) == 0)
	    smtp_ehlo(smtp);
	else if (strncasecmp(smtp->comline, "EXPN", 4) == 0)
	    smtp_expn(smtp);
	else if (strncasecmp(smtp->comline, "HELO", 4) == 0)
	    smtp_helo(smtp);
	else if (strncasecmp(smtp->comline, "HELP", 4) == 0)
	    smtp_help(smtp);
	else if (strncasecmp(smtp->comline, "MAIL", 4) == 0)
	    smtp_mail(smtp);
	else if (strncasecmp(smtp->comline, "NOOP", 4) == 0)
	    smtp_noop(smtp);
	else if (strncasecmp(smtp->comline, "QUIT", 4) == 0)
	    smtp_quit(smtp);
	else if (strncasecmp(smtp->comline, "RCPT", 4) == 0)
	    smtp_rcpt(smtp);
	else if (strncasecmp(smtp->comline, "RSET", 4) == 0)
	    smtp_rset(smtp);
	else if (strncasecmp(smtp->comline, "VRFY", 4) == 0)
	    smtp_vrfy(smtp);
	else if (strncasecmp(smtp->comline, "XBTZ", 4) == 0)
	    smtp_xbtz(smtp);
	else if (strncasecmp(smtp->comline, "XFER", 4) == 0)
	    smtp_xfer(smtp);
	else
	    t_fprintf(&smtp->conn, "%d Syntax error; unknown command.\r\n", SMTP_BADCMD);
    }
    
    t_fflush(&smtp->conn);		/* flush any last output */
    
    if (t_closefd(smtp->conn.fd) < 0)	/* close (don't free; no separate t_file here) */
	t_perror("smtp_serv: close");
	
    free_recips(&smtp->reciplist);	/* clean up recipients */
    
    t_free(smtp);			/* free up smtp state vars */
  
    pthread_mutex_lock(&global_lock);	
    --smtp_num;				/* one less connection */
    pthread_mutex_unlock(&global_lock);	

    pthread_cond_signal(&smtpmax_wait);	/* might be safe to accept connections again */

    if (smtp_debug && logbuf) {
	t_sprintf(logbuf, "[%d] thread done", smtp);
	log_it(logbuf);
    }
    if (logbuf) t_free(logbuf);
    
    return 0;				/* and end the thread */
}

/* smtp_data --

    Receive a message.  
    
*/

void smtp_data(smtpstate *smtp) {

    fileinfo		head;		/* message header */
    fileinfo		text;		/* and text */
    fileinfo		newtext;	/* edited text (binhex removed) */
    t_file		*headf = NULL;
    t_file		*textf = NULL;
    t_file		*newtextf = NULL;
    summinfo		*summ = NULL;	/* summary info for new mesage */
    long		mtype;		/* message type */
    boolean_t		mime_seen = FALSE;
    boolean_t		binhex_seen = FALSE;
    enclinfo		*encl = NULL;	/* de-binhex'd enclosures */
    char		date[64];	/* RFC822 format date */
    boolean_t		inheader = TRUE;
    char		line[SMTP_MAXLINE];
    char		binhexerr[MAX_STR];
    char		*linep;
    long		messid;
    messinfo		mi;		/* message delivery info */
    recip		*r;
    
    head.fname[0] = text.fname[0] = newtext.fname[0] = 0;
    
    /* are we ready for a message? */
    if (!smtp->mail || !smtp->reciplist) {
	t_fprintf(&smtp->conn, "%d Specify sender & recips first.\r\n", SMTP_BADSEQ);
	return;
    }
    
    temp_finfo(&head);			/* generate tempfile names */
    
    /* now open the files */
    if ((headf = t_fopen(head.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("smtp_data: open ", head.fname);
	t_fprintf(&smtp->conn, "%d File error.\r\n", SMTP_FAIL);
	goto cleanup;
    }

    temp_finfo(&text);
    if ((textf = t_fopen(text.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("smtp_data: open ", text.fname);
	t_fprintf(&smtp->conn, "%d File error.\r\n", SMTP_FAIL);
	goto cleanup;
    }
    
    messid = next_messid();		/* generate messid */
    
    get_date(date);
    if (m_smtp_disclaimer)		/* add any X-Disclaimer: line */
	t_fprintf(headf, "%s\r", m_smtp_disclaimer);
    t_fprintf(headf, "Return-path: %s\r", smtp->sender);
    if (smtp->reciplist->next == smtp->reciplist) {
	pthread_mutex_lock(&inet_ntoa_lock);
	t_fprintf(headf, "Received: by %s (%s) via SMTP from %s [%s]\r  for %s id <%ld> %s\r",
			m_fullservname, m_hostname, smtp->remotehost, 
			inet_ntoa(smtp->remoteaddr.sin_addr),
			smtp->reciplist->addr, messid, date);
	pthread_mutex_unlock(&inet_ntoa_lock);
    } else {				/* multiple recipients */
        pthread_mutex_lock(&inet_ntoa_lock);
	t_fprintf(headf, "Received: by %s (%s) via SMTP from %s [%s]\r  id <%ld> %s\r",
			m_fullservname, m_hostname, smtp->remotehost,
			inet_ntoa(smtp->remoteaddr.sin_addr),
			messid, date);    
        pthread_mutex_unlock(&inet_ntoa_lock); 
    }
    t_fprintf(&smtp->conn, "%d Begin message input.\r\n", SMTP_SENDON);
    t_fflush(&smtp->conn);		/* send the prompt */
    
    t_fseek(&smtp->conn, 0, SEEK_CUR);	/* now get set to read it */
    
    /* read message, split into header & text */
    
    for (;;) {
	if (t_gets(line, sizeof(line), &smtp->conn) == NULL)
	    goto cleanup;		/* connection lost */
	    
	if (strcmp(line, ".") == 0)	/* single '.' marks the end */
	    break;
	    
	if (inheader && line[0] == 0) 	/* blank line ends header */
	    inheader = FALSE;
	else {
	    linep = line;
	    if (line[0] == '.')		/* drop doubled '.' at start */
		++linep;
		
	    if (inheader) {		/* partition header & text */
		if (strncasecmp(linep,"mime-version:", 
				strlen("mime_version:")) == 0)
		    mime_seen = TRUE;	/* note that it's MIME */
		t_fprintf(headf, "%s\r", linep);
	    } else {
		if (is_binhex_tag(linep))
		    binhex_seen = TRUE;	/* watch for binhex as we copy data */
		t_fprintf(textf, "%s\r", linep);
	    }
	}
    }
    
    t_fflush(&smtp->conn);
    
    mtype = MESSTYPE_RFC822;		/* by default, not Blitz-format */
    
    /* if message includes binhex, split message into text and enclosures
       MIME messages are exempt */
    if (binhex_seen && m_recvbinhex && !mime_seen) {
	temp_finfo(&newtext);		/* need another temp file for text */
	if ((newtextf = t_fopen(newtext.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	    t_perror1("smtp_data: open ", newtext.fname);
	    t_fprintf(&smtp->conn, "%d File error.\r\n", SMTP_FAIL);
	    goto cleanup;
	}
	encl = binhex2encl(textf, newtextf, binhexerr);	/* unbinhex & create enclosures */
	if (encl == NULL) {		/* bad binhex -- punt */
	    t_sprintf(line, "Message %ld De-binhex failed: %s", messid, binhexerr);
	    log_it(line);
	    t_fclose(newtextf);		/* just forget about new file */
	    finfoclose(&newtext);
	} else {
	    t_fclose(textf);		/* discard raw binhex text */
	    finfoclose(&text);
	    textf = newtextf;		/* use converted binhex + enclosures */
	    bcopy((char *) &newtext, (char *) &text, sizeof(text));
	}
	newtextf = NULL;
	newtext.fname[0] = 0;
	
	mtype = MESSTYPE_BLITZ;		/* using a Blitz-style enclosure */
    }
    
    head.offset = 0;
    head.len = t_fseek(headf, 0, SEEK_END); /* compute lengths of the pieces */
    text.offset = 0;
    text.len = t_fseek(textf, 0, SEEK_END); /* compute lengths of the pieces */
    
    t_fclose(headf);			/* done with these */
    t_fclose(textf);
    headf = textf = NULL;
    
    /* now scan the header to generate summary info */
    if ((summ = mess_scan_head(&head, &text, encl, mtype)) == NULL) {
	t_fprintf(&smtp->conn, "%d File error\r\n", SMTP_FAIL);
	goto cleanup;
    }
    date_time(summ->date, summ->time);		/* update current date & time */
    summ->expire = pick_expire(summ);		/* choose expiration date */
    summ->read = FALSE;
    
    summ->messid = messid;			/* but use the id we assigned */
    pthread_mutex_lock(&inet_ntoa_lock);
    t_sprintf(line, "Incoming message %ld from %s %s; via %s [%s]; %ld bytes %ld enclosures",
    			summ->messid, summ->sender, smtp->sender, 
			smtp->remotehost, inet_ntoa(smtp->remoteaddr.sin_addr),
			summ->totallen, summ->enclosures);
    pthread_mutex_unlock(&inet_ntoa_lock);
    log_it(line);
    ++m_recv_smtp;				/* statistics: count incoming smtp */
        
    /* log each recipient 
       Note that "nosend" recipients *are* logged, to facilitate tracking
       down forwarding problems */

    for (r = smtp->reciplist->next ;; r = r->next) {
        if (strlen(r->addr) == 0)
	    t_sprintf(line, "Message %ld recipient = %s", summ->messid, r->name);
	else if (strlen(r->addr) == 0)
	    t_sprintf(line, "Message %ld recipient = %s", summ->messid, r->addr);
	else
	    t_sprintf(line, "Message %ld recipient = %s (%s)", summ->messid, r->addr, r->name);
	if (r->nosend)
	    strcat(line, " [ FORWARDED ]");
	log_it(line);

        if (r == smtp->reciplist)               /* end of circular list */
            break;
    }

    if (!mess_setup(summ->messid, &head, &text, encl, &mi, m_spool_filesys, summ->type)) {
	t_fprintf(&smtp->conn, "%d File error\r\n", SMTP_FAIL);
	goto cleanup;
    }	
	   	
    /* handle any bad addresses in recip list.  Note that the bounce
       goes to the envelope address, not the From: or Reply-To: */
	    
    if (smtp->sender[0]) {		/* don't bounce if sender null */
	do_bounces(smtp->sender, smtp->reciplist, &head, &text, summ);
	do_vacations(smtp->reciplist, &head, summ); /* vacations for folks that fwd */
    }
    
    /* export message to SMTP queues for internet recips */
    /* set hextext TRUE -- if there are text encls here, it's because they arrived
       as binhex, so turning them back into binhex on the way out is proper */
    internet(smtp->sender, smtp->reciplist, NULL, &head, &text, encl, summ, TRUE);

    /* and add message to our queue(s) for blitz recipients */
    blitzdeliver(smtp->sender, smtp->reciplist, NULL, &mi, &head, &text, encl, summ, TRUE);
    
    mess_done(&mi);			/* release our link to message; it's queued */

    t_fprintf(&smtp->conn, "%d Message accepted\r\n", SMTP_OK);
    
    cleanup:				/* here to clean up & exit */
    
    free_recips(&smtp->reciplist);	/* clean up recip list */
    if (summ)
	t_free(summ);			/* and summary info */
    smtp->recipcount = 0;
    if (headf)				/* if we didn't yet, close files now */
	t_fclose(headf);
    if (textf)
	t_fclose(textf);
    if (newtextf)
    	t_fclose(newtextf);
    headf = textf = newtextf = NULL;
    finfoclose(&head);			/* discard temp files */
    finfoclose(&text);	
    finfoclose(&newtext);
    clean_encl_list(&encl);		/* and any enclosure files */
    
    smtp->mail = FALSE;			/* reset state */
    strcpy(smtp->sender, "");		/* clear sender */
    
}
/* smtp_ehlo --

    Answer greeting from SMTP client; announce what extended capabilities we support.
      
    Search table of peer servers for the name; set smtp->peer
    if matched.
    
    (***require validation for that???)
*/

void smtp_ehlo(smtpstate *smtp) {

    char	*them;
    char	logbuf[MAX_ADDR_LEN+MAX_STR];
    
    them = smtp->comline + strlen("HELO");
    while(*them == ' ')
	++them;
	    
    /* see if it's a peer server; match HELO command against what IP address indicates */
    if ((smtp->peer = blitzserv_match(them)) >= 0) {
    	if (blitzserv_match(smtp->remotehost) != smtp->peer) {
	    t_fprintf(&smtp->conn, "%d-%s; why do you call yourself %s, %s?.\r\n", SMTP_OK,
				m_fullservname, them, smtp->remotehost);
		smtp->peer = -1;
	    	t_sprintf(logbuf, "%s is calling itself %s", smtp->remotehost, them);
		log_it(logbuf);
	} else {
	    t_fprintf(&smtp->conn, "%d-%s; hello again.\r\n", SMTP_OK,
				m_fullservname);
	    smtp->xferok = TRUE;
	}
    } else {
    	/* check list of hosts that's allowed to transfer to us */
    	smtp->xferok = (hostmatch(them, m_xferok) >= 0);
	t_fprintf(&smtp->conn, "%d-%s; pleased to meet you.\r\n", SMTP_OK,
			    m_fullservname);
    }
    
    /* record host name in file state, for debugging */
    t_sprintf(smtp->conn.name, "ESMTP from %s'", them);
    
    if (mess_max_len != 0) {		/* if hard message limit configured */
	t_fprintf(&smtp->conn, "%d-SIZE %ld\r\n", SMTP_OK, mess_max_len); /* announce it */
    } else {
	t_fprintf(&smtp->conn, "%d-SIZE\r\n", SMTP_OK);    /* we can parse SIZE option */
    }
    
    t_fprintf(&smtp->conn, "%d-EXPN\r\n", SMTP_OK);    /* list optional commands supported */
    t_fprintf(&smtp->conn, "%d-XBTZ\r\n", SMTP_OK);
    t_fprintf(&smtp->conn, "%d HELP\r\n", SMTP_OK);
  

}

/* smtp_expn --

    Expand (public) mailing list.  
    
*/

void smtp_expn(smtpstate *smtp) {

    ml_data	*ml;			/* mailing list */
    char	name[MAX_STR];		/* list name */
    char	*p, *q;
    
    strtrimcpy(name, smtp->comline + strlen("EXPN"));
    
    if (!pubml_get(name, &ml)) {	/* get the list */
	t_fprintf(&smtp->conn, "%d No such mailing list: %s\r\n",
				SMTP_BADRECIP, name);
	return;
    }
    
    /* make sure it's world-readable */
    if (!(pubml_acc(NULL, name) & LACC_READ)) {
	t_fprintf(&smtp->conn, "%d Mailing list access denied: %s\r\n",
				SMTP_BADRECIP, name);
	ml_clean(&ml);
	return;
    }

    for( ; ml; ml = ml->next) {		/* for each block of list */  
      	
	p = ml->data;			/* deal with newline terminators... */
	while (p < ml->data + ml->len) { /* for each name in block */
	    q = index(p, '\n');
	    if (!q)
		q = ml->data + ml->len;
	    else 
	    	*q++ = 0;		/* terminate string */
		
	    if (ml->next || q < ml->data + ml->len)
		t_fprintf(&smtp->conn, "%d-%s\r\n", SMTP_OK, p);
	    else 			/* last line */
		t_fprintf(&smtp->conn, "%d %s\r\n", SMTP_OK, p);
	    p = q;
	}
    }
    
    ml_clean(&ml);
}

/*^L smtp_filtermatch --

    Check source IP address of incoming SMTP connection against table of anti-spam
    filters. The table is scanned in order, stopping at the first match found and
    setting smtp->filterlevel according to the match. There are 3 choices:

	FILT_REJECT -- the connection is immediately rejected
	FILT_ACCEPT -- the connection is accepted unreservedly
	FILT_NORELAY -- the connection is accepted, but all we will only accept
			recipients in our local domain (no relaying)

    If no match is found, the default action is FILT_ACCEPT.

*/
void smtp_filtermatch(smtpstate *smtp) {

    int		len;			/* name length */
    u_bit32	addr;			/* addr to match */
    ipfilt	*p;			/* current filter */

    sem_seize(&smtp_filt_sem);

    read_smtp_filters();		/* make sure table is up to date */

    smtp->filterlevel = FILT_ACCEPT;	/* if no match, default is ACCEPT */

    addr = ntohl(smtp->remoteaddr.sin_addr.s_addr); /* get host byte order for compare */

    /* search table in order comparing name or addr & mask */
    for (p = smtp_filt; p != NULL; p = p->next) {
	switch(p->kind) {
	    
	   case FILT_ADDR:			/*  go by ip addr */
		if ((addr & p->mask) == (p->addr & p->mask)) {
		    smtp->filterlevel = p->action;
		    smtp->filt_errcode = p->errcode;
		    goto done;			/* stop at first match */
		}
		break;
	   case FILT_NAME:			/* compare by name */
		len = strlen(p->name);		/* length of name filter */
		len = strlen(smtp->remotehost) - len;	/* length difference for suffix match */
		if (len >= 0 && strcasecmp(smtp->remotehost+len, p->name) == 0) {
		    smtp->filterlevel = p->action;
		    smtp->filt_errcode = p->errcode;
		    goto done;                  /* stop at first match */
		}
    		break;

	    case FILT_LOGIN:			/* match if "recent" login */
		if (recent_login(ntohl(smtp->remoteaddr.sin_addr.s_addr))) {
                    smtp->filterlevel = p->action;
                    smtp->filt_errcode = p->errcode;
                    goto done;                  /* stop at first match */
		}
	        break;
	}
    }

done:
    sem_release(&smtp_filt_sem); 
}
/* smtp_helo --

    Answer greeting from SMTP client.  
    Search table of peer servers for the name; set smtp->peer
    if matched.
    
    (***require validation for that???)
*/

void smtp_helo(smtpstate *smtp) {

    char	*them;
    char	logbuf[MAX_ADDR_LEN+MAX_STR];
    
    them = smtp->comline + strlen("HELO");
    while(*them == ' ')
	++them;
	    
    /* see if it's a peer server; match HELO command against what IP address indicates */
    if ((smtp->peer = blitzserv_match(them)) >= 0) {
    	if (blitzserv_match(smtp->remotehost) != smtp->peer) {
	    t_fprintf(&smtp->conn, "%d %s; why do you call yourself %s, %s?.\r\n", SMTP_OK,
				m_fullservname, them, smtp->remotehost);
		smtp->peer = -1;
	    	t_sprintf(logbuf, "%s is calling itself %s", smtp->remotehost, them);
		log_it(logbuf);
	} else {
	    t_fprintf(&smtp->conn, "%d %s; hello again.\r\n", SMTP_OK,
				m_fullservname);
	    smtp->xferok = TRUE;
	}
    } else {
    	/* check list of hosts that's allowed to transfer to us */
    	smtp->xferok = (hostmatch(them, m_xferok) >= 0);
	t_fprintf(&smtp->conn, "%d %s; pleased to meet you.\r\n", SMTP_OK,
			    m_fullservname);
    }
    
    /* record host name in file state, for debugging */
    t_sprintf(smtp->conn.name, "SMTP from %s'", them);
}

/* smtp_help --

    Print brief list of commands.
    
*/

void smtp_help(smtpstate *smtp) {

    t_fprintf(&smtp->conn, "%d-%s; BlitzMail server here\r\n", SMTP_HELP,
    			m_fullservname);

    t_fprintf(&smtp->conn, "%d-\r\n", SMTP_HELP);
    t_fprintf(&smtp->conn, "%d-Commands:\r\n", SMTP_HELP);
    t_fprintf(&smtp->conn, "%d-\r\n", SMTP_HELP);
    t_fprintf(&smtp->conn, "%d-    DATA  EXPN  HELO  HELP  MAIL\r\n", SMTP_HELP);
    t_fprintf(&smtp->conn, "%d-    NOOP  QUIT  RCPT  RSET  VRFY\r\n", SMTP_HELP);
    t_fprintf(&smtp->conn, "%d-    XBTZ\r\n", SMTP_HELP);
    t_fprintf(&smtp->conn, "%d-\r\n", SMTP_HELP);
    t_fprintf(&smtp->conn, "%d Good Luck.\r\n", SMTP_HELP);
    
}

/* smtp_mail --

    Begin new message, read sender's envelope address.
    
    MAIL FROM:<foo@bar>
*/

void smtp_mail(smtpstate *smtp) {

    char	*comp;			/* current position in command */
    char	*p;			/* end of address */
    int		len;
    boolean_t	badaddr = FALSE;

    free_recips(&smtp->reciplist); 	/* clear out any old message */
    smtp->mail = FALSE;			/* be pessimistic */
    
    comp = smtp->comline + strlen("MAIL"); /* skip "MAIL" */
    if (strncasecmp(comp, " FROM:", strlen(" FROM:")) != 0) {
	t_fprintf(&smtp->conn, "%d Expected FROM:\r\n", SMTP_BADARG);
	return;
    }
    comp += strlen(" FROM:");
    if (*comp == ' ')
	++comp;
    if (strlen(comp) >= MAX_ADDR_LEN) {
	t_fprintf(&smtp->conn, "%d Address too long.\r\n", SMTP_BADCMD);
	return;    
    }
    
    for (p = comp; *p && *p != '>'; )	/* locate end of address */
    	++p;
	
    if (*comp != '<' || *p != '>' || p - comp >= MAX_ADDR_LEN) {
	t_fprintf(&smtp->conn, "%d Bad sender address.\r\n", SMTP_BADARG);
	return;    
    }
    
    /* leave <>s in place for now; isinternet will strip them iff they
       are unneeded */
    len = p - comp + 1;
    strncpy(smtp->sender, comp, len); smtp->sender[len] = 0;		
    
    /* check address syntax; note that a null sender IS legal */
    badaddr = FALSE;
    if (strcmp(smtp->sender, "<>") == 0)
	strcpy(smtp->sender, "");
    else if (!isinternet(smtp->sender, &badaddr) || badaddr) {
	t_fprintf(&smtp->conn, "%d Bad sender address.\r\n", SMTP_BADARG);
	return;    
    }
    
    comp = p + 1;		/* skip past the address */
    
    /* parse options now */
    for (;;) {
    	while (isspace(*comp))	/* skip whitespace between options */
	    ++comp;
	    
	if (*comp == 0)		/* end of line - done */
	    break;
	    
	/* Message size option */
	if (strncasecmp(comp, "SIZE", 4) == 0) {
	    long	size;

	    comp += 4;		/* skip "SIZE" */
	    
	    if (*comp != '=' || !isdigit(*++comp)) {
		t_fprintf(&smtp->conn, "%d SIZE requires a value.\r\n", SMTP_BADARG);
		return;    
	    }
	    comp = strtonum(comp, &size);	/* pick up announced size */
	    
	    /* check against configured hard limit */
	    if (mess_max_len && size > mess_max_len) {
		t_fprintf(&smtp->conn, "%d Max message size is %ld.\r\n", 
				SMTP_RECIPMAX, mess_max_len);
	    	return;
	    }
	    
	    /* check against current amount of temp space free */
	    if (!check_temp_space(size)) {
		t_fprintf(&smtp->conn, "%d Insufficient disk space; try another time.\r\n", 
				SMTP_DISKFULL);
	    	return;	    
	    }
	} else {
	    t_fprintf(&smtp->conn, "%d Unrecognized parameter: %s\r\n", SMTP_BADARG, comp);
	    return;
	}
    }
					    
    smtp->mail = TRUE;			/* ok! */   
    
    t_fprintf(&smtp->conn, "%d %s: sender ok.\r\n", SMTP_OK, smtp->sender);
    
}

/* smtp_noop --

    Nothing.
*/

void smtp_noop(smtpstate *smtp) {

    t_fprintf(&smtp->conn, "%d Nothing accomplished.\r\n", SMTP_OK);
    
}

/* smtp_quit --

    Indicate that connection should be closed by setting smtp->done.
*/

void smtp_quit(smtpstate *smtp) {

    t_fprintf(&smtp->conn, "%d %s saying so long for now!\r\n", SMTP_BYE,
			m_fullservname);
    
    smtp->done = TRUE;
}

/* smtp_rcpt --

    Accept a recipient name.  The recipient is resolved immediately, we return
    a bad status if there's a problem.  But, if the recipient resolves to a mailing
    list which contains some bad addresses, we accept the recipient as long as
    there are ANY good addrs in the list; the message will be delivered to as
    many members as possible and then bounces are generated for the bad ones.
    
    Finally, if a "no dnd" status crops up anywhere, we must be sure to give a
    temporary error status to the sender, so they'll retry later & not give up.

    If anti-spam filtering is configured, and this connection (based on the sender's
    IP address) is in the FILT_NORELAY filter mode, reject any recipients that are
    not within the local domain.
*/

void smtp_rcpt(smtpstate *smtp) {

    recip	*newrecips;		/* recips in this batch */
    int		recipcount = 0;		/* how many */
    boolean_t	good = FALSE;		/* good one seen? */
    boolean_t	nodnd = FALSE;		/* "dnd down" seen? */
    char	*comp;			/* current pos in command line */
    char        logbuf[MAX_ADDR_LEN+MAX_STR]; 
    boolean_t	badaddr = FALSE;
    	
    recip 	*r;
    if (!smtp->mail) {
	t_fprintf(&smtp->conn, "%d Specify sender first.\r\n", SMTP_BADSEQ);
	return;
    }
    
    comp = smtp->comline + 4;		/* skip "RCPT" */
    if (strncasecmp(comp, " TO:", strlen(" TO:")) != 0) {
	t_fprintf(&smtp->conn, "%d Expected TO:\r\n", SMTP_BADARG);
	return;
    }
    comp += strlen(" TO:");
    if (*comp == ' ')
	++comp;
    if (strlen(comp) >= MAX_ADDR_LEN) {
	t_fprintf(&smtp->conn, "%d Address too long.\r\n", SMTP_BADCMD);
	return;    
    }
    
    if (*comp != '<' || comp[strlen(comp)-1] != '>' || strlen(comp) == 2) {
	t_fprintf(&smtp->conn, "%d Bad recipient address.\r\n", SMTP_BADARG);
	return;    
    }
    
    ++comp;				/* strip <>s */
    comp[strlen(comp)-1] = 0;
    
    if (smtp->filterlevel == FILT_NORELAY && isinternet(comp, &badaddr)
		          && (badaddr || !in_local_domain(comp))) {
        t_fprintf(&smtp->conn, "%d Relaying refused.\r\n", 
			smtp->filt_errcode);
        pthread_mutex_lock(&inet_ntoa_lock);    /* get access to inet_ntoa */
	t_sprintf(logbuf, "Refuse relaying from %s [%s]; rcpt=%s", 
			smtp->remotehost, inet_ntoa(smtp->remoteaddr.sin_addr), comp);
	pthread_mutex_unlock(&inet_ntoa_lock);
	log_it(logbuf); 
        return;
    }
    newrecips = resolve(comp, NULL, &recipcount); /* resolve it */
    
    if (recipcount + smtp->recipcount > ADDR_MAX_RECIPS) {
	free_recips(&newrecips);	/* too many; free new batch */
	t_fprintf(&smtp->conn, "%d Too many recipients, max is: .\r\n", SMTP_RECIPMAX, ADDR_MAX_RECIPS);
	return;
    }
    smtp->recipcount += recipcount;
    
    /* run through statuses & see if there are any good ones */
    for (r = newrecips->next ;; r = r->next) {
    
	if (r->stat == RECIP_OK && !r->nosend)
	    good = TRUE;		/* at least one good one */
	else if (r->stat == RECIP_NO_DND)
	    nodnd = TRUE;		/* dnd is down */
	if (r == newrecips)
	    break;			/* end of circular list */
    }
    
    if (nodnd) {			/* reject w/ temp fail if no DND */
	t_fprintf(&smtp->conn, "%d Name directory (DND) not available.\r\n", SMTP_NODND);
	free_recips(&newrecips);
    } else if (!good) {			/* no valid recips - nonrecoverable */
	t_fprintf(&smtp->conn, "%d %s: %s\r\n",  /* pick one error at random */ 
		resptab[newrecips->stat].stat, resptab[newrecips->stat].msg,
		 newrecips->name ? newrecips->name : newrecips->addr);
	free_recips(&newrecips);	
    } else {				/* ok, add these to list */
	if (smtp->reciplist) {		/* any old ones? */
	    r = newrecips->next;	/* new head */
	    newrecips->next = smtp->reciplist->next; /* new tail -> old head */
	    smtp->reciplist->next = r;	/* old tail -> new */
	}
	smtp->reciplist = newrecips;	/* new tail */
	
	t_fprintf(&smtp->conn, "%d %s: recipient ok\r\n", SMTP_OK, comp);	
    }
}

/* smtp_rset --

    Reset state -- clear any recipients.
*/

void smtp_rset(smtpstate *smtp) {

    strcpy(smtp->sender, "");		/* clear sender */
    free_recips(&smtp->reciplist);	/* and recip list */
    smtp->recipcount = 0;
    smtp->mail = FALSE;			/* back to expecting MAIL command */
    
    t_fprintf(&smtp->conn, "%d State reset.\r\n", SMTP_OK);
    
}

/* smtp_vrfy --

    Verify an address & return the resolved form.  Mailing lists
    are not expanded, but forwarding addresses are.  Mark addresses that
    aren't being sent to (i.e., are forwarded).  It's a little tricky
    to distinguish between these and mailing list names (they both
    have the "nosend" bit set).  The key is that mailing lists always 
    have an addr but no name, while DND refs (including forwarded ones)
    will have a name.
*/

void smtp_vrfy(smtpstate *smtp) {

    recip	*newrecips;		/* resolved recipients */
    int		recipcount;		/* how many of them */
    recip	*r;
    char	recipname[MAX_ADDR_LEN+128]; /* resolved name/addr */
    
    newrecips = resolve(smtp->comline + 5, NULL, &recipcount);

    if (recipcount > ADDR_MAX_RECIPS) { /* recip max exceeded - newrecips is NULL */
    	t_fprintf(&smtp->conn, "%d Too many recipients; max is %d\r\n", SMTP_RECIPMAX, ADDR_MAX_RECIPS);
	return;
    }
    
    /* run along until we find something we should show */
    for (r = newrecips->next ;; r = r->next) {
    
	if (!(r->noshow && r->noerr)) {		/* hide list members but show forwarding */
		
	    if (r->stat == RECIP_OK) {
		if (!r->addr[0]) {	/* local recip w/ hostname suppressed */
		    addhost(r->name, recipname, m_hostname); /* generate internet form */
		    if (r->nosend)
		    	strcat(recipname, " (mail forwarded)");
		} else {
		    if (r->nosend && r->name)
			t_sprintf(recipname, "%s (mail forwarded)", r->name);
		    else
			t_sprintf(recipname, "%s <%s>", r->name, r->addr);
		}
	    } else {				/* bad - give just name */
	        strcpy(recipname, r->name);
	    }
	    t_fprintf(&smtp->conn, "%d%s", 
		    resptab[r->stat].stat, (r != newrecips ? "-" : " "));
	    if (r->stat == RECIP_OK)
	    	t_fprintf(&smtp->conn, "%s\r\n", recipname);
	    else
		t_fprintf(&smtp->conn, "%s: %s\r\n", resptab[r->stat].msg, recipname);	    
	}
	if (r == newrecips)
	    break;			/* end of circular list*/
    }
    
    free_recips(&newrecips);		/* discard the recip list */
}

/* smtp_xbtz --

    Accept a blitzformat mesage.
*/

void smtp_xbtz(smtpstate *smtp) {

    fileinfo		head;		/* message header */
    fileinfo		text;		/* and text */
    fileinfo		ctl;		/* and control file */
    summinfo		summ;		/* message summary */
    t_file		*f = NULL;
    t_file		*f1 = NULL;
    long		enclcount = 0;	/* number of enclosures */
    enclinfo		*ep = NULL;	/* enclosure list */
    long		messid;		/* local messageid */
    long		qid;		/* id in queue dir */
    messinfo		mi;		/* the monolithic message file */
    char		got[64];	/* chunk type */
    int			c;
    char		*comma;
    char		fname[128];	/* control file in spool dir */
    char		date[64];	/* RFC822 format date */
    char		line[SUMMBUCK_LEN];
    long		encllen;
    char		sender[MAX_ADDR_LEN];
    char		logbuf[MAX_ADDR_LEN+MAX_STR];
    	
    mi.finfo.fname[0] = 0;		/* no files yet */
    head.fname[0] = 0;
    text.fname[0] = 0;
    ctl.fname[0] = 0;

#define BADFMT(x,y,z) { t_fflush(&smtp->conn); t_fprintf(&smtp->conn, x, y, z);\
		    goto cleanup; }

    if (smtp->peer < 0) {
	t_sprintf(logbuf, "Blitz-format message from non-peer %s", smtp->remotehost);
	log_it(logbuf);
	BADFMT("%d Blitz-format accepted only from peer servers %s.\r\n", SMTP_FAIL, ""); 
    }
    /* note - generate temp filenames only as we create the files -- don't
    	      want finfoclose to try unlinking nonexistent files */

    /* create header file & write received: line (before incoming header) */
    temp_finfo(&head);
    if ((f = t_fopen(head.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("smtp_xbtz: cannot create ", head.fname);
	BADFMT("%d File error creating header file %s.\r\n", SMTP_FAIL, head.fname);
    }


    messid = next_messid();		/* generate messid */
    
    get_date(date);
    pthread_mutex_lock(&inet_ntoa_lock);
    t_fprintf(f, "Received: by %s (%s) via Blitz-SMTP from %s [%s]\r  id <%ld> %s\r",
    		m_fullservname, m_hostname, smtp->remotehost,
		inet_ntoa(smtp->remoteaddr.sin_addr), messid, date);
    pthread_mutex_unlock(&inet_ntoa_lock);

    t_fprintf(&smtp->conn, "%d Begin BlitzFormat input.\r\n", SMTP_BLITZON);
    t_fflush(&smtp->conn);		/* send the prompt */
    
    t_fseek(&smtp->conn, 0, SEEK_CUR);	/* now get set to read it */

    t_gets(got, 4+1, &smtp->conn);
    if (strcmp(got, "BLTZ") != 0) 	/* first 4 bytes identify xmit format */
	BADFMT("%d Expected \"BLTZ\", not \"%s\".\r\n", SMTP_FAIL, got);
	
    /* receive the 3 required pieces:  control file */
    temp_finfo(&ctl);
    if (recv_block(&smtp->conn, &ctl, "CTRL", got, NULL) <= 0)
	BADFMT("%d Expected \"CTRL\", not %s.\r\n", SMTP_FAIL, got);
	
    /* header: append to the file we created above */
    if (recv_block(&smtp->conn, &head, "HEAD", got, f) <= 0) { 
	f = NULL;			/* file already closed... */
	BADFMT("%d Expected \"HEAD\", not %s.\r\n", SMTP_FAIL, got);
    }
    f = NULL;				/* file already closed... */
    
    /* text: create temp file*/
    temp_finfo(&text);
    if (recv_block(&smtp->conn, &text, "TEXT", got, NULL) <= 0)
	BADFMT("%d Expected \"TEXT\", not %s.\r\n", SMTP_FAIL, got);

    while ((encllen = recv_encl(&smtp->conn, &ep)) > 0)
    	++enclcount;			/* get any enclosures */
	
    if (encllen < 0)			/* connection lost while reading encl */
	BADFMT("%d Bad enclosure format.\r\n", SMTP_FAIL, "");
	
    /* message has been transferred, but don't ack until we've queued it */
        
    /* copy control file w/ correct messid */
    if ((f = t_fopen(ctl.fname, O_RDONLY, FILE_ACC)) == NULL) {
	t_perror1("smtp_xbtz: open ", ctl.fname);
	BADFMT("%d File error opening %s.\r\n", SMTP_FAIL, ctl.fname);
    }

    qid = next_qid(); 			/* assign qid */
    
    queue_fname(fname, m_thisserv, qid);
    strcat(fname, "C");			/* generate control file pathname */
    if ((f1 = t_fopen(fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("smtp_xbtz: cannot create ", fname);
	BADFMT("%d File error creating control file %s.\r\n", SMTP_FAIL, fname);
    }
    t_gets(sender, sizeof(sender), f); 	/* sender's addr */
    t_fprintf(f1, "%s\r\n", sender);	/* copy it */
    t_gets(line, sizeof(line), f);	/* summary info */
    t_fprintf(f1, "%ld", messid);	/* make summary use our id, not sender's */
    if ((comma = index(line, ',')) == NULL)
	BADFMT("%d Incomplete summary %s.\r\n", SMTP_FAIL, "(no comma)");    
    t_fprintf(f1, "%s\r\n", comma); /* rest of summary is unchanged */
    while((c = t_getc(f)) != EOF)	/* copy rest of control file */
	t_putc(f1, c);

    t_fclose(f);
    t_fclose(f1);			/* control file all set */
    f = f1 = NULL;
    
    /* parse summary info (unpacked form) */
    if (!summ_parse(line, &summ, "incoming message", FALSE)) {
	BADFMT("%d Invalid summary!\r\n", SMTP_FAIL, fname);
    }
    
    /* combine all pieces of message into single file */
    if (!mess_setup(messid, &head, &text, ep, &mi, m_spool_filesys, summ.type)) {
	t_perror("smtp_xbtz: mess_setup failed ");
	BADFMT("%d File error creating queue file.\r\n", SMTP_FAIL, fname);
    }

    /* now link message into local input queue dir */
    queue_fname(fname, m_thisserv, qid);
    if (link(mi.finfo.fname, fname) != 0) {
	t_perror1("smtp_xbtz: cannot link ", fname);
	BADFMT("%d File error linking queue file %s.\r\n", SMTP_FAIL, fname);
    }

    /* ok, we now accept responsibility for the message */
    t_fprintf(&smtp->conn, "%d Message queued.\r\n", SMTP_OK);

    t_sprintf(logbuf, "Incoming Blitz %ld from %s; %ld bytes, %ld encls, qid %ld.",
    			 messid, sender, mi.finfo.len, enclcount, qid);
    log_it(logbuf);
    ++m_recv_blitz;			/* statistics: count incoming blitzmessages */
    	
    wake_queuethread(m_thisserv, qid); 	/* nudge task that serves queue */
    
cleanup:				/* here to clean up on errors */

    if (mi.finfo.fname[0] > 0)		/* message file created? */
	mess_done(&mi);			/* clean it up */
    if (f)				/* close any open ctl files */
	t_fclose(f);
    if (f1)
	t_fclose(f1);
    finfoclose(&head);			/* clean up temp files */
    finfoclose(&text);
    finfoclose(&ctl);
    clean_encl_list(&ep);		/* discard enclosure temps */
    
#undef BADFMT
    
}

/* smtp_xfer --

    User transfer:  accept entire user mailbox from peer server.
    A number of subcommands will be given to send:  preferences,
    mailing lists, InBox messages, Trash messages, vacation text,
    vacation recip list.
    
    A filesystem is assigned as soon as we begin processing the
    commmand, but the user's DND entry is not changed until all
    the information has been transferred -- that's the point at
    which the transfer takes effect and we become the server of
    record for this user. (for a intra-domain transfer).
    
    We also support inter-domain mailbox transfers (from non-peer
    blitz servers). In that case what's happening isn't a mailbox
    relocation so much as copying the contents of the mailbox in
    the old domain into an existing account in this domain.
    
    ****validation****
*/

void smtp_xfer(smtpstate *smtp) {

    char	*comp;			/* current pos in command line */
    long	uid;			/* uid of user to receive */
    mbox	*mb;			/* box for that user */
    char	messdir[FILENAME_MAX];	/* box's message directory */
    static char *farray[] = {"UID", "BLITZSERV", "BLITZINFO", NULL};
    struct dndresult	*dndres = NULL;	/* results of dndlookup */
    int		dndstat;
    char	*p;
    int		fs;			/* filesystem box belongs on */
    int		hostnum;		/* server index */

    if (smtp->peer < 0 && !smtp->xferok) { /* sender must be peer/trusted server */
	t_fprintf(&smtp->conn, "%d I don't know who you are.\r\n", SMTP_BADARG);
	return;
    }    		
    comp = smtp->comline + 4;		/* skip "XFER" */

    /* first, look up name they're sending in our DND */
    dndstat = t_dndlookup1(comp, farray, &dndres);
             
    if (dndstat != DND_OK) {		/* didn't match */
	if (dndres)
	    t_free(dndres);
	if (dndstat == DND_NOUSER)
	    t_fprintf(&smtp->conn, "%d No such user.\r\n", SMTP_FAIL);
	else if (dndstat == DND_AMBIG || dndstat == DND_VAGUE)
	    t_fprintf(&smtp->conn, "%d Ambiguous user name.\r\n", SMTP_FAIL);
	else
	    t_fprintf(&smtp->conn, "%d Error talking to DND: %d\r\n", SMTP_FAIL, dndstat);
	return;
    }

    strtonum(t_dndvalue(dndres, "UID", farray), &uid);
    
    /* intra- and inter- domain transfers are different, for an inter-domain
       (remote) transfer, we must already be the server of record in the
       local domain for this user. */

    fs = fs_match(t_dndvalue(dndres, "BLITZINFO", farray));
    hostnum = blitzserv_match(t_dndvalue(dndres, "BLITZSERV", farray));
    t_free(dndres);
       
    if (smtp->peer >= 0) {			/* local transfer */
    	if (hostnum == m_thisserv) {	/* box must NOT already be here */
	    t_fprintf(&smtp->conn, "%d That user is already on this server.\r\n", SMTP_FAIL);	
	    return;
	}
	fs = -1;			/* assign fs at random */
	mb = mbox_find(uid, fs, TRUE);	/* set up box, but do NOT change DND yet */
    } else {
    	if (hostnum != m_thisserv) {	/* remote transfer, we SHOULD have box */
	    t_fprintf(&smtp->conn, "%d That user is not on this server.\r\n", SMTP_FAIL);	
	    return;	
	}
	mb = force_disconnect(uid, fs, NULL); /* they might already be signed on! */
	sem_release(&mb->mbsem);	/* but don't keep box locked */
    }
    
    /* may need to create box dir, if user that used to be here is being
      transferred back */
    if (mkdir(mb->boxname, DIR_ACC) < 0 && pthread_errno() != EEXIST) {
	t_perror1("smtp_xfer: cannot create ", mb->boxname); 
	t_fprintf(&smtp->conn, "%d Error creating mailbox.\r\n", SMTP_FAIL);
	goto cleanup;
    }
    /* for convenience, create mess directory now */
    t_sprintf(messdir, "%s%s", mb->boxname, MESS_DIR);
    p = rindex(messdir,'/');
    *p = 0;				/* chop to directory name */	
    if (mkdir(messdir, DIR_ACC) < 0 && pthread_errno() != EEXIST) {
	t_perror1("smtp_xfer: cannot create ", messdir); 
	t_fprintf(&smtp->conn, "%d Error creating message directory.\r\n", SMTP_FAIL);
	goto cleanup;
    }

    t_fprintf(&smtp->conn, "%d Begin mailbox transfer.\r\n", SMTP_BLITZON);
    t_fflush(&smtp->conn);		/* send the prompt */

    for (;;) {				/* loop processing subcommands */
    
	if (server_shutdown) {		/* exit now if server shutting down */
	    t_fprintf(&smtp->conn, "%d %s shutting down now.\r\n", SMTP_SHUTDOWN, 
			    m_fullservname);
	    break;
	}
	
	t_fflush(&smtp->conn);		/* write last response */
	
	t_fseek(&smtp->conn, 0, SEEK_CUR); /* set up to read next command */
	
	if (t_gets(smtp->comline, sizeof(smtp->comline), &smtp->conn) == NULL)
	    break;			/* connection lost */
		
	if (strncasecmp(smtp->comline, "DONE", 4) == 0) {
	    xfer_done(smtp, mb);	/* commit changes */
	    break;
	}
	else if (strncasecmp(smtp->comline, "FOLD", 4) == 0)
	    recv_fold(smtp, mb);
	else if (strncasecmp(smtp->comline, "LIST", 4) == 0)
	    recv_list(smtp, mb);
	else if (strncasecmp(smtp->comline, "MESS", 4) == 0)
	    recv_mess(smtp, mb);
	else if (strncasecmp(smtp->comline, "PREF", 4) == 0)
	    recv_pref(smtp, mb);
	else if (strncasecmp(smtp->comline, "VACA", 4) == 0)
	    recv_vaca(smtp, mb);
	else {
	    t_fflush(&smtp->conn);		/* get set to write */
	    t_fprintf(&smtp->conn, "%d Syntax error; unknown command.\r\n", SMTP_BADCMD);
	}
    }

cleanup:    
    mbox_done(&mb);			/* release our attachment */

}

/* recv_block -- 

    Receive a piece of message (header, text, single enclosure).

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
    
    The caller may pass an open file (which will be appended to
    and closed) or NULL (in which case a temp file is created).
    Either way, the caller is responsible for cleaning up the file.  
    
    Returns 0 if message ends here, -1 if error.
*/

long recv_block(t_file *conn, fileinfo *finfo, char *want, char *got, t_file *f) {

    u_bit32	len;
    int		i;
    int		c;
    
    strcpy(got, "end-of-message");	/* pessimistic */

    if (!f) { 				/* unless file already open */ 
	if ((f = t_fopen(finfo->fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	    t_perror1("recv_block: open ", finfo->fname);
	    return -1;			/* can't create temp file */
	}
    }
    
    /* read length longword */
    if (t_fread(conn, (char *) &len, sizeof(len)) != 4)
    	goto cleanup;
    len = ntohl(len);
    
    if (len == 0) {			/* 0-length chunk is end-of-message */
	t_fclose(f);
	return 0;
    }
    
    if (t_fread(conn, got, 4) != 4)	/* read block type */
    	goto cleanup;
    got[4] = 0;
    
    if (strcmp(want, got) != 0)		/* not what was expected? */
	goto cleanup; 
    
    len -= BLOCK_HDLEN;			/* subtract header length */
    
    for (i = 0; i < len; ++i) {		/* copy entire block */
	if ((c = t_getc(conn)) == EOF) /* watching out for lost connection */
	    goto cleanup;
	t_putc(f, c);			
    }
    
    while (i++ % 4) {			/* now skip the padding */
	if ((c = t_getc(conn)) == EOF)	
	    return -1;
    }  
	
    finfo->offset = 0;			/* frame block in temp file */
    finfo->len = t_fseek(f, 0, SEEK_END);	/* include any previous file contents */

    t_fclose(f);    
    return len + BLOCK_HDLEN;		/* distinguish empty block from eof */
    
cleanup: /* file trouble */
    t_fclose(f);
    return -1;		

}

/* recv_encl -- 

    Receive enclosure block; add it to enclosure list.  Returns 0
    when end-of-message reached, -1 on error.
*/

long recv_encl(t_file *conn, enclinfo **ep) {

    enclinfo	*new;			/* encl we construct */
    char	got[64];
    t_file	*f;
    enclhead	eh;			/* enclosure header in chunk */
    enclinfo	*p;	
    long	len;
    
    new = mallocf(sizeof(enclinfo));
    new->next = NULL;
    temp_finfo(&new->finfo);
    
    /* receive an enclosure; must be at least the header */
    len = recv_block(conn, &new->finfo, "ENCL", got, NULL);
    if (len < 0 || len < EHEAD_LEN) { 	/* error or end of message */
	clean_encl_list(&new);		/* never mind... */
	if (len == 0)
	    return 0;			/* end of message */
	else
	    return -1;			/* error */
    }
    
    /* Enclosure chunk consists of enclhead & enclosure itself */
    
    if ((f = t_fopen(new->finfo.fname, O_RDONLY, FILE_ACC)) == NULL) {
	t_perror1("recv_encl: open ", new->finfo.fname);
	clean_encl_list(&new);
	return -1;
    }
    
    t_fread(f, (char *) &eh, EHEAD_LEN); /* read standard encl header info */
    t_fclose(f);
    
    /* parse standard enclhead & fill in enclinfo */
    new->finfo.offset = EHEAD_LEN; 	/* encl itself follows header */
    new->finfo.len = ntohl(eh.encllen);	/* and is this long */
    
    /* make sure string lengths don't exceed bounds */
    if (ntohl(eh.typelen) > ENCLSTR_LEN) eh.typelen = htonl(ENCLSTR_LEN);
    if (ntohl(eh.namelen) > ENCLSTR_LEN) eh.namelen = htonl(ENCLSTR_LEN);
    
    strncpy(new->type, eh.type, ntohl(eh.typelen)); /* strings are counted... */
    new->type[ntohl(eh.typelen)] = 0;	/* ... and might not be terminated */
    strncpy(new->name, eh.name, ntohl(eh.namelen));
    new->name[ntohl(eh.namelen)] = 0;
   
    /* add new encl to end of list */
    if (*ep) {
	for (p = *ep; p->next; p = p->next)
	    ;
	p->next = new;
    } else
	*ep = new;			/* first one */
    
    new->next = NULL;			/* new one is at end */
    
    return len;				/* block length */
}

/* xfer_done --

    Entire mailbox has been received; commit to the change by making the
    user's DND record point to us.
*/

void xfer_done(smtpstate *smtp, mbox *mb) {

    char	line[MAX_STR];
    int		i;
    
    mb->gone = FALSE;			/* if they were gone; they're back now */
    
    sem_seize(&mb->mbsem);
    record_fs(mb);			/* change the DND entry */
    sem_release(&mb->mbsem);

    /* don't send new user greeting to this user (other server might
       not use PREF_INITIALMESS) */
    pref_set(mb, PREF_INITIALMESS, "\"1\"");
    /* but do send them a transfer welcome, if one is defined */
    initialmess(mb, NULL, f_xfermess);

    /* clear Expired prefs on default folders */
    /* (other folders cleared by recv_fold) */
    for (i = 0; i < DEFAULT_FOLDCOUNT; ++i) {
	t_sprintf(line, "%s%d", PREF_EXP, i);
	pref_rem(mb, line);
    }
    	
    t_sprintf(line, "Mailbox transfer for uid %ld completed ", mb->uid);
    log_it(line);
     
    t_fflush(&smtp->conn);		/* get set to write */ 
    t_fprintf(&smtp->conn, "%d Mailbox received.\r\n", SMTP_OK);
    
}
/* recv_fold --

    Receive folder definition:
	FOLD <number> <quoted-name> 

    Call touch_folder to mark the folder as changed (invalidate client's cache)
    since we're assigning new message id's. 
    
    In an inter-domain transfer, it's possible the folder already exists, or that
    there's a folder numbering conflict. The number they supply is thus just a
    suggestion; we locate/create a folder of the specified name and return its
    number.
	
*/

void recv_fold(smtpstate *smtp, mbox *mb) {

    long	foldnum;
    char	*comp;
    int		i;
    boolean_t	exists = FALSE;		/* appending to existing folder? */
    char	foldname[FOLD_NAMELEN];	/* unquoted folder name */
    char 	foldpref[32];		/* "Expired<n>" */
    
    comp = smtp->comline + 4;		/* skip "FOLD" */
    while (*comp == ' ')
	++comp;

    t_fflush(&smtp->conn);		/* get set to write */ 
	
    comp = strtonum(comp, &foldnum);	/* parse folder # */
    
    if (*comp++ != ' ' || !*comp) {
	t_fprintf(&smtp->conn, "%d Syntax: <foldnum> <foldname>\r\n", SMTP_BADARG);
	return;
    }
    if (foldnum >= FOLD_MAX) {
	t_fprintf(&smtp->conn, "%d Folder number too large.\r\n", SMTP_BADARG);
	return;
    }

    comp = unquote(foldname, comp);		/* get unquoted folder name */
    if (strlen(foldname) == 0) {
	t_fprintf(&smtp->conn, "%d Missing folder name.\r\n", SMTP_BADARG);
	return;
    }
    
    sem_seize(&mb->mbsem);			/* lock while we check & create */

    /* check see if that folder already exists */
    for (i = 0; i < mb->foldmax; ++i) {
	if (strcasecmp(foldname, mb->fold[i].name) == 0) {
	    break;
	}
    }
    if (i < mb->foldmax) {			/* existing folder? */
    	exists = TRUE;
    	foldnum = i;				/* just use it */
    } else {					/* no - create new folder */
    	if (foldnum_valid(mb, foldnum)) {	/* if suggested # is in use, choose another */
	    for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {
	    	if (mb->fold[foldnum].num < 0)	/* find first hole */
		    break;
	    }					/* (or increase foldmax) */
	}
	/* grow folder array if needed */
	if (foldnum >= mb->foldmax) {
	    mb->fold = reallocf(mb->fold, (foldnum + 1) * sizeof(folder)); 
	    for ( ; mb->foldmax < foldnum+1; ++mb->foldmax)
		mb->fold[mb->foldmax].num = -1;	/* any intervening #s are unused */  
	}
    }
    
    strcpy(mb->fold[foldnum].name, foldname);	/* fill in name & # */
    mb->fold[foldnum].num = foldnum;
    if (!exists)				/* if creating new folder */
	summ_read(mb, &mb->fold[foldnum]);	/* initialize empty summary list */
    touch_folder(mb, &mb->fold[foldnum]);	/* update session tag */
    
    t_sprintf(foldpref, "%s%ld", PREF_EXP, foldnum);
    pref_rem_int(mb, foldpref);		/* clear expired pref (messid's have changed) */

    sem_release(&mb->mbsem);	

    t_fprintf(&smtp->conn, "%d %ld\r\n", SMTP_OK, foldnum); /* tell them what number we used */

    return;
}
/* recv_list --

    Receive mailing list:
	LIST <name> 
	<list contents (crlf-terminated lines)> 
	'.'
	  
	
*/

void recv_list(smtpstate *smtp, mbox *mb) {

    char	*comp;			/* pointer into command */
    char	lname[MAX_STR];		/* list name */
    ml_data	*ml;			/* list struct */
    ml_data	*ml_head = NULL, *ml_tail = NULL;
    char	buf[MAX_STR];
    char	*bufp;
    int		l;
     
    comp = smtp->comline + 4;		/* skip "LIST" */

    if (*comp++ != ' ' || !*comp) { /* skip 1 space */
	t_fflush(&smtp->conn);		/* get set to write */ 
	t_fprintf(&smtp->conn, "%d Syntax: LIST <name>\r\n", SMTP_BADARG);
	return;
    }
    
    strtrimcpy(lname, comp);		/* remove leading/trailing spaces from name */
    
    for (;;) {				/* until end of list */
	if (t_gets(buf, sizeof(buf), &smtp->conn) == NULL)
	    break;			/* connection lost */
	
	if (strcmp(buf,".") == 0)	/* end of list? */
	    break;			
	if (*(bufp = buf) == '.')
	    ++bufp;			/* skip escaped dot */
	    
	l = strlen(bufp) + 1 + 1;	/* room for \n and \0 */
	ml = mallocf(l + sizeof(ml_data));	/* allocate space for contents */
	ml->maxlen = l;
	ml->len = l - 1;		/* note: don't write out \0 */
	ml->next = NULL;
	
	strcpy(ml->data, bufp);		/* copy list member */
	strcat(ml->data, "\n");		/* delimit */
	if (ml_head)			/* append to list */
	    ml_tail->next = ml;
	else
	    ml_head = ml;
	ml_tail = ml;  
    }
    
    ml_set(mb, lname, ml_head);		/* define the list in new box */
    
    ml_clean(&ml_head);			/* free list storage */
        
    t_fflush(&smtp->conn);		/* ready to write again */
    
    t_fprintf(&smtp->conn, "%d List received\r\n", SMTP_OK);

}

/* recv_mess --

    Receive message, deliver to appropriate folder.
    If a large number of users are to be transferred from the same
    server, much storage would be wasted if we made personal copies of
    messages shared by multiple users (in particular, the postmaster
    message etc.)  We attempt to keep track of what messages we've
    already been sent by making a hard link (using the name host.messid)
    from the messxfer directory on the destination filesystem.  If
    we already have a copy of the message that we're to deliver, there's
    no need to send another:  just add a link from the user's mailbox
    directory.  Note that we don't generally try to keep links to messages
    by id; mailbox transfer is a special case.  The messxfer directory is
    checked periodically as a system housekeeping chore; the extra link is
    cleaned up if there are no other links remaining, (by time??).
  
    The syntax of the MESS command is:
    
	>>> MESS <folder #> <summary info>
	<<< 250 Already have it
	-or-
	<<< 354 Send it
	>>> <BLTZ format message...>
	<<< 250 Ok
*/

void recv_mess(smtpstate *smtp, mbox *mb) {

    char	*comp;			/* pointer into command */
    long	foldnum;		/* folder to deliver to */
    summinfo	summ;			/* summary info */
    long	sender_messid;		/* message id on sending system */
    char	xfername[FILENAME_MAX];	/* message in messxfer directory */	
    char	messname[FILENAME_MAX];	/* in user directory */
    folder	*fold;			/* folder to deliver to */
    int		stat;			
    boolean_t	ok;
    
    comp = smtp->comline + 4;		/* skip "MESS" */
    while (*comp == ' ')
	++comp;

    t_fflush(&smtp->conn);		/* get set to write */ 
	
    comp = strtonum(comp, &foldnum);	/* parse folder # */
    if (*comp++ != ' ' || !*comp) {
	t_fprintf(&smtp->conn, "%d Syntax: <fold> <summinfo>\r\n", SMTP_BADARG);
	return;
    }
    if (!foldnum_valid(mb, foldnum)) {
	t_fprintf(&smtp->conn, "%d Invalid folder #\r\n", SMTP_BADARG);
	return;
    }
    fold = &mb->fold[foldnum];
	
    /* parse summary info (unpacked form) */
    if (!summ_parse(comp, &summ, "incoming message", FALSE)) {
	t_fprintf(&smtp->conn, "%d Invalid summary!\r\n", SMTP_FAIL);
    	return;
    }
    
    /* rememeber sender's message id, but assign our own */
    sender_messid = summ.messid;
    summ.messid = next_messid();
    
    /* construct name based on source server name & id */
    mess_xfername(xfername, mb->fs, sender_messid, smtp->remotehost);

    /* construct name message will have in user's box */
    mess_name(messname, mb, summ.messid);
    
    /* if we already have a copy of this message saved up, it can
       just be linked into the mess directory (created in smtp_xfer) */
    if ((stat = link(xfername, messname)) < 0) {
  	if (pthread_errno() == ENOENT) {	/* don't have message yet */
	    /* receive message into messxfer dir */	
	    if (!recv_xfermess(smtp, &summ, mb->fs, xfername))
		return;
	    stat = link(xfername, messname);	/* now, link it again */
	}  
	if (stat < 0) {	 			/* can't save message */
	    t_perror1("recv_mess: cannot link ", xfername);
	    t_fprintf(&smtp->conn, "%d Error saving message.\r\n", SMTP_FAIL);
	    return;
	}
    }
    
    /* message file created; now deliver summary to appropriate folder */
    sem_seize(&mb->mbsem); 		
    ok = fold_addsum(mb, fold, &summ);
    if (ok) {				/* update box length total */
	mb->boxlen += summ.totallen;
    }
    sem_release(&mb->mbsem);

    if (!ok) {
	t_errprint_ll("recv_mess: duplicate summary(!) uid %ld messid %ld", mb->uid, summ.messid);
	t_fprintf(&smtp->conn, "%d Error writing summary!\r\n", SMTP_FAIL);
    } else
	t_fprintf(&smtp->conn, "%d Message received.\r\n", SMTP_OK);
	
    return;
}

/* recv_pref --

    Receive preferences:
	<pref name 1> "<pref value 1>"
	...
	<pref name n> "<pref value n>"
	.                       	-- mark end w/ single dot
	
    NOTE: preferences must be the first thing transferred.
*/

void recv_pref(smtpstate *smtp, mbox *mb) {

    char	pref[2*(PREF_MAXLEN+1)];
    char	*p;

    t_fflush(&smtp->conn);		/* get set to write */ 
    t_fprintf(&smtp->conn, "%d Send prefs, end with '.' by itself.\r\n", SMTP_BLITZON);
    t_fflush(&smtp->conn);		/* send the prompt */
    
    t_fseek(&smtp->conn, 0, SEEK_CUR); 	/* set up to read */
    
    for (;;) {				/* read each key/value pair */
	if (t_gets(pref, sizeof(pref), &smtp->conn) == NULL)
	    break;			/* connection lost */
	    
	if (strcmp(pref, ".") == 0) {
	    t_fflush(&smtp->conn);	/* get set to write */ 
	    t_fprintf(&smtp->conn, "%d Prefs received.\r\n", SMTP_OK);
	    break;			
	}

	if ((p = index(pref,' ')) == NULL) {
	     t_fflush(&smtp->conn);		/* get set to write */ 
	     t_fprintf(&smtp->conn, "%d Syntax: <key> \"<value>\".\r\n", SMTP_BADARG);
	     break;
	} else {
	    *p++ = 0;	 		/* split into key & value */	    
	    pref_set(mb, pref, p);	/* set the pref */
	}
    }
    
    /* generate a new session-id, to flush client summary caches */
    sem_seize(&mb->mbsem);
    set_sessionid(mb);			/* bump sessionid */
    touch_folder(mb, &mb->fold[TRASH_NUM]);/* touch builtin folders... */
    touch_folder(mb, &mb->fold[AUDIT_NUM]);/* ...other folders are touched... */
    touch_folder(mb, &mb->fold[INBOX_NUM]);/* ...by recv_fold */

    sem_release(&mb->mbsem);

}
/* recv_vaca --

    Receive vacation message text (as a block of type VACA).
    
*/

void recv_vaca(smtpstate *smtp, mbox *mb) {

    fileinfo	finfo;			/* vacation text */
    char	got[64];		/* chunk type */
    char	fname[MESS_NAMELEN];
    long	l;

    /* receive data into temp file */
    strcpy(finfo.fname, mb->boxname);
    strcat(finfo.fname, VACATION_TEMP);
    finfo.temp = TRUE;

    /* receive header & text */
    l = recv_block(&smtp->conn, &finfo, "VACA", got, NULL);
    t_fflush(&smtp->conn);		/* get set to write status */
    
    if (l <= 0) {			/* transfer failed */
	t_fprintf(&smtp->conn, "%d Expected \"VACA\", not %s.\r\n", SMTP_FAIL, got);
    } else {				/* transfer worked, save results for real */
	strcpy(fname, mb->boxname);
	strcat(fname, VACATION_FNAME);	
	if (rename(finfo.fname, fname) < 0) {
	    t_perror1("recv_vaca: rename failed: ",fname);
	    t_fprintf(&smtp->conn, "%d Error saving vacation text.\r\n", SMTP_FAIL);
	} else {
	    pref_set_int(mb, PREF_VACATION, "\"1\""); /* indicate that file exists */
	    t_fprintf(&smtp->conn, "%d Vacation text received.\r\n", SMTP_OK);		
	    finfo.temp = FALSE;			/* ok, DON'T try to remove... */
	}
    }
    finfoclose(&finfo);
}

/* recv_xfermess --

    Receive message into temp file, save that file in the messxfer
    directory.  The message is sent in BLTZ format, but there is
    no control block (just header, text, and encls.)
    
    The message is assembled, and linked into the messxfer
    directory using the name set by the caller.  (This may fail
    if another thread received a copy of an identical message at
    the same time.  If so, no problem; there will be _a_ copy of
    the message with the right name...)
*/

boolean_t recv_xfermess(smtpstate *smtp, summinfo *summ, int fs, char *xfername) {

    fileinfo	head;			/* message header */
    fileinfo	text;			/* and text */
    long	enclcount = 0;		/* number of enclosures */
    enclinfo	*ep = NULL;		/* enclosure list */
    messinfo	mi;			/* the monolithic message file */
    char	got[64];		/* chunk type */
    long	encllen;
    boolean_t	ok = TRUE;
    
    mi.finfo.fname[0] = 0;		/* no files yet */
    head.fname[0] = 0;
    text.fname[0] = 0;

#define BADFMT(x,y,z) { ok = FALSE; t_fflush(&smtp->conn); t_fprintf(&smtp->conn, x, y, z);\
		    goto cleanup; }

    temp_finfo(&head);			/* generate temp filenames */
    temp_finfo(&text);  

    t_fprintf(&smtp->conn, "%d Begin BlitzFormat input.\r\n", SMTP_BLITZON);
    t_fflush(&smtp->conn);		/* send the prompt */
    
    t_fseek(&smtp->conn, 0, SEEK_CUR);	/* now get set to read it */	
    t_gets(got, 4+1, &smtp->conn);
    if (strcmp(got, "BLTZ") != 0) 	/* first 4 bytes identify xmit format */
	BADFMT("%d Expected \"BLTZ\", not \"%s\".\r\n", SMTP_FAIL, got);
		
    /* receive header & text */
    if (recv_block(&smtp->conn, &head, "HEAD", got, NULL) <= 0) 
	BADFMT("%d Expected \"HEAD\", not %s.\r\n", SMTP_FAIL, got);
    if (recv_block(&smtp->conn, &text, "TEXT", got, NULL) <= 0)
	BADFMT("%d Expected \"TEXT\", not %s.\r\n", SMTP_FAIL, got);

    while ((encllen = recv_encl(&smtp->conn, &ep)) > 0)
	++enclcount;			/* get any enclosures */
	
    if (encllen < 0)			/* connection lost while reading encl */
	BADFMT("%d Bad enclosure format.\r\n", SMTP_FAIL, "");


    /* combine all pieces of message into single file (on same fs as box) */
    if (!mess_setup(summ->messid, &head, &text, ep, &mi, fs, summ->type)) {
 	t_perror1("recv_xfermess: cannot create ", xfername);
	BADFMT("%d File error creating mess file %s.\r\n", SMTP_FAIL, xfername);   
    }

    /* now link message into messxfer dir */
    if (link(mi.finfo.fname, xfername) != 0 && pthread_errno() != EEXIST) {
	t_perror1("recv_xfermess: cannot link ", xfername);
	BADFMT("%d File error linking mess file %s.\r\n", SMTP_FAIL, xfername);
    }
    
cleanup:				/* here to clean up on errors */

    mess_done(&mi);			/* remove message file */
    finfoclose(&head);			/* clean up temp files */
    finfoclose(&text);
    clean_encl_list(&ep);		/* discard enclosure temps */

    return ok;
}

/* read_smtp_filters --

   Incoming SMTP filter information is read upon demand from the
   f_smtpfilter file.

   Check the modification date of the file, if it differs from the
   last time we read it the filter table is discarded and re-read.

*/

void read_smtp_filters() {

    struct stat	statbuf;	/* results of stat() */
    static time_t	mtime;	/* modification time at least read */
    t_file      *f;
    char        buf[MAX_STR];
    char        arg[MAX_STR];
    char        *p;
    ipfilt	*filt = NULL;
    long	errcode = SMTP_REJECT;	/* error code to return */


    sem_check(&smtp_filt_sem);	/* table must be seized */

    if (!f_smtpfilter)		/* no filter file configured */
	return;

    if (stat(f_smtpfilter, &statbuf) < 0) {
	t_perror1("Cannot stat %s:", f_smtpfilter);
	return;
    }

    if (mtime == statbuf.st_mtime)	/* file changed since last read? */
	return;			/* no - done */

    /* make sure we can open file before erasing old table... */
    if ((f = t_fopen(f_smtpfilter, O_RDONLY, 0)) == NULL) {
        t_perror1("read_smtp_filters: cannot open ", f_smtpfilter);
        return;
    }
    mtime = statbuf.st_mtime;	/* remember current mod time */

    while (smtp_filt) {		/* free old table */
	filt = smtp_filt;
	smtp_filt = smtp_filt->next;
	t_free(filt);
    }
    smtp_filt_tail = NULL;	/* empty table */

    g_recent_login_limit = DFT_RECENT_LOGIN_LIMIT; /* reset definition of "recent" */

    while (t_gets(buf, sizeof(buf),f) != NULL) {
        if ((p = index(buf, ';')) != NULL)
            *p = 0;                     /* nail comments */

        if (*buf)
            for (p = buf + strlen(buf) - 1; isascii(*p) && isspace(*p); *p-- = 0)
                ;                       /* nail trailing spaces */

        for (p = buf; isascii(*p) && isspace(*p); ++p)
            ;                           /* skip leading spaces */

        if (!*p)
            continue;                   /* ignore blank lines */
 
        p = strwcpy(arg, p);		/* get filter type */
        while(isascii(*p) && isspace(*p)) /* and skip past it */
                ++p;

	if (strcasecmp(arg, "ADDR") == 0) {	/* filtering by addr/mask? */

            filt = mallocf(sizeof(ipfilt)); /* new filter */  
	    filt->kind = FILT_ADDR;	    /* of this flavor */
	    filt->name[0] = 0;              /* no name for this one */

            p = strwcpy(arg, p);        	/* address */

            filt->addr = ntohl(inet_addr(arg));

            while(isascii(*p) && isspace(*p)) /* trim arg */
                ++p;
            p = strwcpy(arg, p);        /* mask */
            filt->mask = ntohl(inet_addr(arg));

	} else if (strcasecmp(arg, "NAME") == 0) { /* filtering by domain name? */
	    filt = mallocf(sizeof(ipfilt)); /* new filter */  
	    filt->kind = FILT_NAME;	    /* the by-name flavor */
	    p = strwcpy(filt->name, p);	/* set domain name in filter struct */
	} else if (strcasecmp(arg, "RECENT_LOGIN") == 0) { /* match recent logins? */
	    filt = mallocf(sizeof(ipfilt)); /* new filter */
            filt->kind = FILT_LOGIN;         /* of this flavor */
	    filt->name[0] = 0;              /* no name for this one */ 
	} else if (strcasecmp(arg, "ERRCODE") == 0) {
	    p = strtonum(p, &errcode);	/* specifying SMTP error code to use */
	    continue;			/* not filter def; done */
	} else if (strcasecmp(arg, "RECENT_LOGIN_LIMIT") == 0) { 
	    p = strtonum(p, &g_recent_login_limit);
	    g_recent_login_limit *= 60;	/* minutes -> seconds */
	    continue;			/* not filter def; done */
	} else {
            t_errprint_s("Ignoring invalid filter type '%s'", arg);
            continue;
	}

        while(isascii(*p) && isspace(*p)) /* trim arg */
                ++p;

        if (strcasecmp(p, "ACCEPT") == 0) {
            filt->action = FILT_ACCEPT;
        } else if (strcasecmp(p, "REJECT") == 0) {
            filt->action = FILT_REJECT;
        } else if (strcasecmp(p, "NORELAY") == 0) {
            filt->action = FILT_NORELAY;
        } else {
            t_errprint_s("Ignoring invalid filter action '%s'", p);
            t_free(filt);		/* discard the failed filter */
	    filt = NULL;
        }
	
	if (filt) {			/* valid filter - add to end of table */
	    filt->errcode = errcode;	/* proper error code for this filter */
	    filt->next = NULL;
	    if (smtp_filt_tail)
		smtp_filt_tail->next = filt;
	    else
		smtp_filt = filt;
	    smtp_filt_tail = filt;
	}
   }

   t_fclose(f);
}
