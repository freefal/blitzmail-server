/* definitions for clients of the notify server

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

 */

#define NOTDATAMAX	578		/* same as max ATP len, so far */
typedef char notifydat[NOTDATAMAX];	/* room for max length notification text */

#define NTYPE_CTL	0	/* notification control */
#define NTYPE_MAIL	1	/* BlitzMail notification */
#define NTYPE_BULL	2	/* Bulletin notification */
#define NTYPE_TALK	3	/* talkd gateway notification */

/* control messages */
#define NCTL_RESET "\0\0\0\001"	/* client needs to re-find notification server */
#define NCTL_RESET_LEN 	4
