/*	BlitzMail Server -- binhex.
	
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/binhex.c,v 3.1 97/10/19 18:55:51 davidg Exp Locker: davidg $
    
    Convert binhex format text to/from BlitzMail format enclosures.
    
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <fcntl.h>
#include <sys/types.h>
#include <netinet/in.h>
#include "t_io.h"
#include "config.h"
#include "t_err.h"
#include "misc.h"
#include "mbox.h"
#include "mess.h"
#include "smtp.h"
#include "binhex.h"

boolean_t debinhex(debinhex_state *bstate, char *line, char *errstr);
void debinhex_init(debinhex_state *bstate);
void debinhex_finish(debinhex_state *bstate);
void debinhex_backout(debinhex_state *bstate);
static short binhex_crc(unsigned char *p, long len, short seed);
static void rle_decode(debinhex_state *bstate, char *p, long len);
static boolean_t is_binhex_line(debinhex_state *bstate, char *p, int len);
static void eight2six(debinhex_state *bstate, u_char *in_p, long *len_p);
void machost_header(t_file *f, char *fname, finder_info *finder_stuff, u_bit32 datalen, u_bit32 rsrclen);
void machost_trailer (t_file *f);
t_file *get_machost_header(fileinfo *encl, machost **hdr, forkinfo *forks);
static boolean_t binhex_header(binhex_state *bstate, machost *hdr, forkinfo *forks, t_file *binhex_f);
static boolean_t binhex_fork(binhex_state *bstate, t_file *encl_f, long start, long len, t_file *binhex_f);
static void binhex_cleanup(binhex_state *bstate, t_file *binhex_f);
static void binhex_chunk(binhex_state *bstate, u_char *in_p, int in_len, t_file *binhex_f, boolean_t eof);
static void binhex_putc(binhex_state *bstate, t_file *binhex_f, u_char c);
static int rle_encode(binhex_state *bstate, u_char *in_p, int inlen, u_char *out_p, boolean_t eof);

/* xor table for crc calculation */

static unsigned int crctab[] = {
    0x0000,  0x1021,  0x2042,  0x3063,  0x4084,  0x50a5,  0x60c6,  0x70e7,
    0x8108,  0x9129,  0xa14a,  0xb16b,  0xc18c,  0xd1ad,  0xe1ce,  0xf1ef,
    0x1231,  0x0210,  0x3273,  0x2252,  0x52b5,  0x4294,  0x72f7,  0x62d6,
    0x9339,  0x8318,  0xb37b,  0xa35a,  0xd3bd,  0xc39c,  0xf3ff,  0xe3de,
    0x2462,  0x3443,  0x0420,  0x1401,  0x64e6,  0x74c7,  0x44a4,  0x5485,
    0xa56a,  0xb54b,  0x8528,  0x9509,  0xe5ee,  0xf5cf,  0xc5ac,  0xd58d,
    0x3653,  0x2672,  0x1611,  0x0630,  0x76d7,  0x66f6,  0x5695,  0x46b4,
    0xb75b,  0xa77a,  0x9719,  0x8738,  0xf7df,  0xe7fe,  0xd79d,  0xc7bc,
    0x48c4,  0x58e5,  0x6886,  0x78a7,  0x0840,  0x1861,  0x2802,  0x3823,
    0xc9cc,  0xd9ed,  0xe98e,  0xf9af,  0x8948,  0x9969,  0xa90a,  0xb92b,
    0x5af5,  0x4ad4,  0x7ab7,  0x6a96,  0x1a71,  0x0a50,  0x3a33,  0x2a12,
    0xdbfd,  0xcbdc,  0xfbbf,  0xeb9e,  0x9b79,  0x8b58,  0xbb3b,  0xab1a,
    0x6ca6,  0x7c87,  0x4ce4,  0x5cc5,  0x2c22,  0x3c03,  0x0c60,  0x1c41,
    0xedae,  0xfd8f,  0xcdec,  0xddcd,  0xad2a,  0xbd0b,  0x8d68,  0x9d49,
    0x7e97,  0x6eb6,  0x5ed5,  0x4ef4,  0x3e13,  0x2e32,  0x1e51,  0x0e70,
    0xff9f,  0xefbe,  0xdfdd,  0xcffc,  0xbf1b,  0xaf3a,  0x9f59,  0x8f78,
    0x9188,  0x81a9,  0xb1ca,  0xa1eb,  0xd10c,  0xc12d,  0xf14e,  0xe16f,
    0x1080,  0x00a1,  0x30c2,  0x20e3,  0x5004,  0x4025,  0x7046,  0x6067,
    0x83b9,  0x9398,  0xa3fb,  0xb3da,  0xc33d,  0xd31c,  0xe37f,  0xf35e,
    0x02b1,  0x1290,  0x22f3,  0x32d2,  0x4235,  0x5214,  0x6277,  0x7256,
    0xb5ea,  0xa5cb,  0x95a8,  0x8589,  0xf56e,  0xe54f,  0xd52c,  0xc50d,
    0x34e2,  0x24c3,  0x14a0,  0x0481,  0x7466,  0x6447,  0x5424,  0x4405,
    0xa7db,  0xb7fa,  0x8799,  0x97b8,  0xe75f,  0xf77e,  0xc71d,  0xd73c,
    0x26d3,  0x36f2,  0x0691,  0x16b0,  0x6657,  0x7676,  0x4615,  0x5634,
    0xd94c,  0xc96d,  0xf90e,  0xe92f,  0x99c8,  0x89e9,  0xb98a,  0xa9ab,
    0x5844,  0x4865,  0x7806,  0x6827,  0x18c0,  0x08e1,  0x3882,  0x28a3,
    0xcb7d,  0xdb5c,  0xeb3f,  0xfb1e,  0x8bf9,  0x9bd8,  0xabbb,  0xbb9a,
    0x4a75,  0x5a54,  0x6a37,  0x7a16,  0x0af1,  0x1ad0,  0x2ab3,  0x3a92,
    0xfd2e,  0xed0f,  0xdd6c,  0xcd4d,  0xbdaa,  0xad8b,  0x9de8,  0x8dc9,
    0x7c26,  0x6c07,  0x5c64,  0x4c45,  0x3ca2,  0x2c83,  0x1ce0,  0x0cc1,
    0xef1f,  0xff3e,  0xcf5d,  0xdf7c,  0xaf9b,  0xbfba,  0x8fd9,  0x9ff8,
    0x6e17,  0x7e36,  0x4e55,  0x5e74,  0x2e93,  0x3eb2,  0x0ed1,  0x1ef0
};


