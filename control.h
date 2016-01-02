/*  Mach BlitzMail Server -- control definitions

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/control.h,v 3.0 97/01/27 16:54:01 davidg Exp $
    
*/
#ifndef _CONTROL_H
#define _CONTROL_H

#include <sys/types.h>

#include "sem.h"


struct stat_client {	/* info for each client */
    struct stat_client	*next;	/* link in table */
    long	interval;	/* send update this often */
    long	when;		/* time of last registration */
    struct	sockaddr_in addr;/* client address */
};
typedef struct stat_client stat_client;

stat_client	*ctl_clients;	/* table of clients */
struct sem stat_sem;	/* protects it */

int	stat_skt;		/* UDP socket for status packets */

any_t ctl_udplisten(any_t zot);
any_t ctl_udpsend(any_t zot);

#ifndef __NeXT__

/* non-NeXT systems have tbl_dkinfo instead; must translate */
struct tbl_diskinfo
{
    bit32     di_time;        	/* ticks drive active */
    bit32     di_seek;        	/* # drive seeks */
    bit32     di_xfer;        	/* # drive transfers */
    bit32     di_wds;         	/* # sectors transfered */
    bit32     di_bps;         	/* drive transfer rate (bytes per second) */
    char      di_name[8];     	/* drive name */
};
#endif

struct mb_stats_t {
	bit32		summ_r, summ_w;		/* summary reads & writes */
	bit32		pref_r, pref_w;		/* pref reads & writes */
	bit32		mlist_r, mlist_w;	/* mailing list reads + writes */
};
typedef struct mb_stats_t mb_stats_t;

/* vm info, 32-bit version */
struct vm_stats {
        bit32    pagesize;               /* page size in bytes */
        bit32    free_count;             /* # of pages free */
        bit32    active_count;           /* # of pages active */
        bit32    inactive_count;         /* # of pages inactive */
        bit32    wire_count;             /* # of pages wired down */
        bit32    zero_fill_count;        /* # of zero fill pages */
        bit32    reactivations;          /* # of pages reactivated */
        bit32    pageins;                /* # of pageins */
        bit32    pageouts;               /* # of pageouts */
        bit32    faults;                 /* # of faults */
        bit32    cow_faults;             /* # of copy-on-writes */
        bit32    lookups;                /* object cache lookups */
        bit32    hits;                   /* object cache hits */
};
typedef struct vm_stats vm_stats;

#define BLITZ_SERVER_CTL	"BlitzMail-Control"
#define DND_SERVER_CTL		"DND-Control"
#define DISKMAX 7

typedef int cpustates[CPUSTATES];
typedef char diskinfo[DISKMAX][sizeof(struct tbl_diskinfo)];
typedef char vminfo[sizeof(vm_statistics_data_t)];
typedef char mbinfo[sizeof(mb_stats_t)];

/* request bitmap definitions */
#define INFO_CPU	0x1
#define INFO_DISK	0x2
#define INFO_VM		0x4
#define INFO_BLITZ	0x8

/* definitions for udp-based status updating */

/* status packet. */
/* this definition is mostly for reference -- you can't just read
   the packet into it unless you're on a system which does the byte
   ordering & padding to match */
   
struct statpkt {		/* all fields in network byte order */
	bit32		version;			/* packet format version */
	bit32		users;				/* user count */
	u_bit32		cpu[CPUSTATES];
	u_bit32		cpu_hz;
	struct {
		bit32		summ_r, summ_w;		/* summary reads & writes */
		bit32		pref_r, pref_w;		/* pref reads & writes */
		bit32		mlist_r, mlist_w;	/* mailing list reads + writes */
	} mb_stat;
	struct {
		bit32    pagesize;               /* page size in bytes */
		bit32    free_count;             /* # of pages free */
		bit32    active_count;           /* # of pages active */
		bit32    inactive_count;         /* # of pages inactive */
		bit32    wire_count;             /* # of pages wired down */
		bit32    zero_fill_count;        /* # of zero fill pages */
		bit32    reactivations;          /* # of pages reactivated */
		bit32    pageins;                /* # of pageins */
		bit32    pageouts;               /* # of pageouts */
		bit32    faults;                 /* # of faults */
		bit32    cow_faults;             /* # of copy-on-writes */
		bit32    lookups;                /* object cache lookups */
		bit32    hits;                   /* object cache hits */
	} vm_stat;
	struct
	{
	    bit32     di_time;        	/* ticks drive active */
	    bit32     di_seek;        	/* # drive seeks */
	    bit32     di_xfer;        	/* # drive transfers */
	    bit32     di_wds;         	/* # sectors transfered */
	    bit32     di_bps;         	/* drive transfer rate (bytes per second) */
	    char      di_name[8];     	/* drive name */
	} disk[DISKMAX];
};
typedef struct statpkt statpkt;

#define STATPKT_VERS	1
#define STATPKT_LEN (4*(1+1+CPUSTATES+1+6+13) + DISKMAX*(8+4*5))

struct statreq {		/* status registration request */
	bit32		cmd;	/* command (== STAT_REG) */
	bit32		interval; /* refresh interval (seconds) */
};
typedef struct statreq statreq;
#define STATREQ_LEN	8	/* length of request packet */

#define STATREQ_RTX	15	/* retransmit request this often */
#define STATREQ_TIMEOUT 60	/* server times out after this long */

#define STAT_REG	1
#define STAT_PORT	"blitzctl"	/* /etc/services name of udp status port */

#endif
