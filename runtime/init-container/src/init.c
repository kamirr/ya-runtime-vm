#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdnoreturn.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "communication.h"
#include "cyclic_buffer.h"
#include "network.h"
#include "process_bookkeeping.h"
#include "proto.h"
#include "forward.h"

#define CONTAINER_OF(ptr, type, member) (type*)((char*)(ptr) - offsetof(type, member))

// XXX: maybe obtain this with sysconf?
#define PAGE_SIZE 0x1000

#define DEFAULT_UID 0
#define DEFAULT_GID 0
#define DEFAULT_OUT_FILE_PERM S_IRWXU
#define DEFAULT_DIR_PERMS (S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH)
#define DEFAULT_FD_DESC {           \
        .type = REDIRECT_FD_FILE,   \
        .path = NULL,               \
    }

#define VPORT_CMD "/dev/vport0p1"
#define VPORT_NET "/dev/vport0p2"
#define VPORT_INET "/dev/vport0p3"

#define DEV_VPN "eth0"
#define DEV_INET "eth1"

#define MODE_RW_UGO (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH)
#define OUTPUT_PATH_PREFIX "/var/tmp/guest_agent_private/fds"

#define NET_MEM_DEFAULT 1048576
#define NET_MEM_MAX 2097152
#define MTU_VPN 1220
#define MTU_INET 65521


struct new_process_args {
    char* bin;
    char** argv;
    char** envp;
    uint32_t uid;
    uint32_t gid;
    char* cwd;
    bool is_entrypoint;
};

enum epoll_fd_type {
    EPOLL_FD_CMDS,
    EPOLL_FD_SIG,
    EPOLL_FD_OUT,
    EPOLL_FD_IN,
};

struct epoll_fd_desc {
    enum epoll_fd_type type;
    int fd;
    int src_fd;
    struct redir_fd_desc* data;
};

extern char** environ;

static int g_cmds_fd = -1;
static int g_sig_fd = -1;
static int g_epoll_fd = -1;
static int g_vpn_fd = -1;
static int g_vpn_tap_fd = -1;
static int g_inet_fd = -1;
static int g_inet_tap_fd = -1;

static char g_lo_name[16];
static char g_vpn_tap_name[16];
static char g_inet_tap_name[16];

static struct process_desc* g_entrypoint_desc = NULL;

static noreturn void die(void) {
    sync();
    (void)close(g_epoll_fd);
    (void)close(g_sig_fd);
    (void)close(g_inet_fd);
    (void)close(g_vpn_fd);
    (void)close(g_cmds_fd);

    while (1) {
        (void)reboot(RB_POWER_OFF);
        __asm__ volatile ("hlt");
    }
}

#define CHECK(x) ({                                                     \
    __typeof__(x) _x = (x);                                             \
    if (_x == -1) {                                                     \
        fprintf(stderr, "Error at %s:%d: %m\n", __FILE__, __LINE__);    \
        die();                                                          \
    }                                                                   \
    _x;                                                                 \
})

static void load_module(const char* path) {
    int fd = CHECK(open(path, O_RDONLY | O_CLOEXEC));
    CHECK(syscall(SYS_finit_module, fd, "", 0));
    CHECK(close(fd));
}

int make_nonblocking(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFL);
    if (flags == -1 && errno) {
        return -1;
    }
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        return -1;
    }
    return 0;
}

/*
int make_cloexec(int fd) {
    errno = 0;
    int flags = fcntl(fd, F_GETFD);
    if (flags == -1 && errno) {
        return -1;
    }
    if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) < 0) {
        return -1;
    }
    return 0;
}
*/

static void cleanup_fd_desc(struct redir_fd_desc* fd_desc) {
    switch (fd_desc->type) {
        case REDIRECT_FD_FILE:
            free(fd_desc->path);
            break;
        case REDIRECT_FD_PIPE_BLOCKING:
        case REDIRECT_FD_PIPE_CYCLIC:
            if (fd_desc->buffer.fds[0] != -1) {
                close(fd_desc->buffer.fds[0]);
            }
            if (fd_desc->buffer.fds[1] != -1) {
                close(fd_desc->buffer.fds[1]);
            }
            cyclic_buffer_deinit(&fd_desc->buffer.cb);
            break;
        default:
            break;
    }
    fd_desc->type = REDIRECT_FD_INVALID;
}

static bool redir_buffers_empty(struct redir_fd_desc *redirs, size_t len) {
    FILE *f;
    for (size_t fd = 0; fd < len; ++fd) {
        switch (redirs[fd].type) {
            case REDIRECT_FD_FILE:
                if ((f = fopen(redirs[fd].path, "r")) == 0) {
                    continue;
                }
                fseek(f, 0, SEEK_END);
                bool empty = ftell(f) == 0;
                fclose(f);

                if (!empty) {
                    return false;
                }
                break;
            case REDIRECT_FD_PIPE_BLOCKING:
            case REDIRECT_FD_PIPE_CYCLIC:
                if (cyclic_buffer_data_size(&redirs[fd].buffer.cb) != 0) {
                    return false;
                }
                break;
            default:
                break;
        }
    }
    return true;
}

__attribute__((unused)) static void delete_proc(struct process_desc* proc_desc) {
    remove_process(proc_desc);
    for (size_t fd = 0; fd < 3; ++fd) {
        cleanup_fd_desc(&proc_desc->redirs[fd]);
    }
    free(proc_desc);
}

struct exit_reason {
    uint8_t status;
    uint8_t type;
};

static void send_process_died(uint64_t id, struct exit_reason reason) {
    struct msg_hdr resp = {
        .msg_id = 0,
        .type = NOTIFY_PROCESS_DIED,
    };

    CHECK(writen(g_cmds_fd, &resp, sizeof(resp)));
    CHECK(writen(g_cmds_fd, &id, sizeof(id)));
    CHECK(writen(g_cmds_fd, &reason.status, sizeof(reason.status)));
    CHECK(writen(g_cmds_fd, &reason.type, sizeof(reason.type)));
}

static struct exit_reason encode_status(int status, int type) {
    struct exit_reason exit_reason;

    switch (type) {
        case CLD_EXITED:
            exit_reason.type = 0;
            break;
        case CLD_KILLED:
            exit_reason.type = 1;
            break;
        case CLD_DUMPED:
            exit_reason.type = 2;
            break;
        default:
            fprintf(stderr, "Invalid exit reason to encode: %d\n", type);
            die();
    }

    exit_reason.status = (status & 0xff);

    return exit_reason;
}

