/*  BlitzMail Server -- semaphores

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.
    
    $Header: /users/davidg/source/blitzserver/RCS/sem.h,v 3.0 97/01/27 16:59:36 davidg Exp $
*/

#ifndef _T_SEM_H
#define _T_SEM_H

#define SEM_NAMEMAX 32
struct sem {
    char		name[SEM_NAMEMAX]; /* for error messages only */
    pthread_mutex_t	lock;		/* protects the entire semaphore */
    pthread_cond_t	wait;		/* threads waiting to lock */
    pthread_t		owner;		/* thread that has it now */
};
typedef struct sem sem;

void sem_seize(sem *s);
void sem_release(sem *s);
void sem_init(sem *s, char *name);
void sem_check(sem *s);
void sem_destroy(sem *s);
void abortsig();
#endif
