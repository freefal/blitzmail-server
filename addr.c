/*  BlitzMail Server -- address routines

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/addr.c,v 3.6 98/10/21 15:55:43 davidg Exp Locker: davidg $
    
    Addresses may include any of:  mailing lists (public or personal), dnd names, or
    Internet addresses.  This module contains code to parse addresses (handling
    RFC822 syntax), resolve mailing lists and dnd names, and apply various 
    transformations to addresses (e.g., dnd name => RFC822 format address).
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <sys/dir.h>
#include <netinet/in.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "mess.h"
#include "deliver.h"

void normalize_localaddr(char *name);
recip *rresolve(char *inname, int depth, mbox *mb, mbox *mlmb, int *recipcount);
static boolean_t isme(char *name, mbox *mb, recip **r, int depth, int *recipcount);
boolean_t is_all_users(char *name, mbox *mb, recip **r);
static boolean_t dnd_resolve(char *name, recip **r, long *dnd_uid, char *dnd_addr, long *dnd_serv, char *dnd_fs);
static boolean_t dnd_host_check(char *name, recip **r, long *dnd_uid, char *dnd_addr, long *dnd_serv, char *dnd_fs);
void normalize_localaddr(char *name);
boolean_t splitname(char *nameaddr, char *name, char *addr);
void expandlist(char *mlname, ml_data *mlist, recip **rlist, int depth, mbox *mb, mbox *mlmb, int *recipcount);
boolean_t globallist(char *name, recip **rlist, int depth, mbox *mb, int *recipcount);
boolean_t owner_globlist(char *name, recip **rlist, int depth, mbox *mb, int *recipcount);
boolean_t addr_clean(char *out, char *in);

/* alloc_recip --

    Allocate and initialize a one-element recipient list.  Increment the 
    recip counter.
*/

recip *alloc_recip(int *recippcount) {

    recip	*recipp;		/* returned: blank recipient */
    
    recipp = (recip *) mallocf(sizeof(recip));
    
    recipp->next = recipp;		/* head & tail of list */
    recipp->name[0] = 0;
    recipp->addr[0] = 0;
    recipp->id = 0;
    recipp->timestamp = 0;
    recipp->blitzserv = -1;
    recipp->blitzfs[0] = 0;
    recipp->vacation = recipp->local = FALSE;
    recipp->nosend = recipp->noshow = recipp->noerr = recipp->oneshot = FALSE;
    recipp->stat = RECIP_OK;
    
    ++(*recippcount);			/* keep count */
    
    return recipp;
    
}
/* copy_recip --

    Copy recipient node, link it to list.
*/

void copy_recip(recip *r, recip **l) {

    recip	*new;			/* copy of recip */

    new = mallocf(sizeof(recip));
    *new = *r;				/* copy recip from master list */
    if (!*l)				/* first? */
	new->next = new; 		/* link to self */
    else {				/* else add to end */
	new->next = (*l)->next;
	(*l)->next = new;
    }
    (*l) = new;				/* new one is last */

}
/* free_recips --

    Free a recipient list.
*/

void free_recips(recip **rlist) {

    recip 	*p, *pp;
    
    if (*rlist) {
	p = (*rlist)->next;		/* find head */
	(*rlist)->next = NULL;		/* break circular list */
	while(p) {			/* traverse list freeing */
	    pp = p; p = p->next;
	    t_free(pp);
	}
    }
    
    *rlist = NULL;			/* sppml */
}
/* getaddrc --

    Return next char of RFC822-format address, handling quoting, comments,
    and escapes.
*/
char getaddrc(char **p, int *comment, boolean_t *quot, 
	      boolean_t *route, boolean_t *esc) {

    char 	c;
    
    c = *(*p)++;		/* get char & advance */
    if (!c) {			/* the end? */
	--(*p);			/* leave pointer at end */
	return c;
    }
    
    if (*esc)			/* if last char was escape */
	*esc = FALSE;		/* don't treat this one specially */
    else {
	if (*quot) {		/* within quoted string? */
	    if (c == '\\')	/* note: no other specials within string */
		*esc = TRUE;
	    else if (c == '"')
		*quot = FALSE;
	} else {
	    if (c == '(')
		(*comment)++;	/* begin comment */
	    else if (c == ')')
		(*comment)--;	/* end comment */
	    else if (*comment == 0) { /* ignore specials inside comment */
		if (c == '"')
		    *quot = TRUE; /* being quoted-string */
		else if (c == '<')
		    *route = TRUE; /* begin route-addr (these don't nest) */
		else if (c == '>')
		    *route = FALSE; /* end route-addr */
	    } else {		/* within comment, check for escape */
		if (c == '\\')
		    *esc = TRUE;
	    }
	}
    }
    
    return c;
}
    

/* resolve --

    Resolve a recipient name, which may be any of:
    
	- a mailing list name (public or personal)
	- an internet address
	- a name to be looked up in the dnd
	
    Mailing lists expand into a list of recipient names, which are in turn
    resolved recursively.  We return a list of "recip" structs, one for
    each recipient generated.  The "recip" struct contains all the information
    necessary to return status(es) to the client, generate the message header,
    and deliver the message.  Normally, each recip is used for all of these
    purposes, but there are flag bits in the recip to indicate certain special
    cases:  
    
	noshow: the recipient should not appear in the
    		message header (e.g., public mailing list members)
	nosend: don't actually send to this recip (e.g., public
		mailing list _name_)
	noerr:	don't return bad status to client (so one bad entry in a public
		mailing list doesn't cause entire list to be rejected)
    
    If we're not resolving the address in the context of a particular user's
    mailing lists, "mb" will be NULL.
*/

recip *resolve(char *name, mbox *mb, int *recipcount) {
					    
    *recipcount = 0;			/* no recips yet */
    
    return rresolve(name, 1, mb, mb, recipcount);	/* start recursive resolution */
}

