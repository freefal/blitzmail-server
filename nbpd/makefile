LIB=../ddp.o ../t_err.o ../t_io.o ../misc.o ../sem.o ../config.o ../t_dnd.o ../cryptutil.o
BLITZHOME=/usr/local/lib/blitz
ALPHAHOST = magdalena

###################################################################################
#
# OS-dependent section; uncomment the appropriate lines
#
# ############### NeXT:
#
#CFLAGS=-g -Wall -O
#INSTALL=/usr/bin/install
#LFLAGS=
#
# ############## DEC OSF/1
#
#CFLAGS=-g -std -threads -framepointer -non_shared
#INSTALL=/usr/bin/installbsd -c
#LFLAGS=-lpthreads -lmach -lc_r
#
# ############## Digital Unix >= 4.0
#
# shared pthread library not available
# note that because we're using "-threads" instead of "-pthread"
# it's still using the POSIX 1003.4a Draft 4 interface, instead
# of the standard POSIX 1003.1c
CFLAGS=${KRB_CFLAGS} -g -std -threads -framepointer
DEPENDFLAGS=-E -M
LFLAGS=${KRB_LFLAGS}
INSTALL=/usr/bin/installbsd -c
LOCALETC=/usr/local/sbin
#
# ################## IBM AIX 3.2:
#
#CFLAGS=-D_ALL_SOURCE -I. -I/usr/include -I../AIX_INCLUDE/usrinclude\
# -I../AIX_INCLUDE -L/usr/local/lib -D_H_ACCESS -D_AIX -g ${DBCFLAGS}
#DEPENDFLAGS=-E -M
#LFLAGS=-bloadmap:loadmap -lpthreads -lc_r
#INSTALL=/usr/ucb/install
#LOCALETC=/usr/local/etc
#CC=cc_r
#
# ################## IBM AIX 4.1:
#
#CFLAGS=-D_ALL_SOURCE -I. -I/usr/include -I../AIX_INCLUDE/usrinclude \
#       -I../AIX_INCLUDE -L/usr/local/lib -D_H_ACCESS -D_AIX -g ${DBCFLAGS}
#DEPENDFLAGS=-E -M
#LFLAGS=-bloadmap:loadmap -lpthreads -lc_r
#INSTALL=/usr/ucb/install
#LOCALETC=/usr/local/etc
#CC=cc_r
################## end of OS-dependent section ####################################


all: nbpd

nbpd: nbpd.o ${LIB}
	$(CC) ${CFLAGS} ${LFLAGS} -o nbpd nbpd.o ${LIB} 

nbpd.o : nbpd.c ../ddp.h ../t_err.h ../misc.h nbpd.h 

clean:
	rm -f *.o
	rm -f  nbpd

tar:
	tar cf - nbpd.c nbpd.h makefile >nbp.tar

#
# distribute sources to other systems; recompile
#
alpha:
	make rdist
	rsh $(ALPHAHOST) 'cd source/blitzserver/nbpd;make'

rdist:
	rdist -f ../distfile

install: all
	$(INSTALL) nbpd $(BLITZHOME)
