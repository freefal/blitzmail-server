/*
    Connect to blitz server control port; execute command script.

*/

#include "port.h"
#include <netdb.h>
#include <stdio.h>
#include <errno.h>
#include <syslog.h>
#include <signal.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/time.h>
#include <netinet/in.h>

int blitz_login(int net, char *name, char *pw);
int prompt_namepw(char *name, char *pw);
int expect(int net, char *match);
int net_send(int net, char *data);
int netconnect(char *hostname, int *net);

int echo = FALSE;

int main(int argc,char **argv) {

    char 	*hostname;
    int		net = -1;
    char	name[BUFSIZ];
    char	pw[BUFSIZ];
    char	cmd[BUFSIZ+2];
    	
    if (argc != 2) {
	fprintf(stderr, "Usage: %s <host>\n", argv[0]);
	exit(1);
    }
    hostname = argv[1];
    
    if (!prompt_namepw(name, pw))
	exit(1);
    
    if (!netconnect(hostname, &net)) {	/* establish connection */
	exit(1);
    }
    if (!blitz_login(net, name, pw))	/* get validated */
	exit(1);
	
    for(;;) {		/* loop sending commands */
	if (!expect(net, "> "))
	    break;	/* look for prompt */
	    
	if (fgets(cmd, BUFSIZ, stdin) == NULL)
	    break;	/* end of input - done */
	    
	if (strlen(cmd) == 0)
	    continue;	/* skip blank lines in input */
	    	
	if (!net_send(net, cmd))
	    break;
	    
    }
    
    exit(0);
}

/* prompt for name & password (read from tty, not stdin).
   Note that trailing \n is included.
 */

int prompt_namepw(char *name, char *pw) {

    FILE	*tty;
    int		ok = TRUE;
    
    tty = fopen("/dev/tty", "r+");
    if (tty == NULL) {
    	perror("Cannot open /dev/tty");
	return FALSE;
    }
    
    fprintf(stderr, "Name: "); fflush(stderr);
    if (fgets(name, BUFSIZ, tty) == NULL) {
	ok = FALSE;
    } else {
	strcpy(pw, getpass("Password: "));
	strcat(pw, "\n");
    }
    fclose(tty);
    return ok;
}

int netconnect(char *hostname, int *net) {

    struct sockaddr_in sin;
    struct hostent *host;
    struct servent	*sp;	/* services entry */

    /* if hostname begins with digit, treat as ip addr */
    if (isdigit(hostname[0])) {
	sin.sin_addr.s_addr = inet_addr(hostname);
	if (sin.sin_addr.s_addr == -1) {
	    fprintf(stderr, "Ill-formed ip addr: %s\n", hostname);
	    return FALSE;
	}
    } else {
	if ((host = gethostbyname(hostname)) == NULL) {
	    fprintf(stderr, "Gethostbyname %s failed: %ld\n", hostname, h_errno);
	    return FALSE;
	}
	bcopy(host->h_addr, (char *)&sin.sin_addr, host->h_length);
    }
    
    sin.sin_family = AF_INET;
    if ((sp = getservbyname("blitzctl", "tcp")) == NULL) {
	fprintf(stderr, "unknown service: %s", "blitzctl");
	exit(1);
    }    
    sin.sin_port = sp->s_port;	/* control */
    
    *net = socket(AF_INET, SOCK_STREAM, 0);
    if (*net < 0) {
	perror("socket");
	return FALSE;
    }
    
    if (connect(*net, (struct sockaddr *)&sin, sizeof (sin)) < 0) {
	perror("connect");
	(void) close(*net);
	return FALSE;
    }

    return TRUE;	/* connected ok */
}

/* 
    Log in to blitz server. 
*/

int blitz_login(int net, char *name, char *pw) {
    

					
    if (!expect(net, "Name:"))	/* user name prompt should be next */
	return FALSE;
	
    if (!net_send(net, name))
	return FALSE;
    
   if (!expect(net, "Password:"))	/* password next */
	return FALSE;
	
    if (!net_send(net, pw))
	return FALSE;
	
    echo = TRUE;			/* start echoing to tty now */
    
    return TRUE;	/* should get greeting now */
}

int net_send(int net, char *data) {

    if (write(net, data, strlen(data)) < 0) {
	perror("write");
	return FALSE;
    }
    if (echo)
	fwrite(data, strlen(data), 1, stdout); fflush(stdout);
    return TRUE;
}
/* read from net until expected string seen
   Match string may not cross line boundaries.
   
   Everything read is echoed to stdout if echo is set.
*/

int expect(int net, char *match) {

    char	buf[BUFSIZ];	/* input buffer */
    char	linebuf[BUFSIZ+1]; /* broken into lines */
    char	*line;		/* current char of line */
    int		matched = FALSE;
    int		i,n;
        
    line = linebuf;	/* no input yet */
    
    /* loop until match  */
    while(!matched) {
	    
	/* read next chunk of input */
	n = read(net, buf, sizeof(buf));
	if (n < 0) {
	    perror("read");
	    return FALSE;
	} else if (n == 0)
	    return FALSE;	/* EOF */
	    
	/* echo to output */
	if (echo)
	    fwrite(buf, n, 1, stdout); fflush(stdout);
		
	/* break into lines, looking for match string */
	for (i = 0; i < n; ++i) {
	    if (buf[i] == '\r' || buf[i] == '\n') {
		line = linebuf;
		continue;	/* CR or LF starts new line */
	    }
	    *line++ = buf[i];	/* append to line */
	    *line = 0;
	    if (strcmp(match, line - strlen(match)) == 0) {
	    	matched = TRUE;
		break;		
	    }
	    if (line - linebuf > BUFSIZ)
	    	line = linebuf;	/* flush long line */
	}
	/* note that line may continue into next read... */
    }
        
    return matched;
}