/* rresolve --

    Handle mailing list expansions.  Check local lists first, then public ones.
    If not a mailing list, call sresolve to parse it as either a DND name or
    email address.  
    
    "mb" identifies the sender; "mlmb" identifies the personal mailing lists to
    use (which will normally be the sender, except in cases like resolving 
    forwarding addresses.)
    
    Returns NULL iff recipient limit exceeded.
*/
recip *rresolve(char *inname, int depth, mbox *mb, mbox *mlmb, int *recipcount) {

    char 	name[MAX_ADDR_LEN];	/* name w/o spaces */
    recip 	*rlist;			/* returned: recipient list */
    ml_data	*mlist;			/* mailing list contents */
    boolean_t	locallist;		/* personal mailing list? */
    
    rlist = NULL;			/* nothing yet */
    
    if (!addr_clean(name, inname)) {	/* remove spaces before list check */
    	rlist = alloc_recip(recipcount); /* address too long - error */
	strncpy(rlist->name, inname, MAX_ADDR_LEN);
	rlist->name[MAX_ADDR_LEN-1] = 0;
	rlist->stat = RECIP_BAD_ADDRESS;/* return single recip w/ bad status */	
    } else if (depth > ADDR_MAX_DEPTH)	{	/* check for recursing too much */
    	rlist = alloc_recip(recipcount);
	strcpy(rlist->name, name);
	rlist->stat = RECIP_LOOP;	/* return single recip w/ bad status */
    } else {
	locallist = FALSE;		/* assume no local list */
	if (mlmb)			/* iff personal lists to check */
	    locallist = ml_get(mlmb, name, &mlist, NULL);
	    
	/* expand local or public list; else resolve single name */
	if (locallist)	{		
	    expandlist(name, mlist, &rlist, depth, mb, mlmb, recipcount);
	    if (rlist == NULL) {	/* empty local list is illegal */
		rlist = alloc_recip(recipcount);
		strcpy(rlist->name, name);
		rlist->stat = RECIP_BAD_ADDRESS; /* return single recip w/ bad status */
	    }
	}
	else if (!globallist(name, &rlist, depth, mb, recipcount))
	    sresolve(name, &rlist, depth, mb, mlmb, recipcount);	
    }
    
    return rlist;
}
/* expandlist --
    
    Run through mailing list resolving each member.  The list is freed.
*/

void expandlist(char *mlname, ml_data *mlist, recip **rlist, int depth, mbox *mb, mbox *mlmb, int *recipcount) {

    recip 	*newrecip;		/* one chunk of recips */
    recip	*tempr;
    ml_data	*prev;
    char	*p,*q;

    *rlist = NULL;			/* no recips yet */
    
    while(mlist) {			/* for each block of list */  
      	
	p = mlist->data;		/* deal with newline terminators... */
	while (p < mlist->data + mlist->len) {	/* for each name in block */
	    q = index(p, '\n');
	    if (!q)
		q = mlist->data + mlist->len;
	    else 
	    	*q++ = 0;		/* terminate string */
	    
	    if (!isblankstr(p)) {	/* skip blank lines */
		/* recurse to resolve list member */
		newrecip = rresolve(p, depth+1, mb, mlmb, recipcount);
		
		/* if limit exceeed, newrecip will be NULL! */
		
		if (*recipcount > ADDR_MAX_RECIPS) { /* if already over the limit */
		    free_recips(&newrecip);	/* free memory asap */
		    free_recips(rlist);		
		    /* recipcount is set; our caller will know there was trouble */
		} else if (newrecip->stat == RECIP_LOOP) {
		    ml_clean(&mlist);	/* free the rest of the list */
		    free_recips(rlist);
		    /* return list name, not member, in loop status */
		    strcpy(newrecip->name, mlname);
		    *rlist = newrecip;	/* return the bad status */
		    return;			/* if loop detected, stop resolving right away */
		} else {			/* add new results to list */
		    
		    if (*rlist == NULL)
			*rlist = newrecip;	/* no old list */
		    else {			/* append new to old */
			tempr = (*rlist)->next;
			(*rlist)->next = newrecip->next; /* end of old -> beginning of new */
			newrecip->next = tempr; /* end of new -> beginning of old */
			*rlist = newrecip;	/* new tail */
		    }
		}
	    }
	    
	    p = q;			/* on to next */
	}
	
	prev = mlist; mlist = mlist->next; /* free & advance */
	t_free((char *) prev);
    }

}

/* globallist --

    Handle global mailing lists.  
    
    See if the name identifies a global mailing list.  If so, verify that this
    user is allowed access to the list.
    
    A recip struct is created for the list itself with the "nosend" flag:  its
    only real purpose is to appear in the message header.  Each member of the
    list generates one recip with the "noshow" flag; these indicate where the 		
    message is actually to be delivered, but do not appear in the header.  The
    "noerr" flag is also set:  if the list contains errors deliver the message
    to as many people as possible (then bounce the rest) rather than rejecting
    the entire list.
    
    Set "mlmb" to NULL when expanding the list -- personal lists should not be
    used in expanding public lists. 
    
*/

boolean_t globallist(char *name, recip **rlist, int depth, mbox *mb, int *recipcount) {

    char 	lname[MAX_ADDR_LEN];	/* name w/o host name */
    ml_data	*mlist;			/* list contents */
    int		acc;			/* list accesses */
    recip	*rtemp;
    
    *rlist = NULL;			/* no recips yet */
    
    strcpy(lname, name);		/* trashable copy of name */
    
    if (!local(lname)) 			/* strip local hostname */
	return FALSE;			/* going to some other host */

    if (pubml_get(lname, &mlist)) {	/* see if list exists */
	acc = pubml_acc(mb, lname);	/* yes - check accesses */
	if ((acc & LACC_SEND) == 0) {	/* no send permission */
	    ml_clean(&mlist);		/* discard the list data */
	    rtemp = alloc_recip(recipcount);
	    rtemp->stat = RECIP_NO_SEND;
	    strcpy(rtemp->name, lname);	/* return single recip w/ bad status */
	    *rlist = rtemp;
	    return TRUE;		/* don't resolve further */
	}    
	/* access checks ok; expand the list */
	expandlist(lname, mlist, rlist, depth, mb, NULL, recipcount);
    } else if (!owner_globlist(lname, rlist, depth, mb, recipcount)) 
	    return FALSE;		/* not a global list */
	
    if (*recipcount > ADDR_MAX_RECIPS)	/* give up if too many recips already */
	return FALSE;
	
    /* mark contents of list as "noshow" and "noerr" */
    
    for (rtemp = *rlist; *rlist != NULL; rtemp = rtemp->next) {
	rtemp->noshow = rtemp->noerr = TRUE;
	if (rtemp->next == *rlist)
	    break;			/* done when head is next again */
    }
    
    /* set up recip for list itself.  must go at end */
    
    rtemp = alloc_recip(recipcount);
    addhost(lname, rtemp->addr, m_hostname);	/* always give local hostname (see exportaddr) */
    rtemp->stat = RECIP_OK;
    rtemp->nosend = TRUE;		/* for header only; don't try to send */

    if (*rlist == NULL) {		/* watch out for empty list */
	rtemp->next = rtemp;		/* just the one entry */
    } else {
	rtemp->next = (*rlist)->next;
	(*rlist)->next = rtemp;
    }
    *rlist = rtemp;			/* add to end */
    
    return TRUE;			/* global list found */
}

/* owner_globlist --

    Expand owner-<listname> or <listname>-request to the list's owner.
*/

