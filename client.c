/*	BlitzMail Server -- client communication.
	
    Copyright (c) 1992-95 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

	$Header: /users/davidg/source/blitzserver/RCS/client.c,v 3.6 98/10/21 15:57:41 davidg Exp Locker: davidg $

*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/dir.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef KERBEROS
#include <krb.h>
#endif
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "client.h"
#include "config.h"
#include "mess.h"
#include "deliver.h"
#include "queue.h"
#include "notify/not_types.h"
#include "binhex.h"
#include "cryptutil.h"
#include "smtp.h"

#ifdef KERBEROS
char server_vers[] = "BlitzServ 3.6 POP+Kerberos 9/23/98";
#else
char server_vers[] = "BlitzServ 3.6 POP 9/23/98";
#endif

/* dnd fields needed for validation */
static char *val_farray[] = { "NAME", "UID", "GID", "PERM", "DUP",
			    "BLITZSERV", "BLITZINFO", "EMAIL", "EXPIRES", NULL };
static boolean_t expires_defined = TRUE;

pthread_mutex_t vers_lock;
static char **vers_tab = NULL;      /* protected by vers_lock */
static int  vers_count = 0;
static int  vers_max = 0;

/* table of client commands */

cmdent	cmdtab[CMD_COUNT] = { 
	{ TRUE,  "AUDT", c_audt },
	{ FALSE, "CLEA", c_clea },
	{ FALSE, "CLEM", c_clem },
	{ FALSE, "CLER", c_cler },
	{ TRUE,  "COPY", c_copy },
	{ TRUE,  "DELE", c_dele },
	{ TRUE,  "DELX", c_delx },
	{ TRUE,  "EDAT", c_edat },
	{ TRUE,  "EDEL", c_edel },
	{ TRUE,  "ELIS", c_elis },
	{ TRUE,  "ENCL", c_encl },
	{ TRUE,  "EREM", c_erem },
	{ TRUE,  "EXPR", c_expr },
	{ TRUE,  "FDEF", c_fdef },
	{ TRUE,  "FLIS", c_flis },
	{ TRUE,  "FNAM", c_fnam },
	{ TRUE,  "FREM", c_frem },
	{ TRUE,  "FSUM", c_fsum },
	{ TRUE,  "HEAD", c_head },
        { TRUE,  "HIDE", c_hide },
	{ FALSE, "KRB4", c_krb4 },
	{ TRUE,  "LDAT", c_ldat },
	{ TRUE,  "LDEF", c_ldef },
	{ TRUE,  "LIST", c_list },
	{ TRUE,  "LREM", c_lrem },
	{ TRUE,  "LSTS", c_lsts },
	{ TRUE,  "MARK", c_mark },
	{ TRUE,  "MCAT", c_mcat },
	{ TRUE,  "MDAT", c_mdat },
	{ TRUE,  "MESS", c_mess },
	{ TRUE,  "MOVE", c_move },
	{ TRUE,  "MSUM", c_msum },
	{ FALSE, "NOOP", c_noop },
	{ FALSE, "PASE", c_pase },
	{ FALSE, "PASS", c_pass },
	{ TRUE,  "PDEF", c_pdef },
	{ TRUE,  "PREF", c_pref },
	{ TRUE,  "PREM", c_prem },
	{ FALSE, "PUSH", c_push },
	{ FALSE, "QUIT", c_quit },
	{ TRUE,  "RBCC", c_rbcc },
	{ TRUE,  "RCCC", c_rccc },
	{ TRUE,  "RCPT", c_rcpt },
	{ TRUE,  "RPL2", c_rpl2 },
	{ TRUE,  "RTRN", c_rtrn },
	{ TRUE,  "SEND", c_send },
	{ TRUE,  "SIZE", c_size },
	{ TRUE,  "SLOG", c_slog },
	{ TRUE,  "SUMM", c_summ },
        { TRUE,  "TDEL", c_tdel },
	{ TRUE,  "TEXT", c_text },
	{ TRUE,  "THQX", c_thqx },
	{ TRUE,  "TOPC", c_topc },
	{ TRUE,  "TRSH", c_trsh },
	{ TRUE,  "TSIZ", c_tsiz },
	{ TRUE,  "TSUM", c_tsum },
	{ TRUE,  "UDEL", c_udel },
	{ TRUE,  "UDLX", c_udlx },
	{ FALSE, "UID#", c_uid },
	{ FALSE, "USER", c_user },
	{ TRUE,  "VDAT", c_vdat },
	{ FALSE, "VERS", c_vers },
	{ TRUE,  "VREM", c_vrem },
	{ TRUE,  "VTXT", c_vtxt },
	{ TRUE,  "WARN", c_warn }
};

cmdent	popcmdtab[POPCMD_COUNT] = { 
	{ TRUE,  "DELE", pop_dele },
	{ TRUE,  "LAST", pop_last },
	{ TRUE,  "LIST", pop_list_or_uidl },
	{ TRUE,  "NOOP", pop_noop },
	{ FALSE, "PASS", pop_pass },
	{ FALSE, "QUIT", pop_quit },
	{ TRUE,  "RETR", pop_retr_or_top },
	{ TRUE,  "RSET", pop_rset },
	{ TRUE,  "STAT", pop_stat },
	{ TRUE,  "TOP",  pop_retr_or_top },
	{ TRUE,  "UIDL", pop_list_or_uidl },
	{ FALSE, "USER", pop_user }
};

#define	MIME_VERSION_HEADER	"MIME-VERSION: "
#define	CONTENT_TYPE_BINHEX	"CONTENT-TYPE: APPLICATION/MAC-BINHEX40"

void do_recip(udb *user, recip **recips);
void do_summ(udb *user, folder *fold);
void do_clem(udb *user);
void popprint(udb *user, char *s);
void popprint1(udb *user, char *s1, char *s2);
int parse_command(udb *user);
int popparse_command(udb *user);
messlist *parse_messlist(udb *user);
boolean_t parse_messid(udb *user, long *messid, long *foldnum);
boolean_t parse_num(udb *user, long *num);
boolean_t parse_uns(udb *user, u_long *uns);
boolean_t popparse_num(udb *user, long *num);
boolean_t popparse_uns(udb *user, u_long *num);
void parse_arg(udb *user, char *arg);
boolean_t chars_get(udb *user, long wantlen, fileinfo *out);
boolean_t chars_get_buf(udb *user, long wantlen, char *buf);
void do_validate1(udb *user, char *name);
void do_validate2(udb *user, char *cryptpw);
void do_validate3(udb *user, dndresult	*dndres);
void do_signon(udb *user,boolean_t push);
boolean_t notify_lookup();
void popdo_validate1(udb *user, char *name);
void popdo_validate2(udb *user, char *cryptpw);
void popdo_validate3(udb *user,  dndresult *dndres);
void popdo_signon(udb *user);
boolean_t popisdeleted(udb *user, int mindex);
boolean_t pop_expand_encl(long messid, fileinfo *intext, t_file **inf, enclinfo *ep);
boolean_t kpop_peek(udb *user);
extern t_file *exportmess(long messid, fileinfo *head, fileinfo *text, fileinfo *textmess, long mtype);
void catalog_multi(udb *user, t_file *f, long base, long offset, 
			long *len, char *boundary, int level);
boolean_t parse_content_type(char *buf, char *boundary);
boolean_t mime_catalog(udb *user);
void putheadline(udb *user, int level, char *buf);
void foldheadline(udb *user, int level, char *buf);

/* user_init --

    Initialize. Must be called first.
*/

void user_init() {

    int		stat;			/* dnd status */
    int		i;

    pthread_cond_init(&usermax_wait, pthread_condattr_default);
    pthread_mutex_init(&vers_lock, pthread_mutexattr_default);
    
    u_head = NULL;

    do {
    	stat = t_dndfield_exists("EXPIRES", &expires_defined);
    } while (stat == DND_DOWN);
    
    if (stat != DND_OK) {
	t_errprint_l("Error looking for EXPIRES field: %ld", stat);
    }
    
    if (!expires_defined)	/* don't ask for fields not present */
    	val_farray[8] = NULL;

    for (i = 0; i < LOGIN_HASHMAX; ++i)
	login_tab[i] = NULL;	/* initialize login hash table */

}

/* user_alloc --

    Allocate and initialize a udb.  Note that the mb field can't be
    filled in yet, since we don't know who this is until the sign on.
*/

udb *user_alloc() {

    udb		*user;		/* returned: new udb */
            
    user = (udb *) mallocf(sizeof(udb));
    user->next = NULL;
    user->mb = NULL;		/* no mailbox yet */
    user->conn.fd = -1;		/* no connection yet */
    user->uid = 0;
    user->fs = -1;
    user->groupcnt = 0;
    user->prived = FALSE;
    user->name = NULL;
    user->email = NULL;
    user->version = NULL;
    user->currmessfold = -1;
    user->warn = NULL;
    user->dnd = NULL;
    user->validated = user->validating = FALSE;
    user->changing = user->duplicate = FALSE;
    user->shutdown = FALSE;
    user->torecips = user->ccrecips = user->bccrecips = NULL;
    user->recipcount = 0;
    user->wantreceipt = FALSE;
    user->hiderecips = FALSE;
    user->hextext = FALSE;
    user->replyto = NULL;
    user->xtra_audit = -1;
    user->ldat = NULL;
    user->head.fname[0] = 0;	/* null current message */
    user->head.len = 0;
    user->text.fname[0] = 0;
    user->text.len = 0;
    user->ep = NULL;
    user->summ.messid = -1;	/* summary info not valid yet */
    user->summ.enclosures = 0;
    user->summ.sender = user->summ.sender_; /* this is fixed fmt summary */
    user->summ.recipname = user->summ.recipname_;
    user->summ.topic = user->summ.topic_;
    user->summ.sender[0] = 0;
    user->summ.recipname[0] = 0;
    user->summ.topic[0] = 0;
    user->summ.type = MESSTYPE_BLITZ;	/* default to blitz-format message */
    user->summ.len = sizeof(summinfo);

    user->comline[0] = 0;
    
    user->pop = FALSE;		/* assume it's a Blitz client */
    user->pophighestmsg = 0;
    user->popdeleted = NULL;
    user->popdeletedsize = 0;
    user->popdeletedcount = 0;
    
    user->krbvalidated = FALSE;	/* (POP) kerberos ticket not received yet */

    return user;
}

/* user_cmd --

    Thread to read and process commands for a single client.
    
    Note that the mailbox pointer (user->mb) is valid only when
    the user is validated.
*/

any_t user_cmd(any_t user_) {

    udb		*user;			/* user data block */
    mbox	*mb;			/* and mailbox */
    int		cmdnum;			/* index into command table */
    
    user = (udb *) user_;

    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
    
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
	    
	    if ((cmdnum = parse_command(user)) >= 0) { /* lookup */
		if (cmdtab[cmdnum].val && !user->validated)
		    print(user, BLITZ_NOTVAL);	/* must sign on first for this cmd */
		else
		    (*cmdtab[cmdnum].func) (user);	/* call proper function */ 
	    }
	}
	
	if (user->conn.fd >= 0) {
	    t_fflush(&user->conn);	/* flush output, if connected */
	    t_fseek(&user->conn, 0, SEEK_CUR); /* seek to set up to read again */
	}
	
	/* unless in middle of command, don't need dnd server connection */
	if (!user->validating && !user->changing) {
	    if (user->dnd) {		/* dispose of connection */
		t_dndfree(user->dnd);
		user->dnd = NULL;
	    }	
	}
    }
    

    mb = user->mb;			/* remember associated box, if any */
    if (mb) {
	sem_seize(&mb->mbsem);		/* sync w/ duplicate user check */
	free_user(user);		/* clean up & free udb */
	mb->user = NULL;		/* mbox no longer has user active */
	sem_release(&mb->mbsem);	
	mbox_done(&mb);			/* release our use of box */
    } else {				/* no box (never signed on); just free */
	free_user(user);		/* clean up & free udb */    
    }
            
    return 0;
}
/* popuser_cmd --

    Thread to read and process commands for a single POP client.
    
    Note that the mailbox pointer (user->mb) is valid only when
    the user is validated.
*/

any_t popuser_cmd(any_t user_) {

    udb		*user;			/* user data block */
    mbox	*mb;			/* and mailbox */
    int		cmdnum;			/* index into command table */
#ifdef KERBEROS
    boolean_t	firstcmd = TRUE;	/* initial command? */
#endif

    user = (udb *) user_;

    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
    
    popprint(user, POP_WELCOME); /* issue POP greeting */
    if (user->conn.fd >= 0) {
	t_fflush(&user->conn);	/* flush output, if connected */
	t_fseek(&user->conn, 0, SEEK_CUR); /* seek to set up to read again */
    }

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

	/* read command (note: no urgent processing in POP) */

#ifdef KERBEROS
	/* kerberos support -- client sends ticket in "sendauth" format
	   as very first data in session. Must peek to see whether it's
	   a ticket or a POP command */
	if (firstcmd) {
	    firstcmd = FALSE;
	    if (kpop_peek(user)) {		/* if it's a ticket */
		t_fflush(&user->conn);		/* flush, in case error sent */
		t_fseek(&user->conn, 0, SEEK_CUR); /* seek to set up to read again */ 	
		continue;			/* on to next command (no reponse) */
	    }
	} else {
	    user->comp = t_gets(user->comline, MAX_STR, &user->conn);	
	}
#else
	user->comp = t_gets(user->comline, MAX_STR, &user->conn);
#endif
	
	if (user->comp == NULL)  		/* EOF --> connection gone */
	    break;				
	
	/* flag connection as being ready for output (strict alternation, remember)
	    to avoid another pass through t_select before responding */
	user->conn.can |= SEL_WRITE;
	
	if ((cmdnum = popparse_command(user)) >= 0) { /* lookup */
	    if (popcmdtab[cmdnum].val && !user->validated)
		popprint(user, POP_NOTVAL);	/* must sign on first for this cmd */
	    else
		(*popcmdtab[cmdnum].func) (user);	/* call proper function */ 
	}
	
	if (user->conn.fd >= 0) {
	    t_fflush(&user->conn);	/* flush output, if connected */
	    t_fseek(&user->conn, 0, SEEK_CUR); /* seek to set up to read again */
	}	
		
	/* unless in middle of command, don't need dnd server connection */
	if (!user->validating && !user->changing) {
	    if (user->dnd) {		/* dispose of connection */
		t_dndfree(user->dnd);
		user->dnd = NULL;
	    }	
	}
    }
    

    mb = user->mb;			/* remember associated box, if any */
    if (mb) {
	sem_seize(&mb->mbsem);		/* sync w/ duplicate user check */
	free_user(user);		/* clean up & free udb */
	mb->user = NULL;		/* mbox no longer has user active */
	sem_release(&mb->mbsem);	
	mbox_done(&mb);			/* release our use of box */
    } else {				/* no box (never signed on); just free */
	free_user(user);		/* clean up & free udb */    
    }
            
    return 0;
}

/* free_user --

    Clean up and free udb.   Called only from the user_cmd thread
    for this user.  If a routine that is _not_ running in user 
    context wants to look at mb->user, it needs to seize mb->mbsem, 
    to guarantee that the udb isn't freed at an inopportune moment.
    
    This is the only place where conn.fd is closed.  Note the
    way we sync with t_select.
        
    --> mb->mbsem seized <--
*/

void free_user(udb *user) {

    warning	*w;
    udb		*u, *prev;
    
    if (user->conn.fd >= 0) { 
	t_closefd(user->conn.fd);	/* close file & remove from fd table */  
    }
    
    if (user->dnd) {			/* dispose of DND connection */
	t_dndreset_free(user->dnd);	/* note: reset state, then free it */
	user->dnd = NULL;
    }	
    
    if (user->name) {			/* free all attached storage */
	t_free(user->name);
	user->name = NULL;
    }
    if (user->email) { 
        t_free(user->email);
        user->email = NULL;
    }
    
    if (user->warn) {			/* pending warnings? */
	w = user->warn->next;
	user->warn->next = NULL; 	/* break circular list */
	for (user->warn = w; user->warn; user->warn = w) {
	    w = user->warn->next;
	    t_free(user->warn);		/* free them all */
	}
    }
    
    free_recips(&user->torecips); 	/* clean up recip lists */
    free_recips(&user->ccrecips);
    free_recips(&user->bccrecips);
    
    finfoclose(&user->head);		/* and current message */
    finfoclose(&user->text);
    clean_encl_list(&user->ep);
	
    ml_clean(&user->ldat);		/* free mailing list names */

    if (user->replyto) {
	t_free(user->replyto);
	user->replyto = NULL;
    }
        
    if (user->popdeleted) {		/* free POP list of deleted msgs */
    	t_free(user->popdeleted);
	user->popdeleted = NULL;
    }
    
    pthread_mutex_lock(&global_lock);		/* unlink from user list */
    prev = NULL;
    for (u = u_head; u != user; u = u->next)
	prev = u;
		
    if (prev == NULL)			/* first? */
	u_head = user->next;
    else 
	prev->next = user->next;

    --u_num;				/* one less active user */
    pthread_mutex_unlock(&global_lock);
    
    t_free(user);			/* finally, free main user record */
    
    pthread_cond_signal(&usermax_wait);	/* might be safe to accept connections again */
    
}

/* 
    The "buf_" routines are used to buffer large amounts of data while
    the mailbox is locked (we don't want to actually try doing net io
    under those circumstances).


  buf_flush --

    Write buffered output (now that it's safe.)  Free the buffer(s).
*/

void buf_flush(mbox *mb) {

    bufl 	*p;
    int		i;

    if (!mb->user->conn.writing)	/* make sure any leftover input is flushed */
	t_fflush(&mb->user->conn);
    
    while(mb->obuf.first) {		/* for each buffer */
    	p = mb->obuf.first;
	mb->obuf.first = mb->obuf.first->next;
    	for (i = 0; i < p->used; i++)	/* write it */
	    t_putc(&mb->user->conn, p->data[i]);
	t_free((char *) p);
    	pthread_mutex_lock(&global_lock);
   	--malloc_stats.obufs;        /* stats: count allocated obufs */
    	pthread_mutex_unlock(&global_lock);

    }
    mb->obuf.last = NULL;		/* no buffers now */
    
}

/* buf_init --

    Set up mb->obuf to buffer some output.  Output will be buffered up until
    buf_flush is called.
*/


void buf_init(mbox *mb) {

    if (mb->obuf.first || mb->obuf.last)
	abortsig();			/* should not already be buffering */
	
    /* allocate & initialize a buffer */
    mb->obuf.last = (struct bufl *) mallocf(sizeof(struct bufl));
    mb->obuf.first = mb->obuf.last;
    mb->obuf.first->next = NULL;
    mb->obuf.first->used = 0;
    pthread_mutex_lock(&global_lock);
    ++malloc_stats.obufs;        /* stats: count allocated obufs */
    pthread_mutex_unlock(&global_lock);
}

/* buf_putc --

    Add a char to the mb->obuf buffer list.
    
*/

void buf_putc(mbox *mb, char c) {
	
    struct bufl		*new;
    
    if (mb->obuf.last->used == BIG_BUFLEN) { 	/* see if room */
    	new = (struct bufl *) mallocf(sizeof(struct bufl));
    	pthread_mutex_lock(&global_lock);
    	++malloc_stats.obufs;        /* stats: count allocated obufs */
	pthread_mutex_unlock(&global_lock);
	new->used = 0;
	new->next = NULL;
	mb->obuf.last->next = new;
	mb->obuf.last = new;
    }
    
    mb->obuf.last->data[mb->obuf.last->used++] = c;
}

/* buf_putl --

    Copy line, ending w/ CRLF.
*/

void buf_putl (mbox *mb, char *s) {

    while (*s)
	buf_putc(mb, *s++);
	
    buf_putc(mb, '\r'); buf_putc(mb, '\n');
}

/* buf_putsta --

    Begin status line with '0' or '1' (depending on whether warnings are pending),
    then add status.
*/

void buf_putsta(mbox *mb, char *s) {

    buf_putc(mb, mb->user->warn ? '1' : '0');
    
    while (*s)			/* copy status; no eol */
	buf_putc(mb, *s++);

}

/* print --

    Send status line, prefixed by the appropriate warning bit.
*/

void print(udb *user, char *s) {

    if (!user->conn.writing)	/* make sure any leftover input is flushed */
	t_fflush(&user->conn);
    t_putc(&user->conn, user->warn ? '1' : '0');
    
    t_fprintf(&user->conn, "%s\r\n", s);
}

/* print1 --

    Like print, but with an additional string arg.
*/

void print1(udb *user, char *s1, char *s2) {

    if (!user->conn.writing)	/* make sure any leftover input is flushed */
	t_fflush(&user->conn);

    t_putc(&user->conn, user->warn ? '1' : '0');
    t_fprintf(&user->conn, "%s%s\r\n", s1, s2);

}

/* popprint --

    Send status line to POP client (so no warning bit).
*/

void popprint(udb *user, char *s) {

    if (!user->conn.writing)	/* make sure any leftover input is flushed */
	t_fflush(&user->conn);
    
    t_fprintf(&user->conn, "%s\r\n", s);

    /* t_errprint_s("POP server said: %s", s); */
}

/* popprint1 --

    Like popprint, but with an additional string arg.
*/

void popprint1(udb *user, char *s1, char *s2) {

    if (!user->conn.writing)	/* make sure any leftover input is flushed */
	t_fflush(&user->conn);

    t_fprintf(&user->conn, "%s%s\r\n", s1, s2);

    /* {
    char buf[200];
    t_sprintf(buf, "%s%s", s1, s2);
    t_errprint_s("POP server said: %s", buf);
    } */
}

#ifdef KERBEROS

/* kpop_peek --

    Peek at first few bytes of input to see whether it's a Kerberos ticket
    or a POP command. If a command, read the rest of the line into
    user->comline. If a ticket, validate it. If an error is detected
    in the ticket, write an (unsolicited) POP error message to the
    connection; the client will hopefully associate that error with the
    next command (USER) it issues and realize there's a problem.
    
    Returns FALSE if no Kerberos ticket found, or if connection lost.
    
    * The format of the message from the client is:
    *
    * Size			Variable		Field
    * ----			--------		-----
    *
    * KRB_SENDAUTH_VLEN	KRB_SENDAUTH_VER	sendauth protocol
    * bytes					version number
    *
    * KRB_SENDAUTH_VLEN	version			application protocol
    * bytes					version number (ignore)
    *
    * 4 bytes		ticket->length		length of ticket
    *
    * ticket->length	ticket->dat		ticket itself

*/


