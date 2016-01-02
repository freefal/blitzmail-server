/*  BlitzMail Server -- summaries
	
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/summ.c,v 3.6 98/10/21 16:10:46 davidg Exp Locker: davidg $

    Message summaries are stored in text files in the user's mailbox directory,
    one file for each folder.  The format of the files the same as is sent to 
    the client in response to SUMM or MSUM commands.  
        
    While the user is connected, summaries are kept in memory in a bucketed
    linked list.  Changes are made to the in-memory copy, which is periodically
    written back out (the entire file is rewritten).
    
    New messages always appear at the end of the folder.  If a message is
    deleted and then undeleted, the summary will be placed at the end of the
    inbox, instead of in its former position.  Clients may, of course, sort the
    summaries as they please before displaying them.  Note that if the box is
    inactive, a message may be delivered by simply appending to the end of the
    inbox (without reading in all the old summaries).
    
    If fold->summs is NULL, the summaries have not yet been read in.  If the 
    folder is empty (and has been read in), fold->summs points to a summbuck 
    with a count of zero.
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "client.h"
#include "config.h"
#include "mess.h"

typedef struct summent {			/* element of summary list */
    long		messid;			/* message id */
    folder 		*fold;			/* folder it came from */
} summent;

/* private routines */
boolean_t fold_addsum(mbox *mb, folder *fold, summinfo *insumm);
boolean_t fold_delsum(mbox *mb, folder *fold, long messid, summinfo *outsumm);
void fold_fname(char *fname, mbox *mb, folder *fold);
void summ_write(mbox *mb, folder *fold);
int summ_packed_len(summinfo *summ);
void summ_squeeze(folder *fold, summbuck *p, summinfo *summ);
static void fold_list1(mbox *mb, long foldnum, boolean_t last);
void sort_messlist(summent *sum, int count);


/* empty_folder --

    For each message in the folder, delete the message file and remove
    the summary info from the folder.
    
    --> box locked <--
*/

void empty_folder(mbox *mb, folder *fold) {

    summinfo	summ;			/* summary info of current message */
    summinfo	*summp;
    
    sem_check(&mb->mbsem);
    
    if (fold->summs == NULL)	/* get summaries, if not yet present */
	summ_read(mb, fold);
	
    /* delete first message until empty */
    while(fold->count > 0) {
	if (fold->summs == NULL || fold->summs->used == 0) {
	    t_errprint_ll("empty_folder: folder %ld inconsistent uid = %ld", fold->num, mb->uid);
	    break;			/* don't spin if count bogus or bucket empty */
	}
	summp = (summinfo *) fold->summs->data; /* first summary in folder */
	fold_delsum(mb, fold, summp->messid, &summ);
	(void) mess_rem(mb, summ.messid, summ.totallen); /* delete message file */
    }
    fold->foldlen = 0;
    touch_folder(mb, fold);		/* folder is very modified now! */
    
}
/* summ_copymess --

    Copy message from one folder, add to another.  Set new expiration date
    (or leave it alone if newexp == -1).  A new message id is assigned to
    the copy, and a new link to the message file is created.
    
    Returns FALSE if message not in source folder.

*/

boolean_t summ_copymess(mbox *mb, folder *from, folder *to, long *messid, long newexp) {

    summinfo	*oldsumm;		/* summary info of original message */
    summinfo	newsumm;		/* '' of new message */
    char	oldname[MESS_NAMELEN];	/* filename of old message */
    char	newname[MESS_NAMELEN];	/* and new */
    boolean_t	ok = FALSE;		/* returned: message found? */
    
    
    sem_seize(&mb->mbsem);
    
    if (oldsumm = get_summ(mb, *messid, &from)) { /* (sic) */
	summ_copy(&newsumm, oldsumm, TRUE);	/* make a copy of the summary */
	newsumm.messid = next_messid();	/* assign a new message id */
	if (newexp != -1)		/* set new expiration date? */
	    newsumm.expire = newexp;
	mess_name(oldname, mb, oldsumm->messid); /* generate filenames */
	mess_name(newname, mb, newsumm.messid);	
	if (link(oldname, newname) == 0) {	/* create link under new name */
	    (void) fold_addsum(mb, to, &newsumm);
	    *messid = newsumm.messid;	/* return the new id */
	    mb->boxlen += newsumm.totallen;	/* update box length */
	    ok = TRUE;
	} else {
	    t_perror1("summ_copymess: cannot create ", newname); 	
	}
    } 
	
    sem_release(&mb->mbsem);
    
    return ok;
}
/* summ_deliver --

    Add new message to folder.  Returns FALSE if it's already there.
    Note that we do NOT call touch_folder -- additions don't update
    the folder tag.
*/

boolean_t summ_deliver(mbox *mb, summinfo *insumm, int foldnum, long len) {

    boolean_t	ok;
    
    sem_seize(&mb->mbsem);		
    ok = fold_addsum(mb, &mb->fold[foldnum], insumm);
    sem_release(&mb->mbsem);
 
    return ok;	
}
/* summ_move --

    Remove message from one folder, add to another.  Set new expiration date
    (or leave it alone if newexp == -1).
    
    Returns FALSE if message not in folder.

*/

boolean_t summ_move(mbox *mb, folder *from, folder *to, long messid, long newexp) {

    summinfo	summ;			/* summary info of message deleted */
    boolean_t	ok = TRUE;		/* returned: message found? */
    
    sem_seize(&mb->mbsem);
    
    if (fold_delsum(mb, from, messid, &summ)) {
	if (newexp != -1)		/* set new expiration date? */
	    summ.expire = (u_long) newexp;
	(void) fold_addsum(mb, to, &summ);
	touch_folder(mb, from);		/* update session tag of source folder */
    } else
    	ok = FALSE;			/* message not found */
	
    sem_release(&mb->mbsem);
    
    return ok;
}

/* fold_addsum --

    Add new summary to folder.  Search folder to see if summary is already there; 
    if not, add to end of folder. Add length to fold->foldlen.
        
    --> box locked <--
*/

