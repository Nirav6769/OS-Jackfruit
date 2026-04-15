/*
 * engine.c — Multi-container runtime supervisor
 *
 * Usage:
 *   sudo ./engine supervisor <rootfs-base>           # start the long-running supervisor
 *   sudo ./engine start <name> <rootfs> <cmd> [args] [--soft-mib N] [--hard-mib N]
 *   sudo ./engine run   <name> <rootfs> <cmd> [args]
 *   sudo ./engine ps
 *   sudo ./engine logs  <name>
 *   sudo ./engine stop  <name>
 *
 * The CLI client talks to the supervisor via a UNIX domain socket at
 * /tmp/engine.sock.  The logging pipeline uses a pipe + bounded buffer
 * with a dedicated consumer thread writing per-container log files.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/syscall.h>
#include <sys/ioctl.h>
#include <sched.h>
#include "monitor_ioctl.h"

/* ─── constants ──────────────────────────────────────────────────────────── */

#define MAX_CONTAINERS   16
#define SOCK_PATH        "/tmp/engine.sock"
#define LOG_DIR          "/tmp/engine-logs"
#define MONITOR_DEV      "/dev/container_monitor"
#define BUF_SLOTS        256      /* bounded-buffer slot count               */
#define BUF_SLOT_SIZE    4096     /* bytes per slot                          */

/* ─── container state ────────────────────────────────────────────────────── */

typedef enum {
    STATE_EMPTY = 0,
    STATE_STARTING,
    STATE_RUNNING,
    STATE_STOPPED,
    STATE_KILLED,
} ContainerState;

static const char *state_name(ContainerState s) {
    switch (s) {
        case STATE_STARTING: return "starting";
        case STATE_RUNNING:  return "running";
        case STATE_STOPPED:  return "stopped";
        case STATE_KILLED:   return "killed";
        default:             return "empty";
    }
}

typedef struct {
    char           name[64];
    pid_t          pid;
    time_t         start_time;
    ContainerState state;
    unsigned long  soft_limit_bytes;
    unsigned long  hard_limit_bytes;
    char           log_path[256];
    int            exit_status;
    int            log_pipe_read_fd;   /* supervisor reads from here */
} Container;

/* ─── bounded log buffer ─────────────────────────────────────────────────── */

typedef struct {
    char   data[BUF_SLOT_SIZE];
    int    len;
    char   container_name[64];
} LogSlot;

static LogSlot       log_buf[BUF_SLOTS];
static int           buf_head = 0;     /* producer writes here */
static int           buf_tail = 0;     /* consumer reads here  */
static int           buf_count = 0;
static pthread_mutex_t buf_mutex  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  buf_not_empty = PTHREAD_COND_INITIALIZER;
static pthread_cond_t  buf_not_full  = PTHREAD_COND_INITIALIZER;
static int log_consumer_running = 1;

/* ─── container table ────────────────────────────────────────────────────── */

static Container     containers[MAX_CONTAINERS];
static pthread_mutex_t ct_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ─── monitor fd ─────────────────────────────────────────────────────────── */
static int monitor_fd = -1;

/* ─── forward declarations ───────────────────────────────────────────────── */
static void supervisor_loop(void);
static void handle_client_command(int client_fd);
static void *log_consumer_thread(void *arg);
static void *log_reader_thread(void *arg);
static int  do_start_container(const char *name, const char *rootfs,
                                char *const argv[], int foreground,
                                unsigned long soft_mib, unsigned long hard_mib);
static void reap_children(void);
static void shutdown_supervisor(int sig);

/* ─── utilities ──────────────────────────────────────────────────────────── */

static void die(const char *msg) {
    perror(msg);
    exit(1);
}

static Container *find_container(const char *name) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state != STATE_EMPTY &&
            strcmp(containers[i].name, name) == 0)
            return &containers[i];
    return NULL;
}

static Container *alloc_container(void) {
    for (int i = 0; i < MAX_CONTAINERS; i++)
        if (containers[i].state == STATE_EMPTY)
            return &containers[i];
    return NULL;
}

static void register_with_monitor(Container *c) {
    if (monitor_fd < 0) return;
    struct container_limits lim = {
        .pid              = c->pid,
        .soft_limit_bytes = c->soft_limit_bytes,
        .hard_limit_bytes = c->hard_limit_bytes,
    };
    strncpy(lim.name, c->name, sizeof(lim.name) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &lim) < 0)
        perror("[engine] ioctl MONITOR_REGISTER");
}

