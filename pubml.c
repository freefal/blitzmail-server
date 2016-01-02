/*  BlitzMail Server -- Public Mailing lists
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/pubml.c,v 3.4 98/04/27 13:59:26 davidg Exp Locker: davidg $
    
    Public mailing lists are stored in the same format as personal lists, in
    a special mailbox directory dedicated to that purpose.  Unlike personal
    lists, public lists have access permissions associated with them; these
    are implemented using preferences.
    
    The user should see the  same set of public mailing lists, no matter which
    server they happen to use.  Each server has a copy of all the mailing
    lists (there's no central authority).  List queries always take place 
    using the data on the local server; it's only when a change is made that
    inter-server communication is required.  When a change is made on a given
    server, an update message (giving the new contents of the list) is mailed 
    to all the other servers.  A timestamp is associated with the list, so
    the receiving server can choose the most recent data.  
    
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "mess.h"
#include "client.h"
#include "deliver.h"

boolean_t pubml_parsectl(char *value, u_long *modtime, long *owner, long *group, 
			char *lacc, boolean_t *removed);
void pubml_sendupdate(char *name, u_long modtime, long owner, long group, 
			char *lacc, boolean_t removed, ml_data *contents);						
/* pubml_acc --

    Compute mailing list access rights for a given user.
    Global mailing lists have an owner, group, and access bitmap
    associated with them.  There are three sets of accesses -
    those given to the owner, those given to members of the group,
    and those given to anyone else.  The accesses granted are the
    sum of the accesses available to any of the categories that are
    applicable (thus you never lose accesses by being in the more
    restrictive category.)  The accesses are:
    
	LACC_READ	-- required to examine the contents of the list
	LACC_WRITE	-- required to modify the list
	LACC_SEND	-- required to send to the list
	  
    All the necessary control information about a public mailing list is
    stored in the "<name>-ctl" preference in the format:
    
	<mod time>,<owner uid>,<group id>,<acc>,<removed?>
	
    The accesses are stored in octal ascii (1 digit for each category).
    The modification date/time is in mactime units.
    The "removed?" field is either 0 or 1; entries for removed lists
    linger for a while so the removal is propagated properly.
    
    Returns 0 if list doesn't exist.	
*/
int pubml_acc(mbox *mb, char *name) {

    mbox	*pubml_mb;		/* mailbox with public lists */    
    int		result;
 
    /* get box that has public list info */
    pubml_mb = mbox_find(pubml_uid, pubml_fs, FALSE);
    	   
    sem_seize(&pubml_mb->mbsem);
    result = pubml_acc_int(mb, pubml_mb, name);	/* call internal version */
    sem_release(&pubml_mb->mbsem);

    mbox_done(&pubml_mb);
    
    return result;
}

/* internal version -- public box locked */

int pubml_acc_int(mbox *mb, mbox *pubml_mb, char *name) {

    int		acc = 0;		/* returned: list accesses */
    u_long	modtime;		/* modification time */
    long	owner;			/* list owner */
    long	group;			/* and group */
    char	lacc[4];		/* list accesses */
    boolean_t	removed;		/* list removed? */
    int		i;
     
       
    /* get control info; skip if list has been removed */
    if (pubml_getctl(pubml_mb, name, &modtime, &owner, &group, lacc, &removed)
	&& !removed) {	    
    
	acc = lacc[2] - '0';		/* start with world accesses */
	if (mb && mb->user) {		/* now, do we know who they are? */    		
	    if (mb->user->prived)		/* priv'd user gets everything */
		acc = LACC_READ | LACC_WRITE | LACC_SEND;
	    else {
		if (mb->uid == owner)	/* is this the list owner? */
		    acc |= lacc[0] - '0';	/* yes, add owner accesses */
		if (mb->uid == group)	/* everyone is member of their own group */
		    acc |= lacc[1] - '0';	/* give group accs if uid == list gid */
		else {			/* check user's gid list */
		    for (i = 0; i < mb->user->groupcnt; ++i) {
			if (mb->user->gid[i] == group)
			    acc |= lacc[1] - '0';
		    }
		}
	    }
	}
    }

    return acc;
    
}

