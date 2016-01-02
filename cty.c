/*  BlitzMail Server -- control terminal interface

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/cty.c,v 3.6 98/10/21 16:00:13 davidg Exp Locker: davidg $
    
    Handle incoming control terminal connections.  
    
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/errno.h>
#include <sys/dir.h>
#include <arpa/telnet.h>
#include <ctype.h>
#include <netdb.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "client.h"
#include "mess.h"
#include "deliver.h"
#include "cty.h"
#include "smtp.h"
#include "queue.h"
#include "notify/not_types.h"

static any_t cty_serv(any_t cty_);
static void cty_cleanout(ctystate *cty);
static boolean_t invalid_box(ctystate *cty, long uid, int fs);
static boolean_t remove_box(ctystate *cty, long uid, int fs);
boolean_t cty_prompt(ctystate *cty, char *fmt, char *s);
static void cty_count(ctystate *cty);
static void cty_deport(ctystate *cty);
static void cty_forward(ctystate *cty);
static void cty_help(ctystate *cty);
static void cty_ledit(ctystate *cty);
static void cty_list(ctystate *cty);
static void cty_lrem(ctystate *cty);
static void cty_mstat(ctystate *cty);
static boolean_t cty_login(ctystate *cty);
static void cty_quit(ctystate *cty);
static void cty_refresh(ctystate *cty);
static void cty_set(ctystate *cty);
static void cty_uid(ctystate *cty);
static void cty_updatelists(ctystate *cty);
static void cty_user(ctystate *cty);
static void cty_xfer(ctystate *cty);
static void local_xfer(ctystate *cty, long uid, int oldfs, char *diskname);
static boolean_t remote_xfer(ctystate *cty, long uid, int fs, char *servname);
static boolean_t remote_deport(ctystate *cty, long uid, int fs, 
	char *destname, char *desthost, char *fwdaddr);
static void cty_showuser(ctystate *cty, char *name);
static boolean_t cty_whichfs(ctystate *cty, char *name, int *fs, long *uid);
static void cty_showforward(ctystate *cty, mbox *mb, char *name);
char askacc(t_file *f, char *prompt);
boolean_t asks(t_file *f, char *str, char *prompt, char *default_val);
long askuid(t_file *f, char *prompt, char *default_val);
static boolean_t xfer_lists(ctystate *cty, t_file *conn, mbox *mb);
static boolean_t xfer_prefs(ctystate *cty, t_file *conn, mbox *mb);
static boolean_t xfer_vacation(ctystate *cty, t_file *conn, mbox *mb);
static boolean_t xfer_fold(ctystate *cty, t_file *conn, mbox *mb, folder *fold);
static boolean_t xfer_mess(ctystate *cty, t_file *conn, mbox *mb, 
				int destfold, summinfo *summ);
static void linfo_display(t_file *f, long owner, long group, char lacc[4]);
int runcmd(char *name, char **argv);
boolean_t confirm(t_file *f, char *prompt);
static void fmt_acc(char acc, char *accstr);
static boolean_t inactive_box(long uid, int fs);

static char *farray[] =  {"NAME", "NICKNAME", "MAILADDR", "UID", 
			    "BLITZSERV", "BLITZINFO", "DUP", "DEPTCLASS", NULL};
static boolean_t deptclass_defined = TRUE;

/* cty_init --

    Check for presence of optional fields, to control what we display when showing user.
    
*/
void cty_init() {

    int		stat;			/* dnd status */
    
    do {
    	stat = t_dndfield_exists("DEPTCLASS", &deptclass_defined);
    } while (stat == DND_DOWN);
    
    if (stat != DND_OK) {
	t_errprint_l("Error looking for DEPTCLASS field: %ld", stat);
    }
    
    if (!deptclass_defined)	/* don't ask for fields not present */
    	farray[7] = NULL;
}

/* ctylisten --

    Set up a socket listening for cty connections.  When one arrives,
    accept it and spawn a thread to deal with it.
*/

any_t ctylisten (any_t zot) {

    int			fd;	/* socket we listen on */
    struct sockaddr_in	sin;	/* its addr */
    struct servent	*sp;	/* services entry */
    int			connfd; /* new connection */
    int			len = sizeof(sin); /* addr length */
    int			on = 1;	/* for setsockopt */
    ctystate		*cty = NULL; /* connection state */
    static t_file	listen_f; /* t_file for listening socket */
    pthread_t thread;		/* thread var */

    signal(SIGPIPE, SIG_IGN);		/* don't terminate if connections lost */

    cty_init();				/* set up globals */
    
    sem_seize(&herrno_sem);    
    if ((sp = getservbyname(CTYPORT, "tcp")) == NULL) {
	t_errprint_s("ctylisten: unknown service: %s", CTYPORT);
	exit(1);
    }    
    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = sp->s_port;	/* port we listen on */
    sp = NULL;			/* sppml */
    sem_release(&herrno_sem);    
    
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	t_perror("ctylisten: socket: ");
	exit(1);
    }
    
    /* set REUSEADDR so we won't get an EADDRINUSE if there are connections
       from our previous incarnation lingering in TIME_WAIT */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) < 0)
	t_perror("ctylisten: setsockopt (SO_REUSEADDR)");
	
    
    if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	t_perror("ctylisten: bind");
	exit(1);
    }

    listen(fd, 5);			/* accept connections (up to 5 in advance) */
    t_fdopen(&listen_f, fd);		/* set up (useless) t_file for the socket... */
    strcpy(listen_f.name, "ctylisten"); /* ...just to get it entered in t_fdmap */
        
    for (;;) {				/* now just keep accepting */
    
	if (!cty)			/* get state block */
	    cty = mallocf(sizeof(ctystate));
	    
	connfd = accept(fd, (struct sockaddr *) &cty->remoteaddr, &len);
	
	if (connfd < 0) {
	    t_perror("ctylisten: accept");
	    sleep(5);
	    continue;			/* try again */
	}
	
	/* enable periodic tickles */	
	if (setsockopt(connfd, SOL_SOCKET, SO_KEEPALIVE, (char *) &on, sizeof(on)) < 0)
	    t_perror("ctylisten: setsockopt (SO_KEEPALIVE)");

	/* note: no FIONBIO since we don't set conn.select (conn.telnet instead) */

	    
	t_fdopen(&cty->conn, connfd);	/* set up t_file for the conn */
	cty->conn.telnet = TRUE;	/* watch for telnet sequences on it */
	strcpy(cty->conn.name, "CTY (logging in)");
	
	if (pthread_create(&thread, generic_attr,
			(pthread_startroutine_t) cty_serv, (pthread_addr_t) cty) < 0) {
	    t_perror("ctylisten: pthread_create");
	    t_closefd(cty->conn.fd);		/* thread create failed; clean up */	
	    t_free(cty);			/* free cty record */
	    sleep(5);
	} else 
	    pthread_detach(&thread);
	
	cty = NULL;			/* it's their state now */
    }
}


/* cty_serv --

    Thread to handle a single cty connection.
    
*/

static any_t cty_serv(any_t cty_) {

    ctystate	*cty;			/* state variables */
    char	buf[MAX_STR];
    boolean_t	gotcmd = FALSE;
    
    cty = (ctystate *) cty_;

    signal(SIGPIPE, SIG_IGN);		/* don't terminate if connections lost */
    
    cty->done = FALSE;
    strcpy(cty->name, "");
    
    /* default settings for DEPORT behavior: */
    cty->deport_fwd = TRUE;		/* do set forwarding address */
    cty->deport_cleanout = TRUE;	/* and also discard mail */
    
    if (!cty_login(cty))		/* get user name & password */
	cty->done = TRUE;
    else {
	t_fprintf(&cty->conn, "BlitzMail server %s here.  Version %s.\r\n", 
			m_fullservname, server_vers);
	t_sprintf(buf, "Control connection from %s", cty->name);
	log_it(buf);
	/* set name in t_file for debugging */
	t_sprintf(cty->conn.name, "CTY: %s", cty->name);
    }
    while (!cty->done) {		/* loop processing commands */
    
	if (!cty->conn.tel.interrupt)
	    gotcmd = cty_prompt(cty, "Blitzctl %s> ", m_server[m_thisserv]);
	    
	if (cty->conn.tel.interrupt)	{	/* break ? */
	    t_fprintf(&cty->conn, "\r\nOuch!\r\n");
	    cty->conn.tel.interrupt = FALSE;
	    continue;			/* now re-prompt & read again */
	}	
	if (!gotcmd)			/* EOF --> connection gone */
	    break;			/* lost connection */
	
	
	if (strncasecmp(cty->comline, "BYE", 3) == 0)
	    cty_quit(cty);
	else if (strncasecmp(cty->comline, "CLEANOUT", 8) == 0)
	    cty_cleanout(cty);	    
	else if (strncasecmp(cty->comline, "COUNT", 5) == 0)
	    cty_count(cty);	    
	else if (strncasecmp(cty->comline, "DEPORT", 6) == 0)
	    cty_deport(cty);
	else if (strncasecmp(cty->comline, "DIE", 3) == 0)
	    abortsig();	    
	else if (strncasecmp(cty->comline, "HELP", 4) == 0)
	    cty_help(cty);	    
	else if (strncasecmp(cty->comline, "FORWARD", 7) == 0)
	    cty_forward(cty);	    
	else if (strcmp(cty->comline, "?") == 0)
	    cty_help(cty);	    
	else if (strncasecmp(cty->comline, "LEDIT", 5) == 0)
	    cty_ledit(cty);
	else if (strncasecmp(cty->comline, "LIST", 4) == 0)
	    cty_list(cty);
	else if (strncasecmp(cty->comline, "LREM", 4) == 0)
	    cty_lrem(cty);
        else if (strncasecmp(cty->comline, "MSTAT", 5) == 0)
            cty_mstat(cty);
	else if (strncasecmp(cty->comline, "QUIT", 4) == 0)
	    cty_quit(cty);
	else if (strncasecmp(cty->comline, "REFRESH", 7) == 0)
	    cty_refresh(cty);
	else if (strncasecmp(cty->comline, "SET", 3) == 0)
	    cty_set(cty);
	else if (strncasecmp(cty->comline, "UID", 3) == 0)
	    cty_uid(cty);
	else if (strncasecmp(cty->comline, "UPDATELISTS", 11) == 0)
	    cty_updatelists(cty);
	else if (strncasecmp(cty->comline, "USER", 4) == 0)
	    cty_user(cty);
	else if (strncasecmp(cty->comline, "STOP", 4) == 0)
	    server_shutdown = TRUE;
	else if (strncasecmp(cty->comline, "XFER", 4) == 0)
	    cty_xfer(cty);
	else if (strlen(cty->comline) > 0)
	    t_fprintf(&cty->conn, "Huh?\r\n");
    }
    
    t_fflush(&cty->conn);		/* flush any last output */
    
    t_closefd(cty->conn.fd);		/* close (don't free; no separate t_file here) */	
        
    t_free(cty);			/* free cty record */
    return 0;				/* and end the thread */
}
/* cty_prompt --

    Prompt for a line of input and read it.  Set up to write again.
*/

