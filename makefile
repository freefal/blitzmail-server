#
#	Makefile for BlitzMail server
#
#    Copyright (c) 1994 by the Trustees of Dartmouth College; 
#    see the file 'Copyright' in the distribution for conditions of use.
#
#
#	$Header: /users/davidg/source/blitzserver/RCS/makefile,v 3.1 97/10/19 19:03:01 davidg Exp Locker: davidg $
#

BLITZHOME=/usr/local/lib/blitz
LOCALBIN=/usr/local/bin
ADM=/usr/adm

# host to copy backups to, and to rdist changes to
BACKUPHOST = coos
ALPHAHOST = magdalena
###################################################################################
#
# Kerberos option -- uncomment these lines if your site has a DND-capable Kerberos
#
KRB_CFLAGS=-DKERBEROS -I/usr/kerberos/include
KRB_LFLAGS=-L/usr/kerberos/lib
KRB_OBJECTS=-lkrb -ldes
#
###################################################################################
#
# OS-dependent section; uncomment the appropriate lines
#
# ############### NeXT:
#
#CFLAGS=${KRB_CFLAGS} -g -Wall -O
#DEPENDFLAGS=-c -MM
#INSTALL=/usr/bin/install
#LFLAGS=${KRB_LFLAGS}
##LFLAGS=-lMallocDebug ${KRB_LFLAGS}
#LOCALETC=/usr/local/etc
#
# ############## DEC OSF/1 <= 3.2
#
#CFLAGS=${KRB_CFLAGS} -g -std -threads -non_shared -framepointer
#DEPENDFLAGS=-E -M
#INSTALL=/usr/bin/installbsd -c
#LFLAGS=${KRB_LFLAGS} -lpthreads -lmach -lc_r
#LOCALETC=/usr/local/sbin
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
#CFLAGS= -I. -D_H_ACCESS -D_AIX -g ${DBCFLAGS}
#DEPENDFLAGS=-E -M
#LFLAGS=-L/usr/local/lib -lpthreads -lc_r
#INSTALL=/usr/ucb/install
#LOCALETC=/usr/local/etc
#CC=cc_r
#
################## end of OS-dependent section ####################################

OBJECTS= mbox.o mlist.o misc.o t_err.o t_io.o pref.o summ.o client.o mess.o\
	addr.o pubml.o deliver.o queue.o t_dnd.o config.o smtp.o ddp.o sem.o\
	cty.o binhex.o cryptutil.o 
LINK_OBJS=${OBJECTS} ${KRB_OBJECTS}
SERVOBJECTS = blitzserv.o control.o

all: blitzserv makemess blitzq computemessid master checkmess tags

blitzserv: ${SERVOBJECTS} ${OBJECTS} makefile
	$(CC) ${CFLAGS} ${LFLAGS} -o blitzserv ${SERVOBJECTS} ${LINK_OBJS}

makemess: makemess.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o makemess makemess.o ${LINK_OBJS}

blitzq: blitzq.o ${OBJECTS} makefile
	$(CC)  -s ${CFLAGS} ${LFLAGS} -o blitzq blitzq.o ${LINK_OBJS}

fopentest: fopentest.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o fopentest fopentest.o ${LINK_OBJS}

krbtest: krbtest.o ${OBJECTS} makefile
	$(CC) ${CFLAGS} ${LFLAGS} -o krbtest krbtest.o ${LINK_OBJS}

krbclient: krbclient.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o krbclient krbclient.o ${LINK_OBJS}

krb_bench: krb_bench.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o krb_bench krb_bench.o ${LINK_OBJS}

dnstest: dnstest.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o dnstest dnstest.o ${LINK_OBJS}

opentest: opentest.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o opentest opentest.o ${LINK_OBJS}

checkmess: checkmess.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o checkmess checkmess.o ${LINK_OBJS}

computemessid: computemessid.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o computemessid computemessid.o ${LINK_OBJS}
	
