#include "port.h"
#include <stdlib.h>
#include <stdio.h>

/* Mutex test -- start 2 copies of thread that grabs & holds mutex; watch
	what happens to cpu. */

pthread_mutex_t lock;

void f(int zot) {

	pthread_mutex_lock(&lock);
	sleep(120);
	pthread_mutex_unlock(&lock);
}

int main(int argc, char **argv) {

	pthread_t	t1, t2;
	
    pthread_mutex_init(&lock, pthread_mutexattr_default);
	
    pthread_create(&t1, generic_attr,
                   (pthread_startroutine_t) f, (pthread_addr_t) 0);
    pthread_detach(&t1);
    pthread_create(&t2, generic_attr,
                   (pthread_startroutine_t) f, (pthread_addr_t) 0);
    pthread_detach(&t2);

    sleep(999);
}

