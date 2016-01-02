/*  BlitzMail Server -- mailbox definitions
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/mbox.h,v 3.6 98/10/21 16:08:03 davidg Exp Locker: davidg $
    
    Mailbox data structures.
    
    In-memory data stored for each active mailbox.  A mailbox is active either because
    the user is currently connected, or because we're doing something to it (like
    expiring messages).  
    
    The three basic components of the mailbox are preferences, mailing lists, and
    message summaries.  Each is read in when first referenced, and remains in memory
    as long as the box is active.
*/
#ifndef _MBOX_H
#define _MBOX_H

#include "t_dnd.h"
#include "misc.h"
#include "control.h"

#define MAX_STR		256	/* max for many kinds of strings (plus null) */
#define MESS_NAMELEN	128	/* enough for pathname of message or temp file */
#define MBOX_NAMELEN	64	/* long enough for full pathname of box dir */

/*    ----      Addressing         ----    */

#define	ADDR_MAX_DEPTH	6	/* number of recursive levels for address resolution */
#define ADDR_MAX_RECIPS 500	/* max number of recipients per message */
#define MAX_ADDR_LEN	256	/* max mail address */
#define FOLD_MARGIN	72	/* for folding header lines */

/* recipient structure.  Allocated by address resolution, used by delivery routines
   to create the message header & deliver the message. */
   
struct recip {
	struct recip 	*next;	/* link in recip list (circular) */
	char		name[MAX_ADDR_LEN]; /* DND name */
	char		addr[MAX_ADDR_LEN]; /* internet address */
	long		id;	/* uid if recip was DND name */
	u_long		timestamp; /* resolve timestamp (mactime) */
	boolean_t	local; 	/* is recip a blitz mailbox? */
	int		blitzserv; /* local: which server */
	char		blitzfs[MBOX_NAMELEN]; /* local: which partition */
	boolean_t	nosend;	/* don't send to this address */
	boolean_t	noshow;	/* don't display this address */
	boolean_t	noerr;	/* don't give immediate error */
	boolean_t	vacation; /* send vacation message */
	boolean_t	oneshot; /* one-shot address for enclosure clone */
	int		stat;	/* status from resolve routines */
};

typedef struct recip recip;

/* status returns from resolve routines (in recip.stat) */

#define RECIP_OK		0
#define RECIP_AMBIGUOUS 	1
#define RECIP_BAD_ADDRESS	2
#define RECIP_NO_SEND		3
#define RECIP_NO_DND		4
#define RECIP_LOOP		5
#define RECIP_LAST		6

/* special postmaster address */
#define POSTMASTER	"Postmaster"

/*    ----      Warnings           ----    */

struct warning {			/* client warning */
	struct warning	*next;		/* next in list */
	char		text[MAX_STR+1]; /* status & optional text */
};
typedef struct warning warning;

/*    ----      Mailing Lists      ----    */

/* In-memory mailing list representation.
   This is essentially just a linked list, but each block of the list may contain
   multiple values (to avoid the waste of allocating a block for every name).  Each
   name within the block is newline-terminated; the total length used is recorded in
   order to make it easy to append a new name to the end.  
   
   Blocks are not fixed-length:  when reading a mailing list from a file the usual
   strategy is to use one big block; multiple blocks are useful when the list is
   grown one name at a time.
*/

#define ML_MAX			32000		/* max mailing list size */
#define ML_DIR			"/mlist/"	/* list subdirectory string */

#define ML_MINBLOCK		128
struct ml_data {
	struct ml_data		*next;		/* link to next */
	int			len;		/* valid length */
	int			maxlen;		/* block length */
	char			data[1];	/* handle to start of the data */
};

typedef struct ml_data ml_data;

/* Hash table for mailing list lookup.
    List isn't bucketed because (for reasonable #s of lists, at least),
    it would tend to waste memory since most buckets would be empty */
	   
#define ML_HASHMAX		127		/* should be prime */

struct ml_name {
    struct ml_name		*next;		/* link */
    char			name[1];	/* "unwarranted chumminess with the compiler" */
};

typedef struct ml_name 	ml_name, *ml_namep;

struct ml_tab {
    int			count;		/* number of lists in table */
    ml_namep		hashtab[ML_HASHMAX];
};
typedef struct ml_tab ml_tab;

#define PUBML_FS	0		/* public mailing stored on first filesystem by default */
 