boolean_t cty_prompt(ctystate *cty, char *fmt, char *s) {
        
    t_fprintf(&cty->conn, fmt, s);	
    t_fflush(&cty->conn);		/* write last response + new prompt */
    
    t_fseek(&cty->conn, 0, SEEK_CUR); 	/* set up to read next command */
    
    if (t_gets(cty->comline, sizeof(cty->comline), &cty->conn) == NULL)
	return FALSE;			/* connection lost */
        
    t_fflush(&cty->conn);		/* get set to write */
    
    return TRUE;
}

/* cty_cleanout --
  
    Remove mailboxes for users that no longer belong here (account
    devalidated, or transferred to a different server).
    
    For each filesystem, read the box directory to locate all mailboxes.
    Consult the DND to see if the account is still valid, and if the box
    is on the right server and filesystem.  If not, remove it.
    
*/

static void cty_cleanout(ctystate *cty) {

    int			fs;			/* current filesystem */
    char		fname[MBOX_NAMELEN];	/* name of box dir on that fs */
    DIR			*dirf;			/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    long		uid;			/* one box */
    char		*end;			/* end of uid str */
    long		total = 0;		/* total number of boxes */
    long		removed = 0;		/* number removed */
    char		logbuf[MAX_STR];
    boolean_t		doit;			/* really throw stuff away? */

    pthread_mutex_lock(&global_lock);
    if (cleanout_going) {
	t_fprintf(&cty->conn, "Cleanout already in progress.\r\n"); 	
	pthread_mutex_unlock(&global_lock);
	return;
    }
    cleanout_going = TRUE;
    pthread_mutex_unlock(&global_lock);

    doit = confirm(&cty->conn, "Really discard boxes? [Y or N] ");
    if (!doit) {
	t_fprintf(&cty->conn, "\r\n ### Dry run; nothing will be discarded ###\r\n\r\n");
	t_fflush(&cty->conn);
    }
    
    for (fs = 0; fs < m_filesys_count; ++fs) { /* for each filesystem */

	/* open box directory */
    
	t_sprintf(fname, "%s%s", m_filesys[fs], BOX_DIR);
	pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
	dirf = opendir(fname);
	pthread_mutex_unlock(&dir_lock);

	if (dirf == NULL) {
	    t_fprintf(&cty->conn, "Cannot open %s\r\n", fname);
	    t_perror1("cty_cleanout: cannot open ", fname);
	    continue;
	} 

	while ((dirp = readdir(dirf)) != NULL) {	/* read entire directory */
		
	    /* skip  dot-files */
	    if (dirp->d_fileno != 0 && dirp->d_name[0] != '.') {
		end = strtonum(dirp->d_name, &uid);
		if (*end == 0) {	/* ignore non-numeric filenames */
		    ++total;
		    if (inactive_box(uid, fs)	/* if no recent login */
		    	&& invalid_box(cty, uid, fs)) { /* and not in DND */
			if (doit) {
			    if (remove_box(cty, uid, fs))
				++removed;
			}
		    }
		}
	    }
	}
	closedir(dirf);
    }
        	
    t_sprintf(logbuf, "%ld boxes total; %ld removed.", total, removed);
    log_it(logbuf);
    t_errprint(logbuf);
    t_fprintf(&cty->conn, "%s\r\n", logbuf);		

    pthread_mutex_lock(&global_lock);
    cleanout_going = FALSE;
    pthread_mutex_unlock(&global_lock);

}
/* inactive_box --
  
    Check PREF_LASTLOG. If user has signed on "recently", box is not to be
    removed yet.
    
*/

static boolean_t inactive_box(long uid, int fs) {

    char	pref[PREF_MAXLEN];	/* to check prefs */
    char	lastlog[PREF_MAXLEN];	/* date/time of last login */
    mbox	*mb;			/* user's mailbox */
    boolean_t	inactive = FALSE;	/* returned: box is inactive? */
    char	datestr[9];		/* mm/dd/yy */
    char	timestr[9];		/* hh:mm:ss */
    u_long	logindate;		/* last login (mactime) */

    if (cleanout_grace < 0)		/* no grace period configured */
	return TRUE;			/* so everything is "inactive" */
    
    mb = mbox_find(uid, fs, FALSE);	/* locate box */
    if (!pref_get(mb, PREF_LASTLOG, pref))		
	inactive = TRUE;		/* no preference; VERY old box */
    else {
	unquote(lastlog, pref);
	if (!parse_date(lastlog, datestr, timestr, &logindate))
	    inactive = TRUE;		/* wrong format; old box */
	else {
	    if (logindate + cleanout_grace*24*60*60 < add_days(0))
	    	inactive = TRUE;	/* no login with past N days */
	}
    }
    mbox_done(&mb);
	
    return inactive;
}
/* invalid_box --
  
    Check DND to see if box should be nixed.
    
*/

static boolean_t invalid_box(ctystate *cty, long uid, int fs) {

    struct dndresult	*dndres = NULL;	/* results of dndlookup */
    int			stat;		/* dnd status */
    static char *farray[] = {"NAME", "DUP", "BLITZSERV", "BLITZINFO", NULL};
    char		uidname[12];	/* #<uid> */
    char		*name = NULL;	/* DND values:  NAME */
    char		*dup;		/* 		DUP */
    char		*blitzserv;	/*		BLITZSERV */
    char		*blitzinfo;	/*		BLITZINFO */
    boolean_t		remove = FALSE;
    
    if (uid == pubml_uid)
	return FALSE;			/* don't mess with public lists! */
	
    t_sprintf(uidname,"#%ld", uid);	/* #<uid> string */
    
    stat = t_dndlookup1(uidname, farray, &dndres);
            
    if (stat == DND_OK) { 		/* resolved uniquely? */
	name = t_dndvalue(dndres, "NAME", farray); /* get name */
	dup = t_dndvalue(dndres, "DUP", farray);
	blitzserv = t_dndvalue(dndres, "BLITZSERV", farray);
	blitzinfo = t_dndvalue(dndres, "BLITZINFO", farray);
	/* account turned off (though not yet removed)? */
	if (strlen(dup) > 0) {
	    t_fprintf(&cty->conn, "Remove box %s (%s) -- %s\r\n", uidname, name, dup);
	    remove = TRUE;	
	} else if (blitzserv_match(blitzserv) != m_thisserv) {
	    t_fprintf(&cty->conn, "Remove box %s (%s) -- blitzserv = %s\r\n", 
	    		uidname, name, blitzserv);
	    remove = TRUE;	
	} else if (strlen(blitzinfo) == 0) {
	    t_fprintf(&cty->conn, "#### Box %s exists on %s, but BLITZINFO is empty! ###\r\n",
	    		uidname, m_filesys[fs]);
	    remove = FALSE;
	} else if (fs_match(blitzinfo) != fs) {
	    t_fprintf(&cty->conn, "Remove box %s (%s) -- blitzinfo = %s\r\n", 
	    		uidname, name, blitzinfo);
	    remove = TRUE;
	}
    } else if (stat == DND_NOUSER) {		/* user no longer exists */
	t_fprintf(&cty->conn, "Remove box %s -- invalid uid\r\n", uidname);
	remove = TRUE;	
    } else {
	t_fprintf(&cty->conn, "DND error checking box %s (stat=%d)\r\n", uidname, stat);
    }
    
    if (dndres)
	t_free(dndres);

    if (remove)				/* show removals as they happen */
	t_fflush(&cty->conn);
	
    return remove;
	    
}
/* remove_box --
  
    Remove entire mailbox directory (for a user that no longer belongs).
    
*/

static boolean_t remove_box(ctystate *cty, long uid, int fs) {
	
    mbox 	*mb;
    char	boxname[MBOX_NAMELEN];
    boolean_t	ok;
    
    /* push off any user (account could be devalidated & cleaned out while
       user still connected) */
    mb = force_disconnect(uid, fs, NULL);
    mb->gone = TRUE;			/* make sure box is marked as history */
    strcpy(boxname, mb->boxname);	/* remember its name */
    strcpy(mb->boxname, "");		/* zero it - must recreate if user comes back */
    mb->checked = FALSE;  

    ok = do_rm(boxname);		/* remove box (still locked, to prevent race) */

    sem_release(&mb->mbsem);		/* unlock and release box */
    mbox_done(&mb);
    
    if (!ok) {
	t_fprintf(&cty->conn, "#### Error removing %s: %s.\r\n", boxname, 
		strerror(pthread_errno()));
	return FALSE;
    }
        
    return TRUE;
}

/* cty_count --

    Print various counters.
*/

static void cty_count(ctystate *cty) {

    t_fprintf(&cty->conn, "Up since %s on %s\r\n", up_time, up_date);
    t_fprintf(&cty->conn, "%ld users now (peak = %ld)\r\n", u_num, u_hwm);
    t_fprintf(&cty->conn, "%ld messages sent, including:\r\n", m_sent);
    t_fprintf(&cty->conn, "    %ld outgoing blitz; %ld outgoing SMTP\r\n",
    				m_sent_blitzsmtp, m_sent_internet);
    t_fprintf(&cty->conn, "    %ld receipts; %ld vacations; %ld bounces\r\n",
    				 m_sent_receipt, m_sent_vacation, m_sent_bounce);
    t_fprintf(&cty->conn, "%ld incoming blitz; %ld incoming SMTP\r\n",
    			       m_recv_blitz, m_recv_smtp);
    t_fprintf(&cty->conn, "%ld local recipients\r\n", m_delivered);

}
/* cty_forward --

    Enter new forwarding address (or remove an old one).  
*/

