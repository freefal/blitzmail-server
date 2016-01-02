/* BlitzMail Server -- mailboxes
	
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/mbox.c,v 3.6 98/10/21 16:07:54 davidg Exp Locker: davidg $
    
    Mailbox utilities.
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <string.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <netinet/in.h>
#include "t_io.h"
#include "config.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "client.h"
#include "mess.h"
#include "ddp.h"
#include "queue.h"

void do_mkdir(char *s1, char *s2);
static void mbox_setup_folders(mbox *mb);
static boolean_t uid_to_boxname(mbox *mb);
static any_t expirefs(any_t _fs);

/* mbox_alloc --
    
    Allocate and initialize mailbox structure, enter it into hash table.
    Note that all fields need to be initialized to innocuous values before
    releasing mbox_sem.
    
    --> mbox_sem seized <--

*/
mbox *mbox_alloc(long uid, int fs) {
    
    mbox 	*mb;			/* returned: mailbox structure */
    int		hash;
    
    mb = (mbox *) mallocf(sizeof(mbox));
    pthread_mutex_lock(&global_lock);
    ++malloc_stats.mbox;
    pthread_mutex_unlock(&global_lock);

	    
    /* fill in the structure */
    mb->attach = 1;			/* starts out with one thread using it */
    sem_init(&mb->mbsem, "mbsem");
    if (uid < 0)			/* negative uid's aren't real */
    	return mb;

    mb->uid = uid; 
    mb->fs = fs;			/* note that if fs < 0 ... */
    strcpy(mb->boxname, "");		/* ...uid_to_boxname will generate one */
    mb->obuf.first = NULL;
    mb->obuf.last = NULL;
    mb->user = NULL;
    mb->xfering = FALSE;
    mb->gone = FALSE;
    mb->checked = FALSE;		/* summary check not yet done */
    mb->fold = NULL;
    mb->foldmax = 0;
    mb->prefs = NULL;
    mb->lists = NULL;
    mb->boxlen = 0;
    		    
    hash = MBOX_HASH(uid);
    mb->next = mbox_tab[hash];	/* add to table */
    mb->prev = NULL;
    if (mb->next)
	mb->next->prev = mb;
    mbox_tab[hash] = mb;
    
    ++mbox_count;			/* one more entry in table */
    
    return mb;
}

/*^L mbox_free --

    Unlink mbox from hash table & free it.

    --> mbox_sem seized <--

*/

void mbox_free(mbox *mb) {

    int	foldnum;


    if (mb->next)             /* relink list without us */
	mb->next->prev = mb->prev;
    if (mb->prev)
	mb->prev->next = mb->next;
    else
	mbox_tab[MBOX_HASH(mb->uid)] = mb->next; /* new head */
    

    if (mb->prefs)
    	pref_free(mb);  /* free pref hash table */

    if (mb->lists)
        ml_free(mb);    /* free mailing list hash table */

    /* free summaries for each folder */
    for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {
    	if (mb->fold[foldnum].num < 0)
            continue;
        if (!mb->fold[foldnum].dirty) {
            summ_free(mb, &mb->fold[foldnum]);
        }
    }

    t_free(mb->fold);
    sem_destroy(&mb->mbsem);
    t_free(mb);

    pthread_mutex_lock(&global_lock);
    --malloc_stats.mbox;
    pthread_mutex_unlock(&global_lock);

}

/* mbox_find --
    
    Locate/create mailbox block for given uid.  Increment its attachment count,
    and return a pointer to it	
    
*/

mbox *mbox_find(long uid, int fs, boolean_t no_record) {

    mbox 	*mb;			/* returned: mailbox structure */
    int		hash;
    boolean_t	chosen;			/* new fs choice made? */

    hash = MBOX_HASH(uid);		/* hash on low bits of uid */
    	
    sem_seize(&mbox_sem[hash]);		/* search hash table for box */
    for (mb = mbox_tab[hash]; mb != NULL; mb = mb->next) {
    	if (mb->uid == uid)
	    break;
    }
    
    if (mb == NULL)			/* if not in table allocate & add it */
	mb = mbox_alloc(uid, fs);
    else
	++mb->attach;			/* glomming on to existing entry */

    sem_release(&mbox_sem[hash]);	/* allow other mbox_finds to proceed... */
    
    sem_seize(&mb->mbsem);		/* ...while we work on this box */

    if (strlen(mb->boxname) == 0) {	
	chosen = uid_to_boxname(mb);	/* generate pathname, first time */
	if (chosen && !no_record) {	/* unless we're waiting... */
	    record_fs(mb);		/* record fs choice in DND entry */	
	}
    }
    
    if (!mb->checked) {			/* unless they're known to be valid */
	mbox_setup_folders(mb);		/* locate & set up folders */
	summ_check(mb);			/* check out summaries */
    }

    mb->idle = 0;			/* reset idle count to 0 upon attach */

    sem_release(&mb->mbsem);

    return mb;
}

