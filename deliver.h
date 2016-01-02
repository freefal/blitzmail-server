/*  Mach BlitzMail Server -- delivery

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/deliver.h,v 3.3 98/02/03 14:08:07 davidg Exp Locker: davidg $
*/
/* fake uids that encode special delivery actions (all < 0) */

#define ALL_USERS		-42	/* broadcast to all users */
#define ALL_LOCAL_USERS		-43	/* all users on this server */
#define PUBML_UPDATE_REQ	-44	/* public mailing list update broadcast */
#define PUBML_UPDATE		-45	/* update for this server */

#define ALL_USERS_ADDR	"BlitzMail Users"

/* parameter block for alldel_thread */

struct alldel_pb {
    summinfo	summ;		/* summary info */
    messinfo	mi;		/* message info */
};
typedef struct alldel_pb alldel_pb;

/* Flags for intra-blitz spool control files */

#define F_NOFWD		0x01	/* disable forwarding for this recip */
#define F_HEXTEXT	0x02	/* binhex text enclosures? */

void blitzdeliver(char *sender, recip *rlist1, recip *rlist2, messinfo *mi,
            fileinfo *head, fileinfo *text, enclinfo *encl, summinfo *info,
	    boolean_t hextext);
void localdeliver(char *sender, recip *r, messinfo *mi, fileinfo *head, 
		  fileinfo *text, enclinfo *encl, summinfo *summ);
boolean_t deliver(udb *user, char *sender, recip *tolist, recip *cclist, recip *bcclist,
	    fileinfo *text, enclinfo *encl, summinfo *summ, boolean_t hextext, 
	    char *replyto, boolean_t hiderecips);
void internet(char *sender, recip *rlist1, recip *rlist2, fileinfo *head, 
		fileinfo *text, enclinfo *encl, summinfo *summ, boolean_t hextext);
void bad_mail(char *send_addr, char *recip_addr, fileinfo *head, fileinfo *text, summinfo *summ, char *reason); 
void do_bounces(char *sender, recip *rlist, fileinfo *head, fileinfo *text, summinfo *summ);
void do_receipt(char *recip_name, summinfo *summ, fileinfo *head);
void do_vacations(recip *rlist, fileinfo *head, summinfo *summ);
boolean_t localdeliver_one(char *sender, char *name, long uid, int fs, messinfo *mi, 
		      fileinfo *head, fileinfo *text, summinfo *summ);
void do_notify(long uid, int typ, long id, char *data, int len, boolean_t sticky);
t_file *notify_connect();
extern char *mac_char_map[256];		