static void handle_sigchld(void) {
    struct signalfd_siginfo siginfo = { 0 };

    if (read(g_sig_fd, &siginfo, sizeof(siginfo)) != sizeof(siginfo)) {
        fprintf(stderr, "Invalid signalfd read: %m\n");
        die();
    }

    if (siginfo.ssi_signo != SIGCHLD) {
        fprintf(stderr, "BUG: read unexpected signal from signalfd: %d\n",
                siginfo.ssi_signo);
        die();
    }

    pid_t child_pid = (pid_t)siginfo.ssi_pid;

    if (siginfo.ssi_code != CLD_EXITED
            && siginfo.ssi_code != CLD_KILLED
            && siginfo.ssi_code != CLD_DUMPED) {
        /* Received spurious SIGCHLD - ignore it. */
        return;
    }

    pid_t w_pid = waitpid(child_pid, NULL, WNOHANG);
    if (w_pid != child_pid) {
        fprintf(stderr, "Error at waitpid: %d: %m\n", w_pid);
        return;
    }

    struct process_desc* proc_desc = find_process_by_pid(child_pid);
    if (!proc_desc) {
        /* This process was not tracked. */
        return;
    }

    proc_desc->is_alive = false;

    send_process_died(proc_desc->id, encode_status(siginfo.ssi_status,
                      siginfo.ssi_code));

    if (proc_desc == g_entrypoint_desc) {
        fprintf(stderr, "Entrypoint exited\n");
        CHECK(kill(-1, SIGKILL));
        die();
    }

    if (redir_buffers_empty(proc_desc->redirs, 3)) {
        delete_proc(proc_desc);
    }
}

static void block_signals(void) {
    sigset_t set;
    CHECK(sigemptyset(&set));
    CHECK(sigaddset(&set, SIGCHLD));
    CHECK(sigaddset(&set, SIGPIPE));
    CHECK(sigprocmask(SIG_BLOCK, &set, NULL));
}

static void setup_sigfd(void) {
    sigset_t set;
    CHECK(sigemptyset(&set));
    CHECK(sigaddset(&set, SIGCHLD));
    g_sig_fd = CHECK(signalfd(g_sig_fd, &set, SFD_CLOEXEC));
}