/* pubml_get --

    Retrieve contents of public mailing list.  No access checks are performed.


*/
boolean_t pubml_get(char *name, ml_data **result) {

    mbox	*mb;		/* psuedo-box that holds public lists */
    boolean_t	found;		/* list found? */
    
    mb = mbox_find(pubml_uid, pubml_fs, FALSE); /* this is where we keep 'em */

    found = ml_get(mb, name, result, NULL); /* try to get it */
    
    mbox_done(&mb);		/* done with the box */
    
    return found;
}

/* pubml_set --

    Set contents of public mailing list.  No access checks are performed,
    but the control pref must exist (since the modification time is to
    be updated).

*/
void pubml_set(char *name, ml_data *contents) {

    mbox	*mb;			/* psuedo-box that holds public lists */
    u_long	modtime;		/* modification time */
    long	owner;			/* list owner */
    long	group;			/* and group */
    char	lacc[4];		/* list accesses */
    boolean_t	removed;		/* list removed? */

    	
    mb = mbox_find(pubml_uid, pubml_fs, FALSE); /* this is where we keep 'em */

    sem_seize(&mb->mbsem);		/* lock for pubml_getctl */

    /* get control info for this list */
    if (pubml_getctl(mb, name, &modtime, &owner, &group, lacc, &removed)) {
	modtime = mactime();		/* update mod time */
	pubml_putctl(mb, name, modtime, owner, group, lacc, removed);
	sem_release(&mb->mbsem);
	ml_set(mb, name, contents); /* and rewrite the list */
	/* send update messages to other servers */
	pubml_sendupdate(name, modtime, owner, group, lacc, removed, contents);
    } else
	sem_release(&mb->mbsem);
    
    mbox_done(&mb);		/* done with the box */
    
}

/* pubml_rem --

    Remove public mailing list.  No access checks are performed.

*/
boolean_t pubml_rem(char *name) {

    mbox	*mb;		/* psuedo-box that holds public lists */
    boolean_t	found = FALSE;	/* list found? */
    u_long	modtime;		/* modification time */
    long	owner;			/* list owner */
    long	group;			/* and group */
    char	lacc[4];		/* list accesses */
    boolean_t	removed;		/* list removed? */
    
    mb = mbox_find(pubml_uid, pubml_fs, FALSE); /* this is where we keep 'em */

    sem_seize(&mb->mbsem);		/* lock for pubml_getctl */
 
    /* get control info for this list */
    if (pubml_getctl(mb, name, &modtime, &owner, &group, lacc, &removed) && !removed) {
	modtime = mactime();		/* update mod time */
	removed = TRUE;			/* mark list as deleted */
	pubml_putctl(mb, name, modtime, owner, group, lacc, removed);
	sem_release(&mb->mbsem);		
	found = ml_rem(mb, name); 	/* remove it */
	/* send delete message to other servers */
	pubml_sendupdate(name, modtime, owner, group, lacc, removed, NULL);	
    } else
	sem_release(&mb->mbsem);		

    
    mbox_done(&mb);		/* done with the box */
    
    return found;
}

/* pubml_owner --

    Return public mailing list owner's uid (as a string). 
*/

boolean_t pubml_owner(char *name, char *ownerstr) {

    mbox	*pubml_mb;		/* mailbox with public lists */
    boolean_t	found;
    u_long	modtime;		/* modification time */
    long	owner;			/* list owner */
    long	group;			/* and group */
    char	lacc[4];		/* list accesses */
    boolean_t	removed;		/* list removed? */
    
    pubml_mb = mbox_find(pubml_uid, pubml_fs, FALSE);	/* get box that has public list info */
    
    sem_seize(&pubml_mb->mbsem);		/* lock for pubml_getctl */
    
    found = pubml_getctl(pubml_mb, name, &modtime, &owner, &group, lacc, &removed);
    if (removed) found = FALSE;		/* removed list doesn't count */
    if (found)
	numtostr(owner, ownerstr);
 
    sem_release(&pubml_mb->mbsem);
    mbox_done(&pubml_mb);

    return found;    
}


