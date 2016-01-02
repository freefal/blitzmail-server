/*  BlitzMail Server -- control interface.
	
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/control.c,v 3.1 97/10/19 18:58:51 davidg Exp Locker: davidg $
    
    Routines to handle status monitoring functions. Initial versions of this code
    (on the NeXT) used mach RPC (with MiG). That proved to be a barrier to portability;
    plain UDP is now used.
    
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "client.h"
#include "config.h"
#include "mess.h"
#include "deliver.h"
#include "queue.h"
#include "smtp.h"
#include "ddp.h"
#include "control.h"

#ifdef NeXT
extern int table(int id, int index, void *addr, int nel, unsigned lel);
#endif
void make_statpkt(char *statpkt, long users, int cpu[CPUSTATES], int cpu_hz, 
		  mb_stats_t *mb_stats, struct tbl_diskinfo *disk, 
		  vm_statistics_data_t *vm);

/* get_cpu --

    Get cpu usage figures from OS. System-specific.
*/

void get_cpu(int cpu[CPUSTATES], int *cpu_hz) {

#ifdef TBL_CPUINFO
    struct tbl_cpuinfo	cpuinfo;
    if (table(TBL_CPUINFO, 0, &cpuinfo, 1, sizeof(cpuinfo)) < 0)
	t_perror("TBL_CPUINFO");
    else {
	bcopy(cpuinfo.ci_cptime, cpu, sizeof(cpuinfo.ci_cptime));
	*cpu_hz = cpuinfo.ci_hz;
    }
#endif
#ifdef __POWER_AIX__
    cpu[0]=0;
    cpu[1]=0;
    cpu[2]=0;
    cpu[3]=0;
#endif
#ifdef __ALPHA_OSF1__
    struct tbl_sysinfo	sysinfo;
    if (table(TBL_SYSINFO, 0, &sysinfo, 1, sizeof(sysinfo)) < 0)
	t_perror("TBL_SYSINFO");
    else {
	cpu[CP_USER] = sysinfo.si_user;
	cpu[CP_NICE] = sysinfo.si_nice;
	cpu[CP_SYS] = sysinfo.si_sys;
	cpu[CP_IDLE] = sysinfo.si_idle;
/* OSF/1 2.0 introduces new state "wait"; just lump it in with idle */
#ifdef CP_WAIT
	cpu[CP_IDLE] += sysinfo.wait;
#endif
	*cpu_hz = sysinfo.si_hz;
    }

#endif
}
/* get_disk --

    Get disk usage figures from OS. System-specific.
*/

void get_disk(struct tbl_diskinfo *disk) {

    int		i;
    
#ifdef __POWER_AIX__
    for (i = 0; i< DISKMAX; i++)
    {
	bzero((char*)&disk[i],sizeof(disk[i]));
    }
#else
#ifdef TBL_DISKINFO
    /* start with disk #1 -- 0 is the floppy drive */
    for (i = 0; i < DISKMAX; ++i) {
	if (table(TBL_DISKINFO, i+1, (char *) &disk[i], 1, sizeof(struct tbl_diskinfo)) < 0)
	    bzero((char *) &disk[i], sizeof(disk[i]));	/* no such disk */
    }
#else
    /* OSF system; translate tbl_dkinfo format */
    struct tbl_dkinfo dkinfo;
    
    for (i = 0; i < DISKMAX; ++i) {
	if (table(TBL_DKINFO, i, &dkinfo, 1, sizeof(dkinfo)) < 0)
	    bzero((char *) &disk[i], sizeof(disk[i]));	/* no such disk */
	else {
	    disk[i].di_time = dkinfo.di_time;
	    disk[i].di_seek = dkinfo.di_seek;
	    disk[i].di_xfer = dkinfo.di_xfer;
	    disk[i].di_wds  = dkinfo.di_wds;
	    /* (doc. says "wpms" is words per ms; seems to be bytes, so
	        no conversion is needed) */
	    disk[i].di_bps  = dkinfo.di_wpms;
	    sprintf(disk[i].di_name, "%s%d", dkinfo.di_name, dkinfo.di_unit);
	}
    }

#endif	
#endif
}

/* ctl_udplisten --

    Thread to handle udp status requests. Listen for requests from clients
    that want to monitor our status; enter them into client table. Start a
    separate thread to periodically send status packets to each registered client.
*/

