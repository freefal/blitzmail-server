/*  Mach BlitzMail Server -- configuration

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/config.h,v 3.4 98/04/27 13:49:57 davidg Exp Locker: davidg $
    
    The following global variables are read from the configuration
    file at startup time.  After startup, they are constant, so
    no locking is required to access them.
*/

#include "misc.h"

#define CONFIG_FNAME		"/usr/local/lib/blitz/blitzconfig"

/* tables of host & domain names */
#define HOST_MAX	64

char	*m_domain[HOST_MAX+1];	/* name(s) of local domain */
int	m_domaincnt;
char	*m_alias[HOST_MAX+1];	/* all versions of our hostname */
int	m_aliascnt;
char	*m_hostname;		/* our primary hostname */
char 	**m_server;		/* all peer servers */				
int	m_servcount;		/* number of peer servers */
int	m_thisserv;		/* which one we are */
char	*m_dndserver;		/* dnd server to use */
char	*m_dndhost[HOST_MAX+1];	/* hostnames that mean "send to preferred addr" */
int	m_dndhostcnt;
boolean_t m_dndresolver;	/* are we the authoritative dnd resolver? */
char	*m_fullservname;	/* fully-qualified name of this server */
boolean_t m_smtpdisconnect;	/* disconnect from smtphost after each message */
char	*m_xferok[HOST_MAX+1];	/* non-peers allowed to transfer to us */
int	m_xferokcnt;
char	*m_okhead[HOST_MAX+1];	/* header lines client may specify */
int	m_okheadcnt;

/* dnd name, uid, and pw of server entity */
char	*priv_name;
char	*priv_pw;
long	pubml_uid;		/* (uid corresponding to priv_name; from dnd) */
int	pubml_fs;		/* public lists stored on this fs */

long	boxpig;			/* nag users with more mail than this (k) */

long	mess_max_len;		/* limit on message size (bytes) */

long	dndexp_warn;		/* warn if account will expire within this many days */

long	dft_expire;		/* default inbox expiration (months) */
#define DFT_EXPIRE	6	/* (if not overridden by config file) */

char 	*dft_auditexpire;	/* default audit expiration (days or months) */
#define DFT_AUDITEXPIRE	"7"	/* (if not overridden by config file) */

char 	*dft_trashexpire;	/* default trash expiration (days or months) */
#define DFT_TRASHEXPIRE	"1M"	/* (if not overridden by config file) */

#define POP_TRASHEXPIRE 3	/* POP "delete" command causes fast expire */

char	*f_logfile;		/* logfile pathname */
char	*f_initialmess;		/* initial message pathname (on spool fs!) */
char	*f_xfermess;		/* user xfer greeting message ( '' ) */
char	*f_deportmess;		/* user deport farewell message ( '' ) */
char 	*f_messid;		/* file recording last messid used */
char	*f_expdate;		/* file recording date of last exp check */
char	*f_explog;		/* log of expired messages */
char	*f_stolog;		/* per-user storage report */
char	*f_warning;		/* file containing warning text */
char	*f_notifytab;		/* file of notifiers (notifyd) */
char	*f_stickytab;		/* file of sticky notifications (notifyd) */
char	*f_smtpfilter;		/* file of incoming SMTP filter rules */
boolean_t m_binhexencls;	/* send binhex enclosures */
boolean_t m_recvbinhex;		/* receive binhex enclosures */
char    *m_smtp_disclaimer;     /* disclaimer on incoming smtp */

long	cleanout_grace;		/* grace period when cleaning out invalid boxes */
#define DFT_CLEANOUT_GRACE -1

u_long	exp_time;		/* time of day (seconds) to do expiration */

/*    ----      Filesystems      ----    */

#define FILESYS_MAX	16		/* max # of filesystems/server */

char	*m_filesys[FILESYS_MAX];	/* table of filesystems we use */
int	m_filesys_count;		/* how many */
char 	*m_spoolfs_name;		/* pathname of spool filesystem */
int	m_spool_filesys;		/* which fs spool dir is on (or -1) */

#define MESSTMP_DIR	"/mtmp/"	/* directory for temp messages */
#define MESSXFER_DIR	"/messxfer/"	/* directory for transferred messages */
#define BOX_DIR		"/box/"		/* directory for user mailboxes */
#define SPOOL_DIR	"/spool/"	/* directory for message queues */


/*     ----     Constant Parameters     ----    */

#define BLITZ_SERV	"blitzmail"	/* names in /etc/services */
#define	POP_SERV	"pop3"
#define	POPPASSD_SERV	"poppassd"

void read_config();
void strip_domain(char *hostname);
int uid_to_fs(long uid, int *fs);
