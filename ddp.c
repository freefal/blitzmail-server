/*  BlitzMail Server -- DDP
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/ddp.c,v 3.1 97/10/19 18:59:50 davidg Exp Locker: davidg $
    
    This is a simple DDP implementation using DDP-in-UDP encapsulation.

*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <netdb.h>
#include <errno.h>
#include <ctype.h>
#include <netinet/in.h>
#include <sys/param.h>
#include <sys/socket.h>
#include "ddp.h"
#include "zip.h"
#include "t_err.h"
#include "t_io.h"

/* externals */


struct in_addr 		ipaddr;			/* my ip address */
int			gw_wksbase = 0;		/* base udp port for well-known AT sockets */

static u_short ddpchecksum(ddpbuf *bufp, int pktl);
static any_t ddpinit_writer(any_t zot);

/* state vars shared by ddpinit & ddpinit_writer */
static ddpbuf		initpkt;		/* initial ZIP query */
static int		initpktl;		/* its length */
static ataddr		inittoaddr;		/* broadcast ZIP addr */
static ddpsockp		initsock;		/* socket for ZIP query */
static int		initthreads = 0;	/* init threads running */
static pthread_mutex_t 	initlock;		/* protects the counter & socket */
static boolean_t	initdone = FALSE;	/* all set? */

/* ddpinit --

    Determine our Appletalk address & the location of the DDP-in-UDP gateway
    to use.  This is determined dynamically by broadcasting a ZIP request
    (which the gateway will reply to.)  
    
    Some gateways (whose initials are "Fastpath") don't respond to broadcasts;
    to use them it's necessary to configure the gateways IP address statically
    with an ATGATEWAY line in the config file.  You can also use explicit
    ATNET and ATZONE commands to configure the net and zone statically.  We
    still try to locate the gateway, to figure out which port range it is using.

*/

