/*  Mach BlitzMail Server -- message queues

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/queue.h,v 3.0 97/01/27 16:59:00 davidg Exp $
*/

/* possible dispositions of queued message */
#define Q_ABORT		1	/* toss it */
#define Q_RETRY		2	/* try again */
#define Q_OK		3	/* done */

#define BLOCK_HDLEN	8	/* length of XBTZ block header */
#define SMTP_HOSTNUM	-1	/* fake hostnum for smtp queue */

struct qent {		/* list of queued messages */
	struct qent	*next;
	long		qid;	/* qid, not messid! */
};

typedef struct qent qent;

/* queue pointers & locks, 1 per host.  Note that -1st entry in each
   array is used for smtp host */
pthread_mutex_t	*q_lock;
pthread_cond_t  *q_wait;
qent		**q_head;
qent		**q_tail;

u_bit32		relocate_time;	/* last time users moved */
long		next_q_id;	/* next unused qid */

t_file 		*not_f;		/* connection to notification server */
struct sem	not_sem;	/* lock protecting it */

void queue_init ();
long next_qid ();
void queue_startup(int hostnum);
void wake_queuethread(int hostnum, long qid);
void queue_fname(char *fname, int hostnum, long id);
t_file *serv_connect(char *hostname);
boolean_t checkresponse(t_file *f, char *buf, int expect);
void xmit_block(t_file *conn, fileinfo *finfo, char *type);
void xmit_file(t_file *conn, t_file *f, char *type);
void xmit_encl(t_file *conn, enclinfo *ep);
