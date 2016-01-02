#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>


#define MAX 100000

int main(int argc, char **argv) {

   int summlcount, summlmax;
   typedef struct summent {                    /* element of summary list */
        long            messid;                 /* message id */
    } summent;
    summent             *summlist;              /* list of summaries */

    long messid;
    int i,j;

    summlcount = 0; summlmax = 1000;
    summlist = malloc(summlmax * sizeof(summent));

    for (messid = MAX; messid > 0; --messid) {
          if (summlcount == summlmax) {
              summlmax += 1000;   /* need to grow list */
              summlist = realloc(summlist, summlmax * sizeof(summent));
          }
          for (i = 0; i < summlcount; i++) {
              if (messid <= summlist[i].messid)
                  break;
          }


          if (i < summlcount) {   /* need to slide others out of way */
              if (messid == summlist[i].messid) {
                 continue;               /* don't add to list again */
              } else {            /* make room */
                  for (j = summlcount; j > i; --j)
                      summlist[j] = summlist[j-1];
              }
          }

          /* room made; add the new entry */
          summlist[i].messid = messid;
          summlcount++;           /* one more total */

          if (messid % 1000 == 0) {
	     printf("%d\n", messid);
	     fflush(stdout);
	  }
     }
 
     printf("done\n");
     exit(0);

}
