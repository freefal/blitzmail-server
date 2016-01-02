/* BlitzMail Server -- portability definitions

    Copyright (c) 1994 by the Trustees of Dartmouth College; 
    see the file 'Copyright' in the distribution for conditions of use.

   This file includes the appropriate system header files for NeXT
   Mach, Alpha OSF/1, or RS/6000 AIX.
      
   NOTE: This file should always be included FIRST -- thread headers may
   define things that change behavior of other system headers.
   
   Pthread syntax is now used. Macros are provided to translate back to
   cthread syntax on the NeXT (only the subset of pthread operations that
   map directly onto cthreads should be used.) 
*/
#ifndef _PORT_H
#define _PORT_H

#if defined(__osf__) && defined(__alpha)
#define	__ALPHA_OSF1__
#endif

#ifdef _AIX
#define __POWER_AIX__
#endif

#ifdef __NeXT__

/* Assume Motorola 680x0 hardware */
#define IS_BIG_ENDIAN

/* NeXT Mach -- CThreads */
#include <cthreads.h>
#include <mach_error.h>
#include <sys/dk.h>
#include <sys/table.h>
#include <vm/vm_statistics.h>
#include <kern/std_types.h>

/* libc.h provides prototypes for system calls & includes other handy
   files */
#include <libc.h>
/* missing from libc.h for some reason... */
extern int	wait4(int, union wait *, int, struct rusage *);

/* Macros to give access to cthreads using pthread syntax */
#define pthread_create(a,b,c,d) ((*(a) = cthread_fork((cthread_fn_t) (c), (any_t) (d))),0)
#define pthread_detach(a)	cthread_detach(*(a))
#define pthread_t		cthread_t
#define pthread_mutex_t		struct mutex
#define pthread_cond_t		struct condition
#define pthread_addr_t		any_t
#define pthread_startroutine_t	cthread_fn_t
#define pthread_errno()		cthread_errno()
#define pthread_self()		((pthread_t) cthread_self())
#define pthread_cond_signal	condition_signal
#define pthread_cond_wait 	condition_wait	
#define pthread_cond_init(a,b)	condition_init((a)) 
/* note: pthread_cond_destroy and pthread_mutex_destroy reclaim private
   pthread lib storage associated with the mutex/condition (distinct from
   the user-visible pthread_cond_t/pthread_mutex_t). Not necessary w/ cthreads */
#define pthread_cond_destroy(a)	
#define pthread_mutex_destroy(a)	
#define pthread_mutex_init(a,b)	mutex_init((a))
#define pthread_mutex_lock 	mutex_lock	
#define pthread_mutex_unlock 	mutex_unlock	
#define pthread_condattr_default	0
#define pthread_mutexattr_default	0
#define pthread_attr_default		0
#define pthread_equal(a,b)	((a) == (b))
#define pthread_yield		cthread_yield
#define NO_PTHREAD		((pthread_t)0)
#define pthread_attr_t		long
#define pthread_attr_create(a)	(0)
#define pthread_attr_setstacksize(a,b)

/* NeXT statfs takes 1 less arg */
#define STATFS(a,b,c)		statfs((a),(b))

#endif /* !__NeXT__ */

#ifdef __ALPHA_OSF1__

/* Alpha hardware -- don't define BIG_ENDIAN */

/* OSF/1 -- PThreads */
#include <pthread.h>
/* pick up mach definitions */
#include <mach/mach_types.h>
#include <mach/message.h>
#include <mach/vm_statistics.h>
#include <sys/dk.h>
#include <sys/table.h> 

/* Dec doesn't define a null; bozos! */
#ifndef NO_PTHREAD
pthread_t null_thread;
#define NO_PTHREAD      null_thread
#endif

/* posix makes vanilla errno work w/ multiple threads, but we use
pthread_errno() syntax to make it clear something special is happening */
#define pthread_errno()	errno

/* define _BSD to get dirent.h to make "struct direct" work for "struct dirent" */

#define _BSD

#include <wait.h>
#include <sys/mount.h>
#include <sys/vnode.h>
extern int statfs(char *, struct statfs *, int);
#include <sys/socket.h>
#define STATFS(a,b,c)		statfs((a),(b),(c))

#endif /* __ALPHA_OSF1__ */

#ifdef __POWER_AIX__

/* AIX on Power or PowerPC */
#define IS_BIG_ENDIAN

/* For some reason, AIX does not have wait4 */
#define wait4(pid,stat,zero,nullo) waitpid(pid,stat,0)

/* PThreads */
#include <pthread.h>

/* IBM doesn't define a null */
#ifndef NO_PTHREAD
pthread_t null_thread;
#define NO_PTHREAD      null_thread
#endif
/* posix makes vanilla errno work w/ multiple threads, but we use
   pthread_errno() syntax to make it clear something special is
   happening */
#define pthread_errno()	errno

/* define _BSD to get dirent.h to make "struct direct" work for  
   "struct dirent" */
#define _BSD	43

/* AIX calls inode number field in struct direct "d_ino" */
#define	d_fileno	d_ino

#include <sys/types.h>		/* needed for sys/socket.h */
#include <sys/socket.h>
#include <sys/vnode.h>
#include <sys/signal.h>
#include <time.h>
#include <sys/select.h>
#include <sys/statfs.h>
#include <sys/param.h>
#include <sys/wait.h>
extern int statfs(char *, struct statfs *, int);
extern long random(void);

#define STATFS(a,b,c)		statfs((a),(b),(c))

/* types we use but aren't defined by AIX */
typedef long bit32;
typedef unsigned long u_bit32;
typedef unsigned char u_bit8;
typedef unsigned short u_bit16;
typedef short bit16;
typedef int kern_return_t;
typedef int port_t;
typedef unsigned char boolean_t;
#define KERN_SUCCESS 0
#define CP_USER 0
#define CP_NICE 1
#define CP_SYS 2
#define CP_IDLE 3
#define CPUSTATES 4
#define NUMLEN 100

typedef struct vm_stats vm_statistics_data_t;


/* notify fixes (lnb) */
typedef struct msg_hdr msg_header_t;
typedef mtyp_t msg_type_long_t;
typedef mtyp_t msg_type_t;
typedef int msg_return_t;

#endif /* __POWER_AIX__ */

#endif /* _PORT_H */