boolean_t kpop_peek(udb *user) {

    KRB_INT32 	tkt_len;		/* length of ticket read from net */
    KTEXT	authent = NULL;		/* authentication info */
    AUTH_DAT 	*ad = NULL;		/* validated authentication data */
    char 	instance[INST_SZ];  	/* server instance from ticket */
    int		krb_stat;		/* status from kerberos library */
    char	krb_err[MAX_STR];	/* corresponnding error text */
    char	realm[REALM_SZ];	/* local realm */
    boolean_t	eol;			/* end-of-line seen? */

#define	KRB_SENDAUTH_VERS "AUTHV0.1" /* MUST be KRB_SENDAUTH_VLEN chars */


    /* read KRB_SENDAUTH_VLEN bytes (+1 for null) or 1 line */
    user->comp = t_gets_eol(user->comline, KRB_SENDAUTH_VLEN+1, 
				&user->conn, &eol);	
    if (!user->comp) {		/* read error */
	return FALSE;
    }
    
    /* See if it looks like a ticket */
    if (strncmp(user->comline, KRB_SENDAUTH_VERS, KRB_SENDAUTH_VLEN) != 0) {
    	/* no - read the rest of the line, if any */
        if (!eol)
	    user->comp = t_gets(user->comline+KRB_SENDAUTH_VLEN, 
			        MAX_STR-KRB_SENDAUTH_VLEN, &user->conn);
	if (user->comp) 	/* if no error */
	    user->comp = user->comline; /* point back at start of command */
	return FALSE;		/* no ticket */
    }
    
    if (strncmp(user->comline, KRB_SENDAUTH_VERS, KRB_SENDAUTH_VLEN) != 0) {
        popprint1(user, POP_KRBERR, "bad sendauth version");
	return TRUE;
    }
    
    /* read (and ignore) application version */
    if (!chars_get_buf(user, KRB_SENDAUTH_VLEN, user->comline)) {
	return FALSE;
    }
  
    /* read and verify ticket length */
    if (!chars_get_buf(user, sizeof(tkt_len), (char *) &tkt_len)) {
 	return FALSE;    
    }
    tkt_len = ntohl((unsigned KRB_INT32) tkt_len);
    if (tkt_len < 0 || tkt_len > MAX_KTXT_LEN) {
        popprint1(user, POP_KRBERR, "bad ticket length");
	return TRUE;
    }
    
    /* preliminaries finally done; allocate buffers for actual ticket */
    
    authent = mallocf(sizeof(KTEXT_ST)); /* auth info from client */
    ad = mallocf(sizeof(AUTH_DAT));	/* verified ticket info */

    authent->length = tkt_len;
    if (!chars_get_buf(user, authent->length, (char *) authent->dat)) { 
    	t_free(authent);		/* copy failed (conn lost?) */
	t_free(ad);
	return FALSE;
    }
    
    sem_seize(&krb_sem);		/* get access to kerberos library */
    
    /* decrypt & verify the ticket */
    strcpy(instance, "*");		/* wildcard instance */
    krb_stat = krb_rd_req(authent, "kpop", instance, 	/* decrypt... */
    	user->remoteaddr.sin_addr.s_addr, ad, ""); /* ...using default krb-srvtab */
    
    if (krb_stat != KSUCCESS) {
	strcpy(krb_err, krb_get_err_text(krb_stat));	/* must copy static text */
    }
    krb_get_lrealm(realm, 1);	/* get local realm (w/ sem seized) */
    sem_release(&krb_sem);	/* release sem before proceeding further */

    t_free(authent); 		/* done with the input ticket now */

    if (krb_stat != KSUCCESS) {	/* ticket rejected */
	popprint1(user, POP_KRBERR, krb_err); /* pass on the Kerberos error */
	t_free(ad);		/* clean up */
	return TRUE;    
    }
    
    /* ticket decrypted ok; do some consistency checks */
    
    if (strlen(ad->pinst) != 0 ||	/* must be null instance */
    	strcmp(ad->prealm, realm) != 0) { /* in our realm */
    	popprint1(user, POP_KRBERR, "wrong client instance/realm name in ticket");
	t_free(ad);		/* clean up */
	return TRUE;    
    }
   
    /* Everything looks ok. Set user->krbvalidated; stash ticket name in user->name */
    
    user->name = mallocf(1+strlen(ad->pname));
    strcpy(user->name, ad->pname);	/* save name here for later use */
    t_free(ad);			/* done with info from ticket */
    user->krbvalidated = TRUE;	/* can shortcut rest of signon sequence */

    return TRUE; 		/* ticket seen; not ordinary command */   

}
#endif

/* parse_command --

    Parse off client command, search for it in command table.
    
    If command is invalid, send error to client and return -1.
    
    In all the parsing routines here, user->comp is the current
    position in the command line.
*/

int parse_command(udb *user) {

    char	command[5];	/* command from the client */
    int		max, min, mid;	/* for binary search */
    int		cmp;		/* strcmp results */

    if (strlen(user->comline) < 4) {
	print1(user, BLITZ_UNKNOWN, user->comline);
	return -1;
    }
    
    strncpy(command, user->comline, 4);
    command[4] = 0;		/* pick up the command */
    
    user->comp = user->comline + 4; /* point beyond it */
    if (*user->comp == ' ')	/* skip ONE space if present */
	++user->comp;
    
    /* binary search the command table */
    for (max = CMD_COUNT-1, min = 0; max >= min; ) {
	mid = (max + min) >> 1;
	cmp = strcasecmp(command, cmdtab[mid].name);
	if (cmp == 0)	/* match? */
	    return mid;	/* yep */
	else if (cmp > 0)
	    min = mid + 1;
	else 
	    max = mid - 1;						
    }
    
    print1(user, BLITZ_UNKNOWN, command);
    
    return -1;
    
}

/* popparse_command --

    Parse off POP client command, search for it in command table.
    
    If command is invalid, send error to client and return -1.
    
    In all the parsing routines here, user->comp is the current
    position in the command line.
*/

int popparse_command(udb *user) {

    char	command[5];	/* command from the client */
    char	*p;		/* for copying command */
    int		max, min, mid;	/* for binary search */
    int		cmp;		/* strcmp results */

   /*  t_errprint_s("POP client said: %s", user->comline); */

    user->comp = user->comline;	/* pick up the command */
    p = command;
    while (isalnum(*user->comp)) {
    	*p++ = *user->comp++;
	if (p - command >= 4) break;	/* max command length == 4 */
    }
    *p = 0;

    if (*user->comp == ' ')	/* skip ONE space if present */
	++user->comp;
    
    /* binary search the command table */
    for (max = POPCMD_COUNT-1, min = 0; max >= min; ) {
	mid = (max + min) >> 1;
	cmp = strcasecmp(command, popcmdtab[mid].name);
	if (cmp == 0)	/* match? */
	    return mid;	/* yep */
	else if (cmp > 0)
	    min = mid + 1;
	else 
	    max = mid - 1;						
    }
    
    popprint1(user, POP_UNKNOWN, command);
    
    return -1;
    
}

/* parse_arg --

    Pick up next word from command line, handling quoting.
*/

void parse_arg(udb *user, char *arg) {

    while (isspace(*user->comp))
	++user->comp;			/* skip leading spaces */
	
    if (*user->comp == '"') 		/* quoted string? */
	user->comp = strqcpy(arg, user->comp); /* copy it */
    else {				/* else word is space-delimited */
	while (*user->comp != ' ' && *user->comp)
	    *arg++ = *user->comp++;
	*arg++ = 0;
    }
}

/* parse_num --

    Next thing on command line should be a number; parse it off.
    If there isn't a number here, send error to client.
*/

boolean_t parse_num(udb *user, long *num) {

    while (isspace(*user->comp))
	++user->comp;			/* skip leading spaces */

    if (*user->comp == '-' || isdigit(*user->comp)) {
	user->comp = strtonum(user->comp, num);
	return TRUE;
    } else {				/* no number */
	if (*user->comp)		/* anything at all? */
	    print(user, BLITZ_BADARG);
	else
	    print(user, BLITZ_MISSINGARG);
	return FALSE;
    }
}

/* parse_uns --

    Next thing on command line should be unsigned number; parse it off.
    If there isn't a number here, send error to client.
*/

boolean_t parse_uns(udb *user, u_long *num) {

    while (isspace(*user->comp))
	++user->comp;			/* skip leading spaces */

    if (isdigit(*user->comp)) {
	user->comp = strtouns(user->comp, num);
	return TRUE;
    } else {				/* no number */
	if (*user->comp)		/* anything at all? */
	    print(user, BLITZ_BADARG);
	else
	    print(user, BLITZ_MISSINGARG);
	return FALSE;
    }
}

/* popparse_num --

    Next thing on command line should be a number; parse it off.
    If there isn't a number here, send error to POP client.
*/

boolean_t popparse_num(udb *user, long *num) {

    while (isspace(*user->comp))
	++user->comp;			/* skip leading spaces */

    if (*user->comp == '-' || isdigit(*user->comp)) {
	user->comp = strtonum(user->comp, num);
	return TRUE;
    } else {				/* no number */
	if (*user->comp)		/* anything at all? */
	    popprint(user, POP_BADARG);
	else
	    popprint(user, POP_MISSINGARG);
	return FALSE;
    }
}

/* popparse_uns --

    Next thing on command line should be unsigned number; parse it off.
    If there isn't a number here, send error to POP client.
*/

boolean_t popparse_uns(udb *user, u_long *num) {

    while (isspace(*user->comp))
	++user->comp;			/* skip leading spaces */

    if (isdigit(*user->comp)) {
	user->comp = strtouns(user->comp, num);
	return TRUE;
    } else {				/* no number */
	if (*user->comp)		/* anything at all? */
	    popprint(user, POP_BADARG);
	else
	    popprint(user, POP_MISSINGARG);
	return FALSE;
    }
}

/* parse_messid --

    Parse [foldnum/]messid.  Make sure folder number is valid; set it
    to -1 if unspecified.
*/

boolean_t parse_messid(udb *user, long *messid, long *foldnum) {

    if (!parse_num(user, messid))	/* get message id */
	return FALSE;
    if (*user->comp == '/') {	/* wait -- was that really a folder # */
	++user->comp;
	*foldnum = *messid; /* yes */
	if (!foldnum_valid(user->mb, *foldnum)) {
	    print1(user, BLITZ_BADARG_BLANK, "Invalid folder #");
	    return FALSE;
	}
	if (!parse_num(user, messid))  /* really parse message id  */
	    return FALSE; 
    } else
	*foldnum = -1;	/* folder not specified */

    return TRUE;
}
/* parse_messlist --

    Parse a comma-separated list of message identifiers -- [foldnum/]messid.
    A message-list is always the last element of the command line.
*/

messlist *parse_messlist(udb *user){

    messlist	*nums;
    char	*p;
    int		count;
    int		i;
    
    /* first, count number of args to compute length needed */
    count = 1;
    for (p = index(user->comp, ','); p; p = index(p+1, ','))
	++count;
    nums = (messlist *) mallocf(sizeof(messlist) + (2*count) * sizeof(long));
    nums->foldnum = &nums->data[0];	/* set up pointers to variable part */
    nums->messid = &nums->data[count];
    
    for (i = 0; i < count; ++i) { /* pick each up */
	if (!parse_messid(user, &nums->messid[i], &nums->foldnum[i]))
	    goto cleanup;

	if (i < count - 1) {		/* if more to come */
	    ++user->comp;		/* skip the comma */	
	}	    
    }
    
    nums->count = count;
    
    return nums;			/* parsed ok */
    
cleanup:
    t_free(nums);			/* no good; clean up */
    return NULL;		

}

/*^L parse_rangelist --

    Parse a comma-separated list of ranges -- low-high (non-negative).
    A range-list is always the last element of the command line.
*/

rangelist *parse_rangelist(udb *user){

    rangelist    *nums;
    char        *p;
    int         count;
    int         i;

    /* first, count number of args to compute length needed */
    count = 1;
    for (p = index(user->comp, ','); p; p = index(p+1, ','))
        ++count;
    nums = (rangelist *) mallocf(sizeof(rangelist) + (2*count) * sizeof(long));
    nums->low = &nums->data[0];     /* set up pointers to variable part */
    nums->high = &nums->data[count];

    for (i = 0; i < count; ++i) { /* pick each up */
        if (!parse_num(user, &nums->low[i]))
            goto cleanup;
        if (*user->comp++ != '-') {
	     print(user, BLITZ_BADARG);
	     goto cleanup;
	}
        if (!parse_num(user, &nums->high[i]))
            goto cleanup;

	/* sanity check on range value */
        if (nums->low[i] > nums->high[i] || nums->low[0] < 0) {
             print(user, BLITZ_BADARG);
             goto cleanup;
        }
        if (i < count - 1) {            /* if more to come */
            ++user->comp;               /* skip the comma */
        }
    }

    nums->count = count;

    return nums;                        /* parsed ok */

cleanup:
    t_free(nums);                       /* no good; clean up */
    return NULL;

}
/* chars_get --

    Copy given number of chars from network to fileinfo.
*/

