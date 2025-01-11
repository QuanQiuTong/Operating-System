#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void print_help() {
    printf("Usage: mkdir [OPTION]... DIRECTORY...\n");
    printf("  -m, --mode=MODE       set file mode\n");
    printf("  -p, --parents         create parent directories as needed\n");
    printf("  -v, --verbose         print a message for each created directory\n");
    printf("  -Z, --context=CTX     (NOT implemented) set the SELinux security context of each created directory to CTX\n");
    printf("      --help            display this help and exit\n");
    printf("      --version         output version information and exit\n");
    exit(0);
}

static void print_version() {
    printf("mkdir (FDU OS) 1.0\n");
    exit(0);
}

static void setfilecon(const char *path) {
    (void)path;
    printf("setfilecon: not implemented\n");
    exit(1);
}

static int mkdir_parents(const char *path, mode_t mode, bool verbose) {
    char tmp[1024];
    char *p = NULL;
    size_t len;
    strncpy(tmp, path, sizeof(tmp));
    tmp[sizeof(tmp) - 1] = '\0';
    len = strlen(tmp);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    if (mkdir(tmp, mode) == 0 && verbose) {
        printf("mkdir: created directory '%s'\n", tmp);
    }
    return 0;
}

int main(int argc, char *argv[]) {
    static struct option long_options[] = {
        {"mode", required_argument, 0, 'm'},
        {"parents", no_argument, 0, 'p'},
        {"verbose", no_argument, 0, 'v'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'V'},
        {"context", required_argument, 0, 'Z'},
        {0, 0, 0, 0}};

    bool parents = false, verbose = false;
    mode_t mode = 0777;
    int opt;

    while ((opt = getopt_long(argc, argv, "m:pvhVZ:", long_options, NULL)) != -1) {
        switch (opt) {
        case 'm':
            mode = strtol(optarg, NULL, 8);
            break;
        case 'p':
            parents = true;
            break;
        case 'v':
            verbose = true;
            break;
        case 'h':
            print_help();
            break;
        case 'V':
            print_version();
            break;
        case 'Z':
            setfilecon(optarg);
            break;
        default:
            fprintf(stderr, "Try --help for more information.\n");
            exit(1);
        }
    }

    if (optind >= argc) {
        fprintf(stderr, "mkdir: missing operand\n");
        exit(1);
    }

    for (int i = optind; i < argc; i++) {
        int ret;
        if (parents) {
            ret = mkdir_parents(argv[i], mode, verbose);
        } else {
            ret = mkdir(argv[i], mode);
            if (ret == 0 && verbose) {
                printf("mkdir: created directory '%s'\n", argv[i]);
            }
        }
        if (ret < 0) {
            fprintf(stderr, "mkdir: %s: %s\n", argv[i], strerror(errno));
        }
    }
    exit(0);
}