boolean_t owner_globlist(char *name, recip **rlist, int depth, mbox *mb, int *recipcount) {

    char	lname[MAX_ADDR_LEN];	/* unadorned list name */
    char	owner[MAX_ADDR_LEN]; 	/* uid of list owner (ascii) */
    
    if (strncasecmp(name, "OWNER-", strlen("OWNER-")) == 0)
	strcpy(lname, name + strlen("OWNER-"));
    else if (strlen(name) > strlen("-REQUEST") &&
    		strcasecmp(name + strlen(name) - strlen("-REQUEST"), "-REQUEST") == 0) {
	strcpy(lname, name);
	lname[strlen(name) - strlen("-REQUEST")] = 0;
    } else
    	return FALSE;
	
    if (!pubml_owner(lname, owner+1)) 	/* find the owner */
	return FALSE;			/* no such list */
	
    /* use #<uid> hook, resolve the list owner */
    owner[0] = '#';
    sresolve(owner, rlist, depth, mb, NULL, recipcount);
        
    return TRUE;
}

/* sresolve --

    Resolve a name to a single address (local or internet); mailing list
    expansions have all been taken care of by this point.  If the name 
    resolves correctly, the recip struct is filled in as follows:
    
	name:  	      DND name used, or contents of comment field
	addr:	      internet-format address
	id:	      DND uid (if 'local', deliver message to this uid)
	local:	      recipient address is local to this server
	
    If a DND name was used, the "id", "blitzserv" and "blitzfs" fields will be
    set from that DND entry (even if the message isn't to be delivered there);
    this information will later be used for enclosure cloning.	
*/

void sresolve(char *inname, recip **r, int depth, mbox *mb, mbox *mlmb, int *recipcount) {

    char 	name[MAX_ADDR_LEN];		/* trashable copy of input name */
    long	dnd_uid;			/* uid the name resolves to */
    char	dnd_addr[MAX_ADDR_LEN];		/* preferred mail address from dnd */	
    long	dnd_serv;			/* server number from dnd */
    char	dnd_fs[MBOX_NAMELEN];		/* server filesystem from dnd */
    boolean_t	is_dndhost;			/* address refers to dnd host? */
    recip	*rlist;				/* returned by globallist */
    boolean_t	badaddr = FALSE;		/* syntax error in address? */
   
    if (*recipcount > ADDR_MAX_RECIPS) 		/* already over the limit? */
	return;					/* don't consume any more memory */

    *r = alloc_recip(recipcount);		/* set up single recipient */
        
    
    /* remove comment (save in recip.name); get uncommented address in "name" */
    if (!splitname(inname, (*r)->name, name)) {	
    	(*r)->stat = RECIP_BAD_ADDRESS;		/* unmatched parens or multiple addrs */
	strcpy((*r)->name, name);
	return;
    }
    
     if (strlen(name) == 0) {			/* null name? */
	(*r)->stat = RECIP_BAD_ADDRESS;
	return;
    }

    /* note that "mb", not "mlmb" is checked - when (e.g.) resolving a
       forwarding address "me" is not valid */
    if (isme(name, mb, r, depth, recipcount))	/* reference to sender's own box */
	return;	
	
    if (is_all_users(name, mb, r))		/* to all local users? */
	return;
	
    /* remove name of dnd host, if appropriate */
    is_dndhost = dnd_host_check(name, r, &dnd_uid, dnd_addr, &dnd_serv, dnd_fs);
    
    /* bad DND reference (and we're the authority) - go no further */
    if (is_dndhost && (*r)->stat != RECIP_OK)
	return;
	
    /* if not already an internet address, look up name in dnd */
    
    if (!isinternet(name, &badaddr) && !badaddr) {
	if (!is_dndhost) {			/* don't repeat lookup  */
	    if (!dnd_resolve(name, r, &dnd_uid, dnd_addr, &dnd_serv, dnd_fs))
		return;				/* didn't resolve uniquely */
	}
	
	(*r)->id = dnd_uid;			/* this is recipient uid */
	(*r)->blitzserv = dnd_serv;		/* their server */
	strcpy((*r)->blitzfs, dnd_fs);		/* ...and this fs on that server */
	(*r)->timestamp = mactime();		/* record time resolved */
	strcpy((*r)->name, name);		/* remember normalized DND name */

	if (macmatch(name, dnd_addr)) {		/* if address == my_name@mac */
	    (*r)->addr[0] = 0;			/* don't need both name & addr */
	    (*r)->local = TRUE;			/* deliver to local mailbox */
	    (*r)->stat = RECIP_OK;
	    if ((*r)->blitzserv == m_thisserv)	/* if we're the destination */
		check_forward(r, depth, recipcount); /* check forwarding now */
	    else if ((*r)->blitzserv == -1) {	/* server didn't match? */
		t_errprint_s("Bad BLITZSERV in dnd entry for %s", name);
		(*r)->stat = RECIP_BAD_ADDRESS; /* we can't send to this user */
	    }	
	    return;
	} else {				/* must continue using dnd address */
	    strcpy(name, dnd_addr);
	}	
    }		
    
    if (badaddr) {				/* isinternet detected a syntax error */
	(*r)->stat = RECIP_BAD_ADDRESS;		/* (unmatched quote, etc.) */
	return;
    }

    if (local(name)) {				/* address is <something>@mac */
	/* handle DND address that points to global mailing list */
	if (globallist(name, &rlist, depth, mb, recipcount)) {
	    t_free((char *) *r);		/* don't need fragmentary recip */
	    *r = rlist;				/* since we have this whole big list */
	    return;
	}
	
	/* local user name; look them up to get the uid */
	if (!dnd_resolve(name, r, &dnd_uid, dnd_addr, &dnd_serv, dnd_fs)) 
		return;				/* didn't resolve */
		
	(*r)->id = dnd_uid;			/* local uid */
	(*r)->blitzserv = dnd_serv;		/* their server */
	strcpy((*r)->blitzfs, dnd_fs);		/* ...and this fs on that server */
	(*r)->timestamp = mactime();		/* record time resolved */
	strcpy((*r)->name, name);		/* remember normalized DND name */
	addhost(name, (*r)->addr, m_hostname);	/* internet-format address */
	
	(*r)->local = TRUE;			/* deliver to local box */
	(*r)->stat = RECIP_OK;		
	if ((*r)->blitzserv == m_thisserv)	/* if we're the destination */
	    check_forward(r, depth, recipcount); /* check forwarding now */
	else if ((*r)->blitzserv == -1) {	/* server didn't match? */
	    t_errprint_s("Bad BLITZSERV in dnd entry for %s", name);
	    (*r)->stat = RECIP_BAD_ADDRESS; 	/* since we can't send to this user */
	}	
    } else {					/* non-local address */
	strcpy((*r)->addr, name);		/* "name" is just an internet address */
	(*r)->stat = RECIP_OK;			
    }
}

/* trim_comment --

    Remove comment(s) from address, save them up.
    
    Returns FALSE if unmatched parentheses encountered.
    
*/