any_t ctl_udplisten(any_t zot) {

    struct sockaddr_in	sin;	/* its addr */
    struct servent	*sp;	/* services entry */
    static t_file	listen_f; /* t_file for listening socket */
    statreq		req;	/* request packet */
    int			cc;	/* read len */
    struct sockaddr_in 	fromaddr;	
    int			fromlen;
    stat_client		*p;
    pthread_t thread;		/* thread var */
    
    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();

    sem_init(&stat_sem, "stat_sem");

    /* start thread to send status packets */
    if (pthread_create(&thread, generic_attr,
                   (pthread_startroutine_t) ctl_udpsend, (pthread_addr_t) 0) < 0) {
	t_perror("ctl_udplisten: pthread_create");
	return 0;			/* trouble; give up on status thread */	
    }
    pthread_detach(&thread);

    sem_seize(&herrno_sem);		/* serialize access to getservbyname */        
    if ((sp = getservbyname(STAT_PORT, "udp")) == NULL) {
	t_errprint_s("ctl_udplisten: unknown service: %s", STAT_PORT);
	exit(1);
    }
    sin.sin_family = AF_INET;	
    sin.sin_addr.s_addr = INADDR_ANY;
    sin.sin_port = sp->s_port;	/* port we listen on */
    sp = NULL;			/* sppml */
    sem_release(&herrno_sem);
    

    if ((stat_skt = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
	t_perror("ctl_udplisten: socket: ");
	exit(1);
    }   
    
    if (bind(stat_skt, (struct sockaddr *) &sin, sizeof(sin)) < 0) {
	t_perror("ctl_udplisten: bind");
	if (pthread_errno() == EADDRINUSE) {
	    t_errprint("UDP Status service disabled; address in use");
	    return 0;
	}
	exit(1);
    }
 
    t_fdopen(&listen_f, stat_skt);	/* set up (useless) t_file for the socket... */
    strcpy(listen_f.name, "ctl_udplisten"); /* ...just to get it entered in t_fdmap */
    	
    
    for (;;) {		/* now just keep accepting */
    
	fromlen = sizeof(fromaddr);
    	cc = recvfrom(stat_skt, &req, sizeof(req), 0, 
		    (struct sockaddr *) &fromaddr, &fromlen);
	if (cc < 0)
	    t_perror("ctl_udplisten: read");
	else if (cc == STATREQ_LEN && ntohl(req.cmd) == STAT_REG) { /* ignore strays */
	    sem_seize(&stat_sem);	/* get access to table */
	    /* locate existing entry for this client */
	    for (p = ctl_clients; p != NULL; p = p->next) {
		if (p->addr.sin_addr.s_addr == fromaddr.sin_addr.s_addr
		    && p->addr.sin_port == fromaddr.sin_port)
		    break;
	    }
	    if (p == NULL) {		/* make new entry */
		p = mallocf(sizeof(stat_client));
		p->next = ctl_clients;
		p->addr = fromaddr;
		ctl_clients = p;
	    }
	    p->when = time(NULL);	/* current time */
	    p->interval = ntohl(req.interval);	/* refresh this often */
	    sem_release(&stat_sem);
	}
    }
}

/* ctl_udpsend --

    Thread to handle udp status broadcasting. Once a second, send out status updates
    to each client that's due for one. (They specify a refresh interval in seconds).
    Also check for registrations that haven't been updated recently; drop them from
    the table.
*/

any_t ctl_udpsend(any_t zot) {

    char		pkt[STATPKT_LEN];	/* status packet */
    stat_client		*p, *pp, *nextp;
    long		secs = 0;
    vm_statistics_data_t vm;
    cpustates		cpu;
    int			cpu_hz;
    struct tbl_diskinfo	di[DISKMAX];
    int			s;
    
    setup_signals();			/* set up signal handlers for new thread */
    setup_syslog();
    
    for (;;) {				/* send forever */
    
	sleep(1);			/* check once a second (approximately) */
	++secs;				/* count off */

	/* set up info packet (once, no matter how many clients) */
	
	/* ### note that this code needs to be updated if fields are added ### */
	/* fill in cpu info */
	get_cpu(cpu, &cpu_hz);
	/* disk info */
	get_disk(di);
	
	/* vm info */
#ifndef __POWER_AIX__
	if (vm_statistics(task_self(), (vm_statistics_data_t *) &vm) != KERN_SUCCESS) 
#endif
	    bzero((char *) &vm, sizeof(vm_statistics_data_t));
	
	make_statpkt(pkt, u_num, cpu, cpu_hz, &mb_stats, di, &vm);
		
	    	
	sem_seize(&stat_sem);	/* get access to table */
	
	for (p = ctl_clients, pp = NULL; p != NULL; p = nextp) {
	    nextp = p->next;
	    /* if request is stale, drop from table */
	    if (time(NULL) - p->when > STATREQ_TIMEOUT) {
		if (pp)
		    pp->next = p->next;
		else
		    ctl_clients = p->next;
		t_free(p);
		p = NULL;	/* sppml */	
	    } else {		/* see if they're due for an update */
		if (secs % p->interval == 0) {
		    s = sendto(stat_skt, pkt, STATPKT_LEN, 0,
		    	(struct sockaddr *) &p->addr, sizeof(p->addr));
		    if (s < 0)
			t_perror("ctl_udpsend sendto");
		}
		pp = p;		/* didn't free; advance backpointer */
	    }
	}
	
	sem_release(&stat_sem);
    }
}

/* copy all values into status packet in network byte order, avoiding padding */

void make_statpkt(char *statpkt, long users, int cpu[CPUSTATES], int cpu_hz, 
		  mb_stats_t *mb_stats, struct tbl_diskinfo *disk, 
		  vm_statistics_data_t *vm) {

    int		i;
    
    statpkt = putnetlong(statpkt, STATPKT_VERS);	/* generate version 1 packet */
    
    statpkt = putnetlong(statpkt, users);
    
    /* note: # of CPUSTATES is system-specific (is 5 instead of 4 under
       OSF/1 2.0, where there's also CP_WAIT) */
    statpkt = putnetlong(statpkt, cpu[CP_USER]);
    statpkt = putnetlong(statpkt, cpu[CP_NICE]);
    statpkt = putnetlong(statpkt, cpu[CP_SYS]);
    statpkt = putnetlong(statpkt, cpu[CP_IDLE]);
    statpkt = putnetlong(statpkt, cpu_hz);
    
    /* mb */
    statpkt = putnetlong(statpkt, mb_stats->summ_r);
    statpkt = putnetlong(statpkt, mb_stats->summ_w);
    statpkt = putnetlong(statpkt, mb_stats->pref_r);
    statpkt = putnetlong(statpkt, mb_stats->pref_w);
    statpkt = putnetlong(statpkt, mb_stats->mlist_r);
    statpkt = putnetlong(statpkt, mb_stats->mlist_w);

    /* (hope we don't lose anything important by converting to 32 bits...) */
    statpkt = putnetlong(statpkt, vm->pagesize);
    statpkt = putnetlong(statpkt, vm->free_count);
    statpkt = putnetlong(statpkt, vm->active_count);
    statpkt = putnetlong(statpkt, vm->inactive_count);
    statpkt = putnetlong(statpkt, vm->wire_count);
    statpkt = putnetlong(statpkt, vm->zero_fill_count);
    statpkt = putnetlong(statpkt, vm->reactivations);
    statpkt = putnetlong(statpkt, vm->pageins);
    statpkt = putnetlong(statpkt, vm->pageouts);
    statpkt = putnetlong(statpkt, vm->faults);
    statpkt = putnetlong(statpkt, vm->cow_faults);
    statpkt = putnetlong(statpkt, vm->lookups);
    statpkt = putnetlong(statpkt, vm->hits);
    
    /* disks */
    for (i = 0; i < DISKMAX; ++i) {
	statpkt = putnetlong(statpkt, disk[i].di_time);
	statpkt = putnetlong(statpkt, disk[i].di_seek);
	statpkt = putnetlong(statpkt, disk[i].di_xfer);
	statpkt = putnetlong(statpkt, disk[i].di_wds);
	statpkt = putnetlong(statpkt, disk[i].di_bps);
	bcopy(disk[i].di_name, statpkt, 8);
	statpkt += 8;
    }
}
