/*
   Test NBP registration.

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

*/

#include "../port.h"
#include <stdlib.h>	
#include <stdio.h>
#include <syslog.h>
#include <servers/netname.h>
#include <netinet/in.h>
#ifdef NeXT
#include <sys/notify.h>
#else
#include <mach/notify.h>
#include <mach_init.h>
#endif
#include "../ddp.h"
#include "../t_err.h"
#include "../t_io.h"
#include "nbp.h"

port_t serv;

int main(int argc,char **argv) {

    kern_return_t r;
    ddpsockp testsock;

    t_errinit("nbptest", LOG_LOCAL1);	/* set up error package */
    if (!ddpinit())   			/* and ddp package */
	exit(1);
        
    if (argc != 3) {
	printf("Usage: nbptest <object> <type>\n");
	exit(1);
    }
    
    /* lookup, on local machine only */
    r = netname_look_up(name_server_port, "", NBP_SERVER_NAME, &serv);

    if (r != KERN_SUCCESS) {
	t_mach_error("NBP server not found",r);
	exit(1);
    }

    t_errprint_l("NBP port is %ld\n", serv);
     
    testsock = ddpopen(0); /* open socket, assign # dynamically */
	
    r = nameadd(serv, argv[1], argv[2], testsock->socknum, task_self());
    
    if (r != KERN_SUCCESS) {
	t_mach_error("nameadd",r);
    }
    
    t_errprint_ss("Name entered: %s:%s\n", argv[1], argv[2]);

    for(;;)
	sleep(999);
	
    exit(0);
}

void doshutdown() {}