boolean_t trim_comment(char *addr, char *comment) {

    char	new[MAX_ADDR_LEN];	/* address w/o comment */
    boolean_t	route;			/* address parsing state */
    boolean_t	esc;
    boolean_t	quot;
    int		level, plevel;
    char	*p, *newp;
    char 	c;
    
    *new = *comment = 0;		/* assume no comment */
    route = esc = quot = FALSE;		/* initial state */
    plevel = level = 0;	
    p = addr; newp = new;
    
    while(c = getaddrc(&p, &level, &quot, &route, &esc)) {
	if (level > 0) {		/* in comment */
	    if (plevel > 0)		/* (but drop first '(') */
		*comment++ = c;
	    else if (*comment)		/* "(xxx) ... (yyy)" -> "xxx yyy" */
		*comment++ = ' ';
	} else if (level == 0 && plevel == 0) /* not in comment (drop closing ')') */
	    *newp++ = c;
	plevel = level;			/* remember previous nesting level */
    }
    
    *newp = *comment = 0;		/* terminate both strings */
    
    strtrimcpy(addr, new);		/* remove outside spaces from addr */
    
    return (level == 0);		/* parens should match */
}

/* isme --

    Check for canonical name "me".  Don't check for this if not in user 
    context (e.g., on an incoming SMTP message).
*/

static boolean_t isme(char *name, mbox *mb, recip **r, int depth, int *recipcount) {

    if (mb && strcasecmp(name, "me") == 0) {
	strcpy((*r)->name, mb->user->name);	/* get user's full DND name */
	(*r)->addr[0] = 0;			/* can derive addr from name */
	(*r)->id = mb->uid;			/* deliver to local box */
	(*r)->timestamp = mactime();		/* record time resolved */
	(*r)->local = TRUE;			/* a blitz address */
	(*r)->blitzserv = m_thisserv;		/* on this server */
	strcpy((*r)->blitzfs, m_filesys[mb->fs]); /* and this filesystem */
	(*r)->stat = RECIP_OK;
	check_forward(r, depth, recipcount); 	/* check forwarding */
	return TRUE;				/* resolved "me"; done */
    }
    
    return FALSE;				/* keep trying */
}
/* is_all_users 

    Check for special alias for all Blitz users; generate special
    broadcast request address.  Must be privileged to use this.
*/

boolean_t is_all_users(char *name, mbox *mb, recip **r) {

    if (!mb || !mb->user->prived)
	return FALSE;			/* must be privileged */
	
    if (strcasecmp(name, ALL_USERS_ADDR) != 0) 
	return FALSE;			/* no match */
	
    strcpy((*r)->name, ALL_USERS_ADDR);	
    (*r)->addr[0] = 0;			
    (*r)->id = ALL_USERS;		/* broadcast to all users @ all servers */
    (*r)->local = TRUE;			/* a blitz address */
    (*r)->stat = RECIP_OK;
    return TRUE;				
    
}

/* dnd_resolve --

    Look up name in dnd, get resolved name & uid.  Returns false unless
    name matches uniquely.  recip.stat is set appropriately.
*/

static boolean_t dnd_resolve(char *name, recip **r, long *dnd_uid, char *dnd_addr,
			     long *dnd_serv, char *dnd_fs) {

    struct dndresult	*dndres = NULL;	/* results of dndlookup */
    int			stat;		/* dnd status */
    static char *farray[] = {"NAME", "MAILADDR", "UID", "BLITZSERV", "BLITZINFO", NULL};
    char		*p;
    
    if (name[0] == '#') {		/* disallow #<uid> syntax here */
        strcpy((*r)->name, name);       /* set name in error text */
	(*r)->stat = RECIP_BAD_ADDRESS;
	return FALSE;
    }

    stat = t_dndlookup1(name, farray, &dndres);
            
    if (stat == DND_OK) { 		/* resolved uniquely? */
	strcpy(name, t_dndvalue(dndres, "NAME", farray)); /* get resolved form of name */
	strcpy(dnd_addr, t_dndvalue(dndres, "MAILADDR", farray));  /* get mail address */
	strtonum(t_dndvalue(dndres, "UID", farray), dnd_uid); /* and uid */
	p = t_dndvalue(dndres, "BLITZSERV", farray);
	*dnd_serv = blitzserv_match(p);	/* try to locate server in our table */
	p = t_dndvalue(dndres, "BLITZINFO", farray);
	strcpy(dnd_fs, p);		/* partition info */
	(*r)->stat = RECIP_OK;
    } else {
	strcpy((*r)->name, name);	/* set name in error text */
	if (stat == DND_AMBIG) 		/* ambiguous name */
	    (*r)->stat = RECIP_AMBIGUOUS;
	else if (stat == DND_NOUSER || stat == DND_VAGUE) /* no match */
	    (*r)->stat = RECIP_BAD_ADDRESS;
	else 				/* ==> error talking to dnd */
	    (*r)->stat = RECIP_NO_DND;
    }
    
    if (dndres)
	t_free(dndres);
	    
    return (stat == DND_OK);		/* unique match is only way to make us happy */
}

/* dnd_host_check --

    Addresses of the form <user>@<dndhost> are DND references -- they
    should result in the message being directed to the user's preferred
    mail address.  
    
    If <dndhost> is an actual machine that's serving as the mail hub it 
    would be possible to simply let the message go out to the mail hub, 
    which would do the DND resolution.  Its better to short-circuit the 
    process (and do the DND lookup here) and keep messages that will 
    eventually be delivered locally from going out to the mail hub, since
    information (like enclosures) is lost when a message goes out into the
    Internet.
    
    Of course, if there is no mail hub, the resolution obviously has to be
    done here.  (This is the case when <dndhost> is not a actual host but is
    simply a name whose MX record points to us.)
    
    If there's a mail hub, if the DND lookup of such an address fails, 
    we DON'T reject it; the message is passed on to the mail hub for 
    disposition there.  (The address may be legal, even though it isn't 
    in the DND.)  On the other hand, in the absence of a mail hub we're the
    primary provider of DND address resolution, so anything we don't recognize
    is rejected (there's no one else to ask).        
*/

static boolean_t dnd_host_check(char *name, recip **r, long *dnd_uid, char *dnd_addr,
				long *dnd_serv, char *dnd_fs) {


    char	hostpart[MAX_ADDR_LEN];		/* two halves of the address */
    char 	localpart[MAX_ADDR_LEN];
    
    if (!splitaddr(name, localpart, hostpart))	/* extract host part of address */
	return FALSE;
	
    /* check against name(s) that trigger DND matching */
    if (hostmatch(hostpart, m_dndhost) < 0)
	return FALSE;				/* not DND hostname */
	    	
    if (!dnd_resolve(localpart, r, dnd_uid, dnd_addr, dnd_serv, dnd_fs)) { 
	if (!m_dndresolver) {			/* if there's a mail hub to check */
	    (*r)->name[0] = 0;			/* don't put name in comment also */
	    return FALSE;			/* pass the buck to them */
	}
    }
    
    strcpy(name, localpart);			/* is a DND reference; strip hostname */
    return TRUE;				/* and use the DND address we found 
    						   (or the bad status if resolve failed) */
}

