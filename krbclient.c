/*
 * simple_client.c
 *
 * Copyright 1989 by the Massachusetts Institute of Technology.
 *
 * For copying and distribution information, please see the file
 * <mit-copyright.h>.
 *
 */

#define SAMPLE_PORT 2151
#define SERVICE  "BlitzMail"
#define HOST "polvadera"

#define MSG "hello there"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <krb.h>
#include "port.h"

int main(int argc, char **argv)
{
	int sock, i;
	u_long cksum = 0L;		/* cksum not used */
	struct hostent *host;
	struct sockaddr_in s_sock;	/* server address */
	struct sockaddr_in c_sock;	/* client address */
	int	namelen;		/* and length */
    	char	realm[REALM_SZ];	/* local realm */
	char	buf[256];
	char	*hname;

	KTEXT_ST k;			/* Kerberos data */
	KTEXT ktxt = &k;

	/* for krb_mk_safe/priv */
	CREDENTIALS c;			/* ticket & session key */
	CREDENTIALS *cred = &c;
	long		len;

	/* for krb_mk_priv */
	des_key_schedule sched;		/* session key schedule */


	if (argc < 2)
		hname = HOST;		/* default host */
	else
		hname = argv[1];

	/* Look up server host */
	if ((host = gethostbyname(hname)) == (struct hostent *) 0) {
		fprintf(stderr, "%s: unknown host\n", HOST);
		exit(1);
	}

	/* Set server's address */
	memset((char *)&s_sock, 0, sizeof(s_sock));
	memcpy((char *)&s_sock.sin_addr, host->h_addr, host->h_length);

	s_sock.sin_family = AF_INET;
	s_sock.sin_port = ntohs(SAMPLE_PORT);

	/* Open a socket */
	if ((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
		perror("opening socket");
		exit(1);
	}
	
	/* connect to server */
	if (connect(sock, (struct sockaddr *)&s_sock, sizeof (s_sock)) < 0) {
	    perror("connect");
	    exit(1);
	}
	
	krb_get_lrealm(realm, 1);	/* get local realm */


	/* PREPARE KRB_MK_REQ MESSAGE */

	/* Get credentials for server, create krb_mk_req message */
	if ((i = krb_mk_req(ktxt, SERVICE, "", realm, cksum)) /* note: null instance */
		!= KSUCCESS) {
		fprintf(stderr, "%s\n", krb_get_err_text(i));
		exit(1);
	}
	printf("Got credentials for %s.\n", SERVICE);

	/* tell server to expect a ticket */
	sprintf(buf, "KRB4 %d\r\n", ktxt->length);
	i = write(sock, buf, strlen(buf)+1);
	if (i < 0)
		perror("sending krb4 command");
	
	i = read(sock, buf, sizeof(buf));
	printf("%s", buf);
	
	/* Send authentication info to server */
	i = write(sock, (char *)ktxt->dat, ktxt->length);
	if (i < 0)
		perror("sending credentials");
	printf("Sent authentication data: %d bytes\n", i);
	i = read(sock, buf, sizeof(buf));
	printf("%s", buf);
	
	return(0);
}
