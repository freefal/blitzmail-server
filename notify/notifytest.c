/*
   Test notification.

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

*/

#include "../port.h"
#include <stdlib.h>
#include <stdio.h>
#include <syslog.h>
#include <netdb.h>
#include <ctype.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "../ddp.h"
#include "../t_err.h"
#include "../misc.h"
#include "../t_io.h"
#include "not_types.h"
#include "notify.h"

#define MAXSTR 256
static boolean_t checkresponse(t_file *f, char *buf, int expect);

extern char *optarg;		/* for getopt */
extern int optind;

int main(int argc, char **argv) {

    long	uid;
    long 	type;
    char	curdate[9];
    char	curtime[9];
    char	hostname[MAXSTR]; /* server hostname */
    char	note[MAXSTR];
    char	buf[MAXSTR];	/* response from server */
    long	len;
    t_file		f;	/* connection to notify server */
    int			sock;	/* corresponding socket */
    struct hostent *host;	/* host entry for server */
    struct sockaddr_in	sin;	/* its addr */
    struct servent	*sp;	/* services entry */
    boolean_t	err = FALSE;
    boolean_t	clear = FALSE;
    long	messid;
    int		c;

    misc_init();		/* initialize mutexes */        
    t_ioinit();			/* initialize io routines */
    t_errinit("notifytest", LOG_LOCAL1);
    
    strcpy(hostname, "localhost");	 /* set defaults */
    messid = time(NULL);
    
    while ((c = getopt(argc, argv, "ch:i:")) != EOF) {
    	switch(c) {
	    case 'c':		/* clear notificaton */
	    	clear = TRUE;
		break;
	    case 'h':		/* server hostname */
	    	strcpy(hostname, optarg);
		break;
	    case 'i':		/* specify messageid */
	    	strtonum(optarg, &messid);
		break;
	    default:
	    	err = TRUE;
		break;
	}
    }
    if (err || argc-optind < 1 || argc-optind > 2) {
	fprintf(stderr, "%s [-c] [-h <host>] [-i <messid>] <uid> [<type>]\n", argv[0]);
	exit(1);
    }
    
    uid = atoi(argv[optind++]);
    if (optind < argc) 
	type = atoi(argv[optind]);
    else
	type = 1;		/* default -- blitzmail */
        
    if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
	perror("notifytest: socket: ");
	exit(1);
    }
    /* if hostname begins with digit, treat as ip addr */
    if (isdigit(hostname[0])) {
	sin.sin_addr.s_addr = inet_addr(hostname);
	if (sin.sin_addr.s_addr == -1) {
	    fprintf(stderr, "notifytest: ill-formed ip addr: %s\n", hostname);
	exit(1);
	}
    } else {
	if ((host = gethostbyname(hostname)) == NULL) {
	    fprintf(stderr, "gethostbyname for %s fails: %ld\n", 
	    		hostname, h_errno);
	    exit(1);
	}
	bcopy(host->h_addr, (char *)&sin.sin_addr, host->h_length);
    }

    if ((sp = getservbyname(NOTIFYPORT, "tcp")) == NULL) {
	fprintf(stderr, "notifytest: unknown service: %s\n", NOTIFYPORT);
	exit(1);
    }    
    sin.sin_port = sp->s_port;	/* connect to notify port */

    sin.sin_family = AF_INET;	
    
    if (connect(sock, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
    	perror("notifytest: connect");
	exit(1);
    }

    t_fdopen(&f, sock);			/* set up file */
    
    if (!checkresponse(&f, buf, NOT_GREET)) {
	fprintf(stderr, "Unexpected greeting from server: %s\n");
	exit(1);
    }
    if (clear) {			/* clear sticky notification */
    	t_fprintf(&f, "CLEAR %ld,%ld\r\n", uid, type);
    } else {
	date_time(curdate,curtime);
    
	if (type == NTYPE_BULL) {
	    t_sprintf(note+1, "(%s %s) News flash!", curdate, curtime);
	    len = strlen(note+1);	 	/* compute len */
	    note[0] = len++; 		/* account for length byte */
	    note[++len] = 0;		/* null group name */
	} else if (type == NTYPE_CTL) {		/* control - send reset */
	    bcopy(NCTL_RESET, note, NCTL_RESET_LEN);
	    len = NCTL_RESET_LEN;
	} else {
	    t_sprintf(note+1, "(%s %s) You may have received mail!", curdate, curtime);
	    len = strlen(note+1);	 	/* compute len */
	    note[0] = len++; 		/* account for length byte */
	}
	   
	t_fprintf(&f, "NOTIFY %ld,%ld,%ld,%ld,%d\r\n",
		len, uid, type, messid, TRUE);
		
	t_fwrite(&f, note, len);	/* caution: may contain nulls! */
		
    }
    
    if (!checkresponse(&f, buf, NOT_OK)) {
    	fprintf(stderr, "%s\n", buf);
	exit(1);
    } else {
	if (type < 0) 
	    printf("Notification cleared.\n");
	else
	    printf("Notification sent.\n");
    }
    exit(0);
}

/* checkresponse -- 

    Read response, compare status code against what we expect.
    Reads one line, then flushes any other input (puts file back into
    writing state.)

*/
static boolean_t checkresponse(t_file *f, char *buf, int expect) {

    t_fseek(f, 0, SEEK_CUR);		/* make sure we're reading */
    t_gets(buf, MAXSTR, f);		/* get response */
    t_fflush(f);			/* set up to write again */
    
    return atoi(buf) == expect;
}
void doshutdown() {}
