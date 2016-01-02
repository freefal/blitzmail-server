/*  Mach BlitzMail Server -- ZIP definitions
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/zip.h,v 3.0 97/01/27 17:00:33 davidg Exp $
*/

#ifndef	_ZIP_H
#define	_ZIP_H

#define	ZIP_QUERY		1	/* the ZIP commmand field encodings */
#define	ZIP_REPLY		2
#define	ZIP_GET_NET_INFO	5
#define	ZIP_GET_NET_INFO_REPLY	6
#define	ZIP_EXTENDED_REPLY	8
#define	ZIP_NOTIFY		7

#define	ZIP_GET_MY_ZONE		7	/* the ATP/ZIP commands */
#define	ZIP_GET_ZONE_LIST	8
#define	ZIP_GET_LOCAL_ZONES	9

/* note that the 16 bit fields should be read/written with get/putnetshort */
struct	getnetinfo {			/* ZIP GetNetInfo format */
	u_char	command;		/* ZIP command is ZIP_GET_NET_INFO */
	u_char	flags;			/* used to zero all flags */
	char	first[2];		/* net range start -- warning; unaligned */
	char	last[2];		/* net range end -- warning; unaligned */
	u_char	zlen;			/* zone name length */
	u_char	zname[1];		/* zone name placeholder */
};
typedef struct getnetinfo getnetinfo;

#define GETNETINFO_LEN	7		/* sizeof(getnetinfo) includes padding... */
/* GetNetInfo flag bits: */
#define Z_ZINVALID	0x80		/* flag: Zone Invalid */
#define Z_BROADCAST	0x40		/* flag: Use Broadcast */
#define Z_ONEZONE	0x20		/* flag: Only One Zone */

#endif	_ZIP_H
