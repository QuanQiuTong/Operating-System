// Shell.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

// Parsed command representation
#define EXEC 1
#define REDIR 2
#define PIPE 3
#define LIST 4
#define BACK 5

#define REDIR_HEREDOC 0x1000 // avoid conflict with those in fcntl.h

#define MAXARGS 10

struct cmd {
    int type;
};

struct execcmd {
    int type;
    char *argv[MAXARGS];
    char *eargv[MAXARGS];
};

struct redircmd {
    int type;
    struct cmd *cmd;
    char *file;
    char *efile;
    int mode;
    int fd;
};

struct pipecmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

struct listcmd {
    int type;
    struct cmd *left;
    struct cmd *right;
};

struct backcmd {
    int type;
    struct cmd *cmd;
};

int fork1(void); // Fork but panics on failure.

struct cmd *parsecmd(char *);

void *malloc1(size_t sz)
{
#define MAXN 10000
    static char mem[MAXN];
    static size_t i;
    if ((i += sz) > MAXN) {
        fprintf(stderr, "malloc1: memory used out\n");
        exit(1);
    }
    return &mem[i - sz];
}

void PANIC(char *s)
{
    fprintf(stderr, "%s\n", s);
    exit(1);
}

// Execute cmd.  Never returns.
void runcmd(struct cmd *cmd)
{
    int p[2];
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if (cmd == 0)
        exit(0);

    switch (cmd->type) {
    default:
        PANIC("runcmd");

    case EXEC:
        ecmd = (struct execcmd *)cmd;
        if (ecmd->argv[0] == 0)
            exit(0);
        execv(ecmd->argv[0], ecmd->argv);
        fprintf(stderr, "exec %s failed\n", ecmd->argv[0]);
        break;

    case REDIR:
        rcmd = (struct redircmd *)cmd;
        if (rcmd->mode & REDIR_HEREDOC) {
            int pipefd[2];
            if (pipe(pipefd) < 0) {
                PANIC("heredoc: pipe failed");
            }

            static char buf[BUFSIZ];
            static char delimiter[BUFSIZ];
            strncpy(delimiter, rcmd->file, sizeof(delimiter) - 1);
            delimiter[sizeof(delimiter) - 1] = '\0';

            char *d = delimiter;
            while (*d && (*d == ' ' || *d == '\t' || *d == '\n'))
                d++;
            char *e = d + strlen(d) - 1;
            while (e > d && (*e == ' ' || *e == '\t' || *e == '\n'))
                *e-- = '\0';

            // 创建子进程来读取 heredoc 输入
            int pid = fork1();
            if (pid == 0) {        // 子进程负责读取输入并写入管道
                close(pipefd[0]);  // 关闭读端

                while (1) {
                    fprintf(stderr, "> ");
                    if (!fgets(buf, sizeof(buf), stdin)) {
                        break;
                    }

                    char *p = strchr(buf, '\n');
                    if (p)
                        *p = '\0';

                    if (strcmp(buf, d) == 0)
                        break;

                    write(pipefd[1], buf, strlen(buf));
                    write(pipefd[1], "\n", 1);
                }

                close(pipefd[1]);  // 关闭写端，发送EOF
                exit(0);           // 子进程退出
            } else {               // 父进程继续执行
                close(pipefd[1]);  // 关闭写端

                // 重定向标准输入到管道的读端
                dup2(pipefd[0], 0);
                close(pipefd[0]);

                // 等待子进程完成输入
                wait(NULL);

                // 递归执行子命令
                runcmd(rcmd->cmd);
            }
        } else {
            int fd = open(rcmd->file, rcmd->mode);
            if (fd < 0) {
                fprintf(stderr, "open %s failed\n", rcmd->file);
                exit(1);
            }
            // 使用 dup3 将文件描述符重定向到指定的 fd
            if (dup3(fd, rcmd->fd, 0) < 0) {  // flags 设置为 0
                fprintf(stderr, "dup3 failed\n");
                PANIC("dup3 failed");
            }

            // 关闭不再需要的文件描述符
            close(fd);

            // 递归执行子命令
            runcmd(rcmd->cmd);
        }
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
        if (fork1() == 0)
            runcmd(lcmd->left);
        wait(NULL);
        runcmd(lcmd->right);
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        if (pipe(p) < 0)
            PANIC("pipe");
        if (fork1() == 0) {
            close(1);
            dup(p[1]);
            close(p[0]);
            close(p[1]);
            runcmd(pcmd->left);
        }
        if (fork1() == 0) {
            close(0);
            dup(p[0]);
            close(p[0]);
            close(p[1]);
            runcmd(pcmd->right);
        }
        close(p[0]);
        close(p[1]);
        wait(NULL);
        wait(NULL);
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;
        if (fork1() == 0)
            runcmd(bcmd->cmd);
        break;
    }
    exit(0);
}