/* hostmatch --

    Check a hostname against a list of aliases.  If the hostname is within our local
    domain, strip that domain name before checking.  (Also strip local domain from
    alias list members, if it's there).  Returns matching location in list; or
    -1 if not found.
*/

int hostmatch(char *hostpart, char **list) {
    
    int		len;		/* length of hostpart */
    char	cur[MAX_ADDR_LEN];
    int		i;
    
    /* first, strip off local domain name, if present */   
    strip_domain(hostpart);
    
    len = strlen(hostpart);	/* loop invariant */
    
    /* search hostname list until match or end */
    for (i = 0; list[i]; ++i) {
    	/* check prefix match on each */
	if (strncasecmp(hostpart, list[i], len) == 0) {
	    if (len == strlen(list[i]))
		return i;	/* complete match */
	    else {		/* try stripping domain */
		strcpy(cur, list[i]);
		strip_domain(cur);
		if (strlen(cur) == len)	 /* matches now? */
		    return i;
	    }
	}
    }
    return -1;
}
/* blitzserv_match --

    The BLITZSERV dnd field is in the format <hostname>[@<appletalk zone>].
    Extract the hostname, and search our table of peer servers for it.
*/

int blitzserv_match(char *dnddata) {

    char	hostname[MAX_ADDR_LEN];
    char	*p;
    
    strcpy(hostname, dnddata);		/* trashable copy */
    if (p = index(hostname, '@'))	/* (sic) */
	*p = 0;				/* chop off the zone */

    return hostmatch(hostname, m_server); /* search list of server names */
}

/* isinternet --

    Determine whether an address appears to be something resembling a 
    standard internet-style address. The "badaddr" flag is set if invalid
    address syntax is detected.

    This routine has the important side-effect of removing <>s from around
    the address iff it is safe to do so (ie, we're not dealing with a route 
    address.)
    
    Note:  any address comment should already have been stripped, although 
    quoted-strings are still legal here.
*/

boolean_t isinternet(char *name, boolean_t *badaddr) {

    char 	c;			/* current char of address */
    int		comment = 0;		/* address parsing state: */
    boolean_t	quot = FALSE;		/* '' */
    boolean_t	route = FALSE; 		/* '' */
    boolean_t	esc = FALSE;		/* '' */
    char	newname[MAX_ADDR_LEN]; 	/* improved version of name */
    char 	*p1, *p2;
    boolean_t   sawspecial = FALSE;	/* special chars present in name */
    boolean_t	sawroute = FALSE;	/* this is route address */
    
    if (badaddr)
	*badaddr = FALSE;

    /* copy name to temp, stripping spaces at either end */
    strtrimcpy(newname, name);
    
    /* copy back, checking for address special chars */
    for (p1 = name, p2 = newname; c = *p1++ = *p2++; ) { /* sic */
    	if (c == '@' || c == '!' || c == '%')
	    sawspecial = TRUE;		/* some kind of internet/uucp style address */
    }
    
    if (!sawspecial)
    	return FALSE;			/* doesn't appear to be an internet address */
	
    /* now parse address to check syntax & identify route-addrs */
    p1 = name; p2 = newname;		/* set up for another pass */
    while ((c = getaddrc(&p1, &comment, &quot, &route, &esc)) != 0) {
    	if (!quot) {			/* if not quoted, check for route */
	    if (route) {		/* if within <>s */
	    	if (c == ':' || c == ',')
		    sawroute = TRUE;	/* remember if non-trivial route addr seen */
	    } else {
	    	if (c == ',' && badaddr)/* except in route-address/quoted-string... */
		    *badaddr = TRUE;	/* ...commas are not allowed */
	    }
	    
	}
        *p2++ = c;			/* copy unchanged */
    }
    *p2 = 0;				/* terminate newname */
    
    if (badaddr && (quot || route))	/* unterminated quote or route-addr? */
	*badaddr = TRUE;

    /* now, remove any number of enclosing <>s */    
    for (p1 = newname, p2 = newname+strlen(newname)-1; *p1 == '<' && *p2 == '>'; ++p1, --p2)
    	*p2 = 0;
	
    /* finally, copy the result back, adding 1 set of <>s iff needed */
    if (sawroute)
	*name++ = '<';
    strcpy(name, p1);
    if (sawroute)
	strcat(name,">");
	
    return TRUE;	
}

/* local --

    See if this is a local address.  
    
    [begin digression]
    Here "local" means that the address refers to the single "virtual host" 
    that a set of blitzservers present themselves  as, it does not necessarily 
    imply that we are the server that currently holds the user's mailbox.
    
    The partitioning of users among the several servers is transparent:  
    the user's address always uses the primary hostname, not the name of 
    the particular server host.  Thus recipients with addresses
    @ "mac.dartmouth.edu" might be partitioned among 3 servers "blitz1",
    "blitz2", and "blitz3".  For processing incoming SMTP mail, MX records
    for "mac" would point to all 3 servers with equal preference.
    
    Although we never generate addresses that mention the individual
    server names, we're prepared to accept them (in case some misguided
    person tries to send to such an address.)  Read_config automatically
    enters the names of all the individual servers as aliases of the
    primary hostname in our m_alias table.
    [end digression]
    
    So, we're trying to see if this is a local address.  If there's no host 
    part, we know it's local.  Otherwise, check the host part against 
    our primary hostname -- remove the local domain name, and check against
    all aliases of our hostname.  Also check for a route-address with us as
    the first element.  Addresses like "<@us:user@us>" are local; addresses
    like "<@us,@host1:user@host2>" are rewritten as "<@host1:user@host2>".
    
    Returns true if the address is local.
  
    The hostname is removed iff the address is local.	
*/