boolean_t ddpinit() {

    char 		hostname[MAXHOSTNAMELEN]; /* local hostname */
    struct hostent	*localhost ; 	/* host entry for it */
    int			on = 1;		/* for setsockopt */
    getnetinfo		*z;		/* within it, the ZIP command */
    ddpbuf		resp;		/* response */
    int			respl;		/* & length */
    ataddr		fromaddr;	/* address reply came from */
    u_char		*b;
    int			myzonelen;
    u_short		zero = 0;	/* net number 0 for bcmp */
    pthread_t		thread;		/* writer thread */
    
    if (initdone) return TRUE;		/* already done */
		
    pthread_mutex_init(&initlock, pthread_mutexattr_default);
    initthreads = 1;
    
    sem_seize(&herrno_sem); /* gethostbyname isn't reentrant */

    if (gethostname(hostname, MAXHOSTNAMELEN) < 0)
	t_perror("gethostname");

    if ((localhost = gethostbyname(hostname)) == NULL) {
	t_errprint_l("ddpinit: localhost gethostbyname failed: %d",h_errno);
    } else {
	bcopy(localhost->h_addr, (char *)&ipaddr, localhost->h_length);
	my_atnode = ntohl(ipaddr.s_addr) & 0xFF; /* node = low byte of ip addr */
    }
    sem_release(&herrno_sem);

    /* if hostname lookup failed, or gave invalid result, error */
    if (my_atnode == 0 || my_atnode == DDP_BROADCAST) {
	t_errprint_l("ddpinit: invalid node #: %ld", my_atnode);
	return FALSE;
    }
        
    initsock = ddpopen(0);		/* get a socket (don't care what #) */
    
    /* enable broadcasts */
    if (setsockopt(initsock->udpsock, SOL_SOCKET, SO_BROADCAST, (char *) &on, sizeof(on)) < 0) {
	t_perror("ddpinit: SO_BROADCAST setsockopt");
	ddpclose(initsock);
	return FALSE;
    }

    inittoaddr.at_net = 0;		/* to local net */
    inittoaddr.at_node = DDP_BROADCAST;	/* send broadcast */
    inittoaddr.at_sock = SOCK$ZIP;	/* to ZIP socket */
    initpkt.ddptype = DDP$ZIP;		/* set DDP type */
					 					   
    z = (getnetinfo *) initpkt.ddpdata;	/* locate ZIP data */
    z->command = ZIP_GET_NET_INFO;	/* its a ZIP GetNetInfo request */
    z->flags = 0;			/* zero all the flags */
    bzero((char *) z->first, 2);	/* net#s mbz on a request */
    bzero((char *) z->last, 2);
    z->zlen = 0;			/* send null zone name, so we get default */
    initpktl = GETNETINFO_LEN;
    
    gwaddr.sin_family = AF_INET;	/* its an IP address */
    
    /* if gateway address not pre-configured, try broadcasting */
    if (gwaddr.sin_addr.s_addr == 0)
	gwaddr.sin_addr.s_addr = INADDR_BROADCAST; /* we want to broadcast */
    
    /* Loop forever retrying broadcast until we get a valid ZIP GetNetInfoReply
       Spawn a separate thread to send the packet periodically; this thread will
       loop reading responses. */

    initthreads++;			/* starting another thread */
    if (pthread_create(&thread, generic_attr,
                   (pthread_startroutine_t) ddpinit_writer, (pthread_addr_t) 0) < 0) {
	t_perror("ddpinit: pthread_create");
	return FALSE;
    }
    pthread_detach(&thread);
   
    for(;;) {				/* we must find a gw or we can't run! */
    	
	respl = ddpread(initsock, &resp, &fromaddr);
	z = (getnetinfo*) resp.ddpdata; /* lets take a look at the ZIP fields */
	if (resp.ddptype != DDP$ZIP) {	/* ignore unless ZIP */
	    t_errprint("ddpinit: reply not ZIP");
	    continue;
	} else if (z->command != ZIP_GET_NET_INFO_REPLY) { /* or not a reply */
	    t_errprint("ddpinit: reply not ZIP_GET_NET_INFO_REPLY");
	    continue;
	} else if (z->zlen != 0) {	/* our requests were for the null zone */
	    t_errprint("ddpinit: reply not for null zone");
	    continue;		/* retry broadcast and read */
	}
	break;				/* got the data we need */
    }
 
    initdone = TRUE;			/* for better or worse, we're done */
    
    pthread_mutex_lock(&initlock);
    if (--initthreads == 0)		/* last thread out of the building? */
	ddpclose(initsock);		/* done with socket now */
    pthread_mutex_unlock(&initlock);
 
    /* extract the info we need from ZIP GetNetInfoReply */

    if (!(z->flags & Z_ZINVALID)) {	/* trouble if  null zone was ok */
	t_errprint("ddpinit: invalid GetNetInfoReply (null zone name not invalid)!");
	return FALSE;
    }
   
    if (my_atnet == 0) {		/* ignore net # if we're preconfigured */
	if (bcmp((char *) z->first, (char *) z->last, 2) != 0 &&
	    bcmp((char *) z->last,  (char *) &zero, 2) != 0) { 
		t_errprint("ddpinit: invalid GetNetInfoReply (multiple net numbers)");
		return FALSE;
	}
			
	my_atnet = getnetshort(z->first);  /* get AT net# of our wire */		
    }
    
    if (strlen(my_atzone) == 0) {	/* ignore zone name if we already know */
	b = z->zname;			/* point to (null) zone name */
	b += *b + 1;			/* skip multicast address, if any */
	myzonelen = *b++;		/* get zone length */
	strncpy(my_atzone, (char *) b, myzonelen); /* copy zone name */
	my_atzone[myzonelen] = 0;	/* and terminate it */
    }
    
    /* set up gateway's address in "gwaddr" */
    if (gwaddr.sin_addr.s_addr == INADDR_BROADCAST) {
    	
	gwaddr.sin_addr.s_addr = 		/* gw IP address is same as ours... */
	    (ntohl(ipaddr.s_addr) & 0xFFFFFF00) 
	    + resp.snode; /* ...but with his node# in low byte */
	gwaddr.sin_addr.s_addr = htonl(gwaddr.sin_addr.s_addr); /* back into net order */
    }
    return TRUE;			/* DDP ready to go */
    
}
/* ddpinit_writer --

    This thread retransmits the ZIP request packet until we get an answer.

*/

any_t ddpinit_writer(any_t zot) {

    int		count = 0;
    
    while (!initdone) {				/* loop re-sending */
    	ddpwrite(initsock, &initpkt, initpktl, inittoaddr);
	if (initdone)			/* got an answer? */
	    break;
	sleep(5);			/* pause between retries */
	if (++count % 12 == 0)
	    t_errprint("No AppleTalk gateway located yet; still trying...");
    }
    
    pthread_mutex_lock(&initlock);
    if (--initthreads == 0)		/* last thread out of the building? */
	ddpclose(initsock);		/* done with socket now */
    pthread_mutex_unlock(&initlock);

    return 0;
}
/*
   ddpopen --

    Open udp socket (using either the caller-specified atalk socket
    number or a dynamically-chosen one).  Set up ddpsock data.
    Note that we don't do a "connect" to the configured gateway host:
    while it's true that we will want to send all our packets to the
    gateway we don't want to insist that everything come _from_ that
    particular host.
*/

