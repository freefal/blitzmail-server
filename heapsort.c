/* 
   Sort an array of message id's in place. Classic heapsort (Knuth Vol. 3)
   Array is arranged into a heap: a complete binary tree where parent's
   key is always >= both children. Tree is represented linearly using the
   array itself: the 2 children of a[i] are at a[2*i+1] and a[2*i+2].
*/
void sort_messlist(summent *sum, int count) {

    int 	l, r;	/* left & right bounds */
    int		i, j; 	/* key indices */
    summent	cur;	/* entry currenty being moved */
    
    if (count <= 1)		/* avoid annoying boundary cases */
    	return;
    
    /* step 1: arrange array into a heap */
    
    l = count / 2; r = count - 1;	/* work on leaves of tree */
    
    /* loop decreases l first, then r */
    for (;;) {
        if (l > 0) {			/* step 1: arrange array into a heap */
	    --l;			/* work on next entry to left */
	    cur = sum[l];		
	} else {			/* step 2: output largest; re-heapify */	
	    cur = sum[r];	
	    sum[r] = sum[0];		/* move largest entry to end */
	    --r;			/* move heap boundary */
	    if (r == 0) {		/* the end */
	    	sum[0] = cur;		/* output last entry */
		break;			/* done */
	    }
	}

	j = l;				/* prepare to sift up larger child */
	
	for (;;) {			/* until larger than both children */
	    i = j; j = 2*j+1;		/* move down */
	    if (j > r)
	    	break;			/* reached a leaf; done */
	    if (j < r && 		/* if both children exist... */
	    	  sum[j].messid < sum[j+1].messid) /* ...and right is bigger... */
	    	++j;			/* ...advance to it */
	    if (cur.messid > sum[j].messid)
	    	break;			/* larger than both children; done */
	    sum[i] = sum[j];		/* move up larger child & loop */
	}
	sum[i] = cur;
    }

}