/*  Mach BlitzMail Server -- dnd
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/t_dnd.c,v 3.6 98/10/21 16:11:39 davidg Exp Locker: davidg $
    
    Thread-safe interface to dnd server.
    
    A pool of dnd connections are kept active.  t_dndfind allocates a
    connection (making a new one if necessary).  t_dndfree returns
    a connection to the pool (or closes it if there are enough idle
    connections already).  If the other end of a connection goes away,
    we don't notice anything until the next time we try to use that
    connection; most routines that allocate a connection out of the
    pool will retry a "dnd down" status once, for that reason.
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include "t_io.h"
#include "sem.h"
#include "t_dnd.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "cryptutil.h"

int t_dnddoval1(t_file *f, char *name, char **farray, char *randnum);
int t_dnddofield_exists(t_file *f, char *name, boolean_t *exists);
static int do_change_password(t_file *f, char *name, u_char *oldpass, u_char *newpass, boolean_t encrypted);
static t_file *t_dndopen();

/* t_dndinit -- 

    Set up lock & idle connection pool.
    
*/

void t_dndinit() {

    int		i;
    
    pthread_mutex_init(&dnd_lock, pthread_mutexattr_default);
    pthread_cond_init(&dnd_wait, pthread_condattr_default);
    
    for (i = 0; i < DND_POOLMAX; ++i)
	dnd_idlepool[i] = NULL;		/* no idle connections yet */
    dnd_idlecount = 0;
    dnd_opencount = 0;
}

/* t_dndfind -- 

    Get an open dnd connection from the pool (if none are left, allocate
    a new one).  Note that the pool contains only idle connections.  If
    there are too many open connections, wait for one to become free.
    
    Connection attempts (by this routine at least) are serialized, to
    somewhat avoid overwhelming the dnd server with a bunch of simultaneous
    connection attempts (causing some to not get through).
*/

t_file *t_dndfind() {

    t_file	*f;			/* returned: connection */

    pthread_mutex_lock(&dnd_lock);		/* get the pool */
    
    for (;;) {				/* until connection available */
	if (dnd_idlecount > 0) {	/* existing connection available? */
	    f = dnd_idlepool[--dnd_idlecount];
	    break;
	} else {
	    if (dnd_opencount >= DND_MAXCONN) { /* all in use */
		pthread_cond_wait(&dnd_wait, &dnd_lock);
		continue;		/* look for idle one again */
	    }
	    
	    pthread_mutex_unlock(&dnd_lock); /* don't spin while open pending */
	    f = t_dndopen();		/* must make new connection */
	    pthread_mutex_lock(&dnd_lock);
	    break;
	}
    }
    pthread_mutex_unlock(&dnd_lock);	  
   
    return f;
}

/* t_dndfree --

    Return a connection to the idle pool.  If the state of the connection
    is in doubt (e.g., it might be in the midst of a multi-step command),
    it should be closed, not freed.
    
*/

void t_dndfree(t_file *f) {

    pthread_mutex_lock(&dnd_lock);
    if (dnd_idlecount < DND_POOLMAX) {	/* room for another idle one? */
	dnd_idlepool[dnd_idlecount++] = f;
	pthread_mutex_unlock(&dnd_lock);	
	pthread_cond_signal(&dnd_wait);	/* wake thread waiting to connect */
    } else {
	pthread_mutex_unlock(&dnd_lock);	
	t_dndclose(f);			/* pool full; just close it */
    }

}
/* t_dndreset_free -- 

    Reset DND connection to known state, then call t_dndfree.
*/

void t_dndreset_free(t_file *f) {

    char	buf[MAX_STR];
        
    t_fprintf(f, "RSET\r\n", buf);	/* get rid of half-finished command */
    if (t_fflush(f) >= 0) {
	t_fseek(f, 0, SEEK_SET);	/* set up to read now */
	/* read & ignore response */
	if (t_gets(buf, sizeof(buf), f) != NULL) {
	    t_dndfree(f);		/* connection may be reused */
	    return;
	}
    }
    
    /* io trouble on connection; close it */
    t_dndclose(f);

}

/* t_dndopen -- 

    Connect to dnd server, return a t_file for the connection.    
    
    --> dnd_lock NOT seized <--
*/

