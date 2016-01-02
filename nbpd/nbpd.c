/*
    NBP daemon

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    Accept rpc calls to register nbp names to other processes on the
    local machine.  Listen for incoming nbp lookups on the nbp socket,
    consult our name table & send replies.

    Also answer echo requests.
*/

#include "../port.h"
#include <stdlib.h>	
#include <stdio.h>
#include <ctype.h>
#include <syslog.h>
#include <netinet/in.h>
#include "../ddp.h"
#include "../t_err.h"
#include "../t_io.h"
#include "../misc.h"
#include "../config.h"
#include "../t_dnd.h"
#include "nbpd.h"

void sendreply(nbpname *n, ataddr replyaddr,u_char id);
void nbppkt(ddpbuf *pkt,int len);
void nbpregpkt(ddpbuf *pkt,int len);
boolean_t uccopy(char **p,char *out);
boolean_t nbpmatch(nbpname *n,char *obj,char *type);
void nbplisten (any_t unused);
static void echolisten (any_t unused);
void nbpdinit();
void abortsig();

/* globals */

ddpsockp nissock;		/* well-known nbp socket */
ddpbuf reqpkt;			/* request packet buffer */

/*
Main program

    Register our rpc port with the mach name server.  Start threads
    to handle the NBP socket, and the notification port; main thread 
    will loop handling rpc requests.

*/
int main()
{
    pthread_t	thread;
        
    nbpdinit();		/* do our initialization */

    /* start thread to handle the echo socket */
    if (pthread_create(&thread, pthread_attr_default,
                   (pthread_startroutine_t) echolisten, (pthread_addr_t) 0) < 0) {
	t_perror("echolisten pthread_create failed");
	exit(1);
    }		   
    pthread_detach(&thread);

    /* main thread handles NBP socket */
    nbplisten((pthread_addr_t) 0);

    exit(0);		/* notreached */
}
/*
    nbpdinit --

	One-time initialization.
*/
void nbpdinit () {

    char		logbuf[512];

    signal(SIGQUIT, abortsig);		/* generate clean dump on QUIT signal */

    misc_init();			/* set up global locks */
    t_ioinit();				/* set up io routines */
    t_errinit("nbpd", LOG_LOCAL1);	/* set up error package */
    
    t_dndinit();			/* init dnd (for config file) */
    read_config();			/* read config file (for gwaddr) */

    if (m_noappletalk) {		/* if AppleTalk disabled, nothing to do */
    	t_errprint("config file set for NOAPPLETALK; NBP server should not be run");
	t_errprint("sleeping...");
	for (;;)			/* don't exit, lest master process relaunch */
	    sleep(9999);
    }
    if (!ddpinit())			/* and set up ddp package */
	exit(1);

    pthread_mutex_init(&namelock, pthread_mutexattr_default);
    
    t_sprintf(logbuf, "NBP server up.  Gw %s; net %d; zone %s\n",
    		inet_ntoa(gwaddr.sin_addr), my_atnet, my_atzone);
    t_errprint(logbuf);
}
/*
    nameadd --

    Add name to table.
    
    Returns NBP_NAME_EXISTS if name is already in use.
*/

int nameadd(char *object, char *type, int sock) {

    nbpstr 		ucobj, uctype;
    nbpname 		*n;
    int 		i;
    kern_return_t	r;
    
    if (!strcmp(object,"=") || !strcmp(object,"="))
	    return NBP_INVALID_ARGUMENT; 	/* can't use wildcards here */

    if (strlen(object) > NBPMAX || strlen(type) > NBPMAX)
	return NBP_INVALID_ARGUMENT; /* too long for nbp? */

    /* generate uppercase versions for easy comparisons later */

    for(i = 0;
	    ucobj[i] = islower(object[i]) ? toupper(object[i]) : object[i];
	    i++)
    ;

    for(i = 0;
	    uctype[i] = islower(type[i]) ? toupper(type[i]) : type[i];
	    i++)
    ;

    pthread_mutex_lock(&namelock);	/* get access to the table */

    /* search name table for matches */
    for (n = nametab; n != NULL; n = n->link) {
	if (nbpmatch(n, ucobj, uctype)) /* if it matches */
	    break;			/* use this entry */
    }
    
    
    r = NBP_OK;				/* assume the best */

    if (n) {				/* duplicate? */
    	n->sock = sock;	        	/* yes - just switch registration to new skt */
	n->when = time(NULL);		/* and update timestamp */
    } else {		
	n = (nbpname *) mallocf(sizeof(nbpname));
	n->link = nametab; 		/* add to front of table */
	nametab = n;
	strcpy(n->object, object); 	/* copy name elements in */
	strcpy(n->type, type);
	strcpy(n->ucobj, ucobj);	/* ucase versions too */
	strcpy(n->uctype, uctype);		
	n->sock = sock; 		/* record the DDP socket # */
	n->when = time(NULL);		/* and time of registration */
    }
    
    pthread_mutex_unlock(&namelock);

    return r;
}
/*
    echolisten --

    Thread to handle the echo socket.  Loop receiving packets and
    responding to them.
*/

