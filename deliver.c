/*  Mach BlitzMail Server -- delivery routines

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/deliver.c,v 3.6 98/10/21 16:04:24 davidg Exp Locker: davidg $
    
    Messages waiting to be delivered are queued in a spool directory; there's
    a control file that gives the recipient information, plus the (link to)
    the message file itself.  There are actually a number of spool directories:
    one for each of the peer servers, including one for ourself.  Processing
    of the various spool directories can thus be done in parallel, so if one
    recipient of the message is on an unreachable server other recipients
    aren't delayed.
        
    When a message is ready to be delivered (either a locally generated message
    being sent or a newly-received remote message), the recipient list is
    partitioned according to destination routing, and the appropriate control
    files and spool directory links are created.  The thread servicing the
    spool directory is then notified that a message awaits delivery.  The delivery
    is accomplished by the "outqueue_read" thread (as soon as the peer connection
    is idle).  Deliveries on this server are done immediately (by calling 
    "localdeliver"); this avoids the effort of queueing the message.
        
    Messages going to foreign (non-blitz) hosts are not queued by us (that would 
    be redundant); they are handled by "internet", which simply forks off a 
    standard sendmail process to enter the message into sendmail's outgoing queue
    (or uses our internal SMTP queue if we're configured to do outgoing SMTP directly).
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <netdb.h>
#include <errno.h>
#include <sysexits.h>
#include <sys/dir.h>
#include <netinet/in.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "mess.h"
#include "deliver.h"
#include "client.h"
#include "queue.h"
#include "binhex.h"
#include "ddp.h"
#include "t_dnd.h"
#include "notify/not_types.h"
#include "notify/notify.h"

void internet_send(char *sender, recip *rlist1, recip *rlist2, fileinfo *textmess, 
	t_file *textf, fileinfo *head, fileinfo *text, summinfo *summ);
void internet_queue(char *sender, recip *rlist1, recip *rlist2, fileinfo *textmess, 
		t_file *textf, fileinfo *head, fileinfo *text, summinfo *summ);
void alldeliver(fileinfo *head, fileinfo *text, enclinfo *encl, summinfo *summ);
any_t alldel_thread(any_t pb_);
boolean_t get_head(char *sender, recip *tolist, recip *cclist, recip *bcclist, 
	fileinfo *head, summinfo *summ, char *replyto, fileinfo *contenthead);
void getnames(t_file *f, char *label, recip *recipl);
void find_text_encl(enclinfo *encl, enclinfo **text_encl, enclinfo **nontext_encl,
	boolean_t hextext);
void add_text_encl(t_file *text, enclinfo *encl, boolean_t dowrap);
t_file *find_data_fork(fileinfo *text_encl, fileinfo *dinfo);
void binhex_enclosures(char *sender, recip *rlist1, recip *rlist2, fileinfo *head, 
		fileinfo *text, enclinfo *text_encl, enclinfo *nontext_encl, summinfo *summ);
t_file *exportmess(long messid, fileinfo *head, fileinfo *text, fileinfo *textmess, long mtype);
void exportaddr(t_file *out, char *addr, int *linepos, boolean_t comma);
boolean_t get_vacation_from(fileinfo *head, char *send_addr);
void send_vacation(char *send_addr, recip *send_rlist, recip *r);
void wrap(fileinfo *finfo, t_file *in, t_file *out);

/* Translation table for mac special chars
	- note that index is unsigned (u_char) -
*/

char *mac_char_map[256] = {
	/* $00-$7F -- normal ascii; delete control chars (except tab)wrap */
	/* $00-$07 */ "", "", "", "", "", "", "", "", 
	/* $08-$0F */ "", "\t", "", "", "", "", "", "",
	/* $10-$17 */ "", "", "", "", "", "", "", "",
	/* $18-$1F */ "", "", "", "", "", "", "", "",	
	/* $20-$27 */ " ", "!", "\"", "#", "$", "%", "&", "'",
	/* $28-$2F */ "(", ")", "*", "+", ",", "-", ".", "/",
	/* $30-$37 */ "0", "1", "2", "3", "4", "5", "6", "7",
	/* $38-$3F */ "8", "9", ":", ";", "<", "=", ">", "?",
	/* $40-$47 */ "@", "A", "B", "C", "D", "E", "F", "G",
	/* $48-$4F */ "H", "I", "J", "K", "L", "M", "N", "O",
	/* $50-$57 */ "P", "Q", "R", "S", "T", "U", "V", "W",
	/* $57-$5F */ "X", "Y", "Z", "[", "\\", "]", "^", "_",
	/* $60-$67 */ "`", "a", "b", "c", "d", "e", "f", "g",
	/* $68-$6F */ "h", "i", "j", "k", "l", "m", "n", "o",
	/* $70-$77 */ "p", "q", "r", "s", "t", "u", "v", "w",
	/* $77-$7F */ "x", "y", "z", "{", "|", "}", "~", "",
	/*          A-umlaut    A-circle    C-cedilla   E-acute */
	/* $80-$83 */ "A",        "A",        "C",        "E",
	/*          N-tilde     O-umlaut    U-umlaut    a-acute */
	/* $84-$87 */ "N",        "O",        "U",        "a",
	/*          a-grave      a-hat      a-umlaut    a-tilde */
	/* $88-$8B */ "a",        "a",        "a",        "a",
	/*          a-circle    c-cedilla   e-acute     e-grave */
	/* $8C-$8F */ "a",        "c",        "e",        "e",
	/*           e-hat      e-umlaut    i-acute     i-grave */
	/* $90-$93 */ "e",        "e",        "i",        "i",
	/*           i-hat      i-umlaut    n-tilde     o-acute */
	/* $94-$97 */ "i",        "i",        "n",        "o",
	/*          o-grave      o-hat      o-umlaut    o-tilde */
	/* $98-$9B */ "o",        "o",        "o",        "o",
	/*          u-acute     u-grave      u-hat      u-umlaut */
	/* $9C-$9F */ "u",        "u",        "u",        "u",
	/*          dagger      degree      cents       pounds sterling */
	/* $A0-$A3 */ "$A0",     "deg.",     "c",         "#",
	/*          section      bullet     paragraph     Beta   */
	/* $A4-$A7 */ "$A4",      "*",       "$A6",       "B",
	/*          circle-R    copyright  trademark     acute  */
	/* $A8-$AB */ "(R)",     "(c)",      "TM",        "'",
	/*           umlaut    slash-equal AE ligature    null set */
	/* $AC-$AF */ "$AC",      "<>",      "AE",        "$AF",
	/*          infinity   plus-minus  less-equal   greater-equal */
	/* $B0-$B3 */ "$B0",     "+/-",      "<=",        "=>",
	/*            yen         mu        delta         Sigma */
	/* $B4-$B7 */ "$B4",     "$B5",      "d",         "$B7",
	/*             PI         pi       integral       a-bar */
	/* $B8-$BB */ "PI",      "pi",      "$BA",        "$BB",
	/*            o-bar      Omega     ae ligature    null set */
	/* $BC-$BF */ "o",       "$BD",      "ae",        "nil",
	/*          reverse-?   reverse-!    not          root */
	/* $C0-$C3 */ "?",       "!",        "^",       "$C3",
	/*          script f    approx      Delta        less-less */
	/* $C4-$C7 */ "f",       "=",        "D",       "<<",
	/*      greater-greater ellipsis  non-break sp  A-grave */
	/* $C8-$CB */ ">>",      "...",      " ",         "A",
	/*          A-tilde     O-tilde    OE ligature   oe ligature */
	/* $CC-$CF */ "A",        "O",       "OE",        "oe",
	/*          en-dash     em-dash   left quotes   right quotes */
	/* $D0-$D3 */ "-",       "--",        "\"",         "\"",
	/*         left apos.  right apos.  divide        diamond */
	/* $D4-$D7 */ "'",       "'",        "/",         "$D7",
	/*          y-umlaut  === undefined from here on === */
	/* $D8-$DB */ "y",       "$D9",      "$DA",        "$DB",
	/* $DC-$DF */ "$DC",     "$DD",      "$DE",        "$DF",
	/* $E0-$E3 */ "$E0",     "$E1",      "$E2",        "$E3",
	/* $E4-$E7 */ "$E4",     "$E5",      "$E6",        "$E7",
	/* $E8-$EB */ "$E8",     "$E9",      "$EA",        "$EB",
	/* $EC-$EF */ "$EC",     "$ED",      "$EE",        "$EF",
	/* $F0-$F3 */ "$F0",     "$F1",      "$F2",        "$F3",
	/* $F4-$F7 */ "$F4",     "$F5",      "$F6",        "$F7",
	/* $F8-$FB */ "$F8",     "$F9",      "$FA",        "$FB",
	/* $FC-$FF */ "$FC",     "$FD",      "$FE",        "$FF"};	

/* blitzdeliver --

    Begin delivery process by entering message in spool directory(s).
    Internet recipients are ignored; they're handled elsewhere.
    
    The control file format is (CRLF-terminated lines):

	<sender's address>	-- for bounces
	<summary info>
        <uid1>,<blitzfs>,<resolve timestamp>,<recip1 name>,<flags>
	<uid2>,<blitzfs>,<resolve timestamp>,<recip2 name>,<flags>
	...

    Of the recipient info, the name is the only essential part; the
    other fields will be passed along to the receiving server for its
    convenience.  This will allow the receiving server to avoid having
    to do a second dnd lookup to obtain the same information again.  The
    resolve timestamp allows the receiver to determine if the resolved
    data is potentially obsolete:  for example, if users have been
    relocated off the receiving server since the dnd lookup was performed,
    a new lookup is necessary to verify that this is still the correct
    destination for the user's mail.  
    
    The control file is named <messid>C; the message file itself is in
    <messid>.  Currently, all the spool directories must be in the same
    filesystem (since they'll share hard links to the message file).
    
    Caller must have already called mess_setup to construct the message
    file.
    
*/

