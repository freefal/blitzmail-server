/*  Mach BlitzMail Server -- DDP definitions
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/ddp.h,v 3.0 97/01/27 16:55:02 davidg Exp $

*/

#include "misc.h"

boolean_t m_noappletalk;	/* appletalk disabled? */

u_short my_atnet;		/* this machine's appletalk net number */
u_char my_atnode;		/* and node */
char my_atzone[256];		/* local zone name */
struct sockaddr_in gwaddr; 	/* gw address */

#define ZONENAME_MAX 32		/* max legal zone name length */

/* AT socket <-> UDP port mappings for DDP-in-UDP encapsulation.
   Note that there are different UDP port ranges for well-known (NBP, ZIP,...)
   and non-well-know (dynamic) Appletalk sockets. */


#define KIP_WKS_BASE 200	/* base of well-known sockets */
#define KIP_OLD_WKS_BASE 768	/* old gateways use this number (sigh) */
#define KIP_NWKS_BASE 16384	/* base of non-well-known sockets */
#define KIP_1ST_NWKS 128	/* first non-well-known socket */
#define KIP_LAST 254		/* last legal socket # */


#define DDPDATAMAX 	586
#define ALAPHDRLEN	3		/* alap header */
#define DDPHDRLEN	13		/* ddp header */
#define DDPREADMAX (DDPDATAMAX+ALAPHDRLEN+DDPHDRLEN)

/* DDP types */

#define DDP$NBP		2		/* name binding protocol */
#define DDP$ATP		3		/* Appletalk transaction protocol */
#define	DDP$ECHO	4
#define DDP$ZIP		6

/* well-known DDP sockets */

#define SOCK$NIS	2		/* names information socket */
#define	SOCK$ECHO	4
#define SOCK$ZIP	6		/* zone information protocol socket */

/* DDP broadcast node */
#define DDP_BROADCAST	255

/* DDP packet (including magic constant in ALAP header) as it appears when
   using UDP encapsulation */

struct ddpbuf {
	u_bit8  fill;		/* -- make DDP header aligned -- */
	u_bit8  alap_magic1;	/* ALAP  0  	- 0xFA */
	u_bit8  alap_magic2;	/* ALAP  1      - 0xCE */
	u_bit8 	alap_magic3;	/* ALAP  2  	- 0x02 */
				/* DDP header */
	char		len[2];		/* 	0:1	- hops, data len */
	char		cksum[2];	/*      2:3	- ddp checksum */
	char		dnet[2];	/* 	4:5	- destination net */
	char		snet[2];	/* 	6:7	- source net */
	u_bit8 dnode;			/* 	 8	- destination node */
	u_bit8 snode;			/* 	 9	- source node */
	u_bit8 dsock;			/* 	 A	- destination socket */
	u_bit8 ssock;			/* 	 B	- source socket */
	u_bit8 ddptype;			/*	 C	- ddp type */
	char ddpdata[DDPDATAMAX];	/*	D-	- ddp packet data */
			
};
typedef struct ddpbuf ddpbuf;
#define ALAP_MAGIC1 	0xFA
#define ALAP_MAGIC2	0xCE
#define ALAP_MAGIC3 	0x02

/* to skip fill byte */
#define DDPBUFDATA(x) ((char *) &(x->alap_magic1))

#define DDPLENMASK 0x03FF		/* length is low 10 bits */

struct ataddr {				/* appletalk address */
	u_bit16 	at_net;		/* note: host byte order */
	u_bit8 		at_node;
	u_bit8 		at_sock;
};

typedef struct ataddr ataddr;


/* ddp socket data allocated by ddpopen */


struct ddpsockinfo {
    u_bit8	socknum; 	/* AT socket number */
    int		udpsock; 	/* udp socket */
};

typedef struct ddpsockinfo ddpsockinfo,*ddpsockp;

ddpsockp ddpopen(int ddpsocknum);
boolean_t ddplock(ddpsockp ddpsock);
int ddpwrite(ddpsockp ddpsock, ddpbuf *bufp, int len, ataddr remoteaddr);
int ddpread(ddpsockp ddpsock,ddpbuf *bufp,ataddr *source);
void ddpclose (ddpsockp ddpsock);
boolean_t ddpinit();