boolean_t local(char *inname) {

    char	name[MAX_ADDR_LEN];	/* working copy of address */
    char	hostpart[MAX_ADDR_LEN]; /* host part */
    int		comment = 0;		/* address parsing state: */
    boolean_t	quot = FALSE;		/* '' */
    boolean_t	route = FALSE; 		/* '' */
    boolean_t	esc = FALSE;		/* '' */
    char	*namep = name;		/* '' */
    char	*p1, *p2;
    char	*comma;	 		/* ptrs to elements of address */
    char	*colon;			/* '' */
    char 	*atpos;			/* '' */
    char	c;			

    strtrimcpy(name, inname);		/* strip leading/trailing spaces */
    
    if (!index(name, '@') && !index(name, '%') && !index(name, '!'))
    	return TRUE;			/* no hostname, must be local */
	
    /* remove any outer brackets (_should_ be present only for route
       addresses, but there's no sense in being pedantic.  We recognize
       route addresses by colons/commas anyway.  There's some duplication
       of effort with isinternet here, but sometimes one is called but not
       the other, so we really seem to need to do this in both places. */
       
    for (p1 = name, p2 = name+strlen(name)-1; *p1 == '<' && *p2 == '>'; ++p1, --p2)
    	*p2 = 0;
		   
    namep = p1; 			/* remember where it starts */
          
    /* note: comments should have already been stripped, and we just
       removed <>s, so quoted-strings are the only nasty left.  Scan the
       string to locate route addr delimiters. */
        
    comma = colon = atpos = NULL;
    while ((c = getaddrc(&p1, &comment, &quot, &route, &esc)) != 0) {
	if (!quot) {
	    if (c == ':' && !colon)
		colon = p1-1;
	    else if (c == '@' && !atpos)
		atpos = p1-1;
	    else if (c == ',' && !comma)
		comma = p1-1;
	}
    }
    
    if (!colon || (atpos != namep)) {	/* not route addr */
    	if (atpos) {
	    strcpy(hostpart, atpos+1);	/* host part of user@host */
	    if (hostmatch(hostpart, m_alias) >= 0) {
		*atpos = 0;
		strcpy(inname, namep);	/* keep just user part */
		return TRUE;
	    }
	}
	return FALSE;	
    } else {				/* rats, route address */
	if (!comma)			/* multiple hosts? */
	    p2 = colon;			/* no, colon delimits host name */
	else
	    p2 = comma;
	if (p2) {			/* watch out for ill-formed addr */
	    strcpy(hostpart, namep + 1);/* drop leading "@" */
	    hostpart[p2 - namep - 1] = 0; /* chop at "," or ":" */
	    if (hostmatch(hostpart, m_alias) >= 0) { /* if route names us first */
		t_sprintf(inname, "<%s>", p2+1); /* remove us from front of route */
		return local(inname); /* recurse to see rest is local */
	    }
	}
	
	/* we're not the first member of route, replace <>s */
	t_sprintf(inname, "<%s>", namep);
	return FALSE;
    }

}
/*^L in_local_domain --

    See if this address is local, or within the local domain, for purposes of spam
    filtering.

*/
boolean_t in_local_domain(char *inname) {

    char        name[MAX_ADDR_LEN];     /* working copy of address */
    char        hostpart[MAX_ADDR_LEN]; /* host part */
    int         comment = 0;            /* address parsing state: */
    boolean_t   quot = FALSE;           /* '' */
    boolean_t   route = FALSE;          /* '' */
    boolean_t   esc = FALSE;            /* '' */
    char        *namep = name;          /* '' */
    char        *p1, *p2;
    char        *comma;                 /* ptrs to elements of address */
    char        *colon;                 /* '' */
    char        *atpos;                 /* '' */
    int		i;
    char        c;

    strtrimcpy(name, inname);           /* strip leading/trailing spaces */

    if (!index(name, '@') && !index(name, '%') && !index(name, '!'))
        return TRUE;                    /* no hostname, must be local */

    /* remove any outer brackets, as in "local" */
    for (p1 = name, p2 = name+strlen(name)-1; *p1 == '<' && *p2 == '>'; ++p1, --p2)
        *p2 = 0;

    namep = p1;                         /* remember where it starts */

    /* note: comments should have already been stripped, and we just
       removed <>s, so quoted-strings are the only nasty left.  Scan the
       string to locate route addr delimiters. */

    comma = colon = atpos = NULL;
    while ((c = getaddrc(&p1, &comment, &quot, &route, &esc)) != 0) {
        if (!quot) {
            if (c == ':' && !colon)
                colon = p1-1;
            else if (c == '@' && !atpos)
                atpos = p1-1;
            else if (c == ',' && !comma)
                comma = p1-1;
        }
    }

    if (!colon || (atpos != namep)) {   /* not route addr */
        if (atpos) {
            strcpy(hostpart, atpos+1);  /* host part of user@host */
            if (hostmatch(hostpart, m_alias) >= 0) {
                return TRUE;		/* really local */
            }
	    
	    /* "hostmatch" strips the local domain; if it changed
	       "hostpart" that implies it's within the local domain */
	    if (strcmp(hostpart, atpos+1) != 0)
		return TRUE;		/* local domain suffix seen */

	    /* check against DND pseudo-host name(s), including any vanity
	       names */
	    if (hostmatch(hostpart, m_dndhost) >= 0) {
		return TRUE;
	    }

    	    /* finally, check for the user@domain case
	       note that the entries in m_domain have a leading '.'  */
	    for (i = 0; i < m_domaincnt; i++) {
		if (strcasecmp(hostpart, m_domain[i]+1) == 0) {
		    return TRUE;	/* host == local domain */
		}

	    }

        } 
    } else {				/* rats - route address */
        if (!comma)                     /* multiple hosts? */
            p2 = colon;                 /* no, colon delimits host name */
        else
            p2 = comma;
        if (p2) {                       /* watch out for ill-formed addr */
    	    char        newname[MAX_ADDR_LEN];

            strcpy(hostpart, namep + 1);/* drop leading "@" */
            hostpart[p2 - namep - 1] = 0; /* chop at "," or ":" */
            if (hostmatch(hostpart, m_alias) >= 0) { /* if route names us first */
                t_sprintf(newname, "<%s>", p2+1); /* remove us from front of route */
                return in_local_domain(newname); /* recurse to see rest is local */
            }
        }
    }

    return FALSE;


}
/* macmatch -- 

    Try to deduce that "Fred J. Flintstone" and "fred.j.flintstone@mac" are the 
    same address.
    This check is limited to textual rewriting (no dnd lookup is done).
*/

boolean_t macmatch(char *name, char *addr) {

    char	ncopy[MAX_ADDR_LEN]; 	/* normalized copy of name */
    char	acopy[MAX_ADDR_LEN]; 	/* and address */

    strcpy(ncopy, name);		/* copy; don't muss up parameters */
    strcpy(acopy, addr);
    
    if (!local(acopy))			/* strip local hostname */
    	return FALSE;			/* non-local address */
	
    normalize_localaddr(ncopy);		/* normalize both copies */
    normalize_localaddr(acopy);
    
    return (strcasecmp(ncopy, acopy) == 0);
}

/* normalize_localaddr --

    Convert various & sundry delimiters in a local address (dnd name) to single spaces.
*/

void normalize_localaddr(char *name) {

    char 	*out;
    int		spacecount = 0;
    char	c;
    
    out = name;
    while(c = *name++) {		/* sic */
    	if (c == ' ' || c == '-' || c == '_' || c =='.')
	    spacecount++;		/* any delimiter is same as a space */
	else {				
	    if (spacecount) {		/* note: leading/trailing spaces are dropped */
		if (out > name)
		    *out++ = ' ';	/* collapse multiple spaces */
		spacecount = 0;
	    }
	    *out++ = c;
	}
    }
    *out++ = 0;
}