boolean_t fold_addsum(mbox *mb, folder *fold, summinfo *insumm) {

    summbuck	*p, *q;			/* bucket temps */
    summinfo	*summ;			/* place for summary in bucket */
    char	*nextsum;		/* to locate next summary */

    sem_check(&mb->mbsem);

    if (fold->summs == NULL)		/* read summaries in, if necessary */
	summ_read(mb, fold);
	
    /* search for matching messid */   
    for (q = NULL, p = fold->summs; p != NULL; q = p, p = p->next) {
	for (nextsum = p->data; nextsum - p->data < p->used; nextsum += summ->len) {
	    summ = (summinfo *) nextsum;
	    if (summ->messid == insumm->messid) {
		return FALSE;		/* already there; don't add */
	    }
	}
    }
    
    p = q;				/* back up to last bucket */

    /* see if room; get new bucket if not */
    if (summ_packed_len(insumm) + p->used > SUMMBUCK_LEN) {
	q = (summbuck *) mallocf(sizeof(summbuck));
    	pthread_mutex_lock(&global_lock);
    	++malloc_stats.summbuck;
    	pthread_mutex_unlock(&global_lock);
	q->next = NULL;
	q->count = q->used = 0;    
	p->next = q;
	p = q;	    
    }

    summ = (summinfo *) &p->data[p->used]; /* calculate where next one goes */
    summ_copy(summ, insumm, TRUE);	/* copy summary info, packing */
    p->used += summ->len;		/* update valid length */
    p->count++;				/* one more in this bucket */
    fold->count++;			/* and in folder as a whole */
    fold->foldlen += summ->totallen;	/* length of all messages in folder */
    fold->dirty = TRUE;			/* folder has been modified */
    
    return TRUE;			/* added ok */
}

/* fold_autoexp --

    Return folder's automatic expiration setting, or -1 if not set.
*/

long fold_autoexp(mbox *mb, int foldnum) {

    char	temp[MAX_STR];	/* exppref */
    char	value[MAX_STR];	/* exppref */
    long	newexp;		/* returned: expiration setting */
    char	*p;

    /* check destination folder autoexpire */
    t_sprintf(temp, "%s%d", PREF_AUTOEXP, foldnum);
    if (pref_get(mb, temp, value)) {		/* is it defined? */
    	unquote(temp, value);
	if (strlen(temp) == 0)			/* ignore empty value */
	    return -1;
	p = strtouns(temp, (u_long *) &newexp);/* get expiration differential */
	if (*p == 'm' || *p == 'M') 	/* in months? */
	    newexp = add_months(newexp);
	else
	    newexp = add_days(newexp);
	return newexp;
    } else
    	return -1;
}

/* fold_create --

    Create a new folder with the given name, return its number.
    
    Returns -1 if folder name already in use.
    
*/

int fold_create(mbox *mb, char *fname) {

    int		i;
    int		foldnum = -1;
    
    sem_seize(&mb->mbsem);	/* lock the mailbox */

    /* search for unused folder #, checking duplicate names */
    for (i = 0; i < mb->foldmax; ++i) {
    	if (mb->fold[i].num < 0) {	/* found a hole */
	    if (foldnum < 0) foldnum = i;	/* use first hole */
	} else {
	    if (strcasecmp(fname, mb->fold[i].name) == 0) {
		foldnum = -1;
		goto cleanup;
	    }
	}
    }
    if (foldnum < 0) {			/* no unused #; must grow */
	foldnum = mb->foldmax;		/* add one to end */
	if (foldnum >= FOLD_MAX) {
	    foldnum = -1;		/* too darned many; don't create */
	    goto cleanup;		/* (different error msg in this case?) */
	}
	++mb->foldmax;
	mb->fold = reallocf(mb->fold, mb->foldmax * sizeof(folder));
    }
    
    /* folder # assigned; fill in the info */
    strcpy(mb->fold[foldnum].name, fname);	/* fill in name & # */
    mb->fold[foldnum].num = foldnum;
    summ_read(mb, &mb->fold[foldnum]);	/* set up summaries (none yet) */
    touch_folder(mb, &mb->fold[foldnum]);/* initialize session tag */

cleanup:
    sem_release(&mb->mbsem);	/* done with the mailbox */
    return foldnum;
}
/* fold_delsum --

    Locate summary in folder, make a copy, and delete it from the folder.
    If bucket is now empty, delete it too (exception:  last bucket is never
    deleted.)
    
    Returns FALSE if summary not present in folder.
    
    --> box locked <--
*/

boolean_t fold_delsum(mbox *mb, folder *fold, long messid, summinfo *outsumm) {

    summbuck	*p, *pp;		/* current & previous bucket */
    summinfo	*summ;			/* summary in bucket */
    char	*nextsum;		/* to locate next summary */

    sem_check(&mb->mbsem);
    
    if (fold->summs == NULL)		/* read summaries in, if necessary */
	summ_read(mb, fold);
	
    /* linear search for matching messid */   
    for (pp = NULL, p = fold->summs; p != NULL; pp = p, p = p->next) {
	for (nextsum = p->data; nextsum - p->data < p->used; nextsum += summ->len) {
	    summ = (summinfo *) nextsum;
	    if (summ->messid == messid) {
		summ_copy(outsumm, summ, FALSE); /* copy the summary */
		summ_squeeze(fold, p, summ); /* and remove it from the bucket */
		summ = NULL;		/* sppml */
		
		/* delete empty bucket iff it's not the last one */
		if (p->used == 0 && (pp || p->next)) {
		    if (pp)	
			pp->next = p->next;
		    else fold->summs = p->next;
		    t_free((char *) p);
    		    pthread_mutex_lock(&global_lock);
		    --malloc_stats.summbuck;
		    pthread_mutex_unlock(&global_lock);
		}
		fold->foldlen -= outsumm->totallen; /* update folder length */
		return TRUE;		/* all set! */
	    }
	}				/* (end of one bucket) */
    }					/* (end of entire list) */
    
    return FALSE;			/* not found */
}
/* fold_fname --
    
    Generate folder filename.  Canonical folders use just the folder name,
    user-defined folders look like "Fold3.junk".  The name is escaped
    (special characters given in octal).
*/

void fold_fname(char *fname, mbox *mb, folder *fold) {    

    t_sprintf(fname, "%s/", mb->boxname);	/* begin w/ box treename */
    fname += strlen(fname);

    /* patch for compatibility -- disk file is "InBox", but name
       we show to client is "In Box" */
    if (fold->num == INBOX_NUM)	{
    	escname(INBOX_FILE_NAME, fname);
    } else {
	if (fold->num >= DEFAULT_FOLDCOUNT) {
	    t_sprintf(fname, "Fold%d.", fold->num);
	    fname += strlen(fname);
	}
	escname(fold->name, fname);			/* append escaped folder name */
    }
}
/* fold_list --
    
    Return a list of all folder names, numbers, and message counts, and
    total length (bytes). If "foldnum" is non-negative, do just that folder.
*/

void fold_list(mbox *mb, long foldnum) {

    long		i;
    boolean_t		last;		/* last line of output? */
    
    sem_seize(&mb->mbsem);

    buf_init(mb);			/* buffer up output until box unlocked */

    if (foldnum >= 0)			/* specific folder? */
	fold_list1(mb, foldnum, TRUE);	/* yes - do just it */
    else {
	for (i = 0; i < mb->foldmax; ++i) {
	    if (mb->fold[i].num < 0)
		continue;		/* unused folder # */
	    last = (i == mb->foldmax-1); /* note: fold[foldmax-1] guaranteed non-null */
	    fold_list1(mb, i, last);	/* do each folder in turn */
	}    
    }
    sem_release(&mb->mbsem);   
    buf_flush(mb);			/* ok to write now */
       
}
/* fold_list1 --
    
    Format a single folder listing.
*/

