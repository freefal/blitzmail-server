/*

    Display all outgoing queues on this server.
    
    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/blitzq.c,v 3.0 97/01/27 16:50:32 davidg Exp $
    
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/dir.h>
#include <sys/socket.h>
#include <sys/errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <syslog.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"
#include "mess.h"
#include "queue.h"

void showqueue(int hostnum);
void doshutdown() {}

int         count_only = 0;         /* count but do not print? */

int main (int argc, char **argv) {

    int		i;
    int		c;
    	
    while ((c = getopt(argc, argv, "n")) != EOF) {
	switch (c) {
	    case 'n':
		count_only++;		/* count-only mode */
		break;
	    default:
		printf("Usage: %s [-n]\n", argv[0]);
		exit(1);
	}
    }

    misc_init();	    
    t_ioinit();
    t_errinit("blitzq", LOG_LOCAL1);	/* initialize error package */
    t_dndinit();		/* and dnd package */
    
    read_config();		/* read configuration file */
            
    for (i = 0; i < m_servcount; ++i) {	/* for each blitz server */
	if (!count_only)
	    printf("\nMessages queued for %s:\n", m_server[i]);
	showqueue(i);
    }
    if (m_server[SMTP_HOSTNUM]) {
	if (!count_only)
	    printf("\nOutgoing SMTP queue:\n");
	showqueue(SMTP_HOSTNUM);
    } else {
	printf("\nSendmail's queue:\n");
	system("mailq");
    }
    printf("\n\n");
	    
    exit(0);
}

/* print contents of one queue 

    Read directory to locate all control files; sort them.  Read each file to 
    get sender and recipient names.

*/


void showqueue(int hostnum) {

    char		spooldir[FILENAME_MAX];
    DIR			*dirf;			/* open directory file */
    struct direct 	*dirp;			/* directory entry */
    char		fname[FILENAME_MAX];
    t_file		*f;
    char		buf[MAX_STR];
    char		summstr[SUMMBUCK_LEN];	/* summary info in string form */
    summinfo		summ;			/* in broken-down form */
    struct d {					/* to make sorted list of names */
    	char		d_name[MAXNAMLEN + 1];
	struct d	*next;
	};
    struct d		*head = NULL;
    struct d		*d, *nextd, *new, *prev;
    int			count = 0;
    
    t_sprintf(spooldir, "%s%s%s", m_spoolfs_name, SPOOL_DIR, m_server[hostnum]);

    pthread_mutex_lock(&dir_lock);	/* in case opendir isn't thread-safe */
    dirf = opendir(spooldir);
    pthread_mutex_unlock(&dir_lock);

    if (dirf == NULL) {
	if (pthread_errno() != ENOENT) {	/* if no messages yet, dir may not exist */
	    t_perror1("showqueue: cannot open ", spooldir);
	    exit(1);
	}
	return;
    }

    while ((dirp = readdir(dirf)) != NULL) {	/* read entire directory */
	    
	/* skip dot-files */
	if (dirp->d_name[0] != '.') {
	    /* want just the control files */
	    if (dirp->d_name[strlen(dirp->d_name) - 1] == 'C') {

		if (count_only) {	/* just counting; that's easy */
		    ++count;
		    continue;
		}

		new = mallocf(sizeof(struct d));
		strcpy(new->d_name, dirp->d_name);
		prev = NULL;
		for (d = head; d && atoi(d->d_name) < atoi(new->d_name); ) {
		    prev = d;
		    d = d->next;	/* locate insertion place */
		}
		if (prev) {
		    new->next = prev->next;
		    prev->next = new;
		} else {
		    new->next = head;
		    head = new;
		}
	    }
	}
    }
    
    closedir(dirf);
    
    if (count_only) {				/* just hostname/count on 1 line */
	if (hostnum == SMTP_HOSTNUM)
	    printf("Outgoing SMTP: %d\n", count);
	else
	    printf("%s: %d\n", m_server[hostnum], count);
	return;
    }

    for (d = head; d; d = nextd) {
	t_sprintf(fname, "%s/%s", spooldir, d->d_name);
	/* if file is gone, it's ok */
	if (f = t_fopen(fname, O_RDONLY, 0)) {
	    if (t_gets(buf, sizeof(buf), f) 	/* get sender & summary */
	     && t_gets(summstr, sizeof(summstr), f)) {
	        if (summ_parse(summstr, &summ, fname, FALSE)) {
		    printf("%s:  Messid = %ld Sender = %s\n", d->d_name, summ.messid, buf);
		    while (t_gets(buf, sizeof(buf), f)) {
			printf("        %s\n", buf);
		    }
		}
	    }
	    t_fclose(f);
	}
	nextd = d->next;
	t_free(d);
    }
}
