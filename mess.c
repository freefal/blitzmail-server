/*  BlitzMail Server -- messages

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.


    Messages are stored within the "mess" subdirectory of the user mailbox directory.
    A message with multiple recipients will typically result in one file with
    several (hard) links to it.  If there are multiple filesystems holding user
    mailboxes, there will be multiple copies of the message (as many as one per
    filesystem), since hard links cannot cross filesystem boundaries.
    
    During delivery, a copy of the message is saved in a temp directory on each
    filesystem, and then links from recipient mailboxes are added.  When the last
    recipient has received the message, the temp files are unlinked.
    
    A message is deleted from one user's box by unlinking it; the file system takes
    care of maintaining the link count & freeing the storage when the last link
    is gone.
        
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
#include "deliver.h"
#include "queue.h"

/* clean_encl_list --

    Run through list of "enclinfo"s freeing them up.  Also unlink any files
    marked as temps.
*/

void clean_encl_list(enclinfo **elist) {

    enclinfo	*ep = *elist;
    enclinfo	*next;
    
    while (ep) {
    	finfoclose(&ep->finfo);
	next = ep->next;
	t_free((char *) ep);
	ep = next;
    }
    
    *elist = NULL;			/* list is empty now */
}
/* finfoclose --

    Clean up a "fileinfo" -- if "temp" is set, unlink the file.
*/

void finfoclose(fileinfo *finfo) {

	if (finfo->fname[0] && finfo->temp) {
	    if (unlink(finfo->fname) < 0)
		t_perror1("finfoclose: can't unlink ", finfo->fname);
	}
	finfo->fname[0] = 0;
	finfo->len = 0;
}

/* finfocopy --

    Copy "file" (as described by a "fileinfo" structure) to the end of a
    t_file stream.
*/

boolean_t finfocopy(t_file *out, fileinfo *in) {

    char 	buf[8192];		/* use a healthy-sized buffer */
    long	totallen = 0;		/* total len read so far */
    int		len;			/* length this time */
    int		l;
    t_file	*inf = NULL;
    boolean_t	ok = TRUE;
    
    if ((inf = t_fopen(in->fname, O_RDONLY, 0)) == NULL) {
	t_perror1("finfocopy: cannot open ", in->fname);
	return FALSE;
    }
    
    (void) lseek(inf->fd, in->offset, SEEK_SET); /* seek to starting place */

    while(totallen < in->len) {
	len = sizeof(buf);		/* read a buffer full */
	if (len > in->len - totallen)	/* or until end of file chunk */
	    len = in->len - totallen;
	len = read(inf->fd, buf, len);
	if (len < 0) {
	    t_perror1("finfocopy: error reading ", in->fname);
	    ok = FALSE;
	    break;
	}
	totallen += len;
	if (len == 0 && totallen < in->len) {
	    t_perror1("finfocopy: Unexpected eof on ", in->fname);
	    ok = FALSE;
	    break;			/* don't spin */
	}
	
	l = t_fwrite(out, buf, len);	/* write the whole buffer */
	if (l < len) {			/* error//urgent//disconnect */
	    ok = FALSE;
	    if (out->t_errno) {		/* is it an io error? */
		/* don't log remote disconnect */
		if (pthread_errno() != EPIPE && pthread_errno() != ESPIPE)	
		    t_perror1("finfocopy: error writing ", out->name);
	    }
	    break;
	}
    }
            
    (void) t_fclose(inf);		/* done with input file */
    
    return ok;
}

/* temp_finfo --

    Set up a "fileinfo" for a temp file.
*/

void temp_finfo(fileinfo *finfo) {

    static int	n = 1;		/* protected by global_lock */
    
    pthread_mutex_lock(&global_lock);
    t_sprintf(finfo->fname, "/tmp/blitztmp%d", n++);
    finfo->temp = TRUE;		/* unlink this file when done */
    pthread_mutex_unlock(&global_lock);
}