static t_file *t_dndopen() {

    t_file	*f;			/* returned: open connection */
    int		sock;			/* the socket */
    struct hostent *host;	    	/* host entry for dnd server */
    struct servent *sp;			/* service entry */
    struct sockaddr_in sin;		/* service entry */
    int		on = 1;			/* for setsockopt */
    char	buf[MAX_STR];
        
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	return NULL;

    pthread_mutex_lock(&dnd_lock);
    ++dnd_opencount;		/* another open connection */
    pthread_mutex_unlock(&dnd_lock);

    f = (t_file *) mallocf(sizeof(t_file));
    t_fdopen(f, sock);			/* associate file w/ socket */
    strcpy(f->name, "DND server");

    sem_seize(&herrno_sem);		/* serialize access to gethostbyname */
	    
    if ((host = gethostbyname(m_dndserver)) == NULL) {
	t_errprint_l("dndopen: gethostbyname: %d", h_errno);
	sem_release(&herrno_sem);
	t_dndclose(f);
	sleep(30);			/* don't spin too fast */
	return NULL;
    }

    sin.sin_family = AF_INET;
    bcopy(host->h_addr, (char *)&sin.sin_addr, host->h_length);
    
    if (isdigit(*DNDPORT)) {		/* port given by #? */
	sin.sin_port = htons((u_short) atoi(DNDPORT));
    } else {
	if ((sp = getservbyname(DNDPORT, "tcp")) == NULL) {
	    t_perror1("getservbyname: ", DNDPORT);
	    sem_release(&herrno_sem);
	    t_dndclose(f);
	    sleep(30);			/* don't spin too fast */
	    return NULL;
	}
	sin.sin_port = sp->s_port;	/* get port from services entry */    
    }

    sem_release(&herrno_sem);		/* done with hostname etc. */
    
    /* try to connect; may fail if dnd server is down */
    if (connect(sock, (struct sockaddr *)&sin, sizeof (sin)) < 0) {
	/* If dnd is merely down; log to file but not to console */
	if (pthread_errno() == ETIMEDOUT)
	    log_it("t_dndopen: connect attempt timed out");
	else if (pthread_errno() == ECONNREFUSED)
	    log_it("t_dndopen: connection refused");
	else
	    t_perror("t_dndopen: connect");
	t_dndclose(f);
	return NULL;
    }

    /* enable periodic tickles */
    if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &on, sizeof(on)) < 0)
	t_perror("t_dndopen: setsockopt (SO_KEEPALIVE)");    

    /* non-blocking io mode */
    if (ioctl(sock, FIONBIO, (char *) &on) < 0)
        t_perror("t_dndopen: ioctl (FIONBIO)");

    f->select = TRUE;                   /* t_select should check this file */
    f->timeout = DND_TIMEOUT * 60;     /* set timeout interval (seconds) */

    /* connection established; try for greeting message */
        
    t_gets(buf, sizeof(buf), f);	/* get initial line */
    
    if (atoi(buf) == DND_GREET) 
	return f;			/* all set! */

    if (f->t_errno)			/* connection trouble */
	t_perror("t_dndopen: greeting read:");
    else
	t_errprint_s("Unexpected dnd server greeting: %s", buf);

    t_dndclose(f);
    return NULL;

}

/* t_dndclose -- 

    Close dnd connection.
*/

int t_dndclose(t_file *f) {

    pthread_mutex_lock(&dnd_lock);
    --dnd_opencount;
    pthread_mutex_unlock(&dnd_lock);
    pthread_cond_signal(&dnd_wait);	/* wake thread waiting to connect */
    
    return t_fclose(f);		/* easy enough... */
    
}
/* t_dndclosepool -- 

    Close all idle DND connections. This is done whenever a connection taken
    from the pool is discovered to be dead -- if one is gone, it's likely
    all the others are too. Discarding the old connections ensures that we
    retry meaningfully, instead of just trying another stale connection.
*/

