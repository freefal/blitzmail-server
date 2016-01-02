/*  BlitzMail Server -- Preferences
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/pref.c,v 3.6 98/10/21 16:09:15 davidg Exp Locker: davidg $
    
    Preferences are stored in the "prefs" text file of the user's
    mailbox directory; one preference per line.  The preference
    value is stored in the quoted form (ready to be sent to the
    client)
	    
    Preferences are kept in a hash table in memory; all changes are made
    to this table (rather than the file directly).  Once the table has been
    modified, a periodic thread will eventually rewrite the file.  Note
    that the order of preferences within the text file is undefined.
    
    The table is not read in until it's actually referenced.
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <ctype.h>
#include <sys/dir.h>
#include <netinet/in.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"

/* prototypes for private funcs */
static u_int pref_hash(char *name);
static boolean_t pref_hashrem(mbox *mb, char *key);
void pref_hashadd(mbox *mb, char *key, char *value);
static boolean_t pref_hashfind(mbox *mb, char *key, prefp *p);

/* pref_didchange --
    
    Called after client changes a preference, to check for any special cases.
    "value" is NULL if the preference has been removed.
*/

void pref_didchange(mbox *mb, char *key, char *value) {

    long	foldnum;
    
    sem_seize(&mb->mbsem);
    
    /* If Expired<n> pref changed, mark that folder as touched */
    if (strncmp(key, PREF_EXP, strlen(PREF_EXP)) == 0) {
	strtonum(key+strlen(PREF_EXP), &foldnum);
	if (foldnum_valid(mb, foldnum)) {
	    touch_folder(mb, &mb->fold[foldnum]);
	}
    }

    sem_release(&mb->mbsem);
}
/* pref_get --
    
    Retrieve preference value.  Caller must provide sufficient storage for
    a max-sized preference. 
*/

boolean_t pref_get(mbox *mb, char *key, char *value) {

    boolean_t 	found;
    
    sem_seize(&mb->mbsem);  
    found = pref_get_int(mb, key, value);
    sem_release(&mb->mbsem);
    return found;			
    
}

/* internal version -- box locked */

boolean_t pref_get_int(mbox *mb, char *key, char *value) {

    boolean_t 	found = FALSE;
    prefp	thepref;		/* entry in hash table */
    char	*p;			/* value part */

    if (!mb->prefs)			/* get prefs, if not yet here */
	pref_read(mb);
		    
    if (pref_hashfind(mb, key, &thepref)) { /* check hash table */
	p = thepref->name + thepref->namelen + 1; /* locate the value */
	strcpy(value, p);
	found = TRUE;
    }
    
    return found;
}

/* pref_read --
    
    Read preference file into hash table.
    
    --> box locked <--
*/

void pref_read(mbox *mb) {

    char	fname[FILENAME_MAX];	/* pref filename */
    t_file	*f;			/* thread-safe io object */
    char	buf[1024];
    int		i;
    char	*p;

    pthread_mutex_lock(&global_lock);
    ++mb_stats.pref_r;
    ++malloc_stats.preftab;
    pthread_mutex_unlock(&global_lock);

    mb->prefs = mallocf(sizeof(pref_tab));
    for (i = 0; i < PREF_HASHMAX; i++)	/* allocate an empty table */
	mb->prefs->hashtab[i] = NULL;
    mb->prefs->dirty = FALSE;		/* not changed yet */
    
    strcpy(fname, mb->boxname); strcat(fname, PREF_FILE);

    if ((f = t_fopen(fname, O_RDONLY | O_CREAT, FILE_ACC)) == NULL) {
	t_perror1("pref_read: cannot open ", fname);
	return;
    }
    
    while (t_gets(buf, sizeof(buf),f) != NULL) {
	if ((p = index(buf,' ')) == NULL) 
	    t_perror1("Bad pref value in ", fname);
	else {
	    *p++ = 0;	 /* split into 2 strings */
	    if (strlen(p)+1 >= PREF_MAXLEN)
		t_perror1("Pref value too long in ", fname);
	    else
		pref_hashadd(mb, buf, p);	/* add it */
	}
    }
    
    t_fclose(f);
    
    mb->prefs->dirty = FALSE;		/* and they haven't changed yet */
}


/* pref_rem --
    
    Remove the named preference, returns false if it doesn't exist.
*/

boolean_t pref_rem(mbox *mb, char *key) {

    boolean_t	ok;

    sem_seize(&mb->mbsem);
    ok = pref_rem_int(mb, key);
    sem_release(&mb->mbsem);
    
    return ok;
}