/* for binhex decoding */
#define	SIX_BIT_MAX	0x3F	/* the biggest # you can get in six bits */
#define	DONE 		0x7F	/* eight_to_six[':'] = DONE, since Binhex terminator is ':' */
#define	SKIP 		0x7E	/* eight_to_six['\n'|'\r'] = SKIP, i. e. end of line char.  */
#define	FAIL 		0x7D	/* character illegal in binhex file */

static unsigned char eight_to_six[] = 
{
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, SKIP, FAIL, FAIL, SKIP, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, FAIL, FAIL,
    0x0D, 0x0E, 0x0F, 0x10, 0x11, 0x12, 0x13, FAIL,
    0x14, 0x15, DONE, FAIL, FAIL, FAIL, FAIL, FAIL,
    0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1D,
    0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, 0x24, FAIL,
    0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, FAIL,
    0x2C, 0x2D, 0x2E, 0x2F, FAIL, FAIL, FAIL, FAIL,
    0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, FAIL,
    0x37, 0x38, 0x39, 0x3A, 0x3B, 0x3C, FAIL, FAIL,
    0x3D, 0x3E, 0x3F, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
    FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL, FAIL,
};

static unsigned char six_to_eight[64] = "!\"#$%&'()*+,-012345689@ABCDEFGHIJKLMNPQRSTUVXYZ[`abcdefhijklmpqr";

/* binhex2encl --

    Convert a text file with inline binhex into 1 or more Blitz-format enclosures, plus a text
    file for the non-binhex part of the original file.
*/

enclinfo *binhex2encl (t_file *textf, t_file *newtextf, char *errstr) {

    char		line[SMTP_MAXLINE];	/* current line of text file */
    debinhex_state 	bstate;		/* binhex state vars */
    
    strcpy(errstr, "");
    debinhex_init(&bstate);		/* set up initial state */
    
    /* read file, watching for binhex */
    (void) t_fseek(textf, 0, SEEK_SET);
    while (t_gets(line, sizeof(line), textf)) {
    	if (bstate.state == DEBINHEX_NOT_BINHEX) { /* not in binhex yet */
	    if (is_binhex_tag(line))	/* check for beginning */
		bstate.state = DEBINHEX_START;	/* yes - get ready to debinhex */
	    else			/* copy non-binhex to output */
	    	t_fprintf(newtextf, "%s\r", line);
	} else {			/* in middle of binhex */
  	    if (!is_binhex_line(&bstate, line, strlen(line))) {
	    	/* non-binhex in middle of binhex; what to do? */
		if (!isblankstr(line))	/* ignore extra blank lines within binhex */
		    t_fprintf(newtextf, "%s\r", line); /* copy it to output */
	    } else {
	    	if (!debinhex(&bstate, line, errstr)) {
	    	    debinhex_backout(&bstate);
		    return NULL;	/* bad binhex file; give up */
	        }
	    }
	}    
    }
    
    if (textf->t_errno < 0) { 		/* error on read? */
    	debinhex_backout(&bstate);	/* io error - give up */
	strcpy(errstr, "Error reading binhex file");
    } else if (bstate.state != DEBINHEX_NOT_BINHEX) {
    	debinhex_backout(&bstate);	
	strcpy(errstr, "Incomplete binhex file");    
    } else 
    	debinhex_finish(&bstate);	/* all done - clean up */
    
    return bstate.ehead;		/* return enclosure list */
}
/* debinhex_init --

    Set up binhex state variables at the beginning of a text file
    (which may contain 1 or more binhex files).  Reinitialization
    for the beginning of each binhex takes place in "debinhex".
*/

void debinhex_init(debinhex_state *bstate) {

    bstate->state = DEBINHEX_NOT_BINHEX;	/* no binhex seen yet */
    bstate->ehead = bstate->etail = NULL;	/* no enclosures produced */
    bstate->ef = NULL;
    bstate->rsrclen = bstate->datalen = 0;	/* no forks yet */

    /* set up empty rle buffer */
    bstate->rlemax = 512;			/* initial size (grown if needed) */
    bstate->rlebuf = mallocf(bstate->rlemax);
    bstate->rlep = bstate->rlebuf;
    bstate->rlelen = 0;		
}
/* debinhex_finish --

    Clean up after debinhexing.
*/

void debinhex_finish(debinhex_state *bstate) {

    if (bstate->ef) {			/* close any open encl file */
    	t_fclose(bstate->ef);
	bstate->ef = NULL;
    }
    
    t_free(bstate->rlebuf);		/* free storage */
}

/* debinhex_backout --

    Debinhexing has somehow failed; clean up any enclosure files we
    created.
*/

void debinhex_backout(debinhex_state *bstate) {

    debinhex_finish(bstate);		/* close files & free buffers */
    clean_encl_list(&bstate->ehead);	/* clean up all enclosures */
    
}

