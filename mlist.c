/*  BlitzMail Server -- Mailing lists
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/mlist.c,v 3.6 98/10/21 16:08:58 davidg Exp Locker: davidg $
    
    Personal mailing lists are stored in the "mlist" subdirectory of the user's
    mailbox directory; one file per list.  The list name is used as the filename;
    mac special chars not legal in filenames are given in \nnn notation.
    
    List names are case-insensitive (so the hash and string compare functions
    used ignore case), but we want to remember the (possibly) mixed-case name
    of the list, so the list filenames are in mixed-case.  When trying to open
    a list, use the hash table to see if it exists and to determine the exact
    (mixed-case) filename.
    
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <fcntl.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "client.h"
#include "config.h"

/* prototypes for private funcs */

static boolean_t ml_exists(mbox *mb, char *name, char *fname, char *casename);
static u_int ml_hash(char *name);
static void ml_hashadd(mbox *mb, char *name);
static boolean_t ml_hashrem(mbox *mb, char *name);

/* ml_clean --

    Free a ml_data list (and set ptr NULL).
*/

void ml_clean(ml_data **ml) {

    ml_data	*temp;
    
    while (*ml) {
	temp = *ml;
	*ml = (*ml)->next;
	t_free(temp);		
    }

}
/* ml_get --
    
    Retrieve contents of mailing list (as a list of ml_data blocks).
*/

boolean_t ml_get(mbox *mb, char *name, ml_data **result, char *casename) {

    char 	path[FILENAME_MAX]; 	/* including directory name */
    t_file	*f = NULL;
    struct stat statbuf;
    
    sem_seize(&mb->mbsem);
    
    *result = NULL;
    
    if (!ml_exists(mb, name, path, casename)) {	/* check hash table */
	sem_release(&mb->mbsem);
	return FALSE;			/* not found */
    }
    if ((f = t_fopen(path, O_RDONLY, 0)) == NULL) {
	t_perror1("ml_get: open failed: ", path);
	sem_release(&mb->mbsem);		
	return FALSE;
    }
    
    /* Get file length, allocate a block to hold it, and read it in */
    if (fstat(f->fd, &statbuf) != 0) {
	t_perror1("ml_get: fstat failed: ", path);
    } else {
	*result = (ml_data *) mallocf(statbuf.st_size + 1 + sizeof(ml_data));
	if (read(f->fd, (char *) (*result)->data, statbuf.st_size) < 0) {
	    t_perror1("ml_get: read failed: ", path);
	    t_free(*result);
	    *result = NULL;
	}
	(*result)->next = NULL;
	
	(*result)->maxlen = statbuf.st_size + 1;
	(*result)->len = statbuf.st_size;
	
	/* add terminating \n, if necessary */
	if ((*result)->data[(*result)->len-1]  != '\n') {
	    (*result)->data[(*result)->len++] = '\n';
	}	 
    }

    sem_release(&mb->mbsem);
    
    (void) t_fclose(f);
    return (*result != NULL);
}

/* ml_rem --
    
    Remove the named list, returns false if the list doesn't exist.
*/

boolean_t ml_rem(mbox *mb, char *name) {

    char	mlf[FILENAME_MAX];	/* result file */
    
    sem_seize(&mb->mbsem);
    
    if (!ml_exists(mb, name, mlf, NULL)) {
	sem_release(&mb->mbsem);	
	return FALSE;
    }
    
    if (unlink(mlf) < 0) {
	t_perror1("ml_rem: unlink failed: ", mlf);
	sem_release(&mb->mbsem);	
	return FALSE;
    }
    
    /* remove entry from hash table */
    if (!ml_hashrem(mb, name))
	t_perror1("ml_rem:  not in table: ", mlf);

    sem_release(&mb->mbsem);
    
    return TRUE;
}

/* ml_set --
    
    Create/update mailing list.  Write data to a temporary file, then rename it.
    Add list name to in-memory hash list, if necessary.
*/

