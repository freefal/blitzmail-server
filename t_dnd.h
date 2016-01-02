/*  BlitzMail Server -- dnd definitions
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/t_dnd.h,v 3.6 98/10/21 16:12:23 davidg Exp Locker: davidg $
    
    Thread-safe interface to dnd server.
*/

#ifndef _T_DND_H
#define _T_DND_H

#include "sem.h"

/* pool of available dnd connections */

#define		DND_MAXCONN	20	/* max simultaneous connections */
#define 	DND_POOLMAX	5	/* don't keep more than this many _idle_ ones */
t_file 		*dnd_idlepool[DND_POOLMAX];
int 		dnd_idlecount;		/* number of connections in idlepool */
int		dnd_opencount;		/* total open connections */
pthread_mutex_t dnd_lock;		/* protects all the above */
pthread_cond_t 	dnd_wait;		/* wait for idle connection//serialize opens */
#define		DND_TIMEOUT	10	/* inactivity timeout */

#define DND_FIELDMAX	50	/* max fields returned per record */
#define DND_FIELDNAMELEN 32	/* max field name string length */

struct dndresult {		/* note:  for single result only */
	int	count;		/* number of fields */
	long	len;		/* length of entire struct */
	long	used;		/* part in use now */
	char	*value[DND_FIELDMAX];
};
	
typedef struct dndresult dndresult;  
  
#define DFT_DNDSERVER       	"dnd.dartmouth.edu"
#define DNDPORT         	"dnd"
  
/* DND response codes */
#define DND_COUNT	101	/* <n> records follow */
#define DND_FIELDCOUNT  102	/* <n> fields follow */
#define DND_VALUE	110	/* one field value */
#define DND_FIELD	120	/* one fieldname */
#define DND_OK		200
#define DND_OKTRUNC	201	/* ok, but results truncated */
#define DND_GREET	220	/* initial greeting */
#define DND_CONTINUE	300	/* user ok, encrypt this */
#define DND_NOFIELD	501	/* no such field */
#define DND_BADSEQ	502	/* bad sequence of commands */
#define DND_NOARG	503	/* missing argument */
#define DND_BADARG	504	/* bad argument */
#define DND_NOUSER	520	/* no such user */
#define DND_PERM	521	/* field access denied */
#define DND_AMBIG	522	/* ambiguous name */
#define DND_VAGUE	523	/* query too vague */
#define DND_BADPASS	530	/* wrong password */
#define DND_NOTRUST	531	/* not trusted user */

#define DND_DOWN	-1	/* simulated status when DND not available */

#define DND_FATAL	500	/* first digit of unrecoverable errors */

int t_dndlookup1(char *name, char **farray, dndresult **resptr);
char *t_dndvalue(dndresult *result, char *field, char **farray);
int t_dndval1(t_file **f, char *name, char **farray, char *randnum);
int t_dndval2(t_file *f, char *passwd, char **farray, dndresult **resptr);
t_file *t_dndfind();
void t_dndfree(t_file *f);
void t_dndreset_free(t_file *f);
int t_dndclose(t_file *f);
void t_dndclosepool();
int t_dndfield_exists(char *name, boolean_t *exists);
void t_dndinit();
int t_dndpriv(t_file *f, char *name, char *pw);
int t_dnd_privchange(t_file *f, char *name, char **farray, char **varray);
int uid_to_name(long uid, char *name);
long name_to_uid(char *name, int *dndstat);
int change_password(char *name, u_char *oldpass, u_char *newpass, boolean_t encrypted);
#endif
