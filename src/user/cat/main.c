#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct {
    bool number;           // -n
    bool number_nonblank;  // -b
    bool show_ends;        // -E
    bool show_tabs;        // -T
    bool squeeze_blank;    // -s
    bool show_nonprint;    // -v
} flags;

#ifndef BUFSIZ
#define BUFSIZ 32768
#endif

void print_char(char c) {
    if (flags.show_nonprint) {
        if (c < 32 && c != '\n' && c != '\t') {
            printf("^%c", c + 64);
            return;
        } else if (c >= 127) {
            printf("M-%c", c - 128);
            return;
        }
    }

    if (flags.show_tabs && c == '\t') {
        printf("^I");
        return;
    }

    putchar(c);
    if (flags.show_ends && c == '\n') {
        printf("$");
    }
}

void cat_file(int fd, const char *filename) {
    char buf[BUFSIZ];
    int n;
    int line = 1;
    int empty_line = 0;
    bool prev_empty = false;

    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        for (int i = 0; i < n; i++) {
            // Handle squeeze blank lines
            if (flags.squeeze_blank) {
                if (buf[i] == '\n') {
                    if (prev_empty) {
                        continue;
                    }
                    if (i == 0 || buf[i - 1] == '\n') {
                        prev_empty = true;
                        empty_line++;
                    }
                } else {
                    prev_empty = false;
                }
            }

            // Handle line numbering
            if (i == 0 || (i > 0 && buf[i - 1] == '\n')) {
                if (flags.number ||
                    (flags.number_nonblank && (i + 1 >= n || buf[i] != '\n'))) {
                    printf("%6d\t", line++);
                }
            }

            print_char(buf[i]);
        }
    }
    if (n < 0) {
        fprintf(stderr, "cat: %s: read error: %s\n", filename, strerror(errno));
        return;
    }
}

void usage() {
    fprintf(stderr, "Usage: cat [-bEnsTv] [file ...]\n");
    exit(1);
}

void print_help() {
    printf("Usage: cat [OPTION]... [FILE]...\n");
    printf("Concatenate FILE(s) to standard output.\n\n");
    printf("  -b, --number-nonblank    number nonempty output lines\n");
    printf("  -E, --show-ends          display $ at end of each line\n");
    printf("  -n, --number             number all output lines\n");
    printf("  -s, --squeeze-blank      suppress repeated empty output lines\n");
    printf("  -T, --show-tabs          display TAB characters as ^I\n");
    printf("  -v, --show-nonprinting   use ^ and M- notation, except for LFD and TAB\n");
    printf("      --help     display this help and exit\n");
    printf("      --version  output version information and exit\n");
    exit(0);
}

void print_version() {
    printf("cat (FDU OS) 1.0\n");
    exit(0);
}

int main(int argc, char *argv[]) {
    int opt;

    static struct option long_options[] = {
        {"number-nonblank", no_argument, 0, 'b'},
        {"show-ends", no_argument, 0, 'E'},
        {"number", no_argument, 0, 'n'},
        {"squeeze-blank", no_argument, 0, 's'},
        {"show-tabs", no_argument, 0, 'T'},
        {"show-nonprinting", no_argument, 0, 'v'},
        {"show-all", no_argument, 0, 'A'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {0, 0, 0, 0}};

    while ((opt = getopt_long(argc, argv, "AbeEnsTtv", long_options, NULL)) != -1) {
        switch (opt) {
        case 'A':
            flags.show_nonprint = true;
            flags.show_ends = true;
            flags.show_tabs = true;
            break;
        case 'b':
            flags.number_nonblank = true;
            break;
        case 'e':
            flags.show_nonprint = true;
            flags.show_ends = true;
            break;
        case 'E':
            flags.show_ends = true;
            break;
        case 'n':
            flags.number = true;
            break;
        case 's':
            flags.squeeze_blank = true;
            break;
        case 'T':
            flags.show_tabs = true;
            break;
        case 't':
            flags.show_nonprint = true;
            flags.show_tabs = true;
            break;
        case 'v':
            flags.show_nonprint = true;
            break;
        case 'h':
            print_help();
            break;
        case 'V':
            print_version();
            break;
        default:
            usage();
        }
    }

    if (optind >= argc) {
        cat_file(STDIN_FILENO, "stdin");
    } else {
        for (int i = optind; i < argc; i++) {
            int fd;
            if (strcmp(argv[i], "-") == 0) {
                fd = STDIN_FILENO;
                cat_file(fd, "stdin");
            } else if ((fd = open(argv[i], O_RDONLY)) < 0) {
                fprintf(stderr, "cat: %s: %s\n", argv[i], strerror(errno));
                continue;
            } else {
                cat_file(fd, argv[i]);
                if (close(fd) < 0) {
                    fprintf(stderr, "cat: %s: close error: %s\n",
                            argv[i], strerror(errno));
                }
            }
        }
    }

    return 0;
}