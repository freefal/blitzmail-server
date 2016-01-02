/*  Mach BlitzMail Server -- client communication

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/client.h,v 3.6 98/10/21 15:59:03 davidg Exp Locker: davidg $

*/

/* status returns sent to client */

#define BLITZ_LASTLINE		"00 "
#define BLITZ_MOREDATA		"01 "
#define BLITZ_NODATA		"02 "
#define BLITZ_UNDEFMORE		"03 "
#define BLITZ_OK		"10 Ok."
#define BLITZ_OK_BLANK		"10 "
#define BLITZ_UNKNOWN		"11 Unknown command: "
#define BLITZ_BADARG		"12 Bad argument."
#define BLITZ_BADARG_BLANK	"12 "
#define BLITZ_MISSINGARG	"13 Missing argument."
#define BLITZ_ERROR		"14 Server error."
#define BLITZ_ERR_BLANK		"14 "
#define BLITZ_NO_SUPPORT	"15 "
#define BLITZ_NODND		"17 name server(dnd) not available"
#define BLITZ_NOTVAL		"21 user not validated yet"
#define BLITZ_NO_USER_YET	"22 user not specified yet"
#define BLITZ_ALREADY_VAL	"23 already validated"
#define BLITZ_NOMESSAGE		"24 no current message."
#define BLITZ_NORECIPIENTS	"25 no recipients specified."
#define BLITZ_TOOMANYRECIPS	"26 Too many recipients; the maximum allowed is 500."
#define BLITZ_TOOMANYENCLS	"27 Too many enclosures."
#define BLITZ_LOOP		"28 Loop in mailing list or forwarding address: "
#define BLITZ_LOOP_MORE		"29 Loop in mailing list or forwarding address: "
#define BLITZ_VALIDATED		"30 "
#define BLITZ_NOUSER		"31 no such user"
#define BLITZ_VALERR		"32 validation error"
#define BLITZ_KRB_VALERR	"32 kerberos error: "
#define BLITZ_ENCRYPT		"33 "
#define BLITZ_DUPLICATE		"34 Already logged on from "
#define BLITZ_AMBIG		"35 Ambiguous name"
#define BLITZ_WRONGSERV		"36 "
#define BLITZ_RECIP_OK		"40 "
#define BLITZ_NOSEND		"41 "
#define BLITZ_BADADDR		"42 "
#define BLITZ_RECIP_AMBIG	"43 "
#define BLITZ_RECIP_OK_MORE	"44 "
#define BLITZ_NOSEND_MORE	"45 "
#define BLITZ_BADADDR_MORE	"46 "
#define BLITZ_AMBIG_MORE	"47 "
#define BLITZ_RECIP_NODND	"48 "
#define BLITZ_NODND_MORE	"49 "
#define BLITZ_INTERMEDIATE	"50 "
#define BLITZ_NOWARN		"60 no more messages"
#define BLITZ_NEWMAIL		"61 new mail has arrived"
#define BLITZ_USERMSG		"62 "
#define BLITZ_SHUTDOWN		"63 shutdown impending"
#define BLITZ_INSERT		"66 "
#define BLITZ_BREAKACK		"70 Break acknowledged"

/* status returns sent to POP clients */
#define	POP_WELCOME		"+OK BlitzMail POP3 Server Ready."
#define POP_UNKNOWN		"-ERR Unknown command: "
#define	POP_OK			"+OK"
#define	POP_OK_BLANK		"+OK "
#define	POP_OK_SENDPASS		"+OK Please send PASS command."
#define	POP_VALIDATED		"+OK You are signed on as "
#define	POP_ERROR		"-ERR Server error"
#define POP_BADARG		"-ERR Bad argument."
#define POP_MISSINGARG		"-ERR Missing argument."
#define POP_NOTVAL		"-ERR User not validated yet."
#define POP_NO_USER_YET		"-ERR User not specified yet."
#define	POP_SEQERROR		"-ERR Can't give that command now."
#define POP_AMBIG		"-ERR User name is ambiguous."
#define	POP_NOUSER		"-ERR No such user in name directory."
#define	POP_NODND		"-ERR The name directory is not available."
#define	POP_VALERR		"-ERR Validation error."
#define	POP_WRONGSERV		"-ERR Wrong server, try "
#define	POP_BOXLOCKED		"-ERR Mailbox is already in use."
#define	POP_DELETED		"-ERR That message is already marked as deleted."
#define POP_KRBERR		"-ERR Kerberos error: "
#define	POP_ENDOFDATA		"."

/* various counters */
long	m_sent;			/* messages originated locally */
long	m_sent_vacation;	/* '' -- vacations */
long	m_sent_receipt;		/* '' -- receipts */
long	m_sent_bounce;		/* '' -- bounces */
long 	m_sent_internet;	/* '' -- sent to internet */
long 	m_sent_blitzsmtp;	/* '' -- sent to other servers */
long	m_recv_smtp;		/* incoming smtp messages */
long	m_recv_blitz;		/* incoming blitzmessages */
long 	m_delivered;		/* deliveries to local recipients */