static void unregister_with_monitor(pid_t pid) {
    if (monitor_fd < 0) return;
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &pid) < 0)
        perror("[engine] ioctl MONITOR_UNREGISTER");
}

/* ─── bounded buffer: producer ───────────────────────────────────────────── */

static void buf_produce(const char *container_name, const char *data, int len) {
    pthread_mutex_lock(&buf_mutex);
    while (buf_count == BUF_SLOTS)
        pthread_cond_wait(&buf_not_full, &buf_mutex);

    LogSlot *slot = &log_buf[buf_head];
    int copy_len = len < BUF_SLOT_SIZE ? len : BUF_SLOT_SIZE - 1;
    memcpy(slot->data, data, copy_len);
    slot->data[copy_len] = '\0';
    slot->len = copy_len;
    strncpy(slot->container_name, container_name, 63);

    buf_head = (buf_head + 1) % BUF_SLOTS;
    buf_count++;
    pthread_cond_signal(&buf_not_empty);
    pthread_mutex_unlock(&buf_mutex);
}

/* ─── log consumer thread ────────────────────────────────────────────────── */

static void *log_consumer_thread(void *arg) {
    (void)arg;
    while (1) {
        pthread_mutex_lock(&buf_mutex);
        while (buf_count == 0 && log_consumer_running)
            pthread_cond_wait(&buf_not_empty, &buf_mutex);
        if (buf_count == 0 && !log_consumer_running) {
            pthread_mutex_unlock(&buf_mutex);
            break;
        }
        LogSlot slot = log_buf[buf_tail];
        buf_tail = (buf_tail + 1) % BUF_SLOTS;
        buf_count--;
        pthread_cond_signal(&buf_not_full);
        pthread_mutex_unlock(&buf_mutex);

        /* Find the container's log file and write */
        pthread_mutex_lock(&ct_mutex);
        Container *c = find_container(slot.container_name);
        char log_path[256] = {0};
        if (c) strncpy(log_path, c->log_path, sizeof(log_path) - 1);
        pthread_mutex_unlock(&ct_mutex);

        if (log_path[0]) {
            FILE *f = fopen(log_path, "a");
            if (f) {
                fwrite(slot.data, 1, slot.len, f);
                fclose(f);
            }
        }
    }
    return NULL;
}

/* ─── per-container log reader thread ───────────────────────────────────── */

typedef struct { char name[64]; int fd; } LogReaderArg;

static void *log_reader_thread(void *arg) {
    LogReaderArg *a = arg;
    char buf[BUF_SLOT_SIZE];
    ssize_t n;
    while ((n = read(a->fd, buf, sizeof(buf))) > 0)
        buf_produce(a->name, buf, (int)n);
    close(a->fd);
    free(a);
    return NULL;
}

/* ─── container pivot/exec (runs in child after clone) ──────────────────── */

typedef struct {
    char  rootfs[512];
    char *argv[64];
    int   pipe_write_fd;
} ChildArgs;

static int container_child(void *arg) {
    ChildArgs *a = arg;

    /* Redirect stdout + stderr into the log pipe */
    dup2(a->pipe_write_fd, STDOUT_FILENO);
    dup2(a->pipe_write_fd, STDERR_FILENO);
    close(a->pipe_write_fd);

    /* Mount /proc inside the new mount namespace */
    if (mount("proc", "/proc", "proc", 0, NULL) < 0 &&
        errno != EBUSY) {
        perror("[child] mount proc");
    }

    /* chroot into the container rootfs */
    if (chroot(a->rootfs) < 0) { perror("[child] chroot"); exit(1); }
    if (chdir("/") < 0)         { perror("[child] chdir"); exit(1); }

    execvp(a->argv[0], a->argv);
    perror("[child] execvp");
    exit(1);
}

/* ─── start a container ──────────────────────────────────────────────────── */

#define CHILD_STACK_SIZE (1 << 20)  /* 1 MiB */