/* mbox_done --
    
    Release attachment to mailbox.  Each call to mbox_find should eventually
    be matched by an mbox_done.  Note that the attachment count is protected
    by mbox_sem.  The caller should consider mbox_done to be essentially
    equivalent to a call to "free" -- after the call, its reference to the
    box is no longer valid.  To emphasize this point, the caller's pointer
    is NULL'd.  
    
    NOTE: If the caller screwed up and left mb->mbsem seized, deadlock can
    result (because semaphore ordering will be violated when we seize mbox_sem).
    
    If attachment count is zero and the box has been written out, the idle
    task can free it & remove it from the table.
*/

void mbox_done(mbox **mb) {

    int		hash;

    hash = MBOX_HASH((*mb)->uid);
    
    sem_seize(&mbox_sem[hash]);	/* sync w/ mbox_find */
    
    if (--(*mb)->attach < 0) {
	t_errprint_l("Negative attach count box %d!", (*mb)->uid);
	abortsig();
    }	
    
    sem_release(&mbox_sem[hash]);
    
    /* no explicit free here */

    *mb = NULL;				/* sppml */
}

/* mbox_init --
    
    One-time initialization of mailbox routines.

    This must be done _before_ multiple threads are trying to access boxes,
    since this is where mutex's are initialized.
*/


void mbox_init() {
    
    int		i;
    int		j;
    char	subdir[FILENAME_MAX];

    /* create top-level directories on each filesystem, if they don't
	exist yet */
	
    for (i = 0; i < m_filesys_count; ++i) {
	do_mkdir(m_filesys[i], MESSTMP_DIR);
	do_mkdir(m_filesys[i], BOX_DIR);
	do_mkdir(m_filesys[i], MESSXFER_DIR);
	for (j = 0; j < 100; ++j) {
	    t_sprintf(subdir, "%s%d/", MESSXFER_DIR, j);
	    do_mkdir(m_filesys[i], subdir);
	}
	if (i == m_spool_filesys)
	    do_mkdir(m_filesys[i], SPOOL_DIR);
    }
    	
    /* initialize hash table of active boxes */
    for (i = 0; i < MBOX_HASHMAX; i++) {
	mbox_tab[i] = NULL;
	sem_init(&mbox_sem[i], "mbox_sem");	/* semaphores protecting mbox table */
    }
    mbox_count = 0;
        
    /* set up expiration globals */
    pthread_mutex_init(&exp.lock, pthread_mutexattr_default);
    pthread_cond_init(&exp.wait, pthread_condattr_default);


}
/* mbox_setup_folders --

    Called as part of mailbox initialization to locate all folders, set up
    mb->fold and mb->foldcount.  The "count" and "dirty" fields of each folder
    are set up by summ_check.
    
    --> box locked <--
*/

