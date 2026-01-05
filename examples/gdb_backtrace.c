#include <stdio.h>

volatile int arr[] = {1, 2, 3, 4 ,5};

static void foreach(void (*f)(int)) {
    for (int i = 0; i < (sizeof(arr)/sizeof(*arr)); i++) {
        f(arr[i]);
    }
}

static void callback(int n)
{
    printf("callback %d\n", n);
    if (n == 4)
        *(int *)0xDEAD = 42;
}

int main() {
    printf("gdb test\n");

    foreach(callback);

    return 0;
}