long	u_num;			/* current # of connected users */
long	u_hwm;			/* high water mark today */
long	u_max;			/* absolute max # of connections */
long	u_timeout;		/* idle connection timeout (minutes) */
long	u_worry;		/* threshold for faster idle timeout */
pthread_cond_t usermax_wait;	/* wait here for u_num to go down */

char 	*u_warning;		/* warning to send at signon */

/*
    Buffer list structure (for buffering output when box is locked).
*/

#define BIG_BUFLEN 	10000
struct bufl {
	struct bufl	*next;		/* next buffer */
	int		used;		/* length in this one */
	char		data[BIG_BUFLEN];
};
typedef struct bufl bufl;

/*
    list of folder/message numbers (from parse_messlist)
*/

struct messlist {
	int	count;		/* number of elements */
	long	*foldnum;	/* folder... */
	long	*messid;	/*... and message ids */
	long	data[1];
	/* variable-sized arrays begin here */
};
typedef struct messlist messlist;

/*
    list of low-high byte ranges (from parse_rangelist)
*/
struct rangelist {
        int     count;          /* number of elements */
	long	*low;		/* low... */
	long	*high;		/* ...and high end */
        long    data[1];
        /* variable-sized arrays begin here */
};
typedef struct rangelist rangelist;
/* 
    Client command table.
*/

#define CMD_COUNT	65
#define POPCMD_COUNT	12
typedef void (*cmdfunc) (udb *user);

struct cmdent {
    boolean_t	val;		/* requires validation? */
    char	name[5];	/* command name */
    cmdfunc	func;		/* function to execute */
};
typedef struct cmdent cmdent;

cmdent	cmdtab[CMD_COUNT];	/* initialized in client.c */
cmdent	popcmdtab[POPCMD_COUNT];/* ditto */

void buf_flush(mbox *mb);
void buf_init(mbox *mb);
void buf_putc(mbox *mb, char c);
void buf_putl(mbox *mb, char *s);
void buf_putsta(mbox *mb, char *s);
void print(udb *user, char *s);
void print1(udb *user, char *s1, char *s2);
udb *user_alloc();
void free_user(udb *user);
any_t user_cmd(any_t user_);
any_t popuser_cmd(any_t user_);
void newmail_warn(mbox *mb, long messid, int foldnum);
void user_init();
void c_audt(udb *user);
void c_clea(udb *user);
void c_clem(udb *user);
void c_cler(udb *user);
void c_copy(udb *user);
void c_dele(udb *user);
void c_delx(udb *user);
void c_edat(udb *user);
void c_edel(udb *user);
void c_elis(udb *user);
void c_encl(udb *user);
void c_erem(udb *user);
void c_expr(udb *user);
void c_fdef(udb *user);
void c_flis(udb *user);
void c_fnam(udb *user);
void c_frem(udb *user);
void c_fsum(udb *user);
void c_head(udb *user);
void c_hide(udb *user);
void c_krb4(udb *user);
void c_ldat(udb *user);
void c_ldef(udb *user);
void c_list(udb *user);
void c_lrem(udb *user);
void c_lsts(udb *user);
void c_mark(udb *user);
void c_mcat(udb *user);
void c_mdat(udb *user);
void c_mess(udb *user);
void c_move(udb *user);
void c_msum(udb *user);
void c_noop(udb *user);
void c_pase(udb *user);
void c_pass(udb *user);
void c_pdef(udb *user);
void c_pref(udb *user);
void c_prem(udb *user);
void c_push(udb *user);
void c_quit(udb *user);
void c_rbcc(udb *user);
void c_rccc(udb *user);
void c_rcpt(udb *user);
void c_rpl2(udb *user);
void c_rtrn(udb *user);
void c_send(udb *user);
void c_size(udb *user);
void c_slog(udb *user);
void c_summ(udb *user);
void c_tdel(udb *user);
void c_text(udb *user);
void c_thqx(udb *user);
void c_topc(udb *user);
void c_trsh(udb *user);
void c_tsiz(udb *user);
void c_tsum(udb *user);
void c_udel(udb *user);
void c_udlx(udb *user);
void c_uid(udb *user);
void c_user(udb *user);
void c_vdat(udb *user);
void c_vers(udb *user);
void c_vrem(udb *user);
void c_vtxt(udb *user);
void c_warn(udb *user);
mbox *force_disconnect(long uid, int fs, udb *user);

void pop_dele(udb *user);
void pop_list_or_uidl(udb *user);
void pop_last(udb *user);
void pop_noop(udb *user);
void pop_pass(udb *user);
void pop_quit(udb *user);
void pop_retr_or_top(udb *user);
void pop_rset(udb *user);
void pop_stat(udb *user);
void pop_user(udb *user);

any_t poppassd(any_t conn_);
void record_login(u_bit32 addr);
boolean_t recent_login(u_bit32 addr);
