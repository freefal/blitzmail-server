/*  Mach BlitzMail Server -- smtp definitions

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/smtp.h,v 3.6 98/10/21 16:10:44 davidg Exp Locker: davidg $
    
*/
#include "sem.h"

#define	SMTPPORT	"smtp"	/* /etc/services name of port to connect to */

/* SMTP response codes */

#define SMTP_HELP	214
#define	SMTP_GREET	220
#define SMTP_BYE	221
#define SMTP_OK		250
#define SMTP_SENDON	354
#define SMTP_BLITZON	354
#define SMTP_SHUTDOWN	421
#define SMTP_NODND	450
#define SMTP_DISKFULL	452
#define SMTP_BADCMD	500
#define SMTP_BADARG	501
#define SMTP_NOTIMPL	502
#define SMTP_BADSEQ	503
#define SMTP_BADRECIP	550
#define SMTP_RECIPMAX	552
#define SMTP_BADBOX	553
#define SMTP_FAIL	554

/* default code to use when filter rejects host/recipient */
#define SMTP_REJECT	550
/* note that SMTP_CMD_MAX must be long enough for a formatted summary */
#define SMTP_CMD_MAX	1600
#define SMTP_MAXLINE	1024
#define SMTP_HOP_MAX	20

/* definitions for first digit of response: */
#define SMTP_RETRY	'4'	 	/* transient error status id */

enum filterstate { FILT_REJECT, FILT_NORELAY, FILT_ACCEPT };
enum filterkind { FILT_ADDR, FILT_NAME, FILT_LOGIN };

struct smtpstate {			/* state variables for SMTP session */
    t_file		conn;		/* the connection */
    struct sockaddr_in remoteaddr;	/* client's address/port */
    char		remotehost[HOST_MAX+1];
    boolean_t		done;		/* closing down? */
    recip		*reciplist;	/* recipient list */
    int			recipcount;	/* ...and count */
    char		comline[SMTP_CMD_MAX]; /* command line */
    char		sender[MAX_ADDR_LEN]; /* sender addr */
    boolean_t		mail;		/* MAIL cmd seen? */
    long		peer;		/* index into m_server, or -1 if not peer */
    boolean_t		xferok;		/* accept transfers from them? */
    enum filterstate	filterlevel;	/* anti-spam filter level */
    int			filt_errcode;	/* error to return if filter trips */
};

typedef struct smtpstate smtpstate;

pthread_cond_t smtpmax_wait;	/* wait here for smtp_num to go down */
long	smtp_num;		/* current # of connected users */
long	smtp_max;		/* absolute max # of connections */
long	smtp_timeout;		/* idle connection timeout (minutes) */
boolean_t smtp_debug;		/* enable debugging logging */

/* anti-spam incoming SMTP connection filters */

struct  ipfilt {
	struct ipfilt	*next;	/* next in table */
	enum filterkind kind;	/* addr, name, or login time? */
        u_bit32         addr;   /* address/mask to match against */
        u_bit32         mask;
	char		name[MAX_STR];	/* name to match against */
	int		logintime; /* "recent" login limit */
        enum filterstate action;/* what to do when matched */
	int		errcode; /* SMTP error code to give if denying */
};
typedef struct ipfilt ipfilt;
ipfilt *smtp_filt, *smtp_filt_tail; /* table of active filters */

struct sem		smtp_filt_sem;	/* semaphore protecting it */

any_t smtplisten (any_t zot);
void smtp_init(void);