/* public mailing list access bits */
#define	LACC_READ	4
#define LACC_WRITE	2
#define LACC_SEND	1

#define LTYPE_LOCAL		1		/* personal/public list types */
#define LTYPE_GLOBAL		2


/*    ----      Preferences      ----    */

#define PREF_MAXLEN	256		/* max pref name or contents (plus null) */
#define PREF_FILE	"/prefs"	/* pref filename */

/* Hash table for pref lookup. */
	   
#define PREF_HASHMAX	127			/* should be prime */

/* The "name" field also contains the pref value (right after the name). */

struct pref {
    struct pref		*next;		/* link */
    int			namelen;	/* name length */
    char		name[1];	/* name, followed by value */
};

typedef struct pref 	pref, *prefp;

struct pref_tab {
    boolean_t		dirty;		/* needs to be written? */
    prefp		hashtab[PREF_HASHMAX];
};

typedef struct pref_tab pref_tab;

/* preference (suffix) that holds public mailing list control info: */

#define PREF_MLCTL	"-ctl"

/* keep a record of removed lists for 30 days so a stale update doesn't
   recreate a removed list */
#define PUBML_DWELL	(60*60*24*30)

/* Most preferences are used only by the client; these are the values we
    care about: */
    
#define PREF_AUTOEXP	"AutoExp"	 
#define PREF_AUTOEXPAUD	"AutoExp1"
#define PREF_AUTOEXPTRASH	"AutoExp0"	 
#define	PREF_FWD	"ForwardTo"
#define PREF_EXP	"Expired"
#define PREF_VACATION 	"Vacation"
#define PREF_VERBNOT	"VerboseNotify"
#define PREF_LASTLOG	"LastLogin"
#define PREF_MBLEN	"MailboxLength"
#define PREF_NEWMAIL	"NewMail"
#define PREF_SESSIONID	"SessionId"
#define PREF_INITIALMESS "InitialMess"
#define PREF_FOLDSESSIONTAG "FoldSessionTag"

/*    ----      Summaries      ----    */

/*  Message summary information.
	
    When multiple structs are concatenated in a bucket, the full length of
    the varying fields is not allocated, but each struct is padded to a multiple
    of 4 bytes for alignment.  
    
    Individual summaries that need to be edited (e.g., the "current message")
    allocate the full-sized struct, keeping the varying strings at fixed
    locations (so they don't have to be slid around).
*/

#define MESSTYPE_BLITZ	1		/* message body is plain mac text */
#define MESSTYPE_RFC822	2		/* message body is RFC822-friendly (MIME) */
#define MAX_TOPC_LEN	500		/* max subject */

struct summinfo {
	int		len;		/* length of entire struct (w/ padding) */
	long		messid;		/* message id */
	char		date[9];	/* date (mm/dd/yy) */
	char		time[9];	/* time (hh:mm:ss) */
	long		type;		/* message file type */
	long		totallen;	/* length including enclosures */
	long		enclosures;	/* enclosure count */
	boolean_t	receipt;	/* receipt requested? */
	boolean_t	read;		/* message read? */	
	u_long		expire;		/* expiration date/time (using Mac epoch) */
	char		*sender;	/* sender	 -- within varying area */
	char		*recipname;	/* recip name    -- within varying area */
	char		*topic;		/* topic	 -- within varying area */
	char		sender_[MAX_ADDR_LEN]; /* varying-length fields begin here */
	char		recipname_[MAX_ADDR_LEN]; 
	char		topic_[MAX_TOPC_LEN];
};

typedef struct summinfo summinfo;

#define SUMMBUCK_LEN	2000		/* at least large enough for max-size entry */

struct summbuck {			/* one bucket of summary data */
	struct summbuck *next;		/* link */
	int		count;		/* number of entries in bucket */
	int		used;		/* bytes used */
	char		data[SUMMBUCK_LEN];
};
typedef struct summbuck summbuck;

#define SUMM_MAGIC	"BlitzSumm v1"	/* file identifier */

#define FOLD_NAMELEN	32
struct folder {
	char		name[FOLD_NAMELEN]; /* folder name */
	int		num;		/* and number */
	long		count;		/* number of messages in folder */
	long		foldlen;	/* their total length */
	boolean_t	dirty;		/* needs to be written out? */
	summbuck	*summs;		/* pointer to summaries */
};
typedef struct folder folder;