void t_dndclosepool() {

    pthread_mutex_lock(&dnd_lock);		/* get the pool */
    
    while (dnd_idlecount > 0) {	/* existing connection available? */
	(void) t_fclose(dnd_idlepool[--dnd_idlecount]);
	--dnd_opencount;
	pthread_cond_signal(&dnd_wait);	/* wake thread waiting to connect */
    }

    pthread_mutex_unlock(&dnd_lock);
        
}
/* t_dnddolookup1 -- 

    Lookup, expecting only 1 response.  Sets up a "dndresult" for the
    match.  If more than one match results, the others are ignored.
*/
int t_dnddolookup1(t_file *f, char *name, char **farray, dndresult **res) {

    char	buf[512];
    int		stat;
    int		len;
    char 	*p;
    char	**field;
    	
    /* construct dnd server command */
    t_sprintf(buf, "LOOKUP %s", name);
    
    if (*farray)
	strcat(buf, ",");
	
    for (field = farray; *field; ++field) {	/* optional field names */
	strcat(buf, " ");
	strcat(buf, *field);
    }

    t_fprintf(f, "%s\r\n", buf);	/* send it */
    if (t_fflush(f) < 0)
	goto trouble;
    t_fseek(f, 0, SEEK_SET);		/* set up to read now */

    if (t_gets(buf, sizeof(buf), f) == NULL)
	goto trouble;			/* connection trouble */
    
    if ((stat = atoi(buf)) != DND_COUNT) { /* bad status? */
	return stat;			/* just return it */
    }    
    
    len = sizeof(dndresult) + 512;	/* set up initial result array */
    *res = mallocf(len);
    (*res)->len = len;
    (*res)->used = sizeof(dndresult);
    (*res)->count = 0;
    
    for (field = farray; *field; ++field) {	/* read all fields back now */

	if (t_gets(buf, sizeof(buf), f) == NULL)
	    goto trouble;		/* connection trouble */
	
	if (atoi(buf) != DND_VALUE) { /* should be a value */
	    t_errprint_s("t_lookup1; expected value, got %s", buf);
	    goto trouble;
	}
	
	/* grow result struct if needed */
	if ((*res)->used + strlen(buf+4) > (*res)->len) {
	    (*res)->len += 512;
	    (*res) = reallocf(*res, (*res)->len);
	}
	
	p = (*res)->used + (char *) *res; /* put value at end */
	strcpy(p, buf+4);		/* copy value */
	(*res)->value[(*res)->count] = p; /* point to it */
	(*res)->used += strlen(p) + 1;	/* update length used */
	
	++(*res)->count;		/* one more field */
    }
    
    stat = DND_OK;			/* will probably be ok... */
    
    /* eat any additional values (if name was ambiguous) */
    do {
	if (t_gets(buf, sizeof(buf), f) == NULL)
	    goto trouble;
	if (atoi(buf) == DND_VALUE)
	    stat = DND_AMBIG;		/* more than 1 match */
    } while (atoi(buf) == DND_VALUE);
    
    if (atoi(buf) != DND_OK && atoi(buf) != DND_OKTRUNC)
	goto trouble;
	    
    return stat;
    
    trouble:				/* connection trouble etc. */
    
    if (*res) {
	t_free(*res);
	*res = NULL;
    }
    return DND_DOWN;

}

/* t_dndlookup1 -- 

    Lookup, retrying a DND_DOWN status once.  This is a sensible thing to do
    because if the connection is lost, we won't discover this fact until we
    try to write to it.  By that point, the DND may have returned to service,
    so it's worth trying to reconnect.
*/

int t_dndlookup1(char *name, char **farray, dndresult **res) {

    int		stat;			/* returned: DND status */
    int		i;
    t_file	*f;			/* dnd connection */
    
    for (i = 0 ; i < 2; ++i) {		
	if ((f = t_dndfind()) == NULL) { /* find/create dnd connection */  
	    stat = DND_DOWN;		/* dnd is down */
	    break;
	}
	stat = t_dnddolookup1(f, name, farray, res);
	if (stat == DND_DOWN) {		/* connection lost? */
	    t_dndclose(f);		/* close the suspect connection */
	    t_dndclosepool();		/* and any other idle connections saved */
	} else {
	    t_dndfree(f);		/* recycle the connection */
	    break;			/* got answer from dnd */
	}
    }
    
    return stat;
}
/* t_dndfield_exists -- 

    Check for existence of a field. Retry DND_DOWN once.
*/

