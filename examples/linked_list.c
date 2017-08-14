#include <stdio.h>
#include "nvmalloc.h"

struct ll_node {
    int val;
    struct ll_node *next;
};

int main () {
    int v;
    printf ("Type an integer to add to the list:\n");
    if (fscanf (stdin, "%d", &v) != 1)
        return 1;

    pinit ("ll.dump");
    
    struct ll_node *curr = NULL;
    if (v == 0) {//Removes the first element of the list
        struct ll_node *head = pget_root();
        if (head) curr = head->next;
        pfree (head);
        pset_root(curr);
    } else { //Adds an element to the beginning of the list
        curr = pmalloc (sizeof (struct ll_node));
        curr->val = v;
        curr->next = pget_root ();
        pset_root (curr);
    }        
    printf("The list contains: ");
    while (curr) {
        printf("%d ", curr->val);
        curr = curr->next;
    }
    printf("\n");

    pdump ();
        
    return 0;
}