/* is_binhex_line --

    Test a line of text to see if it is valid binhex.
*/
static boolean_t is_binhex_line(debinhex_state *bstate, char *p, int len) {

    int		i;			/* array index */
    short	six_bits;		/* result of table lookup */
    
    /* check for leading colon if we're just starting to decode binhex */
    if (bstate->state == DEBINHEX_START && *p != ':') 
    	return FALSE;
    
    /* check for illegal characters */
    for (i = 0; i < len; p++, i++) {
	six_bits = eight_to_six[*(unsigned char *)p];
	if (six_bits <= SIX_BIT_MAX)	/* this one's legal */
	    continue;	
	else if (six_bits == DONE) {
	    /* colon is permitted in first & last (non-eol, space, period) columns */
	    if (i == 0)
		continue;
	    else {	/* colon marks end of binhex -- rest must be spaces or periods */
		for (p++, i++; i < len; p++, i++)
		    if ((*p != ' ') && (*p != '.'))
			return FALSE;
		return TRUE;
	    } 
	} /* DONE (colon) */
		
	else if ((six_bits == SKIP) && (i == len - 1))
	    continue;		/* CR is permitted in last position */
	else	/* no other excuse for being > SIX_BITS_MAX */
	    return FALSE;
    } 
    
    /* check line length (only the last line can be less than or more than the max) */
    return len == MAX_BINHEX_LINE_LEN;
    
} 
 
/* debinhex --

    Debinhex a hunk of text.  If this is the beginning of a new
    binhex file, create a new enclinfo.  Note that we may leave some
    stuff sitting in rlebuf until the next call, if the chunk ends
    at an awkward place.

    NOTE: we do the 8 bit to 6 bit conversion in place, to avoid memory allocation.

    NOTE: we assume that the first call will give us enough data to get a file name,
    type, and creator.  Assuming that file names adhere to the HFS limit of 31
    characters, we need to get 41 de-binhexed bytes in all.  Since the first line
    of a binhex file generates 47, we should be okay.
 */