static int create_dir_path(char* path) {
    assert(path[0] == '/');

    char* next = path;
    while (1) {
        next = strchr(next + 1, '/');
        if (!next) {
            break;
        }
        *next = '\0';
        int ret = mkdir(path, DEFAULT_DIR_PERMS);
        *next = '/';
        if (ret < 0 && errno != EEXIST) {
            return -1;
        }
    }

    if (mkdir(path, DEFAULT_DIR_PERMS) < 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

static void setup_agent_directories(void) {
    char* path = strdup(OUTPUT_PATH_PREFIX);
    if (!path) {
        fprintf(stderr, "setup_agent_directories OOM\n");
        die();
    }

    CHECK(create_dir_path(path));

    free(path);
}

static int add_network_hosts(char *entries[][2], int n) {
    FILE *f;
    if ((f = fopen("/etc/hosts", "a")) == 0) {
        return -1;
    }

    for (int i = 0; i < n; ++i) {
        fprintf(f, "%s\t%s\n", entries[i][0], entries[i][1]);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return 0;
}

static int set_network_ns(char *entries[], int n) {
    FILE *f;
    if ((f = fopen("/etc/resolv.conf", "w")) == 0) {
        return -1;
    }

    fprintf(f, "search example.com\n");
    for (int i = 0; i < n; ++i) {
        fprintf(f, "nameserver %s\n", entries[i]);
    }

    fflush(f);
    fsync(fileno(f));
    fclose(f);

    return 0;
}

int write_sys(char *path, size_t value) {
    FILE *f;
    if ((f = fopen(path, "w")) == 0) {
        return -1;
    }

    fprintf(f, "%ld", value);
    fflush(f);
    fclose(f);

    return 0;
}

static void setup_network(void) {
    char *hosts[][2] = {
        {"127.0.0.1",   "localhost"},
        {"::1",         "ip6-localhost ip6-loopback"},
        {"fe00::0",     "ip6-localnet"},
        {"ff00::0",     "ip6-mcastprefix"},
        {"ff02::1",     "ip6-allnodes"},
        {"ff02::2",     "ip6-allrouters"},
    };
    char *nameservers[] = {
        "1.1.1.1",
        "8.8.8.8",
    };

    strcpy(g_lo_name, "lo");
    strcpy(g_vpn_tap_name, "vpn%d");
    strcpy(g_inet_tap_name, "inet%d");

    CHECK(add_network_hosts(hosts, sizeof(hosts) / sizeof(*hosts)));
    CHECK(set_network_ns(nameservers, sizeof(nameservers) / sizeof(*nameservers)));

    CHECK(net_create_lo(g_lo_name));
    CHECK(net_if_addr(g_lo_name, "127.0.0.1", "255.255.255.0"));

    CHECK(write_sys("/proc/sys/net/core/rmem_default", NET_MEM_DEFAULT));
    CHECK(write_sys("/proc/sys/net/core/rmem_max", NET_MEM_MAX));
    CHECK(write_sys("/proc/sys/net/core/wmem_default", NET_MEM_DEFAULT));
    CHECK(write_sys("/proc/sys/net/core/wmem_max", NET_MEM_MAX));

    // FIXME: VPORT_NET and VPORT_INET are only present when supervised by a legacy ExeUnit
    if (access(VPORT_NET, F_OK) == 0) {
        int vpn_sz = 4 * (MTU_VPN + 14);

        g_vpn_fd = CHECK(open(VPORT_NET, O_RDWR | O_CLOEXEC));
        g_vpn_tap_fd = CHECK(net_create_tap(g_vpn_tap_name));

        CHECK(net_if_mtu(g_vpn_tap_name, MTU_VPN));
        CHECK(fwd_start(g_vpn_tap_fd, g_vpn_fd, vpn_sz, false, true));
        CHECK(fwd_start(g_vpn_fd, g_vpn_tap_fd, vpn_sz, true, false));
    } else {
        net_if_mtu(DEV_VPN, MTU_VPN);
    }

    if (access(VPORT_INET, F_OK) == 0) {
        int inet_sz = MTU_INET + 14;

        g_inet_fd = CHECK(open(VPORT_INET, O_RDWR | O_CLOEXEC));
        g_inet_tap_fd = CHECK(net_create_tap(g_inet_tap_name));

        CHECK(net_if_mtu(g_inet_tap_name, MTU_INET));
        CHECK(fwd_start(g_inet_tap_fd, g_inet_fd, inet_sz, false, true));
        CHECK(fwd_start(g_inet_fd, g_inet_tap_fd, inet_sz, true, false));
    } else {
        net_if_mtu(DEV_INET, MTU_INET);
    }
}

static void stop_network(void) {
    fwd_stop();
}

static void send_response_hdr(msg_id_t msg_id, enum GUEST_MSG_TYPE type) {
    struct msg_hdr resp = {
        .msg_id = msg_id,
        .type = type,
    };
    CHECK(writen(g_cmds_fd, &resp, sizeof(resp)));
}

static void send_response_ok(msg_id_t msg_id) {
    send_response_hdr(msg_id, RESP_OK);
}

static void send_response_err(msg_id_t msg_id, uint32_t ret_val) {
    send_response_hdr(msg_id, RESP_ERR);
    CHECK(writen(g_cmds_fd, &ret_val, sizeof(ret_val)));
}

static void send_response_u64(msg_id_t msg_id, uint64_t ret_val) {
    send_response_hdr(msg_id, RESP_OK_U64);
    CHECK(writen(g_cmds_fd, &ret_val, sizeof(ret_val)));
}

static void send_response_bytes(msg_id_t msg_id, const char* buf, size_t len) {
    send_response_hdr(msg_id, RESP_OK_BYTES);
    CHECK(send_bytes(g_cmds_fd, buf, len));
}

static void send_response_cyclic_buffer(msg_id_t msg_id, struct cyclic_buffer* cb, size_t len) {
    send_response_hdr(msg_id, RESP_OK_BYTES);
    CHECK(send_bytes_cyclic_buffer(g_cmds_fd, cb, len));
}

static noreturn void handle_quit(msg_id_t msg_id) {
    send_response_ok(msg_id);
    die();
}

static int add_epoll_fd_desc(struct redir_fd_desc* redir_fd_desc,
                             int fd,
                             int src_fd,
                             struct epoll_fd_desc** epoll_fd_desc_ptr) {
    struct epoll_fd_desc* epoll_fd_desc = malloc(sizeof(*epoll_fd_desc));
    if (!epoll_fd_desc) {
        return -1;
    }

    epoll_fd_desc->type = (src_fd == 0) ? EPOLL_FD_OUT : EPOLL_FD_IN;
    epoll_fd_desc->fd = fd;
    epoll_fd_desc->src_fd = src_fd;
    epoll_fd_desc->data = redir_fd_desc;

    struct epoll_event event = {
        .events = (src_fd == 0) ? EPOLLOUT : EPOLLIN,
        .data.ptr = epoll_fd_desc,
    };

    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, fd, &event) < 0) {
        int tmp = errno;
        free(epoll_fd_desc);
        errno = tmp;
        return -1;
    }

    if (epoll_fd_desc_ptr) {
        *epoll_fd_desc_ptr = epoll_fd_desc;
    }
    return 0;

}

static int del_epoll_fd_desc(struct epoll_fd_desc* epoll_fd_desc) {
    if (epoll_ctl(g_epoll_fd, EPOLL_CTL_DEL, epoll_fd_desc->fd, NULL) < 0) {
        return -1;
    }
    free(epoll_fd_desc);
    return 0;
}

/* Assumes fd is either 0, 1 or 2.
 * Returns whether call was successful (setting errno on failures). */
static bool redirect_fd_to_path(int fd, const char* path) {
    assert(fd == 0 || fd == 1 || fd == 2);

    int source_fd = -1;
    if (fd == 0) {
        source_fd = open(path, O_RDONLY);
    } else {
        source_fd = open(path, O_WRONLY | O_CREAT, DEFAULT_OUT_FILE_PERM);
    }

    if (source_fd < 0) {
        return false;
    }

    if (source_fd != fd) {
        if (dup2(source_fd, fd) < 0) {
            int tmp_errno = errno;
            (void)close(source_fd);
            errno = tmp_errno;
            return false;
        }
        if (close(source_fd) < 0) {
            int tmp_errno = errno;
            (void)close(fd);
            errno = tmp_errno;
            return false;
        }
    }

    return true;
}

// lives in a separate memory segment (after forking)
static int child_pipe = -1;

static void close_child_pipe() {
    if (child_pipe != -1) {
        char c = '\0';
        /* Can't do anything with errors here. */
        (void)write(child_pipe, &c, sizeof(c));
        close(child_pipe);
    }
}

static noreturn void child_wrapper(int parent_pipe[2],
                                   struct new_process_args* new_proc_args,
                                   struct redir_fd_desc fd_descs[3]) {
    child_pipe = parent_pipe[1];
    atexit(close_child_pipe);

    if (close(parent_pipe[0]) < 0) {
        goto out;
    }

    sigset_t set;
    if (sigemptyset(&set) < 0) {
        goto out;
    }
    if (sigprocmask(SIG_SETMASK, &set, NULL) < 0) {
        goto out;
    }

    if (new_proc_args->cwd) {
        if (chdir(new_proc_args->cwd) < 0) {
            goto out;
        }
    }

    for (int fd = 0; fd < 3; ++fd) {
        switch (fd_descs[fd].type) {
            case REDIRECT_FD_FILE:
                if (!redirect_fd_to_path(fd, fd_descs[fd].path)) {
                    goto out;
                }
                break;
            case REDIRECT_FD_PIPE_BLOCKING:
            case REDIRECT_FD_PIPE_CYCLIC:
                if (dup2(fd_descs[fd].buffer.fds[fd ? 1 : 0], fd) < 0) {
                    goto out;
                }
                if (close(fd_descs[fd].buffer.fds[0]) < 0
                        || close(fd_descs[fd].buffer.fds[1]) < 0) {
                    goto out;
                }
                break;
            default:
                errno = ENOTRECOVERABLE;
                goto out;
        }
    }

    gid_t gid = new_proc_args->gid;
    if (setresgid(gid, gid, gid) < 0) {
        goto out;
    }

    uid_t uid = new_proc_args->uid;
    if (setresuid(uid, uid, uid) < 0) {
        goto out;
    }

    /* If execve returns we know an error happened. */
    (void)execve(new_proc_args->bin,
                 new_proc_args->argv,
                 new_proc_args->envp ?: environ);

out:
    exit(errno);
}

/* 0 is considered an invalid ID. */
static uint64_t get_next_id(void) {
    static uint64_t id = 0;
    return ++id;
}

static int create_process_fds_dir(uint64_t id) {
    char* path = NULL;
    if (asprintf(&path, OUTPUT_PATH_PREFIX "/%llu", id) < 0) {
        return -1;
    }

    if (mkdir(path, S_IRWXU) < 0) {
        int tmp = errno;
        free(path);
        errno = tmp;
        return -1;
    }

    free(path);
    return 0;
}

static char* construct_output_path(uint64_t id, unsigned int fd) {
    char* path = NULL;
    if (asprintf(&path, OUTPUT_PATH_PREFIX "/%llu/%u", id, fd) < 0) {
        return NULL;
    }
    return path;
}

static uint32_t spawn_new_process(struct new_process_args* new_proc_args,
                                  struct redir_fd_desc fd_descs[3],
                                  uint64_t* id) {
    uint32_t ret = 0;
    pid_t p = 0;
    struct epoll_fd_desc* epoll_fd_descs[3] = { NULL };

    if (new_proc_args->is_entrypoint && g_entrypoint_desc) {
        return EEXIST;
    }

    struct process_desc* proc_desc = calloc(1, sizeof(*proc_desc));
    if (!proc_desc) {
        return ENOMEM;
    }
    for (size_t fd = 0; fd < 3; ++fd) {
        proc_desc->redirs[fd].type = REDIRECT_FD_INVALID;
    }

    proc_desc->id = get_next_id();
    if (create_process_fds_dir(proc_desc->id) < 0) {
        ret = errno;
        goto out_err;
    }

    /* All these shenanigans with pipes are so that we can distinguish internal
     * failures from spawned process exiting. */
    int status_pipe[2] = { -1, -1 };
    if (pipe2(status_pipe, O_CLOEXEC | O_DIRECT) < 0) {
        ret = errno;
        goto out_err;
    }

    for (size_t fd = 0; fd < 3; ++fd) {
        proc_desc->redirs[fd].type = fd_descs[fd].type;
        switch (fd_descs[fd].type) {
            case REDIRECT_FD_FILE:
                if (fd_descs[fd].path) {
                    proc_desc->redirs[fd].path = strdup(fd_descs[fd].path);
                    if (!proc_desc->redirs[fd].path) {
                        ret = errno;
                        goto out_err;
                    }
                } else {
                    proc_desc->redirs[fd].path =
                        construct_output_path(proc_desc->id, fd);
                    if (!proc_desc->redirs[fd].path) {
                        ret = errno;
                        goto out_err;
                    }
                    int tmp_fd = open(proc_desc->redirs[fd].path,
                                      O_RDWR | O_CREAT | O_EXCL,
                                      S_IRWXU);
                    if (tmp_fd < 0 || close(tmp_fd) < 0) {
                        ret = errno;
                        goto out_err;
                    }
                }
                break;
            case REDIRECT_FD_PIPE_BLOCKING:
            case REDIRECT_FD_PIPE_CYCLIC:
                proc_desc->redirs[fd].buffer.fds[0] = -1;
                proc_desc->redirs[fd].buffer.fds[1] = -1;

                if (cyclic_buffer_init(&proc_desc->redirs[fd].buffer.cb, fd_descs[fd].buffer.cb.size) < 0) {
                    ret = errno;
                    goto out_err;
                }

                if (pipe2(proc_desc->redirs[fd].buffer.fds, O_CLOEXEC) < 0) {
                    ret = errno;
                    goto out_err;
                }
                break;
            default:
                break;
        }
    }

    p = fork();
    if (p < 0) {
        ret = errno;
        goto out_err;
    } else if (p == 0) {
        child_wrapper(status_pipe, new_proc_args, proc_desc->redirs);
    }

    CHECK(close(status_pipe[1]));
    status_pipe[1] = -1;

    char c;
    ssize_t x = read(status_pipe[0], &c, sizeof(c));
    if (x < 0) {
        ret = errno;
        goto out_err;
    } else if (x > 0) {
        /* Process failed to spawn. */
        int status = 0;
        CHECK(waitpid(p, &status, 0));
        if (WIFEXITED(status)) {
            ret = WEXITSTATUS(status);
        } else if (WIFSIGNALED(status)) {
            ret = 0x100 | WTERMSIG(status);
        } else {
            ret = ENOTRECOVERABLE;
        }
        goto out_err;
    } // else x == 0, which means successful process spawn.

    CHECK(close(status_pipe[0]));
    status_pipe[0] = -1;


    for (size_t fd = 0; fd < 3; ++fd) {
        if (proc_desc->redirs[fd].type == REDIRECT_FD_PIPE_BLOCKING
                || proc_desc->redirs[fd].type == REDIRECT_FD_PIPE_CYCLIC) {
            CHECK(close(proc_desc->redirs[fd].buffer.fds[fd ? 1 : 0]));
            proc_desc->redirs[fd].buffer.fds[fd ? 1 : 0] = -1;

            if (add_epoll_fd_desc(&proc_desc->redirs[fd],
                                  proc_desc->redirs[fd].buffer.fds[fd ? 0 : 1],
                                  fd,
                                  &epoll_fd_descs[fd]) < 0) {
                if (errno == ENOMEM || errno == ENOSPC) {
                    ret = errno;
                    goto out_err;
                }
                CHECK(-1);
            }

            CHECK(make_nonblocking(epoll_fd_descs[fd]->fd));
        }
    }

    proc_desc->pid = p;
    proc_desc->is_alive = true;

    *id = proc_desc->id;

    add_process(proc_desc);
    if (new_proc_args->is_entrypoint) {
        g_entrypoint_desc = proc_desc;
    }

    return ret;

out_err:
    if (p > 0) {
        (void)kill(p, SIGKILL);
    }
    if (status_pipe[0] != -1) {
        CHECK(close(status_pipe[0]));
    }
    if (status_pipe[1] != -1) {
        CHECK(close(status_pipe[1]));
    }
    for (size_t fd = 0; fd < 3; ++fd) {
        if (epoll_fd_descs[fd]) {
            CHECK(del_epoll_fd_desc(epoll_fd_descs[fd]));
        }
    }
    if (proc_desc) {
        for (size_t fd = 0; fd < 3; ++fd) {
            cleanup_fd_desc(&proc_desc->redirs[fd]);
        }
        free(proc_desc);
    }
    return ret;
}

static bool is_fd_buf_size_valid(size_t size) {
    return size > 0 && (size % PAGE_SIZE) == 0;
}

static uint32_t parse_fd_redir(struct redir_fd_desc fd_descs[3]) {
    uint32_t fd = 0;
    CHECK(recv_u32(g_cmds_fd, &fd));

    uint8_t type;
    CHECK(recv_u8(g_cmds_fd, &type));

    struct redir_fd_desc fd_desc = { .type = type };

    switch (type) {
        case REDIRECT_FD_FILE:
            CHECK(recv_bytes(g_cmds_fd, &fd_desc.path, NULL,
                             /*is_cstring=*/true));
            break;
        case REDIRECT_FD_PIPE_BLOCKING:
        case REDIRECT_FD_PIPE_CYCLIC:
            CHECK(recv_u64(g_cmds_fd, &fd_desc.buffer.cb.size));
            fd_desc.buffer.cb.buf = MAP_FAILED;
            fd_desc.buffer.fds[0] = -1;
            fd_desc.buffer.fds[1] = -1;
            break;
        default:
            fprintf(stderr, "Unknown REDIRECT_FD_TYPE: %hhu\n", type);
            die();
    }

    /* We do this check so late, because we had to receive type specific data
     * anyway. */
    if (fd >= 3) {
        return EINVAL;
    }

    if (fd_desc.type == REDIRECT_FD_PIPE_BLOCKING
            || fd_desc.type == REDIRECT_FD_PIPE_CYCLIC) {
        if (!is_fd_buf_size_valid(fd_desc.buffer.cb.size)) {
            return EINVAL;
        }
    }

    cleanup_fd_desc(&fd_descs[fd]);

    memcpy(&fd_descs[fd], &fd_desc, sizeof(fd_descs[fd]));

    return 0;
}

static void handle_run_process(msg_id_t msg_id) {
    bool done = false;
    uint32_t ret = 0;
    struct new_process_args new_proc_args = {
        .bin = NULL,
        .argv = NULL,
        .envp = NULL,
        .uid = DEFAULT_UID,
        .gid = DEFAULT_GID,
        .cwd = NULL,
        .is_entrypoint = false,
    };
    struct redir_fd_desc fd_descs[3] = {
        DEFAULT_FD_DESC,
        DEFAULT_FD_DESC,
        DEFAULT_FD_DESC,
    };
    uint64_t proc_id = 0;

    while (!done) {
        uint8_t subtype = 0;

        CHECK(recv_u8(g_cmds_fd, &subtype));

        switch (subtype) {
            case SUB_MSG_RUN_PROCESS_END:
                done = true;
                break;
            case SUB_MSG_RUN_PROCESS_BIN:
                CHECK(recv_bytes(g_cmds_fd, &new_proc_args.bin, NULL,
                                 /*is_cstring=*/true));
                break;
            case SUB_MSG_RUN_PROCESS_ARG:
                CHECK(recv_strings_array(g_cmds_fd, &new_proc_args.argv));
                break;
            case SUB_MSG_RUN_PROCESS_ENV:
                CHECK(recv_strings_array(g_cmds_fd, &new_proc_args.envp));
                break;
            case SUB_MSG_RUN_PROCESS_UID:
                CHECK(recv_u32(g_cmds_fd, &new_proc_args.uid));
                break;
            case SUB_MSG_RUN_PROCESS_GID:
                CHECK(recv_u32(g_cmds_fd, &new_proc_args.gid));
                break;
            case SUB_MSG_RUN_PROCESS_RFD: ;
                /* This error is recoverable - we report the first one found. We
                 * still need to consume the rest of sub-messages to keep
                 * the state consistent though. */
                uint32_t tmp_ret = parse_fd_redir(fd_descs);
                if (!ret) {
                    ret = tmp_ret;
                }
                break;
            case SUB_MSG_RUN_PROCESS_CWD:
                CHECK(recv_bytes(g_cmds_fd, &new_proc_args.cwd, NULL,
                                 /*is_cstring=*/true));
                break;
            case SUB_MSG_RUN_PROCESS_ENT:
                new_proc_args.is_entrypoint = true;
                break;
            default:
                fprintf(stderr, "Unknown MSG_RUN_PROCESS subtype: %hhu\n",
                        subtype);
                die();
        }
    }

    if (ret) {
        goto out;
    }
    if (!new_proc_args.bin) {
        ret = EFAULT;
        goto out;
    }
    if (!new_proc_args.argv) {
        ret = EFAULT;
        goto out;
    }

    ret = spawn_new_process(&new_proc_args, fd_descs, &proc_id);

out:
    free(new_proc_args.cwd);
    for (size_t i = 0; i < 3; ++i) {
        cleanup_fd_desc(&fd_descs[i]);
    }
    free_strings_array(new_proc_args.envp);
    free_strings_array(new_proc_args.argv);
    free(new_proc_args.bin);
    if (ret) {
        send_response_err(msg_id, ret);
    } else {
        send_response_u64(msg_id, proc_id);
    }
}

static uint32_t do_kill_process(uint64_t id) {
    struct process_desc* proc_desc = find_process_by_id(id);
    if (!proc_desc) {
        return EINVAL;
    }

    if (!proc_desc->is_alive) {
        return ESRCH;
    }

    if (kill(proc_desc->pid, SIGKILL) < 0) {
        return errno;
    }

    return 0;
}

static void handle_kill_process(msg_id_t msg_id) {
    bool done = false;
    uint32_t ret = 0;
    uint64_t id = 0;

    while (!done) {
        uint8_t subtype = 0;

        CHECK(recv_u8(g_cmds_fd, &subtype));

        switch (subtype) {
            case SUB_MSG_KILL_PROCESS_END:
                done = true;
                break;
            case SUB_MSG_KILL_PROCESS_ID:
                CHECK(recv_u64(g_cmds_fd, &id));
                break;
            default:
                fprintf(stderr, "Unknown MSG_KILL_PROCESS subtype: %hhu\n",
                        subtype);
                die();
        }
    }

    if (!id) {
        ret = EINVAL;
        goto out;
    }

    ret = do_kill_process(id);

out:
    if (ret) {
        send_response_err(msg_id, ret);
    } else {
        send_response_ok(msg_id);
    }
}

static uint32_t do_mount(const char* tag, char* path) {
    if (create_dir_path(path) < 0) {
        return errno;
    }
    if (mount(tag, path, "9p", 0, "trans=virtio,version=9p2000.L") < 0) {
        return errno;
    }
    return 0;
}

static void handle_mount(msg_id_t msg_id) {
    bool done = false;
    uint32_t ret = 0;
    char* tag = NULL;
    char* path = NULL;

    while (!done) {
        uint8_t subtype = 0;

        CHECK(recv_u8(g_cmds_fd, &subtype));

        switch (subtype) {
            case SUB_MSG_MOUNT_VOLUME_END:
                done = true;
                break;
            case SUB_MSG_MOUNT_VOLUME_TAG:
                CHECK(recv_bytes(g_cmds_fd, &tag, NULL, /*is_cstring=*/true));
                break;
            case SUB_MSG_MOUNT_VOLUME_PATH:
                CHECK(recv_bytes(g_cmds_fd, &path, NULL, /*is_cstring=*/true));
                break;
            default:
                fprintf(stderr, "Unknown MSG_MOUNT_VOLUME subtype: %hhu\n",
                        subtype);
                die();
        }
    }

    if (!tag || !path) {
        ret = EINVAL;
        goto out;
    }

    ret = do_mount(tag, path);

out:
    free(path);
    free(tag);
    if (ret) {
        send_response_err(msg_id, ret);
    } else {
        send_response_ok(msg_id);
    }
}

static uint32_t do_query_output_path(char* path, uint64_t off, char** buf_ptr,
                                     uint64_t* len_ptr) {
    uint32_t ret = 0;
    char* buf = MAP_FAILED;
    size_t len = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return errno;
    }

    off_t ls = lseek(fd, 0, SEEK_END);
    if (ls == (off_t)-1) {
        ret = errno;
        goto out;
    }
    len = (size_t)ls;

    if (off >= len) {
        ret = ENXIO;
        goto out;
    }
    len -= off;

    if (*len_ptr < len) {
        len = *len_ptr;
    }

    if (lseek(fd, off, SEEK_SET) == (off_t)-1) {
        ret = errno;
        goto out;
    }

    buf = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE,
               -1, 0);
    if (buf == MAP_FAILED) {
        ret = errno;
        goto out;
    }