/* mess_deliver --

    If recipient's filesystem doesn't yet have a copy of the message, make
    one.  Add link from recipient's message subdirectory to temp file, and
    update mb->boxlen.
    
    Returns TRUE if link created, FALSE if it already existed or the delivery
    otherwise failed.
    
*/

boolean_t mess_deliver(mbox *mb, messinfo *mi, long len, char *err) {

    char 	tmpname[FILENAME_MAX];
    char	messname[FILENAME_MAX];
    char	*p;
    t_file	*f;
    boolean_t	ok = TRUE;
    
    strcpy(err, "");			/* no error yet */

    if (mb->fs == m_spool_filesys)	/* can we use spool copy of message? */
	strcpy(tmpname, mi->finfo.fname); /* yes */
    else {
	mess_tmpname(tmpname, mb->fs, mi->messid);	
    
	if (!mi->present[mb->fs]) {	/* need to copy file to this fs? */
	    f = t_fopen(tmpname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC);
	    if (!f) {
	    	t_perror1("Error creating temp file ", tmpname);
		strcpy(err, "Unable to create temp file");
	    	ok = FALSE;
	    } else {
		if (!finfocopy(f, &mi->finfo))  	/* copy from spool fs to recip fs */
		    ok = FALSE;
                if (ok && t_fflush(f) < 0)            	/* flush, so we detect any errors */
		    ok = FALSE;
    		if (!ok) {
                    t_perror1("Error writing temp file ", tmpname);
		    strcpy(err, "Insufficient disk space/disk trouble copying message");
		}
		t_fclose(f);
	    }
	    if (!ok)
		return FALSE;
	    mi->present[mb->fs] = TRUE;
	}	
    }
    mess_name(messname, mb, mi->messid);
      
    ok = link(tmpname, messname) == 0; 	/* add to message dir */
	    
    if (!ok) {    
	/* didn't work; maybe the mess directory doesn't exist yet? */
	if (pthread_errno() == ENOENT) { 	
	    p = rindex(messname,'/');
	    *p = 0;				/* chop to directory name */	
	    if (mkdir(messname, DIR_ACC) < 0) {	/* create directory  */
		t_perror1("mess_deliver: cannot create ", messname); 
		return FALSE;
	    }
    
	    *p = '/';			/* restore full pathname */	
	    ok = link(tmpname, messname) == 0; /* and try again */
	} 	  
    }
    
    if (ok) {				/* link() worked? */
	sem_seize(&mb->mbsem);
	mb->boxlen += len;		/* yes - update length of total box */
	sem_release(&mb->mbsem);
	return TRUE;
    }
    /* if file already exists, no big deal (just means user was on
       recipient list twice) */
    if (pthread_errno() != EEXIST) {
	t_perror1("mess_deliver: link failed ", messname);
	strcpy(err, "Error copying message to recipient mailbox");
    }
    return FALSE;
}

/* mess_done --

    Message delivery has completed; remove the temp files created by mess_deliver
    (only links to the message will be from recipient mailboxes).  Depending on the
    circumstances, the copy of the message on the spool filesystem may not be located
    in the temp directory (it might be a queue file), or there might not even be a copy
    on the spool filesystem. We want to remove just the additional copies so we skip the 
    spool fs and the file named in the "finfo" struct.
*/

void mess_done(messinfo *mi) {

    int 	i;
    char	name[FILENAME_MAX];
    
    for (i = 0; i < m_filesys_count; i++) {
    	if (mi->present[i] && i != m_spool_filesys) { 	/* for each fs it's on */
	    mess_tmpname(name, i, mi->messid);
	    if (strcmp(name, mi->finfo.fname) != 0) { /* unless finfoclose will get it */
		if (unlink(name) < 0)
		    t_perror1("mess_done: cannot unlink ",name);
	    }
	}
    }
    
    finfoclose(&mi->finfo);
}

/* mess_get --

    Open a message in user context -- fill in all the udb fields.
    In addition to the message file itself, the summary info is also
    filled in.
*/

