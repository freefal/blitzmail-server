#
#	Makefile to install BlitzMail server.
#
# 	Copyright (c) 1992, Trustees of Dartmouth College
#	
#	See the README file for complete installation instructions
#
BLITZHOME=/usr/local/lib/blitz
LOCALBIN=/usr/local/bin
LOCALETC=/usr/local/sbin
ADM=/usr/adm
#
# (use /usr/bin/installbsd on OSF/1 systems)
#
INSTALL=/usr/bin/installbsd

install:
	-mkdir $(BLITZHOME)
	chmod 750 $(BLITZHOME)
	$(INSTALL) -m 750 blitzserv $(BLITZHOME)
	$(INSTALL) -m 750 nbpd $(BLITZHOME)
	$(INSTALL) -m 750 notifyd $(BLITZHOME)
	$(INSTALL) -m 750 master $(BLITZHOME)
	$(INSTALL) -m 750 makemess $(BLITZHOME)
	$(INSTALL) -m 750 computemessid $(BLITZHOME)
	-mkdir $(LOCALBIN)
	$(INSTALL) check_blitz $(LOCALBIN)
	$(INSTALL) blitzq $(LOCALBIN)
	$(INSTALL) -m 750 checkmess $(LOCALBIN)
	$(INSTALL) -m 750 checkallmess $(LOCALBIN)
	$(INSTALL) -m 750 ctyscript $(LOCALBIN)
	-mkdir $(LOCALETC)
	$(INSTALL) kill_blitz $(LOCALETC)
	$(INSTALL) restart_blitz $(LOCALETC)
	-test -d /sbin/init.d && make install-osf-boot

install-config:
	$(INSTALL) -m 750 blitzconfig $(BLITZHOME)
	
install-scripts:
	$(INSTALL) blitzdaily $(ADM)
	$(INSTALL) blitzweekly $(ADM)
	$(INSTALL) blitzmonthly $(ADM)
	$(INSTALL) newlog $(ADM)
	$(INSTALL) -m 640 proclist $(BLITZHOME)

#
# install startup/shutdown script in osf-style init.d
# (non-osf systems; edit /etc/rc.local by hand)
#
install-osf-boot:
	$(INSTALL) -o bin -g bin -m 755 blitzmail.init /sbin/init.d/blitzmail
	ln -s /sbin/init.d/blitzmail /sbin/rc3.d/S40blitzmail
	ln -s /sbin/init.d/blitzmail /sbin/rc2.d/K30blitzmail
	ln -s /sbin/init.d/blitzmail /sbin/rc0.d/K25blitzmail
	