/* canonical folders */
#define DEFAULT_FOLDCOUNT 3		/* how many there are */
#define TRASH_NAME	"Trash"
#define TRASH_NUM	0
#define AUDIT_NAME	"Sent Msgs"
#define AUDIT_NUM	1
#define INBOX_NAME	"In Box"
#define INBOX_NUM	2
#define INBOX_FILE_NAME "InBox"		/* summ file is spelled this way, sigh */

#define FOLD_MAX 256			/* max folders/user */

/*    ----      Vacation           ----    */

#define VACATION_TEMP	"/.vacation"	/* upload new vacation msg here */
#define VACATION_FNAME 	"/vacation"	/* real thing is here */
#define VACATION_LIST	"/vacation-list" /* addrs that have already received msg */

/*    ----      Message element    ----    */

/*  Each element (header, text, 1 enclosure) of the current message may be
    in either a temp file or part of an existing message file.  In either
    case, the file is not generally kept open, so the name must be recorded.
*/

struct fileinfo {
	char		fname[MESS_NAMELEN]; /* pathname */
	long		offset;		/* offset of data within file */
	long		len;		/* and its length */
	boolean_t	temp;		/* remove file when done? */
};
typedef struct fileinfo fileinfo;	

struct enclinfo {			/* one enclosure */
	struct enclinfo *next;		/* next encl in message */
	char		type[MAX_STR]; 	/* quoted file type & creator */
	char		name[MAX_STR]; 	/* mac filename */
	fileinfo	finfo;		/* the file itself */
};
typedef struct enclinfo enclinfo;

/*    ----      User Data Block    ----    */

/*  The following state information is maintained for each client connection.
    Note that "active mailboxes" is a superset of "connected clients", 
    thus the udb can be accessed via a pointer in the mailbox block (which
    is null if the user isn't connected.)
*/

#define GROUP_MAX	16		/* max # of groups/user */

struct udb {
	struct udb	*next;		/* link in u_head list */
	struct mbox	*mb;		/* backpointer to mailbox */
	t_file		conn;		/* connection to client */
	struct sockaddr_in remoteaddr;	/* client's address/port */
	long		uid;		/* user id */
	int		fs;		/* and filesystem */
	int		groupcnt;	/* # of groups they belong to */
	long		gid[GROUP_MAX];	/* group id */
	boolean_t	prived;		/* privileged user? */
	char		*name;		/* user's DND name */
	char		*email;		/* DND email addr (w/ vanity host) */
	char		*version;	/* client version */
	int		currmessfold;	/* folder current mess is in */
	warning		*warn;		/* pending warnings */
	t_file		*dnd;		/* dnd server connection */
	boolean_t	validated;	/* signed on yet? */
	boolean_t	validating;	/* in mid-signon */
	boolean_t	changing;	/* changing dnd info */
	boolean_t	duplicate;	/* user already logged on */
	boolean_t	shutdown;	/* connection closing */
	recip		*torecips;	/* current recipient list: to */
	recip		*ccrecips;	/*	  ''		 : cc */
	recip		*bccrecips;	/*        ''		 : bcc */
	int		recipcount;	/* total # in all 3 lists */
	boolean_t	wantreceipt;	/* requesting return receipt? */
	boolean_t	hiderecips;	/* hide recipient list? */
	boolean_t	hextext;	/* binhex text encls? */
	char		*replyto;	/* special reply-to addr? */
	int		xtra_audit;	/* also audit current msg to here */
	ml_data		*ldat;		/* mailing list data */
	fileinfo	head;		/* current message: header */
	fileinfo	text;		/*       ''       : text */
	enclinfo	*ep;		/*	 ''       : enclosures */
	summinfo	summ;		/*	 ''	  : summary info */
	char		comline[MAX_STR]; /* current command line */
	char		*comp;		/* pointer into it */
	long		cmdtime;	/* time of last command */	
	
	/* POP-specific values */
#define POP_DELETED_CHUNK	100	/* grow popdeleted array this many items at a time */
	boolean_t	pop;		/* is this a POP client? */
	int		pophighestmsg;	/* highest message index accessed in this session */
	int		*popdeleted;	/* array of deleted message indices */
	int		popdeletedsize;	/* length of popdeleted array */
	int		popdeletedcount;/* number of entries in popdeleted array */
	boolean_t	krbvalidated;	/* valid kerberos ticket received */
};

typedef	struct udb	udb;

udb 	*u_head;			/* list of active users */