static void mbox_setup_folders(mbox *mb) {

    DIR			*boxdir;		/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    char		*namep;			/* pointer into folder name */
    long		foldnum;		/* current folder # */

    sem_check(&mb->mbsem);
	
    mb->foldmax = DEFAULT_FOLDCOUNT;
    mb->fold = mallocf(mb->foldmax * sizeof(folder));	/* room for default folders */
    
    /* set up default folders */
    strcpy(mb->fold[TRASH_NUM].name, TRASH_NAME);
    mb->fold[TRASH_NUM].num = TRASH_NUM;
    mb->fold[TRASH_NUM].summs = NULL;
    strcpy(mb->fold[AUDIT_NUM].name, AUDIT_NAME);
    mb->fold[AUDIT_NUM].num = AUDIT_NUM;
    mb->fold[AUDIT_NUM].summs = NULL;
    strcpy(mb->fold[INBOX_NUM].name, INBOX_NAME);
    mb->fold[INBOX_NUM].num = INBOX_NUM;
    mb->fold[INBOX_NUM].summs = NULL;


    /* now, read box directory to locate any user-defined folders
       (they look like "Fold<n>.<name>") */
           
    /* open mailbox directory */
    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    boxdir = opendir(mb->boxname);
    pthread_mutex_unlock(&dir_lock);
    
    if (boxdir == NULL) {
	t_perror1("mbox_setup_folders: cannot open ", mb->boxname);
    } else {   
	while ((dirp = readdir(boxdir)) != NULL) {	/* read entire directory */
		
	    if (strncmp(dirp->d_name, "Fold", 4) != 0)
		continue;			/* ignore all but folders */
		
	    namep = strtonum(dirp->d_name+4, &foldnum);
	    if (*namep++ != '.' || strlen(namep) == 0)	/* ignore bad name */
		continue;
	    if (foldnum < DEFAULT_FOLDCOUNT)
	    	continue;			/* ignore invalid folder # */
		
	    if (foldnum >= mb->foldmax) {	/* need to grow folder array? */
		mb->fold = reallocf(mb->fold, (foldnum+1) * sizeof(folder));
		/* initialize any holes we just introduced */
		for (; foldnum >= mb->foldmax; ++mb->foldmax) {
		    bzero((char *) &mb->fold[mb->foldmax], sizeof(folder));
		    mb->fold[mb->foldmax].num = -1; /* mark new entries invalid */
		}
	    }
	    /* ok, fill in the folder entry */
	    unescname(namep, mb->fold[foldnum].name);
	    mb->fold[foldnum].num = foldnum;
	    mb->fold[foldnum].summs = NULL;
	}
	closedir(boxdir);
    }
   
}

/* do_mkdir --
    
    Create directory if it doesn't yet exist.  Name comes in 2 parts.
*/


void do_mkdir(char *s1, char *s2) {

    char	name[FILENAME_MAX];

    t_sprintf(name,"%s%s", s1, s2);
    name[strlen(name) - 1] = 0;		/* chop trailing '/' */
    
    if (mkdir(name, DIR_ACC) < 0) {
	if (pthread_errno() != EEXIST)
	    t_perror1("do_mkdir: cannot create ", name);    
    }
}

/* uid_to_boxname --
    
    Generate pathname of user's mailbox directory.  Create it, if it doesn't
    already exist.  Set up mb->boxname.  If mb->fs is negative (i.e. this
    user hasn't been assigned to a filesystem yet), pick a value for it
    and change the user's DND entry accordingly.

    --> box locked <--

*/

boolean_t uid_to_boxname(mbox *mb) {

    boolean_t	chosen = FALSE;	/* assume we won't choose new fs */
    
    sem_check(&mb->mbsem);

    if (mb->fs < 0) {		/* choose filesystem  */
    	mb->fs = choose_fs();
	chosen = TRUE;
    }
	    
    t_sprintf(mb->boxname, "%s%s%ld", m_filesys[mb->fs], BOX_DIR, mb->uid);
    /* e.g., "/blitz1/box/13096" */
    
    /* create mailbox directory  */
    if (mkdir(mb->boxname, DIR_ACC) < 0) {
	if (pthread_errno() != EEXIST)
	    t_perror1("uid_to_boxname: cannot create ", mb->boxname);
    }
    
    /* "mess" directory is created by mess_deliver */
    
    /* "mlist" directory is created by ml_readhash */
    
    /* summary files are created by summ_write */
    
    return chosen;
}    

/* choose_fs --

    Choose a filesystem to hold a new mailbox directory.
    We choose the filesystem with the most free space available.
    
*/

int choose_fs() {

    long		avail;		/* in units of 128 bytes */
    int			best = -1;	/* emptyest disk so far */
    long		bestavail = 0;	/* how empty it is */
    struct statfs	buf;
    int			i;
    
    for (i = 0; i < m_filesys_count; ++i) {
	if (STATFS(m_filesys[i], &buf, sizeof(buf)) < 0)
	    t_perror1("choose_fs: cannot statfs ", m_filesys[i]);
	else {				
	    /* normalize block size to make sure we don't run
	       out of bits for very large disks */
	    avail = buf.f_bavail * (buf.f_bsize / 128);
	    if (avail > bestavail) {
		bestavail = avail;
		best = i;
	    }
	}
    }
    
    if (best == -1) {
    	t_errprint("choose_fs: all disks full!");
	best = (int) (random() % m_filesys_count);
    }
    return best;
}