boolean_t mess_get(udb *user, folder **fold, long messid) {

    char	fname[MESS_NAMELEN];
    summinfo	*summ;
    
    sem_seize(&user->mb->mbsem);	/* lock for get_summ */
    
    /* search box for summary for this message */
    if ((summ = get_summ(user->mb, messid, fold)) == NULL) {
	sem_release(&user->mb->mbsem);
	return FALSE;			/* not found */
    }
    
    /* copy summary info into udb (don't pack) */
    summ_copy(&user->summ, summ, FALSE);
    user->currmessfold = (*fold)->num;	/* remember what folder it's in */
    sem_release(&user->mb->mbsem);	/* safe to unlock now */
    
    mess_name(fname, user->mb, messid);	/* get message name */
    
    if (!mess_open(fname, &user->head, &user->text, &user->ep, NULL, &user->summ.type)) {
	user->summ.messid = -1;		/* not there; invalidate summ too */
	user->currmessfold = -1;
	return FALSE;
    }
    
    return TRUE;
}

/* mess_init --

    One-time initialization.  Must be called before any other message routines.
*/

void mess_init() {

    static t_file f;			/* t_file entry for t_fdmap */
    char	buf[MAX_STR];
    
    sem_init(&messid_sem, "messid_sem");
 
    /* open file that records messid.  Do NOT create it if missing. */
    if ((messid_f = open(f_messid, O_RDWR, 0)) < 0) {
	t_perror1("Panic!  Messid file missing: ", f_messid);
	abortsig();
    }
    t_fdopen(&f, messid_f);		/* set up t_file and fill in name... */
    strcpy(f.name, "messid_f");		/* ...for debugging */
    
    if (t_gets(buf, sizeof(buf), &f) == NULL) {
	t_perror1("Panic! Messid file empty/unreadable: ", f_messid);
	abortsig();
    }
    strtonum(buf, &next_mess_id);		/* read next unused id */
    
    if (next_mess_id <= 1) {			/* messid 1 already used... */
	t_errprint_s("Panic! Invalid messid file: %s", f_messid);
	abortsig();
    }        
}

/* mess_name --

    Generate pathname for given mailbox & messid.
    
    For example, "/blitz1/box/23868/mess/12345"
*/

void mess_name(char *name, mbox *mb, long messid) {
     
    t_sprintf(name, "%s%s%ld", mb->boxname, MESS_DIR, messid);

}

/* mess_open --

    Open message, read file header, set up "fileinfo" structures for header, text
    and any enclosures.
    
    Returns false if message file unavailable.
*/