/*    ----      Mailbox block      ----    */

/*  The mailing list, preference, and summary data structures are all
    located through the "mbox" block, which is the central repository
    of information for each active mailbox.  Basically all operations
    on the mbox block or any of the subsidiary data structures require
    that mbox.mbsem be seized.
*/

struct mbox {
	struct mbox	*next;		/* link within hash table */
        struct mbox	*prev;		/* back link */
	long		uid;		/* userid */
	int		attach;		/* attachment count (cannot free if > 0) */
	int		idle;		/* times mbox_dowrite has found box idle */
	struct sem	mbsem;		/* protects entire structure */
	int		fs;		/* which file system box belongs on */
	char		boxname[MBOX_NAMELEN]; /* box pathname */
	struct {
	    struct bufl *first, *last;
	} 		obuf;		/* output buffered while box locked */
	boolean_t	xfering;	/* transfer in progress? */
	boolean_t	gone;		/* user transferred to other server? */
	boolean_t	checked;	/* summ_check done yet? */
	udb		*user;		/* user data block, iff currently connected */
	folder		*fold;		/* all folders */
	int		foldmax;	/* size of allocated folder array */
	pref_tab	*prefs;		/* pref hash table */
	ml_tab		*lists;		/* mailing list hash table */ 
	long		boxlen;		/* total length of messages */
};

typedef struct mbox mbox;

/* Hash table for mbox structures (just use low bits of uid for hash) */
#define MBOX_HASHMAX	256
#define MBOX_HASH(x)	(x % MBOX_HASHMAX)

mbox 	*mbox_tab[MBOX_HASHMAX];
int	mbox_count;			/* number of entries in table now */

/* Semaphore protecting hash table.  To avoid deadlocks, always seize table sem
   before individual box sem, if both are needed. */
   
struct sem	mbox_sem[MBOX_HASHMAX];
pthread_mutex_t	clock_lock;		/* protects localtime(3) */

/*      ----      Statistics      ----    */

mb_stats_t mb_stats;		/* counters; see control.h */

/*      ----      Expiration globals      ----    */

/* Only 1 expiration check may be in progress at a time. A separate
   thread is spawned for each fs; they share these globals. */
   
struct {
    pthread_mutex_t	lock;		/* protects remaining fields */
    pthread_cond_t	wait;		/* wait here for threads to finish */    
    boolean_t		expiring;	/* expiration in process? */
    int			threads;	/* thread count */
    u_long		today;		/* expiration date */
    long		total;		/* total expired */
    t_file		*explog;	/* expiration log file */
    t_file		*stolog;	/* storage log file */
} exp;

/*      ----      Table of recently-used client addresses ---- */

/* Whenever a client logs in, record the IP address & login time in this table.
   The SMTP filtering functions can then search the table to determine whether
   a given SMTP client is "known". The idea is to accept SMTP relaying from
   addresses that we would otherwise reject if one of our users has recently
   logged in from that address. */

struct login_info {
	u_bit32		where;		/* client IP addr */
	u_bit32		when;		/* and login time */
};
typedef struct login_info login_info;

#define LOGIN_INFO_BLOCKSIZE 100
struct login_info_block {
	struct login_info_block *next;	/* link to next */
	login_info	entry[LOGIN_INFO_BLOCKSIZE];
};
typedef struct login_info_block login_info_block;


/* table of login info records - hash on low-order byte of address */
#define LOGIN_HASHMAX    256
#define LOGIN_HASH(x)    (x % LOGIN_HASHMAX)

login_info_block    *login_tab[LOGIN_HASHMAX]; 	/* note - protected by global_lock */
long	g_recent_login_limit;		/* definition of "recent" (seconds) */
#define DFT_RECENT_LOGIN_LIMIT	(120*60)

/*      ----      Prototypes      ----    */