static void fold_list1(mbox *mb, long foldnum, boolean_t last) {

    char 	buf[512];
    char	*bufp;

    if (last)
	buf_putsta(mb, BLITZ_LASTLINE);
    else 
    	buf_putsta(mb, BLITZ_MOREDATA);

    if (mb->fold[foldnum].summs == NULL)  /* get summaries into memory (to count them) */
	summ_read(mb, &mb->fold[foldnum]);
    /* generate <fold #>,<size>,<name>,<bytes> for each folder */
    t_sprintf(buf, "%d,%d,", mb->fold[foldnum].num, mb->fold[foldnum].count); 
    bufp = buf + strlen(buf);
    /* append quoted name */
    bufp = strncpy_and_quote(bufp, mb->fold[foldnum].name, sizeof(buf) - (bufp-buf));
    *bufp++ = ',';
    bufp = numtostr(mb->fold[foldnum].foldlen, bufp); /* append length */   
    buf_putl(mb, buf);

}
/* fold_remove --

    Delete all messages in a folder, then discard the folder itself.
*/

void fold_remove(mbox *mb, folder *fold) {

    char	fname[MAX_STR];
    
    sem_seize(&mb->mbsem);
    
    empty_folder(mb, fold);		/* toss out all messages */
    
    fold_fname(fname, mb, fold);	/* summary filename */
    if (unlink(fname) < 0) {
	t_perror1("fold_remove: cannot unlink ",fname);	
    }
    summ_free(mb, fold);		/* free all summaries */
    fold->num = -1;			/* folder number may be reused */
  
    /* now shrink folder array if room at end */
    while (mb->fold[mb->foldmax-1].num < 0 && mb->foldmax > DEFAULT_FOLDCOUNT) {
	--mb->foldmax;			/* shrink the list */
    }
    mb->fold = reallocf(mb->fold, mb->foldmax * sizeof(folder));   

    sem_release(&mb->mbsem);

}
/* fold_rename --

    Rename an existing folder.
    
    Returns FALSE if duplicate name.
    
*/

boolean_t fold_rename(mbox *mb, int foldnum, char *fname) {

    int		i;
    boolean_t	ok = FALSE;
    char	oldname[FILENAME_MAX];
    char	newname[FILENAME_MAX];
    
    sem_seize(&mb->mbsem);	/* lock the mailbox */

    /* search for unused folder #, checking duplicate names */
    for (i = 0; i < mb->foldmax; ++i) {
    	if (i != foldnum && mb->fold[i].num >= 0) { /* allow renaming to self */
	    if (strcasecmp(fname, mb->fold[i].name) == 0) {
		goto cleanup;
	    }
	}
    }
    
    fold_fname(oldname, mb, &mb->fold[foldnum]);    
    strcpy(mb->fold[foldnum].name, fname);	/* change name in memory */
    fold_fname(newname, mb, &mb->fold[foldnum]);
    
    if (rename(oldname, newname) == 0)		/* rename disk file */
	ok = TRUE;

cleanup:
    sem_release(&mb->mbsem);	/* done with the mailbox */
    return ok;
}

/* fold_size --

    Return number of messages in given folder.
*/

int fold_size(mbox *mb, folder *fold) {

    int		count;
    
    sem_seize(&mb->mbsem);
    
    /* get summaries into memory */
    if (fold->summs == NULL)
	summ_read(mb, fold);
	
    count = fold->count; /* get count */
    sem_release(&mb->mbsem);
    
    return count;
}
/* fold_summary --

    Generate summary lines for the specified range of message positions within the
    folder.
    
    The mailbox must be locked all the while, but it's not prudent to send such a
    potentially large amount of data to the client with the box locked -- if we're
    flow-controlled the box could remain locked for an arbitrary amount of time.
    So, the formatted summaries are constructed in a string of buffers, which are
    sent after the box is unlocked.  
*/

void fold_summary(mbox *mb, folder *fold, long first, long last) {

    long 	count;			/* current position in folder */
    char 	buf[SUMMBUCK_LEN];	/* long enough for max summary */
    summbuck	*p;			/* current bucket */
    summinfo	*summ;			/* current summary in bucket */
    char	*nextsum;		/* to locate next summary */
    
    sem_seize(&mb->mbsem);
    
    if (fold->summs == NULL)		/* get summaries, if not yet present */
    	summ_read(mb, fold);
    
    if (first == -1)			/* handle '$' for last element */
	first = fold->count;
    if (last == -1)
	last = fold->count;
	
    if (first < 1 || last > fold->count || first > last) {
	sem_release(&mb->mbsem);
	print(mb->user, BLITZ_BADARG);	/* first or last out of range */
	return;
    }
    
    buf_init(mb);			/* buffer up output until box unlocked */
    
    count = 1;				/* tracks present position */
    
    for (p = fold->summs; p != NULL && count <= last; p = p->next) {
	for (nextsum = p->data; nextsum - p->data < p->used; nextsum += summ->len) {
	    summ = (summinfo *) nextsum;
	    if (count >= first) {	/* iff within requested range */
		if (count == last)	/* emit status prefix */
		    buf_putsta(mb, BLITZ_LASTLINE);
		else buf_putsta(mb, BLITZ_MOREDATA);
		
		summ_fmt(summ, buf);	/* format the summary info */	
		buf_putl(mb, buf); 	/* and copy line to output buffer */
	    }
	    if (++count > last)		/* can stop when we've done last one requested */
	    	break;
	}
    }    
    
    sem_release(&mb->mbsem);
    
    buf_flush(mb);			/* ok to write now */

    if (count <= last) {		/* fold.count incorrect! */
	print(mb->user, BLITZ_ERROR);
	t_errprint_ll("Folder count inconsistent, uid %ld fold %ld\n", mb->uid, fold->num);
	t_errprint_ll(" ... count = %ld; last = %ld\n", count, last);
    }
    
}
/* fold_total --

    Total the length fields of all summaries in the folder.  
*/

long fold_total(mbox *mb, folder *fold) {

    int 	count;			/* current position in folder */
    summbuck	*p;			/* current bucket */
    summinfo	*summ;			/* current summary in bucket */
    char	*nextsum;		/* to locate next summary */
    long	total = 0;		/* returned: total length */
    
    sem_seize(&mb->mbsem);
    
    if (fold->summs == NULL)		/* get summaries, if not yet present */
    	summ_read(mb, fold);
    
    count = 1;				/* tracks present position */
    
    for (p = fold->summs; p != NULL; p = p->next) {
	for (nextsum = p->data; nextsum - p->data < p->used; nextsum += summ->len) {
	    summ = (summinfo *) nextsum;
	    total += summ->totallen;	/* add up all messages */
	}
    }    
    
    sem_release(&mb->mbsem);
    
    return total;
    
}