boolean_t debinhex(debinhex_state *bstate, char *line, char *errstr) {
	
    enclinfo		*ep;			/* current enclosure */
    long		new_len;		/* length after 8->6 */
    int			i;			/* loop index */
    u_char		*limit_p;		/* ptr past last byte to do */
    short		test_crc;		/* for testing CRC */
    long		count;			/* length to write */
    u_char		*typcre;		/* type & creator bytes */
    char		*p;
    static char 	*hex = "0123456789ABCDEF";
    u_bit32		netlen;			/* length in network byte order */
    
	    
    /* starting new binhex file -- set initial state */
    if (bstate->state == DEBINHEX_START) {	
	if (line[0] != ':') {
	    strcpy(errstr, "Initial ':' missing");
	    return FALSE;			/* should begin with : */
	}
	bstate->state = DEBINHEX_GOT_COLON;
	bstate->partial_byte = 0;		/* reset state vars for new file */
	bstate->fract_bits = 0;
	bstate->crc = 0;
	bstate->saw_flag = bstate->data_good = FALSE;
	bstate->data_char = 0;
	bstate->rlep = bstate->rlebuf;		/* start with empty buffer */
	bstate->rlelen = 0;
	
	ep = mallocf(sizeof(enclinfo));		/* get a new enclosure node */
	ep->next = NULL;			/* it will go at end */
	if (bstate->ehead == NULL) {		/* first enclosure? */
	    bstate->ehead = bstate->etail = ep;		
	    temp_finfo(&ep->finfo);		/* get temp file */
	    if ((bstate->ef = t_fopen(ep->finfo.fname, O_RDWR | O_CREAT | O_TRUNC, FILE_ACC)) == NULL) {
		t_perror1("debinhex: cannot create", ep->finfo.fname);
		strcpy(errstr, "Error creating enclosure file");
		return FALSE;
	    }
	    ep->finfo.offset = 0;		/* first encl is at beginning of file */
	} else {				/* add to existing list */
	    bstate->etail->next = ep;
	    bstate->etail = ep;
	    ep->finfo = bstate->ehead->finfo;	/* put all encls in same file */
	    ep->finfo.temp = FALSE;		/* don't try to remove temp file more than once */
	    ep->finfo.offset = t_fseek(bstate->ef, 0, SEEK_END); /* next one begins here */
	}
	ep->finfo.len = 0;			/* empty so far */
    }

    ep = bstate->etail;				/* shorthand (always working on end) */
    
    /* squeeze eight-bit bytes to six */
    new_len = strlen(line);
    eight2six(bstate, (u_char *) line, &new_len);
    
    /* now do run-length de-coding into an expandable buffer */
    rle_decode(bstate, line, new_len);
    
    /* calculate end of valid data */
    limit_p = bstate->rlebuf + bstate->rlelen;
    
    /* run state machine */
    if (bstate->state == DEBINHEX_GOT_COLON) {
	    
	/* get file name (pstring format) */
	bcopy((char *) bstate->rlep + 1, (char *) ep->name, *(bstate->rlep));
	ep->name[*(bstate->rlep)] = 0;		/* terminate the string */
	if (strlen(ep->name) >= ENCLSTR_LEN)
	   ep->name[ENCLSTR_LEN-1] = 0;		/* enforce encl name length limit */
	   
	bstate->crc = binhex_crc(bstate->rlep, *(bstate->rlep) + 1, bstate->crc);
	bstate->rlep += *(bstate->rlep) + 1;
	
	/* skip version number */
	bstate->crc = binhex_crc(bstate->rlep, 1, bstate->crc);
	bstate->rlep++;

	/* get finder info */
	bcopy((char *) bstate->rlep, (char *) &bstate->finder_stuff, 10);
	bstate->finder_stuff.folder = 0;		/* zero parts that aren't in binhex header */
	bstate->finder_stuff.location = 0;
	bstate->crc = binhex_crc(bstate->rlep, 10, bstate->crc);
	bstate->rlep += 10;
		
	/* enclosure creator & type as 16 quoted hex chars */
	p = ep->type;
	*p++ = '"';
	typcre = (u_char *) bstate->finder_stuff.type;	/* type & creator are contiguous */
	for (i = 0; i < 8; ++i) {
	    *p++ = hex[(typcre[i] >> 4)];
	    *p++ = hex[typcre[i] & 0x0F];
	}
	*p++ = '"';
	*p++ = 0;
	
	/* new state */
	bstate->state = DEBINHEX_GOT_NAME;
    } 

    if (bstate->state == DEBINHEX_GOT_NAME) {
	/* if we don't have lengths and crc, punt until they arrive */
	if ((limit_p - bstate->rlep) < 10)
	    return TRUE;


	/* remember data fork len */
	bcopy((char *) bstate->rlep, (char *) &bstate->datalen, 4);
	bstate->datalen = ntohl(bstate->datalen);	/* convert to host byte order */
	bstate->crc = binhex_crc(bstate->rlep, 4, bstate->crc);
	bstate->rlep += 4;
		
	/* remember rsrc fork len */
	bcopy((char *) bstate->rlep, (char *) &bstate->rsrclen, 4);
	bstate->rsrclen = ntohl(bstate->rsrclen);	/* convert to host byte order */
	bstate->crc = binhex_crc(bstate->rlep, 4, bstate->crc);
	bstate->rlep += 4;
	
	/* check the crc */
	bcopy((char *) bstate->rlep, (char *) &test_crc, 2);
	test_crc = ntohs(test_crc);
	if (bstate->crc != test_crc) {
	    t_sprintf(errstr, "Header CRC is %ld; should be %ld.", bstate->crc, test_crc);
	    return FALSE;
	}
	bstate->crc = 0;
	bstate->rlep += 2;	    

	/* write machost header to the enclosure file */
	machost_header(bstate->ef, ep->name, &bstate->finder_stuff, bstate->datalen, bstate->rsrclen);

	netlen = htonl(bstate->datalen);		/* encl file uses network order */
	t_fwrite(bstate->ef, (char *) &netlen, 4);	/* write data fork length to encl */
		
	/* new state */
	bstate->state = DEBINHEX_GOT_HEADER;
    }
    
    if (bstate->state == DEBINHEX_GOT_HEADER) {
	/* reading data fork */
	count = limit_p - bstate->rlep;
	if (count > bstate->datalen)
	    count = bstate->datalen;
	if (bstate->datalen && count) {
	    t_fwrite(bstate->ef, (char *) bstate->rlep, count);
	    bstate->datalen -= count;
	    bstate->crc = binhex_crc(bstate->rlep, count, bstate->crc);
	    bstate->rlep += count;
	}

	/* next state? */
	if (bstate->datalen == 0 && (limit_p - bstate->rlep) >= 2) {
	    /* check the crc */
	    bcopy((char *) bstate->rlep, (char *) &test_crc, 2);
	    test_crc = ntohs(test_crc);
	    if (bstate->crc != test_crc) {
		t_sprintf(errstr, "Data CRC is %ld; should be %ld.", (long) bstate->crc, (long) test_crc);
		return FALSE;
	    }
	    bstate->crc = 0;
	    bstate->rlep += 2;

	    netlen = htonl(bstate->rsrclen);		/* encl file uses network order */
	    t_fwrite(bstate->ef, (char *) &netlen, 4);	/* write resource fork length to encl */
	    	    
	    bstate->state = DEBINHEX_GOT_DATA;
	} /* done with data */
    } /* if DEBINHEX_GOT_HEADER */
    
    if (bstate->state == DEBINHEX_GOT_DATA) {
	/* reading rsrc fork */
	count = limit_p - bstate->rlep;
	if (count > bstate->rsrclen)
	    count = bstate->rsrclen;
	if (bstate->rsrclen && count) {
	    t_fwrite(bstate->ef, (char *) bstate->rlep, count);
	    bstate->rsrclen -= count;
	    bstate->crc = binhex_crc(bstate->rlep, count, bstate->crc);
	    bstate->rlep += count;
	}

	/* done? */
	if (bstate->rsrclen == 0 && (limit_p - bstate->rlep) >= 2) {
	    /* check the crc */
	    bcopy((char *) bstate->rlep, (char *) &test_crc, 2);
	    test_crc = ntohs(test_crc);
	    if (bstate->crc != test_crc) {
		t_sprintf(errstr, "Resource CRC is %ld; should be %ld.", (long) bstate->crc, (long) test_crc);
		return FALSE;
	    }
	    bstate->crc = 0;
	    bstate->rlep += 2;
	    
	    machost_trailer(bstate->ef);		/* slap on the trailer */
	    ep->finfo.len = t_fseek(bstate->ef, 0, SEEK_END) - ep->finfo.offset; /* record enclosure length */
	    /* file remains open; next encl can be appended to it */
	    
	    /* all done; revert to non-binhex */
	    bstate->state = DEBINHEX_NOT_BINHEX;
	} /* done with data */
    } /* if DEBINHEX_GOT_DATA */
    
    /* if we consumed entire buffer (usual case); reset it to empty */
    if (bstate->rlep == limit_p) {
	bstate->rlep = bstate->rlebuf;
	bstate->rlelen = 0;
    }
    
    return TRUE;				/* so far so good... */
} 
/* binhex_crc --

    Compute the binhex-style CRC for the data pointed to by p, with the
    crc seeded to seed.

*/
static short binhex_crc(unsigned char *p, long len, short seed) {
 
    unsigned short	hold;			/* crc computed so far */
    long	i;				/* index into data */

    hold = seed;				/* start with seed */
    for (i = 0; i < len; i++, p++) {
	hold ^= (*p << 8);
	hold = (hold << 8) ^ crctab[(unsigned char)(hold >> 8)];
    }

    return hold;
}