static void cty_forward(ctystate *cty) {	

    long	uid;			/* user to change */
    char	name[MAX_STR];		/* user in #<uid> form */
    int		fs;
    mbox	*mb;
    char	*p;
    char	fwdaddr[MAX_STR];
  
 
    p = cty->comline + strlen("FORWARD");
    if (*p++ != ' ' || !*p) {
	 t_fprintf(&cty->conn, "Error: no uid given.\r\n");
	 return;
    }
    if (*p == '#')			/* allow optional # */
	++p;

    p = strtonum(p, &uid);		/* user to change */
    if (uid <= 0)  {			/* nonnumeric, or plain bad */
	t_fprintf(&cty->conn, "Invalid uid.\r\n");
	return;
    }
    
    /* must check DND to see where they are now */
    t_sprintf(name, "#%ld", uid);
    if (!cty_whichfs(cty, name, &fs, NULL))
	return;
	    
    t_fprintf(&cty->conn, "New forwarding addr (RETURN for none): ");
    t_fflush(&cty->conn);
    t_fseek(&cty->conn, 0, SEEK_CUR); 	/* set up to read */
    if (t_gets(fwdaddr, sizeof(fwdaddr), &cty->conn) == NULL)
	return;				/* connection lost */
    t_fflush(&cty->conn);
    
    mb = mbox_find(uid, fs, FALSE); 	/* locate the box */
    if (strlen(fwdaddr) > 0)		
	pref_set(mb, PREF_FWD, fwdaddr); /* set new addr */
    else
	pref_rem(mb, PREF_FWD);		/* remove forwarding address */
	
    mbox_done(&mb);			/* and release our attachment */
    
    t_fprintf(&cty->conn, "Ok.\r\n");

}

/* cty_help --

    Print command info.
*/

static void cty_help(ctystate *cty) {

    t_fprintf(&cty->conn, "------ Looking Around -----\r\n");
    t_fprintf(&cty->conn, "BYE          -- End control session, server keeps running.\r\n");
    t_fprintf(&cty->conn, "COUNT        -- Show current statistics.\r\n");
    t_fprintf(&cty->conn, "HELP         -- This is it.\r\n");
    t_fprintf(&cty->conn, "QUIT         -- Same as BYE.\r\n");
    t_fprintf(&cty->conn, "UID <uid>    -- Show DND & mailbox info by UID.\r\n");
    t_fprintf(&cty->conn, "USER <name>  -- Lookup name; show DND & mailbox info.\r\n");
    t_fprintf(&cty->conn, "\r\n");
    t_fprintf(&cty->conn, "------ Public Mailing Lists -----\r\n");
    t_fprintf(&cty->conn, "LEDIT <name>  -- Create list or edit ownership & accesses.\r\n");
    t_fprintf(&cty->conn, "LIST <name>   -- Show list ownership & accesses.\r\n");
    t_fprintf(&cty->conn, "LREM <name>   -- Remove public list.\r\n");
    t_fprintf(&cty->conn, "\r\n");    
    t_fprintf(&cty->conn, "------ Server Administration -----\r\n");
    t_fprintf(&cty->conn, "DEPORT <user>,<blitzserv>,<domain>	-- Copy user's mail to an outside site.\r\n");   
    t_fprintf(&cty->conn, "DIE          -- Kill server, causing a core dump.\r\n");
    t_fprintf(&cty->conn, "STOP         -- Shut down server cleanly.\r\n");
    t_fprintf(&cty->conn, "CLEANOUT     -- Remove boxes belonging to devalidated accounts.\r\n");
    t_fprintf(&cty->conn, "FORWARD <uid>  -- Enter new forwarding addr.\r\n");    
    t_fprintf(&cty->conn, "REFRESH <uid> -- Refresh mailbox info (after reloading messages).\r\n");
    t_fprintf(&cty->conn, "UPDATELISTS  -- Send fresh copy of all public lists to other servers.\r\n");
    t_fprintf(&cty->conn, "XFER <uid> </disk>|<serv> -- Move user to new disk or server.\r\n");    

}
/* cty_ledit --

    Create/edit public mailing list info.
*/

static void cty_ledit(ctystate *cty) {

    char	*p;
    char	listname[MAX_STR];	/* name of list to create/change */
    char	ucname[MAX_STR];
    mbox	*pubml_mb;		/* mailbox with public lists */
    u_long	modtime;		/* modification time */
    long	owner;			/* list owner */
    long	group;			/* and group */
    char	lacc[4];		/* list accesses */
    boolean_t	removed;		/* list removed? */
    boolean_t	exists;			/* list already exists? */
    ml_data	*ml;			/* list contents */
    
    p = cty->comline + strlen("LEDIT");
    if (*p++ != ' ' || !*p) {
	 t_fprintf(&cty->conn, "Error: no list name given.\r\n");
	 return;
    }
    while (*p == ' ')			/* strip leading spaces */
	++p;
	
    strcpy(listname, p);		/* get the name */
    ucase(ucname, listname);		/* normalize name to uppercase */
    
    /* check for illegal characters */
    for (p = ucname; *p; ++p) {
	if (!isascii(*p) || (!isalnum(*p) && *p != '-' && *p != '.' && *p != '_')) {
		t_fprintf(&cty->conn, "Illegal character in list name: %c\r\n", *p);
		return;
	}
    }
    
    /* check for reserved names */
    if ((strncmp(ucname, "OWNER-", strlen("OWNER-")) == 0)
     || (strlen(ucname) > strlen("-REQUEST") &&
    		strcmp(ucname + strlen(ucname) - strlen("-REQUEST"), "-REQUEST") == 0)) {
	t_fprintf(&cty->conn, "List names like owner-foo and foo-request are reserved.\r\n", *p);
	return;
    }
    
    pubml_mb = mbox_find(pubml_uid, pubml_fs, FALSE);	/* get box that has public list info */
    
    sem_seize(&pubml_mb->mbsem);		/* lock for pubml_getctl */
    exists = pubml_getctl(pubml_mb, ucname, &modtime, &owner, &group, lacc, &removed);
    sem_release(&pubml_mb->mbsem);
    
    if (removed)				/* if just placeholder for deleted list remains */
	exists = FALSE;				/* must recreate */
	
    if (exists)
	t_fprintf(&cty->conn, "Editing permissions for list '%s'\r\n", listname);
    else
	t_fprintf(&cty->conn, "Creating new list '%s'\r\n", listname);
    
    /* prompt, checking for lost connection at each step */
    if ((owner = askuid(&cty->conn, "Owner", "Blitzmail")) == -1) /* get owner name */
    	goto done;
    if ((group = askuid(&cty->conn, "Group", "Blitzmail")) == -1) /* and group */
	goto done;
    lacc[0] = askacc(&cty->conn, "Owner accesses");	/* and each access group */
    lacc[1] = askacc(&cty->conn, "Group accesses");		
    lacc[2] = askacc(&cty->conn, "World accesses");		
    lacc[3] = 0;
    if (lacc[0] == 0 || lacc[1] == 0 || lacc[2] == 0)
	goto done;					/* lost connection */
	
    t_fprintf(&cty->conn, "\r\nPermissions for list '%s' will be:\r\n", listname);
    linfo_display(&cty->conn, owner, group, lacc);
    
    if (confirm(&cty->conn, "Is this correct? [Y or N] ")) {
	removed = FALSE;			/* list will exist now */
	modtime = mactime();			/* modified as of right now */
	sem_seize(&pubml_mb->mbsem);
	pubml_putctl(pubml_mb, listname, modtime, owner, group, lacc, removed);
	sem_release(&pubml_mb->mbsem);
	if (!exists) {				/* if new list initialize w/ owner as member */
	    ml = mallocf(MAX_STR + sizeof(ml_data));
	    ml->next = NULL;
	    ml->maxlen = MAX_STR;
	    (void) uid_to_name(owner, ml->data);		/* owner is only member */
	    ml->len = strlen(ml->data);
	    ml_set(pubml_mb, listname, ml);	/* save list data */
	    ml_clean(&ml);			/* and clean up */
	}
	/* now, get & set list to cause update to be sent */
	if (!pubml_get(listname, &ml))
	    t_fprintf(&cty->conn, "That's odd; can't find the list now...\r\n");
	else {
	    pubml_set(listname, ml);		/* alter contents, sending an update */
	    ml_clean(&ml);
	    if (exists)
		t_fprintf(&cty->conn, "List updated.\r\n");
	    else
		t_fprintf(&cty->conn, "List created.\r\n");
	}
    } else {
	t_fprintf(&cty->conn, "Command cancelled.\r\n");
    }
    
    done:
    mbox_done(&pubml_mb);
    
}
       
/* cty_list --

    Print public mailing list info.
*/

static void cty_list(ctystate *cty) {


    mbox	*pubml_mb;		/* mailbox with public lists */
    u_long	modtime;		/* modification time */
    long	owner;			/* list owner */
    long	group;			/* and group */
    char	lacc[4];		/* list accesses */
    boolean_t	removed;		/* list removed? */
    char	*p;
    
    p = cty->comline + strlen("LIST");
    if (*p++ != ' ' || !*p) {
	 t_fprintf(&cty->conn, "Error: no list name given.\r\n");
	 return;
    }
    while (*p == ' ')			/* strip leading spaces */
	++p;
    
    pubml_mb = mbox_find(pubml_uid, pubml_fs, FALSE);	/* get box that has public list info */
    
    sem_seize(&pubml_mb->mbsem);		/* lock for pubml_getctl */
    
    if (pubml_getctl(pubml_mb, p, &modtime, &owner, &group, lacc, &removed)) {
	if (removed) 
	    t_fprintf(&cty->conn, "That list no longer exists.\r\n");
	else {
	    linfo_display(&cty->conn, owner, group, lacc);
	}
    } else {
	t_fprintf(&cty->conn, "No such public list.\r\n");
    }
    sem_release(&pubml_mb->mbsem);
    mbox_done(&pubml_mb);
}