master: master.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o master master.o ${LINK_OBJS}
	
ddptest: ddptest.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o ddptest ddptest.o ${LINK_OBJS}

mbtest: mbtest.o ${OBJECTS} makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o mbtest mbtest.o ${LINK_OBJS}

ctyscript: ctyscript.o makefile
	$(CC) -s ${CFLAGS} ${LFLAGS} -o ctyscript ctyscript.o

tags: *.c *.h
	ctags *.c *.h
#	
# export_tar - binary distribution, with simplified makefile
#
EXPORTBINS=blitzserv makemess computemessid blitzq checkmess checkallmess ctyscript

export_tar:
	tar cvfh export/blitz.tar ${EXPORTBINS} blitzmail.init\
		README* Copyright blitzconfig Welcome.txt proclist changelog\
		-C export makefile\
		-C ../notify notifyd\
		-C ../nbpd nbpd\
		-C $(LOCALETC) kill_blitz restart_blitz\
		-C $(LOCALBIN) check_blitz\
		-C $(BLITZHOME) master\
		-C /usr/adm blitzdaily blitzweekly blitzmonthly newlog
	compress -f export/blitz.tar

#	
# make distribution file:
#	
tar:
	tar cvf blitz.tar *.c *.h *.init makefile export/makefile\
	README* blitzconfig Welcome.txt proclist changelog checkallmess\
	nbpd/*.c nbpd/*.h nbpd/makefile\
	notify/*.c notify/*.h notify/makefile\
	-C $(LOCALETC) kill_blitz restart_blitz\
	-C $(LOCALBIN) check_blitz\
	-C /usr/adm blitzdaily blitzweekly blitzmonthly newlog
	compress -f blitz.tar

backup:
	make tar
	rcp blitz.tar.Z ${BACKUPHOST}:
	rm blitz.tar.Z
#
# distribute sources to other systems; recompile
#
alpha:
	make rdist
	rsh $(ALPHAHOST) 'cd source/blitzserver;make'

rdist:
	rdist -f distfile
#
# install from scratch on new system, including one-time steps of moving scripts
# and config files to their real home
#	
install-new: $(BLITZHOME) install-blitzserv install-nbpd install-notifyd install-master\
	install-utils install-scripts install-config
	
install: install-blitzserv install-nbpd install-notifyd install-master\
	install-utils

$(BLITZHOME):
	mkdir $(BLITZHOME)
	chmod 750 $(BLITZHOME)
	
install-blitzserv: blitzserv	
	$(INSTALL) blitzserv $(BLITZHOME)

install-nbpd: nbpd
	(cd nbpd; make install)

install-notifyd:
	(cd notify; make install)
	
install-master: master
	$(INSTALL) master $(BLITZHOME)	

install-notifytest:
	(cd notify; $(INSTALL)  notifytest $(BLITZHOME))
	
install-utils: makemess computemessid blitzq\
		checkmess checkallmess ctyscript \
		kill_blitz restart_blitz check_blitz
	$(INSTALL) makemess $(BLITZHOME)
	$(INSTALL) computemessid $(BLITZHOME)
	$(INSTALL) blitzq $(LOCALBIN)
	$(INSTALL) checkmess $(LOCALBIN)
	$(INSTALL) checkallmess $(LOCALBIN)
	$(INSTALL) ctyscript $(LOCALBIN)
	$(INSTALL) kill_blitz $(LOCALETC)
	$(INSTALL) restart_blitz $(LOCALETC)
	$(INSTALL) check_blitz $(LOCALBIN)

#
# install scripts in /usr/adm 
#
install-scripts: 
	$(INSTALL) blitzdaily /usr/adm
	$(INSTALL) blitzweekly /usr/adm
	$(INSTALL) blitzmonthly /usr/adm
	$(INSTALL) newlog /usr/adm
	-test -d /sbin/init.d && make install-osf-boot

#
# install config file & proclist; making sure not to overwrite
# existing config
#
install-config:
	test \! \( -f $(BLITZHOME)/blitzconfig \)
	$(INSTALL) -m 644 blitzconfig $(BLITZHOME)
	$(INSTALL) -m 644 proclist $(BLITZHOME)
#
# install startup/shutdown script in osf-style init.d
# (non-osf systems; edit /etc/rc.local by hand)
#
install-osf-boot:
	$(INSTALL) -o bin -g bin -m 755 blitzmail.init /sbin/init.d/blitzmail
	ln -s /sbin/init.d/blitzmail /sbin/rc3.d/S40blitzmail
	ln -s /sbin/init.d/blitzmail /sbin/rc2.d/K30blitzmail
	ln -s /sbin/init.d/blitzmail /sbin/rc0.d/K25blitzmail
		
clean: 
	rm *.o *.lna.out mbtest fopentest makemess ddptest blitzserv blitzq\
	master computemessid checkmess

depend:
	$(CC) $(DEPENDFLAGS) *.c | fgrep -v /usr/include>makedep
	cp makefile makefile.bak
	echo '/^# DO NOT DELETE THIS LINE/+2,$$d' >eddep
	echo '$$r makedep' >>eddep
	echo 'w' >>eddep
	ed - makefile < eddep
	rm eddep makedep
	
# DO NOT DELETE THIS LINE -- make depend uses it

addr.o:	addr.c
addr.o:	./port.h
addr.o:	./t_io.h
addr.o:	./mbox.h
addr.o:	./t_dnd.h
addr.o:	./sem.h
addr.o:	./misc.h
addr.o:	./control.h
addr.o:	./t_err.h
addr.o:	./config.h
addr.o:	./mess.h
addr.o:	./deliver.h
binhex.o:	binhex.c
binhex.o:	./port.h
binhex.o:	./t_io.h
binhex.o:	./config.h
binhex.o:	./misc.h
binhex.o:	./sem.h
binhex.o:	./t_err.h
binhex.o:	./mbox.h
binhex.o:	./t_dnd.h
binhex.o:	./control.h
binhex.o:	./mess.h
binhex.o:	./smtp.h
binhex.o:	./binhex.h
blitzq.o:	blitzq.c
blitzq.o:	./port.h
blitzq.o:	./t_io.h
blitzq.o:	./mbox.h
blitzq.o:	./t_dnd.h
blitzq.o:	./sem.h
blitzq.o:	./misc.h
blitzq.o:	./control.h
blitzq.o:	./t_err.h
blitzq.o:	./config.h
blitzq.o:	./mess.h
blitzq.o:	./queue.h
blitzserv.o:	blitzserv.c
blitzserv.o:	./port.h
blitzserv.o:	./t_io.h
blitzserv.o:	./mbox.h
blitzserv.o:	./t_dnd.h
blitzserv.o:	./sem.h
blitzserv.o:	./misc.h
blitzserv.o:	./control.h
blitzserv.o:	./t_err.h
blitzserv.o:	./client.h
blitzserv.o:	./config.h
blitzserv.o:	./mess.h
blitzserv.o:	./deliver.h
blitzserv.o:	./queue.h
blitzserv.o:	./smtp.h
blitzserv.o:	./cty.h
blitzserv.o:	./ddp.h
blitzserv.o:	./cryptutil.h
checkmess.o:	checkmess.c
checkmess.o:	./port.h
checkmess.o:	./t_io.h
checkmess.o:	./mbox.h
checkmess.o:	./t_dnd.h
checkmess.o:	./sem.h
checkmess.o:	./misc.h
checkmess.o:	./control.h
checkmess.o:	./t_err.h
checkmess.o:	./config.h
checkmess.o:	./mess.h
client.o:	client.c
client.o:	./port.h
client.o:	./t_io.h
client.o:	./mbox.h
client.o:	./t_dnd.h
client.o:	./sem.h
client.o:	./misc.h
client.o:	./control.h
client.o:	./t_err.h
client.o:	./client.h
client.o:	./config.h
client.o:	./mess.h
client.o:	./deliver.h
client.o:	./queue.h
client.o:	./notify/not_types.h
client.o:	./binhex.h
client.o:	./cryptutil.h
client.o:	./smtp.h
computemessid.o:	computemessid.c
computemessid.o:	./port.h
computemessid.o:	./t_io.h
computemessid.o:	./mbox.h
computemessid.o:	./t_dnd.h
computemessid.o:	./sem.h
computemessid.o:	./misc.h
computemessid.o:	./control.h
computemessid.o:	./t_err.h
computemessid.o:	./config.h
computemessid.o:	./mess.h
config.o:	config.c
config.o:	./port.h
config.o:	./t_io.h
config.o:	./mbox.h
config.o:	./t_dnd.h
config.o:	./sem.h
config.o:	./misc.h
config.o:	./control.h
config.o:	./t_err.h
config.o:	./config.h
config.o:	./client.h
config.o:	./queue.h
config.o:	./smtp.h
config.o:	./ddp.h
control.o:	control.c
control.o:	./port.h
control.o:	./t_io.h
control.o:	./mbox.h
control.o:	./t_dnd.h
control.o:	./sem.h
control.o:	./misc.h
control.o:	./control.h
control.o:	./t_err.h
control.o:	./client.h
control.o:	./config.h
control.o:	./mess.h
control.o:	./deliver.h
control.o:	./queue.h
control.o:	./smtp.h
control.o:	./ddp.h
cryptutil.o:	cryptutil.c
cryptutil.o:	./port.h
cryptutil.o:	./t_io.h
cryptutil.o:	./misc.h
cryptutil.o:	./sem.h
cryptutil.o:	./cryptutil.h
cty.o:	cty.c
cty.o:	./port.h
cty.o:	./t_io.h
cty.o:	./mbox.h
cty.o:	./t_dnd.h
cty.o:	./sem.h
cty.o:	./misc.h
cty.o:	./control.h
cty.o:	./t_err.h
cty.o:	./config.h
cty.o:	./client.h
cty.o:	./mess.h
cty.o:	./deliver.h
cty.o:	./cty.h
cty.o:	./smtp.h
cty.o:	./queue.h
cty.o:	./notify/not_types.h
ctyscript.o:	ctyscript.c
ctyscript.o:	./port.h
ddp.o:	ddp.c
ddp.o:	./port.h
ddp.o:	./ddp.h
ddp.o:	./misc.h
ddp.o:	./sem.h
ddp.o:	./zip.h
ddp.o:	./t_err.h
ddp.o:	./t_io.h
ddptest.o:	ddptest.c
ddptest.o:	./port.h
ddptest.o:	./ddp.h
ddptest.o:	./misc.h
ddptest.o:	./sem.h
ddptest.o:	./config.h
ddptest.o:	./t_io.h
ddptest.o:	./t_err.h
deliver.o:	deliver.c
deliver.o:	./port.h
deliver.o:	./t_io.h
deliver.o:	./mbox.h
deliver.o:	./t_dnd.h
deliver.o:	./sem.h
deliver.o:	./misc.h
deliver.o:	./control.h
deliver.o:	./t_err.h
deliver.o:	./config.h
deliver.o:	./mess.h
deliver.o:	./deliver.h
deliver.o:	./client.h
deliver.o:	./queue.h
deliver.o:	./binhex.h
deliver.o:	./ddp.h
deliver.o:	./notify/not_types.h
deliver.o:	./notify/notify.h
dnstest.o:	dnstest.c
dnstest.o:	./port.h
dnstest.o:	./t_io.h
dnstest.o:	./mbox.h
dnstest.o:	./t_dnd.h
dnstest.o:	./sem.h
dnstest.o:	./misc.h
dnstest.o:	./control.h
dnstest.o:	./t_err.h
dnstest.o:	./config.h
dnstest.o:	./mess.h
fopentest.o:	fopentest.c
fopentest.o:	./port.h
fopentest.o:	./t_io.h
fopentest.o:	./mbox.h
fopentest.o:	./t_dnd.h
fopentest.o:	./sem.h
fopentest.o:	./misc.h
fopentest.o:	./control.h
fopentest.o:	./t_err.h
fopentest.o:	./config.h
fopentest.o:	./mess.h
heapsort.o:	heapsort.c
krb_bench.o:	krb_bench.c
krb_bench.o:	./port.h
krbclient.o:	krbclient.c
krbclient.o:	./port.h
krbdemo.o:	krbdemo.c
krbtest.o:	krbtest.c
krbtest.o:	./port.h
krbtest.o:	./t_io.h
krbtest.o:	./t_err.h
krbtest.o:	./sem.h
krbtest.o:	./t_dnd.h
krbtest.o:	./sem.h
krbtest.o:	./misc.h
krbtest.o:	./sem.h
krbtest.o:	./config.h
krbtest.o:	./misc.h
makemess.o:	makemess.c
makemess.o:	./port.h
makemess.o:	./t_io.h
makemess.o:	./mbox.h
makemess.o:	./t_dnd.h
makemess.o:	./sem.h
makemess.o:	./misc.h
makemess.o:	./control.h
makemess.o:	./t_err.h
makemess.o:	./client.h
makemess.o:	./config.h
makemess.o:	./mess.h
makemess.o:	./deliver.h
makemess.o:	./queue.h
master.o:	master.c
master.o:	./port.h
master.o:	./t_io.h
master.o:	./misc.h
master.o:	./sem.h
mbox.o:	mbox.c
mbox.o:	./port.h
mbox.o:	./t_io.h
mbox.o:	./config.h
mbox.o:	./misc.h
mbox.o:	./sem.h
mbox.o:	./mbox.h
mbox.o:	./t_dnd.h
mbox.o:	./control.h
mbox.o:	./t_err.h
mbox.o:	./client.h
mbox.o:	./mess.h
mbox.o:	./ddp.h
mbox.o:	./queue.h
mbtest.o:	mbtest.c
mbtest.o:	./port.h
mbtest.o:	./t_io.h
mbtest.o:	./mbox.h
mbtest.o:	./t_dnd.h
mbtest.o:	./sem.h
mbtest.o:	./misc.h
mbtest.o:	./control.h
mbtest.o:	./t_err.h
mbtest.o:	./config.h
mess.o:	mess.c
mess.o:	./port.h
mess.o:	./t_io.h
mess.o:	./mbox.h
mess.o:	./t_dnd.h
mess.o:	./sem.h
mess.o:	./misc.h
mess.o:	./control.h
mess.o:	./t_err.h
mess.o:	./config.h
mess.o:	./mess.h
mess.o:	./deliver.h
mess.o:	./queue.h
misc.o:	misc.c
misc.o:	./port.h
misc.o:	./t_io.h
misc.o:	./t_err.h
misc.o:	./sem.h
misc.o:	./mbox.h
misc.o:	./t_dnd.h
misc.o:	./misc.h
misc.o:	./control.h
misc.o:	./config.h
misc.o:	./mess.h
mlist.o:	mlist.c
mlist.o:	./port.h
mlist.o:	./t_io.h
mlist.o:	./mbox.h
mlist.o:	./t_dnd.h
mlist.o:	./sem.h
mlist.o:	./misc.h
mlist.o:	./control.h
mlist.o:	./t_err.h
mlist.o:	./client.h
mlist.o:	./config.h
opentest.o:	opentest.c
opentest.o:	./port.h
opentest.o:	./t_io.h
opentest.o:	./mbox.h
opentest.o:	./t_dnd.h
opentest.o:	./sem.h
opentest.o:	./misc.h
opentest.o:	./control.h
opentest.o:	./t_err.h
opentest.o:	./config.h
opentest.o:	./mess.h
pref.o:	pref.c
pref.o:	./port.h
pref.o:	./t_io.h
pref.o:	./mbox.h
pref.o:	./t_dnd.h
pref.o:	./sem.h
pref.o:	./misc.h
pref.o:	./control.h
pref.o:	./t_err.h
pubml.o:	pubml.c
pubml.o:	./port.h
pubml.o:	./t_io.h
pubml.o:	./mbox.h
pubml.o:	./t_dnd.h
pubml.o:	./sem.h
pubml.o:	./misc.h
pubml.o:	./control.h
pubml.o:	./t_err.h
pubml.o:	./config.h
pubml.o:	./mess.h
pubml.o:	./client.h
pubml.o:	./deliver.h
queue.o:	queue.c
queue.o:	./port.h
queue.o:	./t_io.h
queue.o:	./mbox.h
queue.o:	./t_dnd.h
queue.o:	./sem.h
queue.o:	./misc.h
queue.o:	./control.h
queue.o:	./t_err.h
queue.o:	./config.h
queue.o:	./mess.h
queue.o:	./queue.h
queue.o:	./deliver.h
queue.o:	./client.h
queue.o:	./smtp.h
sem.o:	sem.c
sem.o:	./port.h
sem.o:	./sem.h
sem.o:	./t_err.h
smtp.o:	smtp.c
smtp.o:	./port.h
smtp.o:	./t_io.h
smtp.o:	./mbox.h
smtp.o:	./t_dnd.h
smtp.o:	./sem.h
smtp.o:	./misc.h
smtp.o:	./control.h
smtp.o:	./t_err.h
smtp.o:	./config.h
smtp.o:	./smtp.h
smtp.o:	./client.h
smtp.o:	./mess.h
smtp.o:	./deliver.h
smtp.o:	./queue.h
smtp.o:	./binhex.h
srvbug.o:	srvbug.c
srvbug.o:	./port.h
srvbug.o:	./t_io.h
srvbug.o:	./mbox.h
srvbug.o:	./t_dnd.h
srvbug.o:	./sem.h
srvbug.o:	./misc.h
srvbug.o:	./control.h
srvbug.o:	./t_err.h
srvbug.o:	./client.h
summ.o:	summ.c
summ.o:	./port.h
summ.o:	./t_io.h
summ.o:	./mbox.h
summ.o:	./t_dnd.h
summ.o:	./sem.h
summ.o:	./misc.h
summ.o:	./control.h
summ.o:	./t_err.h
summ.o:	./client.h
summ.o:	./config.h
summ.o:	./mess.h
t_dnd.o:	t_dnd.c
t_dnd.o:	./port.h
t_dnd.o:	./t_io.h
t_dnd.o:	./sem.h
t_dnd.o:	./t_dnd.h
t_dnd.o:	./mbox.h
t_dnd.o:	./misc.h
t_dnd.o:	./control.h
t_dnd.o:	./t_err.h
t_dnd.o:	./config.h
t_dnd.o:	./cryptutil.h
t_err.o:	t_err.c
t_err.o:	./port.h
t_err.o:	./t_err.h
t_err.o:	./sem.h
t_err.o:	./t_io.h
t_err.o:	./mbox.h
t_err.o:	./t_dnd.h
t_err.o:	./misc.h
t_err.o:	./control.h
t_err.o:	./config.h
t_io.o:	t_io.c
t_io.o:	./port.h
t_io.o:	./t_io.h
t_io.o:	./t_err.h
t_io.o:	./sem.h
t_io.o:	./misc.h
test.o:	test.c
testmutex.o:	testmutex.c
testmutex.o:	./port.h
testsort.o:	testsort.c
testtable.o:	testtable.c
testtable.o:	./port.h
testtable.o:	./misc.h
testtable.o:	./sem.h
testtable.o:	./config.h
testtable.o:	./control.h
