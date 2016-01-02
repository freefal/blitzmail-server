/*  BlitzMail Server -- semaphores

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

    $Header: /users/davidg/source/blitzserver/RCS/sem.c,v 3.0 97/01/27 16:59:30 davidg Exp $

    In both NeXT and OSF/1, the low-level pthread_mutex_lock and pthread_mutex_unlock 
    operations turn out to be implemented more or less as spin locks - if the mutex is 
    busy, the locking thread does yield, but it remains eligible to be run again more
    or less immediately (it's not suspended.)  This is ok if the critical region
    protected by the mutex is small and cpu-bound, but for longer locking a 
    higher-level mechanism is needed.
    
    The rule (not yet 100% observed) is that if any io is done while a lock is held,
    a semaphore rather than a plain mutex should be used.
        
*/

#include "port.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "sem.h"
#include "t_err.h"

void stackcrawl(void);

/* sem_init -- initialize a newly-allocated semaphore */

void sem_init(sem *s, char *name) {

    pthread_mutex_init(&s->lock, pthread_mutexattr_default);
    pthread_cond_init(&s->wait, pthread_condattr_default);
    strncpy(s->name, name, SEM_NAMEMAX); 
    s->name[SEM_NAMEMAX-1] = 0;
    s->owner = NO_PTHREAD;		/* initial state == free */
}

void sem_seize(sem *s) {

    pthread_mutex_lock(&s->lock);	
    if (pthread_equal(s->owner, pthread_self())) {
    	t_errprint_s("sem_seize [%s]: self-deadlock!", s->name);
	abortsig();
    }	
    while(!pthread_equal(s->owner, NO_PTHREAD))	/* wait for others to finish */
	pthread_cond_wait(&s->wait, &s->lock);
    s->owner = pthread_self();	/* ours now */
    pthread_mutex_unlock(&s->lock);
}

void sem_release(sem *s) {

    pthread_mutex_lock(&s->lock);
    if (!pthread_equal(s->owner, pthread_self())) {
    	t_errprint_s("sem_release [%s]: not our semaphore!", s->name);
	abortsig();			/* we didn't own it! */
    }
    s->owner = NO_PTHREAD;		/* up for grabs now */
    pthread_mutex_unlock(&s->lock);
    pthread_cond_signal(&s->wait);	/* if anyone waiting for it, they can go */
}
/* verify that semaphore is seized by current thread */

void sem_check(sem *s) {

    pthread_mutex_lock(&s->lock);
    if (!pthread_equal(s->owner, pthread_self())) {
    	t_errprint_s("sem_check [%s]: semaphore not seized!", s->name);
	abortsig();			/* we didn't own it! */
    }
    pthread_mutex_unlock(&s->lock);
}

/* clean up semaphore (before freeing) */
void sem_destroy(sem *s) {

    if (!pthread_equal(s->owner, NO_PTHREAD)) {
     	t_errprint_s("sem_destroy [%s]: destroying locked sem!", s->name);
	abortsig();   
    }
    
    /* tell pthread package to clean up */
    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->wait);
    
    /* it's now safe for caller to free the sem */
}

/* abortsig --

    Exit program by calling abort(3). For systems where abort() doesn't generate
    a usable traceback, call stackcrawl to do it by hand (sigh).
    
    Alpha/OSF doesn't generate a proper core dump automatically upon receipt 
    of a SIGQUIT, but the abort() function does work (modulo the lack of traceback). 
    This function should be installed as the SIGQUIT handler.
*/
void abortsig() {

    stackcrawl();		/* manual traceback, if necessary */
    abort();			/* now generate a core dump */
}

/* stackcrawl --

   Attempt to do a stack traceback to stderr. [ Alpha/OSF1 ]
   
   Strategy is to declare exactly one automatic variable; frame pointer should be
   immediately before that in memory. 

   Stack layout looks like (slight simplification):

           0 ##################################
             #                                #
             .                                .
        sp-> #        < return addr >         #
             #       < frame ptr (fp1) >      #
             #         < locals >             #
             .                                .
             .                                .
       fp1-> #      < next return addr >      #
             #      < frame ptr (fp2) >       #
             .                                .
   FFFFFFFF  ##################################

   This is all pretty evil (and very dependent on how the compiler feels like allocating
   stack space); it's a workaround for DEC OSF/1's inability to generate a core dump
   with a usable traceback. We don't attempt to display procedure names (just return
   addresses) because (1) it's hard, and (2) this code should be as simple and foolproof
   as possible, to maximize the chances of getting a usable traceback.
   
   Note that everything must be compiled with the -framepointer option for this
   traceback routine to work. The kludge for locating the initial frame pointer
   works for OSF/1 2.1; no guarantees about other versions.
*/

void stackcrawl() {

/* fortunately, only the Alpha has to deal with this garbage */
#ifdef __alpha
    int zot;           /* push frame size up to 32 */
    int stacktop;      /* placeholder to help locate fp */
/* ### no more auto vars after these 2!!! ### */

    static long *sp, *lastsp;
    static int crawling = 0;
    
    if (crawling) return; 	/* one is plenty */
    crawling = 1;		/* note: avoid mutex's, in case they're corrupted */

    sp = ((long *) &stacktop) - 2; /* CROCK: saved addr/fp will be right below our last var */

    fprintf(stderr, "Attempting stack traceback...\n");
    
    for (;;) {          /* climb back up stack, starting with our caller */
        fprintf(stderr, "Frame at: 0x%016lx; ret = 0x%016lx", sp, sp[0]); 
        if (sp[1])      /* can't compute size of top frame - no fp */
           fprintf(stderr, "; size = %ld\n", (long) sp[1] - (long) sp);
        else
           fprintf(stderr, "\n");

        lastsp = sp;
        sp = (long *) sp[1];            /* pick up frame pointer & move on */
        if (sp == 0 || sp <= lastsp)    /* done when null, or going wrong way */
            break;
    }
#endif
}