int t_dndfield_exists(char *name, boolean_t *exists) {

    int		stat;			/* returned: DND status */
    int		i;
    t_file	*f;			/* dnd connection */
 
    *exists = FALSE;		/* assume the worst */
    
    for (i = 0 ; i < 2; ++i) {	   
	if ((f = t_dndfind()) == NULL) { /* find/create dnd connection */  
	    stat = DND_DOWN;		/* dnd is down */
	    break;
	}    
    	stat = t_dnddofield_exists(f, name, exists);	
	if (stat == DND_DOWN) {		/* connection lost? */
	    t_dndclose(f);		/* close the suspect connection */
	    t_dndclosepool();		/* and any other idle connections saved */
	} else {
	    t_dndfree(f);		/* recycle the connection */
	    break;			/* got answer from dnd */
	}
    }
    
    return stat;
}

int t_dnddofield_exists(t_file *f, char *name, boolean_t *exists) {

    char	buf[512];
    int		stat;

    t_fprintf(f, "FIELDS %s\r\n", name);	/* send command */
    if (t_fflush(f) < 0)
	return DND_DOWN;			/* trouble */
    t_fseek(f, 0, SEEK_SET);			/* set up to read now */
    
    if (t_gets(buf, sizeof(buf), f) == NULL)
	return DND_DOWN;			/* connection lost */
    
    stat = atoi(buf);				/* check status */
    
    if (stat == DND_NOFIELD) {			/* does not exist */
	*exists = FALSE;
	return DND_OK;
    }
    
    if (stat == DND_FIELDCOUNT) { /* first line is field count */
	if (t_gets(buf, sizeof(buf), f) == NULL)
	    return DND_DOWN;			/* connection lost */
	if ((stat = atoi(buf)) == DND_FIELD) {	/* second is field itself */
	    *exists = TRUE;
	    if (t_gets(buf, sizeof(buf), f) == NULL)
		return DND_DOWN;			/* connection lost */
	    stat = atoi(buf);			/* third is <ok> */
	}
    }
    
    return stat;
}

/* t_dndval1 -- 

    Begin validation transaction.  We return the dnd connection
    used; the same connection must be passed to t_dndval2, and
    eventually closed/freed.
        
    Retry DND_DOWN status on write once.
*/

int t_dndval1(t_file **f, char *name, char **farray, char *randnum) {

    int		i;
    int		stat;

    for (i = 0 ; i < 2; ++i) {		
	if ((*f = t_dndfind()) == NULL) { /* find/create dnd connection */  
	    stat = DND_DOWN;		/* dnd is down */
	    break;
	}
	stat = t_dnddoval1(*f, name, farray, randnum);
	if (stat == DND_DOWN) {		/* connection lost? */
	    t_dndclose(*f);		/* close the suspect connection */
	    t_dndclosepool();		/* and any other idle connections saved */
	    *f = NULL;
	} else {
	    break;			/* got answer from dnd */
	}
    }
    
    return stat;
}


int t_dnddoval1(t_file *f, char *name, char **farray, char *randnum) {

    char	buf[512];
    int		stat;
    
    /* construct dnd server command */
    t_sprintf(buf, "VALIDATE %s", name);

    if (*farray)
	strcat(buf, ",");
	    
    while (*farray) {			/* optional field names */
	strcat(buf, " ");
	strcat(buf, *farray++);
    }    
    
    t_fprintf(f, "%s\r\n", buf);	/* send it */
    if (t_fflush(f) < 0)
	return -1;			/* trouble */
    t_fseek(f, 0, SEEK_SET);		/* set up to read now */
    
    if (t_gets(buf, sizeof(buf), f) == NULL)
	return -1;			/* connection lost */
    
    if ((stat = atoi(buf)) == DND_CONTINUE) { /* ok so far? */
	strcpy(randnum, buf+4);		/* yes - here's the random # */
    }
    
    return stat;
}

/* t_dndval2 -- 

    Complete validation transaction.  Caller needs to supply same
    field array as on initial call.  Password may be either
    cleartext or encrypted; PASS or PASE command is selected
    automatically.
    
    The dndresult structure is allocated here (iff good status);
    caller must free.
*/