/* pubml_getctl --

    Retrieve & parse public mailing list control pref.

    --> box locked <--

*/
boolean_t pubml_getctl(mbox *mb, char *name, u_long *modtime, long *owner, long *group, 
			char *lacc, boolean_t *removed) {

    char	pref[MAX_STR+10];	/* constructed pref name */
    char	value[PREF_MAXLEN];	/* value */
    
    
    /* retrieve pref, return FALSE if undefined/ill formed */
    ucase(pref, name);			
    strcat(pref, PREF_MLCTL);
    if (!pref_get_int(mb, pref, value))
	return FALSE;
	
    return pubml_parsectl(value, modtime, owner, group, lacc, removed);
}

/* pubml_parsectl --

    Parse public mailing list control pref.

*/
boolean_t pubml_parsectl(char *value, u_long *modtime, long *owner, long *group, 
			char *lacc, boolean_t *removed) {    

    char	*valp;			/* current loc */
    int		i;

    valp = value + 1;			/* skip initial quote */
    
    /* (note that none of the fields can have '"'s in them, so no need to
       worry about quote-doubling) */
       
    valp = strtouns(valp, modtime);	/* parse the pref */
    if (*valp++ != ',')
	return FALSE;
    valp = strtonum(valp, owner);  
    if (*valp++ != ',') 
	return FALSE;
    valp = strtonum(valp, group);  
    if (*valp++ != ',')
	return FALSE;
    for (i = 0; i < 3; ++i)
	lacc[i] = *valp++;
    lacc[3] = 0;
    if (*valp++ != ',') 
	return FALSE;
    *removed = (*valp++ == '1');
    if (*valp++ != '"')			/* should be close-quote now */
	return FALSE;

    return TRUE;
}

/* pubml_putctl --

    Format & write public mailing list control pref.

    --> box locked <--
*/
void pubml_putctl(mbox *mb, char *name, u_long modtime, long owner, long group, 
			char *lacc, boolean_t removed) {

    char	pref[MAX_STR+10];	/* constructed pref name */
    char	value[PREF_MAXLEN];	/* value */

    t_sprintf(value, "\"%lu,%ld,%ld,%s,%d\"", modtime, owner, group,
    					  lacc, (int) removed);
    
    ucase(pref, name);			
    strcat(pref, PREF_MLCTL);
  
    pref_set_int(mb, pref, value);
}

/*^L pubml_remctl --

    Remove public mailingg list control pref.  The underlying list should already
    have been removed.

    --> box locked <--
*/
void pubml_remctl(mbox *mb, char *name) {

    char        pref[MAX_STR+10];       /* constructed pref name */

    ucase(pref, name);
    strcat(pref, PREF_MLCTL);

    pref_rem_int(mb, pref);
}

/* pubml_sendupdate --

    Construct list update message, send it to all the other servers.
    
    The contents of the control message are:
    
       <name> "<modtime>,<owner>,<group>,<lacc>,<removed>"<CR>
       list contents (CR-delimited)
       "."
       
    (lines in the list contents that begin with a "." have one added)
    
*/