/* cty_lrem --

    Remove public mailing list.
*/

static void cty_lrem(ctystate *cty) {

    char	*p;
    
    p = cty->comline + strlen("LREM");
    if (*p++ != ' ' || !*p) {
	 t_fprintf(&cty->conn, "Error: no list name given.\r\n");
	 return;
    }
    while (*p == ' ')			/* strip leading spaces */
	++p;
        
    if (pubml_rem(p)) 
	 t_fprintf(&cty->conn, "List removed.\r\n");
    else
	 t_fprintf(&cty->conn, "No such list.\r\n");
    
}

/* cty_login --

    Prompt for username and password.  To keep the password from echoing,
    we need to open the can o' Telnet worms.  We do the sleaziest job possible,
    using just enough Telnet to send WILL and WONT echo commands.
*/

static boolean_t cty_login(ctystate *cty) {

    char 	randnum[25];		/* random num for encrypt (ignore) */	 
    int		dndstat;   		/* status from dnd */
    t_file	*dnd = NULL;		/* dnd connection */
    dndresult	*dndres = NULL;		/* dnd results */
    char 	*p;
    /* dnd fields needed for validation */
    static char *val_farray[] = { "NAME", "UID", "PERM", "DUP", NULL };

    if (!cty_prompt(cty, "Name: ", ""))	/* read name */
	return FALSE;
    strcpy(cty->name, cty->comline);

    t_fprintf(&cty->conn, "%c%c%c", IAC, WILL, TELOPT_ECHO);
    if (!cty_prompt(cty, "Password: ", ""))
	return FALSE;			/* connection lost */
    t_fprintf(&cty->conn, "%c%c%c", IAC, WONT, TELOPT_ECHO);
    /* Note: WILL ECHO has side effect of causing TCP/KSP gateways
       to put Darterminal into character mode; send WONT SGA to undo */
    t_fprintf(&cty->conn, "%c%c%c", IAC, WONT, TELOPT_SGA);
    t_fprintf(&cty->conn, "\r\n"); /* pw left us in mid-line */

    
    /* check name, get random number */
    dndstat = t_dndval1(&dnd, cty->name, val_farray, randnum);
    
    if (dndstat == DND_CONTINUE) {	/* so far so good? */
	dndstat = t_dndval2(dnd, cty->comline, val_farray, &dndres);
	bzero(cty->comline, sizeof(cty->comline));/* wipe pw */
	if (dndstat == DND_OK) {	/* valid name & pw */
	    p = t_dndvalue(dndres, "DUP", val_farray);	
	    if (*p) 			/* but is this user devalidated? */
		dndstat = DND_NOUSER;
	    else {
		p = t_dndvalue(dndres, "PERM", val_farray); /* locate permissions */
		if (!strcasematch(p, "BLITZPRIV"))
		    dndstat = DND_NOUSER;	/* not allowed */
	    }
	}
    }
    
    if (dndstat == DND_AMBIG || dndstat == DND_PERM 
        || dndstat == DND_VAGUE || dndstat == DND_NOUSER) 
	t_fprintf(&cty->conn, "Invalid user name.\r\n");
    else if (dndstat == DND_BADPASS)
	t_fprintf(&cty->conn, "Invalid password.\r\n");
    else if (dndstat != DND_OK)				
	t_fprintf(&cty->conn, "DND not available.\r\n");

    if (dnd)
	t_dndreset_free(dnd);			/* done with dnd connection */
    
    if (dndres)
	t_free(dndres);
	
    return dndstat == DND_OK;
}
/*^L cty_mstat --

    Print malloc counters.
*/

static void cty_mstat(ctystate *cty) {

    t_fprintf(&cty->conn, "Malloc counters:\r\n");
    t_fprintf(&cty->conn, "Total (net) mallocs: %ld\r\n", malloc_stats.total);
    t_fprintf(&cty->conn, "Mbox's: %ld\r\n", malloc_stats.mbox);
    t_fprintf(&cty->conn, "Summary buckets: %ld\r\n", malloc_stats.summbuck);
    t_fprintf(&cty->conn, "Obufs: %ld\r\n", malloc_stats.obufs);
    t_fprintf(&cty->conn, "Login addr blocks: %ld\r\n", malloc_stats.login_blocks);
    t_fprintf(&cty->conn, "Pref tables: %ld\r\n", malloc_stats.preftab);
    t_fprintf(&cty->conn, "Pref entries: %ld\r\n", malloc_stats.prefentry); 
    t_fprintf(&cty->conn, "Mailing list tables: %ld\r\n", malloc_stats.mltab);
    t_fprintf(&cty->conn, "Mailing list entries: %ld\r\n", malloc_stats.mlentry);
}

/* cty_quit --

    Indicate that connection should be closed by setting cty->done.
*/

static void cty_quit(ctystate *cty) {

    t_fprintf(&cty->conn, "Goodbye.\r\n");
    
    cty->done = TRUE;
}
/* cty_refresh --

    Refresh mailbox (e.g., after reloading message file).  Disconnect any active user session.
    Write out changes to prefs and summaries, discard pref and mlist hash tables (forcing
    re-read) if possible. Clear mbox.checked to force a new summ_check next time the box is
    referenced.  
*/

static void cty_refresh(ctystate *cty) {	

    long	uid;			/* user to refresh */
    char	name[MAX_STR];		/* user in #<uid> form */
    int		fs;
    mbox	*mb;
    char	*p;
    boolean_t	inuse = FALSE;
    boolean_t	writeerr = FALSE;
    int		foldnum;		/* current folder */
    int		hash;
  
 
    p = cty->comline + strlen("REFRESH");
    if (*p++ != ' ' || !*p) {
	 t_fprintf(&cty->conn, "Error: no uid given.\r\n");
	 return;
    }
    if (*p == '#')			/* allow optional # */
	++p;

    p = strtonum(p, &uid);		/* user to refresh */
    if (uid <= 0)  {			/* nonnumeric, or plain bad */
	t_fprintf(&cty->conn, "Invalid uid.\r\n");
	return;
    }
    
    /* must check DND to see where they are now */
    t_sprintf(name, "#%ld", uid);
    if (!cty_whichfs(cty, name, &fs, NULL))
	return;
	    
    mb = force_disconnect(uid, fs, NULL); /* kick them off and lock the box */
    sem_release(&mb->mbsem);		/* must seize in correct order! */

    hash = MBOX_HASH(mb->uid);
    sem_seize(&mbox_sem[hash]);		/* don't let anyone else attach the box */
    sem_seize(&mb->mbsem);		/* (must seize box _after_ list) */

    if (mb->prefs && mb->prefs->dirty) /* write out preferences */
	pref_write(mb);
    
    /* write out all summaries */
    for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {
	if (mb->fold[foldnum].num < 0)	/* skip holes */
	    continue;
	if (mb->fold[foldnum].dirty) 	
	    summ_write(mb, &mb->fold[foldnum]);
	if (!mb->fold[foldnum].dirty) { 	/* make sure it worked! */
	    summ_free(mb, &mb->fold[foldnum]);	
	} else {
	   writeerr = TRUE; 
	}
		
    }
    
    if (mb->prefs) {			/* forget prefs */
	if (mb->attach == 1)		/* if we're only one looking at box */
	    pref_free(mb);		/* free pref hash table */
	else
	    inuse = TRUE;
    }
    if (mb->lists) {			/* forget lists */
	if (mb->attach == 1)		/* if we're only one looking at box */
	    ml_free(mb);		/* free list hash table */
	else
	    inuse = TRUE;
    }
    
    mb->checked = FALSE;		/* next time, must re-check it */

    if (mb->fold && !writeerr) {	/* forget summaries */
        if (mb->attach == 1) {           /* if we're only one looking at box */
    	    t_free(mb->fold);			/* re-check to see what folders exist at that time */
    	    mb->fold = NULL; mb->foldmax = -1;	/* sppml */
	} else {
	    inuse = TRUE;
	}
    }
    sem_release(&mb->mbsem);		/* unlock the box */
    sem_release(&mbox_sem[hash]);	/* and the list */
    mbox_done(&mb);			/* and release our attachment */
    
    if (inuse)
	t_fprintf(&cty->conn, "Box in use -- pref and mailing list caches not flushed.\r\n");
    else
	t_fprintf(&cty->conn, "Ok.\r\n");

}
/* cty_whichfs --

    Look up user in DND to see which fs their box is on.  Returns FALSE if
    name doesn't resolve, or if user belongs on another server. Optionally
    fill in uid if requested.
*/

static boolean_t cty_whichfs(ctystate *cty, char *name, int *fs, long *uid) {

    struct dndresult	*dndres = NULL;	/* results of dndlookup */
    static char *farray[] = {"BLITZSERV", "BLITZINFO", "UID", NULL};
    int		stat;			/* dnd status */
    char	*p;
    boolean_t	ok = TRUE;
    
    stat = t_dndlookup1(name, farray, &dndres);
    if (stat != DND_OK) {
	if (stat == DND_NOUSER)	/* no match */
	    t_fprintf(&cty->conn, "No such user.\r\n");
	else 				/* ==> error talking to dnd */
	    t_fprintf(&cty->conn, "Error talking to DND: %d.\r\n", stat);
	ok = FALSE;
    } else {
	p = t_dndvalue(dndres, "BLITZSERV", farray);
	if (blitzserv_match(p) != m_thisserv) {
	    t_fprintf(&cty->conn, "That user isn't here; try %s.\r\n", p);
	    ok = FALSE;
	} 
	p = t_dndvalue(dndres, "BLITZINFO", farray);
	*fs = fs_match(p);			/* look up which disk they're on */ 
	if (*fs == -1) {			/* Yow! invalid */
	    t_fprintf(&cty->conn, "#### Invalid BLITZINFO field in DND: %s ####.\r\n", p);
	    ok = FALSE;
	}
	if (uid) {		/* caller wants us to return uid? */
	    p = t_dndvalue(dndres, "UID", farray);
	    strtonum(p, uid);
	}
    }
    if (dndres)
	t_free(dndres);
    
    return ok;
}
/* cty_set --

    Set options for this session.
*/