boolean_t mess_open(char *name, fileinfo *head, fileinfo *text, enclinfo **encl,
			 long *lof, long *mtype) {

    filehead	fh;			/* file header */
    t_file	*mess;			/* open message file */
    enclhead	eh;			/* enclosure header */
    enclinfo	*new, *tail = NULL;	/* for constructing encl list */
    long	pos, eof;		/* current file pos & eof */
    int		len;
 
    *encl = NULL;			/* no enclosures yet */
       
    if ((mess = t_fopen(name, O_RDONLY, 0)) == NULL) {
	t_perror1("mess_open: ", name);
	goto BADMSG;
    }
    
    /* pick up file header */
    if ((len = t_fread(mess, (char *) &fh, FILEHEAD_LEN)) != FILEHEAD_LEN) {
	if (len < 0)
	    t_perror1("mess_open: error reading ", name);
	else
	    t_errprint_s("mess_open: incomplete file header in %s", name);
	goto BADMSG;
    }
    
    /* verify magic bytes & version number */
    if (ntohl(fh.magic) != MESSFILE_MAGIC || FH_VERS(fh.verstype) != MESSFILE_VERS) {
	t_errprint_s("mess_open: not a Blitz message: %s", name);
	goto BADMSG;
    }
    
    /* verify message type */
    *mtype = FH_TYPE(fh.verstype);
    if (*mtype != MESSTYPE_BLITZ && *mtype != MESSTYPE_RFC822) {
	t_errprint_s("mess_open: unknown message type: %s", name);
	goto BADMSG;
    }
  
    /* set up head & text fileinfos */
    strcpy(head->fname, name);		/* in this file */
    head->temp = FALSE;			/* don't unlink file when done! */
    head->offset = ntohl(fh.headoff);
    head->len = ntohl(fh.headlen);
    
    strcpy(text->fname, name);			
    text->temp = FALSE;				
    text->offset = ntohl(fh.textoff);
    text->len = ntohl(fh.textlen);
    
    /* if there's anything after text, we have enclosures */    
    pos = text->offset + text->len;
    eof = t_fseek(mess, 0, SEEK_END);		/* compute lof */

    if (lof)					/* return lof to caller? */
	*lof = eof;
			    
    while (pos < eof) {
    
	t_fseek(mess, pos, SEEK_SET);		/* start of next encl */
	
	/* get enclosure node; link it to udb list */
	new = (enclinfo *) mallocf(sizeof(enclinfo));
	if (*encl == NULL)
	    *encl = new;
	else
	    tail->next = new;
	new->next = NULL;
	tail = new;
	
	if (pos + EHEAD_LEN > eof) {
	    t_errprint_s("mess_open: incomplete enclhead in %s", name);
	    goto BADMSG;
	}
	
	/* read enclosure header, copy info */
	if (t_fread(mess, (char *) &eh, EHEAD_LEN) != EHEAD_LEN) {
	    t_errprint_s("mess_open: error reading enclhead in %s", name);
	    goto BADMSG;	
	}
	new->finfo.len = ntohl(eh.encllen);
	strncpy(new->name, eh.name, ENCLSTR_LEN);
	strncpy(new->type, eh.type, ENCLSTR_LEN);
	/****** allow nulls in type?? *****/
	if (ntohl(eh.namelen) >= ENCLSTR_LEN) {
	    t_errprint_s("mess_open: encl name too long in %s", name);
	    new->name[ENCLSTR_LEN-1] = 0;
	}
	if (ntohl(eh.typelen) >= ENCLSTR_LEN) {
	    t_errprint_s("mess_open: encl type too long in %s", name);
	    new->type[ENCLSTR_LEN-1] = 0;
	}
			
	new->finfo.offset = pos + EHEAD_LEN;
	strcpy(new->finfo.fname, name);
	new->finfo.temp = FALSE;		/* not in a temp file */

	pos += EHEAD_LEN + new->finfo.len;	/* compute where encl ends */
	
	if (pos > eof) {
	    t_errprint_s("mess_open: incomplete enclosure in %s", name);
	    goto BADMSG;
	}	
    }
    
    (void) t_fclose(mess);
    
    return TRUE;				/* ok! */
    
BADMSG:						/* trouble w/ message; clean up */
    if (mess != NULL)
	(void) t_fclose(mess);
	
    head->fname[0] = 0;				/* header & text are invalid */
    text->fname[0] = 0;
    clean_encl_list(encl);			/* clean up enclosures */
        
    return FALSE;
}

/* mess_rem --

    Remove message file from user's mailbox.  
    
    Returns FALSE if the message is not present or cannot be unlinked.
    
    --> box locked <--
*/
boolean_t mess_rem(mbox *mb, long messid, long len) {

    char	name[FILENAME_MAX];	/* message pathname */
        
    mess_name(name, mb, messid);	/* generate the name */
    if (unlink(name) < 0)		/* and try to unlink it */
	return FALSE;
    else {
    	mb->boxlen -= len;
    	return TRUE;
    }
}
/* mess_scan --

    Scan message in mailbox to extract summary fields.  
    
*/

