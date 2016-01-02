/*  BlitzMail Server -- messages

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.


    Do consistency check on message file(s).
        
*/

#include "port.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <syslog.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "mess.h"

boolean_t mess_check(char *name, char *err);

int main(int argc, char **argv) {
	
    char	err[512];
    int		checked = 0;
    int		bad = 0;
    int		i;
    
    misc_init();
    t_errinit("checkmess", LOG_LOCAL1);
    t_ioinit();
    
    if (argc < 2) {
    	fprintf(stderr, "Usage: %s <filename>...\n");
	exit(1);
    }
    
    for (i = 1; i < argc; ++i) {
	++checked;
    	if (!mess_check(argv[i], err)) {
	    ++bad;
	    printf("%s: %s\n", argv[i], err);
	}
    } 
    printf("%d checked; %d bad\n", checked, bad);
    exit(0);

}

/* mess_check --

    Open message, read file header, set up "fileinfo" structures for header, text
    and any enclosures.
    
    Returns false if message file unavailable.
*/

boolean_t mess_check(char *name, char *err) {

    filehead	fh;			/* file header */
    t_file	*mess;			/* open message file */
    enclhead	eh;			/* enclosure header */
    long	pos, eof;		/* current file pos & eof */
    int		len;
        
    if ((mess = t_fopen(name, O_RDONLY, 0)) == NULL) {
	strcpy(err, strerror(pthread_errno()));
	goto BADMSG;
    }
    
    /* pick up file header */
    if ((len = t_fread(mess, (char *) &fh, FILEHEAD_LEN)) != FILEHEAD_LEN) {
	if (len < 0)
	    strcpy(err, "error reading file header");
	else
	    strcpy(err, "incomplete file header");
	goto BADMSG;
    }
    
    /* verify magic bytes & version number */
    if (ntohl(fh.magic) != MESSFILE_MAGIC || FH_VERS(fh.verstype) != MESSFILE_VERS) {
	strcpy(err, "not a Blitz message");
	goto BADMSG;
    }
    
     /* verify message type */
    if (FH_TYPE(fh.verstype) != MESSTYPE_BLITZ && FH_TYPE(fh.verstype) != MESSTYPE_RFC822) {
	sprintf(err, "Unknown message type: %d\n", FH_TYPE(fh.verstype));
	goto BADMSG;
    }
   
    /* if there's anything after text, we have enclosures */    
    pos = ntohl(fh.textoff) + ntohl(fh.textlen);
    eof = t_fseek(mess, 0, SEEK_END);		/* compute lof */
			
    if (pos > eof) {
	strcpy(err, "incomplete text");
	goto BADMSG;
    }
    
    while (pos < eof) {
    
	t_fseek(mess, pos, SEEK_SET);		/* start of next encl */
		
	if (pos + EHEAD_LEN > eof) {
	    strcpy(err, "incomplete enclhead");
	    goto BADMSG;
	}
	
	/* read enclosure header, copy info */
	if (t_fread(mess, (char *) &eh, EHEAD_LEN) != EHEAD_LEN) {
	    strcpy(err, "error reading enclhead");
	    goto BADMSG;	
	}

	pos += EHEAD_LEN + ntohl(eh.encllen);	/* compute where encl ends */
	
	if (pos > eof) {
	    strcpy(err, "incomplete enclosure");
	    goto BADMSG;
	}	
    }
    
    (void) t_fclose(mess);
    
    return TRUE;				/* ok! */
    
BADMSG:						/* trouble w/ message; clean up */
    if (mess != NULL)
	(void) t_fclose(mess);
	        
    return FALSE;
}