again: ;
    ssize_t real_len = read(fd, buf, len);
    if (real_len < 0) {
        if (errno == EINTR) {
            goto again;
        }
        ret = errno;
        goto out;
    }

    *buf_ptr = buf;
    buf = MAP_FAILED;
    *len_ptr = real_len;

out:
    if (buf != MAP_FAILED) {
        CHECK(munmap(buf, len));
    }
    close(fd);
    return ret;
}

static void handle_query_output(msg_id_t msg_id) {
    bool done = false;
    uint32_t ret = 0;
    uint64_t id = 0;
    uint8_t fd = 1;
    uint64_t off = 0;
    uint64_t len = 0;
    char* buf = NULL;

    while (!done) {
        uint8_t subtype = 0;
        CHECK(recv_u8(g_cmds_fd, &subtype));

        switch (subtype) {
            case SUB_MSG_QUERY_OUTPUT_END:
                done = true;
                break;
            case SUB_MSG_QUERY_OUTPUT_ID:
                CHECK(recv_u64(g_cmds_fd, &id));
                break;
            case SUB_MSG_QUERY_OUTPUT_FD:
                CHECK(recv_u8(g_cmds_fd, &fd));
                break;
            case SUB_MSG_QUERY_OUTPUT_OFF:
                CHECK(recv_u64(g_cmds_fd, &off));
                break;
            case SUB_MSG_QUERY_OUTPUT_LEN:
                CHECK(recv_u64(g_cmds_fd, &len));
                break;
            default:
                fprintf(stderr, "Unknown MSG_QUERY_OUTPUT subtype: %hhu\n",
                        subtype);
                die();
        }
    }

    if (!id || !len || !fd || fd > 2) {
        ret = EINVAL;
        goto out_err;
    }

    struct process_desc* proc_desc = find_process_by_id(id);
    if (!proc_desc) {
        ret = ESRCH;
        goto out_err;
    }

    switch (proc_desc->redirs[fd].type) {
        case REDIRECT_FD_FILE:
            ret = do_query_output_path(proc_desc->redirs[fd].path, off, &buf, &len);
            if (ret) {
                goto out_err;
            }
            send_response_bytes(msg_id, buf, len);
            CHECK(munmap(buf, len));
            break;
        case REDIRECT_FD_PIPE_BLOCKING:
        case REDIRECT_FD_PIPE_CYCLIC:
            if (off) {
                ret = EINVAL;
                goto out_err;
            }
            bool was_full = cyclic_buffer_free_size(&proc_desc->redirs[fd].buffer.cb) == 0;
            send_response_cyclic_buffer(msg_id, &proc_desc->redirs[fd].buffer.cb, len);
            if (was_full) {
                if (add_epoll_fd_desc(&proc_desc->redirs[fd],
                                      proc_desc->redirs[fd].buffer.fds[0],
                                      fd,
                                      NULL) < 0) {
                    if (errno != EEXIST) {
                        CHECK(-1);
                    }
                }
            }
            break;
        default:
            die();
    }

    if (!proc_desc->is_alive && redir_buffers_empty(proc_desc->redirs, 3)) {
        delete_proc(proc_desc);
    }

    return;

out_err:
    send_response_err(msg_id, ret);
}

