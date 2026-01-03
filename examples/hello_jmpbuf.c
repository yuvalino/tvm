#include <setjmp.h>
#include <stdio.h>

int main() {
    jmp_buf b;

    if (0 != setjmp(b)) {
        printf("goodbye\n");
        return 55;
    }

    printf("hello!\n");
    longjmp(b, 1);
}