/* rle_decode --
	
    Copy input to rlebuf, expanding run-length-encoding sequences.
    
    We automatically resize rlebuf if it is too small.  Note also
    that there may already be some data in rlebuf -- we just keep
    appending to the end (rlelen) indefinitely.  Whenever our caller
    gets around to cleaning out the entire buffer (rlebuf+rlelen == rlep),
    they should reset the buffer to the empty state.
	
*/
static void rle_decode(debinhex_state *bstate, char *p, long len) {

    u_char		*in_p;			/* for fetching characters */
    u_char		hold;			/* for holding input character */
    int			i;			/* loop index */
    int			j;			/* for repeating characters */
    int			offset;			/* pos of rlep within rlebuf */
    
#define OUTC(c) { \
		    if (bstate->rlelen >= bstate->rlemax) {\
		        offset = bstate->rlep - bstate->rlebuf; /* relocate rlep */\
			bstate->rlemax += 512;\
			bstate->rlebuf = reallocf(bstate->rlebuf, bstate->rlemax);\
			bstate->rlep = bstate->rlebuf + offset;\
		    }\
		    bstate->rlebuf[bstate->rlelen++] = c;\
		} 
        
    /* do the decoding */
    for (i = 0, in_p = (u_char *) p; i < len; i++, in_p++) {
	hold = *in_p;
	if (bstate->saw_flag && (hold == 0)) {  	/* an escaped flag character */
	    OUTC(REPEAT_FLAG);
	    bstate->data_char = REPEAT_FLAG;
	    bstate->data_good = TRUE;
	    bstate->saw_flag = FALSE;
	}
	else if (bstate->saw_flag && bstate->data_good) { 	/* repeat a character */
	    for (j = 0; j < hold - 1; j++)
		OUTC(bstate->data_char);
	    bstate->saw_flag = FALSE;
	    /* could say data_good = FALSE here, but to be conservative allow
	       additional repeat sequences without redefining data char */
	}
	else if (hold == REPEAT_FLAG) {		/* repeat flag */
	    bstate->saw_flag = TRUE;
	}
	else {					/* just a simple character */
	    OUTC(hold);
	    bstate->saw_flag = FALSE;
	    bstate->data_char = hold;
	    bstate->data_good = TRUE;
	}
    } 						/* for each eight-bit byte */
    
#undef OUTC
} 

/* eight2six --

    Convert a run of text in BinHex4's special character set to regular
    binary data, returning the new length in *len_p.  We keep track of
    partial bytes between calls with fract_bits and partial_byte.
*/
static void eight2six(debinhex_state *bstate, u_char *in_p, long *len_p) {

    u_char		*p, *out_p, *limit_p;
    u_char		hold;
    u_long		ul;
    
    /* get limit */
    limit_p = in_p + *len_p;
    
    /* convert to six bits and get rid of non-data chars */
    for (p = out_p = in_p; p < limit_p; p++)
	    if ((hold = eight_to_six[*p]) <= SIX_BIT_MAX)
		    *out_p++ = hold;
    limit_p = out_p;
		    
    /* take care of fract_bits */
    p = out_p = in_p;
    if ((bstate->fract_bits == 6) && (p < limit_p)) {
	    hold = *p++;
	    *out_p++ = (hold >> 4) | (bstate->partial_byte & 0xFC);
	    bstate->partial_byte = hold << 4;
	    bstate->fract_bits = 4;
    }
    if ((bstate->fract_bits == 4) && (p < limit_p)) {
	    hold = *p++;
	    *out_p++ = (hold >> 2) | (bstate->partial_byte & 0xF0);
	    bstate->partial_byte = hold << 6;
	    bstate->fract_bits = 2;
    }
    if ((bstate->fract_bits == 2) && (p < limit_p)) {
	    hold = *p++;
	    *out_p++ = hold | (bstate->partial_byte & 0xC0);
	    bstate->fract_bits = 0;
    }
    
    /* pack middle part */
    while ((limit_p - p) > 3) {			/* 4 6-bit bytes --> 3 8-bit bytes */
	ul = *p++;
	ul = (ul << 6) | *p++;
	ul = (ul << 6) | *p++;
	ul = (ul << 6) | *p++;
	out_p[2] = ul; ul = ul >> 8;		/* note: everything is unsigned */
	out_p[1] = ul; ul = ul >> 8;		/* so no masking is needed */
	out_p[0] = ul;
	out_p += 3;
    }
    
    /* take care of fract bits */
    if (p < limit_p) {
	    bstate->partial_byte = *p++ << 2;
	    bstate->fract_bits = 6;
    }
    if (p < limit_p) {
	    hold = *p++;
	    *out_p++ = (hold >> 4) | (bstate->partial_byte & 0xFC);
	    bstate->partial_byte = hold << 4;
	    bstate->fract_bits = 4;
    }
    if (p < limit_p) {
	    hold = *p++;
	    *out_p++ = (hold >> 2) | (bstate->partial_byte & 0xF0);
	    bstate->partial_byte = hold << 6;
	    bstate->fract_bits = 2;
    }
    
    /* return length */
    *len_p = out_p - in_p;
}
/* machost_header --

    Write machost header to enclosure file.

*/
void machost_header(t_file *f, char *fname, finder_info *finder_stuff, u_bit32 datalen, u_bit32 rsrclen) {

    machost	header;			/* the header we write (network byte order)*/

    header.vers = MACHOST_VERS;				/* current file format version */
    strncpy(header.type, "Mac", 3);
    header.space = htonl(((datalen + rsrclen + 256 + 1023) / 1024) * 1024);
    bcopy((char *) finder_stuff, (char *) header.finder_info, sizeof(header.finder_info));
    header.fname_len = strlen(fname);

    t_fwrite(f, (char *) &header, MACHOST_HDR_LEN); 	/* fixed-length header */
    t_fwrite(f, fname, strlen(fname));			/* followed by filename */

}