/* check_forward --

    Examine the "ForwardTo" preference to see if this user's mail is 
    forwarded elsewhere.  If it is, get the forwarding address & recurse
    to resolve it.  Note that we pass the recipient's mailbox:  the 
    recipient's aliases, not the sender's, should be used when resolving 
    the forwarding address.  If mail is being forwarded, set the 'nosend'
    bit in the recip struct we have so far.
    
    Also take this opportunity to check the "Vacation" preference; if it
    exists set a flag in the recip so an automatic reply will be sent later.
    Note that the reply is sent even if the recipient's mail is forwarded.
 										   
    Note that (unlike public mailing lists) forwarding address contents 
    don't have the "noerr" bit set:  a bad forwarding address needs to 
    be reported synchronously, because the bounce message wouldn't give
    enough information to indicate who has a bad forwarding address.

    If the forwarding address is non-Blitz, record the dnd information
    (uid etc.) in the recip node to allow enclosure cloning.  
    
    Forwarding addresses are resolved at two separate points in the process
    (because the check must be made by the server that owns the mailbox).
    Where possible (for recips on the local server) the check is part of
    the normal address resolution process, a check is also made just before
    delivery on the destination server.  (The first check is an optimization,
    and it also makes recursive forwarding work without having to build the
    recursion into the queue processing code.)
*/

void check_forward(recip **r, int depth, int *recipcount) {

    mbox	*recipmb;		/* recipient box */
    char	s[PREF_MAXLEN];
    char	fwdaddr[MAX_ADDR_LEN];
    char	*fwdp;
    char	name[MAX_ADDR_LEN];
    char	*namep;
    recip	*temp, *temp1, *fwdrecips;
    boolean_t	route;			/* address parsing state */
    boolean_t	esc;
    boolean_t	quot;
    int		level;
    char 	c;
    
    /* get recipient box info */
    recipmb = mbox_find((*r)->id, fs_match((*r)->blitzfs), FALSE);     
    
    if (pref_get(recipmb, PREF_VACATION, s)) 
	(*r)->vacation = TRUE;	/* vacation message active for this recip */
	
    if (pref_get(recipmb, PREF_FWD, s)) { /* forwarding address defined? */
	unquote(fwdaddr, s);	/* remove quotes from pref */
	if (*fwdaddr) {		/* null value means don't forward */	
	    /* ok, there is a forwarding address; resolve it */
	    fwdrecips = NULL;
	    route = esc = quot = FALSE;		/* initial state */
	    level = 0;				
	    for(fwdp = fwdaddr; *fwdp; ) {	/* scan forwarding address(es) */
		strcpy(name, "");		/* set up to parse another */
		namep = name;
		while (c = getaddrc(&fwdp, &level, &quot, &route, &esc)) {
		    if (c == ',' && !quot && !route && level == 0)
			break;			/* unquoted ',' ends recip */
		    *namep++ = c;		/* copy over the char */
		}
		*namep = 0;
		
		/* have one name; resolve it */
		temp = rresolve(name, depth+1, (mbox *) NULL, recipmb, recipcount);
		
		/* if recipcount limit exceeed, temp will be NULL! */
		
		if (*recipcount > ADDR_MAX_RECIPS) { /* if already over the limit */
		    free_recips(&fwdrecips);	/* free memory asap */
		    goto cleanup; /* recipcount is set; our caller will know there was trouble */
		} else if (temp->stat == RECIP_LOOP) { /* oh no! forwarding loop! */
		    /* if we proceed, we'll generate another bad status for each
		       remaining recip.  clean up & back out to avoid generating
		       multiple bounces */
		    free_recips(&fwdrecips);	/* free what we had so far */
		    free_recips(&temp);
		    (*r)->stat = RECIP_LOOP;	/* return the loopy recip */
		    goto cleanup;		    
		}
			
		/* add latest batch to cumulative list */
		if (fwdrecips == NULL)
		    fwdrecips = temp;		/* first batch (easy) */
		else if (temp) {		/* append to old list */
		    temp1 = temp->next;		/* head of new */
		    temp->next = fwdrecips->next; /* new tail -> old head */
		    fwdrecips->next = temp1;	/* old tail -> new head */
		    fwdrecips = temp;		/* point to new tail */
		}
	    }
	    
	    /* mark forwarding address(es) "noshow" */
	    for (temp = fwdrecips;; temp = temp->next) {
		temp->noshow = TRUE;		
		/* remember info for encl cloning */
		if (!temp->local && temp->stat == RECIP_OK) { 
		    temp->id = (*r)->id;
		    temp->blitzserv = (*r)->blitzserv;
		    strcpy(temp->blitzfs, (*r)->blitzfs);
		    strcpy(temp->name, (*r)->name);
		}
		if (temp->next == fwdrecips)
		    break;
	    }
	    
	    /* set up "nosend" recip for header */
	    (*r)->stat = RECIP_OK;
	    (*r)->nosend = TRUE;
	    (*r)->next = fwdrecips->next;	/* link it at end */
	    fwdrecips->next = *r;
	}
    }
    
    cleanup:
    mbox_done(&recipmb);
    
}

/* splitaddr --

    Split address in user@host form into hostname & localpart.  If address is not
    in that form (uucp, route-addr), return false.
*/
boolean_t splitaddr(char *addr, char *localpart, char *hostpart) {

    char 	c;			/* current char of address */
    int		comment = 0;		/* address parsing state: */
    boolean_t	quot = FALSE;		/* '' */
    boolean_t	route = FALSE; 		/* '' */
    boolean_t	esc = FALSE;		/* '' */
    char 	*addrp;			/* current pos */
    char	*atpos = NULL;		/* location of '@' */	
    
    addrp = addr;
    
    /* locate the @ */
    while ((c = getaddrc(&addrp, &comment, &quot, &route, &esc)) != 0) {
	if (route)
	    return FALSE;		/* route addrs are too messy */
	if (c == '@' && !quot)   	/* comments already stripped */
	    atpos = addrp - 1;		/* remember where we saw the @ */
    }
    
    if (!atpos)				/* no @, not in user@host form */
	return FALSE;
	
    strncpy(localpart, addr, atpos - addr); /* everything before the @ */
    localpart[atpos - addr] = 0;    	/* add terminator */
    strcpy(hostpart, atpos + 1);	/* everything after the @ */
    
    return TRUE;
}
/* splitname --

    Given name & address from RFC822-style To: (etc.) line, return name and
    address.  The usual forms seen are:
    
	Both name and address:
	
	    Fred Flintstone <fredf@bedrock.edu>
	    
	In alternate form:
	
	    fredf@bedrock.edu (Fred Flintstone)
	    
	or, just the address:
	
	    barneyr@bedrock.edu
	    
    Caller must supply storage for the name & address.

    Returns FALSE if there are unmatched parens or more than one address.
*/    

