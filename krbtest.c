
/*

    Copyright (c) 1996 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    Test kerberos validation.
	
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/dir.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <syslog.h>
#include <krb.h>
#include "t_io.h"
#include "t_err.h"
#include "t_dnd.h"
#include "misc.h"
#include "config.h"

#define SAMPLE_PORT 1234

int accept_conn();
static char *val_farray[] = { "NAME", "UID", "GID", "DUP",
			    "BLITZSERV", "BLITZINFO", "EXPIRES", NULL };

struct sockaddr_in 	sin;	/* server address */
struct sockaddr_in  remoteaddr; /* client addr */

int main(int argc, char **argv) {

    KTEXT_ST	authent_buf;
    KTEXT 	authent = &authent_buf;	/* kerberos authenticator from client */
    AUTH_DAT 	ad;			/* validated authentication data */
    KTEXT_ST k;
    KTEXT ktxt = &k;			/* encrypted message */
    MSG_DAT 	msg_data;		/* decrypted message */
    int		fd;
    int		krb_stat;
    u_long	from_addr;
    char	realm[REALM_SZ];	/* local realm */
    int		i;
    int		dndstat;  		/* dnd lookup status */
    dndresult	*dndres;		/* result array */

    /* for krb_rd_priv */
    des_key_schedule sched;		/* session key schedule */
    int		namelen;
    
    misc_init();				/* set up global locks */
    t_ioinit();
    t_errinit("krbtest", LOG_LOCAL1); /* initialize error handling */
    t_dndinit();		/* and dnd package */
    
    read_config();		/* read configuration file */

    fd = accept_conn(&from_addr);	/* get connection */

    i = read(fd, (char *)authent->dat, MAX_KTXT_LEN);
    if (i < 0) {
    	perror("read");
	exit(1);
    } else {
	printf("received %d bytes from %s\n", i, inet_ntoa(remoteaddr.sin_addr));
	authent->length = i;
    }

    sem_seize(&krb_sem);	/* get access to kerberos library */
    krb_set_key(priv_pw, 1);	/* set the service key */
    /* decrypt & verify the ticket */
    krb_stat = krb_rd_req(authent, priv_name, "", from_addr, &ad, NULL);
    
    if (krb_stat != KSUCCESS) {
	
	printf("Trouble: %s\n",krb_get_err_text(krb_stat)); /*** w/ semaphore still seized */
	exit(1);
    }
    
    sem_release(&krb_sem);
    
    krb_get_lrealm(realm, 1);
    
    if (strcmp(ad.prealm, realm) != 0) {
    	printf("wrong realm\n");
	exit(1);
    }
    
    if (strlen(ad.pinst) != 0) {
    	printf("not the null instance\n");
	exit(1);
    }
    printf("Ticket from: %s.%s@%s\n", ad.pname, ad.pinst, ad.prealm);

    /* get name from ticket; look it up in DND for other info */
    dndstat = t_dndlookup1(ad.pname, val_farray, &dndres);

    if (dndstat != DND_OK) {		/* did it work? */
    	printf("DND lookup failed: %d\n", dndstat);
	t_free(dndres);
	exit(1);
    }
    
    printf("%s; UID: %s; BlitzServ: %s\n", t_dndvalue(dndres, "NAME", val_farray),
    					t_dndvalue(dndres, "UID", val_farray),
					t_dndvalue(dndres, "BLITZSERV", val_farray));
					
    t_free(dndres);
 
    /* NOW GET ENCRYPTED MESSAGE */
    
    /* need key schedule for session key */
    des_key_sched(ad.session, sched);

    i = read(fd, (char *)ktxt->dat, MAX_KTXT_LEN);
    if (i < 0) {
    	perror("read");
	exit(1);
    }
    printf("received %d bytes from %s\n", i, inet_ntoa(remoteaddr.sin_addr));

    /* get our address for krb_rd_priv */
    namelen = sizeof(sin);
    if (getsockname(fd, (struct sockaddr *)&sin, &namelen) < 0) {
	perror("getsockname");
	exit(1);
    }

    i = krb_rd_priv(ktxt->dat, i, sched, &ad.session, &remoteaddr,
		    &sin, &msg_data);
    if (i != KSUCCESS) {
	    fprintf(stderr, "%s\n", krb_get_err_text(i));
	    exit(1);
    }
    printf("Decrypted message is: %s\n", msg_data.app_data);
    
    exit(0);
}
    
int accept_conn(u_long *from_addr) {
    
    int		connfd;		/* returned: open connection */
    int		fd;		/* socket we listen on */
    int		len = sizeof(sin); /* addr length */
    int         on = 1; /* for setsockopt */

    
    sin.sin_family = AF_INET;	
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = ntohs(SAMPLE_PORT);	/* port we listen on */
    
    if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	perror("socket: ");
	exit(1);
    }   
        
    /* set REUSEADDR so we won't get an EADDRINUSE if there are connections
       lingering in TIME_WAIT */
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &on, sizeof(on)) < 0
)
        perror("setsockopt (SO_REUSEADDR)");

    if (bind(fd, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	perror("bind");
	exit(1);
    }

    listen(fd, 5);		/* accept connections (up to 5 in advance) */
     
	
    connfd = accept(fd, (struct sockaddr *) &remoteaddr, &len);
    
    if (connfd < 0) {
	perror("accept: ");
	exit(1);
    }
    
    *from_addr = sin.sin_addr.s_addr;
    return connfd;
	
}