static int do_start_container(const char *name, const char *rootfs,
                               char *const argv[], int foreground,
                               unsigned long soft_mib, unsigned long hard_mib)
{
    /* Allocate metadata slot */
    pthread_mutex_lock(&ct_mutex);
    if (find_container(name)) {
        pthread_mutex_unlock(&ct_mutex);
        fprintf(stderr, "[engine] container '%s' already exists\n", name);
        return -1;
    }
    Container *c = alloc_container();
    if (!c) {
        pthread_mutex_unlock(&ct_mutex);
        fprintf(stderr, "[engine] max containers reached\n");
        return -1;
    }
    memset(c, 0, sizeof(*c));
    strncpy(c->name, name, sizeof(c->name) - 1);
    c->state            = STATE_STARTING;
    c->start_time       = time(NULL);
    c->soft_limit_bytes = soft_mib * 1024 * 1024;
    c->hard_limit_bytes = hard_mib * 1024 * 1024;
    snprintf(c->log_path, sizeof(c->log_path), "%s/%s.log", LOG_DIR, name);
    pthread_mutex_unlock(&ct_mutex);

    /* Create pipe for container output */
    int pipefd[2];
    if (pipe(pipefd) < 0) { perror("pipe"); return -1; }

    /* Build child args */
    ChildArgs *ca = malloc(sizeof(*ca));
    if (!ca) { close(pipefd[0]); close(pipefd[1]); return -1; }
    strncpy(ca->rootfs, rootfs, sizeof(ca->rootfs) - 1);
    int i = 0;
    while (argv[i] && i < 63) { ca->argv[i] = argv[i]; i++; }
    ca->argv[i]       = NULL;
    ca->pipe_write_fd = pipefd[1];

    /* Clone with new namespaces */
    char *stack = malloc(CHILD_STACK_SIZE);
    if (!stack) { free(ca); close(pipefd[0]); close(pipefd[1]); return -1; }

    pid_t pid = clone(container_child, stack + CHILD_STACK_SIZE,
                      CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                      ca);
    close(pipefd[1]);  /* parent closes write end */
    free(stack);
    free(ca);

    if (pid < 0) {
        perror("clone");
        close(pipefd[0]);
        pthread_mutex_lock(&ct_mutex);
        memset(c, 0, sizeof(*c));
        pthread_mutex_unlock(&ct_mutex);
        return -1;
    }

    pthread_mutex_lock(&ct_mutex);
    c->pid              = pid;
    c->state            = STATE_RUNNING;
    c->log_pipe_read_fd = pipefd[0];
    pthread_mutex_unlock(&ct_mutex);

    /* Register with kernel monitor */
    register_with_monitor(c);

    /* Start log reader thread */
    LogReaderArg *lra = malloc(sizeof(*lra));
    strncpy(lra->name, name, 63);
    lra->fd = pipefd[0];
    pthread_t reader_tid;
    pthread_create(&reader_tid, NULL, log_reader_thread, lra);
    pthread_detach(reader_tid);

    printf("[engine] started container '%s' pid=%d\n", name, pid);

    if (foreground) {
        int status;
        waitpid(pid, &status, 0);
        pthread_mutex_lock(&ct_mutex);
        c->state       = STATE_STOPPED;
        c->exit_status = WEXITSTATUS(status);
        pthread_mutex_unlock(&ct_mutex);
        unregister_with_monitor(pid);
        printf("[engine] container '%s' exited with status %d\n", name, c->exit_status);
    }
    return 0;
}

/* ─── SIGCHLD handler ────────────────────────────────────────────────────── */

static void reap_children(void) {
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        pthread_mutex_lock(&ct_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            if (containers[i].pid == pid && containers[i].state == STATE_RUNNING) {
                containers[i].state       = STATE_STOPPED;
                containers[i].exit_status = WEXITSTATUS(status);
                unregister_with_monitor(pid);
                printf("[engine] reaped container '%s' pid=%d exit=%d\n",
                       containers[i].name, pid, containers[i].exit_status);
                break;
            }
        }
        pthread_mutex_unlock(&ct_mutex);
    }
}

static void sigchld_handler(int sig) {
    (void)sig;
    reap_children();
}

static volatile int supervisor_running = 1;

static void shutdown_supervisor(int sig) {
    (void)sig;
    printf("\n[engine] shutting down supervisor...\n");
    supervisor_running = 0;
}

/* ─── supervisor: handle one client command ─────────────────────────────── */