int t_dndval2(t_file *f, char *passwd, char **farray, dndresult **res) {

    char	buf[512];
    int		len;
    int		stat;
    char	*p;

    *res = NULL;
    
    if (strlen(passwd) == 24)		/* right length for encrypted? */
	t_sprintf(buf, "PASE %s", passwd);
    else
	t_sprintf(buf, "PASS %s", passwd);
	
    t_fprintf(f, "%s\r\n", buf);	/* send it */
    if (t_fflush(f) < 0)
	goto trouble;
    t_fseek(f, 0, SEEK_SET);		/* set up to read */

    if (t_gets(buf, sizeof(buf), f) == NULL)
	return -1;			/* connection trouble */
     if ((stat = atoi(buf)) != DND_COUNT) { /* bad status? */
	return stat;			/* just return it */
    }
       
    len = sizeof(dndresult) + 512;	/* set up initial result array */
    *res = mallocf(len);
    (*res)->len = len;
    (*res)->used = sizeof(dndresult);
    (*res)->count = 0;
    
    for ( ;*farray; ++farray) {		/* read all fields back now */

	if (t_gets(buf, sizeof(buf), f) == NULL)
	    goto trouble;		/* connection trouble */
	
	if ((stat = atoi(buf)) != DND_VALUE) { /* check status */
	    t_free(*res);			/* no good; clean up */
	    *res = NULL;
	    return stat;		/* bad pw, etc. */
	}
	
	/* grow result struct if needed */
	if ((*res)->used + strlen(buf+4) > (*res)->len) {
	    (*res)->len += 512;
	    (*res) = reallocf(*res, (*res)->len);
	}
	
	p = (*res)->used + (char *) *res; /* put value at end */
	strcpy(p, buf+4);		/* copy value */
	(*res)->value[(*res)->count] = p; /* point to it */
	(*res)->used += strlen(p) + 1;	/* update length used */
	
	++(*res)->count;		/* one more field */
    }
    
    /* read the final ok */
    if (t_gets(buf, sizeof(buf), f) == NULL)
    	goto trouble;
    if (atoi(buf) != DND_OK )
	goto trouble;
	    
    return DND_OK;
    
    trouble:				/* connection trouble etc. */
    
    if (*res) {
	t_free(*res);
	*res = NULL;
    }
    
    return -1;
}    

/* t_dndvalue -- 

    Extract value from "dndresult" structure, given field
    name.  Field array must (obviously) match the one used
    on the initial request.  Note that no dnd connection is
    involved; we're just parsing buffered results.
    
*/

char *t_dndvalue(dndresult *res, char *field, char **farray) {

    int		i;
    
    /* search for matching field name */
    for (i = 0; *farray; ++i, ++farray) {
	if (strcasecmp(*farray, field) == 0) {
	    return res->value[i];
	}
    }
    
    return NULL;
}

/* t_dndpriv -- 

    Enable a privileged dnd connection (to allow changing BLITZSERV field
    of arbitrary user records).
*/

int t_dndpriv(t_file *f, char *name, char *pw) {

    char	buf[512];
    int		stat;
    
    /* construct dnd server command */
    t_sprintf(buf, "PRIV %s", name);  
    
    t_fprintf(f, "%s\r\n", buf);	/* send it */
    if (t_fflush(f) < 0)
	return -1;			/* trouble */
    t_fseek(f, 0, SEEK_SET);		/* set up to read now */
    
    if (t_gets(buf, sizeof(buf), f) == NULL)
	return -1;			/* connection lost */
    
    if ((stat = atoi(buf)) == DND_CONTINUE) { /* ok so far? */
	/*** should use random # */
	t_fprintf(f, "PASS %s\r\n", pw); /* send pw */
	if (t_fflush(f) < 0)
	    return -1;
	t_fseek(f, 0, SEEK_SET);	/* set up to read now */
	
	if (t_gets(buf, sizeof(buf), f) == NULL)
	    return -1;			/* connection trouble */
	    
	stat = atoi(buf);	  	/* check validation status */  
    }
    
    return stat;
}

/* t_dnd_privchange -- 

    Issue DND change, using privileged connection.
*/