static void cty_set(ctystate *cty) {

    char	*p;
    char	opt[MAX_STR];		/* option name */
    boolean_t	*flag;			/* bit to flip */
    
    p = cty->comline + strlen("SET");
    while (*p == ' ')			/* strip leading spaces */
	++p;
    
    p = strwcpy(opt, p);		/* pick up option name */
    if (strcasecmp(opt, "CLEANOUT_GRACE") == 0) {
	while (*p == ' ')		/* strip spaces again */
	    ++p;
    	strtonum(p, &cleanout_grace);
	return;				/* skip boolean stuff */
    } else {
	    if (strcasecmp(opt, "DEPORT_FWD") == 0)
	    flag = &cty->deport_fwd;
	else if (strcasecmp(opt, "DEPORT_CLEANOUT") == 0)
	    flag = &cty->deport_cleanout;
	else if (strcasecmp(opt, "SMTP_DEBUG") == 0)
	    flag = &smtp_debug;
	else {
		t_fprintf(&cty->conn, "Valid option names are CLEANOUT_GRACE, DEPORT_FWD, DEPORT_CLEANOUT, SMTP_DEBUG.\r\n");
		return;    
	}
    }
    
    while (*p == ' ')			/* strip spaces again */
	++p;
	
    if (strcasecmp(p, "ON") == 0
     || strcasecmp(p, "TRUE") == 0)
     	*flag = TRUE;
    else if (strcasecmp(p, "OFF") == 0
    	  || strcasecmp(p, "FALSE") == 0)
	*flag = FALSE;
    else 
    	t_fprintf(&cty->conn, "Specify ON or OFF.\r\n");
}

/* cty_uid --
    cty_user --
    
    Show info about one user.
*/

static void cty_uid(ctystate *cty) {

    char	uid[MAX_STR];		/* in #<uid> form */
    char	*p;
    
    p = cty->comline + strlen("UID");
    if (*p++ != ' ' || !*p) {
	 t_fprintf(&cty->conn, "Error: no uid given.\r\n");
	 return;
    }
    if (*p == '#')			/* allow optional # */
	++p;
    
    t_sprintf(uid,"#%s", p);		/* #<uid> */
    
    cty_showuser(cty, uid);		/* display away */

}

static void cty_user(ctystate *cty) {

    char	*p;
    
    p = cty->comline + strlen("USER");
    if (*p++ != ' ' || !*p) {
	 t_fprintf(&cty->conn, "Error: no user name given.\r\n");
	 return;
    }
        
    cty_showuser(cty, p);		/* display away */
}
 
/* cty_showuser --

    Display information about user.  Consult the dnd to see if we're their
    server (if not, display just the dnd information).
*/

static void cty_showuser(ctystate *cty, char *name) {

    struct dndresult	*dndres = NULL;	/* results of dndlookup */
    int			stat;		/* dnd status */
    char		*p;
    long		uid;			/* uid to report on */
    long		dnd_serv;		/* which server they use */
    int			fs;			/* and which fs */
    char		pref[PREF_MAXLEN];	/* to check prefs */
    char		lastlog[PREF_MAXLEN];	/* date/time of last login */
    char		vers[PREF_MAXLEN];	/* client version */
    char		*dndname;		/* name from DND */
    int			foldnum;		/* current folder */
    long		foldlen;		/* its length in bytes */
    long		boxlen;			/* total bytes in box */
    int			foldcount;		/* non-standard folders defined */
    long		foldmsgs;		/* messages in them */
    mbox		*mb;
    
    stat = t_dndlookup1(name, farray, &dndres);
            
    if (stat == DND_OK) { 		/* resolved uniquely? */
	dndname = t_dndvalue(dndres, "NAME", farray);
       	t_fprintf(&cty->conn, "Name: %s; ", dndname);
       	t_fprintf(&cty->conn, "Nickname: %s; ", t_dndvalue(dndres, "NICKNAME", farray));
	if (deptclass_defined) 
	    t_fprintf(&cty->conn, "Deptclass: %s; ", t_dndvalue(dndres, "DEPTCLASS", farray));	
	p = t_dndvalue(dndres, "UID", farray);
	strtonum(p, &uid);
	t_fprintf(&cty->conn, "Uid: %s\r\n", p);
       	t_fprintf(&cty->conn, "Mailaddr: %s\r\n", t_dndvalue(dndres, "MAILADDR", farray));
	p = t_dndvalue(dndres, "DUP", farray);
	if (strlen(p) > 0) {
	    t_fprintf(&cty->conn, "DUP: %s\r\n", p);
	}
	
	p = t_dndvalue(dndres, "BLITZSERV", farray);
	dnd_serv = blitzserv_match(p);	/* try to locate server in our table */
	if (dnd_serv == -1)
	    t_fprintf(&cty->conn, "#### BAD Blitzserv: %s\r\n", p);
	else
	    t_fprintf(&cty->conn, "Blitzserv: %s\r\n", p);
	p = t_dndvalue(dndres, "BLITZINFO", farray);
       	t_fprintf(&cty->conn, "Blitzinfo: %s\r\n", p);

	if (dnd_serv == m_thisserv) {	/* iff they are our user */
	    fs = fs_match(p);		/* look up which disk they're on */
	    mb = mbox_find(uid, fs, FALSE);	/* locate box */
	    if (fs != -1 && fs != mb->fs) {
	    	t_fprintf(&cty->conn, 
		"#### DND has Blitzinfo %s; we have box on %s\r\n",
	    	m_filesys[fs], m_filesys[mb->fs]);
	    }
	    if (!pref_get(mb, PREF_LASTLOG, pref))		
		t_fprintf(&cty->conn, "Never logged in.\r\n");
	    else {
		unquote(lastlog, pref);
		if (mb->user) {
		    t_fprintf(&cty->conn, "On since %s\r\n", lastlog);
		    if (mb->user->version) {
			unquote(vers, mb->user->version);
			t_fprintf(&cty->conn, "Version: %s\r\n", vers);	
		    } 
		} else
		    t_fprintf(&cty->conn, "Last login at %s\r\n", lastlog);
	    }
	    t_fprintf(&cty->conn, "%ld inbox messages; %ld trash messages; %ld Sent Msgs\r\n", 
	    		fold_size(mb, &mb->fold[INBOX_NUM]), fold_size(mb, &mb->fold[TRASH_NUM]),
			fold_size(mb, &mb->fold[AUDIT_NUM]));
			
	    boxlen = foldcount = foldmsgs = 0;
	    /* total up all folders */
	    for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {
	    	if (mb->fold[foldnum].num < 0)	/* skip holes */
		    continue;
		foldlen = fold_total(mb, &mb->fold[foldnum]);
		if (mb->fold[foldnum].foldlen != foldlen) {
		    t_fprintf(&cty->conn, "#### Folder %d real length %ld; folder.foldlen = %ld\r\n",
		    			foldnum, foldlen,  mb->fold[foldnum].foldlen);
		}
		boxlen += foldlen;
		if (foldnum >= DEFAULT_FOLDCOUNT) {
		    ++foldcount;
		    foldmsgs += fold_size(mb, &mb->fold[foldnum]);
		}
	    }
	    if (foldcount > 0) {
	    	t_fprintf(&cty->conn, "%d user-defined folders; %ld messages\r\n", foldcount, foldmsgs);
	    }
	    
	    t_fprintf(&cty->conn, "Mailbox length: %ld bytes\r\n", boxlen);
	    if (mb->boxlen != boxlen) {		/* these _should_ always be the same */
		t_fprintf(&cty->conn, "#### mb->boxlen: %ld bytes\r\n", mb->boxlen);	    	
	    }
	    if (pref_get(mb, PREF_FWD, pref) && strlen(pref) > 2) {
		t_fprintf(&cty->conn, "Forwarding address: %s\r\n", pref);
		cty_showforward(cty, mb, dndname);
	    }
	    if (pref_get(mb, PREF_VACATION, pref))
		t_fprintf(&cty->conn, "Vacation message set.\r\n");
	    mbox_done(&mb);
	}
    } else {
	if (stat == DND_AMBIG || stat == DND_VAGUE) /* ambiguous name */
	    t_fprintf(&cty->conn, "Ambiguous name.\r\n");
	else if (stat == DND_NOUSER)	/* no match */
	    t_fprintf(&cty->conn, "No such user.\r\n");
	else 				/* ==> error talking to dnd */
	    t_fprintf(&cty->conn, "Error talking to DND: %d\r\n", stat);
    }
    
    if (dndres)
	t_free(dndres);
	    
}
/* cty_showforward --

    Resolve and display forwarding address.  Remember that the recipient's local
    aliases are used in the resolution.
    
    Forwarding recipientes are generally marked "noshow", but we ignore that
    bit and display them anyway.
*/