ddpsockp ddpopen(int ddpsocknum) {

    ddpsockp ddpsock; 		/* returned: ddp socket info */
    struct sockaddr_in sin;	/* udp socket info */

    ddpsock = (ddpsockp) mallocf(sizeof (ddpsockinfo));
    
    ddpsock->udpsock = socket(AF_INET, SOCK_DGRAM, 0); /* get udp socket */
    if (ddpsock->udpsock < 0) {
	t_perror("ddpinit: socket");
	t_free((char *) ddpsock);
	return NULL;
    }

    sin.sin_family = AF_INET;
    sin.sin_addr.s_addr = 0;

    if (ddpsocknum > 0) {	/* if specific socket named */
    	if (gw_wksbase != 0)	/* if we know which range gw is using */
	    sin.sin_port = htons(ddpsocknum + gw_wksbase);
	else			/* if not - assume new range */
	    sin.sin_port = htons(ddpsocknum + KIP_WKS_BASE);
	if (bind(ddpsock->udpsock,(struct sockaddr *)&sin, sizeof (sin)) < 0) {
	    t_perror("ddpopen: bind");
	    free ((char *) ddpsock);
	    return NULL;
	}
    } else {		/* assign dynamic socket */
	for (ddpsocknum = KIP_1ST_NWKS; ddpsocknum <= KIP_LAST; ddpsocknum++) {
	    sin.sin_port = htons(ddpsocknum + KIP_NWKS_BASE);
	    if (bind(ddpsock->udpsock,(struct sockaddr *)&sin, sizeof (sin)) >= 0)
		break;
	    if (pthread_errno() != EADDRINUSE && pthread_errno() != EADDRNOTAVAIL) {
		t_perror("ddpopen: bind");
		t_free((char *) ddpsock);
		return NULL;
	    }
	}
	if (ddpsocknum > KIP_LAST) {
	    t_errprint("ddpopen: all sockets in use!");
	    t_free((char *) ddpsock);
	    return NULL;
	}
    }

    ddpsock->socknum = ddpsocknum; /* remember the socket number chosen */
    return ddpsock;
}


/*
    ddpclose --

    Close udp socket, free associated storage.   The caller is responsible
    for ensuring that no other threads are still trying to use the socket. 
*/

void ddpclose (ddpsockp ddpsock) {

    (void) close(ddpsock->udpsock);  	/* close the socket */
    t_free((char *) ddpsock);		/* finally, free the storage */

}

/*
    ddpread --

    Read ddp packet from socket.  Returns ddp data length (not including
    DDP & ALAP headers), or -1 if read failed.  The caller-supplied buffer
    must be large enough for a full-sized packet.

*/

int ddpread(ddpsockp ddpsock, ddpbuf *bufp, ataddr *source) {

    int cc; 		/* length received */ 
    struct sockaddr_in fromaddr;	
    int			fromlen;
    char		buf[256];
    u_short		cksum;
    
    
    for (;;) {				/* until good packet read */
    
	fromlen = sizeof(fromaddr);
	cc = recvfrom(ddpsock->udpsock, DDPBUFDATA(bufp), DDPREADMAX, 0,
		 (struct sockaddr *) &fromaddr, &fromlen);
	
	if (cc >= ALAPHDRLEN + DDPHDRLEN) {	/* if we got something */
	    if (bufp->alap_magic1 != ALAP_MAGIC1 || /* check ALAP header */
		bufp->alap_magic2 != ALAP_MAGIC2 ||
		bufp->alap_magic3 != ALAP_MAGIC3) {
		    t_errprint("ddpread: bad ALAP header!");
		    continue;
	    }

	
	    cc = getnetshort(bufp->len) & DDPLENMASK; 	/* ddp length */
	    
	    /* check DDP checksum */
	    if (getnetshort(bufp->cksum) != 0) {	/* don't bother if packet hasn't one */
		cksum = ddpchecksum(bufp, cc);
		if (getnetshort(bufp->cksum) != cksum) {
		    t_sprintf(buf, "ddpread: bad checksum from %d/%d/%d (is %d, should be %d)",
				    (int) getnetshort(bufp->snet), (int) bufp->snode, (int) bufp->ssock,
				    getnetshort(bufp->cksum), cksum);
		    t_errprint(buf);
		    continue;			/* read another */
		}
	    }
   
	    source->at_net = getnetshort(bufp->snet);  /* extract source AT address (in host order) */
	    source->at_node = bufp->snode;
	    source->at_sock = bufp->ssock;
	    
	    /* determine which socket<->port mapping gw is using, if we don't
	       yet know */
	    if (gw_wksbase == 0 && source->at_sock < KIP_1ST_NWKS) {
		if (ntohs(fromaddr.sin_port) < KIP_OLD_WKS_BASE)
		    gw_wksbase = KIP_WKS_BASE;
		else 
		    gw_wksbase = KIP_OLD_WKS_BASE;
	    }

	    return cc - DDPHDRLEN;		/* return data length */
	}	
	
	t_perror("ddpread");			/* recv failed */
	return -1;
    
    }
}

