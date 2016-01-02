/* Notification Server -- definitions 

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

*/

/* definitions for packet fields with fixed # of bytes
   (note: internally we can use "long" instead) */
typedef bit32 uid_typ;		/* 4 bytes */
typedef bit32 sta_typ;		/* 4 bytes */


/* Bucketed hash table of notifier addresses (keyed by uid).
   So far, we don't keep track of what particular services each notifier
   has registered itself as being interested in.
*/

#define NTABSIZE        128     /* size of hash table */
#define BUCKSIZE        20      /* entries per bucket */

#define SERVMAX		8	/* max # of service codes to remember */
struct servtab {
	long 	count;		/* number of entries */
	long	serv[SERVMAX];
};
typedef struct servtab servtab;	

/* ATP address (either DDP or UDP) */
struct atpaddr {
	int	family;	/* AF_INET or AF_APPLETALK */
	u_bit32	addr;	/* IP or DDP address */
	u_short port;	/* port (UDP only) */
};
typedef struct atpaddr atpaddr;

#define ATP_ADDR_EQ(a,b) ((a)->family == (b)->family &&\
			  (a)->addr == (b)->addr &&\
			  (a)->port == (b)->port)

struct notifybuck {             /* one bucket */
        struct notifybuck *flink;/* next bucket in list */
        struct notifybuck *blink;/* previous bucket in list */
        int count;              /* number of valid entries */
        struct {
            long uid;		/* user id */
            atpaddr regaddr;	/* network address to send notification to */
            long time;		/* age of entry (seconds) */
	    servtab servcode; 	/* list of service codes */
        } entry[BUCKSIZE];
};

typedef struct notifybuck notifybuck;
notifybuck *notifytab[NTABSIZE];/* the hash table */
pthread_mutex_t notifytab_lock;  /* lock protecting it */
boolean_t notifytab_dirty;	/* needs writing? */
#define WRITE_INTERVAL	15	/* write it out this often (seconds) */

/* hash table of "sticky" notifications (keyed by uid). */

struct notif {			/* saved notification */
        struct notif 	*flink; /* next notif in list */
        struct notif	*blink; /* previous notif in list */
    	long		id;	/* notification id */
    	long		typ;	/* and type */
	long		len;	/* length of variable-len data */
    	notifydat	data;	/* data itself */
};
typedef struct notif notif;

struct stickybuck {		/* one bucket */
        struct stickybuck *flink;/* next bucket in list */
        struct stickybuck *blink;/* previous bucket in list */
        int count;              /* number of valid entries */
	struct {
	    long	uid;	/* user id */
	    notif	*not;	/* saved notifications */
	} entry[BUCKSIZE];
};

typedef struct stickybuck stickybuck;
stickybuck *stickytab[NTABSIZE]; 
pthread_mutex_t stickytab_lock;		/* lock protecting the table */
boolean_t stickytab_dirty;	/* needs writing? */

struct {
        long reg;		/* registrations by name */
        long reg_dup;		/* ...duplicating records already there */
        long reg_fail;		/* invalid registration attempts */
        long tickle;		/* registrations by tickle */
        long tickle_dup;	/* duplicates */
        long active;		/* current active entries */
        long sent;		/* notifications sent */
} notifystats;


/* Queues of pending ATP requests:

    When an ATP registration request arrives, it added to the newreq queue.
    The registration processing thread takes requests off this queue,
    checks the DND, then constructs and sends the ATP response.  The
    request is then moved to the oldreq queue for possible retransmission.
    
    The notification itself is also an ATP transaction, except this time
    we're the requester, not the responder.  Notifications that
    haven't yet been acked by the client are queued on rtxreq for possible
    retransmission.
*/

struct req {
	struct req 	*flink;		/* next */
	struct req 	*blink;		/* previous */
	struct ddpbuf 	pkt;		/* request/response packet */
	int		pktl;		/* its length */
	atpaddr 	clientaddr;	/* their address */
	u_short 	tid;		/* ATP tid (in top-level struct...) */
	long		uid;		/* and uid (...for convenience) */
	long		reqtime;	/* time request initiated */
	long		rtxtime;	/* time of last retransmit */
};
typedef struct req req;

struct reqq { req *head; req *tail; };
typedef struct reqq reqq;	

