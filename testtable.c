#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <sys/dir.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <signal.h>
#include "misc.h"
#include "config.h"
#include "control.h"

void get_info();
void get_disk(struct tbl_diskinfo *disk);
void get_cpu(int cpu[CPUSTATES], int *cpu_hz);
void get_info(int cpu[CPUSTATES], int *cpu_hz, struct tbl_diskinfo di[DISKMAX]);

int main(int argc, char **argv) {

    cpustates		cpu, old_cpu, diff;
    int			cpu_hz;
    struct tbl_diskinfo	di[DISKMAX], old_di[DISKMAX];
    int			elapsed;
    long		xfers, wds;
    int			delay = 5;
    int			i;
    
    if (argc > 1)
	delay = atoi(argv[1]);

    get_info(old_cpu, &cpu_hz, old_di);
	
    for (;;) {
	sleep(delay);
	get_info(cpu, &cpu_hz, di);
	
	/* note: cannot just loop 0 .. CPUSTATES-1 */
	diff[CP_NICE] = cpu[CP_NICE] - old_cpu[CP_NICE];
	diff[CP_SYS] = cpu[CP_SYS] - old_cpu[CP_SYS];
	diff[CP_USER] = cpu[CP_USER] - old_cpu[CP_USER];
	diff[CP_IDLE] = cpu[CP_IDLE] - old_cpu[CP_IDLE];
 	diff[CP_WAIT] = cpu[CP_WAIT] - old_cpu[CP_WAIT];
   
	elapsed = diff[CP_NICE] + diff[CP_SYS] + diff[CP_USER] + diff[CP_IDLE] + diff[CP_WAIT];

	xfers = (di[0].di_xfer - old_di[0].di_xfer) / (float) (elapsed / cpu_hz);
	wds = (di[0].di_wds - old_di[0].di_wds)  / (float) (elapsed / cpu_hz);
	
	if (elapsed > cpu_hz) {
	printf("Nice %d; Sys %d; User %d; Idle %d; Wait %d; Xfers %ld; Wds %ld\n",
		100*diff[CP_NICE] / elapsed,
		100*diff[CP_SYS] / elapsed,
		100*diff[CP_USER] / elapsed,
		100*diff[CP_IDLE] / elapsed,
		100*diff[CP_WAIT] /elapsed,
		xfers,
		wds);
	    }
	bcopy(di, old_di, sizeof(di));
	bcopy(cpu, old_cpu, sizeof(cpu));
    }
    exit(0);
}

void get_info(int cpu[CPUSTATES], int *cpu_hz, struct tbl_diskinfo di[DISKMAX]) {

    get_cpu(cpu, cpu_hz);
    get_disk(di);
}

/* get_cpu --

    Get cpu usage figures from OS. System-specific.
*/

void get_cpu(int cpu[CPUSTATES], int *cpu_hz) {

#ifdef TBL_CPUINFO
    struct tbl_cpuinfo	cpuinfo;
    if (table(TBL_CPUINFO, 0, &cpuinfo, 1, sizeof(cpuinfo)) < 0)
	perror("TBL_CPUINFO");
    else {
	bcopy(cpuinfo.ci_cptime, cpu, sizeof(cpuinfo.ci_cptime));
	*cpu_hz = cpuinfo.ci_hz;
    }
#else
    struct tbl_sysinfo	sysinfo;
    if (table(TBL_SYSINFO, 0, &sysinfo, 1, sizeof(sysinfo)) < 0)
	perror("TBL_SYSINFO");
    else {
	cpu[CP_USER] = sysinfo.si_user;
	cpu[CP_NICE] = sysinfo.si_nice;
	cpu[CP_SYS] = sysinfo.si_sys;
	cpu[CP_IDLE] = sysinfo.si_idle;
	cpu[CP_WAIT] = sysinfo.wait;
	*cpu_hz = sysinfo.si_hz;
    }

#endif
}
/* get_disk --

    Get disk usage figures from OS. System-specific.
*/

void get_disk(struct tbl_diskinfo *disk) {

    int		i;
    
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
}