/* internal version -- box locked */

boolean_t pref_rem_int (mbox *mb, char *key) {

    if (!mb->prefs)			/* get table */
	pref_read(mb);
    
    return pref_hashrem(mb, key);	/* locate & delete the entry */
}

/* pref_set --
    
    Create/update preference.  Enter new value in hash table, mark table as
    modified. Note that the value is quoted (if it isn't already).
*/

void pref_set(mbox *mb, char *key, char *value) {
    
    sem_seize(&mb->mbsem);

    pref_set_int(mb, key, value);
	    
    sem_release(&mb->mbsem);
    
}

/* pref_set_int -- entry w/ box already locked */

void pref_set_int(mbox *mb, char *key, char *value) {
    
    char	quotval[PREF_MAXLEN];
    
    if (!mb->prefs)			/* get table */
	pref_read(mb);
	    
    if (value[0] == '"')		/* is value already quoted? */
    	strcpy(quotval, value);		/* yes - use as-is */
    else
    	strncpy_and_quote(quotval, value, PREF_MAXLEN); /* no - add quotes */
	
    (void) pref_hashrem(mb, key);	/* get rid of any old value */
    pref_hashadd(mb, key, quotval);	/* and add new entry */
 
}

/* pref_write --
    
    Write preference hash table to text file.  Clear "dirty" bit.
    Write the new values to a temp file & rename it (so the file
    isn't wiped out if we crash at an unfortunate moment.)
    
    --> box locked <--
*/

