#include "client.h"

#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>
#include <errno.h>

#define PROTOCOL_HELLO "PROTOCOL10: HELLO, I AM: "

#include "shared.h"

static __thread struct {
    pid_t pid;                  /* TODO: Document that this identifies fork()ed threads */
    int connection_fd;
    shmem_context *shmem_context;
    enum need need;
} thread_state = {-1, -1, NULL, -1};

static int gettid(void)
{
    #ifdef __APPLE__
        return pthread_mach_thread_np(pthread_self());
    #else
        return syscall(__NR_gettid);
    #endif
}

static bool send_all(int fd, const char *buf, size_t size)
{
    ssize_t send_rc = send(fd, buf, size, 0);
    return (size_t)send_rc == size;
}

static bool send_size(int fd, size_t size)
{
    uint32_t size32 = ntohl(size);
    return send_all(fd, PS(size32));
}

static int connect_master(const char *need_str)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ASSERT(-1 != fd);

    char *env_sockaddr = getenv(ENVVARS_PREFIX "MASTER_UNIX_SOCKADDR");
    ASSERT(env_sockaddr);

    char *env_job_id = getenv(ENVVARS_PREFIX "JOB_ID");
    ASSERT(env_job_id);

    struct sockaddr_un addr = {
        .sun_family = AF_UNIX,
    };
    ASSERT(strlen(env_sockaddr) < sizeof addr.sun_path);
    strcpy(addr.sun_path, env_sockaddr);

    int connect_rc = connect(fd, (struct sockaddr*) &addr, sizeof addr);
    if(0 != connect_rc) {
        close(fd);
        return -1;
    }

    char hello[strlen(PROTOCOL_HELLO) + strlen(env_job_id) + 24]; /* TODO: Avoid magic constant */
    hello[sizeof hello-1] = 0;

    int len = snprintf(hello, sizeof hello-1, PROTOCOL_HELLO "%d:%d:%s:%s", getpid(), gettid(),
                       env_job_id, need_str);
    if(!send_size(fd, len)) return false;

    bool hello_success = send_all(fd, hello, len);
    if(!hello_success) {
        close(fd);
        return -1;
    }

    return fd;
}

int client_make_connection(enum need need)
{
    pid_t pid = getpid();
    if(pid != thread_state.pid) {
        thread_state.pid = pid;
        thread_state.connection_fd = -1;
        int fd = connect_master(need == HOOK ? "HOOK" : "HINT");
        if(-1 == fd) return -1;
        thread_state.shmem_context = recv_readonly_shmem(fd);
        thread_state.connection_fd = fd;
        thread_state.need = need;
        if(!await_go()) return -1;
    }
    ASSERT(thread_state.need == need);
    return thread_state.connection_fd;
}

bool is_wait_needed(const char *filename)
{
    if (!thread_state.shmem_context) {
        return true;
    }
    return !shmem_get_item_non_deterministic(thread_state.shmem_context,
        filename);
}

static int connection(void)
{
    return client_make_connection(HOOK);
}

static int assert_connection(void)
{
    ASSERT(getpid() == thread_state.pid);
    return thread_state.connection_fd;
}

bool await_go(void)
{
    char buf[2];
    uint32_t got = 0;
    while (got < sizeof(buf)) {
        ssize_t rc = recv(assert_connection(), &buf[got], sizeof(buf) - got, 0);
        if (rc == 0) break;
        if (rc == -1 && errno == EINTR) continue;
        ASSERT(rc > 0);
        got += (uint32_t)rc;
    }
    return !memcmp("GO", PS(buf));
}

bool client__send_hooked(bool is_delayed, const char *buf, size_t size,
                         const char *truncatable_end)
{
    int fd = connection();
    if(-1 == fd) return false;

    if (truncatable_end) {
        size = (truncatable_end + strlen(truncatable_end) + 1) - buf;
    }

    if(!send_size(fd, sizeof(is_delayed)+size)) return false;
    if(!send_all(fd, PS(is_delayed))) return false;
    if(!send_all(fd, buf, size)) return false;

    return true;
}