int getcmd(char *buf, int nbuf)
{
    fprintf(stderr, "$ ");
    memset(buf, 0, nbuf);
    fgets(buf, nbuf - 1, stdin);
    if (buf[0] == 0) // EOF
        return -1;
    return 0;
}

int main(int argc, char *argv[])
{
    for (int i = 0; i < argc; i++) {
        printf("sh: argv[%d] = '%s'\n", i, argv[i]);
    }
    char *test_env = getenv("TEST_ENV");
    if (test_env) {
        printf("sh: testenv = '%s'\n", test_env);
    } else {
        printf("sh: testenv not found!\n");
    }

    static char buf[BUFSIZ];
    int fd;

    // Ensure that three file descriptors are open.
    while ((fd = open("console", O_RDWR)) >= 0) {
        if (fd >= 3) {
            close(fd);
            break;
        }
    }

    // Read and run input commands.
    while (getcmd(buf, sizeof(buf)) >= 0) {
        if (buf[0] == 'c' && buf[1] == 'd' && buf[2] == ' ') {
            // Chdir must be called by the parent, not the child.
            buf[strlen(buf) - 1] = 0; // chop \n
            if (chdir(buf + 3) < 0)
                fprintf(stderr, "cannot cd %s\n", buf + 3);
            continue;
        }
        if (fork1() == 0)
            runcmd(parsecmd(buf));
        wait(NULL);
    }
}

int fork1(void)
{
    int pid;

    pid = fork();
    if (pid == -1)
        PANIC("fork");
    return pid;
}

// Constructors

struct cmd *execcmd(void)
{
    struct execcmd *cmd;