boolean_t splitname(char *nameaddr, char *name, char *addr) {

    char 	c;			/* current char of address */
    int		comment = 0;		/* address parsing state: */
    boolean_t	quot = FALSE;		/* '' */
    boolean_t	route = FALSE; 		/* '' */
    boolean_t	esc = FALSE;		/* '' */
    boolean_t	wasroute = FALSE;
    int		wascomment = 0;
    int		i;
    char	commentstr[MAX_ADDR_LEN]; /* address comment */
    char 	*namep, *addrp, *commentp; /* current pos */
    
    namep = name; *namep = 0;
    addrp = addr; *addrp = 0;
    commentp = commentstr; *commentp = 0;
    
    /* generate name, route addr, and comment strings */
    while ((c = getaddrc(&nameaddr, &comment, &quot, &route, &esc)) != 0) {
	if (c == ',' && comment == 0 && !quot && !esc && !route)
	    break;			/* unquoted ',' ends recipient */
	if (comment > 0) {		/* in comment */
	    if (wascomment > 0) 	/* (but drop first '(') */
		*commentp++ = c;
	    else { 
		if (route)		/* insert whitespace in name/addr... */
		    *addrp++ = ' '; 	/* ...where the comment was */
		else
		    *namep++ = ' ';
		if (commentp > commentstr) /* "(xxx) ... (yyy)" -> "xxx yyy" */
		    *commentp++ = ' ';
	    }
	} else if (wascomment == 0) { /* not in comment (drop closing ')') */
	    if (route || wasroute)	/* include close > in route */
		*addrp++ = c;
	    else 
		*namep++ = c;
	}
	wasroute = route; wascomment = comment;
    }
    
    *namep = *addrp = *commentp = 0;

    /* strip brackets unless it's a route */
    if (*addr) {			/* addr in brackets seen? */
	if (addr[1] != '@') {		/* brackets required? */
	    for (i = 1; addr[i] != '>' && addr[i]; ++i)
		addr[i-1] = addr[i];	/* remove <>s from outside */
	    addr[i-1] = 0;
	}
	if (!*name) {			/* illegal, but allow it... */
	    if (*commentstr) {		/* <foo@bar> (fred) */
		strtrimcpy(name, commentstr); 
	    } else {
		strtrimcpy(name, addr);	/* <foo@bar> */
	    }
	} else {			/* name outside <>s overrides (comment) */
	    strcpy(commentstr, name);
	    strtrimcpy(name, commentstr); 
	}
    } else {				/* no <>s seen */
	strtrimcpy(addr, name);		/* unvarnished stuff must be the addr */
	if (*commentstr) {		/* and use comment for name, if any */
	    strtrimcpy(name, commentstr); 
	} 
    }
    
    /* return FALSE if unmatched parens or <>s or more than one recip */
    return (comment == 0) && (route == 0) && c != ',';
}

/* addhost --

    Generate internet-legal address from DND name.  This basically amounts to
    adding @<host> to the end, but a little additional doctoring is also
    needed:  spaces turn into periods, and extraneous periods (leading, trailing,
    or 2 adjacent periods) are removed.  The hostname to be added might be either
    the blitz hostname or the dnd hostname, depending on whether we want an address
    that refers explicitly to the blitzmail box or one that resolves to the user's
    preferred address.
*/

void addhost(char *from, char *to, char *host) {

    enum 	{s_start, s_blank, s_nonblank} state = s_start;
    char	*nonblank = to;
    char	c;
    
    while(c = *from++) {
	if (c == ' ' || c == '.') {
	    if (state == s_nonblank)
		*to++ = '.';	/* eat all but one '.' between words */
	    state = s_blank;
	} else {
	    *to++ = c;
	    state = s_nonblank;
	    nonblank = to;	/* remember one past last non-blank */
	}	
    }
    *nonblank++ = '@';		/* chop off trailing blanks */
    strcpy(nonblank, host); 	/* and add hostname */
    
}

/* getheadline --

    Return next line of message header.  We need to allocate the storage
    for this dynamically, because in messages we constuct "lines" can
    be very long indeed.  For generality, handle lines terminated with
    CR, LF, or CRLF (although CR is what we expect to see).
    
    If "unfold" is TRUE (the usual case), continued header lines are unfolded
    into a single long line for processing. The other option (unfold=FALSE)
    causes us to return the folded header line with line breaks still present
    (good for copying one header to another with minimum disruption).
    
    
*/
char *getheadline(t_file *in, long *remaining, boolean_t unfold) {

    int		buflen = 1024;	/* length in buffer */
    int		moved;		/* how many moved so far */
    char	*buf, *bufp;
    int		c;
    
    if (*remaining <= 0)		/* at end? */
	return NULL;

    bufp = buf = mallocf(buflen);
	
    for (moved = 0 ;;) {
	if (moved >= buflen - 2) { /* note: leave 2 spaces for !unfold case */
	    buflen *= 2;	/* grow buffer exponentially */
	    buf = reallocf(buf, buflen);
	}
	if ((*remaining)-- <= 0)
	    break;		/* end of header */
	c = t_getc(in);
	if (c == '\r' || c == '\n') {	/* CR, LF or CRLF */
	    if (remaining == 0)
		break;		/* eof */
		
	    if (c == '\r') {
		c = t_getc(in);	/* peek at next char */
		if (c == '\n') {
		    if (--(*remaining) == 0)
			break;		/* header ends with CRLF */
		    c = t_getc(in);	/* eat LF after CR */
		}
	    } else
		c = t_getc(in);	/* peek at char after */	
		
	    /* check first char of next line; if whitespace, combine it */
	    if ((c != ' ' && c != '\t')
		|| moved == 0) { 	/* (blank line ends header; no continue) */
		t_ungetc(in, c);	/* oops, put it back */
		break;			/* line ends now */
	    } else {
		if (unfold)		/* combining into long line? */
		    c = ' ';		/* separate with just a space */
		else
		    buf[moved++] = '\r'; /* no- leave continuation intact */
		--(*remaining);
	    }	    
	}
	
	buf[moved++] = c;
    }
    
    buf[moved] = 0;		/* terminate */
    
    return buf;			/* caller must free */

}
/* addr_clean --
    
    Clean up address before sending it into the address resolution process.
    Remove leading & trailing spaces; delete control characters.
    
    If cleaned output is longer than MAX_ADDR_LEN, return FALSE.
*/

boolean_t addr_clean(char *out, char *in) {

    char 	*oldout = out;
    int		spacecount = 0;
    char	c;
    
    while(c = *in++) {			/* sic */
    	if (c == ' ')
	    spacecount++;		/* save up spaces */
	else {	
	    if (out == oldout) 		/* eat leading spaces */
		spacecount = 0;
	    for( ; spacecount; --spacecount)
		*out++ = ' ';
	    if (!(isascii(c) && iscntrl(c)))	/* eat control chars, but not 8-bits */
		*out++ = c;
	}
	if (spacecount + out - oldout >= MAX_ADDR_LEN) { /* too long? */
	    *oldout = 0;		/* wipe it */
	    return FALSE;
	}
    }
    *out++ = 0;
    return TRUE;
}