summinfo *mess_scan(mbox *mb, long messid) {

    summinfo	*summ;			/* returned: summary info */
    fileinfo	head;			/* header info */
    fileinfo	text;			/* and text (don't care) */
    enclinfo	*encl;			/* enclosure list */
    long	mtype;			/* message type */

    char	messname[FILENAME_MAX];	/* message pathname */

    mess_name(messname, mb, messid);
    
    if (!mess_open(messname, &head, &text, &encl, NULL, &mtype))
	return NULL;			/* message not available */
	
    summ = mess_scan_head(&head, &text, encl, mtype);	/* scan the header */
    summ->messid = messid;		/* don't trust header, since we know real id */
    summ->type = mtype;			/* get message type from file header */
    clean_encl_list(&encl);		/* clean up enclosures */    
 
    return summ;			/* return summary info */	
}

/* mess_scan_head --

    Scan arbitrary message file to extract summary fields.
    The Message-id field is used to fill in summ->messid; this
    will be wrong if the message is not locally-generated, so
    the caller should fill in the correct messid if it is known.
    
*/

summinfo *mess_scan_head(fileinfo *head, fileinfo *text, enclinfo *encl, long mtype) {

    summinfo	*summ;			/* returned: summary info */
    enclinfo	*ep;			/* one enclosure */
    t_file	*f;			/* open header stream */
    char 	*buf;			/* next header line */
    long	remaining;		/* header left to read */
    char 	*p;			/* temp */
    char	sender[MAX_ADDR_LEN];	/* sender name */
    char	fromaddr[MAX_ADDR_LEN]; /* and address */
    char	zot[MAX_ADDR_LEN];	/* not used */
    char 	recipname[MAX_ADDR_LEN];/* 1st recipient name */
    char	subject[MAX_TOPC_LEN];	/* topic */
    boolean_t	receipt = FALSE;	/* return receipt? */
    boolean_t	return_path = FALSE;	/* return path? */
    boolean_t	got_date = FALSE;	/* saw date? */
    
    *sender = *fromaddr = *recipname = *subject = 0;
    
    if ((f = t_fopen(head->fname, O_RDONLY, 0)) == NULL) {
	t_perror1("mess_scan_head: cannot open ", head->fname);
	return NULL;
    }
    
    (void) t_fseek(f, head->offset, SEEK_SET); /* seek to start of header */

    summ = (summinfo *) mallocf(sizeof(summinfo));
    summ->len = sizeof(summinfo);
    summ->messid = 0;
    
    /* read entire header */

    remaining = head->len;
    while ((buf = getheadline(f, &remaining, TRUE)) != NULL) {	
    	p = index(buf, ':');	/* find end of field name */
	if (p) {
	    *p++ = 0;		/* chop to just name */
	    while(isascii(*p) && isspace(*p))	/* skip ' ' after colon */
		++p;
	    if (strcasecmp(buf, "DATE") == 0) { /* try to keep date */
		if (!parse_date(p, summ->date, summ->time, NULL)) 
		    date_time(summ->date, summ->time);
		got_date = TRUE;
	    } else if (strcasecmp(buf, "FROM") == 0) {
		if (strlen(p) >= MAX_ADDR_LEN)
		    p[MAX_ADDR_LEN-1] = 0; /* watch out for outrageously long From: */
		splitname(p, sender, fromaddr);
	    } else if (strcasecmp(buf, "MESSAGE-ID") == 0) {
		while (*p && *p != '<')
		    ++p;
		if (*p == '<')
		    strtonum(p+1, &summ->messid);
	    } else if (strcasecmp(buf, "RETURN-PATH") == 0)
		return_path = TRUE;
	    else if (strcasecmp(buf, "RETURN-RECEIPT-TO") == 0)
		receipt = TRUE;
	    else if (strcasecmp(buf, "SUBJECT") == 0) {
		strncpy(subject, p, MAX_TOPC_LEN);
		subject[MAX_TOPC_LEN-1] = 0;
	    } else if (strcasecmp(buf, "TO") == 0) {
		if (strlen(p) >= MAX_ADDR_LEN)
		    p[MAX_ADDR_LEN-1] = 0; /* watch out for outrageously long line */
	    	splitname(p, recipname, zot);
		if (index(p, ',') != NULL && strlen(recipname)+4 < MAX_ADDR_LEN) {
		    strcat(recipname, ",...");	/* indicate multiple recipients */
		}
	    }
	}
	t_free(buf);
    }
    
    /* header scanned; now fill in summary info */   
    
    if (!got_date)			/* fill in date if missing */
	date_time(summ->date, summ->time);

    summ->type = mtype;			/* message type supplied by caller */
	
    summ->enclosures = 0;
    summ->totallen = head->len + text->len;
    for (ep = encl; ep; ep = ep->next) {
	summ->enclosures++;		/* enclosure count */
	summ->totallen += ep->finfo.len;/* and length */
    }
    if (summ->enclosures == 0)		/* if no enclosures ... */
	summ->totallen = text->len;	/* show just text length (not header) */
    summ->receipt = receipt;
    summ->read = FALSE;
    summ->expire = pick_expire(summ);	/* set expiration date */
    	
    /* if message came from outside but is masquerading as local */
    if (return_path && !isinternet(fromaddr, NULL)) {
	strcpy(sender, "???");		/* flag sender as suspicious in summary info */
    }
    /* used fixed offsets for strings */
    summ->sender = summ->sender_;
    summ->recipname = summ->recipname_;
    summ->topic = summ->topic_;
    strncpy(summ->sender, sender, MAX_ADDR_LEN);
    summ->sender[MAX_ADDR_LEN-1] = 0;
    strncpy(summ->recipname, recipname, MAX_ADDR_LEN);
    summ->recipname[MAX_ADDR_LEN-1] = 0;
    strncpy(summ->topic, subject, MAX_TOPC_LEN);
    summ->topic[MAX_TOPC_LEN-1] = 0;
 
    summ->len = sizeof(summinfo);  	/* fixed length */
    
    (void) t_fclose(f);			/* close the file */
    
    return summ;			/* return our hard-won info */
}
/* mess_copy_contenthead --

    Scan message header copying selected fields to output file. Only fields
    that are listed in m_okhead are copied.
    
*/