void pref_write(mbox *mb) {

    char	fname[FILENAME_MAX];	/* pref filename */
    char	tempname[FILENAME_MAX];	/* temp filename */
    int		i;
    prefp	p;
    t_file	*f;			/* thread-safe io object */

    pthread_mutex_lock(&global_lock);
    ++mb_stats.pref_w;
    pthread_mutex_unlock(&global_lock);
        
    /* name of pref file */
    strcpy(fname, mb->boxname); strcat(fname, PREF_FILE);
    /* write to temp file first */
    strcpy(tempname, fname); strcat(tempname, ".temp"); 

    if ((f = t_fopen(tempname, O_WRONLY | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("pref_write: cannot open ", tempname);
	return;				/* leave dirty bit set */
    }
    
    /* traverse hash table, write everything out */
    for (i = 0; i < PREF_HASHMAX; i ++) {
	for (p = mb->prefs->hashtab[i]; p != NULL; p = p->next) {
	    t_puts(f, p->name);
	    t_putc(f, ' ');
	    t_puts(f, p->name + p->namelen + 1);
	    t_putc(f, '\n');
	}
    }
    
    t_fflush(f);
    
    if (f->t_errno) 			/* see if errors occured during any of that */
	t_perror1("pref_write: error writing ", tempname);
    else {
	
	if (rename(tempname, fname) < 0)
	    t_perror1("pref_write: rename failed: ", fname);
	else
	    mb->prefs->dirty = FALSE;	/* safely written out! */
    }	
    
    t_fclose(f);    
}

/* pref_free --
    
    Free storage used by preference hash table.  Clear mbox.prefs
    (prefs must be re-read next time they're called for).
    
    --> box locked <--
*/

void pref_free(mbox *mb) {

    int		i;
    prefp	p;
    long	freecount = 0;            

    /* traverse hash table, freeing everything */
    for (i = 0; i < PREF_HASHMAX; i ++) {
	while(p = mb->prefs->hashtab[i]) {	/* (sic) */
	    mb->prefs->hashtab[i] = p->next;
	    t_free(p);
	    ++freecount;
	}
    }
    
    t_free(mb->prefs);		/* now free the table itself */
    mb->prefs = FALSE;		/* no pref table here */

    pthread_mutex_lock(&global_lock);
    --malloc_stats.preftab;	/* one less table */
    malloc_stats.prefentry -= freecount; /* n fewer entries */
    pthread_mutex_unlock(&global_lock);
    
}

/* pref_hash --
    
    Hash name into a small integer in the range 0..PREF_HASHMAX-1.  XOR all the chars
    together into a long (rotating left 4 bits each time, to avoid commutativity),
    divide by table size (which should be prime).  Note that preference names
    are case-senstive.
    
*/

static u_int pref_hash(char *name) {

    u_bit32 	hash;			/* returned: hash value */
    char 	c;			/* next char of name */
    hash = 0;
    
    while(c = *name++) {
	hash = ((hash >> 28) & 0xF) | (hash << 4); /* rotate */
	hash ^= c;	/* xor in next byte */
    }
    
    hash = hash % PREF_HASHMAX;		/* get within table range */
    
    return hash;

}
/* pref_hashadd --
    
    Add entry to hash table.  
    
    --> mailbox locked <--
*/

void pref_hashadd(mbox *mb, char *key, char *value) {

    u_int	hash;			/* hash value */
    prefp	p;			/* entry pointer */
    int		len;			/* length needed */
    
    hash = pref_hash(key);		/* choose slot */
    len = sizeof(pref) + strlen(key) + 1 + strlen(value) + 1;	/* need this much room */
    p = (prefp) mallocf(len);

    pthread_mutex_lock(&global_lock);	/* stats: count entries allocated */
    ++malloc_stats.prefentry;
    pthread_mutex_unlock(&global_lock);
    
    strcpy(p->name, key);		/* copy the name */
    p->namelen = strlen(p->name);
    strcpy(p->name + p->namelen + 1, value);
    
    p->next = mb->prefs->hashtab[hash];	/* add it to front of list */
    mb->prefs->hashtab[hash] = p;
    
    mb->prefs->dirty = TRUE;
}

/* pref_hashfind --
    
    Locate entry in hash table.
    
    --> box locked <--
    
*/

static boolean_t pref_hashfind(mbox *mb, char *key, prefp *p) {

    u_int	hash;			/* hash value */

    hash = pref_hash(key);		/* choose slot */
    
    /* search that slot for the key */
    for (*p = mb->prefs->hashtab[hash]; *p != NULL; *p = (*p)->next) {
	if (strcmp(key, (*p)->name) == 0)
	    break;
    }
    
    return(*p != NULL);
}
    
/* pref_hashrem --
    
    Remove entry from hash table.
    
    --> box locked <--
*/

static boolean_t pref_hashrem(mbox *mb, char *key) {
    
    u_int	hash;			/* hash value */
    prefp	p, pp;		

    hash = pref_hash(key);		/* choose slot */
    
    /* search that slot for the name */
    for (pp = NULL, p = mb->prefs->hashtab[hash]; p != NULL; pp = p, p = p->next) {
	if (strcmp(key, p->name) == 0)
	    break;
    }
    
    if (p) {				/* if found, unlink from table */
	if (pp)
	    pp->next = p->next;
	else mb->prefs->hashtab[hash] = p->next;
	t_free(p);
        pthread_mutex_lock(&global_lock);
        --malloc_stats.prefentry;	/* stats: count allocated entries */
        pthread_mutex_unlock(&global_lock);

	mb->prefs->dirty = TRUE; /* table has changed */
	return TRUE;
    } else 
	return FALSE;
}


/* set_sessionid --

    Increment PREF_SESSIONID each time user logs on.

    --> box locked <--
*/

void set_sessionid(mbox *mb) {

    char	val[PREF_MAXLEN];	/* pref value */
    long	new;			/* new value */

    sem_check(&mb->mbsem);
        
    if (!pref_get_int(mb, PREF_SESSIONID, val)) {
	strcpy(val, "\"1\"");		/* initial value = 1 */
    } else {
	new = atoi(val+1);		/* skip quote */
	t_sprintf(val, "\"%ld\"", new+1); /* new value */
    }
    
    pref_set_int(mb, PREF_SESSIONID, val);
        
}

/* touch_folder --

    Set PREF_FOLDSESSIONTAG<n> to PREF_SESSIONID.  Any folder operation
    that deletes or modifies a message should call touch_folder.  (Exception:
    expiration may instead record the expired messageid in the Expired<n>
    preference).  The tag does NOT need to be updated when a message is added;
    since adds always take place at the end of the folder the client can find
    the new messages without reloading the entire folder.
    
    The per-folder session tags allow clients to keep folder 
    caches valid even if another client session has interposed, so long as 
    the specific folder hasn't been touched.

    --> box locked <--
*/

void touch_folder(mbox *mb, folder *fold) {

    char	key[PREF_MAXLEN];	/* pref name */
    char	val[PREF_MAXLEN];	/* pref value */

    sem_check(&mb->mbsem);
        
    if (!pref_get_int(mb, PREF_SESSIONID, val)) {
	strcpy(val, "\"-1\"");		/* undefined - make folder invalid too */
    } 
    
    t_sprintf(key, "%s%d", PREF_FOLDSESSIONTAG, fold->num);    
    pref_set_int(mb, key, val);		/* copy value into folder pref */
        
}
