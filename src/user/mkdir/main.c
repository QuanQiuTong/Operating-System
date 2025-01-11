#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    int i;
    if (argc < 2) {
        printf("give at least 2 args\n");
        exit(1);
    }
    for (i = 1; i < argc; i++) {
        if (mkdirat(AT_FDCWD, argv[i], 0) < 0) {
            printf("%s failed\n", argv[i]);
        }
    }
    exit(0);
}