/* foldnum_valid --

    See if folder number is valid.
*/

boolean_t foldnum_valid(mbox *mb, int foldnum) {

    return foldnum >= 0 && foldnum < mb->foldmax 
	&& mb->fold[foldnum].num >= 0;
}
/* get_summ --

    Search for summary info for a given messid;
    return pointer to it (and to folder it was found in).
    
    If folder specified, search just there; otherwise search the
    InBox and Trash (in that order).
    
    --> box locked <--
*/

summinfo *get_summ(mbox *mb, long messid, folder **fold) {

    summbuck	*p;			/* current bucket */
    summinfo	*summ;			/* current summary in bucket */
    char	*nextsum;		/* to locate next summary */
    folder	*firstfold, *lastfold;	/* first & last folders to check */
    

    sem_check(&mb->mbsem);
    
    if (*fold == NULL) {		/* default - search InBox & trash */
	firstfold = &mb->fold[INBOX_NUM];
	lastfold = &mb->fold[TRASH_NUM];
    } else
    	firstfold = lastfold = *fold;	/* check just specified folder */

    /* try appropriate folders */
    for (*fold = firstfold ;; *fold = lastfold) {
	if ((*fold)->summs == NULL)	/* get summaries, if not yet present */
	    summ_read(mb, *fold);
	    
	/* linear search for matching messid */   
	for (p =(*fold)->summs; p != NULL; p = p->next) {
	    for (nextsum = p->data; nextsum - p->data < p->used; nextsum += summ->len) {
		summ = (summinfo *) nextsum;
		if (summ->messid == messid)
		    return summ;	/* found it */
	    }
	}
	
	if (*fold == lastfold)
	    break;			/* give up after trying last */
    }

    return NULL;			/* not found */
}

/* set_expr --

    Search all folders for specified message.  If found, change the expiration date.
    
    ?? should this work only on a specific folder ??
    
*/

boolean_t set_expr(mbox *mb, folder *fold, long messid, u_long expdate) {

    summinfo	*summ;			/* current summary in bucket */
    boolean_t	found = FALSE;
    
    sem_seize(&mb->mbsem);
    
    if (summ = get_summ(mb, messid, &fold)) {	/* (sic) */
	summ->expire = expdate; 	/* change the date */
	fold->dirty = TRUE;		/* folder has changed */
	touch_folder(mb, fold);		/* invalidate folder cache */
	found = TRUE;			/* all set */
    }    
    
    sem_release(&mb->mbsem);
    
    return found;			
}


/* summ_check --

    Mailbox consistency check (done when box opened).  Read all folders, create 
    a sorted list of all summaries.  If a summary appears in both more than one
    folder, remove it from all but the first.  Read mess directory, create sorted
    list of all messages actually present.  
    
    Compare the lists.  Any summary that refers to a missing message is deleted.
    If there is a message but no summary, redeliver the message to the Inbox.
    
    --> box locked <--
*/