/* machost_trailer --

    Add machost fields that come after the resource fork.  These are mostly null.
    Fill in the current date for the creation/modification dates (this information
    isn't present in the binhex file).
*/
void machost_trailer (t_file *f) {

   struct {			/* (endian ok since all 0) */
    	u_bit32		creat_date;
	u_bit32		mod_date;
	u_char		attributes;
	u_char		vers;
	u_char		commentlen;
	char		eof[3];
    } trailer = { 0, 0, 0, 0, 0, { 'E', 'O', 'F' } };
#define MACHOST_TRAILER_LEN 14
    
    /* use current date/time in absence of anything better */
    trailer.creat_date = trailer.mod_date = htonl(mactime());
    
    t_fwrite(f, (char *) &trailer, MACHOST_TRAILER_LEN);
}

/* encl2binhex --

    Convert a Blitz-format enclosure list into binhex'd text
*/

boolean_t encl2binhex(enclinfo *encl, t_file *text_f) {

    enclinfo	*ep;		/* current enclosure */
    
    for (ep = encl; ep; ep = ep->next) {
	if (!binhex(ep, text_f, FALSE)) /* concatenate them all */
	    return FALSE;	/* bad enclosure file */
    }
    
    return TRUE;
}

/* binhex --

    Convert a Blitz-format enclosure list into binhex'd text.  Note that mac-style (\r)
    line terminators are used, unless the crlf flag is set (in which case we use
    CR-LF, \r\n).
*/

boolean_t binhex(enclinfo *encl, t_file *binhex_f, boolean_t crlf) {

    machost	*hdr = NULL;		/* machost header from encl */
    forkinfo	forks;			/* offset/len of each fork */
    t_file	*encl_f = NULL;		/* open enclosure file */
    binhex_state bstate;		/* binhexing state vars */
    boolean_t	ok = TRUE;
    
    /* set line-ending flag */
    bstate.crlf = crlf;
    
    /* parse machost header */
    if ((encl_f = get_machost_header(&encl->finfo, &hdr, &forks)) == NULL)
	ok = FALSE;
	
    if (ok) 
	ok = binhex_header(&bstate, hdr, &forks, binhex_f); /* write out binhex header */
       
    if (ok) 				/* add data fork */
	ok = binhex_fork(&bstate, encl_f, forks.datastart, forks.datalen, binhex_f);
    if (ok) 				/* add resource fork */
	ok = binhex_fork(&bstate, encl_f, forks.rsrcstart, forks.rsrclen, binhex_f);
	
    binhex_cleanup(&bstate, binhex_f);	/* free state vars */
    
    if (encl_f)
	t_fclose(encl_f);
    if (hdr)
	t_free(hdr);
    
    return ok;
}

