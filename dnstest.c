/*

	Test DNS locking.    

*/
#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <syslog.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "mess.h"

int	finished = 0;
pthread_cond_t finish_wait;

any_t lookthread(any_t _threadnum);

#define THREADCOUNT 100

int main (int argc, char **argv) {

    int		i;
    pthread_t	thread;
    	
    misc_init(); 			/* initialize global locks */
    pthread_cond_init(&finish_wait, pthread_condattr_default);
    
    t_ioinit();
    t_errinit("fopentest", LOG_LOCAL1);	/* initialize error package */
    
    t_errprint("Up");
        
    for (i = 0; i < THREADCOUNT; ++i) {
	if (pthread_create(&thread, generic_attr, 
			(pthread_startroutine_t) lookthread, (pthread_addr_t) i) < 0) {
	    perror("pthread_create");
	} else
	    pthread_detach(&thread);
    }
    pthread_mutex_lock(&syslog_lock);
    fprintf(stderr, "Threads spawned\n");
    pthread_mutex_unlock(&syslog_lock); 
    
    /* wait for all threads to finish */
    pthread_mutex_lock(&global_lock);
    while(finished < THREADCOUNT)
	pthread_cond_wait(&finish_wait, &global_lock);

    fprintf(stderr, "** %s:  Done.\n**\n", argv[0]);
    
    exit(0);
}

any_t lookthread(pthread_addr_t _threadnum) {

    int		threadnum;	
    int		i;
    struct hostent *hinfo;		/* remote host info */
    struct sockaddr_in remoteaddr;	/* client's address/port */
    char 	remotehost[256];
   
    threadnum = (long) _threadnum;		
   
    remoteaddr.sin_addr.s_addr = htonl(0x81AA1000 + threadnum);	
   
    pthread_mutex_lock(&syslog_lock);
    fprintf(stderr, "Thread %d; start\n", threadnum);
    pthread_mutex_unlock(&syslog_lock); 
        
    for (i = 0; i < 10; ++i) {
	sem_seize(&herrno_sem);		/* get access to gethostby[*] */
	hinfo = gethostbyaddr((char *) &remoteaddr.sin_addr,
					 sizeof(remoteaddr.sin_addr), AF_INET);
	if (hinfo)
	    strcpy(remotehost, hinfo->h_name); /* use name if possible */
	else {				/* just have addr; use that */
	    pthread_mutex_lock(&inet_ntoa_lock);	/* get access to inet_ntoa */
	    strcpy(remotehost, inet_ntoa(remoteaddr.sin_addr));
	    pthread_mutex_unlock(&inet_ntoa_lock);
	}
	sem_release(&herrno_sem);
    }
    pthread_mutex_lock(&syslog_lock);
    fprintf(stderr, "Thread %d; hostname is %s\n", threadnum, remotehost);
    pthread_mutex_unlock(&syslog_lock); 

    pthread_mutex_lock(&global_lock);	/* count threads that have finished */
    ++finished;
    pthread_mutex_unlock(&global_lock);
    pthread_cond_signal(&finish_wait);	/* wake main thread */

    pthread_mutex_lock(&syslog_lock);
    fprintf(stderr, "Thread %d; done\n", threadnum);
    pthread_mutex_unlock(&syslog_lock); 
    
    return 0;				/* thread fades away */
}