void summ_check(mbox *mb) {

    summinfo		summbuf;		/* summary info of message */    
    summent 		*summlist;		/* list of summaries */
    int			summlcount, summlmax;	/* current & max length */
    summent 		*summdel;		/* summaries to delete */
    int			summdcount, summdmax;	/* current & max length */
    summent		*messlist;		/* list of messages */
    int			messlcount, messlmax;	/* current & max length */
    summent		*messnew;		/* list of messages to redeliver */
    int			messncount, messnmax;	/* current & max length */
    int			i,j,out;
    folder		*fold;			/* current folder */
    summbuck		*bp;			/* current bucket */
    summinfo		*summ;			/* current summary in bucket */
    char		*nextsum;		/* to locate next summary */
    char		fname[FILENAME_MAX];	/* mess directory name */
    DIR			*messdir;		/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    long		messid;			/* current message file id */	
    char 		logbuf[MAX_STR];	/* log message */
    boolean_t		changed = FALSE;	/* box changed? */
    char		*delim;			/* filename terminator */
    int			foldnum;		/* current folder */

    sem_check(&mb->mbsem);
    
    /* allocate all 4 lists (grown as needed) */
    summlcount = 0; summlmax = 1000;
    summlist = mallocf(summlmax * sizeof(summent));
    summdcount = 0; summdmax = 100;    
    summdel = mallocf(summdmax * sizeof(summent));
    messlcount = 0; messlmax = 1000;
    messlist = mallocf(messlmax * sizeof(summent));
    messncount = 0; messnmax = 100;
    messnew = mallocf(messnmax * sizeof(summent));
    
    		
    mb->boxlen = 0;			/* compute length as we go */
    
    /* combine all summaries into one big list; sort it */ 
    for (foldnum = mb->foldmax-1; foldnum >= 0; --foldnum) {
	fold = &mb->fold[foldnum];
        if (fold->num < 0)	/* ignore unused folder #s */
	    continue;
	
	fold->foldlen = 0;		/* recompute length */
	if (fold->summs == NULL);
	    summ_read(mb, fold);	/* get summaries, if not yet present */
	for (bp = fold->summs; bp != NULL; bp = bp->next) {
	    for (nextsum = bp->data; nextsum - bp->data < bp->used; nextsum += summ->len) {
		summ = (summinfo *) nextsum;
		if (summlcount == summlmax) { 
		    summlmax += 1000;	/* need to grow list */
		    summlist = reallocf(summlist, summlmax * sizeof(summent));
		}				
		/* add the new entry */
		summlist[summlcount].messid = summ->messid;
		summlist[summlcount].fold = fold;		
		summlcount++;		/* one more total */
		mb->boxlen += summ->totallen;	/* keep track of length */
	    }
	}
    }
    sort_messlist(summlist, summlcount);	/* sort all the summaries */

    /* now pass through the list locating duplicates */
    i = 0;			/* summary currently being examined */
    out = 0;			/* output place in list */
    while (i < summlcount) {
    	j = i + 1;		/* look at next */
	/* as long as next is duplicate, keep deleting it */
	while (j < summlcount && summlist[i].messid == summlist[j].messid) {
	    if (summdcount == summdmax) {
		summdmax += 10;	/* need to grow list */
		summdel = reallocf(summdel, summdmax * sizeof(summent));
	    }
	    t_sprintf(logbuf, "Duplicate summary %ld uid %ld",
	    		summlist[j].messid, mb->uid);
	    log_it(logbuf);
	    summdel[summdcount++] = summlist[j];
	    ++j;
	}
	if (out != i) 		/* slide down if necessary */
	    summlist[out] = summlist[i];
	    
	++out;			/* one more output */
	i = j;			/* resume past last duplicate */
    }
    summlcount = out;		/* count w/o duplicates */
        
    /* read message directory, get sorted list of message ids */
    
    strcpy(fname, mb->boxname); strcat(fname, MESS_DIR);
    
    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    messdir = opendir(fname);
    pthread_mutex_unlock(&dir_lock);
    
    if (messdir == NULL) {
	if (pthread_errno() != ENOENT)	/* if no messages yet, dir may not exist */
	    t_perror1("summ_check: cannot open ", fname);
    } else {

	while ((dirp = readdir(messdir)) != NULL) {	/* read entire directory */
		
	    /* skip dot-files & non-numeric names */
	    delim = strtonum(dirp->d_name, &messid);
	    
	    if (*delim == 0) { /* ignore names with non-digits */
		/* insert new id into sorted list */
		
		if (messlcount == messlmax) { 
		    messlmax += 1000;	/* need to grow list */
		    messlist = reallocf(messlist, messlmax * sizeof(summent));
		}
		messlist[messlcount].messid = messid;
		messlist[messlcount].fold = NULL;
		messlcount++;		/* one more total */
	    }
	}
	    
	closedir(messdir);
    }
    sort_messlist(messlist, messlcount);
    
    /* Ok, now run through the two sorted lists, noting any discrepancies */
    
    for (i = 0, j = 0; i < summlcount || j < messlcount; ) {

	/* summary for nonexistent message? */

	if (j >= messlcount ||
	     (i < summlcount && summlist[i].messid < messlist[j].messid)) {
	    if (summdcount == summdmax) {
		summdmax += 10;	/* need to grow list */
		summdel = reallocf(summdel, summdmax * sizeof(summent));
	    }
	    summdel[summdcount++] = summlist[i];
	    i++;			/* next summary; same message */
	}

	/* message exists but no summary? */
	else if (i >= summlcount ||
	    (j < messlcount && summlist[i].messid > messlist[j].messid)) {
	    if (messncount == messnmax) {
		messnmax += 10;	/* need to grow list */
		messnew = reallocf(messnew, messnmax * sizeof(summent));
	    }
	    messnew[messncount++] = messlist[j];
	    j++;			/* next	message; same summary */
	} else {			
	    i++; j++;			/* match; advance in both lists */
	}
    }
    
    /* delete dangling summaries */
    for (i = 0; i < summdcount; i++) {
	(void) fold_delsum(mb, summdel[i].fold, summdel[i].messid, &summbuf);
	mb->boxlen -= summbuf.totallen;	/* correct the box length */
	t_sprintf(logbuf, "Delete dangling summary %ld uid %ld", 
			summdel[i].messid, mb->uid);
	log_it(logbuf);
	if (!changed) {			/* first change? */
	    changed = TRUE;
	    set_sessionid(mb);		/* bump sessionid to flush client cache */
	}
	touch_folder(mb, summdel[i].fold); /* folder modified in this session */
    }
    
    /* create summaries for messages that lack them */
    for (i = 0; i < messncount; i++) {
	if ((summ = mess_scan(mb, messnew[i].messid)) != NULL) {	
	    if (!fold_addsum(mb, &mb->fold[INBOX_NUM], summ)) /* stick it in the inbox */
		t_errprint_ll("summ_check: cannot redeliver mess %ld to uid %ld\n",
						messnew[i].messid, mb->uid); 
	    else {
		t_sprintf(logbuf, "Redeliver mess %ld to uid %ld" ,
					messnew[i].messid, mb->uid);
		log_it(logbuf);
		mb->boxlen += summ->totallen;	/* count its length */
	    }		
	    t_free((char *) summ);
	}
	if (!changed) {			/* first change? */
	    changed = TRUE;
	    set_sessionid(mb);		/* bump sessionid to flush client cache */
	}
	touch_folder(mb, &mb->fold[INBOX_NUM]); /* inbox modified in this session */
    }

    /* free the lists we allocated */
    t_free(summlist);
    t_free(summdel);
    t_free(messlist);
    t_free(messnew); 
        
    mb->checked = TRUE;			/* summaries are now consistent */
	
}
/* 
   Sort an array of message id's in place. Classic heapsort (Knuth Vol. 3)
   Array is arranged into a heap: a complete binary tree where parent's
   key is always >= both children. Tree is represented linearly using the
   array itself: the 2 children of a[i] are at a[2*i+1] and a[2*i+2].
*/
void sort_messlist(summent *sum, int count) {

    int 	l, r;	/* left & right bounds */
    int		i, j; 	/* key indices */
    summent	cur;	/* entry currenty being moved */
    
    if (count <= 1)		/* avoid annoying boundary cases */
    	return;
    
    /* step 1: arrange array into a heap */
    
    l = count / 2; r = count - 1;	/* work on leaves of tree */
    
    /* loop decreases l first, then r */
    for (;;) {
        if (l > 0) {			/* step 1: arrange array into a heap */
	    --l;			/* work on next entry to left */
	    cur = sum[l];		
	} else {			/* step 2: output largest; re-heapify */	
	    cur = sum[r];	
	    sum[r] = sum[0];		/* move largest entry to end */
	    --r;			/* move heap boundary */
	    if (r == 0) {		/* the end */
	    	sum[0] = cur;		/* output last entry */
		break;			/* done */
	    }
	}

	j = l;				/* prepare to sift up larger child */
	
	for (;;) {			/* until larger than both children */
	    i = j; j = 2*j+1;		/* move down */
	    if (j > r)
	    	break;			/* reached a leaf; done */
	    if (j < r && 		/* if both children exist... */
	    	  sum[j].messid < sum[j+1].messid) /* ...and right is bigger... */
	    	++j;			/* ...advance to it */
	    if (cur.messid > sum[j].messid)
	    	break;			/* larger than both children; done */
	    sum[i] = sum[j];		/* move up larger child & loop */
	}
	sum[i] = cur;
    }

}
/* summ_copy --

    Copy a "summinfo" record.  This is tricky because of the pointers and
    variable-length nature of the record.  If "pack" is TRUE, the destination
    record is to be packed, otherwise the fixed offsets are used.
*/

void summ_copy(summinfo *to, summinfo *from, boolean_t pack) {
			
    int		fixedlen;		/* length of fixed fields */
    
    /* compute length of fixed part of header.  note that it is NOT
       safe to assign the entire structure! */
    fixedlen = from->sender_ - (char *) from;
    bcopy(from, to, fixedlen);  	/* this gets all the fixed fields */		
    
    to->sender = to->sender_;		/* now, the varying length fields */
    strcpy(to->sender, from->sender);
    
    if (pack)				/* if packing, use next available spot */
    	to->recipname = to->sender + strlen(to->sender) + 1;
    else
	to->recipname = to->recipname_;	/* using fixed offsets */
    strcpy(to->recipname, from->recipname);

    if (pack)
    	to->topic = to->recipname + strlen(to->recipname) + 1;
    else
	to->topic = to->topic_;
    strcpy(to->topic, from->topic);
    
    /* now that last varying field is set, can compute length */
    if (pack)
    	to->len = 1 + strlen(to->topic) + (to->topic - ((char *) to));
    else
	to->len = sizeof (to);		/* not packed; use full size */
	
    to->len = (to->len + 7) & ~7;	/* round up to 8-byte multiple */
    
}