/* record_fs --

    After choosing which filesystem a new user is to be placed on,
    change the BLITZINFO field of the DND entry to record the choice.
    If DND is unavailable, spin until it's available again -- the
    mailbox must not be used until the DND update goes through.
    
    --> box locked <--
*/

void record_fs(mbox *mb) {

    t_file	*dnd = NULL;		/* our private DND connection */
    int		i;
    int		dndstat;
    char	*servname;
    char	*fsname;
    char	*uidname;	/* #uid */
    static char	*farray[] = { "BLITZSERV", "BLITZINFO", NULL };
    char 	*varray[3];
    char	logbuf[128];

    sem_check(&mb->mbsem);

    /* construct ascii strings for DND change command */
    servname = mallocf(MAX_STR);
    if (m_noappletalk)		/* if no AppleTalk at this site, just "host" */
	t_sprintf(servname, "\"%s\"", m_fullservname);
    else			/* "host"@"zone" */
	t_sprintf(servname, "\"%s@%s\"", m_fullservname, my_atzone);
    varray[0] = servname;
    fsname = mallocf(MAX_STR);
    t_sprintf(fsname, "\"%s\"", m_filesys[mb->fs]);
    varray[1] = fsname;
    varray[2] = NULL;
    uidname = mallocf(MAX_STR);
    t_sprintf(uidname, "#%ld", mb->uid);
    
    t_sprintf(logbuf, "Assigning uid %ld to disk %s...", mb->uid, m_filesys[mb->fs]);
    log_it(logbuf);
    
    for (i = 0 ;; ++i) {		/* keep retrying until we get through */
	if (i) {			/* if retrying, spin slowly */
	    if (dnd) {			/* reconnect next time */
		t_dndclose(dnd);
		dnd = NULL;
	    }
	    sleep(60);			
	}
	if ((dnd = t_dndfind()) == NULL) /* connect to dnd server */
	    continue;			
	
	/* enable privileges to allow us to change arbitrary user entry */
	dndstat = t_dndpriv(dnd, priv_name, priv_pw);
	if (dndstat != DND_OK) {
	    if (dndstat == DND_DOWN) {	/* may just be a timed-out connection */
		t_dndclosepool();	/* close any other idle connections saved */
		if (i > 0)		/* don't log first time */
		    t_errprint("record_fs: DND is down; retrying.");
	    } else
		t_errprint_l("record_fs: dnd privileges rejected: %ld", dndstat);
	    continue;
	}
	
	dndstat = t_dnd_privchange(dnd, uidname, farray, varray);
	if (dndstat != DND_OK) {
	    t_errprint_ll("record_fs: can't change dnd entry for uid %ld: %ld",
	    			 mb->uid, dndstat);
	    continue;	
	}
	break;
    }
    
    t_dndclose(dnd);			/* don't keep priv'd connection around */
    t_free(servname);
    t_free(fsname);
    t_free(uidname);
}

/* mbox_size --
  
    Compute total length of all messages in box.  Read message directory,
    stat each file in turn. 
*/

long mbox_size(mbox *mb) {

    char		fname[MBOX_NAMELEN];	/* name of mess dir */
    DIR			*dirf;			/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    struct stat		*statbuf;		/* stat(2) info */
    long		size = 0;		/* returned: total size */

    sem_seize(&mb->mbsem);			/* get access to box */

    statbuf = mallocf(sizeof(struct stat));
    
    /* open mess directory */
    strcpy(fname, mb->boxname); strcat(fname, MESS_DIR);
    
    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    dirf = opendir(fname);
    pthread_mutex_unlock(&dir_lock);

    if (dirf == NULL) {
	if (pthread_errno() != ENOENT)	/* if no messages yet, dir may not exist */
	    t_perror1("mbox_size: cannot open ", fname);
    } else { 
    
	while ((dirp = readdir(dirf)) != NULL) {	/* read entire directory */
	    
	    /* skip dot-files */
	    if (dirp->d_name[0] != '.') {
		/* construct full pathname of message file */
		t_sprintf(fname, "%s%s/%s", mb->boxname, MESS_DIR, dirp->d_name);
		if (stat(fname, statbuf) != 0) /* find out about it */
		    t_perror1("mbox_size: cannot stat ", fname);
		else
		    size += statbuf->st_size;
	    }
	}
	closedir(dirf);
    }

    t_free(statbuf);
    
    sem_release(&mb->mbsem);			/* done with box */
    
    return size;
}
/* open_vacation --
  
    Check vacation preference; if set, try to open vacation text (in user's
    mailbox directory) and set up fileinfo for it. 
*/