static void handle_client_command(int cfd) {
    char line[1024] = {0};
    ssize_t n = recv(cfd, line, sizeof(line) - 1, 0);
    if (n <= 0) { close(cfd); return; }
    line[n] = '\0';

    /* Tokenise */
    char *tokens[32]; int tc = 0;
    char *p = strtok(line, " \t\n");
    while (p && tc < 31) { tokens[tc++] = p; p = strtok(NULL, " \t\n"); }
    tokens[tc] = NULL;
    if (tc == 0) { close(cfd); return; }

    char reply[4096] = {0};

    if (strcmp(tokens[0], "ps") == 0) {
        int off = 0;
        off += snprintf(reply + off, sizeof(reply) - off,
                        "%-16s %-8s %-10s %-10s %-10s\n",
                        "NAME", "PID", "STATE", "SOFT(MB)", "HARD(MB)");
        pthread_mutex_lock(&ct_mutex);
        for (int i = 0; i < MAX_CONTAINERS; i++) {
            Container *c = &containers[i];
            if (c->state == STATE_EMPTY) continue;
            char ts[32];
            struct tm *tm_info = localtime(&c->start_time);
            strftime(ts, sizeof(ts), "%H:%M:%S", tm_info);
            off += snprintf(reply + off, sizeof(reply) - off,
                            "%-16s %-8d %-10s %-10lu %-10lu\n",
                            c->name, c->pid, state_name(c->state),
                            c->soft_limit_bytes / (1024*1024),
                            c->hard_limit_bytes / (1024*1024));
        }
        pthread_mutex_unlock(&ct_mutex);

    } else if (strcmp(tokens[0], "logs") == 0 && tc >= 2) {
        pthread_mutex_lock(&ct_mutex);
        Container *c = find_container(tokens[1]);
        if (!c) {
            snprintf(reply, sizeof(reply), "ERROR: container '%s' not found\n", tokens[1]);
        } else {
            char log_path[256];
            strncpy(log_path, c->log_path, sizeof(log_path) - 1);
            pthread_mutex_unlock(&ct_mutex);
            FILE *f = fopen(log_path, "r");
            if (!f) {
                snprintf(reply, sizeof(reply), "(no log yet for '%s')\n", tokens[1]);
            } else {
                int off = 0;
                int ch;
                while ((ch = fgetc(f)) != EOF && off < (int)sizeof(reply) - 2)
                    reply[off++] = (char)ch;
                reply[off] = '\0';
                fclose(f);
            }
            goto send_reply;
        }
        pthread_mutex_unlock(&ct_mutex);

    } else if (strcmp(tokens[0], "stop") == 0 && tc >= 2) {
        pthread_mutex_lock(&ct_mutex);
        Container *c = find_container(tokens[1]);
        if (!c) {
            snprintf(reply, sizeof(reply), "ERROR: container '%s' not found\n", tokens[1]);
        } else if (c->state != STATE_RUNNING) {
            snprintf(reply, sizeof(reply), "container '%s' is not running\n", tokens[1]);
        } else {
            pid_t pid = c->pid;
            pthread_mutex_unlock(&ct_mutex);
            kill(pid, SIGTERM);
            usleep(300000);
            kill(pid, SIGKILL);
            snprintf(reply, sizeof(reply), "stopped container '%s'\n", tokens[1]);
            goto send_reply;
        }
        pthread_mutex_unlock(&ct_mutex);

    } else if (strcmp(tokens[0], "start") == 0 && tc >= 4) {
        /* start <name> <rootfs> <cmd> [args...] [--soft-mib N] [--hard-mib N] */
        const char *cname  = tokens[1];
        const char *rootfs = tokens[2];
        char *argv[32]; int argc = 0;
        unsigned long soft_mib = 0, hard_mib = 0;
        for (int i = 3; i < tc; i++) {
            if (strcmp(tokens[i], "--soft-mib") == 0 && i+1 < tc)
                { soft_mib = atol(tokens[++i]); }
            else if (strcmp(tokens[i], "--hard-mib") == 0 && i+1 < tc)
                { hard_mib = atol(tokens[++i]); }
            else
                argv[argc++] = tokens[i];
        }
        argv[argc] = NULL;
        if (argc == 0) {
            snprintf(reply, sizeof(reply), "ERROR: no command given\n");
        } else {
            int r = do_start_container(cname, rootfs, argv, 0, soft_mib, hard_mib);
            snprintf(reply, sizeof(reply), r == 0 ? "started '%s'\n" : "ERROR starting '%s'\n", cname);
        }

    } else {
        snprintf(reply, sizeof(reply), "unknown command: %s\n", tokens[0]);
    }

send_reply:
    send(cfd, reply, strlen(reply), 0);
    close(cfd);
}