void ml_set(mbox *mb, char *name, ml_data *contents) {

    char 	tempf[FILENAME_MAX]; 	/* temp file we create */
    char	mlf[FILENAME_MAX];	/* result file */
    ml_data 	*p;			/* current block of data */
    t_file	*f = NULL;		/* temp file */
    boolean_t	newlist;

    sem_seize(&mb->mbsem);
    
    strcpy(mlf, mb->boxname); 		/* construct full pathname of result file */
    strcat(mlf, ML_DIR);		
    escname(name, mlf+strlen(mlf)); 	/* escape any special chars in name */
	    
    /* If we replace an existing list and the new name differs in case
       from the old, the hash table entry needs to be altered. */
	
    newlist = !ml_exists(mb, name, tempf, NULL);
    if (!newlist) {
	if (strcmp(tempf, mlf) != 0) {	/* case sensitive compare this time! */
	    (void) ml_hashrem(mb, name);/* delete old entry */
	    ml_hashadd(mb, name);	/* reinsert w/ new capitalization */
	    if (rename(tempf, mlf) < 0) { /* change case, so there's always just 1 */
		t_perror1("ml_set: case rename failed: ", mlf);
	    }
	}
    } 

    /* note that dot-files are ignored by ml_readhash */
	
    strcpy(tempf, mb->boxname); strcat(tempf, ML_DIR);
    strcat(tempf, ".mltemp");
    
    if ((f = t_fopen(tempf, O_WRONLY | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	    t_perror1("ml_set: open failed: ", tempf);
	    sem_release(&mb->mbsem);		
	    return;
    }
    
    /* write each block of list data */
    for (p = contents; p ; p = p->next) {
	if (write(f->fd, p->data, p->len) < 0) {
	    t_perror1("ml_set: open failed: ", tempf);
	    (void) t_fclose(f);
	    sem_release(&mb->mbsem);					
	    return;
	}
    }	
    
    (void) t_fclose(f);
	    
    if (rename(tempf,mlf) < 0) {
	t_perror1("ml_set: rename failed: ", mlf);
	sem_release(&mb->mbsem);		
	return; 
    }
    
    if (newlist)
	ml_hashadd(mb, name);	/* new list; add to table */

    sem_release(&mb->mbsem);		
	    
}

/* ml_summary --
    
    Generate list of all mailing list names in this box.  Note that this is
    the only mailing list routine that assumes the presence of a client
    connection.  
    
    Sort the list before sending it:  allocate an array of pointers, traverse
    the hash table locating all the entries, then sort the table.
    
*/

void ml_summary(mbox *usermb, boolean_t global) {

    mbox		*mb;		/* box with lists */
    ml_namep		*names;		/* for sort */
    ml_namep		np;
    int			count = 0;
    int			i, j, stride;
    int			strides[20];
    int			t;		/* number of passes */
    char		buf[MAX_STR];
    char		*p;

#define STRIDE_FACTOR 3 		/* Shell sort stride factor */
    
    if (global)				/* is it global mailbox we want? */
	mb = mbox_find(pubml_uid, pubml_fs, FALSE);
    else
	mb = usermb;			/* no - personal lists */
	
    sem_seize(&mb->mbsem); 

    buf_init(usermb);			/* buffer output until box unlocked */
 
    if (!mb->lists)			/* if no hash table, get it */
	ml_readhash(mb);
   
    if (mb->lists->count == 0) {	/* none there? */
	buf_putsta(usermb, BLITZ_NODATA); /* avoid bother & 0-length malloc */
	buf_putl(usermb, "No mailing lists defined.");
    } else {

	names = (ml_namep*) mallocf(mb->lists->count * sizeof(ml_namep*));
	
	/* search hash table for all entries */
	for (i = 0; i < ML_HASHMAX; i++) {
	    for (np = mb->lists->hashtab[i]; np; np = np->next) 
		names[count++] = np;
	}
	
	if (count != mb->lists->count)
	    t_errprint_l("Inconsistent mlist hash table uid %ld!", mb->uid);
    
	/* Shell sort (diminishing increment insertion sort) */
	/* increments are actually static... */
	stride = 1; t = 0;
	for (;;) {			/* stride(t+1) = 3*stride(t) + 1 */
	    strides[t] = stride;
	    stride = stride*STRIDE_FACTOR + 1;
	    if (stride >= count)
		break;
	    t++;
	}
	if (t) {			/* stop when stride(t+2) >= n (Knuth) */
	    t -= t > 1 ? 2 : 1;	/* but make at least 1 pass! */
	}
	
	while(t >= 0) {		/* until stride = 1 */
	    for (j = strides[t]; j < count; j++) { /* make a pass */
		np = names[j];	/* current element */
		/* insertion sort on elements "strides[t]" apart */
		for (i = j - strides[t]; i >= 0; i -= strides[t]) {
		    if (strcasecmp(np->name, names[i]->name) >= 0 )
			break;	/* new element belongs after i */
		    names[i+strides[t]] = names[i]; /* slide out of the way */
		}
		names[i+strides[t]] = np;
	    }
	    t -= 1;			/* next time, smaller stride */
	}
			    
	for (i = 0; i < count; i++) {
	    if (i == count - 1)
		buf_putsta(usermb, BLITZ_LASTLINE);
	    else 
		buf_putsta(usermb, BLITZ_MOREDATA);	
	    strcpy(buf, names[i]->name);
	    if (global) {		/* add access encoding */
		strcat(buf, ",");
		p = buf + strlen(buf);
		*p++ = pubml_acc_int(usermb, mb, names[i]->name) + '0';
		*p = 0;
	    }
	    buf_putl(usermb, buf);
	}
	
	t_free(names);
	
    }				/* (>0 lists exist) */
    sem_release(&mb->mbsem);
    if(global)
        mbox_done(&mb);
	
    buf_flush(usermb);		/* ok to write to net now */
    		
}

/* ml_exists --
    
    Determine (by checking the hash table) whether a given mailing list
    exists.  If caller requested it, return pathname of the mailing list file
    and/or mixed-case mailing list name.
    
    --> box locked <--
*/

static boolean_t ml_exists(mbox *mb, char *name, char *fname, char *casename) {

    u_int 	hash;			/* hashed name */
    ml_namep	np;			/* current name in table */
    
    if (!mb->lists)			/* if no hash table, get it */
	ml_readhash(mb);
		    
    hash = ml_hash(name);		/* generate hash value */		
    
    for (np = mb->lists->hashtab[hash]; np; np = np->next) { /* run down the list */
	if (strcasecmp(name, np->name) == 0) {
	    if (fname) {			/* if caller wants it */
		strcpy(fname, mb->boxname); /* construct full pathname */
		strcat(fname, ML_DIR);		
		escname(np->name, fname+strlen(fname)); /* escape special chars */
	    }
	    if (casename) {		/* caller wants to know name case */
		strcpy(casename, np->name);
	    }
	    return TRUE;
	}
    }
    return FALSE;
}

/* ml_hash --
    
    Hash name into a small integer in the range 0..ML_HASHMAX-1.  XOR all the chars
    together into a long (rotating left 4 bits each time, to avoid commutativity),
    divide by table size (which should be prime).
    
*/

static u_int ml_hash(char *name) {

    u_bit32 	hash;			/* returned: hash value */
    char 	c;			/* next char of name */
    hash = 0;
    
    while(c = *name++) {
	if (isascii(c) && isupper(c))	/* hash func should be case-insensitive */
	    c = tolower(c);
	hash = ((hash >> 28) & 0xF) | (hash << 4); /* rotate */
	hash ^= c;	/* xor in next byte */
    }
    
    hash = hash % ML_HASHMAX;	/* get within table range */
    
    return hash;

}

/* ml_hashadd --
    
    Add name to hash table.  Note that names in the table are mixed-case,
    without special chars escaped (just as the client sends them).
*/

static void ml_hashadd(mbox *mb, char *name) {

    u_int	hash;			/* hash value */
    ml_namep	np;			/* name entry pointer */
    int		len;			/* length needed */
    
    hash = ml_hash(name);		/* choose slot */
    len = strlen(name) + sizeof(ml_name); /* need this much room */
    np = (ml_namep) mallocf(len);
    
    pthread_mutex_lock(&global_lock);   /* stats: count entries allocated */
    ++malloc_stats.mlentry;
    pthread_mutex_unlock(&global_lock);

    strcpy(np->name, name);		/* copy the name */
    
    np->next = mb->lists->hashtab[hash];	/* add it to front of list */
    mb->lists->hashtab[hash] = np;
    mb->lists->count++;			/* keep count of total # */
}

/* ml_hashrem --
    
    Remove name from hash table.
*/

static  boolean_t ml_hashrem(mbox *mb, char *name) {

    u_int	hash;			/* hash value */
    ml_namep	np, pp;			/* name entry pointers */
  					  
    hash = ml_hash(name);		/* choose slot */
    
    /* search that slot for the name */
    for (pp = NULL, np = mb->lists->hashtab[hash]; np; pp = np, np = np->next) {
	if (strcasecmp(name, np->name) == 0)
	    break;
    }
    
    if (np) {				/* if not found, never mind */
	if (pp)
	    pp->next = np->next;
	else mb->lists->hashtab[hash] = np->next;
	t_free(np);
    	pthread_mutex_lock(&global_lock);   /* stats: count entries allocated */
    	--malloc_stats.mlentry;
   	pthread_mutex_unlock(&global_lock);

	mb->lists->count--;		/* keep score */
	return TRUE;
    } else
    	return FALSE;
}

/* ml_free --
    
    Free storage consumed by mailing list hash table.
    Clear mb->lists.
    
    --> box locked <--
*/

void ml_free(mbox *mb) {

    ml_name 	*p;
    int		i;
    long        freecount = 0;
    
    /* free entire table */
    for (i = 0; i < ML_HASHMAX; i++) {
	while(p = mb->lists->hashtab[i]) {	/* (sic) */
	    mb->lists->hashtab[i] = p->next;
	    t_free(p);
            ++freecount;
	}
    } 
    
    t_free(mb->lists);			/* free the table itself */
    mb->lists = NULL;			/* lists not present */

    pthread_mutex_lock(&global_lock);
    --malloc_stats.mltab;     /* one less table */
    malloc_stats.mlentry -= freecount; /* n fewer entries */
    pthread_mutex_unlock(&global_lock);
}

/* ml_readhash --
    
    Read directory to determine what mailing lists exist, construct hash table.
    Note that we use the un-escaped names in the hash table, thus the escaping
    is done only when actually opening the list, not for every lookup.
    
    --> box locked <--
*/

void ml_readhash(mbox *mb) {

    DIR			*dirf;			/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    int			i;		/* temp */
    char		name[MAX_STR]; /* list name (w/o escaping) */
    char		dir[FILENAME_MAX]; /* list directory pathname */
 
    pthread_mutex_lock(&global_lock);
    ++mb_stats.mlist_r;
    ++malloc_stats.mltab;
    pthread_mutex_unlock(&global_lock);
    
    /* allocate & initialize hash table */
    mb->lists = mallocf(sizeof(ml_tab));
    for (i = 0; i < ML_HASHMAX; i++)
	mb->lists->hashtab[i] = (ml_namep) NULL;
    mb->lists->count = 0;
    
    strcpy(dir, mb->boxname);
    strcat(dir, ML_DIR);
    dir[strlen(dir) - 1] = 0;	/* drop trailing '/' */		

    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    dirf = opendir(dir);
    pthread_mutex_unlock(&dir_lock);
	    
    if (dirf == NULL) {
	if (pthread_errno() == ENOENT) { /* if it doesn't exist yet */
	    if (mkdir(dir, DIR_ACC) < 0) /* try to create */
		t_perror1("mlist_exists: cannot mkdir ", dir);
	    else
		;		/* ok; empty list directory */
	} else			/* other problem opening */
	    t_perror1("Mailing list dir open failed: ", dir);
	return;
    }
        
    while ((dirp = readdir(dirf)) != NULL) {	/* read entire directory */
	    
	if (dirp->d_name[0] != '.') {	/* ignore dot-files */
	    unescname(dirp->d_name, name); /* unescape it */
	    ml_hashadd(mb, name); /* enter it */
	}
    }
    
    closedir(dirf);
    
}
