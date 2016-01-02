OBJECTS=  ../ddp.o ../t_err.o ../misc.o ../t_io.o ../t_dnd.o\
          ../config.o ../sem.o ../cryptutil.o 
BLITZHOME=/usr/local/lib/blitz
ALPHAHOST = magdalena

###################################################################################
#
# OS-dependent section; uncomment the appropriate lines
#
# ############### NeXT:
#
#CFLAGS=-g -Wall -O
#DEPENDFLAGS=-c -MM
#INSTALL=/usr/bin/install
#LFLAGS=
#
# ############## DEC OSF/1
#
#CFLAGS=-g -std -threads -non_shared -framepointer
#DEPENDFLAGS=-E -M
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
#CFLAGS=-I. -I/usr/include -I.. -D_H_ACCESS -D_AIX -g ${DBCFLAGS}
#DEPENDFLAGS=-E -M
#LFLAGS=-lpthreads -lc_r -L/usr/local/lib
#INSTALL=/usr/ucb/install
#LOCALETC=/usr/local/etc
#CC=cc_r
#
# ################## IBM AIX 4.1:
#
#CFLAGS=-D_ALL_SOURCE -I. -I/usr/include -I../AIX_INCLUDE/usrinclude \
#        -I../AIX_INCLUDE -L/usr/local/lib -D_H_ACCESS -D_AIX -g ${DBCFLAGS}
#DEPENDFLAGS=-E -M
#LFLAGS=-bloadmap:loadmap -lpthreads -lc_r
#INSTALL=/usr/ucb/install
#LOCALETC=/usr/local/etc
#CC=cc_r
#

################## end of OS-dependent section ####################################

all: notifyd notifytest tags

notifyd: ${OBJECTS} notifyd.o
	 $(CC) ${CFLAGS} ${LFLAGS} -o notifyd notifyd.o ${OBJECTS}

notifytest: ${OBJECTS} notifytest.o
	$(CC) -s ${CFLAGS} ${LFLAGS} -o notifytest notifytest.o ${OBJECTS}

clean:
	rm -f *.o
	rm -f notifyd

tar:
	tar cf - notifyd.c notify.h makefile >not.tar

tags: notifyd.c notify.h not_types.h
	ctags notifyd.c notify.h not_types.h

depend:
	$(CC) $(DEPENDFLAGS) *.c | fgrep -v /usr/include>makedep
	cp makefile makefile.bak
	echo '/^# DO NOT DELETE THIS LINE/+2,$$d' >eddep
	echo '$$r makedep' >>eddep
	echo 'w' >>eddep
	ed - makefile < eddep
	rm eddep makedep

#
# distribute sources to other systems; recompile
#
alpha:
	make rdist
	rsh $(ALPHAHOST) 'cd source/blitzserver/notify;make'

rdist:
	rdist -f ../distfile

install: notifyd notifytest
	$(INSTALL) notifyd $(BLITZHOME)
	$(INSTALL) notifytest $(BLITZHOME)
	
# DO NOT DELETE THIS LINE -- make depend uses it

notifyd.o : notifyd.c ../port.h ../ddp.h ../misc.h ../t_err.h ../sem.h \
  ../nbpd/nbpd.h ../t_io.h ../t_dnd.h ../config.h not_types.h notify.h 
notifytest.o : notifytest.c ../port.h ../ddp.h ../misc.h ../t_err.h \
  ../sem.h ../t_io.h not_types.h notify.h 
