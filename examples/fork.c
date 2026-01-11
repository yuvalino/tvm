#include <unistd.h>
#include <stdio.h>

static int a = 0;

int main() {
    a++;

    int p;
    if (-1 == (p = fork()))
        return 1;

    if (p)
        printf("[PARENT %d] BEFORE %d (%p)\n", getpid(), a, &a);
    else
        printf("[CHILD  %d] BEFORE %d (%p)\n", getpid(), a, &a);

    a++;
    if (p)
        printf("[PARENT %d] %d (%p)\n", getpid(), a, &a);
    else
        printf("[CHILD  %d] %d (%p)\n", getpid(), a, &a);
    return 0;
}