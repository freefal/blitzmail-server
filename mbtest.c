/*

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    Test mailbox routines.
	
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <sys/dir.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <syslog.h>
#include "t_io.h"
#include "mbox.h"
#include "t_err.h"
#include "misc.h"
#include "config.h"

#define PREF_CNT 10000
#define LIST_CNT 1000

int main(int argc, char **argv) {

    mbox 	*mb;
    long 	uid;
    int		fs;
    boolean_t	done = FALSE;
    char	name[MAX_STR];
    char 	line[MAX_STR];
    char	value[MAX_STR];
    ml_data	*head, *tail, *p;
    int		len;
    int 	i;
    struct timeval		starttime, donetime;
    float			elapsed;
    
    misc_init();				/* set up global locks */
    t_ioinit();
    t_errinit("mbtest", LOG_LOCAL1);
    t_dndinit();		/* and dnd package */
    
    read_config();		/* read configuration file */
    mbox_init();
    
    if (argc != 3) {
    	fprintf(stderr, "Usage: %s uid fs#\n", argv[0]);
	exit(1);
    }
    
    uid = atoi(argv[1]);
    fs = atoi(argv[2]);
    mb = mbox_find(uid, fs, FALSE);
    
    mb->user = (udb *) mallocf(sizeof(udb));
    t_fdopen(&mb->user->conn, 1);	/* use stdout for testing */
    
    do {
	t_fflush(&mb->user->conn);	/* flush output from last command */
	
	printf("{p(ref) | l(ist) }{s(et) | g(et) | r(em) } or q(uit)? ");
	gets(line);
	switch(*line) {
	    case 'l':
	    case 'L':
		switch(line[1]) {	
		    case 'g':
		    case 'G':
			printf("list name: ");
			gets(name);			
			if (!ml_get(mb, name, &head, NULL))
			    printf("Nope.\n");
			else {
			    for (p = head; p; p = p->next) {
				(void) fwrite(p->data, 1, p->len, stdout);
			    }
			}
			break;
		    case 's':
		    case 'S':
			printf("list name: ");
			gets(name);
			printf("\nEnter contents, end with blank line.\n: ");
			head = tail = p = NULL;
			for (gets(line); *line; gets(line)) {
         		    len = strlen(line) + 1;
			    p = mallocf(sizeof(ml_data) + len);
			    strcpy(p->data, line);
			    p->data[len-1] = '\n'; /* note: terminate w/ \n */
			    p->len = p->maxlen = len;
			    if (head == NULL)
				head = tail = p;
			    else {
				tail->next = p;
				tail = p;
			    }
			    p->next = NULL;
			    printf(": ");
			}
			ml_set(mb, name, head);
			
			while(head) {
			    p = head;
			    head = head->next;
			    t_free(p);
			}
			break;
		    case 'r':
		    case 'R':
			printf("list name: ");
			gets(name);			
			if (!ml_rem(mb, name))
			    printf("Nope.\n");
			else printf("List removed.\n");
			break;
		    case 'L':
		    case 'l':
			ml_summary(mb, FALSE);
			break;
		    case 't':
			if (gettimeofday(&starttime, NULL) < 0) {
			    perror("gettimeofday");
			    exit(1);
			}
		    
			for (i = 0; i < LIST_CNT; i++) {			
			    sprintf(name, "%d", i);
			    len = strlen(name) + 1;
			    p = mallocf(sizeof(ml_data) + len);
			    strcpy(p->data, name);
			    p->data[len-1] = '\n'; /* note: terminate w/ \n */
			    p->len = p->maxlen = len;
			    p->next = NULL;	
			    ml_set(mb, name, p);			   						    		    
			    t_free(p);		    
			}
			if (gettimeofday(&donetime, NULL) < 0) {
			    perror("gettimeofday");
			    exit(1);
			}
			
			/* compute elapsed time */
			elapsed = donetime.tv_sec - starttime.tv_sec;
			elapsed += (float) (donetime.tv_usec - starttime.tv_usec) / 1000000;
			
			printf("%d lists saved\n", LIST_CNT);						
			printf("Elapsed time: %.2f seconds.\n", elapsed);
			break;
		    case 'T':
			if (gettimeofday(&starttime, NULL) < 0) {
			    perror("gettimeofday");
			    exit(1);
			}
			for (i = 0; i < LIST_CNT; i++) {
			    sprintf(name, "%d", i);
			    if (!ml_rem(mb, name))
				printf("cannot remove %s\n",name);
			}
			if (gettimeofday(&donetime, NULL) < 0) {
			    perror("gettimeofday");
			    exit(1);
			}
			
			/* compute elapsed time */
			elapsed = donetime.tv_sec - starttime.tv_sec;
			elapsed += (float) (donetime.tv_usec - starttime.tv_usec) / 1000000;
			
			printf("%d lists removed\n", LIST_CNT);						
			printf("Elapsed time: %.2f seconds.\n", elapsed);
			break;				
		}
		break;
	    case 'p':
	    case 'P':
		switch(line[1]) {
		    case 'g':
		    case 'G':
			printf("pref name: ");
			gets(name);			
			if (!pref_get(mb, name, value))
			    printf("Nope.\n");
			else 
			    printf("%s\n", value);
			break;
		    case 'r':
		    case 'R':
			printf("pref name: ");
			gets(name);			
			if (!pref_rem(mb, name))
			    printf("Nope.\n");
			else
			    printf("Preference removed.\n");
			break;		
		    case 's':
		    case 'S':
			printf("pref name: ");
			gets(name);			
			printf("value: ");
			gets(value);			
			pref_set(mb, name, value);
			break;
		    case 't':
			if (gettimeofday(&starttime, NULL) < 0) {
			    perror("gettimeofday");
			    exit(1);
			}
			for (i = 0; i < PREF_CNT; i++) {
			    sprintf(name, "%d", i);
			    strcpy(value, name);
			    pref_set(mb, name, value);
			}
			if (gettimeofday(&donetime, NULL) < 0) {
			    perror("gettimeofday");
			    exit(1);
			}
			/* compute elapsed time */
			elapsed = donetime.tv_sec - starttime.tv_sec;
			elapsed += (float) (donetime.tv_usec - starttime.tv_usec) / 1000000;
			
			printf("%d prefs saved\n", PREF_CNT);						
			printf("Elapsed time: %.2f seconds.\n", elapsed);
			break;
		    case 'T':
			if (gettimeofday(&starttime, NULL) < 0) {
			    perror("gettimeofday");
			    exit(1);
			}
			for (i = 0; i < PREF_CNT; i++) {
			    sprintf(name, "%d", i);
			    if (!pref_rem(mb, name))
				printf("cannot remove %s\n",name);
			}
			if (gettimeofday(&donetime, NULL) < 0) {
			    perror("gettimeofday");
			    exit(1);
			}
			
			/* compute elapsed time */
			elapsed = donetime.tv_sec - starttime.tv_sec;
			elapsed += (float) (donetime.tv_usec - starttime.tv_usec) / 1000000;
			
			printf("%d prefs removed\n", PREF_CNT);						
			printf("Elapsed time: %.2f seconds.\n", elapsed);
			break;					
		}
		break;
	    case 'q':
	    case 'Q':
	    case '\0':
		done = TRUE;
		break;
	}
	if (mb->prefs && mb->prefs->dirty) {
	    printf("write preferences...\n");
	    pref_write(mb);
	}
    } while (!done);
    
    mbox_done(&mb);
    
    exit(0);
}

void doshutdown() {}