static void send_output_available_notification(uint64_t id, uint32_t fd) {
    struct msg_hdr resp = {
        .msg_id = 0,
        .type = NOTIFY_OUTPUT_AVAILABLE,
    };

    CHECK(writen(g_cmds_fd, &resp, sizeof(resp)));
    CHECK(writen(g_cmds_fd, &id, sizeof(id)));
    CHECK(writen(g_cmds_fd, &fd, sizeof(fd)));
}

static void handle_output_available(struct epoll_fd_desc** epoll_fd_desc_ptr) {
    struct epoll_fd_desc* epoll_fd_desc = *epoll_fd_desc_ptr;
    struct cyclic_buffer* cb = &epoll_fd_desc->data->buffer.cb;
    size_t to_read = cyclic_buffer_free_size(cb);
    bool needs_notification = cyclic_buffer_data_size(cb) == 0;

    if (to_read == 0) {
        /* Buffer is full, deregister `epoll_fd_desc` untill it get's emptied. */
        CHECK(del_epoll_fd_desc(epoll_fd_desc));
        *epoll_fd_desc_ptr = NULL;
        return;
    }

    ssize_t ret = cyclic_buffer_read(epoll_fd_desc->fd, cb, to_read);
    if (ret < 0) {
        if (errno == EAGAIN) {
            /* This was a spurious wakeup. */
            return;
        } else {
            fprintf(stderr, "Unexpected error while reading in handle_output_available: %m\n");
            die();
        }
    } else if (ret == 0) {
        /* EOF. This actually cannot happen, since if we came here, there must
         * have been some output available and space in the buffer. Maybe just
         * print an error and die() here? */
        CHECK(del_epoll_fd_desc(epoll_fd_desc));
        *epoll_fd_desc_ptr = NULL;
    }

    if (needs_notification) {
        /* XXX: this is ugly, but for now there is no other way of obtaining process id here. */
        int fd = epoll_fd_desc->src_fd;
        struct process_desc* process_desc = CONTAINER_OF(epoll_fd_desc->data, struct process_desc, redirs[fd]);
        send_output_available_notification(process_desc->id, fd);
    }
}