boolean_t summ_move(mbox *mb, folder *from, folder *to, long messid, long newexp);
boolean_t fold_addsum(mbox *mb, folder *fold, summinfo *insumm);
boolean_t foldnum_valid(mbox *mb, int foldnum);
boolean_t fold_rename(mbox *mb, int foldnum, char *fname);
summinfo *get_summ(mbox *mb, long messid, folder **fold);
mbox *mbox_alloc(long uid, int fs);
void mbox_done(mbox **mb);
int fs_match(char *dnddata);
mbox *mbox_find(long uid, int fs, boolean_t no_record);
void mbox_init();
void ml_clean(ml_data **ml);
boolean_t ml_get(mbox *mb, char *name, ml_data **result, char *casename);
boolean_t ml_rem(mbox *mb, char *name);
void ml_set(mbox *mb, char *name, ml_data *contents);
void ml_summary(mbox *mb, boolean_t global);
void ml_free(mbox *mb);
void ml_readhash(mbox *mb);
boolean_t open_vacation(mbox *mb, fileinfo *finfo);
void pref_didchange(mbox *mb, char *key, char *value);
boolean_t pref_get(mbox *mb, char *key, char *value);
boolean_t pref_get_int(mbox *mb, char *key, char *value);
void pref_read(mbox *mb);
boolean_t pref_rem(mbox *mb, char *key);
boolean_t pref_rem_int (mbox *mb, char *key);
void pref_free(mbox *mb);
void pref_set(mbox *mb, char *key, char *value);
void pref_set_int(mbox *mb, char *key, char *value);
void pref_write(mbox *mb);
void set_sessionid(mbox *mb);
void touch_folder(mbox *mb, folder *fold);
int pubml_acc(mbox *mb, char *name);
boolean_t pubml_getctl(mbox *mb, char *name, u_long *modtime, long *owner, long *group, 
			char *lacc, boolean_t *removed);
void pubml_putctl(mbox *mb, char *name, u_long modtime, long owner, long group, 
			char *lacc, boolean_t removed);	
void pubml_sendupdate_all ();
int pubml_acc_int(mbox *mb, mbox *putmlmb, char *name);
boolean_t pubml_get(char *name, ml_data **result);
boolean_t pubml_owner(char *name, char *owner);
void pubml_set(char *name, ml_data *contents);
boolean_t pubml_rem(char *name);
void pubml_update(fileinfo *head, fileinfo *text);
void summ_check(mbox *mb);
void summ_copy(summinfo *to, summinfo *from, boolean_t pack);
boolean_t summ_copymess(mbox *mb, folder *from, folder *to, long *messid, long newexp);
void summ_fmt(summinfo *summ, char *buf);
boolean_t summ_parse(char *buf, summinfo *summ, char *fname, boolean_t pack);
void summ_read(mbox *mb, folder *fold);
void summ_write(mbox *mb, folder *fold);
void summ_free(mbox *mb, folder *fold);
boolean_t summ_deliver(mbox *mb, summinfo *insumm, int foldnum, long len);
void empty_folder(mbox *mb, folder *fold);
long fold_autoexp(mbox *mb, int foldnum);
int fold_create(mbox *mb, char *fname);
void fold_summary(mbox *mb, folder *fold, long first, long last);
void fold_list(mbox *mb, long foldnum);
void fold_remove(mbox *mb, folder *fold);
int fold_size(mbox *mb, folder *fold);
long fold_total(mbox *mb, folder *fold);
char getaddrc(char **p, int *comment, boolean_t *quot, boolean_t *route, boolean_t *esc);
boolean_t splitname(char *nameaddr, char *name, char *addr);
void free_recips(recip **rlist);
void copy_recip(recip *r, recip **l);
void addhost(char *from, char *to, char *host);
boolean_t macmatch(char *name, char *addr);
int blitzserv_match(char *dnddata);
boolean_t isinternet(char *name, boolean_t *badaddr);
boolean_t local(char *inname);
boolean_t in_local_domain(char *inname);
boolean_t trim_comment(char *addr, char *comment);
recip *resolve(char *name, mbox *mb, int *recipcount);
char *getheadline(t_file *in, long *remaining, boolean_t unfold);
void check_forward(recip **r, int depth, int *recipcount);
void sresolve(char *inname, recip **r, int depth, mbox *mb, mbox *mlmb, int *recipcount);
recip *alloc_recip(int *recippcount);
boolean_t splitaddr(char *addr, char *localpart, char *hostpart);
int hostmatch(char *hostpart, char **list);
boolean_t set_expr(mbox *mb, folder *fold, long messid, u_long expdate);
pthread_addr_t mbox_writer(pthread_addr_t zot);
long mbox_size(mbox *mb);
void mbox_dowrite(boolean_t shouldfree);
pthread_addr_t expire(pthread_addr_t zot);
void expire1(long uid, int fs, u_long today);
int choose_fs();
void record_fs(mbox *mb);
#endif /* _MBOX_H */