/* get_machost_header --

    Read machost header info and fork lengths from enclosure file.  Note that the resource
    fork length is buried in the middle of the file (sigh).
*/
t_file *get_machost_header(fileinfo *finfo, machost **hdr, forkinfo *forks) {

    t_file	*f;			/* returned:  t_file for the enclosure */
    int		inlen;
    char 	*p;
    bit32	namelen;
    bit32	netlen;			/* to extract fork length */
    
    if ((f = t_fopen(finfo->fname, O_RDONLY, 0)) == NULL) {
    	t_perror1("get_machost_header: cannot open ", finfo->fname);
	return NULL;
    }
    (void) t_fseek(f, finfo->offset, SEEK_SET); /* seek to start of encl */

    inlen = MACHOST_HDR_LEN + 256 + 4;	/* enough for header & max-sized filename, data fork len */
    if (inlen > finfo->len)
	inlen = finfo->len;		/* watch out for very short enclosures */
    (*hdr) = mallocf(inlen);
    
    if (t_fread(f, (char *) (*hdr), inlen) != inlen) {	/* read header (network byte order) */
    	t_perror1("get_machost_header: can't read encl header ", finfo->fname);
	goto cleanup;
    }
    
    /* must be the version we understand */ 
    if ((*hdr)->vers != 0xFE || strncmp((*hdr)->type,"Mac",3) != 0) {    	
    	t_perror1("get_machost_header: enclosure not MacHost format ", finfo->fname);
	goto cleanup;
    }
    
    /* extract fork lengths, dealing with byte ordering */
    namelen = (*hdr)->fname_len;
    p = ((char *) &(*hdr)->fname_len) + namelen + 1; /* deal with varying-length filename */
    forks->datalen = getnetlong(p);		/* ...to get data fork length */
    
    /* location of forks within file */
    forks->datastart = finfo->offset + MACHOST_HDR_LEN + namelen + 4;
    forks->rsrcstart = forks->datastart + forks->datalen + 4;
    t_fseek(f, forks->rsrcstart - 4, SEEK_SET); /* seek to resource fork len */
    if (t_fread(f, (char *) &netlen, 4) != 4) {	/* read it (network byte order) */
    	t_perror1("get_machost_header: can't read rsrc len ", finfo->fname);
	goto cleanup;
    }
    forks->rsrclen = ntohl(netlen);

    return f;					/* return open encl file */
    
cleanup:					/* error - back out */
    if (f)
	(void) t_fclose(f);
    if (*hdr) {
	t_free(*hdr);
	*hdr = NULL;
    }
    return NULL;	
}
/* binhex_header --

    Initialize binhex state; write the binhex header to the file.
*/
static boolean_t binhex_header(binhex_state *bstate, machost *hdr, forkinfo *forks, t_file *binhex_f) {

    u_char	*binhexhdr = NULL;		/* buffer to hold binhex header */
    int		binhexhdrlen;			/* its length */
    u_char	*p;				/* current pos within it */
    long	netlen;				/* fork len in net byte order */
    short	crc = 0;
    boolean_t	ok = TRUE;
    
    bstate->partial_byte = bstate->fract_bits = 0; /* no partial char */
    bstate->last_char = 0; 
    bstate->char_count = -1;			/* no previous char for rle */
    bstate->ibuf = mallocf(BINHEX_IBUFLEN);	/* allocate conversion buffers */
    bstate->obuf = mallocf(BINHEX_OBUFLEN);
    bstate->colpos = 1;				/* starting at column 1 */
    
    if (bstate->crlf)
	t_fprintf(binhex_f, "%s\r\n\r\n", BINHEX_TAG);	/* identify start of binhex (+blank line) */
    else
	t_fprintf(binhex_f, "%s\r\r", BINHEX_TAG);	/* identify start of binhex (+blank line) */
    binhex_putc(bstate, binhex_f, ':');		/* first line begins with a colon */
    
    /* compute header length (with filename); allocate buffer */
    binhexhdrlen = BINHEX_HDR_LEN + hdr->fname_len;
    binhexhdr = mallocf(binhexhdrlen);
    p = binhexhdr;
    
    /* copy length byte & fname */
    bcopy((char *) &hdr->fname_len, (char *) p, hdr->fname_len + 1);
    p += hdr->fname_len + 1;
    
    *p++ = BINHEX_VERSION;			/* version number */
    
    /* note: finder_info already in network byte order */
    /* finder info (last 6 bytes not present in hqx) */
    bcopy((char *) hdr->finder_info, (char *) p, 10);		
    p += 10;
    
    netlen = htonl(forks->datalen);		/* data fork length */
    bcopy((char *) &netlen, (char *) p, 4);
    p += 4;

    netlen = htonl(forks->rsrclen);		/* resource fork length */
    bcopy((char *) &netlen, (char *) p, 4);
    p += 4;
    
    /* compute header crc */
    crc = binhex_crc(binhexhdr, binhexhdrlen - 2, crc);
    crc = htons(crc);
    bcopy((char *) &crc, (char *) p, 2);
    p += 2;
    	
    /* finally, binhex & write all that */
    binhex_chunk(bstate, binhexhdr, binhexhdrlen, binhex_f, FALSE);
    
    if (binhexhdr)
	t_free(binhexhdr);

    return ok;
}
/* binhex_fork --

    Write one fork of binhex file.  Use read/lseek directly to avoid an extra recopying of the
    data.
*/

static boolean_t binhex_fork(binhex_state *bstate, t_file *encl_f, long start, long inlen, t_file *binhex_f) {

    int		len;		/* length of current chunk */
    int		totallen = 0;	/* total length copied so far */
    boolean_t	ok = TRUE;
    short	crc = 0;	/* fork crc */

    t_fflush(encl_f);		/* flush buffers; we want the raw file */
    
    (void) lseek(encl_f->fd, start, SEEK_SET); /* seek to starting place */
    
    while(totallen < inlen) {
	len = BINHEX_IBUFLEN;		/* read a buffer full */
	if (len > inlen - totallen)	/* or until end of fork */
	    len = inlen - totallen;
	len = read(encl_f->fd, bstate->ibuf, len);
	if (len < 0) {
	    t_perror1("binhex_fork: error reading ", encl_f->name);
	    ok = FALSE;
	    break;
	}
	totallen += len;
	if (len == 0 && totallen < inlen) {
	    t_perror1("binhex_fork: Unexpected eof on ", encl_f->name);
	    ok = FALSE;
	    break;			/* don't spin */
	}
	
	crc = binhex_crc(bstate->ibuf, len, crc); /* compute crc for this piece */
	binhex_chunk(bstate, bstate->ibuf, len, binhex_f, FALSE); /* and binhex away */
    }
    
    if (ok) {
	crc = htons(crc);
	binhex_chunk(bstate, (u_char *) &crc, 2, binhex_f, FALSE);	/* add the crc */
    }
    
    return ok;
}
/* binhex_cleanup --

    Finish writing binhex file.  If there's a partial char left by six2eight, add one zero char
    to get those last bits out.  Add the binhex terminator, and free the conversion buffers.
*/

static void binhex_cleanup(binhex_state *bstate, t_file *binhex_f) {

    u_char	zero = 0;
    
    if (bstate->char_count > 1)		/* complete pending rle */
	binhex_chunk(bstate, &zero, 0, binhex_f, TRUE);
	
    if (bstate->fract_bits > 0) {	/* flush out partial last char */
	binhex_chunk(bstate, &zero, 1, binhex_f, TRUE);
    }
    
    binhex_putc(bstate, binhex_f, ':');	/* add the final colon */
    t_putc(binhex_f, '\r');
    if (bstate->crlf)
    	t_putc(binhex_f, '\n');
    
    t_free(bstate->ibuf); bstate->ibuf = NULL;
    t_free(bstate->obuf); bstate->obuf = NULL;

}

/* binhex_chunk --

    Convert and write one chunk of data to a binhex file; this is where the real work of generating
    binhex is done.  First, make a pass over the input buffer doing run-length encoding, then convert
    each six bits of input to 1 bixhex text char.  
    
    CRC's are computed by higher-level routines.
    
    Note: input buffer is overwritten
*/