boolean_t mess_copy_contenthead(fileinfo *head, t_file *outf) {

    t_file	*f;			/* open header stream */
    char 	*buf;			/* next header line */
    long	remaining;		/* header left to read */
    int		len;			/* field name length */
    char 	*p;			/* temp */
    int		i;			/* temp */
    
    if ((f = t_fopen(head->fname, O_RDONLY, 0)) == NULL) {
	t_perror1("mess_copy_contenthead: cannot open ", head->fname);
	return FALSE;
    }
    
    (void) t_fseek(f, head->offset, SEEK_SET); /* seek to start of header */
    
    /* read header, but don't unfold lines */
    remaining = head->len;
    while ((buf = getheadline(f, &remaining, FALSE)) != NULL) {
	p = index(buf, ':');	/* find end of field name */
	if (p) {
	    len = (p - buf) + 1;	/* field name length, including ':' */
	    for (i = 0; i < m_okheadcnt; ++i) {	/* a field we recognize? */
		if (strncasecmp(buf, m_okhead[i], strlen(m_okhead[i])) == 0) {
		    t_fprintf(outf, "%s\r", buf); /* copy folded line to output */
		    break;		/* get next line */
		}
	    }
	}
	t_free(buf);
    }
        
    (void) t_fclose(f);			/* close the file */
    
    return TRUE;			/* copy went ok */
}