/* summ_fmt --

    Convert "summinfo" record to ascii text suitable for downloading or storing
    in a summary file.  Note that the string fields are quoted only in this
    ascii representation, not in the actual structure.
*/
void summ_fmt(summinfo *summ, char *buf) {    
	
    t_sprintf(buf, "%ld,%s,%s,%ld,", summ->messid, 
    		summ->date, summ->time, summ->type);
    buf += strlen(buf);
    /* string fields need special quoting */
    buf = strncpy_and_quote(buf, summ->sender, MAX_ADDR_LEN);
    *buf++ = ',';
    buf = strncpy_and_quote(buf, summ->recipname, MAX_ADDR_LEN);
    *buf++ = ',';
    buf = strncpy_and_quote(buf, summ->topic, MAX_TOPC_LEN);
    *buf++ = ',';
   
    t_sprintf(buf, "%ld,%ld,", summ->totallen, summ->enclosures);
    buf += strlen(buf);
    
    if (summ->read)	/* encode read & receipt into single char */
	*buf++ = 'R';
    else if (summ->receipt)
	*buf++ = 'C';
    else
	*buf++ = 'U';
    *buf++ = ',';
    
    t_sprintf(buf, "%lu", summ->expire);    
	
}

/* summ_packed_len --

    Compute length summary will require when packed.
*/

int summ_packed_len(summinfo *summ) {
					
    int		len;			/* returned: byte length */
    
    /* compute length of fixed header */
    len = (summ->sender_) - ((char *) summ);	
    
    len += 1 + strlen(summ->sender);
    len += 1 + strlen(summ->recipname);
    len += 1 + strlen(summ->topic);	/* plus varying strings */
    len = (len + 7) & ~7;		/* round up to 4-byte multiple */
    
    return len;
}

/* summ_parse --

    Parse text form of summary info, filling in a "summinfo" structure.
    The file name is passed for error diagnostics only.
    
    Sample:
    3568897,03/31/91,19:16:11,1,"sender","recipname","Topic",289912,3,U,2769120000
    
    If "pack" is TRUE, the varying-length fields are to be packed contiguously
    (using the minimum amount of storage necessary.)  summ->len will be the
    actual length used.

*/

boolean_t summ_parse(char *buf, summinfo *summ, char *fname, boolean_t pack) {

#define CHECK_COMMA(x)	if (*(x)++ != ',') { t_errprint_s("summ_parse: bad summary: %s\n", fname); return FALSE; }

    buf = strtonum(buf, &summ->messid);
    CHECK_COMMA(buf);
    
    strncpy(summ->date, buf, 8); buf += 8;
    summ->date[8] = 0;
    CHECK_COMMA(buf);
   
    strncpy(summ->time, buf, 8); buf += 8;
    summ->time[8] = 0;
    CHECK_COMMA(buf);
    
    buf = strtonum(buf, &summ->type);
    CHECK_COMMA(buf);
    
    summ->sender = summ->sender_;		/* now, the varying length fields */
    buf = unquote(summ->sender, buf);		
    CHECK_COMMA(buf);		
    
    if (pack)					/* if packing, use next available spot */
    	summ->recipname = summ->sender + strlen(summ->sender) + 1;
    else
	summ->recipname = summ->recipname_;	/* using fixed offsets */
	
    buf = unquote(summ->recipname, buf);	/* get strings w/o quotes */	
    CHECK_COMMA(buf);		

    if (pack)
    	summ->topic = summ->recipname + strlen(summ->recipname) + 1;
    else
	summ->topic = summ->topic_;
	
    buf = unquote(summ->topic, buf);
    CHECK_COMMA(buf);    
    
    /* now that last varying field is set, can compute length */
    if (pack)
    	summ->len = 1 + strlen(summ->topic) + (summ->topic - ((char *) summ));
    else
	summ->len = sizeof (summinfo);		/* not packed; use full size */
	
    summ->len = (summ->len + 3) & ~3;		/* round up to 4-byte multiple */
    
    buf = strtonum(buf, &summ->totallen);
    CHECK_COMMA(buf);
    
    buf = strtonum(buf, &summ->enclosures);
    CHECK_COMMA(buf);    
    
    switch(*buf++) {				/* read & receipt encoding */
	case 'c':				
	case 'C':				/* not read, want receipt */
	    summ->read = FALSE;
	    summ->receipt = TRUE;
	    break;
	case 'r':
	case 'R':				/* read */
	    summ->read = TRUE;
	    summ->receipt = FALSE;
	    break;
	case 'U':
	case 'u':				/* unread, no receipt */
	    summ->read = FALSE;
	    summ->receipt = FALSE;
	    break;
    }
    CHECK_COMMA(buf);    
    
    buf = strtouns(buf, &summ->expire);

    return TRUE;
}