static void binhex_chunk(binhex_state *bstate, u_char *in_p, int in_len, t_file *binhex_f, boolean_t eof) {

    int		len;			/* length of binhex'd text */
    u_char	*limit_p, *p;
    u_char	hold;
    u_long	ul;
    
    len = rle_encode(bstate, in_p, in_len, bstate->obuf, eof); /* binhex it, into our buffer */
     
    if (len == 0)			/* if rle generated nothing */
	return;				/* nothing to do */
	
    /* get limit */
    p = bstate->obuf;
    limit_p = p + len;
    
    if (bstate->fract_bits == 2) { /* deal with bits from last time */
	if (limit_p - p > 2) {	   /* 18 bits --> 3 chars */
	    ul = bstate->partial_byte;
	    ul = (ul << 8) | *p++;
	    ul = (ul << 8) | *p++;
	    binhex_putc(bstate, binhex_f, six_to_eight[(ul >> 12) & SIX_BIT_MAX]);
	    binhex_putc(bstate, binhex_f, six_to_eight[(ul >> 6) & SIX_BIT_MAX]);
	    binhex_putc(bstate, binhex_f, six_to_eight[ul & SIX_BIT_MAX]);
	    bstate->fract_bits = 0;
	}
    } else if (bstate->fract_bits == 4) {
    	if (limit_p - p > 1) {	/* 12 bits --> 2 chars */
	    ul = bstate->partial_byte;
	    ul = (ul << 8) | *p++;
	    binhex_putc(bstate, binhex_f, six_to_eight[(ul >> 6) & SIX_BIT_MAX]);
	    binhex_putc(bstate, binhex_f, six_to_eight[ul & SIX_BIT_MAX]);
	    bstate->fract_bits = 0;
	}
    }
    
    /* 3 chars --> 4 binhex'd chars */
    while (limit_p - p > 3) {
	ul = *p++;		/* pick up 24 bits */
	ul = (ul << 8) | *p++;
	ul = (ul << 8) | *p++;
	/* spit out 4 chars */
	binhex_putc(bstate, binhex_f, six_to_eight[(ul >> 18) & SIX_BIT_MAX]);
	binhex_putc(bstate, binhex_f, six_to_eight[(ul >> 12) & SIX_BIT_MAX]);
	binhex_putc(bstate, binhex_f, six_to_eight[(ul >> 6) & SIX_BIT_MAX]);
	binhex_putc(bstate, binhex_f, six_to_eight[ul & SIX_BIT_MAX]);
    }

    /* deal with last few chars 1 at a time */
    while (p < limit_p) {
	hold = *p++;
	if (bstate->fract_bits == 0) {		/* 8 bits */
	    bstate->partial_byte = hold & 0x03;	/* keep 2 bits */
	    bstate->fract_bits = 2;
	    binhex_putc(bstate, binhex_f, six_to_eight[hold >> 2]); /* and put six */
	} else if (bstate->fract_bits == 2) {	/* 10 bits */
	    ul = bstate->partial_byte;
	    ul = (ul << 8) | hold;
	    bstate->partial_byte = ul & 0x0F;	/* keep 4 bits */
	    bstate->fract_bits = 4;
	    ul = ul >> 4;
	    binhex_putc(bstate, binhex_f, six_to_eight[ul]); /* and put six */
	} else if (bstate->fract_bits == 4) {	/* 12 bits */
	    ul = bstate->partial_byte;
	    ul = (ul << 8) | hold;
	    binhex_putc(bstate, binhex_f, six_to_eight[(ul >> 6) & SIX_BIT_MAX]);
	    binhex_putc(bstate, binhex_f, six_to_eight[ul & SIX_BIT_MAX]);
	    bstate->fract_bits = 0;
	} else
	    abortsig();
    }
}

/* rle_encode --

    Do run-length-encoding.  If "eof" is set, flush out any in-progress rle.
*/

static int rle_encode(binhex_state *bstate, u_char *in_p, int inlen, u_char *out_p, boolean_t eof) {

    int		i;
    u_char	c;				/* next char */
    u_char	*out = out_p;			/* current pos in output */

    for (i = 0; i < inlen; ++i) {		/* process each binhex char */

	c = in_p[i];
	
	/* can we run-length encode here? */
	if (bstate->char_count > 0 && c == bstate->last_char && bstate->char_count < 0xFF)
	    ++bstate->char_count;	/* yes */
	else {				/* no - must output char now */
	    if (bstate->char_count > 1) { /* already did some rle? */
		*out++ = REPEAT_FLAG; /* yes - finish that up */
		*out++ = bstate->char_count;
	    }
	    *out++ = c;	/* put the char */
	    if (c == REPEAT_FLAG) {	/* flag char must be escaped */
		*out++ = 0x00;
		bstate->char_count = -1;/* can't do rle with this */
	    } else {
		bstate->char_count = 1;	/* reset repeat count */
		bstate->last_char = c;	/* remember last char used */
	    }
	}
    }
    
    if (eof && bstate->char_count > 1) { /* if rle at end of file */
	*out++ = REPEAT_FLAG; 		/* must finish it up now */
	*out++ = bstate->char_count;  
	bstate->char_count = -1;  
    }
    
    return out - out_p;
}

/* binhex_putc --

    Put 1 char, starting new line if needed.
*/

static void binhex_putc(binhex_state *bstate, t_file *binhex_f, u_char c) {

    t_putc(binhex_f, (char) c);
    if (++bstate->colpos > MAX_BINHEX_LINE_LEN) {
	t_putc(binhex_f, '\r');
	if (bstate->crlf)
	    t_putc(binhex_f, '\n');
	bstate->colpos = 1;
    }
}