void echolisten (any_t unused) {

    int len;				/* packet length */
    ataddr remoteaddr;			/* packet source addr */
    ddpsockp echosock;			/* well-known nbp socket */
    ddpbuf echopkt;			/* request packet buffer */

    echosock = ddpopen(SOCK$ECHO);	/* get the socket */

    if (echosock == NULL) {
	t_errprint("Couldn't open echo socket; are we running as root?\n");
	exit(1);
    }


    for (;;) {		/* just loop processing packets */
	len = ddpread(echosock,&echopkt,&remoteaddr);
	if (len > 0) {	/* if we got one... */
	    switch(echopkt.ddptype) {	/* process it */
	    	case DDP$ECHO:
			if (echopkt.ddpdata[0] == ECHO_REQUEST) {
			    echopkt.ddpdata[0] = ECHO_REPLY;
			    (void) ddpwrite(echosock, &echopkt, len, remoteaddr);
			}
			break;
		default:		/* ignore unexpected DDP type */
			break;
	    }
	}
    }
}

/*
    nbplisten --

    Thread to handle the NIS.  Loop receiving packets and
    responding to them. Note that we use this same socket to
    handle registration requests
*/

void nbplisten (any_t unused) {

    int len;		/* packet length */
    ataddr remoteaddr;	/* packet source addr */

    nissock = ddpopen(SOCK$NIS);	/* get the socket */

    if (nissock == NULL) {
	t_errprint("Couldn't open NIS; are we running as root?\n");
	exit(1);
    }

    for (;;) {		/* just loop processing packets */
	len = ddpread(nissock,&reqpkt,&remoteaddr);
	if (len > 0) {	/* if we got one... */
	    switch(reqpkt.ddptype) {	/* process it */
	    	case DDP$NBP:
			nbppkt(&reqpkt,len); 
			break;
		case DDP$NBP_REG:
			nbpregpkt(&reqpkt,len); 
			break;
		default:		/* ignore unexpected DDP type */
			break;
	    }
	}
    }
}
/*
    nbppkt --
	
    Handle NBP packet received on NIS (socket 2); it should be an NBP
    lookup.  Search our name table, send responses for any matches
    we find.
*/

void nbppkt(ddpbuf *pkt,int len) {

    nbphdr *nhdr;		/* nbp header */
    ataddr replyaddr;		/* address to send reply to */
    char *p;			/* current spot in data */
    nbpstr obj, type, zone;	/* elements of nbp name */
    nbpname *n;			/* name table entry */


    nhdr = (nbphdr *) pkt->ddpdata; /* locate nbp header */

    if (NBP_TYPE(nhdr->typecount) != NBP$LKUP ||	/* verify nbp type */
	(getnetshort(pkt->dnet) && getnetshort(pkt->dnet) != my_atnet))	/* and dest net # */
	    return;			/* ignore stray packet */

    p = pkt->ddpdata + NBPHDRLEN;	/* locate lookup tuple */

    /* get address reply should go to */
    replyaddr.at_net  = getnetshort(p); p += 2;
    replyaddr.at_node = *((u_bit8 *) p); p++;
    replyaddr.at_sock = *((u_bit8 *) p); p++;
    
    p += 1; 				/* skip the enumerator */
    if (!uccopy(&p, obj))		/* copy object, uppercased */
	return;				/* (watching for bad strings...) */
    if (!uccopy(&p, type))		/* and type */
    	return;
    if (!uccopy(&p, zone))		/* and zone */
	return;
    if (strcasecmp(zone, my_atzone) != 0) /* if zone doesn't match */
	return;				/* lookup isn't for us; ignore */

    pthread_mutex_lock(&namelock);		/* table mustn't change just now... */

    /* search name table for matches */
    for (n = nametab; n != NULL; n = n->link) {
	if (nbpmatch(n, obj, type)) 	/* if it matches */
	    sendreply(n, replyaddr, nhdr->id); /* send reply packet */
    }
    
    pthread_mutex_unlock(&namelock);
}

