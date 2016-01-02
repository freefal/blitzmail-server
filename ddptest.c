/*
    Test DDP read&write.
	
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "ddp.h"
#include "config.h"
#include "t_io.h"
#include "t_err.h"

ddpsockp testsock;
int i,cc;
ddpbuf buf;
ataddr remoteaddr;
extern struct sockaddr_in 	gwaddr;
any_t respond(any_t zot);

void doshutdown() {}

int main () {

    pthread_t	thread;
    
    misc_init(); 			/* initialize global locks */
     
    t_ioinit();

    t_errinit("ddptest", LOG_LOCAL1); /* set up error package */
    read_config();		/* read configuration file */
    
    if (!ddpinit())         /* now, initialize ddp */
	exit(1);

    fprintf(stderr, "Gateway %s; Net %d; Zone: %s\n", 
    			inet_ntoa(gwaddr.sin_addr), my_atnet, my_atzone);
    
    testsock = ddpopen(0);

    fprintf(stderr, "socket is: %d/%d/%d\n", my_atnet, my_atnode, testsock->socknum);

    for(;;) {
	cc = ddpread(testsock,&buf,&remoteaddr);
	printf("read length: %d\n",cc);
	printf("remote addr: %o %o %o\n",
		remoteaddr.at_net,remoteaddr.at_node,remoteaddr.at_sock);
	printf("data: ");
	for (i = 0; i < cc; i++)
		putchar(buf.ddpdata[i]);
	putchar('\n');
	fflush(stdout);
		
	if (pthread_create(&thread, generic_attr,
			(pthread_startroutine_t) respond, (pthread_addr_t) 0) < 0) {
	    perror("pthread_create");
	    exit(1);
	}
	pthread_detach(&thread);
    }
}
	
any_t respond(any_t zot) {
    
    printf("ddpwrite: %d\n",ddpwrite(testsock,&buf,cc,remoteaddr));
    fflush(stdout);

    return 0;
}