static void cty_showforward(ctystate *cty, mbox *mb, char *name) {

    recip	*r, *onefwd;		/* forwarding recipients */
    int		recipcount = 0;
    int		depth = 0;
    char	recipname[MAX_ADDR_LEN];
    static char *stats[] = { "",	/* RECIP_OK */
    	   " ### ambiguous name",	/* RECIP_AMBIG */
	   " ### invalid address",	/* RECIP_BAD_ADDRESS */
	   " ### send permission denied", /* RECIP_NO_SEND */
	   " ### DND unavailable",	/* RECIP_NO_DND */
	   " ### forwarding loop detected!" };
    
    r = alloc_recip(&recipcount);	/* construct recipient */
    
    strcpy(r->name, name);		/* get user's full DND name */
    r->addr[0] = 0;			/* can derive addr from name */
    r->id = mb->uid;			/* deliver to local box */
    r->timestamp = mactime();		/* record time resolved */
    r->local = TRUE;			/* a blitz address */
    r->blitzserv = m_thisserv;		/* on this server */
    strcpy(r->blitzfs, m_filesys[mb->fs]); /* and this filesystem */
    r->stat = RECIP_OK;
    
    check_forward(&r, depth, &recipcount); 	/* resolve forwarding addr */
    
    t_fprintf(&cty->conn, "  Resolves to: \r\n");
    for (onefwd = r->next ;; onefwd = onefwd->next) {
	/* except to report error, don't show forwardee */
	if (onefwd->id != mb->uid || strlen(onefwd->addr) > 0 || onefwd->stat != RECIP_OK) {
	    strcpy(recipname, onefwd->addr);/* usually return internet addr */
	    if (onefwd->stat != RECIP_OK)	/* but return name they gave if error */
		strcpy(recipname, onefwd->name);
	    else if (!recipname[0])		/* local recip w/ host suppressed */
		strcpy(recipname, onefwd->name);/* return DND name */
	    if (onefwd->nosend)
		t_fprintf(&cty->conn, "      (%s)%s\r\n", recipname, stats[onefwd->stat]);
	    else
		t_fprintf(&cty->conn, "      %s%s\r\n", recipname, stats[onefwd->stat]);
	}
    	if (onefwd == r)
	    break;			/* end of circular list */
    }
    
    free_recips(&r);
    
}
/* cty_updatelists --

    Send all public lists to all other servers.
*/

static void cty_updatelists(ctystate *cty) {
    
    if (confirm(&cty->conn, "Really update public lists on all other servers? [Y or N] ")) {
	t_fprintf(&cty->conn, "Sending update messages..."); t_fflush(&cty->conn);
	pubml_sendupdate_all();
	t_fprintf(&cty->conn, "...update messages queued.\n");
	t_fprintf(&cty->conn, "Allow a few minutes for the messages to reach the other servers\n");
    }

}
/* cty_xfer --

    Transfer mailbox to new disk or new server.  An intra-server transfer just
    involves copying all the files to the new disk; interserver transfers use
    the special box transfer protocol.
    
    For simplicity, only one xfer command is accepted at a time; this avoids some
    messy synchronization issues (like, what happens if two xfer commands are issued
    on the same box simultaneously).
*/

static void cty_xfer(ctystate *cty) {	

    long	uid;			/* user to move */
    int		fs;			/* disk they're now on */
    char	*dest;			/* destination server or disk */
    char	name[MAX_STR];		/* user in #<uid> form */
    char	*p;
  
#define CHECK_SPACE(x) if (*x != ' ') { \
	t_fprintf(&cty->conn, "Syntax is:  XFER <uid> <host>|<path>.\r\n"); goto done; }\
	while(*x == ' ') ++x;
     	
    p = cty->comline + strlen("XFER");
    CHECK_SPACE(p);

    p = strtonum(p, &uid);		/* user to move */
    if (uid <= 0)  {			/* nonnumeric, or plain bad */
	t_fprintf(&cty->conn, "Invalid uid.\r\n");
	goto done;;
    }
    CHECK_SPACE(p);
    dest = p;				/* destination disk/server */

    t_sprintf(name, "#%ld", uid);
    
    /* must check DND to see where they are now */
    if (!cty_whichfs(cty, name, &fs, NULL))
	goto done;;

    if (*dest == '/')			/* leading / distinguishes pathnames */
	local_xfer(cty, uid, fs, dest); /* transfer to new disk on this server */
    else {
	if (!remote_xfer(cty, uid, fs, dest))	/* transfer to a new server */
	    t_fprintf(&cty->conn, "\r\nTransfer failed.\r\n");	
	else
	    t_fprintf(&cty->conn, "\r\nTransfer finished.\r\n");
    }

done:    
     
    pthread_mutex_lock(&global_lock);
    relocate_time = mactime();		/* remember time of most recent transfer */
    pthread_mutex_unlock(&global_lock);
    
}
/* cty_deport --

    Copy mailbox to a blitz server in a different domain.  Destination server must
    be configured to accept transfers from us, and user must already have an account
    in the new DND. All messages, folders, and mailing lists are copied to the new
    server; preferences are not copied. After the transfer is completed, a forward
    address pointing to the new domain is established, and a message announcing the
    transfer is stuffed in the local box.
        
*/

static void cty_deport(ctystate *cty) {	

    long	uid;			/* user to move */
    int		fs;			/* disk they're now on */
    char	*name;			/* name on this system */
    char	*desthost = NULL;	/* destination server hostname */
    char	fwdaddr[MAX_STR];
    char	*comma;
       	
    name = cty->comline + strlen("DEPORT");
    
    comma = index(name, ',');		/* find end of first name */
    if (comma) {
    	*comma = 0;			/* terminate local name */
	desthost = comma+1;		/* hostname is second */
	comma = index(desthost, ','); 	/* domain name is third */
    }
    if (!comma) {
    	t_fprintf(&cty->conn, "Syntax is:  DEPORT <user>,<blitzserv>,<domain>.\r\n");
	goto done;
    }
    *comma = 0;				/* terminate hostname */
    
    addhost(name, fwdaddr, comma+1);	/* fwdaddr = name@new-domain */
    
    /* check DND to get uid & fs */
    if (!cty_whichfs(cty, name, &fs, &uid))
	goto done;

    if (blitzserv_match(desthost) >= 0) {
	t_fprintf(&cty->conn, "Server %s is local; use XFER, not DEPORT.\r\n", desthost);
	goto done;
    }
    if (!remote_deport(cty, uid, fs, name, desthost, fwdaddr)) /* transfer to a new server */
	t_fprintf(&cty->conn, "\r\nTransfer failed.\r\n");	
    else {
	t_fprintf(&cty->conn, "\r\nTransfer finished.\r\n");
    }
done:    
     
    pthread_mutex_lock(&global_lock);
    relocate_time = mactime();		/* remember time of most recent transfer */
    pthread_mutex_unlock(&global_lock);
    
}


/* local_xfer --

    Transfer mailbox to new disk.
*/

static void local_xfer(ctystate *cty, long uid, int oldfs, char *diskname) {	

    int		newfs;			/* filesystem to move them to */
    mbox	*mb;			/* their box */
    char	oldname[MBOX_NAMELEN];	/* name of old box directory */

    if ((newfs = fs_match(diskname)) < 0) {
	t_fprintf(&cty->conn, "Unknown filesystem name: %s.\r\n", diskname);
	return;
    }
	
    if (oldfs == newfs) {		/* watch it! */
	t_fprintf(&cty->conn, "Box #%ld is already on %s!\r\n", uid, diskname);
	return;	
    }
    
    mb = mbox_find(uid, oldfs, TRUE);	/* locate box; not changing DND */
    sem_seize(&mb->mbsem);		/* lock it for the duration */

    if (mb->xfering || mb->gone) {	/* synchronize with other transfers */
	t_fprintf(&cty->conn, "Mailbox is already being transferred.\r\n");
	sem_release(&mb->mbsem);	/* unlock & give up */	
	mbox_done(&mb);
	return;
    }
    mb->xfering = TRUE;			/* lock out other transfer attempts */
     
    strcpy(oldname, mb->boxname);	/* remember current name */
    
    mb->fs = newfs;			/* point them to new disk */
    t_sprintf(mb->boxname, "%s%s%ld", m_filesys[mb->fs], BOX_DIR, mb->uid); /* construct new name */
    
    t_fprintf(&cty->conn, "Copying..."); t_fflush(&cty->conn);
    
    if (!do_cp(oldname, mb->boxname)) {
	t_fprintf(&cty->conn, "Copy failed; backing out.\r\n");
	mb->fs = oldfs;			/* put back old name & fs values */
	strcpy(mb->boxname, oldname);
    } else {				/* copy successful; we are now committed */
	t_fprintf(&cty->conn, "Changing DND..."); t_fflush(&cty->conn);
	record_fs(mb);			/* alter dnd entry */
	t_fprintf(&cty->conn, "Removing old box..."); t_fflush(&cty->conn);	
	if (!do_rm(oldname)) 
	    t_fprintf(&cty->conn, "rm failed: %s.\r\n", strerror(pthread_errno()));
	t_fprintf(&cty->conn, "Transfer done.\r\n");		
    }
    mb->xfering = FALSE;		/* transfer finished */
    sem_release(&mb->mbsem);		/* finally, release the box */
    mbox_done(&mb);
}

/* remote_xfer --

    Transfer mailbox to different server.
*/

