/*

	Filter to convert message in RFC822 format to
	blitzformat message.
	
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/makemess.c,v 3.1 97/10/19 19:03:34 davidg Exp Locker: davidg $

*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <syslog.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "client.h"
#include "config.h"
#include "mess.h"
#include "deliver.h"
#include "queue.h"

void doshutdown() {}

int main(int argc, char **argv) {

    fileinfo	tempf;
    int 	c;
    int 	prev = 0;
    filehead	fh;
    boolean_t	inheader = TRUE;
    t_file	*f;
 
    if (argc != 1) {
	fprintf(stderr, "Usage: %s < text_message > blitz_message\n", argv[0]);
	exit(1);
    }
    
    misc_init(); 			/* initialize global locks */
     
    t_ioinit();
    t_errinit("makemess", LOG_LOCAL1);	/* initialize error package */
    t_dndinit();		/* and dnd package */

    read_config();		/* read configuration file */

    mess_init();		/* initialize message code */
    mbox_init();		/* initialize mailbox code */
    user_init();		/* initialize client code */

    temp_finfo(&tempf);		/* get temp file */
    if ((f = t_fopen(tempf.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
	t_perror1("open", tempf.fname);
	exit(1);
    }
    
    fh.magic = htonl(MESSFILE_MAGIC);	/* file identifier */
    /* file format version // blitz (not 822) format */
    fh.verstype = FH_VERSTYPE(MESSFILE_VERS,MESSTYPE_BLITZ);
    fh.headoff = htonl(FILEHEAD_LEN);	/* message header follows file header */
    fh.headlen = fh.textlen = 0;	/* no header/text yet */
    
    /* copy to temp file, computing lengths & converting */
    while ((c = getchar()) != EOF) {
	if (c == '\n') {	/* end of header? */
	    c = '\r';		/* map lf -> cr */
	    if (inheader && prev == '\r') {
		inheader = FALSE;
		continue;	/* not part of header OR text */
	    }
	} 
	
	if (inheader)			/* (lengths in host order for now) */
	    ++fh.headlen;
	else
	    ++fh.textlen;
	    
	t_putc(f, c);
	
	prev = c;
    }
    
    fh.headlen = htonl(fh.headlen);	/* now, switch lengths to network order */
    fh.textlen = htonl(fh.textlen);
    
    /* text immediately follows header */
    fh.textoff = htonl(ntohl(fh.headoff) + ntohl(fh.headlen));     
    
    t_fseek(f, 0, SEEK_SET);
    fwrite(&fh, FILEHEAD_LEN, 1, stdout);	/* write file header */
    
    while ((c = t_getc(f)) != EOF)
	putchar(c);
	
    exit(0);
}
