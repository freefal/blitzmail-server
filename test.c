#include <stdio.h>
#include <syslog.h>


int main(int argc, char **argv) {

	char *s = "summ_parse: bad summary: %s\n";
	char *mess = "12324";
	int i;
	char x[1024] = "beep";

	if (argc > 0) main(argc -1, argv);
	else {
	openlog("test", 0, LOG_LOCAL1);

	for (i = 0;i < 5;++i)
	syslog(LOG_ERR, s, mess);

	exit(0);
	}

}
