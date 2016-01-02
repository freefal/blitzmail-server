/*  Mach BlitzMail Server -- message definitions
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/mess.h,v 3.3 98/02/03 14:18:19 davidg Exp Locker: davidg $
    
    All elements of a message (header, text, enclosures) are stored together
    in a single file.  All the messages belonging to a given user are kept
    in the "mess" subdirectory of that user's mailbox.  A message that had
    multiple recipients may have links pointing to it from several mailboxes;
    thus the message file does not have any recipient-specific attributes
    (such as "read" status, expiration date, etc.)
    
    The message file begins with a file header (not to be confused with the
    mail header) that gives the offset & length of the header and text.  If
    there are enclosures, they follow the text.
*/

#include "sem.h"

#define MESSFILE_MAGIC	0xBAAFBAAF
#define MESSFILE_VERS	0

/* File header as it appears on disk.

   Note: file header format changed after blitzserv3.0 to include
   message type. "vers" used to be a 32-bit quantity (== 1); 
   new format is 16-bit "vers" (== 0) followed by 16-bit type
   (MESSTYPE_BLITZ==1). This is a backwards-compatible change since
   old files are all MESSTYPE_BLITZ.
*/

struct filehead {			/* NOTE: network byte order */
	u_bit32		magic;		/* identifies this as Blitz message */
	u_bit32		verstype;	/* file format version (== 0) // type */
	bit32		headoff;	/* header offset/length */
	bit32		headlen;
	bit32		textoff;	/* text offset/length */
	bit32		textlen;
};
typedef struct filehead filehead;
#define FILEHEAD_LEN	24

/* macros to pack/unpack version & type from verstype field. Tricky because
   file is stored in network byte order & we're maintaining compatiability
   with old file format */
#define FH_VERS(x)	((ntohl(x) >> 16) & 0xFFFF)
#define FH_TYPE(x)	(ntohl(x) & 0xFFFF)
#define FH_VERSTYPE(x,y) (htonl(x << 16 | y))

/* note that the encl header format is exchanged among servers, so everyone
   must agree... */
#define ENCLSTR_LEN	64
struct enclhead {			/* enclosure header in file */
	u_bit32	encllen;		/* length of entire file */
	u_bit32 typelen;		/* (matches VPL1 "char(n) var") */
	char	type[ENCLSTR_LEN];	/* creator/type -- quoted */
	u_bit32 namelen;
	char	name[ENCLSTR_LEN];	/* mac filename -- not quoted */
};
typedef struct enclhead enclhead;
#define EHEAD_LEN 	140

/* during delivery, keep track of what filesystems have received a
   copy of the message so far: */
struct messinfo {
	long		messid;		/* message id */
	boolean_t	present[FILESYS_MAX]; /* copy present on this filesys? */
	fileinfo	finfo;		/* file in spool dir */
};

typedef struct messinfo messinfo;

#define MESS_DIR	"/mess/"	/* message subdirectory (within mailbox dir) */

long		next_mess_id;		/* next message id to use */
int		messid_f;		/* file recording value */
struct sem	messid_sem;		/* semaphore protecting it */
#define HEAD_MAXLINE	512		/* max header line we'll deal with */

#define BOUNDS_HDR "X-Part-Bounds"
#define MAX_BOUNDARY_LEN 70

#define MIME_MAX_NEST 20		/* max nesting depth we'll parse */

void clean_encl_list(enclinfo **elist);
void temp_finfo(fileinfo *finfo);
boolean_t mess_deliver(mbox *mb, messinfo *mi, long len, char *err);
void mess_done(messinfo *mi);
boolean_t mess_get(udb *user, folder **fold, long messid);
void mess_init();
void initialmess(mbox *mb, char *username, char *fname);
boolean_t mess_rem(mbox *mb, long messid, long len);
summinfo *mess_scan(mbox *mb, long messid);
summinfo *mess_scan_head(fileinfo *head, fileinfo *text, enclinfo *encl, long mtype);
boolean_t mess_copy_contenthead(fileinfo *head, t_file *outf);
boolean_t mess_setup(long messid, fileinfo *head, fileinfo *text, enclinfo *encl, messinfo *mi, int fs, long mtype);
boolean_t mess_open(char *name, fileinfo *head, fileinfo *text, enclinfo **encl, long *len, long *mtype);
u_long pick_expire(summinfo *summ);
void finfoclose(fileinfo *finfo);
long next_messid ();
long next_receipt ();
void mess_name(char *name, mbox *mb, long messid);
void mess_tmpname(char *name, int fs, long messid);
void mess_xfername(char *name, int fs, long messid, char *srchost);
boolean_t finfocopy(t_file *out, fileinfo *in);
