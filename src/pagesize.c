#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main () { 
    printf("%ld\n", sysconf(_SC_PAGE_SIZE));
    return EXIT_SUCCESS;
}