boolean_t chars_get(udb *user, long wantlen, fileinfo *out) {

    char 	buf[8192];		/* use a healthy-size buffer */
    int		totallen = 0;		/* total len read so far */
    int		len;			/* length of this chunk */
    t_file 	*outf = NULL;		/* output file */
    boolean_t	ok = TRUE;
    
    if ((outf = t_fopen(out->fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("chars_get: cannot open ", out->fname);
	return FALSE;
    }
    
    t_fseek(&user->conn, 0, SEEK_CUR);	/* set up to read */
    
    while(totallen < wantlen) {
	len = sizeof(buf);		/* read up to a buffer full */
	if (len > wantlen - totallen)	/* but no more than advertised */
	    len = wantlen - totallen;
    
	len = t_fread(&user->conn, buf, len);
	
	totallen += len;
	if (user->conn.urgent || user->shutdown || (len <= 0 && totallen < wantlen)) {
	    ok = FALSE;
	    break;			/* don't spin */
	}
	
	/* write that to output file (raw write; avoid extra copy) */
	if (write(outf->fd, buf, len) < 0) {
	    ok = FALSE;
	    t_perror("chars_get: write");
	}
    }
        
    out->offset = 0;
    out->len = t_fseek(outf, 0, SEEK_END);	/* compute lof */
    
    t_fclose(outf);			/* done with output file */
    
    return ok;
}
/* chars_get_buf --

    Copy given number of chars from network to buffer in memory.
    Note: file should already be in reading state.
*/

boolean_t chars_get_buf(udb *user, long wantlen, char *buf) {

    int		totallen = 0;		/* total len read so far */
    int		len;			/* length of this chunk */
    boolean_t	ok = TRUE;
    
        
    while(totallen < wantlen) {
	len = wantlen - totallen;	/* fill entire buffer */
    
	len = t_fread(&user->conn, buf + totallen, len);
	
	totallen += len;
	if (user->conn.urgent || user->shutdown || (len <= 0 && totallen < wantlen)) {
	    ok = FALSE;
	    break;			/* don't spin */
	}
    }
            
    return ok;
}

/* check_mess_space --

    Before accepting an upload, check available space on the temp disk;
    also check message size against configured limit.
    
    Prints error & returns FALSE if the upload is too large.
    
*/

boolean_t check_mess_space(udb *user, long l) {

    long		messlen; /* total length of pending message */
    enclinfo		*ep;

    if (!check_temp_space(l)) {
	print1(user, BLITZ_ERR_BLANK, "Disk full!");
	return FALSE;
    }
    
    if (mess_max_len != 0) { /* enforce mess length limit */
    	messlen = user->text.len;
    	for (ep = user->ep; ep; ep = ep->next) {
	    messlen += ep->finfo.len;
    	}
	if (messlen + l > mess_max_len) {
	    print1(user, BLITZ_ERR_BLANK, "Message too long!");
	    return FALSE;
        }
    }

    return TRUE;
}

/* add_warn --

    Queue up a warning.
    
    --> box locked <--
*/

void add_warn(udb *user, char *text) {

    warning	*warn;			/* new warning */
 					   
    warn = mallocf(sizeof(warning));
    strcpy(warn->text, text);
    if (!user->warn) 			/* first one? */
	warn->next = warn;
    else {				/* append to circular list */
	warn->next = user->warn->next;
	user->warn->next = warn;
    }
    user->warn = warn;			/* new tail */
}
    
/* newmail_warn --

    Set PREF_NEWMAIL to indicate that new mail has arrived.  If user
    is signed on, queue up a warning.
*/

void newmail_warn(mbox *mb, long messid, int foldnum) {

    char	buf[MAX_STR];		/* warning info */
    
    sem_seize(&mb->mbsem);		/* sync with WARN etc. */
    if (mb->user) {			/* if user is active... */
	add_warn(mb->user, BLITZ_NEWMAIL);/* generic new mail warning */
	t_sprintf(buf, "%s%ld %d 0", BLITZ_INSERT, messid, foldnum);
	add_warn(mb->user, buf);	/* plus insertion warning with messid */
    }
    
    pref_set_int(mb, PREF_NEWMAIL, "\"1\""); /* set newmail flag */
    sem_release(&mb->mbsem);
}

/* newmail_check --

    At signon time, check PREF_NEWMAIL to see if new mail is waiting;
    queue up a warning if so.
*/

void check_newmail(udb *user) {

    char	zot[MAX_STR];
    
    sem_seize(&user->mb->mbsem);	/* sync with newmail_warn */
    if (pref_get_int(user->mb, PREF_NEWMAIL, zot))
	add_warn(user, BLITZ_NEWMAIL);
    sem_release(&user->mb->mbsem);
    
}

/* get_warning --

    At signon time, send a global warning if there's one active.

*/

void get_warning(udb *user) {

    char	buf[MAX_STR];
    char	warn[2*MAX_STR];
    t_file	*f;
       
    /* re-read file at each signon; it may have changed */
    if (f_warning && (f = t_fopen(f_warning, O_RDONLY, 0))) {
	u_warning = t_gets(buf, sizeof(buf), f);
	if (u_warning && *u_warning) {		/* warning text set? */
	    sem_seize(&user->mb->mbsem);	
	    t_sprintf(warn, "%s%s", BLITZ_USERMSG, u_warning);
	    add_warn(user, warn);
	    sem_release(&user->mb->mbsem);
	}
	t_fclose(f);
    }
        
}

/* get_vwarn --

    If client software version is out-of-date, send them a warning.

*/

void get_vwarn(udb *user) {

    return;		/* not implemented */
}

/* dndexp_check --

    Give warning to users whose DND entry is about to expire.

*/

void dndexp_check(udb *user, char *expstr) {

    char	warn[MAX_STR];
    u_long	exp;			/* expiration date (mactime) */
    
    if (!expstr || strlen(expstr) == 0)	/* account not set to expire */
	return; 
 
    if (!parse_expdate(expstr, &exp))
	return;				/* EXPIRES field not in format we expect */
    
    if (exp <= add_days(dndexp_warn)) {
	sem_seize(&user->mb->mbsem);	
	t_sprintf(warn, "%sThis account is scheduled to be deactivated on %s. Contact the Postmaster for more information.",
		 BLITZ_USERMSG, expstr);
	add_warn(user, warn);
	sem_release(&user->mb->mbsem);
    }

}
/* get_pigwarn --

    Give warning to users with too much mail.
    
    Note that the total box length is kept in mb->boxlen. It is computed (by adding up the values 
    from all summaries) by summ_check, and updated by all commands that add or delete messages.  This
    way we don't have to read in all the summaries at every signon.

*/

void get_pigwarn(udb *user) {

    char	warn[MAX_STR];
    
    if (boxpig == 0)		/* no limit set */
	return; 
 
    if (user->mb->boxlen / 1024 > boxpig) {
	sem_seize(&user->mb->mbsem);	
	t_sprintf(warn, "%sYou have more than %ldk of mail in total!  Please discard some of it.",
		 BLITZ_USERMSG, boxpig);
	add_warn(user, warn);
	sem_release(&user->mb->mbsem);
    }


}
/* mime_catalog --

   Generate a catalog of the MIME structure of this message, showing content
   type (etc.) information for each part (if a multi-part message).
*/

boolean_t mime_catalog(udb *user) {

    t_file	*f;			/* open header stream */
    char 	*buf;			/* next header line */
    long	remaining;		/* header left to read */
    boolean_t	ismulti = FALSE;	/* multipart message? */
    char	boundary[MAX_BOUNDARY_LEN+1];/* multipart boundary */
    
    if ((f = t_fopen(user->head.fname, O_RDONLY, 0)) == NULL) {
	t_perror1("mime_catalog: cannot open ", user->head.fname);
	return FALSE;
    }
    
    (void) t_fseek(f, user->head.offset, SEEK_SET); /* seek to start of header */
    
    /* read header, but don't unfold lines */
    remaining = user->head.len;
    while ((buf = getheadline(f, &remaining, FALSE)) != NULL) {
	
	/* don't let hacker confuse client with bogus bounds: line */
	if (strncasecmp(buf, BOUNDS_HDR, strlen(BOUNDS_HDR)) != 0) {
	    putheadline(user, 1, buf);	/* copy entire header, at level 1 */
	}

	if (strncasecmp(buf, "Content-Type", strlen("Content-Type")) == 0) {
	    ismulti = parse_content_type(buf, boundary);
	}
	t_free(buf);
    }

    t_fclose(f);			/* done with header file */
    
    /* top-level header copied; if multi-part, need to look at text too */   
    if (ismulti) {
	if ((f = t_fopen(user->text.fname, O_RDONLY, 0)) == NULL) {
	    t_perror1("mime_catalog: cannot open ", user->text.fname);
	    return FALSE;
	}
	/* parse text to look at multipart headers */
	remaining = user->text.len;
	catalog_multi(user, f, user->text.offset, 0, &remaining, boundary, 2); 
	t_fclose(f);
    }

    /* finally, add top-level bounds line */
    t_putc(&user->conn, user->warn ? '1' : '0');
    t_fprintf(&user->conn, "%s1 %s: %d %d\r\n", 
    		BLITZ_LASTLINE, BOUNDS_HDR, 0, user->text.len);
    
        
    return TRUE;
}

/* Generate MIME catalog lines for a body part of type multipart.
   Recurse for any nested parts of type multipart.
   
   Note that we presently treat message/rfc822 as an atomic type, instead
   of recursively parsing the structure of the message.
*/
void catalog_multi(udb *user, t_file *f, long base, long offset, long *len, 
		   char *boundary, int level) {

    char 	*buf = NULL;		/* next header line */
    long	inlen = *len;		/* initial length */
    long	loc;			/* current location in file */
    long	startsect = 0;		/* where this section started */
    enum	{ S_PROLOG, S_HEAD, S_BODY, S_END } 
    		state; 			/* parse state */
    boolean_t	ismulti = FALSE;	/* multipart content-type? */
    char	sub_boundary[MAX_BOUNDARY_LEN+1]; /* multipart boundary */
    
    (void) t_fseek(f, base+offset, SEEK_SET); /* seek to start of body */
    
    state = S_PROLOG;			/* in prolog until first boundary */
    
    /* read header, but don't unfold lines */
    while ((buf = getheadline(f, len, FALSE)) != NULL) {
    
	/* is it a boundary? */
	if (strncmp(buf, "--", 2) == 0
	 && strncmp(buf+2, boundary, strlen(boundary)) == 0) {
	    if (state == S_BODY || state == S_HEAD) { /* finish previous section */
	    	loc = offset + (inlen - *len); /* current byte offset */
		loc -= (1 + strlen(buf) + 1); /* omit boundary and \r before */
		t_putc(&user->conn, user->warn ? '1' : '0');
		/* note: make len non-negative even in ill-formed message that
			lacks a blank line after the header */
		t_fprintf(&user->conn, "%s%d %s: %d %d\r\n",   
				BLITZ_MOREDATA, level, BOUNDS_HDR,
				startsect, /* offset... */
			 (loc - startsect > 0 ? loc - startsect : 0)); /* ...and len */
	    }
	    
	    /* beginning or end boundary? */
	    if (strlen(buf) == strlen(boundary) + 4
	     && strcmp(buf + strlen(buf)-2, "--") == 0) {
	     	t_free(buf);		/* end boundary -- clean up */
		state = S_END;
		break;			/* and done */
	     } else {
	     	state = S_HEAD;		/* start boundary -- expect header next */
		ismulti = FALSE;	/* not multipart (so far) */
		startsect = offset + (inlen - *len); /* record where section began */
	     }
	 } else if (state == S_HEAD) {	/* not boundary; in header? */
	    if (strlen(buf) == 0) {
		state = S_BODY;	/* blank line ends header */
	    	if (ismulti) {	/* is current type multipart? */
		    /* yes - compute where we are in the file */
		    loc = offset + (inlen - *len);
		    if (level < MIME_MAX_NEST) /* recurse for nested multipart body */
			catalog_multi(user, f, base, loc, len, sub_boundary, level+1);
		    
		    /* return w/ subpart finished, ready to read next boundary */
		}
	    } else {
		/* add header line to catalog (catch bogus bounds: line!) */
		if (strncasecmp(buf, BOUNDS_HDR, strlen(BOUNDS_HDR)) != 0) {
		    putheadline(user, level, buf);
		}
		if (strncasecmp(buf, "Content-Type", strlen("Content-Type")) == 0) {
		    ismulti = parse_content_type(buf, sub_boundary);
		}
	    }
	 }
	 t_free(buf); buf = NULL;
    }  

    /* Deal with missing end boundary (message truncated?) */
    if (state == S_BODY || state == S_HEAD) {
	loc = offset + (inlen - *len); /* current byte offset */
	t_putc(&user->conn, user->warn ? '1' : '0');
	t_fprintf(&user->conn, "%s%d %s: %d %d\r\n",
			BLITZ_MOREDATA, level, BOUNDS_HDR,
			startsect, loc - startsect); /* catalog offset/length */
    }
}

/* parse_content_type --

   Parse a MIME Content-Type: header line. Returns TRUE if the type is
   "multipart"; also sets "boundary" in that case.
*/

boolean_t parse_content_type(char *buf, char *boundary) {

    char	*p;
    char	*q;
    
    p = buf + strlen("Content-Type:");
    while (isspace(*p))		/* locate the type */
	++p;
	    
    if (strncasecmp(p, "multipart/", strlen("multipart/")) != 0)
	return FALSE;		/* not multipart */

    for (;;) {			/* parse all parameters */
	q = index(p, ';');
	if (q == NULL) 
	    return FALSE;		/* no boundary parameter? */
	p = q + 1;			/* skip the ';' */
	while (isspace(*p))
	    ++p;
   
	if (strncasecmp(p, "boundary", strlen("boundary")) == 0)
	    break;		/* boundary parameter located */
    }
    
    p += strlen("boundary");
    
    while (isspace(*p))
	++p;
    if (*p++ != '=')
	return FALSE;
    while (isspace(*p))
	++p;

    q = mallocf(strlen(p)+1);	/* temp buffer for unquote */
    (void) unquote(q, p);	/* copy & unquote boundary string */
    if (strlen(q) > MAX_BOUNDARY_LEN) {
        t_free(q);
	return FALSE;		/* illegally long boundary */
    }

    strcpy(boundary, q);	/* return boundary string */
    t_free(q);
    
    return TRUE;		/* valid multipart header */
}

/* putheadline --

   Copy header line to client w/ MIME nesting level. Need to watch for
   folded header lines, and prefix the continuation with new status/nesting level.
   
   If this is a Blitz-generated message (whether MESSTYPE_BLITZ or MESSTYPE_RFC822),
   it may have very long (not folded) address lines; if so, fold it.
*/

void putheadline(udb *user, int level, char *buf) {

    char	*p;
    boolean_t	recip_field = FALSE;
    
    /* Check what kind of field this is */
    if (strncasecmp(buf, "TO:", strlen("TO:")) == 0
	|| strncasecmp(buf, "CC:", strlen("CC:")) == 0
	|| strncasecmp(buf, "BCC:", strlen("BCC:")) == 0) {
	recip_field = TRUE;
    }
    
    /* message w/ long To: line? */
    if (recip_field && strlen(buf) > FOLD_MARGIN) {
	foldheadline(user, level, buf);		/* yes - must fold */
	return;
    }

    /* BLITZ_MOREDATA <level> <text> */
    t_fprintf(&user->conn, "%c%s%d ", user->warn ? '1' : '0', BLITZ_MOREDATA, level);
    for (p = buf; *p; ++p) {
	t_putc(&user->conn, *p);
	if (*p == '\r')	{	/* new line? */
	    t_putc(&user->conn, '\n');
	    t_fprintf(&user->conn, "%c%s%d ", user->warn ? '1' : '0', BLITZ_MOREDATA, level);
	}
    }
    t_fprintf(&user->conn, "\r\n");
}

/* foldheadline --

   Copy long header line to client w/ MIME nesting level, folding it. 
   
   The header line is parsed as an address-list, so line breaks can be placed between
   addresses.
   
*/

void foldheadline(udb *user, int level, char *buf) {

    char	*p;
    char 	c;			/* current char of address */
    int		comment;		/* address parsing state: */
    boolean_t	quot;			/* '' */
    boolean_t	route; 			/* '' */
    boolean_t	esc;			/* '' */
    int		linepos;		/* output column position */
    char	addr[MAX_ADDR_LEN];	/* current address */
    char	*addrp;

    /* BLITZ_MOREDATA <level> <text> */
    t_fprintf(&user->conn, "%c%s%d ", user->warn ? '1' : '0', BLITZ_MOREDATA, level);
        
    p = index(buf, ':');		/* copy the field name */
    *p = 0;
    t_fprintf(&user->conn, "%s: ", buf);
    *p++ = ':';			/* restore the ":" we nailed */
    linepos = (p - buf) + 2;	/* column position in output msg */

	    
    /* now parse & copy addresses */
    comment = 0;
    quot = route = esc = FALSE;
    addrp = addr;
    if (*p == ' ')		/* skip ": " */
	++p;		
		
    while ((c = getaddrc(&p, &comment, &quot, &route, &esc)) != 0) {
		
	if (c == '\r')		/* strip out any existing folding */
	   continue;

	/* are we at the end of an address? */
	if (c == ',' && comment == 0 && !quot && !route) {
	    *addrp = 0;
	    /* if line is full, break before putting this addr */
	    if (linepos + (addrp-addr) + 2 > FOLD_MARGIN) {
		t_fprintf(&user->conn, "\r\n%c%s%d  ", user->warn ? '1' : '0', BLITZ_MOREDATA, level);
		linepos = 1;			/* ident w/ extra space */
	    }
	    t_fprintf(&user->conn, "%s,", addr);	/* copy address & comma */
	    linepos += 1+strlen(addr);		/* update column position */
	    addrp = addr;			/* reset for next addr */
	} else {				/* in middle of address */
	    /* if address is obscenely long, break it now */
	    if ((addrp-addr) + 1 >= MAX_ADDR_LEN-8) { /* keep line < MAX_ADDR_LEN */
		*addrp = 0;
		/* line break before & after enormo address */
		t_fprintf(&user->conn, "\r\n%c%s%d  ", user->warn ? '1' : '0', BLITZ_MOREDATA, level);
		t_fprintf(&user->conn, "%s", addr);	/* copy partial address  */
                t_fprintf(&user->conn, "\r\n%c%s%d  ", user->warn ? '1' : '0', BLITZ_MOREDATA, level);
		addrp = addr;			/* reset for next one */
                linepos = 1;                    /* starting new indented line */
	    }
	    *addrp++ = c;			/* another char for this addr */
	}
    }
    
    *addrp++ = 0;
    t_fprintf(&user->conn, "%s\r\n", addr);	/* copy last address  */

}

/* ##################### Client Commands ############################## */
/*  c_audt --

    Add an additional audit folder for current message.
*/

void c_audt(udb *user) {

    long	foldnum;	/* audit folder */
    
    if (!parse_num(user, &foldnum))	/* parse the folder # */
	return;
    if (!foldnum_valid(user->mb, foldnum)) {
	print(user, BLITZ_BADARG);	/* invalid folder # */
    } else {
	if (foldnum != AUDIT_NUM)	/* don't put 2 copies in audit folder */
	    user->xtra_audit = foldnum;
    	print(user, BLITZ_OK);
    } 
}

/* c_clea --

    Clear current message & current recipient lists.
*/

void c_clea(udb *user) {

    free_recips(&user->torecips); /* clean up recip lists */
    free_recips(&user->ccrecips);
    free_recips(&user->bccrecips);
    user->recipcount = 0;	/* reset total */
   
    ml_clean(&user->ldat);	/* free mailing list names */
    
    if (user->replyto) {	/* free mailing list names */
	t_free(user->replyto);
	user->replyto = NULL;
    }
    user->wantreceipt = FALSE;	/* clear receipt request */
    user->hiderecips = FALSE;	/* clear "hide recipient list" option */
    user->hextext = FALSE;	/* and text binhexing */
    user->xtra_audit = -1;	/* and extra audit folder */
    
    do_clem(user);		/* clean up current message */
    
    print(user, BLITZ_OK);

}

/* c_clem --

    Clear current message.
*/

void c_clem(udb *user) {

    do_clem(user);
    print(user, BLITZ_OK);
    
}
void do_clem(udb *user) {

    finfoclose(&user->head);	/* clean up current message */
    finfoclose(&user->text);
    clean_encl_list(&user->ep);
    user->summ.messid = -1;	/* summary info not valid */
    user->summ.enclosures = 0;
    user->summ.topic[0] = 0;
    user->currmessfold = -1;
    user->summ.type = MESSTYPE_BLITZ;	/* revert to default type */
}       


/* c_cler --

    Clear recipient list.
*/

void c_cler(udb *user) {
    
    free_recips(&user->torecips); /* clean up recip lists */
    free_recips(&user->ccrecips);
    free_recips(&user->bccrecips);
    user->recipcount = 0;	/* reset total */
   
    ml_clean(&user->ldat);	/* free mailing list names */
    
    if (user->replyto) {	/* free reply address */
	t_free(user->replyto);
	user->replyto = NULL;
    }
    user->wantreceipt = FALSE;	/* clear receipt request */
    user->hiderecips = FALSE;   /* clear "hide recipient list" option */
    user->hextext = FALSE;	/* and text binhexing */
    user->xtra_audit = -1;	/* and extra audit folder */

    print(user, BLITZ_OK);

}
/* c_copy --

    Copy a list of messages, setting a new expiration date if the destination folder
    has a default expiration pref set.
    
    COPY <from> <to> <messid>[,<messid>...]
*/

void c_copy(udb *user) {

    messlist	*nums;		/* list of ids */
    char	resp[32];
    long	from, to;	/* source & destination folder #s */
    folder	*fromfold;	/* source folder */
    u_long	newexp;		/* new expiration date */
    int		i;
    
    /* get source & destination folder numbers */
    if (!parse_num(user, &from) || !parse_num(user, &to))
	return;
    if (!foldnum_valid(user->mb, from) || !foldnum_valid(user->mb, to)) {
	print(user, BLITZ_BADARG);
	return;
    }
    
    /* check destination folder autoexpire */
    newexp = fold_autoexp(user->mb, to);
    if (newexp == -1)			/* will expiration change? */
	t_sprintf(resp, "%s%ld", BLITZ_MOREDATA, newexp); /* no */
    else
    	t_sprintf(resp, "%s%lu", BLITZ_MOREDATA, newexp);
	
    print(user, resp);			/* tell client what expiration will be */
    
    if ((nums = parse_messlist(user)) == NULL)
	return;				/* syntax error */
	
    for (i = 0; i < nums->count; ++i) {
	if (nums->foldnum[i] == -1)	/* source folder specified for this mess? */
	    fromfold = &user->mb->fold[from]; 
	else
	    fromfold = &user->mb->fold[nums->foldnum[i]];
	if (summ_copymess(user->mb, fromfold, 
	                        &user->mb->fold[to], &nums->messid[i], newexp)) {
	    t_sprintf(resp, "%ld %ld %ld", nums->messid[i], to, user->mb->fold[to].count);
	    if (i == nums->count - 1)
		print1(user, BLITZ_LASTLINE, resp);
	    else
		print1(user, BLITZ_MOREDATA, resp);
	} else 
	    print(user, BLITZ_BADARG);	/* no such messid? */
    }
    
    t_free(nums);
	
}

/* c_dele --

    Delete a list of messages. The "insertion position" in the response
    status is no longer really necessary (since the server is no longer
    in charge of folder sorting), it's simply set to zero here.
    
*/

void c_dele(udb *user) {

    messlist	*nums;		/* list of ids */
    char	resp[32];
    int		i;
    
    if ((nums = parse_messlist(user)) == NULL)
	return;			/* syntax error */
	
    for (i = 0; i < nums->count; ++i) {
	if (summ_move(user->mb, &user->mb->fold[INBOX_NUM], 
			        &user->mb->fold[TRASH_NUM], nums->messid[i], -1)) {
	    t_sprintf(resp, "%ld %d %ld", nums->messid[i], TRASH_NUM, user->mb->fold[TRASH_NUM].count);
	    if (i == nums->count - 1)
		print1(user, BLITZ_LASTLINE, resp);
	    else
		print1(user, BLITZ_MOREDATA, resp);
	    if (user->summ.messid == nums->messid[i]) /* moved current message? */
	    	user->currmessfold = TRASH_NUM;	/* yes - update folder # */
	} else 
	    print(user, BLITZ_BADARG);	/* no such messid? */
    }	
    
    t_free(nums);
}

/* c_delx --

    Delete a list of messages, setting a new expiration date.
    
*/

void c_delx(udb *user) {

    messlist	*nums;		/* list of ids */
    char	resp[32];
    u_long	newexp;		/* new expiration date */
    int		i;
    
    if (!parse_uns(user, &newexp))
	return;
	
    /* months or days? */
    if (*user->comp == 'm' || *user->comp == 'M') {
	newexp = add_months(newexp);
	++user->comp;
    } else
	newexp = add_days(newexp);
	
    t_sprintf(resp, "%s%lu", BLITZ_MOREDATA, newexp);
    print(user, resp);		/* tell client what expiration will be */
    
    if ((nums = parse_messlist(user)) == NULL)
	return;			/* syntax error */
	
    for (i = 0; i < nums->count; ++i) {
	if (summ_move(user->mb, &user->mb->fold[INBOX_NUM], 
	                        &user->mb->fold[TRASH_NUM], nums->messid[i], newexp)) {
	    t_sprintf(resp, "%ld %d %d", nums->messid[i], TRASH_NUM, user->mb->fold[TRASH_NUM].count);
	    if (i == nums->count - 1)
		print1(user, BLITZ_LASTLINE, resp);
	    else
		print1(user, BLITZ_MOREDATA, resp);
	    if (user->summ.messid == nums->messid[i]) /* moved current message? */
	    	user->currmessfold = TRASH_NUM;	/* yes - update folder # */
	} else 
	    print(user, BLITZ_BADARG);	/* no such messid? */
    }
    
    t_free(nums);
	
}

/* c_edat --

    Upload an enclosure.
    
*/

void c_edat(udb *user) {

    long	l;			/* message length */
    char	resp[32];		/* intermediate response */
    enclinfo	*new;			/* new enclosure node */
    enclinfo	*tail;			/* tail of old list */
	
    if (!parse_num(user, &l))		/* get length */
	return;	

    if (l < 1) {			/* bad length? */
	print(user, BLITZ_BADARG);
	return;
    }
    
    /* get a new enclosure node */
    new = (enclinfo *) mallocf(sizeof(enclinfo));
    
    parse_arg(user, new->type);		/* 2nd arg -- file type */
    
    /* important to preserve leading spaces in name */
    if (!*user->comp)
	new->name[0] = 0;
    else 				/* skip exactly 1 space */
	strcpy(new->name, user->comp + 1);
    
    if (!new->name[0] || !new->type[0]) {
	print(user, BLITZ_MISSINGARG);
	t_free(new);
	return;
    }
    
    /* check lengths to make sure they'll fit in enclhead struct
       (note that the fields in enclinfo are MAX_STR, so no danger
       of overflowing them) */
    new->name[ENCLSTR_LEN-1] = 0;	/* silently truncate long name */
    
    if (strlen(new->type) >= ENCLSTR_LEN) { /* quoted type is hard to truncate */
 	print(user, BLITZ_BADARG);	/* so return an error */
	t_free(new);
	return;
    }
    
    temp_finfo(&new->finfo);		/* get temp file */
    if (!check_mess_space(user, l))	/* make sure there will be room */
	return;
    
    t_sprintf(resp, "%ld", l);		/* tell client to fire away */
    print1(user, BLITZ_INTERMEDIATE, resp);
    t_fflush(&user->conn);	

    if (!chars_get(user, l, &new->finfo)) {	/* read specified # of chars */
	finfoclose(&new->finfo);	/* copy failed (conn lost?) */
	t_free(new);
	print1(user, BLITZ_ERR_BLANK, "copy failed.");
	return;
    }
    
    new->next = NULL;			/* new one is last */
    
    /* enclosure node is all set up; just need to link it in */
    if (!user->ep)			/* if first encl */
	user->ep = new;
    else {				/* locate end of list & append */
	for (tail = user->ep; tail->next; tail = tail->next)
	    ;
	tail->next = new;	
    }
    
    ++user->summ.enclosures;		/* update enclosure count */
    user->summ.type = MESSTYPE_BLITZ;	/* message is old blitz-format */
    
    print(user, BLITZ_OK);		/* enclosure received ok */
    
}

/* c_edel --

    Remove the n'th enclosure from the current message, AND from the
    saved copy of the message.
*/

void c_edel(udb *user) {

    enclinfo	*p;
    enclinfo    *pp = NULL;
    long	num;
    int		i;
    folder	*fold;			/* folder message is in */
    long	messid;			/* new messageid  */
    messinfo	mi;			/* new message info */
    summinfo	*summ;			/* summary info (modified in place) */
    char	resp[MAX_STR];
    long	newlen;
    					    
    if (!parse_num(user, &num))		/* which encl? */
	return;	

    if (num <= 0 || num > user->summ.enclosures) {
	print(user, BLITZ_BADARG);
	return;		
    }
    
    /* both the text and the enclosures must belong to a saved message */
    if (user->summ.messid < 0) {
	print1(user, BLITZ_BADARG_BLANK, "Current message has been modified");
	return;
    }
    for (p = user->ep; p; p = p->next) {
	if (p->finfo.temp) {
	    print1(user, BLITZ_BADARG_BLANK, "Current message has been modified");
	    return;
	}
    }

    sem_seize(&user->mb->mbsem);	
    fold = &user->mb->fold[user->currmessfold]; /* current message is in this folder */
    summ = get_summ(user->mb, user->summ.messid, &fold);	/* locate the summary */
    sem_release(&user->mb->mbsem);
    if (summ == NULL) {
	print(user, BLITZ_BADARG);
	return;		
    }
    if (summ->enclosures != user->summ.enclosures) {
	print1(user, BLITZ_BADARG_BLANK, "Current message has been modified");
	return;
    }

    p = user->ep;
    if (num == 1)			/* new first? */
	user->ep = user->ep->next;
    else {				/* count through list */
	for (i = 2; i <= num; ++i) {
	    pp = p;			/* back pointer */
	    p = p->next;
	}
	pp->next = p->next;		/* relink around */
    }

    p->next = NULL;
    clean_encl_list(&p); 		/* clean up & free */

    /* create new message file without that enclosure */
    messid = next_messid();
    if (!mess_setup(messid, &user->head, &user->text, user->ep, &mi, 
    			    user->fs, summ->type)) {
	print(user, BLITZ_ERROR);
	return;		
    }
    /* give user a copy of the new message */
    if (summ->enclosures > 1)		/* if any enclosures left */
	newlen = mi.finfo.len;	/* total length including header & encls */
    else
	newlen = user->text.len;/* show just text, not header */
    if (!mess_deliver(user->mb, &mi, newlen, resp)) {
	t_errprint_ll("c_edel: error delivering uid %ld messid %ld", user->uid, messid);
	mess_done(&mi);
	print(user, BLITZ_ERROR);
	return;		
    }
    /* and get rid of the old one */
    if (!mess_rem(user->mb, user->summ.messid, user->summ.totallen)) {
	t_errprint_ll("c_edel: error removing uid %ld messid %ld", user->uid, messid);
    }

    /* update the summary info in the folder */
    sem_seize(&user->mb->mbsem);
    fold->foldlen -= summ->totallen - newlen; /* correct folder length */
    summ->messid = messid;		/* assign new message id */
    --summ->enclosures;			/* one less enclosure */
    summ->totallen = newlen;		/* updated length */
    touch_folder(user->mb, fold);	/* update session tag of folder */
    fold->dirty = TRUE;			/* folder has changed */
    mess_done(&mi);
    sem_release(&user->mb->mbsem);
        
    finfoclose(&user->head);		/* clean up old current message */
    finfoclose(&user->text);
    clean_encl_list(&user->ep);
    
    /* open edited message, fill in udb fields */
    if (mess_get(user, &fold, messid)) {	
	t_sprintf(resp, "%ld", user->summ.messid);
	print1(user, BLITZ_LASTLINE, resp);	/* return new messid */
    } else
    	print(user, BLITZ_ERROR);
    	
    
}
/* c_elis --

    List enclosures of current message.
    
*/

void c_elis(udb *user) {

    enclinfo	*ep;
    char	resp[MAX_STR];
    
    if (!user->ep) {			/* any enclosures? */
	print(user, BLITZ_NODATA);
	return;
    }
    
    for (ep = user->ep; ep; ep = ep->next) {
	t_sprintf(resp, "%s %ld %s", ep->type, ep->finfo.len, ep->name);
	if (ep->next)
	    print1(user, BLITZ_MOREDATA, resp);
	else
	    print1(user, BLITZ_LASTLINE, resp);
    }
}

/* c_encl --

    Download the n'th enclosure of the current message.
    
*/

void c_encl(udb *user) {

    enclinfo	*ep;
    long	num;
    char	resp[32];		/* intermediate response */
    
    if (!parse_num(user, &num))		/* which encl? */
	return;	

    /* if no current mess, summ.enclosures == 0 */
    if (num <= 0 || num > user->summ.enclosures) {
	print(user, BLITZ_BADARG);
	return;		
    }
    
    for (ep = user->ep; --num > 0; ep = ep->next)
    	;				/* locate the right encl */
	
    /* command valid, tell client how much to expect */
    t_sprintf(resp, "%ld", ep->finfo.len);
    print1(user, BLITZ_INTERMEDIATE, resp);
    t_fflush(&user->conn);		/* send that on ahead */
    
    /* copy the enclosure file */
    if (finfocopy(&user->conn, &ep->finfo))
	print(user, BLITZ_OK);
}

/* c_erem --

    Remove the n'th enclosure from the current message.
*/

void c_erem(udb *user) {

    enclinfo	*p;
    enclinfo    *pp = NULL;
    long	num;
    int		i;
    
    if (!parse_num(user, &num))		/* which encl? */
	return;	

    if (num <= 0 || num > user->summ.enclosures) {
	print(user, BLITZ_BADARG);
	return;		
    }
    
    p = user->ep;
    if (num == 1)			/* new first? */
	user->ep = user->ep->next;
    else {				/* count through list */
	for (i = 2; i <= num; ++i) {
	    pp = p;			/* back pointer */
	    p = p->next;
	}
	pp->next = p->next;		/* relink around */
    }

    p->next = NULL;
    clean_encl_list(&p); 		/* clean up & free */
    
    --user->summ.enclosures;		/* one less enclosure */
    
    print(user, BLITZ_OK);
    
}

/* c_expr --

    Set message expiration date.
*/

void c_expr(udb *user) {

    long	messid;			/* message to expire */
    long	foldnum;		/* folder it resides in */
    folder	*fold;			/* .. */
    u_long	expdate;		/* new expiration date */
    
    if (!parse_messid(user, &messid, &foldnum))	/* get message id */
    	return;	

    if (foldnum == -1)			/* folder not specified */
	fold = NULL; 			/* search InBox/Trash */
    else
    	fold = &user->mb->fold[foldnum];
	
    if (!parse_uns(user, &expdate))	/* get desired date */
	return;
	
    if (set_expr(user->mb, fold, messid, expdate))
	print(user, BLITZ_OK);
    else
	print1(user, BLITZ_BADARG_BLANK, "No such message.");
	
}	
/* c_fdef --

    Create new folder.
*/

void c_fdef(udb *user) {

    char	temp[MAX_STR];
    char	fname[MAX_STR];		/* folder name */
    int		foldnum;		/* folder number we assigned */
    
    parse_arg(user, temp);		/* get quoted folder name */
    unquote(fname, temp);			
    
    if (strlen(fname) == 0) {		/* null name not allowed */
	print(user, BLITZ_MISSINGARG);
	return;
    } else if (strlen(fname) >= FOLD_NAMELEN) {
    	print1(user, BLITZ_BADARG_BLANK, "Folder name too long");
	return;
    }	
    
    foldnum = fold_create(user->mb, fname);
    
    if (foldnum < 0)
	print1(user, BLITZ_BADARG_BLANK, "Duplicate folder name");
    else {
	t_sprintf(fname, "%d", foldnum); /* return folder # */
	print1(user, BLITZ_LASTLINE, fname);    
    }
    
}
/* c_flis --

    List folder names & sizes.
*/

void c_flis(udb *user) {

    long	foldnum;		/* specific folder number */

    while (isspace(*user->comp))
	++user->comp;			/* skip leading spaces */
    
    if (*user->comp) {			/* folder number given? */
	if (!parse_num(user, &foldnum))	/* parse the folder # */
	    return;
	if (!foldnum_valid(user->mb, foldnum)) {
	    print(user, BLITZ_BADARG);	/* invalid folder # */
	    return;
	}
    } else
	foldnum = -1;			/* do all folders */
	
    fold_list(user->mb, foldnum);
}
/* c_fnam --

    Rename folder.
*/

void c_fnam(udb *user) {

    char	temp[MAX_STR];
    char	fname[MAX_STR];		/* folder name */
    long	foldnum;		/* folder to rename */
    
    if (!parse_num(user, &foldnum))	/* parse the folder # */
	return;
    if (!foldnum_valid(user->mb, foldnum)) {
	print(user, BLITZ_BADARG);	/* invalid folder # */
    	return;
    } else if (foldnum < DEFAULT_FOLDCOUNT) {
	print1(user, BLITZ_BADARG_BLANK, "Standard folders may not be renamed.");
	return;
    }	
    parse_arg(user, temp);		/* get new folder name */
    unquote(fname, temp);			
    
    if (strlen(fname) == 0) {		/* null name not allowed */
	print(user, BLITZ_MISSINGARG);
	return;
    } else if (strlen(fname) >= FOLD_NAMELEN) {
    	print1(user, BLITZ_BADARG_BLANK, "Folder name too long");
	return;
    }	
        
    if (!fold_rename(user->mb, foldnum, fname)) 
	print1(user, BLITZ_BADARG_BLANK, "Duplicate folder name");
    else 
	print(user, BLITZ_OK);    
    
}
/* c_frem --

    Remove folder(s).
*/

void c_frem(udb *user) {

    long	foldnum;	/* folder to remove */
    
    if (!parse_num(user, &foldnum))	/* parse the folder # */
	return;
    if (!foldnum_valid(user->mb, foldnum)) {
	print(user, BLITZ_BADARG);	/* invalid folder # */
    } else if (foldnum < DEFAULT_FOLDCOUNT) {
	print1(user, BLITZ_BADARG_BLANK, "Standard folders may not be removed.");
    } else {
	fold_remove(user->mb, &user->mb->fold[foldnum]);
	if (user->currmessfold == foldnum)  /* was current message in there? */
	    do_clem(user);		/* yes - it's destroyed now */
	print(user, BLITZ_OK);
    } 
}
/* c_fsum --

    Get message summaries from arbitrary folder.
*/

void c_fsum(udb *user) {

    long	foldnum;		/* folder number */
    
    if (!parse_num(user, &foldnum))	/* parse the folder # */
	return;
    if (!foldnum_valid(user->mb, foldnum))
	print(user, BLITZ_BADARG);	/* invalid folder # */
    else {
	do_summ(user, &user->mb->fold[foldnum]); /* parse rest of command & do it */
    }
}
/* c_head --

    Download header of current message.
*/

void c_head(udb *user) {

    char	resp[32];
    
    if (!user->head.fname[0]) {		/* no file? */
    	print(user, BLITZ_NOMESSAGE);
	return;
    }
    
    /* tell client how much there is */
    t_sprintf(resp, "%ld", user->head.len);
    print1(user, BLITZ_INTERMEDIATE, resp);
    
    /* copy the file */
    if (finfocopy(&user->conn, &user->head))
	print(user, BLITZ_OK);
    else
	print(user, BLITZ_ERROR);
    
}
/*^L c_hide --

    Recipient list should be hidden when current message is sent.

*/

void c_hide(udb *user) {

    user->hiderecips = TRUE;
    print(user, BLITZ_OK);

}
/* c_krb4 --

    Accept Kerberos (V4) ticket for validatation.
    
*/

void c_krb4(udb *user) {

#ifdef KERBEROS

    long	l;			/* message length */
    char	resp[32];		/* intermediate response */
    KTEXT	authent = NULL;		/* authentication info */
    AUTH_DAT 	*ad = NULL;		/* validated authentication data */
    int		krb_stat;		/* status from kerberos library */
    char	krb_err[MAX_STR];	/* corresponnding error text */
    char	realm[REALM_SZ];	/* local realm */
    dndresult	*dndres;		/* dnd lookup results */	
    int		dndstat;   		/* and status */
    
    if (user->validated) {
	print(user, BLITZ_ALREADY_VAL);	/* already signed on */
	return;
    }
    
    if (!parse_num(user, &l))		/* get length */
	return;	

    if (l > MAX_KTXT_LEN) {		/* make sure it's not too much */
	print1(user, BLITZ_ERR_BLANK, "Kerberos data too long.");
	return;    
    } 
    
    /* buffers are allocated from heap to keep stack size down */
    authent = mallocf(sizeof(KTEXT_ST)); /* auth info from client */
    ad = mallocf(sizeof(AUTH_DAT));	/* verified ticket info */
    
    t_sprintf(resp, "%ld", l);		/* tell client to fire away */
    print1(user, BLITZ_INTERMEDIATE, resp);
    t_fflush(&user->conn);	
    t_fseek(&user->conn, 0, SEEK_CUR);	/* set up to read */

    /* read the Kerberos authenticator */
    if (!chars_get_buf(user, l, (char *) authent->dat)) {
	t_free(authent);		/* copy failed (conn lost?) */
	t_free(ad);
	print1(user, BLITZ_ERR_BLANK, "read failed.");
	return;
    }
    authent->length = l;		/* length of ticket etc. */
    
    sem_seize(&krb_sem);		/* get access to kerberos library */
    krb_set_key(priv_pw, 1);		/* set the service key */
    
    /* decrypt & verify the ticket */
    krb_stat = krb_rd_req(authent, priv_name, "", user->remoteaddr.sin_addr.s_addr, ad, NULL);
    
    if (krb_stat != KSUCCESS) {
	strcpy(krb_err, krb_get_err_text(krb_stat));	/* must copy static text */
    }
    krb_get_lrealm(realm, 1);	/* get local realm (w/ sem seized) */
    sem_release(&krb_sem);	/* release sem before proceeding further */

    t_free(authent); 		/* done with the input ticket now */

    if (krb_stat != KSUCCESS) {	/* ticket rejected */
	print1(user, BLITZ_KRB_VALERR, krb_err); /* pass on the Kerberos error */
	t_free(ad);		/* clean up */
	return;    
    }
    
    /* ticket decrypted ok; do some consistency checks */
    
    if (strlen(ad->pinst) != 0 ||	/* must be null instance */
    	strcmp(ad->prealm, realm) != 0) { /* in our realm */
    	print1(user, BLITZ_KRB_VALERR, "wrong client instance/realm name in ticket");
	t_free(ad);		/* clean up */
	return;    
    }
    	
    /* ticket looks valid; look name up in DND for other info */
    
    dndstat = t_dndlookup1(ad->pname, val_farray, &dndres);

    if (dndstat != DND_OK) {		/* did it work? */
	if (dndstat != DND_DOWN)	/* log odd errors to console */
	    t_errprint_l("c_krb4: unexpected DND error %ld", dndstat);
	print(user, BLITZ_NODND);
	t_free(ad);		/* clean up */
	return;    	
    }
    
    do_validate3(user, dndres);		/* process results from DND */
    t_free(dndres);			/* and discard them */
    t_free(ad);				/* clean up */

#else
    print1(user, BLITZ_NO_SUPPORT, "Kerberos validation not supported.");
    return;
#endif

}

/* c_ldat --

    Upload mailing list contents.  Each line of data received should
    contain one recipient name (terminated with CR).  No validity
    checks (name lookup etc.) are done on the recipients; if the
    client wishes to validate the list before uploading it that's
    its responsibility.  Note that our internal format for list
    storage uses newline terminators, so the CRs are discarded.
*/

void c_ldat(udb *user) {

    long	l;		/* length */
    char	resp[32];
    ml_data	*ml;
    char	c;		/* one char */
    	
    if (!parse_num(user, &l))
	return;
	
    if (l > ML_MAX) {		/* check against max list length */
	print1(user, BLITZ_ERR_BLANK, " Mailing list too long; sorry.");
	return;
    }

    /* command valid, tell client how much we expect */
    t_sprintf(resp, "%ld", l);
    print1(user, BLITZ_INTERMEDIATE, resp);
    t_fflush(&user->conn);	/* send that */
    t_fseek(&user->conn, 0, SEEK_CUR); /* and set up to read */
	
    ml_clean(&user->ldat);	/* discard any old data */
    
    ml = mallocf(l + sizeof(ml_data));
    
    ml->next = NULL;
    ml->maxlen = l;
    ml->len = 0;
    
    for ( ; l > 0 ; --l) {
	c = t_getc(&user->conn);
	if (c == EOF)		/* connection gone? */
	    break;
	if (c == '\r')	/* CR->LF */
	    c = '\n';
	ml->data[ml->len++] = c;
    }

    user->ldat = ml;		/* all set, record it */
    
    print(user, BLITZ_OK);	/* data received ok */
}

/* c_ldef --

    Create/update mailing list (with data from a previous LDAT command).
    
*/

void c_ldef(udb *user) {

    long	ltype;			/* list type */
    char	lname[MAX_STR];		/* list name */
    char	*comma;
    	
    comma = index(user->comp, ',');	/* find delimiter */
    if (!comma) 
	ltype = LTYPE_LOCAL;		/* default is local list */
    else {
	strtonum(comma+1, &ltype);	/* get list type */
	*comma = 0;			/* list name is before delim */
    }
    
    strtrimcpy(lname, user->comp);	/* remove leading/trailing spaces */
    
    if (!lname[0]) {			/* null name not allowed */
	print(user, BLITZ_MISSINGARG);
	return;
    }
    
    if (strcasecmp(lname, "me") == 0) {	/* can't redefine special alias "me" */
	print1(user, BLITZ_BADARG_BLANK, "You cannot redefine the alias 'me'.");
	return;
    }
    
    if (!user->ldat) {			/* reject empty mailing list */
	print(user, BLITZ_NORECIPIENTS);
	return;
    }
    
    if (ltype == LTYPE_GLOBAL) {	/* global list -- do access checks */
	if (!(pubml_acc(user->mb, lname) & LACC_WRITE)) {
	    print1(user, BLITZ_BADARG_BLANK, "You are not allowed to change that list.");
	    return;
	}
	pubml_set(lname, user->ldat);	/* set public list */
    } else if (ltype == LTYPE_LOCAL)
	ml_set(user->mb, lname, user->ldat); /* set private list */
    else {				/* bad list type */
	print(user, BLITZ_BADARG);
	return;
    }
    
    print(user, BLITZ_OK);		/* list set */

}

/* c_list --

    Retrieve mailing list.
    
*/

void c_list(udb *user) {

    long	ltype;			/* list type */
    char	lname[MAX_STR];		/* list name */
    ml_data	*ml_head;		/* the list */
    char	*comma;
    ml_data	*ml;
    char	*p, *q;
    	
    comma = index(user->comp, ',');	/* find delimiter */
    if (!comma) 
	ltype = LTYPE_LOCAL;		/* default is local list */
    else {
	strtonum(comma+1, &ltype);	/* get list type */
	*comma = 0;			/* list name is before delim */
    }
    
    strtrimcpy(lname, user->comp);	/* remove leading/trailing spaces */
    
    if (!lname[0]) {			/* null name not allowed */
	print(user, BLITZ_MISSINGARG);
	return;
    }
        
    if (ltype == LTYPE_GLOBAL) {	/* global list -- do access checks */
	if (!(pubml_acc(user->mb, lname) & LACC_READ)) {
	    print1(user, BLITZ_BADARG_BLANK, "You are not allowed to see that list.");
	    return;
	}
	if (!pubml_get(lname, &ml_head)) {
	    print(user, BLITZ_BADARG);	/* no such public list */
	    return;
	}
    } else if (ltype == LTYPE_LOCAL) {
	if (!ml_get(user->mb, lname, &ml_head, NULL)) {
	    print(user, BLITZ_BADARG);	/* no such private list */
	    return;
	}
    } else {				/* bad list type */
	print(user, BLITZ_BADARG);
	return;
    }
    
    /* for each block of list */
    for (ml = ml_head; ml; ml = ml->next) {
      	/* for each name in block */
	for (p = ml->data; p < ml->data + ml->len; p = q) {
	    q = index(p, '\n');		/* deal with \n name terminators */
	    if (!q)			/* don't run off end */
		q = ml->data + ml->len;
	    else	
		*q++ = 0;		/* terminate string */ 
		
	    /* print a name */
	    if (ml->next || q < ml->data + ml->len)
		print1(user, BLITZ_MOREDATA, p);
	    else
		print1(user, BLITZ_LASTLINE, p);
	}  
    }
    
    ml_clean(&ml_head);			/* free up the list copy */
}

/* c_lrem --

    Remove mailing list.
    
*/

void c_lrem(udb *user) {

    long	ltype;			/* list type */
    char	lname[MAX_STR];		/* list name */
    char	*comma;
    	
    comma = index(user->comp, ',');	/* find delimiter */
    if (!comma) 
	ltype = LTYPE_LOCAL;		/* default is local list */
    else {
	strtonum(comma+1, &ltype);	/* get list type */
	*comma = 0;			/* list name is before delim */
    }
    
    strtrimcpy(lname, user->comp);	/* remove spaces from either end */
    
    if (!lname[0]) {			/* null name not allowed */
	print(user, BLITZ_MISSINGARG);
	return;
    }
    
    if (ltype == LTYPE_GLOBAL) {	/* global list -- do access checks */
	if (!(pubml_acc(user->mb, lname) & LACC_WRITE)) {
	    print1(user, BLITZ_BADARG_BLANK, "You are not allowed to remove that list.");
	    return;
	}
	if (!pubml_rem(lname)) {	/* remove public list */
	    print(user, BLITZ_BADARG);
	    return;
	}
    } else if (ltype == LTYPE_LOCAL) {
	if (!ml_rem(user->mb, lname)) {
	    print(user, BLITZ_BADARG);	/* no such private list */
	    return;
	}
    } else {				/* bad list type */
	print(user, BLITZ_BADARG);
	return;
    }
    
    print(user, BLITZ_OK);		/* list removed */

}

/* c_lsts --

    Enumerate mailing lists (personal or public).
    
*/

void c_lsts(udb *user) {

    char	*p;		
    long	ltype;
    
    p = user->comp;
    if (!*p)
	ltype = LTYPE_LOCAL;		/* default to personal lists */
    else
	strtonum(p, &ltype);
	
    if (ltype == LTYPE_LOCAL)		
	ml_summary(user->mb, FALSE);	/* display personal lists */
    else if (ltype == LTYPE_GLOBAL)
	ml_summary(user->mb, TRUE);	/* display public lists */
    else
	print(user, BLITZ_BADARG); 	/* none of the above */
}
/*^L c_mark --


    Mark a list of messages read/unread.
*/

void c_mark(udb *user) {

    boolean_t	read;		/* new read/unread state */
    messlist    *nums;          /* list of ids */
    char        resp[32];
    folder	*fold;
    summinfo	*summ;
    int         i;

    while (isspace(*user->comp))
        ++user->comp;                   /* skip leading spaces */

    if (*user->comp != 'R' && *user->comp != 'U') {
	print(user, BLITZ_BADARG);
	return;
    }
    read = *user->comp++ == 'R';
       
    if ((nums = parse_messlist(user)) == NULL)
        return;                 /* syntax error */

    /* lock box, then validate all messid's */

    sem_seize(&user->mb->mbsem);

    for (i = 0; i < nums->count; ++i) {
	if (nums->foldnum[i] == -1)	/* [foldnum/] messid */
	    fold = NULL;		/* no number - search inbox & trash */
	else
	    fold = &user->mb->fold[nums->foldnum[i]];

        if (get_summ(user->mb, nums->messid[i], &fold) == NULL) {
		/* message missing - clean up & fail */
	    sem_release(&user->mb->mbsem);
	    t_free(nums);
	    t_sprintf(resp, "%ld", nums->messid[i]);
            print1(user, BLITZ_BADARG_BLANK, resp);  /* no such messid? */
	    return;
	}
    }

    /* second pass, actually mark everything */
    for (i = 0; i < nums->count; ++i) {
        if (nums->foldnum[i] == -1)     /* [foldnum/] messid */
            fold = NULL;                /* no number - search inbox & trash */
        else
            fold = &user->mb->fold[nums->foldnum[i]];

        if ((summ = get_summ(user->mb, nums->messid[i], &fold)) == NULL) {
	    t_errprint_ll("c_mark: summary vanished uid %ld messid %ld", user->uid, nums->messid[i]);
        }
	if (read != summ->read || summ->receipt) {	/* if changing status */
	    touch_folder(user->mb, fold); /* modifying folder; flush caches */
            summ->read = read;	      	/* new setting */
	    summ->receipt = FALSE;	/* either way, clear receipt flag */
            fold->dirty = TRUE;     	/* folder has changed */
	}
    }
    sem_release(&user->mb->mbsem);

    t_free(nums);
    print(user, BLITZ_OK);
}
/* c_mcat --

    Download MIME catalog.
    
*/

void c_mcat(udb *user) {
   
    if (!user->text.fname[0]) {		/* no file? */
    	print(user, BLITZ_NOMESSAGE);
	return;
    }
        
    if (!mime_catalog(user)) {		/* generate catalog */
	print(user, BLITZ_ERROR);	/* disk error etc. */
    }
}

/* c_mdat --

    Upload message text.
    
*/

void c_mdat(udb *user) {

    fileinfo	finfo;			/* temp file */
    long	l;			/* message length */
    long	t = MESSTYPE_BLITZ;	/* and type */
    char	resp[32];		/* intermediate response */
    
    if (!parse_num(user, &l))		/* get length */
	return;	

    while (isspace(*user->comp))	/* skip leading spaces */
	++user->comp;

    if (*user->comp && !parse_num(user, &t)) /* parse optional type */
	return;

    if (t != MESSTYPE_BLITZ && t != MESSTYPE_RFC822) {
    	print1(user, BLITZ_BADARG_BLANK, "unknown message type");
	return;
    }
    
    if (!check_mess_space(user, l))	/* make sure there will be room */
	return;
	
    temp_finfo(&finfo);			/* get temp file */
    
    t_sprintf(resp, "%ld", l);		/* tell client to fire away */
    print1(user, BLITZ_INTERMEDIATE, resp);
    t_fflush(&user->conn);	

    if (!chars_get(user, l, &finfo)) {	/* read specified # of chars */
	finfoclose(&finfo);		/* copy failed (conn lost?) */
	print1(user, BLITZ_ERR_BLANK, "copy failed.");
	return;
    }

    print(user, BLITZ_OK);		/* data received ok */
    
    finfoclose(&user->text);		/* forget any previous text */
    user->text = finfo;			/* this is it now */
    user->summ.messid = -1;		/* current message isn't saved */
    user->currmessfold = -1;
    user->summ.type = t;		/* message type (defalut is old blitz-format) */
    
    /* if client uploaded header information as well as text,
       must scan to determine where one ends and the next begins */
    if (t == MESSTYPE_RFC822) {
	t_file		*f;
	char		*buf;
	long		remaining;
	
	finfoclose(&user->head);	/* forget previous header */

	if ((f = t_fopen(user->text.fname, O_RDONLY, 0)) == NULL) {
	    t_perror1("c_mdat: cannot open ", user->text.fname);
	    finfoclose(&user->text);
	    return;
	}
    
	(void) t_fseek(f, user->text.offset, SEEK_SET); /* seek to start of header */
    
	/* scan looking for end of header */
	remaining = user->text.len;
	while ((buf = getheadline(f, &remaining, FALSE)) != NULL) {
	    if (strlen(buf) == 0)
		break;			/* blank line - done */
	    t_free(buf);
    	}
	if (buf) t_free(buf);
	
	user->head = user->text;	/* header & text in same file */
	user->head.temp = FALSE;	/* (don't try to unlink temp file twice) */
	if (remaining <= 0) {		/* nothing after header? */
	    user->head.len = user->text.len; /* all header */
	    user->text.len = 0;		/* no text */
	} else {
	    user->head.len = user->text.len - (remaining+1); /* header is before blank line */
	    user->text.offset = user->head.len + 1;	/* text is after it */
	    user->text.len = remaining;
	}
	
	t_fclose(f);
    }
}

/* c_mess --

    Open message, make it the "current message".
    
*/

void c_mess(udb *user) {

    long	messid;			/* id desired */
    folder	*fold;			/* folder to find it in */
    long	foldnum;		/* .. */
 				   
    if (!parse_messid(user, &messid, &foldnum)) /* which message? */
	return;			
    if (foldnum == -1) 			/* folder unspecified? */
    	fold = NULL;			/* search InBox & Trash */
    else
    	fold = &user->mb->fold[foldnum];
	
    finfoclose(&user->head);		/* clean up old current message */
    finfoclose(&user->text);
    clean_encl_list(&user->ep);
    user->summ.messid = -1;		/* old summary info not valid */
    user->summ.enclosures = 0;
    user->currmessfold = -1;
    
    if (mess_get(user, &fold, messid))	/* open message, fill in udb fields */
	print(user, BLITZ_OK);
    else
	print(user, BLITZ_BADARG);

}
/* c_move --

    Move a list of messages, setting a new expiration date if the destination folder
    has a default expiration pref set.
    
    MOVE <from> <to> <messid>[,<messid>...]
*/

void c_move(udb *user) {

    messlist	*nums;		/* list of ids */
    char	resp[32];
    long	from, to;	/* source & destination folder #s */
    folder	*fromfold;	/* source folder */
    u_long	newexp;		/* new expiration date */
    int		i;
    
    /* get source & destination folder numbers */
    if (!parse_num(user, &from) || !parse_num(user, &to))
	return;
    if (!foldnum_valid(user->mb, from) || !foldnum_valid(user->mb, to)) {
	print(user, BLITZ_BADARG);
	return;
    }
    
    /* check destination folder autoexpire */
    newexp = fold_autoexp(user->mb, to);
    if (newexp == -1)			/* will expiration change? */
	t_sprintf(resp, "%s%ld", BLITZ_MOREDATA, newexp); /* no */
    else
    	t_sprintf(resp, "%s%lu", BLITZ_MOREDATA, newexp);
	
    print(user, resp);			/* tell client what expiration will be */
    
    if ((nums = parse_messlist(user)) == NULL)
	return;				/* syntax error */
	
    for (i = 0; i < nums->count; ++i) {
	if (nums->foldnum[i] == -1)	/* source folder specified for this mess? */
	    fromfold = &user->mb->fold[from]; 
	else
	    fromfold = &user->mb->fold[nums->foldnum[i]];
	if (summ_move(user->mb, fromfold, 
	                        &user->mb->fold[to], nums->messid[i], newexp)) {
	    t_sprintf(resp, "%ld %ld %ld", nums->messid[i], to, user->mb->fold[to].count);
	    if (i == nums->count - 1)
		print1(user, BLITZ_LASTLINE, resp);
	    else
		print1(user, BLITZ_MOREDATA, resp);
	    if (user->summ.messid == nums->messid[i]) /* moved current message? */
	    	user->currmessfold = to;	/* yes - update folder # */
	} else 
	    print(user, BLITZ_BADARG);	/* no such messid? */
    }
    
    t_free(nums);
	
}

/* c_msum --

    Return summary info for message(s) by messid.
    
*/

void c_msum(udb *user) {

    messlist	*nums;		/* list of ids */
    summinfo	*summ;		/* a summary */
    char	buf[SUMMBUCK_LEN]; /* a formatted summary */
    int		i;
    folder	*fold;
    
    if ((nums = parse_messlist(user)) == NULL)
	return;			/* syntax error */
	
    for (i = 0; i < nums->count; ++i) {
	sem_seize(&user->mb->mbsem);	/* lock for get_summ */

	if (nums->foldnum[i] == -1)	/* folder specified for this mess? */
	    fold = NULL; 		/* no - search InBox & Trash */
	else
	    fold = &user->mb->fold[nums->foldnum[i]];
	
	/* search box to locate the summary */
	if ((summ = get_summ(user->mb, nums->messid[i], &fold)) == NULL) {
	    sem_release(&user->mb->mbsem);
	    print1(user, BLITZ_BADARG_BLANK, "No such message.");
	} else {		
	    summ_fmt(summ, buf); /* get into download format */
	    sem_release(&user->mb->mbsem); /* unlock BEFORE printing */
	    if (i == nums->count - 1)
		print1(user, BLITZ_LASTLINE, buf);
	    else
		print1(user, BLITZ_MOREDATA, buf);
	}
    }
    
    t_free(nums);
}

/* c_noop --

    Nothing.
    
*/

void c_noop(udb *user) {

    print(user, BLITZ_OK);
}

/* c_pase --

    Encrypted password.
    
*/

void c_pase(udb *user) {

    if (!user->validating) {
	print(user, BLITZ_NO_USER_YET);
	return;
    }
    
    do_validate2(user, user->comp);	/* rest of line is pw */
}

/* c_pass --

    Cleartext pw.  Should probably hand-encrypt here, but for
    now we're lazy.
    
*/

void c_pass(udb *user) {

    if (!user->validating) {
	print(user, BLITZ_NO_USER_YET);
	return;
    }
    
    do_validate2(user, user->comp);	/* rest of line is pw */
}

/* c_pdef --

    Define one or more preferences.
    
*/

void c_pdef(udb *user) {

    char	key[MAX_STR];		/* pref name */
    char	value[MAX_STR];		/* and value */
    char	temp[MAX_STR];
    
    if (!*user->comp) {			/* must be at least one */
	print(user, BLITZ_MISSINGARG);
	return;
    }
    
    while(*user->comp) {		/* for each pair */
	parse_arg(user, temp);		/* get quoted name */
	unquote(key, temp);		/* pref name */
	parse_arg(user, value);		/* value (keep quoted) */
	/* (this check is just paranoia in case
	    PREF_MAXLEN is ever < MAX_STR) */
	if (strlen(value) >= PREF_MAXLEN) {
	    print1(user, BLITZ_BADARG, " (value too long)");
	    return;
	}
	pref_set(user->mb, key, value); /* set it */
	pref_didchange(user->mb, key, NULL); /* do any special processing */
    }
    
    print(user, BLITZ_OK);

	/***** check ForwardTo??? ****/	
}

/* c_pref --

    Retrieve one or more preferences.
    
*/

void c_pref(udb *user) {

    char	key[MAX_STR];		/* pref name */
    char	value[MAX_STR];		/* and value */
    char	temp[MAX_STR];

    if (!*user->comp) {			/* none at all? */
	print(user, BLITZ_MISSINGARG);
	return;
    }  

    while(*user->comp) {		/* for each pair */
	parse_arg(user, temp);		/* get quoted name */
	unquote(key, temp);		/* pref name */
	
	if (pref_get(user->mb, key, value)) {
	    if (*user->comp)
		print1(user, BLITZ_MOREDATA, value);
	    else
		print1(user, BLITZ_LASTLINE, value);
	} else {			/* not found */
	    if (*user->comp)
		print1(user, BLITZ_UNDEFMORE, key);
	    else
		print1(user, BLITZ_NODATA, key);
	}
    }      
}

/* c_prem --

    Remove one or more preferences.
    
*/

void c_prem(udb *user) {

    char	key[MAX_STR];		/* pref name */
    char	temp[MAX_STR];

    if (!*user->comp) {			/* none at all? */
	print(user, BLITZ_MISSINGARG);
	return;
    }  

    while(*user->comp) {		/* for each pair */
	parse_arg(user, temp);		/* get quoted name */
	unquote(key, temp);		/* pref name */
	
	if (pref_rem(user->mb, key)) {	/* ok, remove it */
	    pref_didchange(user->mb, key, NULL); /* do any special processing */
	    if (*user->comp)
		print1(user, BLITZ_MOREDATA, key);
	    else
		print1(user, BLITZ_LASTLINE, key);
	} else {			/* not found */
	    if (*user->comp)
		print1(user, BLITZ_UNDEFMORE, key);
	    else
		print1(user, BLITZ_NODATA, key);
	}
    }      
}

/* c_push --

    Kick off alter ego.
    
*/

void c_push(udb *user) {

    mbox	*mb;
    char	buf[16];
    
    if (!user->duplicate) {		/* ready for a PUSH? */
	print(user, BLITZ_NOUSER);	/* no - reject */
	return;
    }
    
    /* forcibly disconnect the other session */
    mb = force_disconnect(user->uid, user->fs, user);
    sem_release(&mb->mbsem);		/* don't need to keep this locked */

    t_sprintf(buf, "%ld", user->uid);
    print1(user, BLITZ_OK_BLANK, buf);	/* PUSH worked */
    user->duplicate = FALSE;		/* don't allow another */
    
    do_signon(user,TRUE);		/* all set; finish the login sequence */
}

/* c_quit --

    Mark user as going away.
    
*/

void c_quit(udb *user) {
    
    print(user, BLITZ_OK);
    user->shutdown = TRUE;
}

/* c_rcpt --
    c_rccc --
    c_rbcc --

    Add recipient(s) to [to,cc,bcc] recip list.
    
*/

void c_rcpt(udb *user) {

    do_recip(user, &user->torecips);
}

void c_rccc(udb *user) {

    do_recip(user, &user->ccrecips);
}

void c_rbcc(udb *user) {

    do_recip(user, &user->bccrecips);
}

void do_recip(udb *user, recip **recips) {

    recip	*newrecips;		/* all recips from this cmd */
    recip	*temp;			/* one batch */
    recip	*temp1, *prev;
    boolean_t	more;
    int		recipcount;		/* count for just this cmd */
    boolean_t	route;			/* address parsing state */
    boolean_t	esc;
    boolean_t	quot;
    int		level;
    char 	c;
    char	name[MAX_ADDR_LEN];	/* current recip */
    char	*namep;
    char	*msg;			/* one of the following: */
    static char	*resptab[RECIP_LAST][2] = {
    		{ BLITZ_RECIP_OK, BLITZ_RECIP_OK_MORE },
		{ BLITZ_RECIP_AMBIG, BLITZ_AMBIG_MORE },
		{ BLITZ_BADADDR, BLITZ_BADADDR_MORE },
		{ BLITZ_NOSEND, BLITZ_NOSEND_MORE },
		{ BLITZ_RECIP_NODND, BLITZ_NODND_MORE },
		{ BLITZ_LOOP, BLITZ_LOOP_MORE }}; 
    
    while (isspace(*user->comp))		/* skip leading spaces */
	++user->comp;
    if (!*user->comp) {			/* must be at least one */
	print(user, BLITZ_MISSINGARG);
	return;
    }
    
    /* now set up to parse off the recipient(s) */
    newrecips = NULL;
    route = esc = quot = FALSE;		/* initial state */
    level = 0;	
    while(*user->comp) {		/* scan rest of command line */
	strcpy(name, "");		/* set up to parse another */
	namep = name;
	while (c = getaddrc(&user->comp, &level, &quot, &route, &esc)) {
	    if (c == ',' && !quot && !route && level == 0)
		break;			/* unquoted ',' ends recip */
	    *namep++ = c;		/* copy over the char */
	}
	*namep = 0;
	
	/* have one name; resolve it */
	temp = resolve(name, user->mb, &recipcount);
	
	/* enforce recipient limit */
	if (user->recipcount + recipcount > ADDR_MAX_RECIPS) {
	    free_recips(&temp);		/* free recips from this cmd */
	    free_recips(&newrecips);
	    print(user, BLITZ_TOOMANYRECIPS);
	    return;
	}
	
	user->recipcount += recipcount; /* update the total */
	
	/* add latest batch to cumulative list */
	if (newrecips == NULL)
	    newrecips = temp;		/* first batch (easy) */
	else if (temp) {		/* append to old list */
	    temp1 = temp->next;		/* head of new */
	    temp->next = newrecips->next; /* new tail -> old head */
	    newrecips->next = temp1;	/* old tail -> new head */
	    newrecips = temp;		/* point to new tail */
	}
    }					/* end of parsing/resolving */

    /* print out statuses for new recips */
    temp = newrecips->next;		/* head */
    prev = newrecips;			/* tail */
    more = TRUE;
    
    do {
	more = (temp != newrecips);	/* is this last? */
	msg = resptab[temp->stat][more]; /* get status from table */
	
	strcpy(name, temp->addr);	/* usually return internet addr */
	if (temp->stat != RECIP_OK)	/* but return name they gave if error */
	    strcpy(name, temp->name);
	else if (!name[0])		/* local recip w/ host suppressed */
	    addhost(temp->name, name, m_hostname);	/* return internet form */
	    
	/* don't show "noshow"s unless there was an error */
	if (!temp->noshow || (temp->stat != RECIP_OK && !temp->noerr))
	    print1(user, msg, name);	/* return status to client */
	    
	/* if bad status, discard the recip now (exception:
	"noerr" may be set to allow public list w/ bad members; see do_bounces) */
	if (temp->stat == RECIP_OK || temp->noerr) {
	    prev = temp;		/* this one's ok; move on */
	    temp = temp->next;
	} else {			/* pull bad addr out of list */
	    prev->next = temp->next;	/* relink list */
	    temp1 = temp;
	    temp = temp->next;		/* move on */
	    if (!more) { 		/* freeing last? */
		if (temp1 == temp)	/* last & only? */
		    newrecips = NULL;	/* yes - empty list */
		else
		    newrecips = prev; 	/* no - but new tail */
	    }
	    t_free(temp1);		/* free the bad one */
	}
    } while (more);
    
    /* finally add results to list in udb */
    if (*recips == NULL)
	*recips = newrecips;		/* new ones are it */
    else if (newrecips) {		/* append new ones (if any) */
	temp = newrecips->next;
	newrecips->next = (*recips)->next; /* new tail -> old head */
	(*recips)->next = temp;		/* old tail -> new head */
	(*recips) = newrecips;		/* point to new tail */
    }
}

/* c_rpl2 --

    Set reply-to address. *** validate it??
    
*/

void c_rpl2(udb *user) {

    if (!user->replyto)			/* allocate room, if not yet */
	user->replyto = mallocf(MAX_ADDR_LEN);
	
    strcpy(user->replyto, user->comp);	/* copy it in */
    print(user, BLITZ_OK);
    
}

/* c_rtrn --

    Indicate that we want a receipt for this message.
    
*/

void c_rtrn(udb *user) {

    user->wantreceipt = TRUE;
    print(user, BLITZ_OK);

}

/* c_send --

    Send current message.
    
*/

void c_send(udb *user) {

    boolean_t ok;

    if (!user->torecips && !user->ccrecips && !user->bccrecips) {
	print(user, BLITZ_NORECIPIENTS);
	return;				/* no recipients, no send */
    }
    
    if (user->summ.type != MESSTYPE_BLITZ && user->ep) {
	print1(user, BLITZ_BADARG_BLANK, "Cannot include Blitz enclosures in MIME message");
	return;
    }

    user->summ.receipt = user->wantreceipt; /* want a receipt? */
	
    /* all set, deliver away! */
    ok = deliver(user, user->name, user->torecips, user->ccrecips, user->bccrecips,
    		&user->text, user->ep, &user->summ, user->hextext, user->replyto, user->hiderecips);

    if (ok)
	print(user, BLITZ_OK);		/* ok, it's sent */
    else
	print(user, BLITZ_ERROR);	/* couldn't send (disk trouble?) */

    user->summ.messid = -1;		/* summary info not valid */

}

/* c_size --

    Tell how many messages are in given folder.
    
*/

void c_size(udb *user) {

    long	foldnum;		/* folder number */
    long	count;
    char	resp[32];

    while(isspace(*user->comp))
	++user->comp;			/* skip leading spaces */
    if (!*user->comp) {
	foldnum = INBOX_NUM;		/* default is inbox */
    } else {    
	if (!parse_num(user, &foldnum))	/* parse the folder # */
	    return;
    }
    
    if (!foldnum_valid(user->mb, foldnum))
	print(user, BLITZ_BADARG);	/* invalid folder # */
    else {
	count = fold_size(user->mb, &user->mb->fold[foldnum]);
	t_sprintf(resp, "%ld", count);
	print1(user, BLITZ_LASTLINE, resp);
    }
}

/* c_slog --

    Write to system log file.
    
*/

void c_slog(udb *user) {

    log_it(user->comp);		/* log whatever they have to say */
    print(user, BLITZ_OK);

}
/* c_summ --
    c_tsum --
    
    Download [inbox,trash] summaries.
    
*/

void c_summ(udb *user) {

    do_summ(user, &user->mb->fold[INBOX_NUM]);
}

void c_tsum(udb *user) {

    do_summ(user, &user->mb->fold[TRASH_NUM]);
}

void do_summ(udb *user, folder *fold) {

    long	first, last;		/* folder position range */
    char	*p;
        
    while(isspace(*user->comp))
	++user->comp;			/* skip leading spaces */
    if (!*user->comp) {
	print(user, BLITZ_MISSINGARG);
	return;
    }
    
    p = index(user->comp, '-');		
    if (!p) {				/* range given? */
    	p = user->comp; 		/* no: first arg = last arg */
    } else
	*p++ = 0;
	
    /* user->comp = arg1; p = arg2 */
    
    if (strcmp(user->comp, "$") == 0)	/* parse first arg */
	first = -1;			/* last summ in folder */
    else if (!parse_num(user, &first))
	return;
	
    user->comp = p;	
    if (strcmp(user->comp, "$") == 0)	/* parse second arg */
	last = -1;			/* last summ in folder */
    else if (!parse_num(user, &last))
	return;
    	
    /* ok, now generate the summary lines */
    fold_summary(user->mb, fold, first, last);  
}

/*^L c_tdel --

    Remove text range from the current message, AND from the
    saved copy of the message.
*/

void c_tdel(udb *user) {

    int         i,j,pos;
    folder      *fold;                  /* folder message is in */
    long        messid;                 /* new messageid  */
    messinfo    mi;                     /* new message info */
    summinfo    *summ;                  /* summary info (modified in place) */
    char        resp[MAX_STR];
    long        newlen;
    rangelist	*r = NULL;		/* byte range(s) to delete */
    long	low,high;
    enclinfo	*encl, *tail, *ep;	/* encl list for copying */
    fileinfo    newhead;		/* modified message header */
    fileinfo	newtext;		/* copy of text info */
    t_file	*headf = NULL;
    char        date[64];       	/* RFC822 format date */

    newhead.fname[0] = newtext.fname[0] = 0;

    /* parse list of byte ranges */
    if ((r = parse_rangelist(user)) == NULL)
        return;                 /* syntax error */
    
    if (r->count == 0) {		/* none specified */
        print(user, BLITZ_MISSINGARG);
        goto cleanup;
    }

    for (i = 0; i < r->count; ++i) {	/* sort list by start value */
        low = r->low[i]; pos = i;	/* pos = smallest seen yet */
	for (j = i + 1; j < r->count; ++j) { /* locate next smallest */
	    if (r->low[j] < low) {
		low = r->low[j]; pos = j;
	    }
        }
	if (pos != i) {			/* move smallest to front */
	    high = r->high[pos];	/* copy it to temp */
	    r->low[pos] = r->low[i];
            r->high[pos] = r->high[i];	/* old front out of way */
            r->low[i] = low; r->high[i] = high; /* move to front */
	}
    }

    /* list sorted; now check for overlap */
    for (i = 1; i < r->count; ++i) {
	if (r->high[i-1] >= r->low[i]) {
	    print1(user,BLITZ_BADARG_BLANK, "Ranges overlap");
	    goto cleanup;
	}
    }
    /* finally, make sure last range isn't past end of message */
    if (r->high[r->count-1] >= user->text.len) {
	print1(user,BLITZ_BADARG_BLANK, "Range out of bounds");
	goto cleanup;
    }

    /* the text must belong to a saved message */
    if (user->summ.messid < 0 || user->text.temp) {
        print1(user, BLITZ_BADARG_BLANK, "Current message has been modified");
        goto cleanup;
    }
    if (user->summ.type != MESSTYPE_RFC822) {
        print1(user, BLITZ_BADARG_BLANK, "Current message not in MIME format");
        goto cleanup;
    }

    /* make sure we can locate summary for the message */
    sem_seize(&user->mb->mbsem);
    fold = &user->mb->fold[user->currmessfold]; /* current message is in this folder */
    summ = get_summ(user->mb, user->summ.messid, &fold);        /* locate the summary */
    sem_release(&user->mb->mbsem);
    if (summ == NULL) {
        print(user, BLITZ_BADARG);
        goto cleanup;
    }

    /* create a list of "enclinfo"s describing the sections of the text
       that are not being deleted */
    encl = tail = NULL;
    for (pos = 0, i = 0; ; ++i) {

        /* copy everything before next range (or before EOF if last
           range already passed) */
        low = (i < r->count) ? r->low[i] : user->text.len;
	
        if (low > pos) { 	/* anything before next deletion? */

	    /* get a new enclosure node */
	    ep = (enclinfo *) mallocf(sizeof(enclinfo));
	    ep->next = NULL;
	    ep->finfo = user->text;    /* in this file */
	    				/* note: user->text.temp known FALSE */
	    ep->finfo.offset += pos;
            ep->finfo.len = low - pos;	/* copy just this much */

	    if (encl == NULL) 
	       encl = ep;		/* first one so far */
	    else
		tail->next = ep;	/* link from old tail */
	    tail = ep;			/* new one is at end */
        }
        if (i >= r->count)	/* done when stuff after last range copied */
	    break;

        pos = r->high[i] + 1; 	/* next pass, skip past deleted stuff */
    }

    /* create new message file without deleted range(s) */

    newlen = 0;
    for (ep = encl; ep; ep = ep->next) {
        newlen += ep->finfo.len;        /* compute new text length */
    }
    messid = next_messid();

    temp_finfo(&newhead);               /* need tempfile for edited header */
    if ((headf = t_fopen(newhead.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
        t_perror1("c_tdel: open ", newhead.fname);
	print(user, BLITZ_ERROR);
        goto cleanup;
    }
    get_date(date);
    t_fprintf(headf, "X-Attachment-Removed: %ld bytes deleted; %s id <%ld>\r",
		user->summ.totallen - newlen, date, messid);
    if (!finfocopy(headf, &user->head)) {
	t_perror1("c_tdel: write error ",newhead.fname);
	print(user, BLITZ_ERROR);
        goto cleanup;
    }
    newhead.offset = 0;			/* set up fileinfo for new header */
    newhead.len = t_fseek(headf, 0, SEEK_END);

    newtext = user->text;		/* tell mess_setup... */
    newtext.len = 0;			/* ...to ignore old text */

    if (!mess_setup(messid, &newhead, &newtext, encl, &mi,
                            user->fs, summ->type)) {
        print(user, BLITZ_ERROR);
        goto cleanup;
    }

    /* give user a copy of the new message */
    if (!mess_deliver(user->mb, &mi, newlen, resp)) {
        t_errprint_ll("c_tdel: error delivering uid %ld messid %ld", user->uid, messid);
        mess_done(&mi);
        print(user, BLITZ_ERROR);
        goto cleanup;
    }
    /* and get rid of the old one */
    if (!mess_rem(user->mb, user->summ.messid, user->summ.totallen)) {
        t_errprint_ll("c_tdel: error removing uid %ld messid %ld", user->uid, 
			user->summ.messid);
    }

    /* update the summary info in the folder */
    sem_seize(&user->mb->mbsem);
    fold->foldlen -= summ->totallen - newlen; /* correct folder length */
    summ->messid = messid;              /* assign new message id */
    summ->totallen = newlen;            /* updated length */
    touch_folder(user->mb, fold);       /* update session tag of folder */
    fold->dirty = TRUE;                 /* folder has changed */
    mess_done(&mi);
    sem_release(&user->mb->mbsem);

    finfoclose(&user->head);            /* clean up old current message */
    finfoclose(&user->text);
    clean_encl_list(&user->ep);

    /* open edited message, fill in udb fields */
    if (mess_get(user, &fold, messid)) {
        t_sprintf(resp, "%ld", user->summ.messid);
        print1(user, BLITZ_LASTLINE, resp);     /* return new messid */
    } else
        print(user, BLITZ_ERROR);

cleanup:
    if (r) 				/* free range structures */
	t_free(r);
    clean_encl_list(&encl);
    if (headf)				/* close tempfile */
	t_fclose(headf);
    finfoclose(&newhead);		/* and discard it */

    return;

}
/* c_text --

    Download text of current message.  Send return receipt if
    appropriate.
*/

void c_text(udb *user) {

    char	resp[32];
    fileinfo	textinfo;
    summinfo	*summ;
    folder	*fold;
     
    if (!user->text.fname[0]) {		/* no file? */
    	print(user, BLITZ_NOMESSAGE);
	return;
    }
 
    textinfo = user->text;		/* working copy of fileinfo */

    while(isspace(*user->comp))
	++user->comp;			/* skip leading spaces */

    if (*user->comp) {			/* optional offset/length pair */
	long	offset, len;
	 
	if (!parse_num(user, &offset))	/* get offset */
	    return;	
	if (!parse_num(user, &len))	/* get length */
	    return;
	    
	if (offset < 0 || len < 0 || (offset + len) > user->text.len) {
	    print1(user, BLITZ_BADARG_BLANK, "offset/length out of bounds.");
	    return;
	}
	textinfo.offset += offset;	/* return selected part of text */
	textinfo.len = len;	
    }
    
    /* tell client how much there is */
    t_sprintf(resp, "%ld", textinfo.len);
    print1(user, BLITZ_INTERMEDIATE, resp);
    
    /* copy the file */
    if (finfocopy(&user->conn, &textinfo))
	print(user, BLITZ_OK);
    else
	print(user, BLITZ_ERROR);
    
    if (user->summ.messid >= 0) {	/* if text is part of message in box */
	sem_seize(&user->mb->mbsem);	/* must lock for get_summ */
	fold = &user->mb->fold[user->currmessfold]; /* current message is in this folder */
	if (summ = get_summ(user->mb, user->summ.messid, &fold)) { /* (sic) */
	    if (!summ->read) {		/* was it read? */
		touch_folder(user->mb, fold); /* modifying folder; flush caches */
		sem_release(&user->mb->mbsem); /* unlock, in case do_receipt locks */
		summ->read = TRUE;	/* it is now */
		fold->dirty = TRUE;	/* folder has changed */
		if (summ->receipt)	/* receipt requested? */
		    do_receipt(user->name, summ, &user->head);
	    } else
		sem_release(&user->mb->mbsem);
	} else
	    sem_release(&user->mb->mbsem);
    }
}
/* c_thqx --

    Indicate that text enclosures should be binhexed.
    
*/

void c_thqx(udb *user) {

    user->hextext = TRUE;
    print(user, BLITZ_OK);

}
/* c_topc --

    Set topic of current message.
    
*/

void c_topc(udb *user) {

    strcpy(user->summ.topic, user->comp);	/* rest of line is the topic */
    print(user, BLITZ_OK);
    
}

/* c_trsh --

    Empty the trash.
    
*/

void c_trsh(udb *user) {

    print(user, BLITZ_OK);		/* status always good; speed up response */
    t_fflush(&user->conn);		/* ...by sending it now */
    
    sem_seize(&user->mb->mbsem);
    empty_folder(user->mb, &user->mb->fold[TRASH_NUM]);	/* empty it */
    sem_release(&user->mb->mbsem);
    
    if (user->currmessfold == TRASH_NUM) /* was current message in trash? */
	do_clem(user);			/* if so, it's history! */
}

/* c_tsiz --

    Tell how many messages are in trash.
    
*/

void c_tsiz(udb *user) {

    long	count;
    char	resp[32];
    
    count = fold_size(user->mb, &user->mb->fold[TRASH_NUM]);
    
    t_sprintf(resp, "%ld", count);

    print1(user, BLITZ_LASTLINE, resp);
}

/* c_udel --

    Undelete a list of messages. The "insertion position" in the response
    status is no longer really relevant (the messages are placed at the
    end of the inbox, not in their former location); it's simply set to zero here.
    
*/

void c_udel(udb *user) {

    messlist	*nums;		/* list of ids */
    char	resp[32];
    int		i;
    
    if ((nums = parse_messlist(user)) == NULL)
	return;			/* syntax error */
	
    for (i = 0; i < nums->count; ++i) {
	if (summ_move(user->mb, &user->mb->fold[TRASH_NUM],
			       &user->mb->fold[INBOX_NUM], nums->messid[i], -1)) {
	    t_sprintf(resp, "%ld %d %ld", nums->messid[i], INBOX_NUM, user->mb->fold[INBOX_NUM].count);
	    if (i == nums->count - 1)
		print1(user, BLITZ_LASTLINE, resp);
	    else
		print1(user, BLITZ_MOREDATA, resp);
	    if (user->summ.messid == nums->messid[i]) /* moved current message? */
	    	user->currmessfold = INBOX_NUM;	/* yes - update folder # */
	} else 
	    print(user, BLITZ_BADARG);	/* no such messid? */
    }	
    
    t_free(nums);
}

/* c_udlx --

    Undelete a list of messages, setting a new expiration date.
    
*/

void c_udlx(udb *user) {

    messlist	*nums;		/* list of ids */
    char	resp[32];
    u_long	newexp;		/* new expiration date */
    int		i;
    
    if (!parse_uns(user, &newexp))
	return;
	
    /* months or days? */
    if (*user->comp == 'm' || *user->comp == 'M') {
	newexp = add_months(newexp);
	++user->comp;
    } else
	newexp = add_days(newexp);
	
    t_sprintf(resp, "%s%lu", BLITZ_MOREDATA, newexp);
    print(user, resp);		/* tell client what expiration will be */
    
    if ((nums = parse_messlist(user)) == NULL)
	return;			/* syntax error */
	
    for (i = 0; i < nums->count; ++i) {
	if (summ_move(user->mb, &user->mb->fold[TRASH_NUM], 
	                        &user->mb->fold[INBOX_NUM], nums->messid[i], newexp)) {
	    t_sprintf(resp, "%ld %d %ld", nums->messid[i], INBOX_NUM, user->mb->fold[INBOX_NUM].count);
	    if (i == nums->count - 1)
		print1(user, BLITZ_LASTLINE, resp);
	    else
		print1(user, BLITZ_MOREDATA, resp);
	    if (user->summ.messid == nums->messid[i]) /* moved current message? */
	    	user->currmessfold = INBOX_NUM;	/* yes - update folder # */
	} else 
	    print(user, BLITZ_BADARG);	/* no such messid? */
    }
    
    t_free(nums);
	
}

/* c_uid

    Specify user by uid -- use the #<uid> syntax.  
*/

void c_uid(udb *user) {

    long	uid;
    char	uidstr[16];
    
    
    if (user->validated) {
	print(user, BLITZ_ALREADY_VAL);	/* already signed on */
	return;
    }
    
    if (!parse_num(user, &uid))		/* parse the uid */
	return;
	
    t_sprintf(uidstr, "#%ld", uid);	/* make uid into "name" for dnd */
    
    do_validate1(user, uidstr);		/* begin validation process */
}

/*  c_user

    Specify user name.  Set up dnd connection (a private one for
    this thread), send off the user name.  If it resolves
    uniquely, return randnum to client, and prepare for PASS or
    PASE command.
*/

void c_user(udb *user) {

    if (user->validated) {
	print(user, BLITZ_ALREADY_VAL);	/* already signed on */
	return;
    }
    
    if (!*user->comp) {			/* name present? */
	print(user, BLITZ_MISSINGARG);
	return;
    }
    
    do_validate1(user, user->comp);	/* begin validation process */
}

/* do_validate1 --

    Begin validation (called by USER and UID#).
*/

void do_validate1(udb *user, char *name) {
	
    char 	randnum[25];			 
    int		dndstat;   
		
    /* if we were already validating, forget the first dnd connection */
    if (user->dnd) {
	t_dndreset_free(user->dnd);	/* were in middle of command; reset */
	user->dnd = NULL;
	user->validating = FALSE;
    }	         
    
    /* check name, get random number */
    dndstat = t_dndval1(&user->dnd, name, val_farray, randnum);
    
    if (dndstat == DND_CONTINUE) {	/* so far so good? */
	print1(user, BLITZ_ENCRYPT, randnum); /* prompt for pw */
	user->validating = TRUE;	/* enter that state */
	user->duplicate = FALSE;
    } else if (dndstat == DND_AMBIG || dndstat == DND_VAGUE || dndstat == DND_PERM)
	print(user, BLITZ_AMBIG); /* can't read PERM when ambiguous... */
    else if (dndstat == DND_NOUSER)	/* no such user */
	print(user, BLITZ_NOUSER);
    else 				/* lost connection etc. */
	print(user, BLITZ_NODND);
    
}

/*  do_validate2 --

    Check results of DND validate call (called by PASS and PASE).
*/

void do_validate2(udb *user, char *pw) {

    int		dndstat;
    dndresult	*dndres;
    
    dndstat = t_dndval2(user->dnd, pw, val_farray, &dndres);
					 
    user->validating = FALSE;		/* no longer awaiting pw */
    t_dndfree(user->dnd);		/* free asap (avoid possible deadlock) */	
    user->dnd = NULL;
    
    if (dndstat != DND_OK) {		/* did it work? */
	if (dndstat == DND_BADPASS)
	    print(user, BLITZ_VALERR);
	else {
	    if (dndstat != DND_DOWN)	/* log odd errors to console */
		t_errprint_l("do_validate2: unexpected DND error %ld", dndstat);
	    print(user, BLITZ_NODND);
	}
	return;	
    }
    
    do_validate3(user, dndres);		/* process results from DND */
    t_free(dndres);			/* and discard them */
}

/*  do_validate3 --

    Complete validation.  Called with a dndresult array from either a DND
    validate (user/password login) or lookup (kerberos login) operation.
    The result array is processed, and the login is completed.
*/

void do_validate3(udb *user, dndresult	*dndres) {

    char	*p;
    long	serv;			/* server this user belongs on */
    mbox	*mb;
    char	buf[16];

    p = t_dndvalue(dndres, "DUP", val_farray);	/* is this user devalidated? */
    if (*p) {
	print(user, BLITZ_VALERR);	/* yes - reject them */
	return;	   
    }
    
    p = t_dndvalue(dndres, "BLITZSERV", val_farray);
    serv = blitzserv_match(p);		/* search table for server name */

    if (serv != m_thisserv) {		/* reject if not our user */
	print1(user, BLITZ_WRONGSERV, t_dndvalue(dndres, "BLITZSERV", val_farray));
	return;
    }
    
    p = t_dndvalue(dndres, "NAME", val_farray);
    user->name = mallocf(strlen(p)+1);
    strcpy(user->name, p);		/* get resolved name */

    p = t_dndvalue(dndres, "EMAIL", val_farray);
    user->email = mallocf(strlen(p)+1);
    strcpy(user->email, p);              /* email addr (possibly w/vanity domain) */
    
    /* pick up the uid */
    strtonum(t_dndvalue(dndres, "UID", val_farray), &user->uid);
    
    p = t_dndvalue(dndres, "GID", val_farray);	/* parse group list */
    user->groupcnt = 0;
    while(*p && user->groupcnt < GROUP_MAX) {	/* for all groups named */
	while (*p == ' ')		/* skip spaces */
	    ++p;
	if (*p) 			/* get another group id */
	    p = strtonum(p, &user->gid[user->groupcnt++]);
    } 
    
    /* see if they are privileged */
    p = t_dndvalue(dndres, "PERM", val_farray); /* locate permissions */
    if (strcasematch(p, "BLITZPRIV"))
	user->prived = TRUE;		/* this is privileged user */
        
    p = t_dndvalue(dndres, "BLITZINFO", val_farray); /* which fs do we have them on? */
    if (!*p)
	user->fs = -1;			/* not assigned yet */
    else {
    	if ((user->fs = fs_match(p)) == -1) {
	    print1(user, BLITZ_ERROR, " *** Invalid BLITZINFO in DND");
	    t_errprint_l("Bad BLITZINFO for uid %ld", user->uid);
	    return;
	}
    }

    mb = mbox_find(user->uid, user->fs, FALSE);/* locate/set up mailbox */
    sem_seize(&mb->mbsem);		/* synchronize duplicate check */
    if (mb->gone) {			/* box was just now transferred away */
	sem_release(&mb->mbsem);
	mbox_done(&mb);			/* forget the box */
	print1(user, BLITZ_WRONGSERV, t_dndvalue(dndres, "BLITZSERV", val_farray));
	return;
    }
    if (mb->user) {			/* is user already signed on? */
	char otherip[64];
	
	user->duplicate = TRUE;		/* PUSH is legal now */
    	pthread_mutex_lock(&inet_ntoa_lock); /* record other client's addr */
    	strcpy(otherip, inet_ntoa(mb->user->remoteaddr.sin_addr));
    	pthread_mutex_unlock(&inet_ntoa_lock);

	sem_release(&mb->mbsem);
	mbox_done(&mb);			/* forget the box */

	print1(user, BLITZ_DUPLICATE, otherip);	/* reject */
	return;
    } else {
	mb->user = user;		/* cross-link user & mb */
	user->mb = mb;
	/* no mbox_done -- leave box attached until signoff */
	sem_release(&mb->mbsem);
    }

    /* If DND expiration warning is configured, check if their account is
       about to expire and let them know */
    if (expires_defined && dndexp_warn) {
	dndexp_check(user, t_dndvalue(dndres, "EXPIRES", val_farray));
    }
    
    t_sprintf(buf, "%ld", user->uid);
    print1(user, BLITZ_VALIDATED, buf);
    
    do_signon(user,FALSE);		/* all set; finish the login sequence */
}

/* do_signon --

    Validation successful, set up the user (called by do_validate3, and PUSH).
    The first time the user signs on, deliver initial greeting message.
    Note that PREF_SESSIONID may be set (by summ_check) even if
    the user has never signed on, so a separate PREF_INITIALMESS
    is used.    
    
*/

void do_signon(udb *user, boolean_t push) {

    char	buf[MAX_STR];
    char	fromip[MAX_STR];
    
    user->validated = TRUE;		/* finally, we're signed on */
    
    strcpy(user->conn.name, user->name); /* record name in t_file, for debugging */
    
    t_sprintf(buf, "%ld", user->uid);
        
    get_vwarn(user);			/* warn of obsolete client version */
    get_pigwarn(user);			/* chastise users with too much mail */
    get_warning(user);			/* send any current global warning */
    check_newmail(user);		/* see if there is new mail */
    /* deliver initial message, if first login  */
    if (!pref_get(user->mb, PREF_INITIALMESS, buf)) {
	initialmess(user->mb, user->name, f_initialmess);
	pref_set(user->mb, PREF_INITIALMESS, "\"1\"");
    }
    
    /* set audit folder expiration if it's undefined */
    if (!pref_get(user->mb, PREF_AUTOEXPAUD, buf)) {
	strncpy_and_quote(buf, dft_auditexpire, PREF_MAXLEN);
	pref_set(user->mb, PREF_AUTOEXPAUD, buf);
    }
    /* ditto trash */
    if (!pref_get(user->mb, PREF_AUTOEXPTRASH, buf)) {
	strncpy_and_quote(buf, dft_trashexpire, PREF_MAXLEN);
	pref_set(user->mb, PREF_AUTOEXPTRASH, buf);
    }
    sem_seize(&user->mb->mbsem);
    set_sessionid(user->mb);		/* generate sessionid */
    sem_release(&user->mb->mbsem);

    pthread_mutex_lock(&inet_ntoa_lock);	/* get access to inet_ntoa */
    strcpy(fromip, inet_ntoa(user->remoteaddr.sin_addr));
    pthread_mutex_unlock(&inet_ntoa_lock);
    
    t_sprintf(buf, "Logged on: %s %ld; currently: %ld; peak: %ld from %s %s",
    			user->name, user->uid, u_num, u_hwm, fromip, (push ? "PUSH" : ""));
    log_it(buf);
    
    get_date(buf+1);
    buf[0] = '"'; strcat(buf, "\"");	/* quote for pref_set */
    pref_set(user->mb, PREF_LASTLOG, buf); /* record date/time of last login */

    /* record client addr in table of recently-used IP addresses */
    record_login(ntohl(user->remoteaddr.sin_addr.s_addr));
    
}

/* c_vdat --

    Accept vacation message text.  The text is stored in the file "vacation"
    in the mailbox directory.  PREF_VACATION is set to indicate that a
    vacation message is present.  Erase the "vacation-" file (list of
    addresses that have already received the vacation message.)
    
*/

void c_vdat(udb *user) {

    fileinfo		finfo;		/* the file */
    char		fname[MESS_NAMELEN];
    long		l;
    char		resp[32];
    
    if (!parse_num(user, &l))		/* get length */
	return;	

    /* upload data into temp file */
    strcpy(finfo.fname, user->mb->boxname);
    strcat(finfo.fname, VACATION_TEMP);
    finfo.temp = TRUE;
    
    t_sprintf(resp, "%ld", l);		/* tell client to fire away */
    print1(user, BLITZ_INTERMEDIATE, resp);
    t_fflush(&user->conn);	

    if (!chars_get(user, l, &finfo)) {	/* read specified # of chars */
	finfoclose(&finfo);		/* copy failed (conn lost?) */
	print1(user, BLITZ_ERR_BLANK, "copy failed.");
	return;
    }

    /* upload worked, save results for real */
    strcpy(fname, user->mb->boxname);
    strcat(fname, VACATION_FNAME);	
    if (rename(finfo.fname, fname) < 0) {
	t_perror1("c_vdat: rename failed: ",fname);
	print(user, BLITZ_ERROR);
	finfoclose(&finfo);		/* lose the temp */
	return;
    }
    
    pref_set(user->mb, PREF_VACATION, "\"1\""); /* indicate that file exists */
    strcpy(fname, user->mb->boxname);
    strcat(fname, VACATION_LIST);	
    
    (void) unlink(fname);		/* reset list of vacation recips */
    
    print(user, BLITZ_OK);		/* vacation set */

    finfo.temp = FALSE;			/* ok, DON'T try to remove... */
    finfoclose(&finfo);
}

/* c_vers --

    Record client software version.  There's a global table of known
    version strings (added to dynamically), instead of having a copy
    of the string for each user.  Thus, user->version should NOT be
    freed when deallocating a udb...
    
*/

void c_vers(udb *user) {

    static char	**vers_tab = NULL;	/* protected by vers_lock */
    static int	vers_count = 0;
    static int	vers_max = 0;

    int		i;
    
        
    while (isspace(*user->comp))	/* eat leading spaces */
	++user->comp;
    if (!user->comp) {			/* anything there? */
	print(user, BLITZ_MISSINGARG);
	return;
    }

    /* respond with our version string */
    print1(user, BLITZ_OK_BLANK, server_vers);
    
    pthread_mutex_lock(&vers_lock);		/* get access to static table */
    if (!vers_tab) {			/* first time, allocate table */
	vers_max = 100;
	vers_tab = mallocf(vers_max * sizeof(char *));
    }
    
    for (i = 0; i < vers_count; ++i) {
	if (strcmp(user->comp, vers_tab[i]) == 0) {
	    user->version = vers_tab[i];	/* found match; done */
	    pthread_mutex_unlock(&vers_lock);
	    return;
	}
    }
    
    /* no match; make new entry */
    if (vers_count == vers_max) {	/* grow if needed */
	vers_max += 100;
	vers_tab = reallocf(vers_tab, vers_max * sizeof(char *));
    }
    
    vers_tab[vers_count] = mallocf(MAX_STR);
    strcpy(vers_tab[vers_count], user->comp);
    user->version = vers_tab[vers_count];
    ++vers_count;
    
    pthread_mutex_unlock(&vers_lock);
    
}

/* c_vrem --

    Remove vacation message.  Clear the preference, and erase the files.	
*/

void c_vrem(udb *user) {

    char		fname[MESS_NAMELEN];

					
    if (pref_rem(user->mb, PREF_VACATION)) 	/* clear vacation setting */
	print(user, BLITZ_OK);
    else				/* pref wasn't set */
	print1(user, BLITZ_NODATA, PREF_VACATION);
	
	
    /* always try to remove files */
    strcpy(fname, user->mb->boxname);
    strcat(fname, VACATION_FNAME);	
    (void) unlink(fname);		/* remove vacation text */
    
    strcpy(fname, user->mb->boxname);
    strcat(fname, VACATION_LIST);	
    (void) unlink(fname);		/* remove list of vacation recips */
    

}

/* c_vtxt --

    Download vacation message text.
*/

void c_vtxt(udb *user) {

    char	resp[32];
    fileinfo	finfo;
    
    /* make sure message exists; check file length */
    if (!open_vacation(user->mb, &finfo)) {
	print1(user, BLITZ_NODATA, PREF_VACATION);
	return;				/* nope; never mind */
    }   
        
    /* tell client how much there is */
    t_sprintf(resp, "%ld", finfo.len);
    print1(user, BLITZ_INTERMEDIATE, resp);
    t_fflush(&user->conn);		/* send that on ahead */
    
    /* copy the vacation file */
    if (finfocopy(&user->conn, &finfo))
	print(user, BLITZ_OK);
    else
	print(user, BLITZ_ERROR);
    
}

/* c_warn --

    Send any pending warnings.
*/

void c_warn(udb *user) {

    warning		*warn;		/* copy of warning list */
    warning		*w;		/* one warning */
    boolean_t		cleared = FALSE; /* warning cleared? */
 					   
    sem_seize(&user->mb->mbsem);	/* sync w/ threads adding warnings */
    warn = user->warn;			/* pick up any pending warnings */
    user->warn = NULL;			/* and clear */
    if (warn) {				/* clear "new mail" bit; they've been told */
	if (pref_rem_int(user->mb, PREF_NEWMAIL)) { 	/* if we removed pref */
	    cleared = TRUE;
	}
    }
    
    sem_release(&user->mb->mbsem);	/* unlock box before calling notify server */
    
    /* call notification server to clear sticky notification
	(in case mail/notify clients fail to do it) */
    if (cleared)
	do_notify(user->uid, -NTYPE_MAIL, 0, "", 0, FALSE);

    if (warn) {				/* if we got any... */
	w = warn;			/* break circular list */
	warn = warn->next;		/* head */
	w->next = NULL;			/* break link from tail->head */
	
	while (warn) { 			/* run through them */
	    w = warn; warn = warn->next;
	    print(user, w->text);	/* print it */
	    t_free(w);			/* and free it */
	}
    }
    
    print(user, BLITZ_NOWARN);		/* no more to send */
}

/* force_disconnect --

    If there is an active connection for a given uid, forcibly disconnect it.
    
    --> returns locked box <--
*/

mbox *force_disconnect(long uid, int fs, udb *user) {

    mbox 	*mb;
    int		fd;
    
    for (;;) {				/* until user booted */
					
	mb = mbox_find(uid, fs, FALSE);	/* locate/set up mailbox */
	sem_seize(&mb->mbsem);		/* synchronize duplicate check */
	if (mb->user) {			/* is user already signed on? */
	    mb->user->shutdown = TRUE;	/* don't accept any more commands */
	    if ((fd = mb->user->conn.fd) >= 0) { /* if connection is open */
		pthread_mutex_lock(&sel_lock);	/* sync w/ t_select */
	    	mb->user->conn.want |= SEL_CLOSE; /* ask connection thread to close */
#ifdef T_SELECT
		pthread_cond_signal(&mb->user->conn.wait); /* nudge it */
#endif
		pthread_mutex_unlock(&sel_lock);
	    }

	    sem_release(&mb->mbsem);
	    mbox_done(&mb);		/* forget the box */
	    sleep(1);			/* wait for something to happen */
	} else {
	    if (user) {			/* iff new connection is to take over */
		mb->user = user;	/* cross-link user & mb */
		user->mb = mb;
	    }
	    /* no mbox_done -- leave box attached until signoff */
	    break;			/* ok to proceed now! */
	}    
    }
    
    return mb;				/* return w/ mb->mbsem still seized */
}    

/* pop_dele --

    Mark a message as deleted; must not be so marked already.
    
*/

void pop_dele(udb *user) {

    folder	*inbox;
    long	mindex;
    int		msgcount;
    int		max, min, mid;	/* for binary search */
    int		cmp;		/* comparison result */

    /* get argument */
    if (!popparse_num(user, &mindex))
    	return;
	
    /* check bounds */    
    sem_seize(&user->mb->mbsem);
    
    inbox = &user->mb->fold[INBOX_NUM];
    if (inbox->summs == NULL)  /* get summaries into memory (to count them) */
	summ_read(user->mb, inbox);
    msgcount = inbox->count;
    
    sem_release(&user->mb->mbsem);   
    
    if ((mindex < 1) || (mindex > msgcount)) {
    	popprint(user, POP_BADARG);
    	return;
    }
    
    /* create array if necessary */
    if (user->popdeleted == NULL) {	/* need to allocate array? */
	user->popdeleted = mallocf(POP_DELETED_CHUNK * sizeof(int));
	user->popdeletedsize = POP_DELETED_CHUNK;
	user->popdeletedcount = 0;
    }
    
    /* binary search the deleted index array */
    for (max = user->popdeletedcount-1, min = 0; max >= min; ) {
	mid = (max + min) >> 1;
	cmp = mindex - user->popdeleted[mid];
	if (cmp == 0)	/* already deleted? */
	{
	    popprint(user, POP_DELETED);
	    return;
	}
	else if (cmp > 0)
	    min = mid + 1;
	else 
	    max = mid - 1;						
    }
    
    /* need to grow array? */
    if (user->popdeletedsize == user->popdeletedcount) {
    
    	int	*temp;
	
	temp = (int *) mallocf((user->popdeletedsize + POP_DELETED_CHUNK) * sizeof(int));
	memcpy(temp, user->popdeleted, user->popdeletedsize * sizeof(int));
	t_free(user->popdeleted);
	user->popdeleted = temp;
	user->popdeletedsize += POP_DELETED_CHUNK;
    }
    
    /* insert at min (memmove because the blocks overlap) */    
    memmove(user->popdeleted + min + 1, user->popdeleted + min, 
    		(user->popdeletedcount - min) * sizeof(int));
    user->popdeleted[min] = mindex;
    user->popdeletedcount++;
    
    /* update highest message accessed */
    if (mindex > user->pophighestmsg)
    	user->pophighestmsg = (int) mindex;
    
    popprint(user, POP_OK);
}

/*  popisdeleted --

    Return whether the specified message is marked as deleted.
*/

boolean_t popisdeleted(udb *user, int mindex) {

    int		max, min, mid;	/* for binary search */
    int		cmp;		/* comparison result */

    for (max = user->popdeletedcount-1, min = 0; max >= min; ) {
	mid = (max + min) >> 1;
	cmp = mindex - user->popdeleted[mid];
	if (cmp == 0)	/* already deleted? */
	    return TRUE;
	else if (cmp > 0)
	    min = mid + 1;
	else 
	    max = mid - 1;						
    }
    
    return FALSE;
}

/* pop_list_or_uidl --

    Return either scan listing or unique id for one or all messages
    (i.e. handle either LIST or UIDL command).
    
*/

void pop_list_or_uidl(udb *user) {

    folder 	*inbox;			/* In Box folder, for convenience */
    summbuck	*p;			/* current bucket */
    summinfo	*summ;			/* current summary in bucket */
    char	*nextsum;		/* to locate next summary */
    int		i;			/* index into user->popdeleted */
    int		count;			/* summary index counter */
    long	mindex;			/* message index */
    char	buf[200];		/* buffer for building response lines */
    boolean_t	list_cmd;		/* is this LIST (as opposed to UIDL)? */
    long	poplen;			/* length returned in LIST command */
    
    list_cmd = !strncasecmp(user->comline, "LIST", 4);
    
    while (isspace(*user->comp))
	++user->comp;			/* skip leading spaces */
    
    if (isdigit(*user->comp)) {		/* message index given? */
	if (!parse_num(user, &mindex))	/* parse the message index */
	    return;
	if ((mindex < 1) || popisdeleted(user, mindex)) {
	    popprint(user, POP_BADARG);
	    return;
	}
    }
    else
    	mindex = -1;			/* -1 indicates list all msgs */
	
    inbox = &user->mb->fold[INBOX_NUM];
    
    sem_seize(&user->mb->mbsem);
    if (inbox->summs == NULL)		/* get summaries, if not yet present */
	summ_read(user->mb, inbox);
    if (mindex > inbox->count) {		/* index out of range? */
        sem_release(&user->mb->mbsem);
	popprint(user, POP_BADARG);
	return;
    }
    
    buf_init(user->mb);			/* buffer up output until box unlocked */
    
    if (mindex < 0) {
    	buf_putl(user->mb, POP_OK);	/* buffer up positive response (for multi-line) */
    	/* t_errprint_s("POP server said: %s", POP_OK); */
    }
    
    count = 1;				/* tracks present summ position */
    i = 0;				/* present user->popdeleted position */
    for (p = inbox->summs; p != NULL; p = p->next) {
	for (nextsum = p->data; nextsum - p->data < p->used; nextsum += summ->len) {
	    summ = (summinfo *) nextsum;
	    /* always return a length > 0 to avoid divide-by-zero client bugs */
	    poplen = summ->totallen > 0 ? summ->totallen : 1; 
	    if ((i < user->popdeletedcount) && (user->popdeleted[i] == count))
		i++;				/* marked for deletion?  then skip */
	    else if (mindex < 0) {		/* printing all? */
	    	t_sprintf(buf, "%d %ld", count, list_cmd ? poplen : summ->messid);
		buf_putl(user->mb, buf);
                /* t_errprint_s("POP server said: %s", buf); */
	    }
	    else if (count == mindex) {		/* just this one on response line? */
	    	t_sprintf(buf, "%s %d %ld", POP_OK, count, list_cmd ? poplen : summ->messid);
		buf_putl(user->mb, buf);
                /* t_errprint_s("POP server said: %s", buf); */
	    	goto loopexit;			/* did our one scan listing, bail out */
	    }
	    
	    count++;
	}
    }
    
loopexit:
    sem_release(&user->mb->mbsem);
    
    if (mindex < 0) {			/* need end-of-data for multi-line? */
    	buf_putl(user->mb, POP_ENDOFDATA);
    	/* t_errprint_s("POP server said: %s", POP_ENDOFDATA); */
    }

    /* send the buffered responses */
    buf_flush(user->mb);
}

/* pop_last --

    Return highest message index accessed.
    
*/

void pop_last(udb *user) {

    char	num[10];
	
    t_sprintf(num, "%d", user->pophighestmsg);
    popprint1(user, POP_OK_BLANK, num);
}

/* pop_noop --

    Do nothing.
    
*/

void pop_noop(udb *user) {

	popprint(user, POP_OK);
}

/* popdo_validate1 --

    Begin POP validation (called by USER).
*/

void popdo_validate1(udb *user, char *name) {
	
    char 	randnum[25];			 
    int		dndstat;   
		
    /* if we were already validating, forget the first dnd connection */
    if (user->dnd) {
	t_dndreset_free(user->dnd);	/* were in middle of command; reset */
	user->dnd = NULL;
	user->validating = FALSE;
    }	         
    
    /* check name, get random number */
    dndstat = t_dndval1(&user->dnd, name, val_farray, randnum);
    
    if (dndstat == DND_CONTINUE) {	/* so far so good? */
	popprint(user, POP_OK_SENDPASS); /* prompt for pw */
	user->validating = TRUE;	/* enter that state */
	user->duplicate = FALSE;
    } else if (dndstat == DND_AMBIG || dndstat == DND_VAGUE || dndstat == DND_PERM)
	popprint(user, POP_AMBIG); /* can't read PERM when ambiguous... */
    else if (dndstat == DND_NOUSER)	/* no such user */
	popprint(user, POP_NOUSER);
    else 				/* lost connection etc. */
	popprint(user, POP_NODND);
    
}

/*  popdo_validate2 --

    Complete POP validation (called by PASS).
*/

void popdo_validate2(udb *user, char *pw) {

    int		dndstat;
    dndresult	*dndres;
    
    dndstat = t_dndval2(user->dnd, pw, val_farray, &dndres);
					 
    user->validating = FALSE;		/* no longer awaiting pw */
    t_dndfree(user->dnd);		/* free asap (avoid possible deadlock) */	
    user->dnd = NULL;
    
    if (dndstat != DND_OK) {		/* did it work? */
	if (dndstat == DND_BADPASS)
	    popprint(user, POP_VALERR);
	else {
	    if (dndstat != DND_DOWN)	/* log odd errors to console */
		t_errprint_l("do_validate2: unexpected DND error %ld", dndstat);
	    popprint(user, POP_NODND);
	}
	return;	
    }
    
    popdo_validate3(user, dndres);	/* process results from DND */
    t_free(dndres);			/* and discard them */

}

/*  popdo_validate3 --

    Complete validation.  Called with a dndresult array from either a DND
    validate (user/password login) or lookup (kerberos login) operation.
    The result array is processed, and the login is completed.
*/

void popdo_validate3(udb *user, dndresult *dndres) {

    char	*p;
    long	serv;			/* server this user belongs on */
    mbox	*mb;

    p = t_dndvalue(dndres, "DUP", val_farray);	/* is this user devalidated? */
    if (*p) {
	print(user, POP_VALERR);	/* yes - reject them */
	return;	   
    }
    
    p = t_dndvalue(dndres, "BLITZSERV", val_farray);
    serv = blitzserv_match(p);		/* search table for server name */

    if (serv != m_thisserv) {		/* reject if not our user */
	popprint1(user, POP_WRONGSERV, t_dndvalue(dndres, "BLITZSERV", val_farray));
	return;
    }
    
    p = t_dndvalue(dndres, "NAME", val_farray);
    user->name = mallocf(strlen(p)+1);
    strcpy(user->name, p);		/* get resolved name */

    p = t_dndvalue(dndres, "EMAIL", val_farray);
    user->email = mallocf(strlen(p)+1);
    strcpy(user->email, p);              /* email addr (possibly w/vanity domain) */

    /* pick up the uid */
    strtonum(t_dndvalue(dndres, "UID", val_farray), &user->uid);
    
    p = t_dndvalue(dndres, "GID", val_farray);	/* parse group list */
    user->groupcnt = 0;
    while(*p) {				/* for all groups named */
	while (*p == ' ')		/* skip spaces */
	    ++p;
	if (*p) 			/* get another group id */
	    p = strtonum(p, &user->gid[user->groupcnt++]);
    } 
    
    /* see if they are privileged */
    p = t_dndvalue(dndres, "PERM", val_farray); /* locate permissions */
    if (strcasematch(p, "BLITZPRIV"))
	user->prived = TRUE;		/* this is privileged user */
        
    p = t_dndvalue(dndres, "BLITZINFO", val_farray); /* which fs do we have them on? */
    if (!*p)
	user->fs = -1;			/* not assigned yet */
    else {
    	if ((user->fs = fs_match(p)) == -1) {
	    popprint1(user, POP_ERROR, " *** Invalid BLITZINFO in DND");
	    t_errprint_l("Bad BLITZINFO for uid %ld", user->uid);
	    return;
	}
    }

    mb = mbox_find(user->uid, user->fs, FALSE);/* locate/set up mailbox */
    sem_seize(&mb->mbsem);		/* synchronize duplicate check */
    if (mb->gone) {			/* box was just now transferred away */
	sem_release(&mb->mbsem);
	mbox_done(&mb);			/* forget the box */
	popprint1(user, POP_WRONGSERV, t_dndvalue(dndres, "BLITZSERV", val_farray));
	return;
    }
    if (mb->user) {			/* is user already signed on? */
	sem_release(&mb->mbsem);
	mbox_done(&mb);			/* forget the box */
	popprint(user, POP_BOXLOCKED);	/* reject */
	return;
    } else {
	mb->user = user;		/* cross-link user & mb */
	user->mb = mb;
	/* no mbox_done -- leave box attached until signoff */
	sem_release(&mb->mbsem);
    }

    popprint1(user, POP_VALIDATED, user->name);
    
    popdo_signon(user);			/* all set; finish the login sequence */
}

/* popdo_signon --

    Validation successful, set up the user (called by popdo_validate2).
    The first time the user signs on, deliver initial greeting message.
    Note that PREF_SESSIONID may be set (by summ_check) even if
    the user has never signed on, so a separate PREF_INITIALMESS
    is used.    
    
*/

void popdo_signon(udb *user) {

    char	buf[MAX_STR];
    char	fromip[MAX_STR];
    
    user->validated = TRUE;		/* finally, we're signed on */
    
    strcpy(user->conn.name, user->name); /* record name in t_file, for debugging */
    
    t_sprintf(buf, "%ld", user->uid);
        
    /* deliver initial message, if first login  */
    if (!pref_get(user->mb, PREF_INITIALMESS, buf)) {
	initialmess(user->mb, user->name, f_initialmess);
	pref_set(user->mb, PREF_INITIALMESS, "\"1\"");
    }
    
    /* set audit folder expiration if it's undefined */
    if (!pref_get(user->mb, PREF_AUTOEXPAUD, buf)) {
	strncpy_and_quote(buf, dft_auditexpire, PREF_MAXLEN);
	pref_set(user->mb, PREF_AUTOEXPAUD, buf);
    }
    sem_seize(&user->mb->mbsem);
    set_sessionid(user->mb);		/* generate sessionid */
    sem_release(&user->mb->mbsem);

    pthread_mutex_lock(&inet_ntoa_lock);/* get access to inet_ntoa */
    strcpy(fromip, inet_ntoa(user->remoteaddr.sin_addr));
    pthread_mutex_unlock(&inet_ntoa_lock);
    
    t_sprintf(buf, "POP Logged on: %s %ld; currently: %ld; peak: %ld from %s",
    			user->name, user->uid, u_num, u_hwm, fromip);
    log_it(buf);
    
    get_date(buf+1);
    buf[0] = '"'; strcat(buf, "\"");	/* quote for pref_set */
    pref_set(user->mb, PREF_LASTLOG, buf); /* record date/time of last login */

    /* record client addr in table of recently-used IP addresses */
    record_login(ntohl(user->remoteaddr.sin_addr.s_addr));
    
}

/* pop_pass --

    Cleartext POP pw (too bad we can't do APOP).  We should hand-encrypt,
    but we're too lazy.
    
    For Kerberos, the actual password is irrelevant, but we must check the
    name from the ticket (stashed in user->name with the ticket was decoded)
    against the name given in the USER command (uid stashed in user->uid).
*/

void pop_pass(udb *user) {

    long	ticket_uid;		/* uid from Kerberos ticket */
    dndresult	*dndres;		/* dnd lookup results */	
    int		dndstat;   		/* and status */
    
    if (user->validated) {
	popprint(user, POP_SEQERROR);	/* already signed on */
	return;
    }
    
   if (!user->validating) {
	popprint(user, POP_NO_USER_YET);
	return;
    }
    user->validating = FALSE;		/* no longer awaiting pw */

    /* if we've already been given a Kerberos ticket, the password is irrelevant.
       Just verify that the name in the USER command matches the ticket and
       proceed from there.
    */
    if (user->krbvalidated) {	
	/* look ticket name up in DND for other info */
	dndstat = t_dndlookup1(user->name, val_farray, &dndres);
    
	t_free(user->name);		/* done with stashed ticket name */
	user->name = NULL;
	
	if (dndstat != DND_OK) {	/* did it work? */
	    if (dndstat != DND_DOWN)	/* log odd errors to console */
		t_errprint_l("pop_pass: unexpected DND error %ld", dndstat);
	    popprint(user, POP_NODND);
	    return;    	
	}
	strtonum(t_dndvalue(dndres, "UID", val_farray), &ticket_uid);
	if (ticket_uid != user->uid) {	/* ticket must match USER command */
	    t_free(dndres);		/* clean up */
	    popprint1(user, POP_KRBERR, "wrong name in ticket");
	    return;
	}
	popdo_validate3(user, dndres);	/* process results from DND */
	t_free(dndres);			/* and discard them */
    	
    } else {
	popdo_validate2(user, user->comp);	/* rest of line is pw */
    }
}

/* pop_quit --

    Move deleted messages to trash (if validated) and mark user as going away.
    
*/

void pop_quit(udb *user) {

    folder 	*inbox;			/* In Box folder, for convenience */
    folder	*trash;			/* Trash folder, for convenience */
    summbuck	*p;			/* current bucket */
    summinfo	*summ;			/* current summary in bucket */
    char	*nextsum;		/* to locate next summary */
    long	*idstomove;		/* array of In Box mess ids to move */
    long	newexp;			/* new expiration date for msgs */
    int		count;			/* 1-based summary index */
    int		i;			/* index into user->popdeleted or idstomove */

    if (user->validated && (user->popdeletedcount > 0))
    {
    	/* we need to move marked message to the Trash */
	
	/* turn summary indices into message ids */
	inbox = &user->mb->fold[INBOX_NUM];
	trash = &user->mb->fold[TRASH_NUM];
	idstomove = (long *) mallocf(user->popdeletedcount * sizeof(long));
	
	sem_seize(&user->mb->mbsem);
	if (inbox->summs == NULL)		/* get summaries, if not yet present */
	    summ_read(user->mb, inbox);
	    
	count = 1;				/* tracks present summ position */
    	i = 0;					/* present user->popdeleted position */
	for (p = inbox->summs; (p != NULL) && (i < user->popdeletedcount); p = p->next) {
	    for (nextsum = p->data; (nextsum - p->data < p->used) && (i < user->popdeletedcount); 
			nextsum += summ->len) {
		summ = (summinfo *) nextsum;
		if (user->popdeleted[i] == count) {
		    /* one of the messages to move */
		    idstomove[i++] = summ->messid;
		}
		count++;
   	    }
	}
	sem_release(&user->mb->mbsem);

	/* set a short expiration for messages deleted by POP users */
    	newexp = add_days(POP_TRASHEXPIRE);

	/* actually move the messages */
	for (i = 0; i < user->popdeletedcount; i++)
		(void) summ_move(user->mb, inbox, trash, idstomove[i], newexp);
		
	/* clean up */
	t_free(idstomove);
	t_free(user->popdeleted);
	user->popdeleted = NULL;
	user->popdeletedcount = 0;
	user->popdeletedsize = 0;
    } /* user in TRANSACTION state */
    
    popprint(user, POP_OK);
    user->shutdown = TRUE;
}

/* pop_retr_or_top --

    Return first n lines of a message, or all if no n is specified.
    Use for both RETR and TOP commands, out of laziness.
        
*/

void pop_retr_or_top(udb *user) {

    folder 	*inbox;			/* In Box folder, for convenience */
    summbuck	*p;			/* current bucket */
    summinfo	*summ;			/* current summary in bucket */
    char	*nextsum;		/* to locate next summary */
    long	mindex;			/* message index */
    long	messid;			/* message id */
    long	lines;			/* number of lines to return, or -1 */
    fileinfo	textmess;    		/* textified version of message */
    t_file	*textf;			/* ditto */
    int		count;			/* summary index counter */
    long	linecounter;		/* for counting body lines */
    char	buf[MAX_STR];		/* for writing out message a line at a time */
    boolean_t	msgread = FALSE;	/* had this message been read before? */
    boolean_t	inheader = FALSE;	/* still writing out header? */
    
    if (!parse_num(user, &mindex))	/* parse the message index */
    	return;
    if ((mindex < 1) || popisdeleted(user, mindex)) {
	popprint(user, POP_BADARG);
	return;
    }

    while (isspace(*user->comp))
	++user->comp;			/* skip leading spaces */
    
    if (isdigit(*user->comp)) {		/* number of lines given? */
	if (!parse_num(user, &lines))	/* parse the line count */
	    return;
    }
    else
    	lines = -1;			/* -1 indicates list entire message */
	
    inbox = &user->mb->fold[INBOX_NUM];
    
    sem_seize(&user->mb->mbsem);
    if (inbox->summs == NULL)		/* get summaries, if not yet present */
	summ_read(user->mb, inbox);
    if (mindex > inbox->count) {		/* index out of range? */
        sem_release(&user->mb->mbsem);
	popprint(user, POP_BADARG);
	return;
    }
    
    count = 1;				/* tracks present summ position */
    for (p = inbox->summs; p != NULL; p = p->next) {
	for (nextsum = p->data; nextsum - p->data < p->used; nextsum += summ->len) {
	    summ = (summinfo *) nextsum;
	    if (count == mindex) {
	        messid = summ->messid;
		msgread = summ->read;
	    	goto loopexit;		/* did our one scan listing, bail out */
	    }
	    count++;
	}
    }
    sem_release(&user->mb->mbsem);	/* did we not find summary? weird... */
    popprint(user, POP_BADARG);
    return;
    
loopexit:
    sem_release(&user->mb->mbsem);
    
    /* set current message */
    
    finfoclose(&user->head);		/* clean up old current message */
    finfoclose(&user->text);
    clean_encl_list(&user->ep);
    user->summ.messid = -1;		/* old summary info not valid */
    user->summ.enclosures = 0;
    user->currmessfold = -1;
    
    if (!mess_get(user, &inbox, messid)) {/* open message, fill in udb fields */
	popprint(user, POP_BADARG);
        return;
    }
    
    /* convert to RFC822 format file (with \n line endings, no enclosures) */
    
    textf = exportmess(messid, &user->head, &user->text, &textmess, user->summ.type);
    if (!textf) {
    	popprint(user, POP_ERROR);
	return;
    }
    
    
    /* If there are enclosures, add them back to text (as BinHex) */
      
   if (user->ep) { 			/* iff enclosures */
   	if (!pop_expand_encl(messid, &textmess, &textf, user->ep)) {
	    popprint(user, POP_ERROR);
	    return;
	}
    }

    popprint(user, POP_OK);			/* positive response */
   
    inheader = TRUE;
    linecounter = 0;
    (void) t_fseek(textf, 0, SEEK_SET);		/* go to beginning */
    while(t_gets(buf, sizeof(buf), textf)) { 	/* read each line */
	if (!buf[0] && inheader) {		/* end of header? */
	    inheader = FALSE;			/* not in the header anymore */
	    if (msgread) {
		t_fprintf(&user->conn, "Status: RO\r\n");	/* pretend we're a Unix spool file */
    		/* t_errprint_s("POP server said: %s", "Status: RO"); */
	    }
	} 

	if (!inheader) {
	    linecounter++;
	    if ((lines != -1) 			/* unless retrieving whole message */
	    		&& (lines < linecounter-1))	/* are we done? */
		break;				/* note: -1 'cause 1st blank line doesn't count */
	}
		
	if (buf[0] == '.')
	    t_putc(&user->conn, '.');		/* escape initial dots */
	t_fprintf(&user->conn, "%s\r\n", buf);	/* use CRLF terminators */
    	/* t_errprint_s("POP server said: %s", buf); */	
    }
    
    popprint(user, POP_ENDOFDATA);	/* we're done */
    
    /* update highest message accessed */
    if (mindex > user->pophighestmsg)
    	user->pophighestmsg = (int) mindex;
    
    finfoclose(&textmess);		/* dispose of our copy of the message */
    t_fclose(textf);

    /* mark as read (if it was RETR command) */
    if (!strncasecmp(user->comline, "RETR", 4))	/* it was RETR command */
    {
	if (user->summ.messid >= 0) {	/* if text is part of message in box */
	    sem_seize(&user->mb->mbsem);	/* must lock for get_summ */
	    if (summ = get_summ(user->mb, user->summ.messid, &inbox)) { /* (sic) */
		if (!summ->read) {		/* was it read? */
		    touch_folder(user->mb, inbox); /* modifying folder; flush caches */
		    sem_release(&user->mb->mbsem); /* unlock, in case do_receipt locks */
		    summ->read = TRUE;	/* it is now */
		    inbox->dirty = TRUE;	/* folder has changed */
		    if (summ->receipt)	/* receipt requested? */
			do_receipt(user->name, summ, &user->head);
		} else
		    sem_release(&user->mb->mbsem);
	    } else
		sem_release(&user->mb->mbsem);
	}
    }
}

/* pop_rset --

    Un-mark any messages marked as deleted, and set the highest index
    accessed to zero.
    
*/

void pop_rset(udb *user) {

    if (user->popdeleted)		/* wipe out record of deleted msgs */
    {
    	t_free(user->popdeleted);
	user->popdeleted = NULL;
    }
    user->popdeletedsize = 0;
    user->popdeletedcount = 0;
    
    user->pophighestmsg = 0;		/* zero highest-msg-index-accessed */
    
    popprint(user, POP_OK);
}

/* pop_stat --

    Print a POP "drop listing" for the In Box folder.  Has side effect of clearing
    the new-mail flag and any warnings (code taken from c_warn).
    
*/

void pop_stat(udb *user) {

    folder		*inbox;
    char		buf[100];
    warning		*warn;		/* copy of warning list */
    warning		*w;		/* one warning */
    boolean_t		cleared = FALSE; /* warning cleared? */
 					   
    sem_seize(&user->mb->mbsem);
    
    inbox = &user->mb->fold[INBOX_NUM];
    if (inbox->summs == NULL)  /* get summaries into memory (to count them) */
	summ_read(user->mb, inbox);
    t_sprintf(buf, "%d %ld", inbox->count, inbox->foldlen);
    
    sem_release(&user->mb->mbsem);   
    
    popprint1(user, POP_OK_BLANK, buf);

    sem_seize(&user->mb->mbsem);	/* sync w/ threads adding warnings */
    warn = user->warn;			/* pick up any pending warnings */
    user->warn = NULL;			/* and clear */
    
    /* clear "new mail" bit (regardless of whether or not warning is
       pending; POP clients don't see warnings) */
    if (pref_rem_int(user->mb, PREF_NEWMAIL)) { 	/* if we removed pref */
	cleared = TRUE;
    }
    
    sem_release(&user->mb->mbsem);	/* unlock box before calling notify server */
    
    /* call notification server to clear sticky notification
	(in case mail/notify clients fail to do it) */
    if (cleared)
	do_notify(user->uid, -NTYPE_MAIL, 0, "", 0, FALSE);

    if (warn) {				/* if we got any... */
	w = warn;			/* break circular list */
	warn = warn->next;		/* head */
	w->next = NULL;			/* break link from tail->head */
	
	while (warn) { 			/* run through them */
	    w = warn; warn = warn->next;
	    t_free(w);			/* and free it */
	}
    }
}

/*  pop_user

    Specify user name.  Set up dnd connection (a private one for
    this thread), send off the user name.  If it resolves
    uniquely, return randnum to client, and prepare for PASS or
    PASE command.
*/

void pop_user(udb *user) {

    int		dndstat;		/* lookup status */
    
    if (user->validated) {
	popprint(user, POP_SEQERROR);	/* already signed on */
	return;
    }
    
    if (!*user->comp) {			/* name present? */
	popprint(user, POP_MISSINGARG);
	return;
    }
    
    if (user->krbvalidated) {		/* Kerberos ticket already here? */
	user->uid = name_to_uid(user->comp, &dndstat);	/* resolve name to get uid */
	if (user->uid < 0) {
	    if (dndstat == DND_AMBIG || dndstat == DND_VAGUE)
	    	popprint(user, POP_AMBIG);
	    else if (dndstat == DND_NOUSER)	/* no such user */
		popprint(user, POP_NOUSER);
	    else 				/* lost connection etc. */
		popprint(user, POP_NODND);
	    return;
	}
	/* uid will be compared against ticket when PASS cmd given */
	user->validating = TRUE;	/* enter validating state */
	user->duplicate = FALSE;
	popprint(user, POP_OK_SENDPASS); /* prompt for pw */
    } else {				/* not Kerberos */
	popdo_validate1(user, user->comp);	/* begin validation process */
    }
}

/* poppassd --

    Thread to read and process commands for a poppassd client (i.e. a POP
    client using Steve Dorner's protocol for changing a password on a
    POP server).
    
    The protocol:
	S: 200 hello\r\n
	E: user yourloginname\r\n
	S: 300 please send your password now\r\n
	E: pass yourcurrentpassword\r\n
	S: 200 My, that was tasty\r\n
	E: newpass yournewpassword\r\n
	S: 200 Happy to oblige\r\n
	E: quit\r\n
	S: 200 Bye-bye\r\n
	S: <closes connection>
	E: <closes connection>
    
*/

any_t poppassd(any_t conn_) {

    char	name[MAX_STR], pass[MAX_STR];	/* name and password */
    char	newpass[MAX_STR];	/* new password */
    char	line[MAX_STR];		/* command line */
    t_file	*conn;			/* the connection */
    int		sta;			/* DND status */
    
    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
    
    conn = (t_file *) conn_;		/* argument is connection */
    
    t_fprintf(conn, "200 Welcome to the BlitzMail POP password-changing server.\r\n");
    t_fflush(conn);			/* flush output, if connected */
    t_fseek(conn, 0, SEEK_CUR); 	/* seek to set up to read again */
    
    t_gets(line, MAX_STR, conn);	/* get user command */
    if (strncasecmp(line, "USER ", 5))
    {
    	t_fprintf(conn, "500 Error: you were supposed to say USER.\r\n");
	t_fflush(conn);			/* flush output, if connected */
    	goto exit;
    }
    strcpy(name, line + 5);		/* keep copy of user name */
    t_fprintf(conn, "200 Now send PASS command with current password.\r\n");
    t_fflush(conn);			/* flush output, if connected */
    t_fseek(conn, 0, SEEK_CUR); 	/* seek to set up to read again */
    
    t_gets(line, MAX_STR, conn);	/* get pass command */
    if (strncasecmp(line, "PASS ", 5))
    {
    	t_fprintf(conn, "500 Error: you were supposed to say PASS.\r\n");
	t_fflush(conn);			/* flush output, if connected */
    	goto exit;
    }
    strcpy(pass, line + 5);		/* keep copy of password */
    t_fprintf(conn, "200 Now send NEWPASS command with new password.\r\n");
    t_fflush(conn);			/* flush output, if connected */
    t_fseek(conn, 0, SEEK_CUR); 	/* seek to set up to read again */
    
    t_gets(line, MAX_STR, conn);	/* get newpass command */
    if (strncasecmp(line, "NEWPASS ", 8))
    {
    	t_fprintf(conn, "500 Error: you were supposed to say NEWPASS.\r\n");
	t_fflush(conn);			/* flush output, if connected */
    	goto exit;
    }
    strcpy(newpass, line + 8);		/* keep copy of new password */
    pad_pw(pass);		/* zero-pad passwords */
    pad_pw(newpass);
    sta = change_password(name, (u_char *) pass, (u_char *) newpass, FALSE);
    if (sta == DND_OK)
    	t_fprintf(conn, "200 Password changed.\r\n");
    else
    	t_fprintf(conn, "500 DND error: %d\r\n", sta);
    t_fflush(conn);			/* flush output, if connected */
    
exit:
    if (conn->fd >= 0)
	t_closefd(conn->fd);		/* close file & remove from fd table */  
    t_free(conn);			/* free connection block */   
            
    return 0;				/* thread dies */
}

/* pop_expand_encl --

    Turn blitz-format enclosures back into BinHex for download to POP client.

    Make a pass over the file inserting the BinHex in an appropriate place. 
    If the original message had MIME headers, reinsert the BinHex following 
    the applicable Content-Type: line. Otherwise, just append it to the end.
*/

boolean_t pop_expand_encl(long messid, fileinfo *intext, t_file **inf, enclinfo *ep) {

    enclinfo	*curencl;		/* current enclosure */
    boolean_t	inheader = FALSE;	/* still writing out header? */
    boolean_t	ismime = FALSE;		/* a MIME message? */
    boolean_t	bhexpart = FALSE;	/* is this an application/mac-binhex40 part? */
    char	buf[MAX_STR];		/* for writing out message a line at a time */
    fileinfo	outtext;		/* output message */
    t_file	*outf;			/* and corresponding file */
    static int	serial = 1;		/* temp file discriminator */
    		
    mess_tmpname(outtext.fname, m_spool_filesys, messid);	
    strcat(outtext.fname, "exp-pop");	/* base tempname on message id */
    pthread_mutex_lock(&global_lock);	/* ...adding serial number in case 2 threads... */
    numtostr(serial++, outtext.fname + strlen(outtext.fname)); /* ... are using same mess */
    pthread_mutex_unlock(&global_lock);
    
    if ((outf = t_fopen(outtext.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	return FALSE;
    }
    
    outtext.temp = TRUE;			/* unlink this file when done */
    outtext.offset = 0;

    curencl = ep;				/* get head of enclosure list */
    inheader = TRUE;
    (void) t_fseek(*inf, 0, SEEK_SET);		/* go to beginning */
    while(t_gets(buf, sizeof(buf), *inf)) { 	/* read each line */
	if (!buf[0] && inheader) {		/* end of header? */
	    inheader = FALSE;			/* not in the header anymore */
	} 
	else if (inheader && !strncasecmp(buf, MIME_VERSION_HEADER, strlen(MIME_VERSION_HEADER)))
		ismime = TRUE;			/* this is a MIME message */
	else if (!inheader && ismime && !strncasecmp(buf, CONTENT_TYPE_BINHEX, strlen(CONTENT_TYPE_BINHEX)))
		bhexpart = TRUE;
		
	if (buf[0] == '.')
	    t_putc(outf, '.');			/* escape initial dots */
	t_fprintf(outf, "%s\r\n", buf);		/* use CRLF terminators */
	
	if (bhexpart && !buf[0] && curencl) {	/* time to spit out an enclosure? */
	    (void) binhex(curencl, outf, TRUE);	/* convert to binhex and output */
	    curencl = curencl->next;		/* move to next enclosure */
	    bhexpart = FALSE;
	}
    }
    
    while (curencl) {				/* spit out any remaining enclosures */
	t_fprintf(outf, "\r\n");		/* blank line */
	(void) binhex(curencl, outf, TRUE);	/* convert to binhex and output */
	curencl = curencl->next;		/* move to next enclosure */
    }
    
    /* message copied; close old copy & return new */
    
    finfoclose(intext);				/* dispose of input copy */
    t_fclose(*inf);
	
    *intext = outtext;				/* give caller the output copy */
    *inf = outf;
    
    return TRUE;
} 

/* record_login --

   Record client IP address and current time in a hash table. This table is 
   searched by recent_login() when it wants to see if a given IP address has
   been used by a client "recently".

   The table is hashed on the low-order byte of the IP address; each element
   of the hash table points to a linked list of blocks (each containing
   LOGIN_INFO_BLOCKSIZE entries). Entries older than g_recent_login_limit
   are no longer "recent", and may be overwritten if needed.
*/

void record_login(u_bit32 addr) {

    int		hash = LOGIN_HASH(addr);	/* hash entry */
    login_info	*hole = NULL;		/* hole we can overwrite */
    login_info	*us = NULL;		/* entry to fill in */
    login_info_block *buck;		/* hash table bucket */
    u_bit32	now = mactime();	/* current time */
    int		i;

    sem_seize(&smtp_filt_sem);

    /* search for existing entry; also remember first hole seen */
    for (buck = login_tab[hash]; buck != NULL; buck = buck->next) {
	/* search this bucket */
	for (i = 0; i < LOGIN_INFO_BLOCKSIZE; ++i) {
	   if (buck->entry[i].where == addr) {
		us = &buck->entry[i];	/* matched addr; done */
		break;
	   } else if (buck->entry[i].when + g_recent_login_limit < now
		  && hole == NULL) {
	        hole = &buck->entry[i]; /* remember hole; keep searching */
	   }
	}
	if (us != NULL)
	   break;
    }

    /* no match, no hole - need to allocate new block */
    if (us == NULL && hole == NULL) {
	/* allocate bucket; initialize all entries */
	buck = mallocf(sizeof (login_info_block));
        pthread_mutex_lock(&global_lock);
        ++malloc_stats.login_blocks;  /* stats: count allocations */
        pthread_mutex_unlock(&global_lock);

	for (i = 0; i < LOGIN_INFO_BLOCKSIZE; ++i) {
	    buck->entry[i].where = 0;
	    buck->entry[i].when = 0; 
	}
 	/* link into hash table */
	buck->next = login_tab[hash];
	login_tab[hash] = buck;
	hole = &buck->entry[0];		/* use this entry */
    }

    if (us == NULL)
	us = hole;			/* no match; fill in hole */

    us->where = addr;			/* record addr & time */
    us->when  = now;

    sem_release(&smtp_filt_sem);
}
/* recent_login --

   Search login table to see if this IP address has logged in "recently".

*/

boolean_t recent_login(u_bit32 addr) {


    int         hash = LOGIN_HASH(addr);        /* hash entry */
    login_info_block *buck;             /* hash table bucket */
    u_bit32     now = mactime();        /* current time */
    int		i;


    sem_check(&smtp_filt_sem);		/* make sure table locked */

    /* search for matching entry; check time */

    for (buck = login_tab[hash]; buck != NULL; buck = buck->next) {
        /* search this bucket */
        for (i = 0; i < LOGIN_INFO_BLOCKSIZE; ++i) {
           if (buck->entry[i].where == addr) {
		if (buck->entry[i].when + g_recent_login_limit > now)
		    return TRUE;	/* yes! */
		else
		    return FALSE;	/* matched; but not recent */
	   }
	}
    }
    return FALSE;			/* not found */
}