void blitzdeliver(char *sender, recip *rlist1, recip *rlist2, messinfo *mi,
		 fileinfo *head, fileinfo *text, enclinfo *encl, summinfo *summ,
		 boolean_t hextext) {
					
    recip	*rlist;			/* current list */
    recip 	*r;			/* current recip */
    recip	**servrecips;		/* recips for each server */
    char	fname[128];		/* name of file in spool dir */
    t_file	*f;			/* to create control file */
    int		i;
    char	summstr[SUMMBUCK_LEN];	/* summary info in string form */
    long	qid;			/* queue id */
    boolean_t	sentout = FALSE;	/* sent to another server? */
    long 	flags;			/* delivery flags */
    	
    if (!rlist1 && !rlist2)
    	return;				/* no recips */
    
    servrecips = (recip **) mallocf(m_servcount * sizeof(recip *));
    for (i = 0; i < m_servcount; ++i)
	servrecips[i] = NULL;
	
    /* partition recipient list by destination server */
    for (rlist = rlist1 ;; rlist = rlist2) {
	if (rlist) {			/* note that all recips might be in 2nd list */
	    for (r = rlist->next ;; r = r->next) {
	    
		if (r->nosend 		/* if this isn't really an address */
		||  r->stat != RECIP_OK) /* or it's invalid */
			;		/* ignore it */
		else {
		    if (r->oneshot)	/* an enclosure clone copy? */
			r->nosend = TRUE; /* don't send again if recip list reused */
			
		    if (r->local) {	/* blitz user? */
		    
			/* explode broadcast requests */
			if (r->id == ALL_USERS) {
			    r->id = ALL_LOCAL_USERS; /* send to all users... */
			    for (r->blitzserv = 0; r->blitzserv < m_servcount; ++r->blitzserv)
				copy_recip(r, &servrecips[r->blitzserv]); /* ...on every server */
			} else if (r->id == PUBML_UPDATE_REQ) {
			    r->id = PUBML_UPDATE;	/* a mailing list update */
			    for (r->blitzserv = 0; r->blitzserv < m_servcount; ++r->blitzserv) {
			    	if (r->blitzserv != m_thisserv) /* don't send to self */
				    copy_recip(r, &servrecips[r->blitzserv]);	/* ...on every server */
			    }
			} else		/* not broadcast */
			    copy_recip(r, &servrecips[r->blitzserv]);
		    }
		}
		    
		if (r == rlist)
		    break;		/* end of one list */
	    }
	}
	if (rlist == rlist2)		/* end of both lists */
	    break;
    }
    
    /* for each destination, create control file & link to temp file */
    for (i = 0; i < m_servcount; ++i) {
	if (!servrecips[i])
	    continue;			/* no recips for this server */
    	
	if (i == m_thisserv) {		/* do local deliveries right now */
	    for (r = servrecips[i]->next ;; r = r->next) {
		localdeliver(sender, r, mi, head, text, encl, summ);
		if (r == servrecips[i])
		    break;
	    }
	    free_recips(&servrecips[i]);
	    continue;			/* no need to queue */
	}
	
	qid = next_qid();		/* assign unique qid */
	
	/* now do the control file */
	queue_fname(fname, i, qid);
	strcat(fname, "C");
	if ((f = t_fopen(fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	    t_perror1("blitzdeliver: cannot create ", fname);
	    free_recips(&servrecips[i]); /* giving up on this server */
	    break;
	}

	t_fprintf(f, "%s\r\n", sender);		/* bounce address */
	summ_fmt(summ, summstr);		/* text form of summary info */
	t_fprintf(f, "%s\r\n", summstr);	/* pass that along also */
	
	
	/* one control file line for each recip:
	   <uid>,<blitzfs>,<resolve timestamp>,<name>,<flags>
	*/	
	flags = hextext ? F_HEXTEXT : 0;	/* same for all recips */
	for (r = servrecips[i]->next ;; r = r->next) {
	    if (r->oneshot)
		flags |= F_NOFWD;
	    else
		flags &= ~F_NOFWD;
	    t_fprintf(f, "%ld,%s,%lu,%s,%ld\r\n",
			 r->id, r->blitzfs, r->timestamp, r->name, flags);
	    if (r == servrecips[i])
		break;
	}
	t_fclose(f);	

	/* link data file only after control file completely filled (in case of crash,
	   the incomplete control file will thus be ignored) */
	queue_fname(fname, i, qid);
	if (link(mi->finfo.fname, fname) != 0) {
	    t_perror1("blitzdeliver: cannot link ", fname);
	    free_recips(&servrecips[i]); /* giving up on this server */
	    break;
	}
	
	free_recips(&servrecips[i]);															
	wake_queuethread(i, qid);		/* wake thread that runs queue */
	sentout = TRUE;
    }
    
    if (sentout)				/* statistics: messages sent to other server(s) */
	++m_sent_blitzsmtp;
	
    t_free(servrecips);
    
    /* -- caller will mess_done temp file -- */
    
}

/* localdeliver --

    Deliver message to a single local recipient.   Handle special recipient "uids"
    (for functions like sending to all local users and updating mailing lists.)

*/

void localdeliver(char *sender, recip *r, messinfo *mi, fileinfo *head, fileinfo *text, 
                  enclinfo *encl, summinfo *summ) {
		  
    char	logbuf[MAX_ADDR_LEN];
    int		blitzfs;		/* which filesystem box is on */
		  
    if (r->id == ALL_LOCAL_USERS)  /* delivery to everyone? */
	alldeliver(head, text, encl, summ);
    else if (r->id == PUBML_UPDATE)  /* mailing list update message? */
	pubml_update(head, text);
    else if (r->id >= 0) {	/* negative uids aren't real */
	blitzfs = fs_match(r->blitzfs);
	if (localdeliver_one(sender, r->name, r->id, blitzfs, mi, head, text, summ)) {
	    t_sprintf(logbuf, "Message %ld sent to %s", summ->messid, r->name);
	    log_it(logbuf);
	}
    } else {
	t_sprintf(logbuf, "Ignore message %ld to unknown special uid %ld",
			    summ->messid, r->id);
	log_it(logbuf);
    }
}



/* alldeliver --

    Deliver message to all mailboxes on this server.  This can take quite a
    while, and we don't want to delay other deliveries in the meantime, so
    a thread is spawned to do the delivery (independent of the inqueue thread).
    
    A separate copy of the message must be made for this purpose (to avoid trouble
    when inqueue_read cleans up & we're still delivering).  Since this copy is
    outside the queueing system, a crash will interrupt the delivery.  Oh well.
    
    Set up a new copy of the message, create a parameter block, and spawn thread
    to do the delivery.
*/

void alldeliver(fileinfo *head, fileinfo *text, enclinfo *encl, summinfo *summ) {

    alldel_pb	*pb;		/* param block */
    pthread_t	thread;		/* deliver thread */
    
    pb = mallocf(sizeof(alldel_pb));		/* get parameter block */
    
    summ_copy(&pb->summ, summ, FALSE);		/* copy summary info (unpacked) */
    pb->summ.messid = next_messid();		/* need to assign a new messageid */
    
    /* create our own copy of the message */
    if (!mess_setup(pb->summ.messid, head, text, encl, 
    		   &pb->mi, m_spool_filesys, pb->summ.type)) {
	t_perror("alldeliver: mess_setup failed");
	return;
    }
    
    /* start up thread to do the mass mailing */
    if (pthread_create(&thread, generic_attr,
		    (pthread_startroutine_t) alldel_thread, (pthread_addr_t) pb) < 0) {
	t_perror("alldeliver: pthread_create");
	return;
    }
    pthread_detach(&thread);
    	
    /* thread will mess_done & free the pb when done */
    
    return;					/* caller can now remove original mess */
}
/* alldel_thread --

    Thread spawned to deliver message to all local boxes.
    
    For each filesystem, read box directory to locate all local boxes.
    Call localdeliver_one to send the message to each.
    
*/

any_t alldel_thread(any_t pb_) {

    alldel_pb		*pb;			/* our parameter block */
    int			fs;			/* current filesystem */
    char		fname[MBOX_NAMELEN];	/* name of box dir on that fs */
    DIR			*boxdir;		/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    long		uid;			/* one box */
    char		*end;			/* end of uid str */
    char		logbuf[MAX_STR];


    pb = (alldel_pb *) pb_;
    
    for (fs = 0; fs < m_filesys_count; ++fs) { /* for each filesystem */

	/* open box directory */
    
	t_sprintf(fname, "%s%s", m_filesys[fs], BOX_DIR);
	pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
	boxdir = opendir(fname);
	pthread_mutex_unlock(&dir_lock);
	
	if (boxdir == NULL) {
	    t_perror1("alldel_thread: cannot open ", fname);
	    continue;
	} 

    while ((dirp = readdir(boxdir)) != NULL) {	/* read entire directory */
		
	    /* skip dot-files */
	    if (dirp->d_name[0] != '.') {
		end = strtonum(dirp->d_name, &uid);
		if (*end == 0)	/* ignore non-numeric filenames */
		    (void) localdeliver_one(NULL, NULL, uid, fs, &pb->mi, NULL, NULL, &pb->summ);	
	    }
	}
	closedir(boxdir);
    }
    
    t_sprintf(logbuf, "Message %ld delivered to all users.", pb->summ.messid);
    log_it(logbuf);
    
    mess_done(&pb->mi);			/* clean up our copy of the message */
    t_free(pb);
    
    return 0;				/* thread fades away */
}

/* localdeliver_one --

    Deliver message to a single mailbox on this server.  Send notification.
    If user is active, construct warning for client.  Note that the same
    user may be on a recipient list more than once; in that case we detect
    that a copy of the message has already been delivered and refrain from
    sending another.

*/

boolean_t localdeliver_one(char *sender, char *name, long uid, int fs, messinfo *mi, 
		      fileinfo *head, fileinfo *text, summinfo *summ) {

    mbox	*mb;			/* recipient mailbox */
    char	verbose[PREF_MAXLEN];
    char	data[MAX_STR];		/* notification data (pstring) */
    char	err[MAX_STR];
    char	*notmsg = data+1;	/* first char of it */
    long	len;
    boolean_t	sticky;			/* persistent notifacation? */
    boolean_t 	ok = FALSE;		/* returned: delivered ok? */
    				
    mb = mbox_find(uid, fs, FALSE);	/* get the mailbox */
    
    /* if box has since been transferred, bounce the message */
    if (mb->gone) {
	if (head && text)		/* can't bounce message to all; no big deal */
	  bad_mail(sender, name, head, text, summ, "Mailbox in transit; try again.");
    } else if (mess_deliver(mb, mi, summ->totallen, err)) {	/* save message (if already there, dup recip) */
	if (!summ_deliver(mb, summ, INBOX_NUM, summ->totallen)) 	/* add summary to inbox */
	    t_errprint_ll("Duplicate summary delivering mess %ld to uid %ld",
	    		   summ->messid, uid);
			   
	++m_delivered;			/* statistics: local deliveries */   
	ok = TRUE;			/* return good status */
	newmail_warn(mb, summ->messid, INBOX_NUM); /* queue up warning iff user active */
    	
	if (!pref_get(mb, PREF_VERBNOT, verbose)) /* verbose notification? */
	    strcpy(verbose, "");	/* default is non-verbose */
	    
	/* construct notification message */
	
	/* (date time) */
	date_time(notmsg+1, notmsg+10);
	notmsg[0] = '('; notmsg[9] = ' ';
	strcat(notmsg, ") ");
	
	if (!name)			/* recip name unknown for broadcasts */
	    strcat(notmsg, "You have received new mail");
	else {
	    strcat(notmsg, name);
	    strcat(notmsg, " has received new mail");
	}
	
	if (strcmp(verbose, "\"1\"") == 0) {
	    if (summ->sender && *summ->sender &&
		    strlen(summ->sender) + strlen(notmsg) + strlen(" from ") < MAX_STR) {
		strcat(notmsg, " from ");
		strcat(notmsg, summ->sender);
	    }
	    if (summ->topic && *summ->topic &&
		    strlen(summ->topic) + strlen(notmsg) + strlen(" about \"") + 1 < MAX_STR) {
		strcat(notmsg, " about \"");
		strcat(notmsg, summ->topic);
		strcat(notmsg, "\"");
	    } else
	        strcat(notmsg, ".");
	} else
	    strcat(notmsg, ".");
	    
	len = strlen(notmsg);		/* set up pstring */
	*data = len++;			/* accounting for length byte */
	
	/* call notification server to deliver it. */
	sticky = TRUE;			
	do_notify(uid, NTYPE_MAIL, summ->messid, data, len, sticky);
    } else {				/* mess_deliver failed */
	if (strlen(err) > 0) {		/* ignore "duplicate recip" state; that's normal */
	    bad_mail(sender, name, head, text, summ, err);
	}
    }
	
    mbox_done(&mb);
    return ok;
}
/* audit_deliver --

    Deliver copy of message to audit folder.  If user also has the message in the InBox,
    a new messid must be assigned.  Note:  the summary and messinfo are modified.

*/

void audit_deliver(udb *user, summinfo *summ, messinfo *mi) {

    long 	newexp;			/* expiration setting */
    u_long	oldexp;
    long	messid;			/* messid of the audit copy */
    char	err[MAX_STR];

    oldexp = summ->expire;		/* remember old setting for below */
    newexp = fold_autoexp(user->mb, AUDIT_NUM);	/* get expiration setting */
    if (newexp != -1) {
	summ->expire = (u_long) newexp;
    }
 
    messid = summ->messid;			/* new id will be assigned if necessary */
   
    /* try to deliver; will fail if already in InBox */
    if (mess_deliver(user->mb, mi, summ->totallen,err)) {
	    if (!summ_deliver(user->mb, summ, AUDIT_NUM, summ->totallen)) {
	    t_errprint_ll("audit_deliver: duplicate summary delivering message %ld to uid %ld",
			    messid, user->uid);
	}
    } else {					/* message is in InBox - copy it */
	if (!summ_copymess(user->mb, &user->mb->fold[INBOX_NUM], 
		                     &user->mb->fold[AUDIT_NUM], 
				     &messid, newexp)) {
	    t_errprint_ll("audit_deliver: can't audit summary mess %ld uid %ld",
			    messid, user->uid);    
	}
    }    
    
    /* also deliver a copy to additional audit folder, if one is defined */
    if (user->xtra_audit >= 0) {
	newexp = fold_autoexp(user->mb, user->xtra_audit);	/* get expiration setting */
	if (newexp == -1) {			/* no setting for this folder */
	    newexp = oldexp;			/* revert to initial default */
	}
	
	if (!summ_copymess(user->mb, &user->mb->fold[AUDIT_NUM], 
		                     &user->mb->fold[user->xtra_audit], 
				     &messid, newexp)) {
	    t_errprint_ll("audit_deliver: can't copy summary mess %ld uid %ld",
			    messid, user->uid);
	}
    }
}

/* deliver --

    Construct (we, not the client, build the header) and deliver a message.  
    If blind-carbon-copies are involved, several messages actually need to be 
    constructed.  If this is a user-generated message (as opposed to an automatic
    reply or something); save a copy of the message in the user's audit folder.
    
    Returns FALSE if the message cannot be delivered (e.g., disk error).
*/

boolean_t deliver(udb *user, char *sender, recip *tolist, recip *cclist, recip *bcclist,
	    fileinfo *text, enclinfo *encl, summinfo *summ, boolean_t hextext,
	    char *replyto, boolean_t hiderecips) {
	    
					
    fileinfo 	head;			/* message header */
    fileinfo	*contenthead = NULL;	/* partial header w/ Content-Type: etc. */
    messinfo	mi;			/* delivery information */
    char	*logbuf; 		/* log string */
    recip	*new, *onebcc, *bcccopy; /* for bcc copying */
    recip 	*r;			/* to locate recip name */
    char	*internet_sender = sender; 	/* w/ vanity hostname */

    /* If in user context, user->head should be scanned to pick up
       Content-type: etc., *if* current message is in RFC822 format.
       This handles the case of full-MIME uploads, as well as a
       "forward without change" on a RFC822-format message. */
    if (user && summ->type == MESSTYPE_RFC822) {
	contenthead = &(user->head);	/* look here for content type etc. */
    } 
    
    /* set up summary info */
    date_time(summ->date, summ->time);	/* fill in current date & time */
    summ->expire = pick_expire(summ);	/* choose expiration date */
    					/* message type set by caller */
    strcpy(summ->sender, sender);
    summ->read = FALSE;			/* new message; unread so far */
    
    strcpy(summ->recipname, "");	/* who is first recipient? */
    if (tolist && !hiderecips) {
	r = tolist->next;
	do {				/* locate first recip on To: line */
	    if (!r->noshow) {
		if (r->name[0])		/* use DND name if present */
		    strcpy(summ->recipname, r->name);
		else			/* else just regular addr */
		    strcpy(summ->recipname, r->addr);
		if (r->next != tolist->next && strlen(summ->recipname)+4 < MAX_ADDR_LEN)
		    strcat(summ->recipname, ",...");
		break;			/* done when 1st recip located */
	    }
	    r = r->next;
	} while (r != tolist->next);
    }
    if (hiderecips) {			/* if hiding, omit recip names from header */
    	if (!get_head(sender, NULL, NULL, NULL, &head, summ, replyto, contenthead))
            return FALSE;                   /* io error writing header; give up */
    } else {				/* construct message header; don't show bcc's */
   	if (!get_head(sender, tolist, cclist, NULL, &head, summ, replyto, contenthead))
    	    return FALSE;			/* io error writing header; give up */
    }
    
    if (!mess_setup(summ->messid, &head, text, encl, 
    		    &mi, m_spool_filesys, summ->type)) {
	t_perror("deliver: mess_setup failed");
	finfoclose(&head);	/* forget the header */
	return FALSE;
    }	
    if (summ->enclosures)		/* if any enclosures */
	summ->totallen = mi.finfo.len;	/* total length including header & encls */
    else
	summ->totallen = text->len;	/* show just text, not header */
	
    logbuf = mallocf(255 + strlen(sender));
    t_sprintf(logbuf, "Delivering message %ld from %s; %ld bytes; %ld enclosures",
    		summ->messid, sender, summ->totallen, summ->enclosures);
    log_it(logbuf);
    t_free(logbuf);
    ++m_sent;				/* statistics: count messages sent */
    
    /* handle any bad addresses in to/cc lists */
	    
    do_bounces(sender, tolist, &head, text, summ);
    do_bounces(sender, cclist, &head, text, summ);
    
    /* export message to SMTP queues for internet recips */
    if (user)				/* in user context... */
	internet_sender = user->email;	/* ...pick up vanity domain name */
    internet(internet_sender, tolist, cclist, &head, text, encl, summ, hextext);

    /* and add message to our queue(s) for blitz recipients */
    blitzdeliver(sender, tolist, cclist, &mi, &head, text, encl, summ, hextext);
 
    /* In "hiderecips" case, the same message can serve for bcc recipients as well */

    if (bcclist && hiderecips) {	

	/* handle any bad addresses */

    	do_bounces(sender, bcclist, &head, text, summ);

    	/* export message to SMTP queues for internet recips */
    	internet(internet_sender, bcclist, NULL, &head, text, encl, summ, hextext);

	/* and add message to our queue(s) for blitz recipients */
    	blitzdeliver(sender, bcclist, NULL, &mi, &head, text, encl, summ, hextext);
    }


    /* if no bcc's, and recipient list not hidden, same message is audit copy */
    if (user && (bcclist == NULL && !hiderecips)) 
        audit_deliver(user, summ, &mi);

    mess_done(&mi);			/* release our link to message; it's queued */

					  
    /* Now, create bcc messages */
    
    /* Note: global aliases are expanded into one or more 'noshow' nodes
    followed by a 'nosend' node w/ the name of the list.  Handle each
    such sequence as a unit (everyone on the list gets a copy that says
    "bcc: list-name"). */
    
    if (bcclist && !hiderecips) {
	
	onebcc = bcclist->next ;	/* head */
	do { 				/* while more bcc's */  
	    bcccopy = NULL;
	    for(;;) {			/* combine sequence of "noshow"s */
		new = mallocf(sizeof(recip));
		*new = *onebcc;		/* copy recip from master list */
		if (!bcccopy)		/* first? */
		    new->next = new; 	/* link to self */
		else {			/* else add to end */
		    new->next = bcccopy->next;
		    bcccopy->next = new;
		}
		bcccopy = new;		/* new one is last */
		
		if (!onebcc->noshow)	/* if we've reached end of "noshows" */
		    break;
		else
		    onebcc = onebcc->next;
		    
		if (onebcc == bcclist->next) /* should never happen... */
		    break;		/* don't spin if whole list is "noshow" */
	    }
	    
	    finfoclose(&head);			/* close old header */
	    
	    /* construct new message with personalized bcc: line */
	    if (!get_head(sender, tolist, cclist, bcccopy, &head, summ, replyto, contenthead)) {
		free_recips(&bcccopy);		/* oops; io trouble */		
		return FALSE;	
	    }
	    if (!mess_setup(summ->messid, &head, text, encl, 
	    		    &mi, m_spool_filesys, summ->type)) {
		t_perror("deliver: bcc mess_setup failed");
		finfoclose(&head);		/* can't; clean up */
		free_recips(&bcccopy);		
		return FALSE;
	    }	
	    if (summ->enclosures)		/* if any enclosures */
		summ->totallen = mi.finfo.len;	/* total length including header & encls */
	    else
		summ->totallen = text->len;	/* show just text, not header */
		
	    logbuf = mallocf(255 + strlen(sender));
	    t_sprintf(logbuf, "Delivering bcc %ld from %s", summ->messid, sender);
	    ++m_sent;
	    log_it(logbuf);
	    t_free(logbuf);
	
	    /* bounce bad bcc addrs */
	    do_bounces(sender, bcccopy, &head, text, summ);
	    
	    /* and queue this one up */
	    internet(internet_sender, bcccopy, NULL, &head, text, encl, summ, hextext);
	    blitzdeliver(sender, bcccopy, NULL, &mi, &head, text, encl, summ, hextext);
	    
	    free_recips(&bcccopy);		/* clean up recip copy */
	    mess_done(&mi);			/* done with message */
	    
	    onebcc = onebcc->next;		/* on to next */
	} while (onebcc != bcclist->next);	/* done when end of list reached */		
     }
	
     /* Audit copy of bcc message shows ALL bcc's;
        Audit copy of "hide recipients" message shows all recipients */

    if (user && (bcclist || hiderecips)) {	/* no audit copy of system mail */
	finfoclose(&head);			/* close old header */
	    
	/* construct new message with complete bcc: line */
	if (!get_head(sender, tolist, cclist, bcclist, &head, summ, replyto, contenthead)) {
	    free_recips(&bcccopy);		/* oops; io trouble */		
	    return FALSE;	
	}
	if (!mess_setup(summ->messid, &head, text, encl, 
		    &mi, m_spool_filesys, summ->type)) {
	    t_perror("deliver: audit bcc mess_setup failed");
	    finfoclose(&head);		/* can't; clean up */
	    free_recips(&bcccopy);		
	    return FALSE;
	}	
	if (summ->enclosures)			/* if any enclosures */
	    summ->totallen = mi.finfo.len;	/* total length including header & encls */
	else
	    summ->totallen = text->len;		/* show just text, not header */
	    
	audit_deliver(user, summ, &mi);
	mess_done(&mi);				/* done with audit copy */
    }
    
    do_vacations(tolist, &head, summ);		/* send vacation messages */
    do_vacations(cclist, &head, summ);	
    do_vacations(bcclist, &head, summ);	
    
    finfoclose(&head);			/* close the last header used */
					    
    return TRUE;			/* message delivered */							
}

/* get_head --

    Assign a message-id and create the message header. 
    
    We always construct the blitz-version (as opposed to the RFC822-version)
    of the header; if the message is exported to internet systems the appropriate 
    transformations can be made at that time.
    
    Sets up a "fileinfo" describing the header file.  (The header will be
    in a temp file, so fileinfo.temp will be set.)
    
    Returns false if header could not be created (e.g., disk error).
*/

boolean_t get_head(char *sender, recip *tolist, recip *cclist, recip *bcclist, 
		fileinfo *head, summinfo *summ, char *replyto, fileinfo *contenthead) {
	
    t_file 	*f;
    char	buf[MAX_ADDR_LEN];
    boolean_t	err;
    
    summ->messid = next_messid();	/* assign message id */

    mess_tmpname(head->fname, m_spool_filesys, summ->messid);	
    strcat(head->fname, "h");		/* base tempname on message id */
  
    if ((f = t_fopen(head->fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("get_head: cannot create ", head->fname);
	return FALSE;			/* open failed */
    }
        
    /* id @ this-server */
    t_fprintf(f, "Message-id: <%ld@%s>\r", summ->messid, m_fullservname);
    
    get_date(buf);			/* date in standard format */
    t_fprintf(f, "Date: %s\r", buf);

    t_fprintf(f, "From: %s\r", sender);	/* use just name in From: line */
    
    if (replyto)
	t_fprintf(f, "Reply-To: %s\r", replyto); /* add reply-to if present */
    
    t_fprintf(f, "Subject: %s\r", summ->topic);
    
    if (!tolist && !cclist && !bcclist)	/* if hiding all recipients... */
	t_fprintf(f, "To: (Recipient list suppressed)\r");

    if (tolist)				/* now, do the To: line */
	getnames(f, "To: ", tolist);
    if (cclist)				/* cc's, if any */
    	getnames(f, "Cc: ", cclist);
    if (bcclist)			/* bcc's, if any & if we should */
	getnames(f, "Bcc: ", bcclist);

    if (summ->receipt) 			/* receipt requested? */
	t_fprintf(f, "Return-receipt-to: %s\r", sender);

    if (contenthead) {			 /* scan old header for content type? */
    	err = !mess_copy_contenthead(contenthead, f);	/* copy matching lines */
	if (err) 
	    goto cleanup;		/* give up on io error */
    }
    
    t_fflush(f);
    err = f->t_errno < 0;		/* trouble doing any of that? */

    head->offset = 0;			/* start at beginning of file */    
    head->len = t_fseek(f, 0, SEEK_END);/* for this much */
    head->temp = TRUE;			/* unlink this file when done */

cleanup:
    t_fclose(f);
    
    if (err)				/* if it didn't work */
	finfoclose(head);		/* clean up the file now */
	
    return !err;
    
}

/* getnames --

    Construct the to/cc/bcc line of a message header from recipient list.
    
    The following basic rules determine what form of a recipient's name or
    address should be used in the header line:
    
    *  The "@mac" hostname shouldn't be included when it's not necessary
    *  Replying to the address in a "to:" field should send the reply to
       the same address that got the original message.
    *  The resolved DND name (if any) should be included in a comment so
       there isn't just a list of cryptic host user ids, recipients should
       also see the name of the person who gets their mail at "k98765@d1".
       
    For example:
    
    Input name/addr      DND address used (if any)     To: field
    ==============       =========================     =========
    Fred Flintstone      Fred_J._Flintstone@mac        Fred J. Flintstone
    Fred Flintstone      fredf@u2                      fredf@u2 (Fred J. Flintstone)
    Fred Flintstone@mac                                Fred.J.Flintstone@mac
    53780x@d1                                          53780x@d1
    
    This is the internal format header; some additional rewriting will take
    place for messages going out into the Internet.
    
    See also "sresolve" to see how recipient names supplied by the user get
    resolved into the "name" and "address" fields of a "recip".
    
*/

void getnames(t_file *f, char *label, recip *recipl) {

    recip	*r;
    char	name[MAX_ADDR_LEN];	/* name/address we finally decide on */
    char	buf[MAX_ADDR_LEN];	/* temp */
    boolean_t	done;

    t_puts(f, label);			/* put the "To: " or whatever */
    
    for (r = recipl->next, done = FALSE ; !done ; r = r->next) {
    
	done = r == recipl;		/* end of circular list? */
	
    	if (!r->noshow) {		/* unless we're hiding this name */
	    if (r->name[0]) {		/* DND name present? */
		if (!r->addr[0]) 	/* DND address = name@mac */
		    strcpy(name, r->name); /* can use just the plain name */
		else {
		    strcpy(name, r->addr); /* we'll need to use the addr */
		    addhost(r->name, buf, m_hostname);
		    if (strcmp(r->addr, buf) == 0) /* DND name + explicit @mac */
			; 		/* we aren't preferred addr; keep the @mac */
		    else if ((strcmp(r->name, r->addr) != 0) /* don't duplicate needlessly */
		         && (strlen(r->addr) + strlen(r->name) + 3 + 1 < MAX_ADDR_LEN)) {
		    	strcat(name, " ("); /* add name as comment iff there's room */
			strcat(name, r->name); /* DND name w/ non-local address */
			strcat(name, ")");
		    }
		}

	    } else			/* else - no DND reference */
	    	strcpy(name, r->addr);	/* just plain address */
		
	    t_puts(f, name);		/* finally, output the name/addr we've chosen */
	    if (!done) 			/* if more to come, delimit w/ comma */
		t_puts(f, ", ");
	}
    }
	
    t_putc(f, '\r');
}    

/* internet --

    Deliver message to a list of internet addresses.  Since the message is leaving
    Blitzland, there are a number of special considerations:
    
    - The message must be converted from Blitz format into something that can
      be sent through text-only mail systems.  Long lines are wrapped, Mac
      special characters are translated into the closest Ascii equivalents,
      and names in the header are converted to user@host format.
      
    - Text enclosures are appended to the end of the message body.
    
    - Non-text enclosures are converted to binhex, then appended to the message.
      For recipients that were specfied as a DND name (but whose
      preferred address is another mail system), deliver a copy of the message
      (with blitz enclosures) to the corresponding blitz mailbox.  (Thus this
      routine may split a message with enclosures into 3 separate messages:
      the local one, the one sent to recipients that have a blitz mailbox,
      and the one sent to those who don't have a blitz mailbox.)
*/

void internet(char *sender, recip *rlist1, recip *rlist2, fileinfo *head, 
		fileinfo *text, enclinfo *encl, summinfo *summ, boolean_t hextext) {

    recip 	*rlist;			/* current recip list */
    recip	*r;			/* current recip */
    enclinfo	*text_encl, *nontext_encl; /* partitioned enclosure list */
    fileinfo	textmess;    		/* textified version of message */
    t_file	*textf;			/* '' (the file itself) */
    
    /* search recip lists to see if there are any internet addrs */
    for (rlist = rlist1 ;; rlist = rlist2) {
	if (rlist) {
	    for (r = rlist->next ;; r = r->next) {
		if (!r->local && !r->nosend && r->stat == RECIP_OK)
		    goto haverecip;	/* at least one; search no more */
		if (r == rlist)
		    break;		/* end of one list */
	    }
	}
	
	if (rlist == rlist2)		/* end of both lists */
	    break;			/* (if rlist1 == rlist2 == NULL, still ok) */
    }    
    return;				/* search failed:  nothing to do */
    
    haverecip:				/* here if internet recip found */
	
    find_text_encl(encl, &text_encl, &nontext_encl, hextext); /* partition enclosure list */
    
    if (nontext_encl) {			/* enclosures - binhex them & recurse */
	binhex_enclosures(sender, rlist1, rlist2, head, text, text_encl, nontext_encl, summ);
	clean_encl_list(&text_encl);	/* clean up the list copies */
	clean_encl_list(&nontext_encl);
	return;				/* and done */
	
    }
    
    /* create RFC822 format version of message in "textf"/"textmess" */    
    textf = exportmess(summ->messid, head, text, &textmess, summ->type);
    if (textf)
    	add_text_encl(textf, text_encl, TRUE); /* append any text enclosures */
    
    clean_encl_list(&text_encl);	/* clean up enclosure list copy */
    					/* (nontext_encl known to be null) */
    
    if (!textf)
	return;				/* couldn't read file; bail out */
	
    /* queue message for internet delivery, via sendmail or our own builtin
       SMTP, depending on configuration */
    if (m_server[SMTP_HOSTNUM])		/* are we doing SMTP ourself? */
	internet_queue(sender, rlist1, rlist2, &textmess, textf, head, text, summ); 
    else
	internet_send(sender, rlist1, rlist2, &textmess, textf, head, text, summ); 

    finfoclose(&textmess);		/* dispose of our copy of the message */
    t_fclose(textf);
    
}    
/* internet_queue --

    Add RFC822-format message to our own outgoing SMTP queue (i.e., we're configured
    to handle outgoing SMTP ourself, not using sendmail). 
    
    Note that we queue the RFC822 format message, not the blitzformat file. 
 
    The control file format is (CRLF-terminated lines):

	<sender's address>	
	<summary info>		-- for bounces
        <recip1 addr>
        <recip2 addr>
	...
  
*/

void internet_queue(char *sender, recip *rlist1, recip *rlist2, fileinfo *textmess, 
		t_file *textf, fileinfo *head, fileinfo *text, summinfo *summ) {

    char	fname[MAX_STR];		/* name of file in spool dir */
    long	qid;			/* queue id */
    t_file	*f;			/* temp file */
    char	send_addr[MAX_ADDR_LEN]; /* internet form of address */
    char	summstr[SUMMBUCK_LEN];	/* summary info in string form */
    recip	*rlist;			/* current list */
    recip 	*r;			/* current recipient */
    
    qid = next_qid();			/* assign qid */

    queue_fname(fname, SMTP_HOSTNUM, qid); /* generate name */
    strcat(fname, "C");			/* ... of control file */
    
    if ((f = t_fopen(fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("internet_queue: cannot create ", fname);
	return;				/* *** try to bounce? *** */
    }


    strcpy(send_addr, sender);
    
    /* sending address must be internet format */
    if (!isinternet(send_addr, NULL) && strlen(send_addr) > 0) { 
	if (strcasecmp(POSTMASTER, sender) == 0)	/* special-case "postmaster" */
	    addhost(sender, send_addr, m_hostname); /* add local hostname */
	else
	    addhost(sender, send_addr, m_dndhost[0]); /* add dnd hostname */
    }

    t_fprintf(f, "%s\r\n", send_addr);	/* bounce address */
    summ_fmt(summ, summstr);		/* text form of summary info */
    t_fprintf(f, "%s\r\n", summstr);	/* pass that along also */

    /* one control file line for each recip */	
    
    for (rlist = rlist1 ;; rlist = rlist2) {
	if (rlist) {
	    for (r = rlist->next ;; r = r->next) {
		if (!r->local && !r->nosend && r->stat == RECIP_OK)
		    t_fprintf(f, "%s\r\n", r->addr);
		if (r == rlist)
		    break;			/* end of one list */
	    }
	}	
	if (rlist == rlist2)		/* end of both lists */
	    break;
    }
    t_fclose(f);	

    /* link data file only after control file completely filled (in case of crash,
	the incomplete control file will thus be ignored) */
    queue_fname(fname, SMTP_HOSTNUM, qid);	/* generate name */
    if (link(textmess->fname, fname) != 0) {
	t_perror1("internet_queue: cannot link ", fname);
	/***** try to bounce? ****/
    } else
	wake_queuethread(SMTP_HOSTNUM, qid);		/* wake thread that runs queue */
    
}

/* internet_send --

    Run sendmail to deliver message to a list of internet addresses.  
    All special processing has been completed (we now have an RFC822-compliant 
    message), so just send it.
*/

void internet_send(char *sender, recip *rlist1, recip *rlist2, fileinfo *textmess, t_file *textf, 
		   fileinfo *head, fileinfo *text, summinfo *summ) {

    recip 	*rlist;			/* current recip list */
    recip	*r;			/* current recip */
    char	send_opt[MAX_ADDR_LEN+2]; /* -f<sender> */
    char	*send_addr;		/* == send_opt+2 */
    char	*logbuf;
    int		argc;
    char  	**argv;
    union wait  stat;
    boolean_t	ok;
    int		pid;			/* child process */
    int		i;		
   
    logbuf = mallocf(MAX_ADDR_LEN+256);
    argv = mallocf((ADDR_MAX_RECIPS+2+1) * sizeof(char *));
    
    /* construct arg list for sendmail */
    argv[0] = "sendmail";
    strcpy(send_opt, "-f");		/* -f<sender addr> */
    send_addr = send_opt + 2;
    strcpy(send_addr, sender);
    if (!isinternet(send_addr, NULL) && strlen(send_addr) > 0) {
	/* special-case "postmaster" */
	if (strcasecmp(POSTMASTER, sender) == 0)
	    addhost(sender, send_addr, m_hostname);	/* add local hostname */
	else
	    addhost(sender, send_addr, m_dndhost[0]);	/* add dnd hostname */
    }
    
    argv[1] = send_opt;
    argv[2] = "-oi";			/* "." is not message terminator */
    argc = 3;				/* 3 args, plus one per recip */
    	
    for (rlist = rlist1 ;; rlist = rlist2) {
	if (rlist) {
	    for (r = rlist->next ;; r = r->next) {
		if (!r->local && !r->nosend && r->stat == RECIP_OK)
		    argv[argc++] = r->addr; /* get each address we're sending to */
		if (r == rlist)
		    break;			/* end of one list */
	    }
	}
	
	if (rlist == rlist2)		/* end of both lists */
	    break;
    }
    argv[argc] = NULL;
    
    t_fflush(textf);			/* flush buffered output */
    
    pid = fork();			/* fork subprocess to run sendmail */
    if (pid == -1) {
	t_perror("internet: fork");
    } else if (pid == 0) {		/* CHILD: */
	close(0);
	dup2(textf->fd, 0);		/* put message on stdin */
	lseek(0, 0, SEEK_SET);		/* seek to start */
    	for (i = 3; i < maxfds; ++i)	/* close all our other files */
	    close(i);
	/* do something with stderr?? */
	execv("/usr/lib/sendmail", argv);
	perror("Cannot execv sendmail");
	exit(1);
    } 					

    /* wait for child to finish, so we know message is queued up ok */
    if (wait4(pid, &stat, 0, NULL) != pid) 
	t_perror("internet: wait4");
    else {				/* 2nd pass through recips bouncing/logging */
	++m_sent_internet;		/* statistics: count internet messages sent */
	
	if (stat.w_termsig != 0) {
	    t_sprintf(logbuf, "Sendmail aborted w/ signal %d", stat.w_termsig);
	    t_errprint(logbuf);
	    ok = FALSE;
	} else {
	    ok = stat.w_retcode == EX_OK 	/* message delivered */
		|| stat.w_retcode == EX_TEMPFAIL; /* not delivered, but queued safely */
	}
	for (rlist = rlist1 ;; rlist = rlist2) {
	    if (rlist) {
		for (r = rlist->next ;; r = r->next) {
		    if (!r->local && !r->nosend && r->stat == RECIP_OK) {
			if (ok) {		/* if message delivered */
			    t_sprintf(logbuf, "Sent message %ld to %s", summ->messid, r->addr);
			    log_it(logbuf);
			} else
			    bad_mail(send_addr, r->addr, head, text, summ, "Unable to queue message"); 
		    }
		    if (r == rlist)
			break;			/* end of one list */
		}
	    }
	    
	    if (rlist == rlist2)		/* end of both lists */
		break;
	}
    } 
    t_free(logbuf);
    t_free(argv);
}

/* find_text_encl --

    Given an enclosure list, create two new lists, one of text enclosures
    and one of non-text.  If "hextext" is set, even text enclosures are to 
    be binhexed, so treat all as non-text.
*/
void find_text_encl(enclinfo *encl, enclinfo **text_encl, enclinfo **nontext_encl,
	boolean_t hextext) {

    enclinfo	*p, *new;		/* current encl & copy */
    enclinfo	*txtlast, *nontxtlast; 	/* tails */
    char	encltype[64];		/* enclosure type/creator */
    
    *text_encl = *nontext_encl = NULL;
    txtlast = nontxtlast = NULL;
    
    for (p = encl; p != NULL; p = p->next) {
	new = mallocf(sizeof(enclinfo));
	*new = *p;			/* copy enclosure */
	new->next = NULL;		/* it'll be last */
	new->finfo.temp = FALSE;	/* file is shared; don't unlink */
	
	unquote(encltype, p->type);	/* get & unquote type */
	/* (old clients give type in Ascii, new ones in hex) */
	if (!hextext && (strncmp(encltype, "TEXT", 4) == 0
	 	      || strncmp(encltype, "54455854", 8) == 0)) {
	    if (txtlast)		/* add to text list */
		txtlast->next = new;
	    else
		*text_encl = new;
	    txtlast = new;
	} else {
	    if (nontxtlast)		/* add to nontext list */
		nontxtlast->next = new;
	    else
		*nontext_encl = new;
	    nontxtlast = new;
	}
    }
}

/* add_text_encl --

    Enclosures of type "TEXT" can reasonably be transmitted to internet
    systems by sending just the data fork of the file.  This requires
    us to poke into the internal structure of the enclosure file (sigh...)
    
    "dowrap" indicates whether we should word-wrap the enclosure as we
    append it (it will generally be true, unless the whole file is due
    to be wrapped at a later point in the process).
*/

void add_text_encl(t_file *out, enclinfo *text_encl, boolean_t dowrap) {

    fileinfo	dinfo;		/* data fork of enclosure */
    t_file	*f;		/* file holding it */
    enclinfo	*p;

    t_fseek(out, 0, SEEK_END);	/* start at end of message */
    
    for (p = text_encl; p != NULL; p = p->next) {
	f = find_data_fork(&p->finfo, &dinfo); /* locate data fork */
	if (f) {		/* skip bad files */
	    if (dowrap) {
	    	t_putc(out, '\n'); /* make sure encl starts on a new line */
		wrap(&dinfo, f, out); /* line-wrap & append enclosure */
	    } else {		   /* append, but keep in mac format */
	    	t_putc(out, '\r'); /* mac-format newline */
		(void) finfocopy(out, &dinfo);   /* copy verbatim */	
	    }
	    (void) t_fclose(f);
	}   
    }
}

/* find_data_fork --

    Examine enclosure file (machost format) to locate the data fork.
    Construct a "fileinfo" describing the data fork.
*/
t_file *find_data_fork(fileinfo *text_encl, fileinfo *dinfo) {

    t_file	*f;			/* returned:  t_file for the data fork */
    int		inlen;
    char 	*p;
    struct machost *hdr;		/* machost header (network byte order) */
    
    if ((f = t_fopen(text_encl->fname, O_RDONLY, 0)) == NULL) {
    	t_perror1("find_data_fork: cannot open ", text_encl->fname);
	return NULL;
    }
    (void) t_fseek(f, text_encl->offset, SEEK_SET); /* seek to start of encl */

    inlen = MACHOST_HDR_LEN + 256 + 4;	/* enough for header & max-sized filename, data fork len */
    if (inlen > text_encl->len)
	inlen = text_encl->len;		/* watch out for very short enclosures */
    hdr = mallocf(inlen);
    
    if (t_fread(f, (char *) hdr, inlen) != inlen) {
    	t_perror1("find_data_fork: can't read encl header ",text_encl->fname);
	(void) t_fclose(f);
	t_free(hdr);
	return NULL;
    }
    
    /* must be the version we understand */ 
    if (hdr->vers != 0xFE || strncmp(hdr->type,"Mac",3) != 0) {    	
    	t_perror1("find_data_fork: enclosure not MacHost format ",text_encl->fname);
	(void) t_fclose(f);
	t_free(hdr);
	return NULL;	
    }
    
    /* skip fixed header, filename, and data fork len to find start of data fork */
    dinfo->offset = text_encl->offset + MACHOST_HDR_LEN + hdr->fname_len + 4; 
    strcpy(dinfo->fname, text_encl->fname);
					
    p = ((char *) &hdr->fname_len) + hdr->fname_len + 1; /* deal with varying-length filename */
    dinfo->len = getnetlong(p);		/* ...to get data fork length */
    dinfo->temp = FALSE;
    
    (void) t_fseek(f, dinfo->offset, SEEK_SET);	/* leave file positioned at start of data fork */
    t_free(hdr);
    
    return f;
}
/* binhex_enclosures --
	
    Deal with non-text enclosures in messages going into the internet world.
    Add explanatory note; recurse to deliver the modified message(s).
    Retain any text enclosures; they will be appended to the message.  If
    enclosure binhexing is enabled, include the binhex, otherwise discard the
    non-text enclosures.
        
    If a local box for the recipient is identifiable, deliver a copy of the
    message there (by adding the local box to the recip list, with the "oneshot"
    attribute.)
*/

void binhex_enclosures(char *sender, recip *rlist1, recip *rlist2, fileinfo *head, 
		fileinfo *text, enclinfo *text_encl, enclinfo *nontext_encl, summinfo *summ) {
					
    recip 	*reciphalf[2];		/* copy of recipients */
    recip	*rlist, *r, *new;
    int		i;
    enclinfo	*e;			/* one encl */
    t_file	*f = NULL;		/* file with edited message */
    fileinfo	xtext;			/* description of it */
    summinfo	xsumm;			/* summary info for it */
    char	logbuf[2*MAX_STR];
    long	baselen;		/* basic message length */
    
    reciphalf[0] = reciphalf[1] = NULL;
    
    /* create 2 new recipients lists, one for recips with a corresponding
       local mailbox, and one for the others */
       

    for (rlist = rlist1 ;; rlist = rlist2) {
	if (rlist) {
	    for (r = rlist->next ;; r = r->next) {
	    	/* ignore non-internet recips & errors */
		if (!r->nosend && !r->local && r->stat == RECIP_OK) {
		    i = (r->id != 0);	/* local uid? */
		    if (i && r->blitzserv == -1) {
			t_errprint_l("Bad BLITZSERV in dnd entry for uid %ld",
					 r->id);
			i = 0;		/* can't clone; don't know which server */
		    }
		    new = (recip *) mallocf(sizeof(recip));
		    *new = *r;		/* copy the recipient */
		    if (!reciphalf[i])	/* first one? */
			new->next = new;/* link to self */
		    else {		/* no - add to list */
			new->next = reciphalf[i]->next;
			reciphalf[i]->next = new;
		    }
		    reciphalf[i] = new;
		}
		    
		if (r == rlist)
		    break;		/* end of one list */
	    }	    
	}
    
	if (rlist == rlist2)
	    break;
    } 
    
    /* add any local boxes we identified to the original recip list */
    
    if (reciphalf[1]) {
	for (r = reciphalf[1]->next ;; r = r->next) {	
	    t_sprintf(logbuf,"Create enclosure clone %ld for %s %ld",
			summ->messid, r->name, r->id);
	    log_it(logbuf);
	    new = (recip *) mallocf(sizeof(recip));
	    *new = *r;			/* copy the recipient */
	    new->local = TRUE;		/* but this time use local box */
	    new->oneshot = TRUE;
	    new->noshow = TRUE;
	    new->vacation = FALSE;	/* these don't generate vacation msgs */
	    
	    if (rlist1)	{		/* first list exists? */
		new->next = rlist1->next;
		rlist1->next = new;
	    } else {			/* no, second must */
		new->next = rlist2->next;
		rlist2->next = new;
	    }
	    
	    if (r == reciphalf[1])
		break;
	}
    }

    xtext.temp = TRUE;			/* unlink this file when done */
    mess_tmpname(xtext.fname, m_spool_filesys, summ->messid);	
    strcat(xtext.fname, "encl");	/* base tempname on message id */
      
    if ((f = t_fopen(xtext.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("binhex_enclosures: cannot open ", xtext.fname);
	goto cleanup;
    } 
    if (text->len && !finfocopy(f, text)) { /* copy old text (if any) */
	t_perror1("binhex_enclosures: cannot copy ", text->fname);
	goto cleanup;
    }
    
    add_text_encl(f, text_encl, FALSE);	/* add enclosures; don't wrap twice */
    
    t_fprintf(f, "\r");
    t_fprintf(f, "---------------------------------------------------------------------\r");
    t_fprintf(f, "\r");
    if (m_binhexencls) {
	t_fprintf(f, "NOTE:  The following file(s) are enclosed with this message,\r");
	t_fprintf(f, "in BinHex format.  If your mail system does not convert BinHex\r");
	t_fprintf(f, "files automatically, you will need to save the message and\r");
	t_fprintf(f, "decode the file(s) with an application such as StuffIt Expander.\r");
    } else {
	t_fprintf(f, "NOTE:  The following Macintosh files were enclosed with this\r");
	t_fprintf(f, "message. They could not be included because BlitzMail cannot\r");
	t_fprintf(f, "deliver an enclosure outside the Macintosh mail system:\r");
    }
    t_fprintf(f, "\r");
    
    for (e = nontext_encl; e; e = e->next) {
	t_fprintf(f, "Filename: %s    Size:  %ld bytes\r", e->name, e->finfo.len);
    }

    baselen = t_fseek(f, 0, SEEK_END);	/* remember end of invariant part */
    xtext.offset = 0;			/* set up fileinfo & summinfo for new text */
    summ_copy(&xsumm, summ, FALSE);
    
    if (reciphalf[0]) {			/* that's it for non-locals */
	t_fprintf(f, "\r");
	t_fprintf(f, "---------------------------------------------------------------------\r");
	t_fprintf(f, "\r");
	    
	if (m_binhexencls)
	    encl2binhex(nontext_encl, f);   /* generate binhex */
	
	xtext.len = t_fseek(f, 0, SEEK_END);	/* compute lof */
	xsumm.totallen = xtext.len;		/* no encls, just text length */
	
	/* deliver that much to recips w/o a local mailbox */
	internet(sender, reciphalf[0], NULL, head, &xtext, NULL, &xsumm, FALSE);
    }
    
    /* if we delivered any copies locally, tell those folks */
    if (reciphalf[1]) {
	t_fseek(f, baselen, SEEK_SET);	/* overwrite end of 1st version */
	t_fprintf(f, "\r");
	t_fprintf(f, "For your convenience, a copy of the message has also been delivered\r");
	t_fprintf(f, "to your BlitzMail account; if you prefer, you can read it there.\r");
	t_fprintf(f, "\r");
	t_fprintf(f, "---------------------------------------------------------------------\r");
	t_fprintf(f, "\r");

	if (m_binhexencls)
	    encl2binhex(nontext_encl, f);   /* generate binhex */
	
	xtext.len = t_fseek(f, 0, SEEK_END);	/* adjust length again */
	xsumm.totallen = xtext.len;
	
	internet(sender, reciphalf[1], NULL, head, &xtext, NULL, &xsumm, FALSE);
    }
    
    cleanup:				/* here on error */
    
    free_recips(&reciphalf[0]);
    free_recips(&reciphalf[1]);
    if (f) {
	(void) t_fclose(f);		/* close the file */
	finfoclose(&xtext);		/* and unlink it */
    }
}

/* do_bounces --

    Scan a recipient list, calling bad_mail to send a bounce message
    for any bad addresses.  Note that bad addresses are usually
    rejected much earlier than this (before we've accepted the message).
    The exception is bad addresses that are within a public mailing
    list.  In that case, we accept the flawed recipient list (so it's
    possible to send to the part of the list that isn't bad), and then
    send bounces for the bad ones.  
*/

void do_bounces(char *sender, recip *rlist, fileinfo *head, fileinfo *text, summinfo *summ) {
		
    char	bounce_addr[MAX_ADDR_LEN]; /* address that gets the bounce */
    char	reason[MAX_ADDR_LEN+128]; /* error text */
    char	*badname;		/* name/addr that failed */
    recip	*r;
		
    if (!rlist)			/* null list - don't bother */
    	return;
    
    /* bounce goes where msg came from, not to preferred address */
    strcpy(bounce_addr, sender);
    if (!isinternet(bounce_addr, NULL) && strlen(bounce_addr) > 0)
    	addhost(sender, bounce_addr, m_hostname);
	
    for (r = rlist->next ;; r = r->next) {
    	if (r->stat != RECIP_OK) {
	    badname = r->name[0] ? r->name : r->addr; /* get bad name/addr */
	    switch (r->stat) {
		case RECIP_AMBIGUOUS:
		    strcpy(reason, "Ambiguous user name:  ");
		    strcat(reason, badname);
		    break;
		case RECIP_BAD_ADDRESS:
		    strcpy(reason, "No match found for this name: ");
		    strcat(reason, badname);
		    break;
		case RECIP_NO_SEND:
		    strcpy(reason, "Mailing list access denied: ");
		    addhost(badname, reason + strlen(reason), m_hostname);
		    break;
		case RECIP_NO_DND:
		    strcpy(reason, "Name directory (DND) not available at this time.");
		    break;
		case RECIP_LOOP:
		    strcpy(reason, "Forwarding loop detected:  ");
		    strcat(reason, badname);
		    break;	    	    
	    }
	    
	    bad_mail(bounce_addr, badname, head, text, summ, reason);
	}
    	if (r == rlist)			/* end of circular list */
	    break;
    }
}

/* bad_mail --

    Construct a new message containing the error text and the header of
    the old message; return it to the sender.  
    This is (hopefully) a relatively infrequent happening (we try to catch 
    most errors before accepting the message.)
    
    The two cases in which this routine is called are:
    
    - when sendmail returns an error (refusing to queue an outgoing message)
    - when a public mailing list contains an invalid address
    
*/

void bad_mail(char *send_addr, char *recip_addr, fileinfo *head, fileinfo *text, 
		summinfo *summ, char *reason) {
					
    recip	*rlist;			/* recip list for the bounce */
    int		recipcount;		/* number of recips it resolves to */
    t_file	*f;			/* to construct bounce */
    fileinfo	newtext;		/* .. */
    summinfo	newsumm;		/* summary info for bounce msg */
    char	buf[MAX_ADDR_LEN+256];
    
    /* if we (postmaster) originated the message, don't bounce it! */
    if (macmatch(POSTMASTER, send_addr)) {
	t_sprintf(buf, "Postmaster message returned: %ld %s", summ->messid, reason);
	log_it(buf);
	return;
    }

    t_sprintf(buf, "Returning message %ld to %s: %s",
		summ->messid, send_addr, reason);
    log_it(buf);

    rlist = resolve(send_addr, NULL, &recipcount);	/* resolve the bounce addr */
    
    mess_tmpname(newtext.fname, m_spool_filesys, summ->messid);	
    strcat(newtext.fname, "bounce");	/* base tempname on message id */
  
    if ((f = t_fopen(newtext.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("bad_mail: cannot open ", newtext.fname);
	return;
    }
    
    t_fprintf(f, "Your message could not be sent to %s", recip_addr);
    t_fprintf(f, " for the following reason:\r%s\r", reason);
    t_fprintf(f, "\rThe header of the unsent message follows:\r\r");
   
    finfocopy(f, head);			/* append header */
    t_putc(f, '\r');			/* blank line */
#if 0
    if (text->len)			/* (text might be null) */
	finfocopy(f, text);		/* append original message text */
#endif
  
    /* now set up relevant parts of the summary info */
    newsumm.len = sizeof(newsumm);
    newsumm.totallen = t_fseek(f, 0, SEEK_END); /* length of message text */
    newsumm.enclosures = 0;
    newsumm.receipt = FALSE;
    newsumm.type = MESSTYPE_BLITZ;	/* blitz format */
    newsumm.sender = newsumm.sender_;	/* unpacked form */
    newsumm.topic = newsumm.topic_;
    newsumm.recipname = newsumm.recipname_;
    strcpy(newsumm.topic,"Failed mail"); /* need to set this */
    *newsumm.recipname = 0;		/* but this doesn't matter */
    *newsumm.sender = 0;		
        
    /* create "fileinfo" describing the file */
    newtext.offset = 0;
    newtext.len = newsumm.totallen;
    newtext.temp = TRUE;		/* unlink file when done */
    t_fclose(f);    
    
    deliver(NULL, POSTMASTER, rlist, NULL, NULL, &newtext, NULL, &newsumm, FALSE, NULL, FALSE);
    ++m_sent_bounce;			/* statistics: count bounce messages */
    
    finfoclose(&newtext);
    free_recips(&rlist);
}

/* do_vacations --

    Send off vacation messages for any recips in the list that requested them.
    This is done whenever we call blitzdeliver to deliver/send off messages.

*/
void do_vacations(recip *rlist, fileinfo *head, summinfo *summ) {

    recip	*r;
    boolean_t	resolved = FALSE; 	/* sender resolved yet? */
    char	send_addr[MAX_ADDR_LEN];/* sender's address (w/ host name) */
    recip	*send_rlist = NULL;	/* sender in recip form */
    int		recipcount;		/* how many people sender resolves to */
    
    if (rlist == NULL)			/* any recips at all? */
	return;
	
    for (r = rlist->next ;; r = r->next) {

	if (r->vacation) {		
	    if (!resolved) {	/* don't resolve sender unless we have to */
		if (!get_vacation_from(head, send_addr)) /* get From: address */
		    return;		/* something that shouldn't be replied to */
		    
		/* resolve address; using no mailing lists */
		send_rlist = resolve(send_addr, NULL, &recipcount);
		resolved = TRUE;
	    }
	    
	    if (recipcount != 1) { /* if we can tell reply would go to a list, don't send */
		break;			/* stop processing recips right now */
	    }
	    
	    send_vacation(send_addr, send_rlist, r); /* send one */	
	}

	if (r == rlist)			/* end of circular list */
	    break;
    }

    free_recips(&send_rlist);		/* clean up recip list */

    return;
}

/* get_vacation_from --

    Scan header of message we're about to automatically reply to.  Locate the "From:"
    field, and extract the address part of it.  Ignore messages that appear to be
    machine-generated, or directed to mailing lists.
*/

boolean_t get_vacation_from(fileinfo *head, char *send_addr) {

    t_file	*f;
    char	send_name[MAX_ADDR_LEN];	/* sender name (don't care) */
    long	remaining;			/* length left to read */
    char	*s;				/* header line */

    /* read header to locate sender's address */
    if ((f = t_fopen(head->fname, O_RDONLY, 0)) == NULL) {
	t_perror1("get_vacation_from: cannot open ", head->fname);
	return FALSE;    
    }
      
    *send_addr = 0; 				/* nothing yet */
    
    (void) t_fseek(f, head->offset, SEEK_SET);	/* start at beginning of header */
    
    remaining = head->len;
    while((s = getheadline(f, &remaining, TRUE)) != NULL) {
    	/* check for things that we shouldn't reply to */
	if (strncasecmp(s, "FROM: ", strlen("FROM: ")) == 0
	       ||  strncasecmp(s, "RETURN-PATH: ", strlen("RETURN-PATH: ")) == 0
	       ||  strncasecmp(s, "ERRORS-TO: ", strlen("ERRORS-TO: ")) == 0
	       ||  strncasecmp(s, "SENDER: ", strlen("SENDER: ")) == 0) {
	
	    if (strcasematch(s, "-REQUEST")	/* does this look like a mailing list msg? */
	    ||  strcasematch(s, "-DIGEST")	
	    ||  strcasematch(s, "-L@")
	    ||  strcasematch(s, "OWNER-")	
	    ||  strcasematch(s, "LISTSERV")	
	    ||  strcasematch(s, "AUTO ACK")	/* apple dev. support ack */
	    ||  strcasematch(s, "POSTMASTER")	/* or a bounce? */
	    ||  strcasematch(s, "-DAEMON")
	    ||  strcasematch(s, "MAILER-AGENT")
	    ||  strcasematch(s, "MAILING LIST"))
	    	break;				/* don't reply to those */
	    
	} else if (strncasecmp(s, "TO: ", strlen("TO: ")) == 0
	       ||  strncasecmp(s, "CC: ", strlen("CC: ")) == 0
	       ||  strncasecmp(s, "BCC: ", strlen("BCC: ")) == 0) {
	    if (strcasematch(s, "MULTIPLE RECIPIENTS")	/* conventional mailing list to: ? */
	       || strcasematch(s, "MAIL NEWSGROUP"))
		break;				/* no reply */
		
	} else if (strncasecmp(s, "PRECEDENCE: ", strlen("PRECEDENCE: ")) == 0) {
	    if (strcasematch(s, "BULK")
	    ||  strcasematch(s, "JUNK")) {	/* don't reply to junk mail */
		break;
	    }
	}

	/* extract address from From: line */
	if (strncasecmp(s, "FROM: ", strlen("FROM: ")) == 0) {
	    splitname(s + strlen("FROM: "), send_name, send_addr); /* locate address part */
	    if (local(send_addr)) {		/* local address? */
		strcpy(send_name, send_addr);	/* (scratch copy) */
		addhost(send_name, send_addr, m_dndhost[0]); /* normalize to DND form for comparison */
	    }
	}
	t_free(s);
    }
    t_fclose(f);
    
    if (s) {					/* broke out before eof? */
	t_free(s);
	return FALSE;				/* --> we found something bad */
    }
    
    return strlen(send_addr) > 0;		/* ok, assuming we located an address */
}

/* send_vacation --

    Construct & send a single vacation message.  The VACATION_LIST file
    in the user mailbox is a list of addresses that have already received
    this vacation message; check the list (if we're on it, don't send,
    otherwise add us to it).
*/

void send_vacation(char *send_addr, recip *send_rlist, recip *r) {

    summinfo	newsumm;		/* summary info for vacation msg */
    t_file	*text = NULL;		/* vacation text */
    t_file	*list = NULL;		/* list of vacation recipients */
    fileinfo	finfo;		
    mbox	*mb;			
    char	*replyto;
    char	s[2*MAX_ADDR_LEN+MAX_STR];

    /* set up relevant parts of the summary info */
    newsumm.len = sizeof(newsumm);
    newsumm.type = MESSTYPE_BLITZ;
    newsumm.sender = newsumm.sender_;	/* unpacked format */
    newsumm.topic = newsumm.topic_;
    newsumm.recipname = newsumm.recipname_;
    
    *newsumm.sender = 0;			/* n/a */
    if (strlen(r->name) > 0)			/* replies to user, not postmaster */
	replyto = r->name;
    else					/* (use addr if name not available) */
	replyto = r->addr;
	
    strcpy(newsumm.topic, replyto);
    strcat(newsumm.topic, " -- Automatic Reply");
    newsumm.enclosures = 0;
    newsumm.receipt = FALSE;    
    mb = mbox_find(r->id, fs_match(r->blitzfs), FALSE);	/* locate user's box */
    sem_seize(&mb->mbsem);		/* synchronize with ourself */
 
    /* vacation message may have been cleared since addr resolved */
    if (!pref_get_int(mb, PREF_VACATION, s)) { /* message still active? */
	sem_release(&mb->mbsem);
	goto cleanup;			/* no - never mind */
    }
    /* check file listing vacations already sent */
    strcpy(finfo.fname, mb->boxname);
    strcat(finfo.fname, VACATION_LIST);	

    if ((list = t_fopen(finfo.fname, O_RDWR | O_CREAT, FILE_ACC)) == NULL) {
	if (pthread_errno() != ENOENT)
	    t_perror1("send_vacation: open ", finfo.fname);
	sem_release(&mb->mbsem);
	goto cleanup;
    }
    
    while(t_gets(s, sizeof(s), list)) {	/* search the file */
	if (strcasecmp(s, send_addr) == 0) {
	    t_sprintf(s, "Not sending duplicate vacation msg from %s to %s",
	    		replyto, send_addr);
	    log_it(s);
	    sem_release(&mb->mbsem);
	    goto cleanup;		
	}
    }
    
    /* not in file yet; add us */
    t_fflush(list);
    t_fprintf(list,"%s\n", send_addr);
    t_fclose(list); list = NULL;
    sem_release(&mb->mbsem);
    
    /* ok, we DO want to send; get the text */
    strcpy(finfo.fname, mb->boxname);
    strcat(finfo.fname, VACATION_FNAME);	
    if ((text = t_fopen(finfo.fname, O_RDONLY, 0)) == NULL) {
	if (pthread_errno() != ENOENT)
	    t_perror1("send_vacation: cannot open ", finfo.fname);
	pref_rem_int(mb, PREF_VACATION);	/* oh well - no vacation file after all */
    } else {
	newsumm.totallen = t_fseek(text, 0, SEEK_END); /* set the length */
	/* create "fileinfo" describing the file */
	finfo.offset = 0;
	finfo.len = newsumm.totallen;
	finfo.temp = TRUE;		/* unlink file when done */
	t_fclose(text);
	deliver(NULL, POSTMASTER, send_rlist, NULL, NULL, &finfo, NULL, &newsumm, FALSE, replyto,FALSE);
	++m_sent_vacation;		/* statistics: count vacations sent */

    }
    
cleanup: /* close files & release box */
    if (list)
	t_fclose(list);
    mbox_done(&mb);
    
    return;
}

/* do_receipt --

    Construct and send a return receipt message.  Note that several receipts
    for the same message might be under construction simultaneously, so a
    further level of tempfile discriminator is needed.
*/

void do_receipt(char *recip_name, summinfo *summ, fileinfo *head) {
					
    recip	*rlist;			/* recip list for the receipt */
    int		recipcount;		/* number of recips it resolves to */
    t_file	*f;			/* to construct receipt */
    fileinfo	newtext;		/* .. */
    summinfo	newsumm;		/* summary info for receipt msg */
    long	remaining;		/* header length to go */
    char	*s;
    char	buf[HEAD_MAXLINE];
    char	receipt[MAX_ADDR_LEN];
    char	receiptname[MAX_ADDR_LEN];
    char	receiptaddr[MAX_ADDR_LEN];
    char	*replyto;
    
#define RCPT_TEXT	"RETURN-RECEIPT-TO: "

    /* read header to locate receipt address */
    if ((f = t_fopen(head->fname, O_RDONLY, 0)) == NULL) {
	t_perror1("do_receipt: cannot open ", head->fname);
	return;    
    }
      
    (void) t_fseek(f, head->offset, SEEK_SET);	/* start at beginning of header */
    
    *receipt = 0;
    remaining = head->len;
    while((s = getheadline(f, &remaining, TRUE)) != NULL) { /* (sic) */
	if (strncasecmp(s, RCPT_TEXT, strlen(RCPT_TEXT)) == 0)
	    strcpy(receipt, s + strlen(RCPT_TEXT));
	t_free(s);
    }
    t_fclose(f);
    
    if (!*receipt) {
	t_sprintf(buf, "Can't find return receipt address for message %ld", summ->messid);
	log_it(buf);
	return;
    }
    
    /* parse name/addr to get just the address */
    splitname(receipt, receiptname, receiptaddr);
    
    rlist = resolve(receiptaddr, NULL, &recipcount); /* resolve the receipt addr */
 
    temp_finfo(&newtext);			/* set up temp file */
  
    if ((f = t_fopen(newtext.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("do_receipt: cannot open ", newtext.fname);
	return;
    }

    get_date(buf);
    
    t_fprintf(f, "Your message was read by %s at %s\r", recip_name, buf);
    t_fprintf(f, "\rThe header of your message follows:\r\r");
   
    finfocopy(f, head);			/* append original header */
    
    /* now set up relevant parts of the summary info */
    newsumm.len = sizeof(newsumm);
    newsumm.sender = newsumm.sender_;	/* unpacked format */
    newsumm.topic = newsumm.topic_;
    newsumm.recipname = newsumm.recipname_;
    
    *newsumm.sender = 0;			/* n/a */
    replyto = recip_name;		/* replies to user, not postmaster */
    strcpy(newsumm.topic, recip_name);
    strcat(newsumm.topic, " -- Return receipt");
    newsumm.totallen = t_fseek(f, 0, SEEK_END);
    newsumm.enclosures = 0;
    newsumm.receipt = FALSE;
    newsumm.type = MESSTYPE_BLITZ;
    
    /* create "fileinfo" describing the file */
    newtext.offset = 0;
    newtext.len = newsumm.totallen;
    newtext.temp = TRUE;			/* unlink file when done */
    t_fclose(f);
    
    /* don't send receipts to self */
    if (strcasecmp(receiptaddr, replyto) != 0) {
    	deliver(NULL, POSTMASTER, rlist, NULL, NULL, &newtext, NULL, &newsumm, FALSE, replyto, FALSE);
	++m_sent_receipt;			/* statistics: count receipts sent */
    }
    
    finfoclose(&newtext);
    free_recips(&rlist);
}

/* exportmess --

    Convert internal message to RFC822 format (almost -- use \n as line terminator).  
    
    If the message is MESSTYPE_BLITZ, long paragraphs of text are converted to
    reasonable-length lines by "wrap"; if it's MESSTYPE_RFC822 that's not necessary.
    
    In either case, the header needs special processing to convert addresses
    to internet format.  The fields that need special attention are address fields
    that we (or a peer server) generate.  The addresses need to be run through addhost,
    and line breaks may need to be inserted.  
            
    "textmess" will be set up to describe the new text message.  The open message file
    is also returned, for convenience.
    
    There's one sendmail-dependent feature here:  the bcc line is prefixed with a
    sendmail conditional (?F? -- show this line to mailers with the "F" flag set
    in sendmail.cf) that has the effect of preventing sendmail from eating the
    bcc line.  There doesn't appear to be a way to do this using the sendmail
    config file...
*/

t_file *exportmess(long messid, fileinfo *head, fileinfo *text, 
		   fileinfo *textmess, long mtype) {
					
    char	*s;			/* header line */
    t_file	*in;			/* mac-format message */
    t_file	*out;			/* text message */
    char 	c;			/* current char of address */
    int		comment;		/* address parsing state: */
    boolean_t	quot;			/* '' */
    boolean_t	route; 			/* '' */
    boolean_t	esc;			/* '' */
    long	remaining;		/* header left */
    boolean_t	sender_field;		/* field is "From:" etc. */
    boolean_t	recip_field;		/* field is "To:" etc. */
    char	addr[MAX_ADDR_LEN];
    char	*addrp;
    int		linepos;
    char	*s1;
    static int	serial = 1;		/* temp file discriminator */
    int		i;
    
    mess_tmpname(textmess->fname, m_spool_filesys, messid);	
    strcat(textmess->fname, "exp");	/* base tempname on message id */
    pthread_mutex_lock(&global_lock);	/* ...adding serial number in case 2 threads... */
    numtostr(serial++, textmess->fname + strlen(textmess->fname)); /* ... are using same mess */
    pthread_mutex_unlock(&global_lock);

    if ((out = t_fopen(textmess->fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL)
	return NULL;			/* open failed */
    
    textmess->temp = TRUE;		/* unlink this file when done */
    textmess->offset = 0;
    
    if ((in = t_fopen(head->fname, O_RDONLY, 0)) == NULL) {
    	t_perror1("exportmess: cannot open header ", head->fname);
	t_fclose(out);
	finfoclose(textmess);
	return NULL;
    }
    (void) t_fseek(in, head->offset, SEEK_SET); /* seek to start of header */
    remaining = head->len;		/* this much to read */
    
    /* read header line-by-line, looking for fields to edit */
    while(s = getheadline(in, &remaining, TRUE)) {	/* (sic) */
	sender_field = recip_field = FALSE;
	
    	if (strncasecmp(s, "FROM:", strlen("FROM:")) == 0
	  || strncasecmp(s, "REPLY-TO:", strlen("REPLY-TO:")) == 0
	  || strncasecmp(s, "RETURN-RECEIPT-TO:", strlen("RETURN-RECEIPT-TO:")) == 0) {
	    sender_field = TRUE;
	}
	if (strncasecmp(s, "TO:", strlen("TO:")) == 0
	  || strncasecmp(s, "CC:", strlen("CC:")) == 0
	  || strncasecmp(s, "BCC:", strlen("BCC:")) == 0) {
	    recip_field = TRUE;
	}
	
	/* if an address field, rewriting needed */
	if (sender_field || recip_field) {
	
	    if (!m_server[SMTP_HOSTNUM]) {	/* iff we're using sendmail for delivery */
		if (strncasecmp(s, "BCC:", strlen("BCC:")) == 0)
		    t_puts(out, "?F?");	/* make sendmail not eat this bcc */
	    }
	    
	    s1 = index(s, ':');		/* copy the field name */
	    *s1 = 0;
	    t_puts(out, s); t_putc(out, ':'); t_putc(out, ' ');
	    *s1++ = ':';		/* restore the ":" we nailed */
	    linepos = (s1 - s) + 2;	/* column position in output msg */
	    
	    /* now parse & export addresses */
	    comment = 0;
	    quot = route = esc = FALSE;
	    addrp = addr;
	    if (*s1 == ' ')		/* skip ": " */
		++s1;		
		
	    while ((c = getaddrc(&s1, &comment, &quot, &route, &esc)) != 0) {
		if (c == ',' && comment == 0 && !quot && !route) {
		    exportaddr(out, addr, &linepos, TRUE);
		    addrp = addr;	/* reset for next one */
		} else {		/* copy, mapping special chars */
		    /* if address is obscenely long, break it now */
		    if ((addrp-addr) + strlen(mac_char_map[(u_char) c]) >= MAX_ADDR_LEN) {
			exportaddr(out, addr, &linepos, FALSE);
			addrp = addr;	/* reset for next one */
		    }
		    strcpy(addrp, mac_char_map[(u_char) c]); 
		    addrp += strlen(addrp);			
		}
	    }
	    *addrp++ = 0;
	    exportaddr(out, addr, &linepos, FALSE);
	} else {			/* no rewriting, but check special chars */
	    for (s1 = s; *s1; s1++)	/* note that index must be unsigned */
		t_puts(out, mac_char_map[*((u_char *) s1)]);
	}
	t_putc(out, '\n');		/* either way, end the line */
	
    	t_free(s);
    }					/* (end of header) */
    t_fclose(in);			/* close header */
    
    t_putc(out, '\n');			/* blank line to mark end of header */
    
    if (text->fname[0]) {		/* text may be null */
	if ((in = t_fopen(text->fname, O_RDONLY, 0)) == NULL) {
	    t_perror1("exportmess: cannot open text ", text->fname);
	    t_fclose(out);
	    finfoclose(textmess);
	    return NULL;
	}
	
	if (mtype == MESSTYPE_BLITZ) {	/* Mac-format text? */
	    wrap(text, in, out);	/* word-wrap & append */
	} else {			/* already RFC822; just convert line endings */
	    (void) t_fseek(in, text->offset, SEEK_SET); /* seek to start of text */
	    
	    /* copy in -> out mapping \r to \n */
	    for (i = 0; i < text->len; ++i) {
		c = t_getc(in);	
		t_putc(out, c == '\r' ? '\n' : c);
	    } 
	}
	t_fclose(in);			/* close text */
    }
    textmess->len = t_fseek(out, 0, SEEK_END); /* compute total length */
    
    return out;
}

/* exportaddr --

    If address doesn't appear to be in internet format, add DND hostname.  The
    address may contain a comment field at this point; strip it out before dealing
    with the address, then replace it later.

    Addresses are given the canonical DND hostname, (rather than our local hostname), so
    replies go to the preferred address, not necessarily the local box.  
    The exception is public mailing lists:  they must point back here since only we,
    not the DND, can expand them.  Since it's impossible to distinguish between a
    mailing list and an ordinary recipient just by the text of the address, and since
    the export may take place on a different server from the one that originated
    the message, public mailing lists are always given the @mac hostname, even
    for local messages.  Messages from "postmaster" are treated similarly -- we
    want replies to go to the blitz postmaster list, not the general one for
    the institution as a whole.
        
    A certain amount of (mostly harmless)
    rewriting is done here in an attempt to gloss over certain common errors:

        - leading/trailing spaces are ignored
        - multiple spaces are collapsed
        - spaces around an "@" or "." are removed
        - any remaining spaces are turned into periods
        - trailing '.'s are removed

    Collapsing multiple spaces and removing spaces around "@" and "."
    accomplishes the RFC822 canonicalization, fixing up any bizarre
    but legal things like "fred@foo.(comment)edu".

    Mapping the remaining spaces to periods is intended to help out the user
    in cases like:

      " Fred Flintstone @ hinman " --> "Fred.Flintstone@hinman"
    Start new line (and indent) if need be.
*/

void exportaddr(t_file *out, char *addr, int *linepos, boolean_t comma) {

    char	addrcomment[MAX_ADDR_LEN];/* address comment, if any */
    char	expaddr[MAX_ADDR_LEN];	/* exported address */
    char        c;                      /* current char of address */
    int         comment = 0;            /* address parsing state: */
    boolean_t   quot = FALSE;           /* '' */
    boolean_t   route = FALSE;          /* '' */
    boolean_t   esc = FALSE;            /* '' */
    boolean_t	sawroute = FALSE;	/* '' */
    int         spacecount = 0;         /* '' */
    char	*p1, *p2;
    
    trim_comment(addr, addrcomment);	/* stip address comment */

    /* scan address to see if it's in "full name <addr@domain>" format */
    p1 = addr;
    while ((c = getaddrc(&p1, &comment, &quot, &route, &esc)) != 0) {
	if (route)
	    sawroute = TRUE;
    }

    p1 = addr; p2 = expaddr;            /* set up to copy */
    while ((c = getaddrc(&p1, &comment, &quot, &route, &esc)) != 0) {
        if (quot)                       /* if quoted, copy unchanged */
            *p2++ = c;
        else {

            if (c == ' ' && !sawroute)  /* accumulate multiple spaces */
                ++spacecount;
            else {                      /* non-space */
                if (spacecount) {       /* deal with any preceding spaces */
                    if (c != '@' && c != '.'
                                && (p2 == expaddr || *(p2-1) != '@')
                                && (p2 == expaddr || *(p2-1) != '.'))
                        *p2++ = '.';    /* except near '.' or '@', ' '* -> '.' */
                    spacecount = 0;
                }
                *p2++ = c;
            }
        }
    }
   /* remove trailing '.'s */
    while (p2 > expaddr && p2[-1] == '.')
        --p2;

    *p2 = 0;                            /* terminate expaddr */

    if (!isinternet(expaddr, NULL) && strlen(expaddr) > 0
      && !sawroute) {	/* name <addr> syntax is too hard; don't touch it */
	/* special-case "postmaster" */
	if (strcasecmp(POSTMASTER, addr) == 0)
	    addhost(addr, expaddr, m_hostname);	/* add local hostname */
	else {				/* unqualified name - check DND for vanity domain */
            static char *email_farray[] = {"NAME", "EMAIL", NULL};
     	    struct dndresult *dndres=NULL;

	    if (t_dndlookup1(addr,email_farray,&dndres) == DND_OK) { 
                strcpy(expaddr,dndres->value[1]); /* use DND EMAIL field */
		t_free(dndres);
 	    } else {
	    	addhost(addr, expaddr, m_dndhost[0]);	/* DND lookup failed; use generic host */
	    }
	    if (strlen(addr) + 3 + strlen(expaddr) + 1 < MAX_ADDR_LEN
	     && strlen(addrcomment) == 0) {	/* if already a comment, don't add one */
	    	strcat(expaddr, " (");	/* if room... */
		strcat(expaddr, addr);	/* ...append unqualified name as a comment... */
		strcat(expaddr, ")");	/* ...e.g., Fred.Flintstone@foo.com (Fred Flintstone) */
	    }
	}
    }
    
    /* replace any comment, iff there's room */
    if (addrcomment[0] && strlen(addrcomment) + 3 + strlen(expaddr) + 1 < MAX_ADDR_LEN) {		
	strcat(expaddr, " (");
	strcat(expaddr, addrcomment);
	strcat(expaddr, ")");
    }
     
    if (*linepos + strlen(expaddr) + 2*comma > FOLD_MARGIN) {
	t_putc(out, '\n');
	t_putc(out, '\t');		/* start new line, indent by a tab */
	*linepos = 8;
    }
    
    t_puts(out, expaddr);		/* put the address */
    *linepos += strlen(expaddr);
    
    if (comma) {			/* if more to come, delimit */
	t_putc(out, ',');
	t_putc(out, ' ');
	*linepos += 2;
    }
}
/* wrap --

    Convert message text to RFC822-legal format -- plain ascii, reasonable line
    lengths, etc.
    
    When breaking the line, any excess whitespace characters located at the point
    we choose to break are left at the end of the previous line.  This means
    that the line may acutally be longer than the margin setting,  although
    the excess will always be spaces, not printing characters.  The idea is
    a) remove only 1 whitespace char when breaking, and b) avoid falsely
    indenting the next line in cases where a sentence (with 2 spaces after
    the period) ends right at the margin.
*/

void wrap(fileinfo *finfo, t_file *in, t_file *out) {

#define 	MARGIN    	80
#define		MAX_MARGIN	120	/* absolute max, even if just whitespace */
#define		TABSTOP		8
					
    int		i, j;
    int		c;			/* next (mac) char */
    int		charlen;		/* its length when converted */
    char	line[256];		/* at least MAX_MARGIN + widest char */
    char	*linep;
    char	*p;
    char	*breakp;		/* where to break */
    int		linepos;		/* width, including tab expansion allowance */ 
 
    (void) t_fseek(in, finfo->offset, SEEK_SET); /* seek to start of text */
    
    linep = line; line[0] = 0;
    linepos = 0;
    	
    for (i = 0; i < finfo->len; ++i) {
	c = t_getc(in);	
	if (c == '\r') {		/* forced break? */
	    t_puts(out, line);		/* write any line */
	    t_putc(out, '\n');		/* add the eol */
	    linepos = 0;		/* line is empty */
	    linep = line; line[0] = 0;
	    continue;
	}
	
	strcpy(linep, mac_char_map[(u_char) c]);	/* append the char */
	charlen = strlen(mac_char_map[(u_char) c]);
	linep += charlen;		/* advance */
			
	if (c == '\t')
	    linepos = (linepos + TABSTOP) & -TABSTOP;	/* advance to next tab */
	else
	    linepos += charlen;
	    
	if (linepos >= MARGIN) {	/* time to wrap? */
	    if ((c == ' ' || c == '\t') && linepos < MAX_MARGIN)
	    	continue;		/* wait for printing char before breaking */
	    breakp = NULL;
	    for (p = linep-1; p >= line; --p) {
		if (*p == ' ' || *p == '\t') {
		    breakp = p;		/* find last whitespace char */
		    break;
		}
	    }
	    
	    if (!breakp) {		/* line is all one word */
		for (j = 0; j < MARGIN; ++j)
		    t_putc(out, line[j]); /* break word at margin */
		*linep = 0;
		strcpy(line, line+MARGIN); /* slide down */
	    } else {			/* remove 1 whitespace char */
		*breakp = 0;
		t_puts(out, line);	/* everything before break */
		strcpy(line, breakp+1); /* keep everything after */
	    }
	    
	    t_putc(out, '\n');		/* add the eol */
	    linepos = strlen(line);	/* know there are no tabs yet... */
	    linep = line + linepos;
	}
    }
    
    if (linepos > 0) {			/* file ended in middle of line */
	t_puts(out, line);		/* write the final line */
	t_putc(out, '\n');		/* and terminate it */
    }
    
}
/* do_notify --

    Find notification server & send request to it.
    If the notification type is negative, clear an existing notification
    (instead of sending a new one).
    
    A timed out connection is retried (once).
*/

void do_notify(long uid, int typ, long id, char *data, int len, boolean_t sticky) {

    int 	tries = 0;	/* retry counter */
    char	buf[MAX_STR];	/* response from server */

    sem_seize(&not_sem);
    for (tries = 0; tries < 2; ++tries) { /* retry timeouts once */
	/* find notification server if we're not connected */
	if (not_f == NULL) {
	    if ((not_f = notify_connect()) == NULL)
		break;		/* not there; don't loop waiting for it */
	}


	if (typ < 0) {		/* clearing? */
	    t_fprintf(not_f, "CLEAR %ld,%d\r\n", uid, -typ);
	} else {
	    t_fprintf(not_f, "NOTIFY %ld,%ld,%ld,%ld,%d\r\n",
		    len, uid, typ, time(NULL), TRUE);
		    
	    t_fwrite(not_f, data, len); /* caution: may contain nulls! */
	}
	
	if (checkresponse(not_f, buf, NOT_OK)) 
	    break;		/* good status; all set */

	/* some kind of error; discard the file & see if we should retry */	
	t_fclose(not_f);
	not_f = NULL;
	
	if (strlen(buf) > 0) {	/* we got something, but not what we wanted */
	    t_errprint_s("Unexpected response from notify server: %s", buf);
	    break;		/* hard error - don't retry */
	} else 
		;		/* connection timed out -- retry */
		
	sleep(5);		/* retry slowly */
    }
	    
    sem_release(&not_sem);

}
/* notify_connect --

    Return connection to notify server.
*/

t_file *notify_connect() {

    int		sock;		/* connection to server */
    t_file	*f;		/* returned: corresponding file */
    char	buf[MAX_STR];

    struct sockaddr_in	sin;	/* local notify server addr */
    struct servent *sp = NULL;	/* services entry */


    sin.sin_family = AF_INET;	/* internet address... */
    sin.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* ... of this host */

    sem_seize(&herrno_sem);		/* serialize access to getservbyname */
    
    /* (note: look up services entry each time, in case it changes) */
    if ((sp = getservbyname(NOTIFYPORT, "tcp")) == NULL) {
	t_perror1("notify_connect: getservbyname: ", NOTIFYPORT);
	sem_release(&herrno_sem);
	return NULL;
    }
    sin.sin_port = sp->s_port;	/* get port from services entry */    
    sp = NULL;			/* sppml */

    sem_release(&herrno_sem);	/* don't hold other people up while we connect */
 
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    	t_perror("notify_connect: socket");
	return NULL;
    }
    
    /* try to connect; may fail if server is down */
    if (connect(sock, (struct sockaddr *)&sin, sizeof (sin)) < 0) {
	/* If they are merely down; log to file but not to console */
	if (pthread_errno() == ETIMEDOUT) {
	    t_sprintf(buf, "notify_connect: connection attempt timed out");
	    log_it(buf);
	} else if (pthread_errno() == ECONNREFUSED) {
	    t_sprintf(buf, "notify_connect: connection refused");
	    log_it(buf);	
	} else 
	    t_perror("notify_connect: connecting to notify server");
	close(sock);
	return NULL;
    }

    
    /* connection established; try for greeting message */
    
    f = (t_file *) mallocf(sizeof(t_file));
    t_fdopen(f, sock);			/* associate file w/ socket */
    
    /* record name as debugging aid */
    strcpy(f->name, "Notify server");

    if (!checkresponse(f, buf, NOT_GREET)) {
	if (f->t_errno || strlen(buf) == 0)
	    ;				/* lost connection; don't log */
	else
	    t_errprint_s("Unexpected greeting from notify server:%s\n", buf);
	t_fclose(f);
	return NULL;
    }
    
    return f;
   
}