boolean_t open_vacation(mbox *mb, fileinfo *finfo) {

    char	zot[MAX_STR];
    t_file	*f;
    
    if (!pref_get(mb, PREF_VACATION, zot))
	return FALSE;			/* pref not set; no vacation message */
	
    /* constuct filename */
    strcpy(finfo->fname, mb->boxname);
    strcat(finfo->fname, VACATION_FNAME);	
    
    if ((f = t_fopen(finfo->fname, O_RDONLY, 0)) == NULL)
	return FALSE;			/* file isn't there */

    finfo->offset = 0;			/* starts at beginning */
    finfo->len = lseek(f->fd, 0, SEEK_END); /* is this long */
    finfo->temp = FALSE;		/* not a temp file */
    
    t_fclose(f);
    
    return TRUE;
}

/* mbox_writer --
  
    Thread to write out mailbox changes periodically.  Sleep for
    a while, then check for modified mailboxes (folders or summaries);
    write any we find.
    
    Unmodified mbox structures whose attach count is zero may be
    freed, but we generally do NOT do so.
    This is because of the summary consistency code:  the first time
    the box is opened a full consistency check is done; from then on
    it's known to be up to date (since the in-memory copy is still there).
    The only time we actually free the entire mbox structure is if the
    "gone" flag is set indicating that the mailbox files have been removed
    from disk (account invalidated or transferred to another server).
    
    Although the mbox structure itself stays around forever, we don't want
    to spend the memory to keep the all the subsidiary tables around too:
    if the box hasn't been used recently, free them up.  
    
    If we're due to expire messages (we haven't done it yet today), fire
    off a thread to do that.    
*/

any_t mbox_writer(any_t zot) {

    char	buf[MAX_STR];
    u_long	today;			/* today (mactime format) */
    u_long	lastexp = 0;		/* mactime of day last exp check done */
    t_file	*f;			/* expdate file */
    int		i;
    
    /* read file to determine when last expiration was done */
    
    if (f = t_fopen(f_expdate, O_RDONLY, 0)) {
	if (t_gets(buf, sizeof(buf), f))
	    strtouns(buf, &lastexp);
	t_fclose(f);
    }
        
    for (;;) {
						 
	sleep(60);			/* once a minute */

	/* if it's time for today's expiration, start the thread to
	   do that.  If the current time is more than a few days greater
	   than the last time we checked, DON'T expire anything (if the
	   clock gets set into the far future somehow, we don't want to
	   throw everything away). */
	if ((today = add_days(0)) != lastexp 	/* not expired yet today? */
	    && mactime() - today > exp_time) { /* and it's right time of day */
	    if (lastexp > 0 && (today - lastexp > 7*24*60*60)) {
		t_errprint("More than 7 days since last expiration?!");	 
		t_errprint("Automatic expiration check disabled.");
		t_errprint("CHECK SYSTEM DATE/TIME!");   
		lastexp = today;		/* don't keep repeating message */
	    } else {
		pthread_t thread;		/* thread var */

		if (pthread_create(&thread, generic_attr,
				(pthread_startroutine_t) expire, (pthread_addr_t) 0) < 0) {
		    t_perror("mbox_writer: pthread_create");
		} else {
		    lastexp = today;		/* don't do it more than once */
		    pthread_detach(&thread);
		}
	    }
	}

	/* nudge thread to time out idle connections */
	pthread_cond_signal(&timeout_wait);
	
	/* wake all queue routines, so they will time out unused connections */
	for (i = -1; i < m_servcount; ++i) {
	    pthread_cond_signal(&q_wait[i]);
	}
	
	mbox_dowrite(TRUE);			/* write all dirty boxes */
    }
}