/*
    nbpregpkt --
	
    Handle packet in our homemade NBP-register protocol. The format is identical
    to normal NBP packets, but with a different DDP type and NBP command code.
    
    We accept packets only from our own net/node.
*/

void nbpregpkt(ddpbuf *pkt,int len) {

    nbphdr *nhdr;		/* nbp header */
    ataddr regaddr;		/* address to register */
    char *p;			/* current spot in data */
    nbpstr obj, type, zone;	/* elements of nbp name */

    nhdr = (nbphdr *) pkt->ddpdata; /* locate nbp header */

    if (NBP_TYPE(nhdr->typecount) != NBP$REG ||	/* verify nbp type */
	(getnetshort(pkt->dnet) && getnetshort(pkt->dnet) != my_atnet) || /* and dest net # */
	(getnetshort(pkt->snet) && getnetshort(pkt->snet) != my_atnet) || /* and source net # */
	(pkt->snode != my_atnode)) 	/* and source node # */
	    return;			/* ignore stray packet */

    p = pkt->ddpdata + NBPHDRLEN;	/* locate registration tuple */

    /* get address being registered */
    regaddr.at_net  = getnetshort(p); p += 2;
    regaddr.at_node = *((u_bit8 *) p); p++;
    regaddr.at_sock = *((u_bit8 *) p); p++;

    p += 1; 				/* skip the enumerator */
    p = pstrcpy(obj, p);		/* copy object name */
    p = pstrcpy(type, p);		/* and type */
    p = pstrcpy(zone, p);		/* and zone */
    if (strcasecmp(zone, my_atzone) != 0) /* if zone doesn't match */
	return;				/* can't register in other zone */

    /* enter name into table */
    if (nameadd(obj, type, regaddr.at_sock) != NBP_OK) {
	t_errprint_ss("Error registering %s:%s", obj, type);
    }
    
}


/*
    uccopy --

    Copy nbp string (pascal format), uppercasing.  Advances pointer
    past end of string. Returns FALSE if string too long.
*/

boolean_t uccopy(char **p,char *out) {

	int i,l;

	l = *((*p)++);		/* get length byte */
	
	if (l <= 0 || l > NBPMAX)
	    return FALSE;	/* invalid string length */
	    
 	for (i = 0; i < l; i++)	/* copy & case map */
		out[i] = islower((*p)[i]) ? toupper((*p)[i]) : (*p)[i];

	out[i] = '\0';
	*p += l;		/* return pointer past end */
	
	return TRUE;		
}

/*
    nbpmatch --

    See if name in lookup packet matches an entry in our table
    (including wildcards).

*/

boolean_t nbpmatch(nbpname *n,char *obj,char *type) {

    if (strcmp(obj, "=") != 0 && strcmp(n->ucobj, obj) != 0)
	return FALSE; 			/* object doesn't match */

    if (strcmp(type, "=") != 0 && strcmp(n->uctype, type) != 0)
	return FALSE; 			/* type doesn't match */	

    return TRUE;			/* it matches */  	

}


void sendreply(nbpname *n, ataddr replyaddr,u_char id) {

    ddpbuf replypkt;		/* reply packet we send */
    nbphdr *replyhdr;		/* nbp reply header */
    char *p;
    int len;

    replypkt.ddptype = DDP$NBP;		/* set ddp type */

    replyhdr = (nbphdr *) replypkt.ddpdata;
    replyhdr->typecount = NBP_TYPECOUNT(NBP$REPLY, 1); /* reply; one name per packet */
    replyhdr->id = id;			/* return id they sent in request */

    p = replypkt.ddpdata + NBPHDRLEN;	/* next, construct reply tuple */
    p = putnetshort(p, my_atnet);	/* our net (static) */
    *p++ = my_atnode; 			/* our node (static) */
    *p++ = n->sock; 			/* socket that owns the name */
    *p++ = 1;				/* alias enumerator */

    /* ok, the name follows */
    *p = strlen(n->object);		/* nbp object name */
    strcpy(p+1,n->object);
    p += *p + 1;
    *p = strlen(n->type);		/* nbp type name */
    strcpy(p+1,n->type);
    p += *p + 1;
    *p++ = 1;
    *p++ = '*';				/* set zone name to '*' */

    len = p - replypkt.ddpdata;		/* data length */

    (void) ddpwrite(nissock,&replypkt,len,replyaddr); /* send the reply */

}