/* summ_read --

    Read summaries from file.
    
    Allocate first bucket.  If file is present, verify magic header line.
    Begin reading summaries in (allocating more buckets as needed).
    
    Note that if fold->summs is NULL, it means summaries have not yet been
    read; if the folder is empty, fold->summs will point to an empty bucket.
    
    --> box locked <--
*/
void summ_read(mbox *mb, folder *fold) {
	
    summbuck	*p;			/* current bucket */
    summbuck	*q;			/* next one */
    summinfo	*summ;			/* current summary in bucket */
    summinfo	insumm;			/* summary read from file */
    char	fname[FILENAME_MAX];	/* summary filename */
    char 	buf[SUMMBUCK_LEN];	/* long enough for max summary */
    t_file	*f;			/* the file */

    sem_check(&mb->mbsem);
       
    pthread_mutex_lock(&global_lock);
    ++mb_stats.summ_r;
    pthread_mutex_unlock(&global_lock);
    
    /* set up initial bucket, even if no summaries */
    p = (summbuck *) mallocf(sizeof(summbuck));
    pthread_mutex_lock(&global_lock);
    ++malloc_stats.summbuck;
    pthread_mutex_unlock(&global_lock);

    p->next = NULL;
    p->count = p->used = 0;
    fold->summs = p;
    fold->count = 0;
    fold->foldlen = 0;
    fold->dirty = FALSE;
    
    fold_fname(fname, mb, fold);	/* generate filename */

    /* create file if not there */
    if ((f = t_fopen(fname, O_RDONLY | O_CREAT, FILE_ACC)) == NULL) {
	t_perror1("summ_read: cannot open", fname);
    	return;				
    }

    /* consistency check on first line of file */
    if (t_gets(buf, sizeof(buf), f) == NULL)
	goto cleanup;			/* no summaries -- easy */
    if (strcmp(buf, SUMM_MAGIC) != 0) {
	t_errprint_s("summ_read: bad header line in %s\n", fname);
	goto cleanup;
    }
    
    /* read summaries in line-by-line */
    while (t_gets(buf, sizeof(buf), f) != NULL) {
    
	if (!summ_parse(buf, &insumm, fname, TRUE)) /* parse summ. info to determine length */
	    continue;			/* bad summary; skip */
	
    	/* see if there's room in current bucket */
	if (p->used + summ_packed_len(&insumm) > SUMMBUCK_LEN) {
	    if (p->used == 0) {		/* oops!  too long for a bucket! */
		t_errprint_s("Summary too long in %s\n", fname);
		continue;
	    }
	    /* get another bucket */
	    q = (summbuck *) mallocf(sizeof(summbuck));
    	    pthread_mutex_lock(&global_lock);
    	    ++malloc_stats.summbuck;
    	    pthread_mutex_unlock(&global_lock);

	    q->next = NULL;
	    q->count = q->used = 0;    
	    p->next = q;
	    p = q;	    
	}
	summ = (summinfo *) &p->data[p->used]; 	/* calculate where next one begins */
	(void) summ_copy(summ, &insumm, TRUE); 	/* copy summary info (packing) */
	p->used += summ->len;			/* update valid length */
	p->count++;				/* one more in this bucket */
	fold->count++;				/* and in folder as a whole */
	fold->foldlen += summ->totallen;	/* compute folder length */
    }

cleanup:  
    t_fclose(f);				/* close summary file */
}


/* summ_write --

    Write summaries to file.  Create temp file; rename it.
    
    --> mailbox locked <--
*/