void pubml_sendupdate(char *name, u_long modtime, long owner, long group, 
			char *lacc, boolean_t removed, ml_data *ml_head) {
			
    fileinfo	text;		/* and text */
    summinfo	summ;		/* message summary */
    t_file	*f;		/* file to construct message in */
    ml_data	*ml;		/* mailing list block */
    int		i;
    char	c = 0;
    recip	*r;		/* recipient info */
    int 	recipcount = 0;
    char	logbuf[MAX_STR];
    boolean_t	start_of_line = TRUE;
    	
    temp_finfo(&text);		/* set up temp file */
    if ((f = t_fopen(text.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("pubml_sendupdate: cannot open ", text.fname);
	return;
    }
    
    /* first line: name & control info */
    t_fprintf(f, "%s \"%lu,%ld,%ld,%s,%d\"\r", name, modtime, owner, group,
    					 lacc, (int) removed);

    c = '\n';		/* if empty file, send '.' immediately */
    
    /* copy list to file, mapping \n -> \r */
    for (ml = ml_head; ml; ml = ml->next) {
	for (i = 0; i < ml->len; ++i) {
	    c = ml->data[i];
	    if (c == '\n') {
		t_putc(f, '\r');
		start_of_line = TRUE;
	    }
	    else {
		if (start_of_line && c == '.')
		    t_putc(f, c);	/* double '.' at start of line */
		t_putc(f, c);
		start_of_line = FALSE;
	    }
	}
    }    
    if (c != '\n')		/* add terminating \r, if needed */
	t_putc(f, '\r');
    
    t_putc(f, '.'); t_putc(f, '\r'); /* mark the end */
    
    text.offset = 0;
    text.len = t_fseek(f, 0, SEEK_END);
    t_fclose(f);
    
    /* now set up relevant parts of the summary info */
    summ.len = sizeof(summ);
    summ.totallen = text.len; /* length of message text */
    summ.enclosures = 0;
    summ.receipt = FALSE;
    summ.type = MESSTYPE_BLITZ;
    summ.sender = summ.sender_;	/* unpacked form */
    summ.topic = summ.topic_;
    summ.recipname = summ.recipname_;
    strcpy(summ.topic,"List update"); 
    strcpy(summ.recipname, "Blitz servers");
    *summ.sender = 0;
        
    /* construct a recipient */
    r = alloc_recip(&recipcount);
    r->local = TRUE;			/* a blitz message */
    r->id = PUBML_UPDATE_REQ;		/* explode to all servers */
    strcpy(r->name, "Mailing List Updater"); /* cosmetic only */
    
    t_sprintf(logbuf, "Send mailing list update for list %s", name);
    
    /* send the message */
    deliver(NULL, POSTMASTER, r, NULL, NULL, &text, NULL, &summ, FALSE, NULL, FALSE);
    
    free_recips(&r);			/* clean up recip */
    finfoclose(&text);			/* and temp file */
}
/* pubml_sendupdate_all --

    Send update messages for all mailing lists to all peer servers. Also
    send removal updates for any removed lists we still know about.
    
    The update is sent all in one message

*/
void pubml_sendupdate_all () {

    mbox	*mb;			/* public mailing list box */
    fileinfo	text;			/* message text */
    summinfo	summ;			/* message summary */
    t_file	*f;			/* file to construct message in */
    ml_data	*ml;			/* mailing list block */
    recip	*r;			/* recipient info */
    int 	recipcount = 0;
    char	logbuf[MAX_STR];
    boolean_t	start_of_line = TRUE;
    int		i;
    char	c = 0;
    prefp 	p;			/* current pref */
    char	*name;			/* current list */
    char	*value;			/* its value */
    ml_data 	*contents = NULL;	/* list contents */
    u_long 	modtime;		/* list modification time */
    long 	owner;			/* list owner */
    long 	group;			/* list group ownership */
    char	lacc[4];		/* list accesses */
    boolean_t	removed;		/* list removed? */
    char	*listnames;		/* all names/values */
    int		listmax;		/* space allocated */
    char	*listp;			/* pointer into it */
    char	*listend;		/* to end of it */
    int		len;

    temp_finfo(&text);		/* set up temp file */
    if ((f = t_fopen(text.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("pubml_sendupdate_all: cannot open ", text.fname);
	return;
    }
    
    mb = mbox_find(pubml_uid, pubml_fs, FALSE); /* this is where we keep 'em */

    /* step 1: locate all lists & removed lists */
    
    listmax = 10*MAX_STR;		/* set up name array (grown if needed) */
    listnames = mallocf(listmax);
    listp = listnames;
    
    sem_seize(&mb->mbsem);		/* lock while we traverse prefs */

    if (!mb->prefs)			/* get prefs, if not yet here */
	pref_read(mb);

    /* traverse prefs hash table to locate all lists */
    for (i = 0; i < PREF_HASHMAX; i ++) {
	for (p = mb->prefs->hashtab[i]; p != NULL; p = p->next) {
	    value = p->name + p->namelen + 1;
	    if (p->namelen > strlen(PREF_MLCTL) /* a control pref? */
	        && strcmp(p->name+p->namelen-strlen(PREF_MLCTL), PREF_MLCTL) == 0) {
		    
		    if (listp - listnames + 2*MAX_STR > listmax) {
			len = listp - listnames;
		    	listmax += 10*MAX_STR;
			listnames = reallocf(listnames, listmax);
			listp = listnames + len;
		    }
		    strcpy(listp, p->name);	/* concatenate alternating names/values */
		    listp[p->namelen-strlen(PREF_MLCTL)] = 0; /* chop suffix */
		    listp += strlen(listp) + 1;
		    strcpy(listp, value);
		    listp += strlen(listp) + 1;
	    }
	}
    }
    listend = listp;			/* amount actually used */

    sem_release(&mb->mbsem);
    
    /* step 2: constuct update message including each list */
    
    for (listp = listnames; listp < listend; ) {
    	
	name = listp;			/* current list's name/ctl value */
	value = listp + strlen(listp) + 1;
	
	listp = value + strlen(value) + 1; /* move pointer */
	
	if (!pubml_parsectl(value, &modtime, &owner, &group, lacc, &removed)) {
	    t_errprint_ss("Bad list control pref: %s %s", name, value);
	    continue;
	}
	
	/* if list was removed long ago, take this opportunity to
	    get rid of the old control info. The list is still included
	    in the update message, to induce our peers to get rid of it
	    as well. */

        if (removed && mactime() > modtime + PUBML_DWELL) {
	    sem_seize(&mb->mbsem);
	    pubml_remctl(mb, name);	/* clean out old control info */
	    sem_release(&mb->mbsem);
	}

	if (!removed) {
	    /* get contents & mixed-case name */
	    if (!ml_get(mb, name, &contents, name)) {
		t_errprint_s("Missing contents of public list %s", name);
		continue;
	    }
	}
	
	/* Add this list to the message: */

	/* first line: name & control info */
	t_fprintf(f, "%s \"%lu,%ld,%ld,%s,%d\"\r", name, modtime, owner, group,
						lacc, (int) removed);
	
	c = '\n';		/* if empty file, send '.' immediately */
	
	/* copy list to file, mapping \n -> \r */
	for (ml = contents; ml; ml = ml->next) {
	    for (i = 0; i < ml->len; ++i) {
		c = ml->data[i];
		if (c == '\n') {
		    t_putc(f, '\r');
		    start_of_line = TRUE;
		} else {
		    if (start_of_line && c == '.')
			t_putc(f, c);	/* double '.' at start of line */
		    t_putc(f, c);
		    start_of_line = FALSE;
		}
	    }
	}    
	if (c != '\n')		/* add terminating \r, if needed */
	    t_putc(f, '\r');	
	t_putc(f, '.'); t_putc(f, '\r'); /* mark the end */
	
	ml_clean(&contents);
    
    }
    
    /* text all filled in; send it! */

    text.offset = 0;
    text.len = t_fseek(f, 0, SEEK_END);
    t_fclose(f);
    
    /* now set up relevant parts of the summary info */
    summ.len = sizeof(summ);
    summ.totallen = text.len; /* length of message text */
    summ.enclosures = 0;
    summ.receipt = FALSE;
    summ.type = MESSTYPE_BLITZ;
    summ.sender = summ.sender_;	/* unpacked form */
    summ.topic = summ.topic_;
    summ.recipname = summ.recipname_;
    strcpy(summ.topic,"List bulk update"); 
    strcpy(summ.recipname, "Blitz servers");
    *summ.recipname = 0;
    *summ.sender = 0;
        
    /* construct a recipient */
    r = alloc_recip(&recipcount);
    r->local = TRUE;			/* a blitz message */
    r->id = PUBML_UPDATE_REQ;		/* explode to all servers */
    strcpy(r->name, "Mailing List Updater"); /* cosmetic only */
    
    t_sprintf(logbuf, "Send bulk mailing list update.");
    
    /* send the message */
    deliver(NULL, POSTMASTER, r, NULL, NULL, &text, NULL, &summ, FALSE, NULL, FALSE);
    
    free_recips(&r);			/* clean up recip */
    finfoclose(&text);			/* and temp file */
    t_free(listnames);
    mbox_done(&mb);		/* done with the box */

}
/* pubml_update --

    Parse & process mailing list update message received from peer server.

*/

void pubml_update(fileinfo *head, fileinfo *text) {

    mbox	*mb;			/* public lists */
    t_file	*f;
    char	buf[2*MAX_STR];
    char	*p;
    char	*value;
    int		c;
    char	name[MAX_STR];
    ml_data	*ml = NULL;
    u_long	modtime;		/* modification time */
    long	owner;			/* list owner */
    long	group;			/* and group */
    char	lacc[4];		/* list accesses */
    boolean_t	removed;		/* list removed? */
    u_long	oldmodtime;		/* mod time of current list */
    boolean_t	start_of_line;
    boolean_t   truncate;
    int		maxlen;			/* allocation length */
    
    if ((f = t_fopen(text->fname, O_RDONLY, 0)) == NULL) {
    	t_perror1("pubml_update: cannot open ", text->fname);
	return;
    }

    strcpy(name, "???");
    
    mb = mbox_find(pubml_uid, pubml_fs, FALSE); /* this is where we keep 'em */
    
    t_fseek(f, text->offset, SEEK_SET); /* seek to start of text */
    
    /* for each list included in message */
    while (t_gets(buf, sizeof(buf), f)) { 	/* read first line (list name & control pref) */
	    
	if ((p = index(buf,' ')) == NULL)	/* find end of name */
	    goto badfile;
	*p = 0;				/* break string */
	strcpy(name, buf);		/* this much is name */
	value = p+1;			/* rest of line is control info */

	/* check timestamp of any list we already have (*before* pubml_parsectl) */
	sem_seize(&mb->mbsem);
	if (pubml_getctl(mb, name, &modtime, &owner, &group, lacc, &removed))
	    oldmodtime = modtime;		/* timestamp of one we have now */
	else
	    oldmodtime = 0;			/* we don't know about this one */
	sem_release(&mb->mbsem); 
    	    
	/* parse control information for new list */
	if (!pubml_parsectl(value, &modtime, &owner, &group, lacc, &removed))
	    goto badfile;			
			
	/* allocate block big enough for entire list */
	maxlen = (text->len > ML_MAX) ? ML_MAX : text->len;
	ml = mallocf(sizeof(ml_data) + maxlen);
	ml->maxlen = maxlen;
	ml->next = NULL;
	truncate = FALSE;			/* assume no need to truncate */
	
	/* get list, mapping \r -> \n */
	ml->len = 0; start_of_line = TRUE;
	while((c = t_getc(f)) != EOF) {
            if (ml->len >= ML_MAX) {		/* sanity check on length */
		truncate = TRUE;		/* note the error */
		continue;			/* but keep reading */
	    }
	    if (c == '\r') {
		ml->data[ml->len++] = '\n';
		start_of_line = TRUE;
	    } else {
		if (start_of_line && c == '.') { /* leading '.' - end of list? */
		    c = t_getc(f);
		    if (c != '.')
			break;		/* yes! */
		}
		ml->data[ml->len++] = c;
		start_of_line = FALSE;
	    }
	}

	if (truncate) {
	    t_errprint_s("Invalid mailing list update (too long) for %s -- truncated", name);
	}

	if (oldmodtime > modtime) {		/* we already have newer list */
	    t_sprintf(buf, "Ignore outdated update for list %s", name);
	} else {
    
	    /* record new contents & control info
	       if list was removed long ago, just forget about it */
	    
	    sem_seize(&mb->mbsem);
	    if (removed && mactime() > modtime + PUBML_DWELL) {
		pubml_remctl(mb, name);     /* clean out old control info */
	    } else {
	        pubml_putctl(mb, name, modtime, owner, group, lacc, removed);
	    }
	    sem_release(&mb->mbsem);
	
	    if (removed) {
		(void) ml_rem(mb, name); /* remove it */
		t_sprintf(buf, "Process removal for public list %s", name);
	    } else {
		ml_set(mb, name, ml); 	/* else rewrite the list */
		t_sprintf(buf, "Process update for public list %s", name);
	    }
    
	}
	log_it(buf);
	
	ml_clean(&ml);
    }
    
cleanup:
    
    t_fclose(f);
    ml_clean(&ml);

    mbox_done(&mb);
    
    return;
    
badfile:
    t_sprintf(buf, "Ignore bad mailing list update message for %s", name);
    goto cleanup;
}