/* mess_setup --

    Begin delivery process:  combine pieces of message into one file, saved in
    the message temp directory of the given filesystem (usually the spool filesystem.)
    (If another filesystem is to be used, caller must contain itself to that fs; the
    auto-copy features of mess_deliver and mess_done assume the spool fs has the primary copy).
        
    Note that "encl" may be non-null even for MESSTYPE_RFC822 messages;
    in that case the "enclosure list" is just a mechanism for concatenating
    multiple chunks of data into the message text.

    Returns false if message can't be created (disk full, etc.)
*/
boolean_t mess_setup(long messid, fileinfo *head, fileinfo *text, 
		     enclinfo *encl, messinfo *mi, int fs, long mtype) {

    t_file	*f;
    filehead	*fp;			/* pointer to it */
    enclinfo 	*ep;
    enclhead	eh;
    int		i;
    	
    mi->messid = messid;		/* use caller's messid choice */
    
    for (i = 0; i < m_filesys_count; ++i)
    	mi->present[i] = FALSE;		/* haven't saved copy anywhere yet */
	
    /* set up file header */
    fp = (filehead *) mallocf(FILEHEAD_LEN);
    fp->magic = htonl(MESSFILE_MAGIC);		/* file identifier */
    fp->verstype = FH_VERSTYPE(MESSFILE_VERS,mtype); /* version // message type */
    fp->headoff = htonl(FILEHEAD_LEN);		/* message header follows file header */
    fp->headlen = htonl(head->len);		/* for this much */
    fp->textoff = htonl(FILEHEAD_LEN + head->len); /* followed immediately by text */

    /* compute text length: if MESSTYPE_RFC822, "encl"s are just more text */
    fp->textlen = text->len;
    if (mtype == MESSTYPE_RFC822) {
	for (ep = encl; ep != NULL; ep = ep->next) 
		fp->textlen += ep->finfo.len;	/* compute total text length */
    }
    fp->textlen = htonl(fp->textlen);	/* finally, fix byte order */

    /* write a copy to specified filesystem */
    mess_tmpname(mi->finfo.fname, fs, mi->messid);	
    mi->finfo.offset = 0;
    mi->finfo.temp = TRUE;		/* unlink upon close */
    
    if (fs != -1)			/* if this filesys has mailboxes */
	mi->present[fs] = TRUE;		/* we have a copy there */
    	
    f = t_fopen(mi->finfo.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC);
    if (!f) 
	return FALSE;			/* oops; couldn't create the file */
	
    t_fwrite(f, (char *) fp, FILEHEAD_LEN); /* write file header */

    if (!finfocopy(f, head))	/* append message header */
	goto BADMESS;
    if (text->len && !finfocopy(f, text)) /* text may be null */
	goto BADMESS;
    
    /* for each encl, write header info & enclosure file
       "encl" may be non-null even for MESSTYPE_RFC822 messages;
	in that case it's just a list of additional chunks to 
	be concatenated to the text (no encl header) */
    for (ep = encl; ep != NULL; ep = ep->next) {
        if (mtype == MESSTYPE_BLITZ) {	/* only MESSTYPE_BLITZ gets header */
	    eh.encllen = htonl(ep->finfo.len);	/* create standard encl header */
	    eh.typelen = htonl(strlen(ep->type));
	    strcpy(eh.type, ep->type);
	    eh.namelen = htonl(strlen(ep->name));
	    strcpy(eh.name, ep->name);
	    t_fwrite(f, (char *) &eh, EHEAD_LEN); /* write header */
	}
	if (!finfocopy(f, &ep->finfo)) /* write enclosure file itself */
	    goto BADMESS;
    }
    
    t_fflush(f);			/* flush, so we detect any errors */
    if (f->t_errno != 0) {
	t_perror1("mess_setup: error writing ", mi->finfo.fname);
	goto BADMESS;
    }
    
    mi->finfo.len = t_fseek(f, 0, SEEK_END);	/* compute lof */

    (void) t_fclose(f);
    t_free(fp);
    return TRUE;
    
BADMESS:	/* trouble creating message: back out */
    t_free(fp);
    (void) t_fclose(f);
    (void) unlink(mi->finfo.fname);
    mi->finfo.temp = FALSE;	/* don't try to unlink again */
    return FALSE;
}