/* mbox_dowrite --

    Write out all dirty boxes.  Called from above, plus anywhere else where
    we need to flush changes (e.g., shutdown).

*/

void mbox_dowrite(boolean_t shouldfree) {

    int		hash;			/* which hash table list */
    mbox	*mb,*nextmb;		/* one box, and the next */
    mbox	**boxlist;		/* list of boxes to write */
    int		boxcount;		/* how many */
    int		foldnum;		/* current folder */
    long	delay;			/* time consumed */

#define MB_IDLEFREE	5		/* free box structs that are unused this long */

    /* lock each row of hash table while we traverse it constructing a consistent
       list of all boxes.  We _don't_ want to lock the individual boxes
       while mbox_sem is seized because that introduces a bottleneck
       (one busy box could cause mbox_sem to be tied up).  Note that
       it's safe to look at the box without locking if attach == 0. */
        
    boxcount = 0;			/* none found yet */
    boxlist = mallocf((mbox_count + 1) * sizeof(mbox *));
        
    for (hash = 0; hash < MBOX_HASHMAX; ++hash) {

	delay = time(NULL);
	sem_seize(&mbox_sem[hash]);	/* lock hash table row */
	
	for (mb = mbox_tab[hash]; mb != NULL; mb = nextmb) {
	
            nextmb = mb->next;		/* remember link in case of free */

	    ++mb->attach;		/* box may not be freed */
	    
	    /* if we're only user of box, free structures if requested
	       (never free public mailing list data, we'll need it soon anyway) */
	    if (shouldfree && mb->attach == 1 && mb->uid != pubml_uid) {
		if (++mb->idle >= MB_IDLEFREE) {	/* not recently used; free */
		    mb->idle = 0;	/* reset for next time */
		    if (mb->prefs && !mb->prefs->dirty)	
			pref_free(mb);	/* free pref hash table */
			
		    if (mb->lists)	
			ml_free(mb); 	/* free mailing list hash table */
			
		    /* free summaries for each folder */
		    for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {
			if (mb->fold[foldnum].num < 0)
			    continue;
			if (!mb->fold[foldnum].dirty) 
			    summ_free(mb, &mb->fold[foldnum]);
		    }
                    if (mb->gone && !mb->xfering) {	/* vanished box; discard */
                        mbox_free(mb);
                        mb = NULL;
                    }
		}
	    }
	    if (mb) 
	        boxlist[boxcount++] = mb;	/* construct list of all boxes */
	}
	sem_release(&mbox_sem[hash]);	
	delay = time(NULL) - delay;
	if (delay >= 5) {
	    char logbuf[128];
	    t_sprintf(logbuf, "mbox_dowrite: mbox_sem[%d] locked for %ld seconds.", hash, delay);
	    log_it(logbuf);
	}

    }
    /* list of boxes has been constructed.  note that we locked 
       only one row of the hash table at a time, and don't have
       it locked at all while we write dirty ones out
       (so we don't hold things up) */    
    
    while(--boxcount >= 0) {		/* for each box located above */
	mb = boxlist[boxcount];
	
	sem_seize(&mb->mbsem);
	if (!mb->gone) {		/* if box is being moved, don't write to it */
	    if (mb->prefs && mb->prefs->dirty) /* write out preferences */
		    pref_write(mb);
    
	    /* write out any modified folder summaries */
	    for (foldnum = 0; foldnum < mb->foldmax; ++foldnum) {
		if (mb->fold[foldnum].num < 0)
		    continue;	    
		if (mb->fold[foldnum].dirty)
		    summ_write(mb, &mb->fold[foldnum]);
	    }
	}
	sem_release(&mb->mbsem);
	
	mbox_done(&mb);		/* release attachment */
    }	
    
    t_free(boxlist);	        
}

/* expire --
  
    Thread to do expiration check.
    
    For each filesystem, read the box directory to locate all mailboxes.
    For each box, scan all folders for any messages whose expiration
    date has arrived.  Record the uid & summary of the doomed message in
    the f_explog file and delete the message.
    
    When done, the date we _began_ the check is written to the f_expdate
    file.  (So we know not to bother to check again for another day.)
*/