/*
    ddpwrite --

    Caller should already have filled in the ddp type.
    Set up source address, length, checksum, and ALAP magic header constant.
    
    If we haven't yet determined which UDP port range our gateway uses for
    well-known AT sockets, send packets to those sockets twice.
    
*/

int ddpwrite(ddpsockp ddpsock, ddpbuf *bufp, int len, ataddr remoteaddr) {

    int s;
    struct sockaddr_in gwport; 		/* gw udp socket to send to */
    int	i;

    bufp->alap_magic1 = ALAP_MAGIC1; 	/* set known constant */
    bufp->alap_magic2 = ALAP_MAGIC2;
    bufp->alap_magic3 = ALAP_MAGIC3;

    putnetshort(bufp->snet, my_atnet);	/* set our address */
    bufp->snode = my_atnode;
    bufp->ssock = ddpsock->socknum;

    putnetshort(bufp->dnet, remoteaddr.at_net); /* set up their address */
    bufp->dnode = remoteaddr.at_node;
    bufp->dsock = remoteaddr.at_sock;

    len += DDPHDRLEN;			/* length for ddp header */

    putnetshort(bufp->len, len);	/* set length, clear hop count */

    putnetshort(bufp->cksum, ddpchecksum(bufp, len)); /* compute checksum */
    len += ALAPHDRLEN;			/* now include ALAP header len too */

    /* Set udp port in the gateway that the encapsulated packet is
	directed to.  Use the mapped version of the ultimate destination socket. */
	
    gwport = gwaddr;			/* don't change global copy of addr! */

    if (bufp->dsock < KIP_1ST_NWKS) {	/* well-known socket? */
    	if (gw_wksbase != 0)		/* do we know how gateway wants it? */
	    gwport.sin_port = htons(bufp->dsock + gw_wksbase);
	else				/* if not, must try them both */
	    gwport.sin_port = htons(bufp->dsock + KIP_WKS_BASE);
    } else				/* no - these are easy */
	gwport.sin_port = htons(bufp->dsock + KIP_NWKS_BASE);
    
    for (i = 0; i < 2; ++i) {		/* may have to send twice */
	s = sendto(ddpsock->udpsock, DDPBUFDATA(bufp), len, 0,
				    (struct sockaddr *)&gwport, sizeof(gwport));    
	if (s < 0)
	    t_perror("ddpwrite sendto");
	    
	if (bufp->dsock >= KIP_1ST_NWKS || gw_wksbase != 0)
	    break;			/* no port # confusion - done */
	gwport.sin_port = htons(bufp->dsock + KIP_OLD_WKS_BASE); /* try again using old port #s */
    }

    return s;
}

/*
    ddpchecksum --
 
 Checksum a DDP packet. 
*/

static unsigned short ddpchecksum(ddpbuf *bufp, int len) {

	u_char	*b;				/* ptr into packet */
	long	cks;				/* the checksum */
	long	b16 = (1 << 16);		/* 0x00010000 */

	b = (u_char *) DDPBUFDATA(bufp);			
	b += ALAPHDRLEN;			/* locate DDP header */
	len -= 4; b += 4; 			/* we skip first 4 bytes of packet */
	cks = 0;				/* initialize checksum */
	
	while( --len >= 0 ) {			/* loop over each byte */
		cks += *b++;			/* add in next byte */
		cks <<= 1;			/* shift over 1 */
		if ( cks & b16 )		/* if we shifted a 1 out of 16-bit field */
		 	cks += 1;		/* rotate back in */
	}
	
	cks &= 0xFFFF;				/* done - get bottom 16 bits */
	if (cks == 0)				/* but if it computes to 0 */
	    cks = 0xFFFF;			/* return FFFF instead */
	    
	return	(u_short) cks;			/* return low-order 16 bits */
}