static void handle_net_ctl(msg_id_t msg_id) {
    bool done = false;
    uint16_t flags = 0;
    char* addr = NULL;
    char* mask = NULL;
    char* gateway = NULL;
    char* if_addr = NULL;
    uint16_t if_kind = 0;

    char* if_name = NULL;
    int ret = 0;

    while (!done) {
        uint8_t subtype = 0;
        CHECK(recv_u8(g_cmds_fd, &subtype));

        switch (subtype) {
            case SUB_MSG_NET_CTL_END:
                done = true;
                break;
            case SUB_MSG_NET_CTL_FLAGS:
                CHECK(recv_u16(g_cmds_fd, &flags));
                break;
            case SUB_MSG_NET_CTL_ADDR:
                CHECK(recv_bytes(g_cmds_fd, &addr, NULL, /*is_cstring=*/true));
                break;
            case SUB_MSG_NET_CTL_MASK:
                CHECK(recv_bytes(g_cmds_fd, &mask, NULL, /*is_cstring=*/true));
                break;
            case SUB_MSG_NET_CTL_GATEWAY:
                CHECK(recv_bytes(g_cmds_fd, &gateway, NULL, /*is_cstring=*/true));
                break;
            case SUB_MSG_NET_CTL_IF_ADDR:
                CHECK(recv_bytes(g_cmds_fd, &if_addr, NULL, /*is_cstring=*/true));
                break;
            case SUB_MSG_NET_CTL_IF:
                CHECK(recv_u16(g_cmds_fd, &if_kind));
                break;
            default:
                fprintf(stderr, "Unknown MSG_NET_CTL subtype: %hhu\n",
                        subtype);
                die();
        }
    }

    if (addr && (strlen(addr) == 0)) addr = NULL;
    if (mask && (strlen(mask) == 0)) mask = NULL;

    switch (if_kind) {
        case SUB_MSG_NET_IF_INET:
            if (g_inet_tap_fd != -1) {
                if_name = g_inet_tap_name;
            } else {
                if_name = DEV_INET;
            }
            break;
        default:
            if (g_vpn_tap_fd != -1) {
                if_name = g_vpn_tap_name;
            } else {
                if_name = DEV_VPN;
            }
    }

    if (if_addr) {
        fprintf(stderr, "Configuring '%s' with IP address: %s\n", if_name, if_addr);

        if (strstr(if_addr, ":")) {
            if ((ret = net_if_addr6(if_name, if_addr)) < 0) {
                perror("Error setting IPv6 address");
                goto out_err;
            }

            char hw_addr[6] = { 0, 0, 0, 0, 0, 0};

            if ((ret = net_if_addr6_to_hw_addr(if_addr, hw_addr)) < 0) {
                perror("Error setting MAC address");
                goto out_err;
            }
            if ((ret = net_if_hw_addr(if_name, hw_addr)) < 0) {
                perror("Error setting MAC address");
                goto out_err;
            }
        } else {
            if (!mask) {
                ret = EINVAL;
                goto out_err;
            }
            if ((ret = net_if_addr(if_name, if_addr, mask)) < 0) {
                perror("Error setting IPv4 address");
                goto out_err;
            }

            char hw_addr[6] = { 0, 0, 0, 0, 0, 0};

            if ((ret = net_if_addr_to_hw_addr(if_addr, hw_addr)) < 0) {
                perror("Error setting MAC address");
                goto out_err;
            }
            if ((ret = net_if_hw_addr(if_name, hw_addr)) < 0) {
                perror("Error setting MAC address");
                goto out_err;
            }
        }
    }

    if (gateway) {
        fprintf(stderr, "Configuring '%s' with gateway: %s\n", if_name, gateway);

        if (strstr(gateway, ":")) {
            if ((ret = net_route6(if_name, addr, gateway)) < 0) {
                perror("Error setting IPv6 route");
                goto out_err;
            }
        } else {
            if ((ret = net_route(if_name, addr, mask, gateway)) < 0) {
                perror("Error setting IPv4 route");
                goto out_err;
            }
        }
    }

out_err:
    if (addr) free(addr);
    if (mask) free(mask);
    if (gateway) free(gateway);
    if (if_addr) free(if_addr);

    ret == 0
        ? send_response_ok(msg_id)
        : send_response_err(msg_id, ret);
}