/* ─── supervisor main loop ───────────────────────────────────────────────── */

static void supervisor_loop(void) {
    /* Open kernel monitor device if available */
    monitor_fd = open(MONITOR_DEV, O_RDWR);
    if (monitor_fd < 0)
        fprintf(stderr, "[engine] WARNING: could not open %s (kernel module loaded?)\n", MONITOR_DEV);

    /* Set up log directory */
    mkdir(LOG_DIR, 0755);

    /* Start log consumer thread */
    pthread_t consumer_tid;
    pthread_create(&consumer_tid, NULL, log_consumer_thread, NULL);

    /* Set up UNIX domain socket */
    unlink(SOCK_PATH);
    int srv = socket(AF_UNIX, SOCK_STREAM, 0);
    if (srv < 0) die("socket");

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);
    if (bind(srv, (struct sockaddr *)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(srv, 8) < 0) die("listen");

    /* Signal handlers */
    struct sigaction sa_chld = { .sa_handler = sigchld_handler, .sa_flags = SA_RESTART };
    sigemptyset(&sa_chld.sa_mask);
    sigaction(SIGCHLD, &sa_chld, NULL);
    signal(SIGINT,  shutdown_supervisor);
    signal(SIGTERM, shutdown_supervisor);

    printf("[engine] supervisor ready, socket at %s\n", SOCK_PATH);

    /* Accept loop */
    fd_set rfds;
    struct timeval tv;
    while (supervisor_running) {
        FD_ZERO(&rfds);
        FD_SET(srv, &rfds);
        tv.tv_sec = 1; tv.tv_usec = 0;
        int r = select(srv + 1, &rfds, NULL, NULL, &tv);
        if (r < 0 && errno == EINTR) continue;
        if (r > 0 && FD_ISSET(srv, &rfds)) {
            int cfd = accept(srv, NULL, NULL);
            if (cfd >= 0) handle_client_command(cfd);
        }
    }

    /* Cleanup */
    close(srv);
    unlink(SOCK_PATH);

    /* Kill all running containers */
    pthread_mutex_lock(&ct_mutex);
    for (int i = 0; i < MAX_CONTAINERS; i++) {
        if (containers[i].state == STATE_RUNNING)
            kill(containers[i].pid, SIGKILL);
    }
    pthread_mutex_unlock(&ct_mutex);
    sleep(1);
    reap_children();

    /* Stop consumer thread */
    pthread_mutex_lock(&buf_mutex);
    log_consumer_running = 0;
    pthread_cond_signal(&buf_not_empty);
    pthread_mutex_unlock(&buf_mutex);
    pthread_join(consumer_tid, NULL);

    if (monitor_fd >= 0) close(monitor_fd);
    printf("[engine] supervisor exited cleanly\n");
}

/* ─── CLI client: send command to supervisor ─────────────────────────────── */

static int send_to_supervisor(int argc, char *argv[]) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is supervisor running?)");
        close(fd);
        return 1;
    }

    /* Join args into a single space-separated string */
    char msg[1024] = {0};
    int off = 0;
    for (int i = 0; i < argc; i++) {
        if (off) msg[off++] = ' ';
        int n = snprintf(msg + off, sizeof(msg) - off, "%s", argv[i]);
        off += n;
    }

    send(fd, msg, strlen(msg), 0);

    /* Print reply */
    char buf[4096];
    ssize_t n;
    while ((n = recv(fd, buf, sizeof(buf) - 1, 0)) > 0) {
        buf[n] = '\0';
        printf("%s", buf);
    }
    close(fd);
    return 0;
}

/* ─── main ───────────────────────────────────────────────────────────────── */

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr,
            "Usage:\n"
            "  sudo ./engine supervisor <rootfs-base>\n"
            "  sudo ./engine start <name> <rootfs> <cmd> [--soft-mib N] [--hard-mib N]\n"
            "  sudo ./engine run   <name> <rootfs> <cmd>\n"
            "  sudo ./engine ps\n"
            "  sudo ./engine logs  <name>\n"
            "  sudo ./engine stop  <name>\n");
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0) {
        supervisor_loop();
        return 0;
    }

    /* Everything else is a CLI command forwarded to the supervisor */
    return send_to_supervisor(argc - 1, argv + 1);
}