void summ_write(mbox *mb, folder *fold) {

    char 	buf[SUMMBUCK_LEN];	/* long enough for max summary */
    summbuck	*p;			/* current bucket */
    summinfo	*summ;			/* current summary in bucket */
    char	*nextsum;		/* to locate next summary */
    t_file	*f;			/* the file */
    char	fname[FILENAME_MAX];	/* temp filename */
    char	summname[FILENAME_MAX];	/* summary filename */
   
    if (fold->summs == NULL)		/* if never read in, don't write */
	return;

    pthread_mutex_lock(&global_lock);
    ++mb_stats.summ_w;
    pthread_mutex_unlock(&global_lock);
	
    /* first, get temp file */  
    t_sprintf(fname, "%s/.summtemp", mb->boxname);	

    if ((f = t_fopen(fname, O_WRONLY | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror("summ_write: open ");
	return;
    } 
    
    /* begin file with 1 line of version id magic */
    t_puts(f, SUMM_MAGIC); t_putc(f, '\n');
    
    /* for each summary in each bucket */
    for (p = fold->summs; p != NULL; p = p->next) {
	for (nextsum = p->data; nextsum - p->data < p->used; nextsum += summ->len) {
	    summ = (summinfo *) nextsum;
	    summ_fmt(summ, buf);	/* generate ascii version */
	    t_puts(f, buf);		/* write to temp file */
	    t_putc(f, '\n');		/* one per line */
	}
    }
    
    t_fflush(f);			/* flush, so we detect any errors */
    if (f->t_errno != 0) {
	t_perror1("summ_write: error writing ", fname);
	(void) t_fclose(f);
	unlink(fname);			/* discard the bad file */
	return;				
    }
    (void) t_fclose(f);

    /* now generate result filename */
    fold_fname(summname, mb, fold);	/* generate filename */
    
    if (rename(fname, summname) < 0)	/* move temp to result file */
	t_perror1("summ_write: rename failed: ", summname);
    else
        fold->dirty = FALSE;
}

/* summ_free --

    Free storage used by summaries.
    
    --> mailbox locked <--
*/

void summ_free(mbox *mb, folder *fold) {

    summbuck	*p;			/* current bucket */
       
    /* free each bucket in the folder */
    while (p = fold->summs) {		/* (sic) */
    	fold->summs = p->next;
	t_free(p);
        pthread_mutex_lock(&global_lock);
        --malloc_stats.summbuck;
        pthread_mutex_unlock(&global_lock);

    }	
}

/* summ_squeeze --

    Squeeze a summary out of a bucket.  Slide any subsequent
    summaries in the bucket down to fill the hole.  Relocate
    pointers within the summary struct to account for the move.
    Adjust bucket length, and bucket/folder counts.
    
    Note: empty buckets should _not_ be left allocated to the folder
    list (except for the special case of an empty folder with 1 empty bucket);
    our caller must unlink & free the bucket if it's become empty.
        
    --> mailbox locked <--
*/

void summ_squeeze(folder *fold, summbuck *p, summinfo *summ) {

    int		after;			/* remaining length */
    int		slidelen;		/* length removed */
    char	*nextsum;		/* following summary */
    summinfo	*s;			/* temp */
    
    nextsum = (char *) summ + summ->len;
    
    after = p->used - (nextsum - p->data); /* any after us? */
    slidelen = summ->len;		/* safe-store the length */
    p->used -= slidelen; 		/* adjust length */
    
    if (after > 0) {
	bcopy(nextsum, (char *) summ, after); /* slide them all down */
	/* adjust pointers that point within block just moved */
	for (nextsum = (char *) summ; nextsum - p->data < p->used; ) {
	    s = (summinfo *) nextsum;
	    nextsum += s->len;
	    s->sender -= slidelen;
	    s->topic -= slidelen;
	    s->recipname -= slidelen;
	}
    }

    --p->count;		/* update bucket/folder counts */
    --fold->count;
    fold->dirty = TRUE; /* folder has been modified */

}

/* expire1 --
  
    Check all folders for messages due to expire.  Delete the message,
    log the summary info to explog, update the folder.
    
    We maintain a blank-separated list of exprired messageids in PREF_EXP
    for the client's benefit (to help it maintain its local cache of summary
    info).  If this list gets too long, generate a new sessionid to tell the
    client to invalidate its cache.
    
    If "stolog" is non-null, record uid and total box length there (expiration
    is a convenient time to do this, since we're examining every box.)
    (Note that other threads may be expiring on other disks in parallel; must
    seize exp_sem whenever accessing the log files).
*/

void expire1(long uid, int fs, u_long today) {

    mbox	*mb;			/* box to check */
    folder	*fold;			/* current folder */
    int		foldnum;		/* its number */
    summbuck	*p;			/* current summary chunk */
    summbuck	*pp;			/* previous */
    summbuck 	*nextp;			/* next */
    summinfo	*summ;			/* current summary in bucket */
    char	*nextsum;		/* to locate next summary */
    char 	datestr[9]; 
    char 	timestr[9];
    char	buf[SUMMBUCK_LEN];	/* formatted summary */
    long	count = 0;		/* count of messages expired */
    char	expired[PREF_MAXLEN];	/* list of expired messages */
    char	val[PREF_MAXLEN];	/* quoted copy */
    char	foldexpired[PREF_MAXLEN]; /* '' in current folder */
    char	messid[16];		/* one messid */
    char 	foldpref[32];		/* "Expired<n>" */
    boolean_t	pref_overflow = FALSE;	/* too much to fit in pref? */
    boolean_t	foldpref_overflow = FALSE; /* '' in current folder */
    long	foldcount;		/* count expired in current folder */
    int		dnd_fs;			/* filesystem DND says they belong on */    
    
    /* before working on this box, check against DND to verify that it
       really belongs on this disk (avoid setting up bogus mbox structure) */
    if (uid_to_fs(uid, &dnd_fs) != DND_OK || dnd_fs != fs) {
    	if (uid != pubml_uid) {		/* don't log for this guy */
	    date_time(datestr, timestr);
	    pthread_mutex_lock(&exp.lock);
	    t_fprintf(exp.explog, 
		"%s %s skipping uid %ld; can't confirm they belong on %s\n", 
		datestr, timestr, uid, m_filesys[fs]);
	    pthread_mutex_unlock(&exp.lock);
	}
     	return;
    } 
	
    /* if user is signed on; force a disconnect */		    
    mb = force_disconnect(uid, fs, NULL); /* disconnect user & lock box */
    	
    /* do all folders */
    for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {   
	fold = &mb->fold[foldnum];
	if (fold->num < 0)		/* skip holes */
	    continue;
	    
	if (fold->summs == NULL)	/* read summaries, if not yet here */
	    summ_read(mb, fold);
	foldcount = 0;			/* nothing expired in this folder yet */

	/* for every bucket & every summary */
	for (pp = NULL, p = fold->summs; p != NULL; p = nextp) {
	    nextp = p->next; 
	    for (nextsum = p->data; p != NULL && nextsum - p->data < p->used; ) {
		summ = (summinfo *) nextsum; 	/* cast pointer to current summ */
		nextsum += summ->len;		/* compute where next one will be */
				
		if (summ->expire <= today) { 	/* due to expire? */
		
		    /* log date, uid, and summary to explog */
		    date_time(datestr, timestr);
		    summ_fmt(summ, buf);
		    pthread_mutex_lock(&exp.lock);
		    t_fprintf(exp.explog, "%s %s uid %ld; %s\n", datestr, timestr, uid, buf);
		    pthread_mutex_unlock(&exp.lock);
			
		    /* first time, get current list of expired stuff */
		    if (count == 0) {
			if (pref_get_int(mb, PREF_EXP, val)) 
			    unquote(expired, val);
			else
			    strcpy(expired, "");
		    }

		    numtostr(summ->messid, messid);

		    /* when expiring from InBox or trash, update generic
		       list of deleted messageids (for old clients) */
		    
		    if (fold->num == INBOX_NUM || fold->num == TRASH_NUM) {
			/* is there room, allowing for quotes & space & terminator
			    (also allow 4 chars for status code, in case client is has
			    255-char response line limit) */
			if (strlen(messid) + strlen(expired) + 1 + 2 + 4 >= PREF_MAXLEN)
			    pref_overflow = TRUE;		/* too much to fit */
			else {
			    if (strlen(expired) > 0)
				strcat(expired, " ");
			    strcat(expired, messid);
			}
		    }
		    
		    /* same deal now for per-folder expiration list: */
		    t_sprintf(foldpref, "%s%d", PREF_EXP, fold->num);
		    
		    if (foldcount == 0) {	/* get folder's expriation list */
			if (pref_get_int(mb, foldpref, val)) 
			    unquote(foldexpired, val);
			else
			    strcpy(foldexpired, "");
		    }		    
		    if (strlen(messid) + strlen(foldexpired) + 1 + 2 + 4 >= PREF_MAXLEN)
			foldpref_overflow = TRUE;	/* too much to fit */
		    else {
			if (strlen(foldexpired) > 0)
			    strcat(foldexpired, " ");
			strcat(foldexpired, messid);
		    }


		    /* delete the message itself */
		    mess_rem(mb, summ->messid, summ->totallen);
		    fold->foldlen -= summ->totallen;
		    
		    /* squeeze this summary out of the bucket */
		    summ_squeeze(fold, p, summ);
		    
		    nextsum = (char *) summ;	/* stay in the same place */

		    /* delete empty bucket iff it's not the only one */
		    if (p->used == 0 && (pp || p->next)) {
			if (pp)	
			    pp->next = p->next;
			else fold->summs = p->next;
			t_free((char *) p);
    			pthread_mutex_lock(&global_lock);
    			--malloc_stats.summbuck;
    			pthread_mutex_unlock(&global_lock);

			p = NULL;
		    }
		    		    
		    /* count # of messages expired */
		    ++count; ++foldcount;
		    
		} 
	    }
	    if (p != NULL)			/* unless deleted current bucket */
		pp = p;				/* advance backpointer */
	}
	
	/* if folder isn't dirty, free summaries now (don't hog memory) */
	if (!fold->dirty) /* inbox summaries */
	    summ_free(mb, fold);

	if (foldcount > 0) {		/* rewrite pref iff we did something in this folder */
	    strncpy_and_quote(val, foldexpired, PREF_MAXLEN);
	    pref_set_int(mb, foldpref, val);/* set list of expired messids */
	
	    if (foldpref_overflow) {	/* if list is incomplete */
		set_sessionid(mb);	/* bump sessionid */
	    }
	    touch_folder(mb, fold); 	/* either way, stamp folder w/ sessionid */
	}
	    
    }

    if (count > 0) {			/* rewrite pref iff we did something */
	strncpy_and_quote(val, expired, PREF_MAXLEN);
	pref_set_int(mb, PREF_EXP, val);/* set list of expired messids */
    
	if (pref_overflow)		/* if PREF_EXP list is incomplete */
	    set_sessionid(mb);		/* bump sessionid to invalidate cache */
    }
    
    pthread_mutex_lock(&exp.lock);
    if (exp.stolog) {			/* generate storage report? */
	int 	i;
	long 	messcount = 0;
    	for (i = 0; i < mb->foldmax; ++i) {
	    if (mb->fold[i].num < 0)
		continue;		/* unused folder # */
	    messcount += mb->fold[i].count;
	}
	t_fprintf(exp.stolog, "%ld %ld %ld %s\n", uid, messcount, mb->boxlen, m_filesys[mb->fs]);
    }
    exp.total += count;
    pthread_mutex_unlock(&exp.lock);
    
    sem_release(&mb->mbsem);		/* box can change now */
    mbox_done(&mb);
	
}