static void handle_net_host(msg_id_t msg_id) {
    bool done = false;
    size_t cap = 8;
    size_t sz = 0;
    int ret = 0;

    char* (*hosts)[][2] = malloc(sizeof(char*[cap][2]));
    char *ip, *hostname;

    while (!done) {
        uint8_t subtype = 0;
        CHECK(recv_u8(g_cmds_fd, &subtype));

        switch (subtype) {
            case SUB_MSG_NET_HOST_END:
                done = true;
                break;
            case SUB_MSG_NET_HOST_ENTRY:
                CHECK(recv_bytes(g_cmds_fd, &ip, NULL, /*is_cstring=*/true));
                CHECK(recv_bytes(g_cmds_fd, &hostname, NULL, /*is_cstring=*/true));

                if (sz == cap - 1) {
                    cap *= 2;
                    hosts = realloc(hosts, sizeof(char*[cap][2]));
                    if (!hosts) {
                        free(ip); free(hostname);
                        ret = ENOMEM;
                        goto out_err;
                    }
                }

                (*hosts)[sz][0] = ip;
                (*hosts)[sz++][1] = hostname;
                break;
            default:
                fprintf(stderr, "Unknown MSG_NET_HOST subtype: %hhu\n",
                        subtype);
                die();
        }
    }

    ret = add_network_hosts((*hosts), sz);

out_err:
    for (int i = sz - 1; i >= 0; --i) {
        free((*hosts)[i][0]);
        free((*hosts)[i][1]);
    }
    free((*hosts));

    ret == 0
        ? send_response_ok(msg_id)
        : send_response_err(msg_id, ret);
}

static void handle_message(void) {
    struct msg_hdr msg_hdr;

    CHECK(readn(g_cmds_fd, &msg_hdr, sizeof(msg_hdr)));

    switch (msg_hdr.type) {
        case MSG_QUIT:
            fprintf(stderr, "Exiting\n");
            handle_quit(msg_hdr.msg_id);
        case MSG_RUN_PROCESS:
            fprintf(stderr, "MSG_RUN_PROCESS\n");
            handle_run_process(msg_hdr.msg_id);
            break;
        case MSG_KILL_PROCESS:
            fprintf(stderr, "MSG_KILL_PROCESS\n");
            handle_kill_process(msg_hdr.msg_id);
            break;
        case MSG_MOUNT_VOLUME:
            fprintf(stderr, "MSG_MOUNT_VOLUME\n");
            handle_mount(msg_hdr.msg_id);
            break;
        case MSG_QUERY_OUTPUT:
            fprintf(stderr, "MSG_QUERY_OUTPUT\n");
            handle_query_output(msg_hdr.msg_id);
            break;
        case MSG_NET_CTL:
            fprintf(stderr, "MSG_NET_CTL\n");
            handle_net_ctl(msg_hdr.msg_id);
            break;
        case MSG_NET_HOST:
            fprintf(stderr, "MSG_NET_HOST\n");
            handle_net_host(msg_hdr.msg_id);
            break;
        case MSG_UPLOAD_FILE:
        case MSG_PUT_INPUT:
        case MSG_SYNC_FS:
            fprintf(stderr, "Not implemented yet!\n");
            send_response_err(msg_hdr.msg_id, EPROTONOSUPPORT);
            die();
        default:
            fprintf(stderr, "Unknown message type: %hhu\n", msg_hdr.type);
            send_response_err(msg_hdr.msg_id, ENOPROTOOPT);
            die();
    }
}

