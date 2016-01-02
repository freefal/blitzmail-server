/*  Mach BlitzMail Server -- binhex definitions
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/binhex.h,v 3.0 97/01/27 16:50:18 davidg Exp $
*/

#ifndef _BINHEX_H
#define _BINHEX_H

#define BINHEX_TAG "(This file must be converted with BinHex 4.0)"
/* macro to detect beginning of binhex file */
#define is_binhex_tag(s) (strncmp(s, BINHEX_TAG, 45) == 0)

struct finder_info {				/* Mac finder info (network byte order!) */
	char	type[4];			/* file type */
	char	creator[4];			/* and creator */
	u_bit16	flags;				/* finder flags */
	u_bit32	location;			/* location on desk -- not in binhex header */
	u_bit16	folder;				/* parent folder -- not in binhex header */
};
typedef struct finder_info finder_info;

#define REPEAT_FLAG	0x90	/* repeat char for run-length-encoding */

struct debinhex_state {		/* all the state associated with expanding binhex */    
    int		state;		/* DEBINHEX_* state */
    enclinfo	*ehead;		/* enclosures extracted from this binhex file */
    enclinfo	*etail;		/* .  .  . */
    t_file	*ef;		/* current enclosure file */
    u_bit32	rsrclen;	/* remaining resource fork length */
    u_bit32	datalen;	/*     ''      data    ''    ''   */
    finder_info	finder_stuff;	/* for setting finder info */
    bit16	crc;		/* current crc */
    u_char	partial_byte;	/* eight2six - partial byte */
    u_char	fract_bits;	/* eight2six - #of bits in partial_byte */
    boolean_t	saw_flag;	/* rle - last char was flag */
    boolean_t	data_good;	/* rle - had valid data char */
    u_char	data_char;	/* rle - last data char */
    u_char	*rlebuf;	/* rle - buffer for rle expansion */
    u_char	*rlep;		/* rle - current pos for reading buffer */
    long	rlelen;		/* rle - valid length in buffer */
    long	rlemax;		/* rle - buffer's allocated size */
};
typedef struct debinhex_state debinhex_state;

/* values for binhex_state.state */
#define DEBINHEX_NOT_BINHEX	0
#define	DEBINHEX_START		1
#define DEBINHEX_GOT_COLON	2
#define	DEBINHEX_GOT_NAME	3
#define	DEBINHEX_GOT_HEADER	4
#define	DEBINHEX_GOT_DATA	5

#define BINHEX_IBUFLEN	1024			/* input buffer size to use */
#define BINHEX_OBUFLEN  (BINHEX_IBUFLEN*2)	/* output buffer (twice as big for worst-case rle) */

struct binhex_state {		/* conversely, the state associated with creating binhex */    
    u_char	partial_byte;	/* six2eight - partial byte */
    u_char	fract_bits;	/* six2eight - #of bits in partial_byte */
    u_char	last_char;	/* rle - last data char sent */
    int		char_count;	/* rle - repeat count */
    u_char	*ibuf;		/* input buffer */
    u_char	*obuf;		/* output buffer */
    int		colpos;		/* column position in output */
    boolean_t	crlf;		/* \r\n line ending? (otherwise \r) */
};
typedef struct binhex_state binhex_state;

#define	MAX_BINHEX_LINE_LEN	64		/* len of binhex line */

#define BINHEX_HDR_LEN	22			/* length of binhex header (w/o var-length fname) */
#define BINHEX_VERSION  0			/* version # in binhex header */

enclinfo *binhex2encl (t_file *textf, t_file *newtextf, char *errstr);
boolean_t encl2binhex(enclinfo *encl, t_file *text_f);
boolean_t binhex(enclinfo *encl, t_file *binhex_f, boolean_t crlf);

#endif 	/* _BINHEX_H */