    cmd = malloc1(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = EXEC;
    return (struct cmd *)cmd;
}

struct cmd *redircmd(struct cmd *subcmd, char *file, char *efile, int mode,
                     int fd)
{
    struct redircmd *cmd;

    cmd = malloc1(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = REDIR;
    cmd->cmd = subcmd;
    cmd->file = file;
    cmd->efile = efile;
    cmd->mode = mode;
    cmd->fd = fd;
    return (struct cmd *)cmd;
}

struct cmd *pipecmd(struct cmd *left, struct cmd *right)
{
    struct pipecmd *cmd;

    cmd = malloc1(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = PIPE;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}

struct cmd *listcmd(struct cmd *left, struct cmd *right)
{
    struct listcmd *cmd;

    cmd = malloc1(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = LIST;
    cmd->left = left;
    cmd->right = right;
    return (struct cmd *)cmd;
}

struct cmd *backcmd(struct cmd *subcmd)
{
    struct backcmd *cmd;

    cmd = malloc1(sizeof(*cmd));
    memset(cmd, 0, sizeof(*cmd));
    cmd->type = BACK;
    cmd->cmd = subcmd;
    return (struct cmd *)cmd;
}

// Parsing

char whitespace[] = " \t\r\n\v";
char symbols[] = "<|>&;()";

int gettoken(char **ps, char *es, char **q, char **eq)
{
    char *s;
    int ret;

    s = *ps;
    while (s < es && strchr(whitespace, *s))
        s++;
    if (q)
        *q = s;
    ret = *s;
    switch (*s) {
    case 0:
        break;
    case '|':
    case '(':
    case ')':
    case ';':
    case '&':
    case '<':
        s++;
        if (*s == '<') {
            ret = 'h';  // heredoc
            s++;
        }
        break;
    case '>':
        s++;
        if (*s == '>') {
            ret = '+';
            s++;
        }
        break;
    default:
        ret = 'a';
        while (s < es && !strchr(whitespace, *s) && !strchr(symbols, *s))
            s++;
        break;
    }
    if (eq)
        *eq = s;

    while (s < es && strchr(whitespace, *s))
        s++;
    *ps = s;
    return ret;
}

int peek(char **ps, char *es, char *toks)
{
    char *s;

    s = *ps;
    while (s < es && strchr(whitespace, *s))
        s++;
    *ps = s;
    return *s && strchr(toks, *s);
}

struct cmd *parseline(char **, char *);
struct cmd *parsepipe(char **, char *);
struct cmd *parseexec(char **, char *);
struct cmd *nulterminate(struct cmd *);

struct cmd *parsecmd(char *s)
{
    char *es;
    struct cmd *cmd;

    es = s + strlen(s);
    cmd = parseline(&s, es);
    peek(&s, es, "");
    if (s != es) {
        fprintf(stderr, "leftovers: %s\n", s);
        PANIC("syntax");
    }
    nulterminate(cmd);
    return cmd;
}

struct cmd *parseline(char **ps, char *es)
{
    struct cmd *cmd;

    cmd = parsepipe(ps, es);
    while (peek(ps, es, "&")) {
        gettoken(ps, es, 0, 0);
        cmd = backcmd(cmd);
    }
    if (peek(ps, es, ";")) {
        gettoken(ps, es, 0, 0);
        cmd = listcmd(cmd, parseline(ps, es));
    }
    return cmd;
}

struct cmd *parsepipe(char **ps, char *es)
{
    struct cmd *cmd;

    cmd = parseexec(ps, es);
    if (peek(ps, es, "|")) {
        gettoken(ps, es, 0, 0);
        cmd = pipecmd(cmd, parsepipe(ps, es));
    }
    return cmd;
}

struct cmd *parseredirs(struct cmd *cmd, char **ps, char *es)
{
    int tok;
    char *q, *eq;

    while (peek(ps, es, "<>")) {
        tok = gettoken(ps, es, 0, 0);
        if (gettoken(ps, es, &q, &eq) != 'a')
            PANIC("missing file for redirection");
        switch (tok) {
        case '<':
            cmd = redircmd(cmd, q, eq, O_RDONLY, 0);
            break;
        case '>':
            cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREAT | O_TRUNC, 1);
            break;
        case '+':  // >>
            cmd = redircmd(cmd, q, eq, O_WRONLY | O_CREAT | O_APPEND, 1);
            break;
        case 'h':  // <<
            cmd = redircmd(cmd, q, eq, O_RDONLY | REDIR_HEREDOC, 0);
            break;
        }
    }
    return cmd;
}

struct cmd *parseblock(char **ps, char *es)
{
    struct cmd *cmd;

    if (!peek(ps, es, "("))
        PANIC("parseblock");
    gettoken(ps, es, 0, 0);
    cmd = parseline(ps, es);
    if (!peek(ps, es, ")"))
        PANIC("syntax - missing )");
    gettoken(ps, es, 0, 0);
    cmd = parseredirs(cmd, ps, es);
    return cmd;
}

struct cmd *parseexec(char **ps, char *es)
{
    char *q, *eq;
    int tok, argc;
    struct execcmd *cmd;
    struct cmd *ret;

    if (peek(ps, es, "("))
        return parseblock(ps, es);

    ret = execcmd();
    cmd = (struct execcmd *)ret;

    argc = 0;
    ret = parseredirs(ret, ps, es);
    while (!peek(ps, es, "|)&;")) {
        if ((tok = gettoken(ps, es, &q, &eq)) == 0)
            break;
        if (tok != 'a')
            PANIC("syntax");
        cmd->argv[argc] = q;
        cmd->eargv[argc] = eq;
        argc++;
        if (argc >= MAXARGS)
            PANIC("too many args");
        ret = parseredirs(ret, ps, es);
    }
    cmd->argv[argc] = 0;
    cmd->eargv[argc] = 0;
    return ret;
}

// NUL-terminate all the counted strings.
struct cmd *nulterminate(struct cmd *cmd)
{
    int i;
    struct backcmd *bcmd;
    struct execcmd *ecmd;
    struct listcmd *lcmd;
    struct pipecmd *pcmd;
    struct redircmd *rcmd;

    if (cmd == 0)
        return 0;

    switch (cmd->type) {
    case EXEC:
        ecmd = (struct execcmd *)cmd;
        for (i = 0; ecmd->argv[i]; i++)
            *ecmd->eargv[i] = 0;
        break;

    case REDIR:
        rcmd = (struct redircmd *)cmd;
        nulterminate(rcmd->cmd);
        *rcmd->efile = 0;
        break;

    case PIPE:
        pcmd = (struct pipecmd *)cmd;
        nulterminate(pcmd->left);
        nulterminate(pcmd->right);
        break;

    case LIST:
        lcmd = (struct listcmd *)cmd;
        nulterminate(lcmd->left);
        nulterminate(lcmd->right);
        break;

    case BACK:
        bcmd = (struct backcmd *)cmd;
        nulterminate(bcmd->cmd);
        break;
    }
    return cmd;
}