static noreturn void main_loop(void) {
    g_epoll_fd = CHECK(epoll_create1(EPOLL_CLOEXEC));
    struct epoll_event event;

    struct epoll_fd_desc* epoll_fd_desc = malloc(sizeof(*epoll_fd_desc));
    if (!epoll_fd_desc) {
        fprintf(stderr, "epoll_fd_desc malloc failed: %m\n");
        die();
    }

    epoll_fd_desc->type = EPOLL_FD_CMDS;
    epoll_fd_desc->fd = g_cmds_fd;
    epoll_fd_desc->data = NULL;
    event.events = EPOLLIN;
    event.data.ptr = epoll_fd_desc;
    CHECK(epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_cmds_fd, &event));

    epoll_fd_desc = malloc(sizeof(*epoll_fd_desc));
    if (!epoll_fd_desc) {
        fprintf(stderr, "epoll_fd_desc malloc failed: %m\n");
        die();
    }

    epoll_fd_desc->type = EPOLL_FD_SIG;
    epoll_fd_desc->fd = g_sig_fd;
    epoll_fd_desc->data = NULL;
    event.events = EPOLLIN;
    event.data.ptr = epoll_fd_desc;
    CHECK(epoll_ctl(g_epoll_fd, EPOLL_CTL_ADD, g_sig_fd, &event));

    while (1) {
        if (epoll_wait(g_epoll_fd, &event, 1, -1) < 0) {
            if (errno == EINTR || errno == EAGAIN) {
                continue;
            }
            fprintf(stderr, "epoll failed: %m\n");
            die();
        }

        if (event.events & EPOLLNVAL) {
            fprintf(stderr, "epoll error event: 0x%04hx\n", event.events);
            die();
        }

        if ((event.events & EPOLLERR) && epoll_fd_desc->type != EPOLL_FD_OUT) {
            fprintf(stderr, "Got EPOLLERR on fd: %d, type: %d\n",
                    epoll_fd_desc->fd, epoll_fd_desc->type);
            die();
        }

        epoll_fd_desc = event.data.ptr;
        switch (epoll_fd_desc->type) {
            case EPOLL_FD_CMDS:
                if (event.events & EPOLLIN) {
                    handle_message();
                }
                break;
            case EPOLL_FD_SIG:
                if (event.events & EPOLLIN) {
                    handle_sigchld();
                }
                break;
            case EPOLL_FD_OUT:
                /* Need to handle EPOLLOUT and EPOLLERR here. */
                fprintf(stderr, "EPOLL_FD_OUT is not implemented yet\n");
                die();
            case EPOLL_FD_IN:
                if (event.events & EPOLLIN) {
                    assert(epoll_fd_desc->data);
                    handle_output_available(&epoll_fd_desc);
                } else if (event.events & EPOLLHUP) {
                    CHECK(del_epoll_fd_desc(epoll_fd_desc));
                }
                break;
            default:
                fprintf(stderr, "epoll_wait: invalid fd type: %d\n",
                        epoll_fd_desc->type);
                die();
        }
    }
}

static void create_dir(const char *pathname, mode_t mode) {
    if (mkdir(pathname, mode) < 0 && errno != EEXIST) {
        fprintf(stderr, "mkdir(%s) failed with: %m\n", pathname);
        die();
    }
}

int main(void) {
    setbuf(stdin, NULL);
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    create_dir("/dev", DEFAULT_DIR_PERMS);
    CHECK(mount("devtmpfs", "/dev", "devtmpfs", MS_NOSUID,
                "mode=0755,size=2M"));

    load_module("/failover.ko");
    load_module("/virtio.ko");
    load_module("/virtio_ring.ko");
    load_module("/virtio_pci.ko");
    load_module("/net_failover.ko");
    load_module("/virtio_net.ko");
    load_module("/virtio_console.ko");
    load_module("/rng-core.ko");
    load_module("/virtio-rng.ko");
    load_module("/virtio_blk.ko");
    load_module("/squashfs.ko");
    load_module("/overlay.ko");
    load_module("/fscache.ko");
    load_module("/af_packet.ko");
    load_module("/ipv6.ko");
    load_module("/tun.ko");
    load_module("/9pnet.ko");
    load_module("/9pnet_virtio.ko");
    load_module("/9p.ko");

    g_cmds_fd = CHECK(open(VPORT_CMD, O_RDWR | O_CLOEXEC));

    CHECK(mkdir("/mnt", S_IRWXU));
    CHECK(mkdir("/mnt/image", S_IRWXU));
    CHECK(mkdir("/mnt/overlay", S_IRWXU));
    CHECK(mkdir("/mnt/newroot", DEFAULT_DIR_PERMS));

    // 'workdir' and 'upperdir' have to be on the same filesystem
    CHECK(mount("tmpfs", "/mnt/overlay", "tmpfs",
                MS_NOSUID,
                "mode=0777,size=128M"));

    CHECK(mkdir("/mnt/overlay/upper", S_IRWXU));
    CHECK(mkdir("/mnt/overlay/work", S_IRWXU));

    CHECK(mount("/dev/vda", "/mnt/image", "squashfs", MS_RDONLY, ""));
    CHECK(mount("overlay", "/mnt/newroot", "overlay", 0,
                "lowerdir=/mnt/image,upperdir=/mnt/overlay/upper,workdir=/mnt/overlay/work"));

    CHECK(umount2("/dev", MNT_DETACH));

    CHECK(chdir("/mnt/newroot"));
    CHECK(mount(".", "/", "none", MS_MOVE, NULL));
    CHECK(chroot("."));
    CHECK(chdir("/"));

    create_dir("/dev", DEFAULT_DIR_PERMS);
    create_dir("/tmp", DEFAULT_DIR_PERMS);

    CHECK(mount("proc", "/proc", "proc",
                MS_NODEV | MS_NOSUID | MS_NOEXEC,
                NULL));
    CHECK(mount("sysfs", "/sys", "sysfs",
                MS_NODEV | MS_NOSUID | MS_NOEXEC,
                NULL));
    CHECK(mount("devtmpfs", "/dev", "devtmpfs",
                MS_NOSUID,
                "exec,mode=0755,size=2M"));
    CHECK(mount("tmpfs", "/tmp", "tmpfs",
                MS_NOSUID,
                "mode=0777"));

    create_dir("/dev/pts", DEFAULT_DIR_PERMS);
    create_dir("/dev/shm", DEFAULT_DIR_PERMS);

    CHECK(mount("devpts", "/dev/pts", "devpts",
                MS_NOSUID | MS_NOEXEC,
                "gid=5,mode=0620"));
    CHECK(mount("tmpfs", "/dev/shm", "tmpfs",
                MS_NODEV | MS_NOSUID | MS_NOEXEC,
                NULL));

    if (access("/dev/null", F_OK) != 0) {
        CHECK(mknod("/dev/null",
                    MODE_RW_UGO | S_IFCHR,
                    makedev(1, 3)));
    }
    if (access("/dev/ptmx", F_OK) != 0) {
        CHECK(mknod("/dev/ptmx",
                    MODE_RW_UGO | S_IFCHR,
                    makedev(5, 2)));
    }

    setup_network();
    setup_agent_directories();

    block_signals();
    setup_sigfd();

    main_loop();
    stop_network();
}