int t_dnd_privchange(t_file *f, char *name, char **farray, char **varray) {

    char	buf[512];
    int		stat;
    char	**field, **value;
    	
    /* construct dnd server command */
    t_sprintf(buf, "CHANGE %s ,", name);
	
    /* fields & values now */
    for (field = farray, value = varray; *field && *value; ) {
	strcat(buf, " ");
	strcat(buf, *field++);
	strcat(buf, " ");
	strcat(buf, *value++);
    }

    t_fprintf(f, "%s\r\n", buf);	/* send CHANGE command */
    if (t_fflush(f) < 0)
	return -1;
    t_fseek(f, 0, SEEK_SET);		/* set up to read now */


    if (t_gets(buf, sizeof(buf), f) == NULL)
	return -1;			/* connection lost */
    
    if ((stat = atoi(buf)) == DND_CONTINUE) { /* ok so far? */
	t_fprintf(f, "TRUST\r\n"); /* use our privileges */
	if (t_fflush(f) < 0)
	    return -1;
	t_fseek(f, 0, SEEK_SET);	/* set up to read again */
	
	if (t_gets(buf, sizeof(buf), f) == NULL)
	    return -1;			/* connection trouble */
	    
	stat = atoi(buf);	  	/* check change status */  
    }
    
    return stat;

}
/* name_to_uid --

    Consult DND to convert name to uid.  Return -1 if unable to obtain valid uid.
    Status from the dnd is returned in "dndstat".
*/
long name_to_uid(char *name, int *dndstat) {

    struct dndresult	*dndres = NULL;	/* results of dndlookup */
    static char *farray[] = {"UID", NULL};
    long		uid = -1;	/* returned: resolved uid */
    
    *dndstat = t_dndlookup1(name, farray, &dndres);
            
    if (*dndstat == DND_OK)  		/* if ok, use real name instead */	
	strtonum(t_dndvalue(dndres, "UID", farray), &uid);
 
    if (dndres)
	t_free(dndres);
	
    return uid;
}

/* uid_to_name --

    Consult DND to convert uid to name.  Return #<uid> if unable to obtain name.
*/
int uid_to_name(long uid, char *name) {

    struct dndresult	*dndres = NULL;	/* results of dndlookup */
    int			stat;		/* dnd status */
    static char *farray[] = {"NAME", NULL};
    
    t_sprintf(name, "#%ld", uid);	/* construct #uid form */
    stat = t_dndlookup1(name, farray, &dndres);
            
    if (stat == DND_OK)  		/* if ok, use real name instead */	
	strcpy(name, t_dndvalue(dndres, "NAME", farray));
 
    if (dndres)
	t_free(dndres);
	
    return stat;
}

/* change_password --

    Change a DND password (passwords supplied as 8-byte binary).
*/
int change_password(char *name, u_char *oldpass, u_char *newpass, boolean_t encrypted) {


    int		stat;			/* returned: DND status */
    int		i;
    t_file	*f;			/* dnd connection */
    
    for (i = 0 ; i < 2; ++i) {		
	if ((f = t_dndfind()) == NULL) { /* find/create dnd connection */  
	    stat = DND_DOWN;		/* dnd is down */
	    break;
	}
	stat = do_change_password(f, name, oldpass, newpass, encrypted);
	if (stat == DND_DOWN) {		/* connection lost? */
	    t_dndclose(f);		/* close the suspect connection */
	    t_dndclosepool();		/* and any other idle connections saved */
	} else {
	    t_dndfree(f);		/* recycle the connection */
	    break;			/* got answer from dnd */
	}
    }
    
    return stat;

}
static int do_change_password(t_file *f, char *name, u_char *oldpass, u_char *newpass, boolean_t encrypted) {

    char		buf[512];
    u_char		eoldpass[PW_LEN], enewpass[PW_LEN];
    char		ooldpass[CRYPTPW_LEN+1], onewpass[CRYPTPW_LEN+1];
    	    
    if (!encrypted) {
	/* encrypt passwords */
	dnd_encrypt(newpass, oldpass, enewpass, FALSE);
	dnd_encrypt(oldpass, newpass, eoldpass, FALSE);
    }
    
    /* convert to octal */
    tooctal(enewpass, onewpass);
    tooctal(eoldpass, ooldpass);

    /* construct dnd server command */
    t_sprintf(buf, "CHPW %s,%s,%s", name, ooldpass, onewpass);
    
    t_fprintf(f, "%s\r\n", buf);	/* send it */
    if (t_fflush(f) < 0) {
	return DND_DOWN;
    }
    t_fseek(f, 0, SEEK_SET);		/* set up to read now */

    if (t_gets(buf, sizeof(buf), f) == NULL) {
	return DND_DOWN;
    }
    
    return atoi(buf);
}