any_t expire(any_t zot) {

    t_file		*expdate;		/* recorded expiration date */
    int			fs;			/* current filesystem */
    char		logbuf[MAX_STR];
    pthread_t		thread;
    
    setup_signals();

    pthread_mutex_lock(&exp.lock);
    if (exp.expiring) {
	t_errprint("Expiration check already in progress; not starting another.");
	pthread_mutex_unlock(&exp.lock);
	return 0;
    }
    log_it("Beginning expiration check");
    t_errprint("Beginning expiration check");
    
    exp.expiring = TRUE;
    exp.threads = 0;
    exp.total = 0;
    exp.stolog = exp.explog = NULL;
    exp.today = add_days(0);
    
    /* get log file */
    if ((exp.explog = t_fopen(f_explog, O_APPEND | O_CREAT | O_WRONLY, FILE_ACC)) == NULL) {
	t_perror1("Expire: cannot open ", f_explog);
	goto done;
    }
    
    /* optional file to record everyone's storage usage */
    if (f_stolog) {
	if ((exp.stolog = t_fopen(f_stolog, O_TRUNC | O_CREAT | O_WRONLY, FILE_ACC)) == NULL)
	    t_perror1("Expire: cannot open ", f_stolog);
    }


    for (fs = 0; fs < m_filesys_count; ++fs) { /* start thread for each filesystem */
	if (pthread_create(&thread, generic_attr,
			(pthread_startroutine_t) expirefs, (pthread_addr_t) fs) < 0) {
	    t_perror("expire: pthread_create");
	} else {
	    pthread_detach(&thread);
	    ++exp.threads;			/* count active threads */
	}
    }
    
    while (exp.threads > 0) {		/* wait for everyone to finish */
    	pthread_cond_wait(&exp.wait, &exp.lock);
    }

    t_fclose(exp.explog);
    if (exp.stolog)
	t_fclose(exp.stolog);
    
    if ((expdate = t_fopen(f_expdate, O_TRUNC | O_CREAT | O_WRONLY, FILE_ACC)) == NULL) {
	t_perror1("Expire: cannot open ", f_expdate);
	goto done;
    }
    
    t_fprintf(expdate, "%lu\n", exp.today);	/* record exp date */
    t_fclose(expdate);
    	
    t_sprintf(logbuf, "Expiration completed; %ld expired messages removed.", exp.total);
    log_it(logbuf);
    t_errprint(logbuf);
    	
done:
    exp.expiring = FALSE;
    pthread_mutex_unlock(&exp.lock);
    return 0;
}

/* expirefs --
  
    Thread to do expiration check on one filesystem.
    
    For this filesystem, read the box directory to locate all mailboxes.
    For each box, scan all folders for any messages whose expiration
    date has arrived.  Record the uid & summary of the doomed message in
    the f_explog file and delete the message.
*/

static any_t expirefs(any_t _fs) {

    int			fs;			/* our filesystem */
    char		fname[MBOX_NAMELEN];	/* name of box dir on that fs */
    DIR			*boxdir;		/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    long		uid;			/* one box */
    char		*end;			/* end of uid str */

    fs = (int) _fs;				/* pick up our arg */
    
    setup_signals();

    /* open box directory */

    t_sprintf(fname, "%s%s", m_filesys[fs], BOX_DIR);
    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    boxdir = opendir(fname);
    pthread_mutex_unlock(&dir_lock);
    
    if (boxdir == NULL) {
	t_perror1("expire: cannot open ", fname);
	goto done;
    } 

    while ((dirp = readdir(boxdir)) != NULL) {	/* read entire directory */
	
	/* skip dot-files */
	if (dirp->d_name[0] != '.') {
	    end = strtonum(dirp->d_name, &uid);
	    if (*end == 0)	/* ignore non-numeric filenames */
		expire1(uid, fs, exp.today);	
	}
    }

done:
    if (boxdir) closedir(boxdir);

    pthread_mutex_lock(&exp.lock);
    --exp.threads;			/* this thread is done */
    pthread_cond_signal(&exp.wait);	/* wake up anyone waiting for that */
    pthread_mutex_unlock(&exp.lock);

    return 0;
}