/* mess_tmpname --

    Generate message tempfile name for given filesystem & messid.
    
    For example, "/blitz1/mtmp/123456"
*/

void mess_tmpname(char *name, int fs, long messid) {
    
    t_sprintf(name, "%s%s%ld", (fs == -1 ? m_spoolfs_name : m_filesys[fs]), 
    			MESSTMP_DIR, messid);
}

/* mess_xfername --

    Generate message transfer name for given filesystem,
    source server & messid.  Hash on low-order 2 digits of messid.
    
    For example, "/blitz1/messxfer/56/vixen.123456"
*/

void mess_xfername(char *name, int fs, long messid, char *srchost) {
    
    t_sprintf(name, "%s%s/%ld/%s.%ld", m_filesys[fs], MESSXFER_DIR, messid % 100,
    				  srchost, messid);
}

/* next_messid --

    Message ids are simply sequential integers.  Assign one & increment.
    
    Write the new message id to our file; fsync to force the disk write
    to go through.  It's a serious problem if the latest messid isn't
    recorded (reusing messid's is a real no-no), so io trouble here
    is fatal.
*/

long next_messid () {

    long 	messid;
    char	buf[16];
    
    sem_seize(&messid_sem);
    
    messid = next_mess_id++;
    t_sprintf(buf, "%ld\n", next_mess_id);
    lseek(messid_f, 0, SEEK_SET);		/* rewrite from start */
    if (write(messid_f, buf, strlen(buf)) < 0 || fsync(messid_f) < 0) {
	t_perror("next_messid: panic! cannot record new messid");
	abort();			/* can't continue */
    }

    sem_release(&messid_sem);
    
    return messid;
}


/* pick_expire --

    Choose expiration date for message.  Currently this is a constant number
    of months, but we could do things like varying the expiration date based
    on message size, etc.
    
*/

u_long pick_expire(summinfo *summ) {    

    return add_months(dft_expire);		/* current time + proper # of months */
}

/* initialmess --

    Deliver a builtin greeting message (the first time the user signs on, or when they're
    transferred to us.)  The message file must reside on the spool filesystem.  (This means
    that recips on other filesystems don't share copies; it might be worth figuring out
    a different approach someday.)	
*/

void initialmess(mbox *mb, char *username, char *fname) {    
					
    fileinfo	head;			/* initial message: header */
    fileinfo	text;			/* 	''	  : text */
    enclinfo	*ep;			/*      ''	  : encls */
    summinfo	*summ;			/* 	''	  : summary */
    messinfo	mi;			/* 	''	  : messinfo */
    long	mtype;			/*	''	  : type */
    int		i;
    	
    /* get initial message, if one is defined */
    if (fname && mess_open(fname, &head, &text, &ep, &mi.finfo.len, &mtype)) {
	summ = mess_scan_head(&head, &text, ep, mtype); /* construct summary info */
	date_time(summ->date, summ->time);	/* use current date & time in summary */
	summ->type = mtype;			/* mess type from file header */

	/* set up messinfo describing it */
	mi.messid = summ->messid;
	for (i = 0; i < m_filesys_count; ++i)
	    mi.present[i] = FALSE;		/* haven't saved copy anywhere yet */
	if (m_spool_filesys != -1)		/* if spool filesys has mailboxes */
	    mi.present[m_spool_filesys] = TRUE; /* don't need to copy */
	strcpy(mi.finfo.fname, fname);
	mi.finfo.offset = 0;			/* (length set above) */
	mi.finfo.temp = FALSE;			/* don't unlink */
	
	/* deliver it to user's local box */
	(void) localdeliver_one("Postmaster", username, mb->uid, mb->fs, &mi, 
			  &head, &text, summ);
	
	mess_done(&mi);				/* remove mess_deliver temps */	
	clean_encl_list(&ep);			/* clean up enclosure list */
	t_free(summ);				/* and summary info */
	/* (don't need finfoclose since not using temp files) */
	
    }
}