static boolean_t remote_xfer(ctystate *cty, long uid, int fs, char *servname) {

    int		hostnum;		/* destination server */
    t_file	*conn;			/* SMTP connection to it */
    mbox	*mb;			/* box to move */
    char	buf[MAX_STR];		/* last SMTP response */
    boolean_t	ok = FALSE;		/* returned: transfer status */
    int		foldnum;		/* current folder */

    if ((hostnum = blitzserv_match(servname)) < 0) {
	t_fprintf(&cty->conn, "Invalid server name: %s.\r\n", servname);
	return FALSE;
    }
    if (hostnum == m_thisserv) {	/* don't deadlock talking to self */
	t_fprintf(&cty->conn, "Hey!  That's us!.\r\n");
	return FALSE;
    }
    
    if ((conn = serv_connect(m_server[hostnum])) == NULL) {
	t_fprintf(&cty->conn, "Unable to connect to destination server.\r\n");
	return FALSE;
    }
    
    /* disconnect any active user & lock the box */
    mb = force_disconnect(uid, fs, NULL);
    
    if (mb->xfering || mb->gone) {		/* synchronize with other transfers */
	t_fprintf(&cty->conn, "Mailbox is already being transferred.\r\n");
	sem_release(&mb->mbsem);		/* unlock & give up */	
	mbox_done(&mb);
	t_fclose(conn);				/* close connection */
	return FALSE;
    }
    mb->gone = TRUE;				/* don't deliver anything more here */
    mb->xfering = TRUE;				/* lock out other transfer attempts */
    
    sem_release(&mb->mbsem);			/* don't keep locked whole time */
    
    t_fprintf(conn, "XFER #%ld\r\n", uid); /* begin the transfer */
    if (!checkresponse(conn, buf, SMTP_BLITZON)) {
	t_fprintf(&cty->conn, "XFER command rejected: %s\r\n", buf);
	goto cleanup;
    }
    
    if (!xfer_prefs(cty, conn, mb))		/* transfer preferences (first!) */
	goto cleanup;
	
    if (!xfer_lists(cty, conn, mb))		/* transfer mailing lists */
	goto cleanup;

    for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {
	if (mb->fold[foldnum].num < 0)		/* skip holes */
	    continue;
	if (!xfer_fold(cty, conn, mb, &mb->fold[foldnum])) /* transfer each folder */
	    goto cleanup;
    }

    if (!xfer_vacation(cty, conn, mb))		/* transfer vacation text & list */
	goto cleanup;
	
    /* that's it; tell the other server to commit to the change */
    t_fprintf(conn, "DONE\r\n");
    if (!checkresponse(conn, buf, SMTP_OK))
	goto cleanup;

    if (!do_rm(mb->boxname)) 			/* remove box from our disk */
	t_fprintf(&cty->conn, "rm failed: %s.\r\n", strerror(pthread_errno()));
    
    ok = TRUE;					/* all done; return good status */
    t_sprintf(buf, "Box #%ld transferred to %s.", uid, servname);
    log_it(buf);
    
    /* send notification control to force their notifier to re-register
       with the new server */
    do_notify(uid, NTYPE_CTL, 0, NCTL_RESET, NCTL_RESET_LEN, 0);
       
    cleanup:					/* here to back out if trouble */
    
    if (!ok) mb->gone = FALSE;			/* if backing out; resurrect */
    mb->xfering = FALSE;			/* transfer finished */
    
    mbox_done(&mb);
    t_fclose(conn);				/* close connection */
    
    return ok;
}
/* remote_deport --

    Like remote_xfer, but copying to different domain.
*/

static boolean_t remote_deport(ctystate *cty, long uid, int fs, 
	char *destname, char *desthost, char *fwdaddr) {

    t_file	*conn;			/* SMTP connection to destination */
    mbox	*mb;			/* box to move */
    char	buf[MAX_STR];		/* last SMTP response */
    boolean_t	ok = FALSE;		/* returned: transfer status */
    int		foldnum;		/* current folder */
    
    if ((conn = serv_connect(desthost)) == NULL) {
	t_fprintf(&cty->conn, "Unable to connect to destination server.\r\n");
	return FALSE;
    }
    
    /* disconnect any active user & lock the box */
    mb = force_disconnect(uid, fs, NULL);
    
    if (mb->xfering || mb->gone) {		/* synchronize with other transfers */
	t_fprintf(&cty->conn, "Mailbox is already being transferred.\r\n");
	sem_release(&mb->mbsem);		/* unlock & give up */	
	mbox_done(&mb);
	t_fclose(conn);				/* close connection */
	return FALSE;
    }
    mb->xfering = TRUE;				/* lock out other transfer attempts */

    /* don't set mb->gone -- local box remains active */
    
    sem_release(&mb->mbsem);			/* don't keep locked whole time */
    
    t_fprintf(conn, "XFER %s\r\n", destname); /* begin the transfer */
    if (!checkresponse(conn, buf, SMTP_BLITZON)) {
	t_fprintf(&cty->conn, "XFER command rejected: %s\r\n", buf);
	goto cleanup;
    }

    if (!xfer_prefs(cty, conn, mb))		/* transfer preferences (first!) */
	goto cleanup;
    
    if (!xfer_lists(cty, conn, mb))		/* transfer mailing lists */
	goto cleanup;

    for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {
	if (mb->fold[foldnum].num < 0)		/* skip holes */
	    continue;
	if (!xfer_fold(cty, conn, mb, &mb->fold[foldnum])) /* transfer each folder */
	    goto cleanup;
    }

    if (!xfer_vacation(cty, conn, mb))		/* transfer vacation text & list */
	goto cleanup;

    pref_rem(mb, PREF_VACATION);		/* clear vacation at this end (no dups) */
    
    /* that's it; tell the other server to commit to the change */
    t_fprintf(conn, "DONE\r\n");
    if (!checkresponse(conn, buf, SMTP_OK))
	goto cleanup;
    
    ok = TRUE;					/* all done; return good status */
    t_sprintf(buf, "Box #%ld deported to %s.", uid, desthost);
    log_it(buf);
    
    if (cty->deport_fwd && fwdaddr) {		/* establish forwarding? */
	pref_set(mb, PREF_FWD, fwdaddr); 	/* forward to their new home */
    }
    
    if (cty->deport_cleanout) {		/* clean out this box? */
	t_fprintf(&cty->conn, "Cleaning up..."); t_fflush(&cty->conn);
	for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {
	    if (mb->fold[foldnum].num < 0)		/* skip holes */
		continue;
	    fold_remove(mb, &mb->fold[foldnum]); /* erase each folder */
	}
    	sem_seize(&mb->mbsem);
	/* recreate default folders */
	strcpy(mb->fold[TRASH_NUM].name, TRASH_NAME);
	mb->fold[TRASH_NUM].num = TRASH_NUM;
	mb->fold[TRASH_NUM].summs = NULL;
	strcpy(mb->fold[AUDIT_NUM].name, AUDIT_NAME);
	mb->fold[AUDIT_NUM].num = AUDIT_NUM;
	mb->fold[AUDIT_NUM].summs = NULL;
	strcpy(mb->fold[INBOX_NUM].name, INBOX_NAME);
	mb->fold[INBOX_NUM].num = INBOX_NUM;
	mb->fold[INBOX_NUM].summs = NULL;	
    	sem_release(&mb->mbsem);
    }
    
    /* send them a farewell message, if one is defined */
    initialmess(mb, NULL, f_deportmess);
	
    cleanup:					/* here to back out if trouble */
    
    mb->xfering = FALSE;			/* no longer in transit */
    mbox_done(&mb);
    t_fclose(conn);				/* close connection */

    return ok;
}
/* xfer_fold --

    Transfer all the messages in the given folder.
*/

static boolean_t xfer_fold(ctystate *cty, t_file *conn, mbox *mb, folder *fold) {

    summbuck	*p;			/* current bucket */
    summinfo	*summ;			/* current summary in bucket */
    char	*nextsum;		/* to locate next summary */
    boolean_t	ok = TRUE;		/* folder sent ok? */
    char	foldname[2*FOLD_NAMELEN+2]; /* quoted folder name */
    char	resp[MAX_STR];		/* reponse from other server */
    long	destfold = fold->num;	/* folder number on destination server */

    t_fprintf(&cty->conn, "Fold #%d...", fold->num); t_fflush(&cty->conn);

    sem_seize(&mb->mbsem);

    /* if non-default folder, must tell them to create it */
    if (fold->num >= DEFAULT_FOLDCOUNT) {
	strncpy_and_quote(foldname, fold->name, sizeof(foldname));
	t_fprintf(conn, "FOLD %d %s\r\n", fold->num, foldname);
	if (!checkresponse(conn, resp, SMTP_OK)) {
	    t_fprintf(&cty->conn, "Error creating folder %d %s: \r\n  %s",
				    fold->num, foldname, resp);
	    ok = FALSE;
	    goto cleanup;
	}
	/* if folder already existed, their folder number may be different
	   so always pick up their number from the "ok" response */
	strtonum(resp+4, &destfold);
	if (destfold < DEFAULT_FOLDCOUNT) {
	    t_fprintf(&cty->conn, "Error creating folder %d %s: \r\n  %s",
				    fold->num, foldname, resp);
	    ok = FALSE;
	    goto cleanup;
	}		
    }
    
    if (fold->summs == NULL)		/* get summaries, if not yet present */
    	summ_read(mb, fold);

    /* run through each bucket of folder */
    for (p = fold->summs; p != NULL; p = p->next) {
	for (nextsum = p->data; nextsum - p->data < p->used; nextsum += summ->len) {
	    summ = (summinfo *) nextsum;
	    if (!xfer_mess(cty, conn, mb, destfold, summ)) { /* do 1 transfer */
		ok = FALSE;
		goto cleanup;			/* transfer error; give up */
	    }
	    t_fprintf(&cty->conn, "."); 
	    if (t_fflush(&cty->conn) < 0) {
	    	ok = FALSE;		/* abort if control connection interrupt */
		goto cleanup;
	    }
	}
    }    

cleanup:
    sem_release(&mb->mbsem);

    /* explicit yield here to make sure we don't starve threads trying to lock this box */
    pthread_yield();
    
    return ok;
}
/* xfer_mess --

    Transfer one message from the given folder.

    The syntax of the MESS command is:
    
	>>> MESS <folder #> <summary info>
	<<< 250 Already have it
	-or-
	<<< 354 Send it
	>>> <BLTZ format message...>
	<<< 250 Ok

*/

static boolean_t xfer_mess(ctystate *cty, t_file *conn, mbox *mb, 
			   int destfold, summinfo *summ) {

    char 	buf[SUMMBUCK_LEN];	/* long enough for max summary */
    char	resp[MAX_STR];		/* reponse from other server */
    char	fname[MESS_NAMELEN];	/* message filename */
    fileinfo	head;			/* pieces of the message itself */
    fileinfo	text;			/* '' */
    enclinfo	*encl;			/* '' */
    enclinfo	*ep;			/* temp */
    long	mtype;			/* message type */
    int		i;
    
    summ_fmt(summ, buf);		/* format the summary info */	

    /* tell them what we're sending */
    t_fprintf(conn, "MESS %d %s\r\n", destfold, buf);
    if (checkresponse(conn, resp, SMTP_OK))
	return TRUE;			/* they already have the message */
	
    /* otherwise, they should be asking to receive it */
    if (atoi(resp) != SMTP_BLITZON) {
	t_fprintf(&cty->conn, "Error sending message %ld: \r\n  %s",
				summ->messid, resp);
	return FALSE;			/* unexpected response */
    }

    /* they don't yet have the message; open it */

    mess_name(fname, mb, summ->messid);	/* get message name */
    
    if (!mess_open(fname, &head, &text, &encl, NULL, &mtype)) {
	t_fprintf(&cty->conn, "Cannot open %s\r\n", fname);
	return FALSE;
    }
    
    t_fprintf(conn, "BLTZ");		/* identify xmit format */
    xmit_block(conn, &head, "HEAD");	/* send header */
    xmit_block(conn, &text, "TEXT");	/* send text */
    for (ep = encl; ep; ep = ep->next) {
	xmit_encl(conn, ep);	    	/* send enclosures */
    }
    
    for (i = 0; i < 4; ++i)		/* indicate end of message */
	t_putc(conn, 0);		/* with a zero-length "block" */

    clean_encl_list(&encl);		/* clean up enclosures */
    /* (don't need to finfoclose head & text; they aren't temp files) */

    return checkresponse(conn, resp, SMTP_OK);

}
/* xfer_lists --

    Transfer mailing lists.  Returns FALSE on error.
    For each list in box:
    
      >>> LIST <name>
      >>> member 1
      >>> ...
      >>> member n
      >>> .

*/