reqq 		newreq;		/* registration requests not yet processed */
reqq 		oldreq;		/* registrations processed & waiting for ATP retransmit */
reqq		rtxreq;		/* notifications sent & waiting for response from client */
pthread_mutex_t req_lock;	/* protects all request lists */
pthread_cond_t 	req_wait;	/* wait here for non-null newreq */

#define ATPHDRLEN	8	/* (includes userbytes) */
#define ATPDATAMAX	578

struct atphdr {			/* ATP header */
	u_bit8	cmd; 		/* command/control field */
	u_bit8	bitseq;		/* bitmap/sequence no. */
	char	tid[2];		/* transaction id */
	char userbytes[4];	/* for next-level protocol */
	char atpdata[ATPDATAMAX];
};
typedef struct atphdr atphdr;

#define ATP_CMDMSK	0xC0	/* mask for command field */
#define ATP_TREQ	0x40	/* ATP command - request */
#define ATP_TRES	0x80	/* ATP command - response */
#define ATP_TREL	0xC0	/* ATP command - release */
#define ATP_XO		0x20	/* exactly-once flag */
#define ATP_EOM		0x10	/* end-of-message flag */
#define ATP_STS		0x08	/* send transaction status */

u_long tid;			/* ATP transaction id */
pthread_mutex_t	tid_lock;		/* lock protecting it */

#define ATP_RTX_CHECK	5	/* retransmit thread granularity (seconds) */
#define ATP_RTX_INT	30	/* retransmit this often (seconds) */
#define ATP_TIMEOUT	180	/* time out transaction after this long */

#define NOTHDRLEN	12	/* length of fixed header */

struct nothdr {			/* notify header -- in atpdata */
    	char 		typ[4];	/* notification type */
	char		uid[4];	/* userid */
	char		id[4];	/* notification identifier */
	notifydat	data;	/* notification data (length determined from DDP hdr) */
};
typedef struct nothdr nothdr;

struct atpskt {			/* dual protocol sockets */
    ddpsockp 	ddp;		/* (DDP) */
    int		udp;		/* (UDP) */
};
typedef struct atpskt atpskt;

atpskt regskt;			/* registration/notification sockets */

#define NOTIFYNAME	"dartnotify-me"
pthread_mutex_t herrno_lock;  /* lock for get[host|serv]byname routines */

/* notify packet userbyte values */

#define N_REGISTER	"NR02"	/* registration request */
#define N_NOTIFY	"NOTI"	/* notification transaction */
#define N_CLEAR		"CLEN"	/* clear sticky notification */

/* notify registration status codes (DND status codes are also possibilities) */
#define N_OK		0
#define N_NODND		901
#define N_WRONGSERV	902
#define N_BADREQ	903

/* client responses to notification transaction */
#define NC_OK	 	0		/* notification accepted */
#define NC_NOUSER	-1		/* client doesn't care about that user */
#define NC_NOSERVICE	-2		/* client doesn't care about that service */

/* Definitions for TCP interface to submit notifications */
#define NOTIFYPORT	"dartnotify"	 /* /etc/services name to listen for */
#define NOT_CMD_MAX	256

struct notifystate {			/* state variables for connection */
	t_file		conn;		/* the connection */
	boolean_t	done;		/* time to close? */
	boolean_t	validated;	/* client validated yet? */
	boolean_t	validating;	/* validation in progress? */
	t_file		*dnd;		/* dnd connection for validation */
	char		comline[NOT_CMD_MAX]; /* command line */
	struct sockaddr_in	remoteaddr;	/* client's addr */
};
typedef struct notifystate notifystate;

/* active NBP registration info */

struct nbpinfo {
	struct ddpbuf 	regpkt;		/* registration packet */
	int		len;		/* its length */
	ddpsockp	ddpsock;	/* socket to send it on */
};
typedef struct nbpinfo nbpinfo;

#define NOT_OK		200		
#define NOT_GREET	220
#define NOT_BYE		221
#define NOT_NEEDPW	300	
#define NOT_NODND	450
#define NOT_BADCMD	500
#define NOT_BADARG	501
#define NOT_BADSEQ	503
#define NOT_BADUSER	550
#define NOT_BADPW	551
#define NOT_FAIL	554		/* generic failure */
