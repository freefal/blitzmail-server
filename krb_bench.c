/*
 * simple_client.c
 *
 * Copyright 1989 by the Massachusetts Institute of Technology.
 *
 * For copying and distribution information, please see the file
 * <mit-copyright.h>.
 *
 */

#define SAMPLE_PORT 1234
#define SERVICE  "BlitzMail"
#define HOST ""

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

	KTEXT_ST k;			/* Kerberos data */
	KTEXT ktxt = &k;

	/* for krb_mk_safe/priv */
	CREDENTIALS c;			/* ticket & session key */
	CREDENTIALS *cred = &c;
	long		len;

	/* for krb_mk_priv */
	des_key_schedule sched;		/* session key schedule */


	/* Look up server host */
	if ((host = gethostbyname("localhost")) == (struct hostent *) 0) {
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
	if ((i = krb_mk_req(ktxt, SERVICE, HOST, realm, cksum))
		!= KSUCCESS) {
		fprintf(stderr, "%s\n", krb_get_err_text(i));
		exit(1);
	}
	printf("Got credentials for %s.\n", SERVICE);

	/* Send authentication info to server */
	i = write(sock, (char *)ktxt->dat, ktxt->length);
	if (i < 0)
		perror("sending credentials");
	printf("Sent authentication data: %d bytes\n", i);

	sleep(1);	/* make sure separate message */
	
	/* Get session key */
	i = krb_get_cred(SERVICE, HOST, realm, cred);

	if (i != KSUCCESS) {
	    printf("krb_get_cred returned %d: %s\n", i, krb_get_err_text(i));
	    exit(1);
	}
	
	/* PREPARE KRB_MK_PRIV MESSAGE */

	/* Get key schedule for session key */
	des_key_sched(cred->session, sched);

	/* get our address for krb_mk_priv */
	namelen = sizeof(c_sock);
	if (getsockname(sock, (struct sockaddr *)&c_sock, &namelen) < 0) {
	    perror("getsockname");
	    exit(1);
	}

/*** benchmark encryption ***/
	{   int i;
	    char testbuf[1024] = "aaaaaabbbbbbbbbbbbbbcccccccccccc";

	    for (i = 0; i < 1024; ++i) {
	 	len = krb_mk_priv((u_char *) testbuf, ktxt->dat, sizeof(testbuf),
                          sched, &cred->session, &c_sock, &s_sock);
	    }
	}

	/* Make the encrypted message */
	len = krb_mk_priv((u_char *) MSG, ktxt->dat, strlen(MSG)+1,
			  sched, &cred->session, &c_sock, &s_sock);
	if (len < 0) {
	    printf("krb_mk_priv returned %d: %s\n", len);
	    exit(1);
	}

	/* Send it */
	i = write(sock, (char *)ktxt->dat, (int) len);
	if (i < 0)
		perror("sending encrypted message");
	printf("Sent encrypted message: %d bytes\n", i);
	
	return(0);
}