static boolean_t xfer_lists(ctystate *cty, t_file *conn, mbox *mb) {

    ml_namep	np;			/* current list name */
    int		i;
    ml_data	*ml_head = NULL;	/* list contents */
    ml_data	*ml;			/* current member */
    char	*p, *q;		
    char	buf[MAX_STR];	

    t_fprintf(&cty->conn, "Lists..."); t_fflush(&cty->conn);
    
    sem_seize(&mb->mbsem);
    if (!mb->lists)			/* if no hash table, get it */
	ml_readhash(mb);
    sem_release(&mb->mbsem);
       
    /* search hash table for all entries */
    for (i = 0; i < ML_HASHMAX; i++) {
    	for (np = mb->lists->hashtab[i]; np; np = np->next) {
	    t_fprintf(conn, "LIST %s\r\n", np->name); /* cue other end */
	    /* get list contents */
	    if (!ml_get(mb, np->name, &ml_head, np->name)) {
		t_fprintf(&cty->conn, "#### List %s vanished!?\r\n", np->name);
		return FALSE;
	    }
	    /* now run through list contents */
	    for (ml = ml_head; ml; ml = ml->next) {
		/* for each name in block */
		for (p = ml->data; p < ml->data + ml->len; p = q) {
		    q = index(p, '\n');		/* deal with \n name terminators */
		    if (!q)			/* don't run off end */
			q = ml->data + ml->len;
		    else	
			*q++ = 0;		/* terminate string */ 
			
		    /* print a name */
		    if (*p == '.')		/* escape leading '.' */
			t_putc(conn, '.');
		    t_fprintf(conn, "%s\r\n", p);
		}  
	    }
	    t_fprintf(conn, ".\r\n");	/* mark end of list */
	    ml_clean(&ml_head);		/* free up the list copy */
	    if (!checkresponse(conn, buf, SMTP_OK))
		return FALSE;	    
	}				/* end of 1 list */
    }					/* end of all lists */

    return TRUE;			/* lists transferred ok */
}
/* xfer_prefs --

    Transfer mailing lists.  Returns FALSE on error.
    For each list in box:
    
      >>> PREF
      <<< 354 ok
      >>> pref name 1 "pref value 1"
      >>> ...
      >>> pref name n "pref value n"
      >>> .

*/

static boolean_t xfer_prefs(ctystate *cty, t_file *conn, mbox *mb) {

    char 	buf[MAX_STR];		/* response from peer */
    int		i;
    prefp	p;

    t_fprintf(&cty->conn, "Prefs..."); t_fflush(&cty->conn);
    
    t_fprintf(conn, "PREF\r\n"); 	/* here come the prefs */
    if (!checkresponse(conn, buf, SMTP_BLITZON))
	return FALSE;
	
    sem_seize(&mb->mbsem);
    if (!mb->prefs)			/* get prefs, if not yet here */
	pref_read(mb);

    /* traverse each list in the hash table */
    for (i = 0; i < PREF_HASHMAX; ++i) {
	for (p = mb->prefs->hashtab[i]; p != NULL; p = p->next) {
	    t_fprintf(conn, "%s %s\r\n", p->name, p->name + p->namelen + 1);
	}
    }
    sem_release(&mb->mbsem);
    
    t_fprintf(conn, ".\r\n");
    return checkresponse(conn, buf, SMTP_OK);

}

/* xfer_vacation --

    Transfer vacation message text.
    
      >>> VACA
      >>> <block of type "VACA">
      <<< 250 OK

*/

static boolean_t xfer_vacation(ctystate *cty, t_file *conn, mbox *mb) {

    fileinfo	finfo;			/* vacation file */
    char	resp[MAX_STR];		/* response code */

    if (!open_vacation(mb, &finfo))
	return TRUE;			/* no vacation; nothing to do */

    t_fprintf(&cty->conn, "Vacation..."); t_fflush(&cty->conn);
	
    t_fprintf(conn, "VACA\r\n");	/* sending vacation text */
    xmit_block(conn, &finfo, "VACA");	/* here it is */
    
    return checkresponse(conn, resp, SMTP_OK);
}

/* linfo_display --

    Format & display public list ownership & access info.
*/

static void linfo_display(t_file *f, long owner, long group, char lacc[4]) {

    char	accstr[4];		/* ascii version of 1 access set */
    char	name[MAX_STR];		/* DND name */
    
    (void) uid_to_name(owner, name);
    t_fprintf(f, "Owner:   %s\r\n", name);
    (void) uid_to_name(group, name);
    t_fprintf(f, "Group:   %s\r\n", name);
    fmt_acc(lacc[0], accstr);
    t_fprintf(f, "Access:  (Owner: %s; ", accstr);
    fmt_acc(lacc[1], accstr);
    t_fprintf(f, "Group: %s; ", accstr);
    fmt_acc(lacc[2], accstr);
    t_fprintf(f, "World: %s)\r\n", accstr);
}

/* runcmd --

    Similar to system(3), but avoid starting up a shell.
*/

int runcmd(char *name, char **argv) {

    union wait  stat;
    int		pid;			/* child process */
    int		i;
    char	buf[MAX_STR];
    	        
    pid = fork();			/* fork subprocess  */
    if (pid == -1) {
	t_perror("runcmd: fork");
    } else if (pid == 0) {		/* CHILD: */
    	for (i = 3; i < maxfds; ++i)	/* close all but standard files */
	    close(i);
	execv(name, argv);
	t_sprintf(buf, "Cannot execv %s", name);
	perror(buf);
	exit(1);
    } 					

    /* wait for child to finish, get status */
    if (wait4(pid, &stat, 0, NULL) != pid) {
	t_perror("runcmd: wait4");
	return -1;
    }
    
    return stat.w_retcode;
}
/* askacc --

    Prompt for list access; convert to octal bitmap version.
    Return 0 if connection lost.
*/
char askacc(t_file *f, char *prompt) {

    char	acc = 0;		/* access bitmap */
    char	accstr[MAX_STR];	/* encoded accesses */
    char	*p;
    
    if (!asks(f, accstr, prompt, NULL))	/* get access letters (no default) */
	return 0;			/* connection lost */
    for (p = accstr; *p; ++p) {
	if (*p == 'r' || *p == 'R')
	    acc |= LACC_READ;
	else if (*p == 'w' || *p == 'W')
	    acc |= LACC_WRITE;
	else if (*p == 's' || *p == 'S')
	    acc |= LACC_SEND;
    }
    return acc + '0';
}

/* asks --

    Prompt for string, with optional default.  Return FALSE on disconnect.
*/
boolean_t asks(t_file *f, char *str, char *prompt, char *default_val) {

    if (default_val) 
	t_fprintf(f, "%s [%s] ", prompt, default_val);
    else
	t_fprintf(f, "%s ", prompt);
	
    t_fflush(f);
    t_fseek(f, 0, SEEK_CUR); 			/* set up to read */
    if (t_gets(str, MAX_STR, f) == NULL)
	return FALSE;				/* connection lost */

    if (default_val && strlen(str) == 0)	/* null response; use default if any */
	strcpy(str, default_val);

    t_fflush(f);				/* get set to write again */

    return TRUE;
}

/* askuid --

    Prompt for name, return uid.  Returns -1 if connection lost.
*/
long askuid(t_file *f, char *prompt, char *default_val) {

    char	name[MAX_STR];			/* name entered */
    long	uid;				/* returned: uid to use */
    int		dndstat;
       
    for (;;) {					/* until valid name given */
	if (!asks(f, name, prompt, default_val)) /* prompt for name */
	    return -1;				/* connection lost */
	if ((uid = name_to_uid(name, &dndstat)) >= 0)
	    return uid;				/* valid uid given */
	else if (dndstat == DND_NOUSER)
	    t_fprintf(f, "No such user.\r\n");
	else if (dndstat == DND_AMBIG || dndstat == DND_VAGUE)
	    t_fprintf(f, "Ambiguous user name.\r\n");
	else
	    t_fprintf(f, "Error talking to DND: %d\r\n", dndstat);
    }
}

/* confirm --

    Prompt for 'y' or 'n' reponse.  Prompt string should include leading (but not
    trailing) newline.
*/
boolean_t confirm(t_file *f, char *prompt) {

    int		c;
    
    while(!f->urgent) {
	t_fprintf(f, prompt); t_fflush(f);	/* send prompt */
	t_fseek(f, 0, SEEK_CUR); 		/* set up to read */
	c = t_getc(f);
	t_fflush(f);				/* ready to write again */
	if (c == 'y' || c == 'Y')
	    return TRUE;
	else if (c == 'n' || c == 'N' || c == EOF)
	    return FALSE;
    }
    return FALSE;				/* interrupt counts as "no" */
}

/* fmt_acc --

    Format public mailing list access encoding.
*/

static void fmt_acc(char acc, char *accstr) {

    if (acc & LACC_READ)
	*accstr++ = 'R';
    if (acc & LACC_WRITE)
	*accstr++ = 'W';
    if (acc & LACC_SEND)
	*accstr++ = 'S';
    *accstr = 0;
}


