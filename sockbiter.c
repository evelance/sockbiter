#define _GNU_SOURCE 
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <errno.h>
#include <signal.h>
#include <assert.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/sendfile.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>


/*
** af           AF_INET, AF_INET6, AF_UNSPEC
** node         Hostname or IP address
** service      Port or service name like HTTP
** msgbuf       Buffer for error message
** msglen       Size of error buffer
** nonblock     Make socket non-blocking
** cloexec      Make socket auto-closing on exec
** reuseaddr    Bind to local address even when TIME_WAIT from last socket isn't over yet
** reuseport    Allow multithreaded accept
** tcpfastopen  TCP Fast Open
** 
** Returns socket with bound address on success. Otherwise, prints message
** into given buffer and returns -1.
*/
static int connecttcpsock(int af, const char* node, const char* service, char* msgbuf, size_t msglen,
                       int nonblock, int cloexec, int tcpfastopen)
{
    /* Convert address string into numeric one */
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof hints);
    hints.ai_family     = af;
    hints.ai_socktype   = SOCK_STREAM;
    int err = getaddrinfo(node, service, &hints, &result);
    if (err) {
        snprintf(msgbuf, msglen, "getaddrinfo: %s", gai_strerror(err));
        return -1;
    }
    /* Walk linked list of results */
    if (result == NULL) {
        snprintf(msgbuf, msglen, "No results for getaddrinfo");
        return -1;
    }
    int fd = -1;
    const char* lasterr = "";
    struct addrinfo* start = result;
    while (result != NULL) {
        /* Try to create socket */
        fd = socket(result->ai_family, result->ai_socktype|(nonblock ? SOCK_NONBLOCK : 0)|(cloexec ? SOCK_CLOEXEC : 0), 0);
        if (fd < 0) {
            snprintf(msgbuf, msglen, "socket(%s, %s, 0): %s",
                result->ai_family == AF_INET ? "AF_INET" : (result->ai_family == AF_INET6 ? "AF_INET6" : "<unknown>"),
                result->ai_socktype == SOCK_STREAM ? "SOCK_STREAM" : "<unknown>",
                strerror(errno));
            goto fail;
        }
        /* Options must be set prior to bind */
        if (tcpfastopen) {
            int qlen = 5;
            if (setsockopt(fd, SOL_TCP, TCP_FASTOPEN, &qlen, sizeof qlen) < 0) {
                snprintf(msgbuf, msglen, "setsockopt(SOL_TCP, TCP_FASTOPEN, 5): %s", strerror(errno));
                goto fail;
            }
        }
        /* Try to connect/bind, otherwise try next result from getaddrinfo */
        if (connect(fd, result->ai_addr, result->ai_addrlen) == 0) {
            freeaddrinfo(start);
            return fd;
        }
        lasterr = strerror(errno);
        close(fd);
        result = result->ai_next;
    }
    /* Got results but bind failed for all of them */
    snprintf(msgbuf, msglen, "No usable address: %s", lasterr);
fail:
    freeaddrinfo(start);
    return -1;
}

/*
** Multi-sendfile worker threads, shared data and helper functions
*/
struct ms_thread {
    pthread_t thread;
    int created;            /* Indicates that the thread should be joined/canceled for cleanup */
    int successful;         /* 1 if no errors occurred, 0 otherwise. The error will be printed into errmsg. */
    char errmsg[8192];
};

struct ms_conn {
    int fd_in;                          /* Request file to send */
    int fd_out;                         /* Response log */
    int fd_sock;                        /* TCP socket for HTTP connection, created by sender */
    const char* host;                   /* Host name */
    const char* port;                   /* Host port */
    int use_shutdown;                   /* shutdown(SHUT_WR) after send is complete */
    int ignore_out;                     /* Do not use fd_out */
    size_t in_len;                      /* Length of data to send */
    char in_file[4096];                 /* Path of fd_in */
    char out_file[4096];                /* Path of fd_out */
    pthread_barrier_t* barrier;         /* Barrier to block all threads until all are ready, belonging to lcf_multi_sendfile */
    pthread_mutex_t connectmx;          /* Mutex to block receiver until fd_sock is connected */
    int connectmx_created;              /* Indicates that connectmx should be destroyed for cleanup */
    struct ms_thread sender;            /* Sender status */
    struct ms_thread receiver;          /* Receiver status */
    char recvbuf[32 * 1024];            /* Receive buffer; threads have only minimal stack space */
    size_t recv_total;
    struct timespec connect_start;      /* Before connect() */
    struct timespec connect_end;        /* After connect() */
    struct timespec send_start;         /* Before first sendfile() */
    struct timespec send_end;           /* After last sendfile() */
    struct timespec receive_start;      /* Before first recv() */
    struct timespec receive_end;        /* After last recv() */
    struct ms_conn* prev;               /* Chain connection structures into simple linked list */
};

static void* ms_sender_thread(struct ms_conn* conn)
{
    /* Initialize and wait */
    struct ms_thread* status = &conn->sender;
    status->successful = 0;
    int err = pthread_mutex_lock(&conn->connectmx);
    if (err) {
        snprintf(status->errmsg, sizeof status->errmsg,
            "pthread_mutex_lock failed: %s", strerror(err));
    }
    pthread_barrier_wait(conn->barrier);
    if (err) {
        return NULL;
    }
    /* Connect TCP socket */
    clock_gettime(CLOCK_MONOTONIC, &conn->connect_start);
    char errmsg[256];
    if ((conn->fd_sock = connecttcpsock(AF_UNSPEC, conn->host, conn->port, errmsg, sizeof errmsg, 0, 0, 0)) < 0) {
        snprintf(status->errmsg, sizeof status->errmsg,
            "Cannot open TCP connection to %s:%s: %s", conn->host, conn->port, errmsg);
        return NULL;
    }
    clock_gettime(CLOCK_MONOTONIC, &conn->connect_end);
    /* Unblock receiver thread */
    err = pthread_mutex_unlock(&conn->connectmx);
    if (err) {
        snprintf(status->errmsg, sizeof status->errmsg,
            "pthread_mutex_unlock failed: %s", strerror(err));
        return NULL;
    }
    /* Send all requests */
    clock_gettime(CLOCK_MONOTONIC, &conn->send_start);
    size_t remaining = conn->in_len;
    while (remaining > 0) {
        ssize_t sent = sendfile(conn->fd_sock, conn->fd_in, NULL, remaining);
        if (sent < 0) {
            snprintf(status->errmsg, sizeof status->errmsg,
                "sendfile failed: %s", strerror(errno));
            return NULL;
        }
        if ((size_t)sent >= remaining) {
            if (conn->use_shutdown) {
                shutdown(conn->fd_sock, SHUT_WR);
            }
            remaining = 0;
        } else {
            remaining -= sent;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &conn->send_end);
    /* No problems occurred */
    status->successful = 1;
    return NULL;
}

static void* ms_receiver_thread(struct ms_conn* conn)
{
    /* Initialize and wait */
    struct ms_thread* status = &conn->receiver;
    status->successful = 0;
    conn->recv_total = 0;
    pthread_barrier_wait(conn->barrier);
    /* Wait until fd_sock is connected */
    int err = pthread_mutex_lock(&conn->connectmx);
    if (err) {
        snprintf(status->errmsg, sizeof status->errmsg,
            "pthread_mutex_lock failed: %s", strerror(err));
        return NULL;
    }
    err = pthread_mutex_unlock(&conn->connectmx);
    if (err) {
        snprintf(status->errmsg, sizeof status->errmsg,
            "pthread_mutex_unlock failed: %s", strerror(err));
        return NULL;
    }
    /* Read until EOF */
    clock_gettime(CLOCK_MONOTONIC, &conn->receive_start);
    for (;;) {
        /* Blocking read */
        ssize_t rlen = recv(conn->fd_sock, conn->recvbuf, sizeof conn->recvbuf, MSG_WAITALL);
        if (rlen == 0) {
            /* Stream socket peer has performed an orderly shutdown */
            clock_gettime(CLOCK_MONOTONIC, &conn->receive_end);
            break;
        }
        if (rlen < 0) {
            snprintf(status->errmsg, sizeof status->errmsg,
                "recv failed: %s", strerror(errno));
            return NULL;
        }
        conn->recv_total += rlen;
        /* Write received responses to output file */
        if (! conn->ignore_out) {
            char* now = conn->recvbuf;
            while (rlen > 0) {
                ssize_t wlen = write(conn->fd_out, now, (size_t)rlen);
                if (wlen < 0) {
                    snprintf(status->errmsg, sizeof status->errmsg,
                        "Cannot write to output file '%s': %s", conn->out_file, strerror(errno));
                    return NULL;
                }
                rlen -= wlen;
		now += wlen;
            }
        }
    }
    /* No problems occurred */
    status->successful = 1;
    return NULL;
}

static void ms_destroy_conns(struct ms_conn* conn, int cancel_threads)
{
    while (conn != NULL) {
        if (conn->fd_in >= 0)
            close(conn->fd_in);
        if (conn->fd_out >= 0)
            close(conn->fd_out);
        if (conn->fd_sock >= 0)
            close(conn->fd_sock);
        if (conn->sender.created && cancel_threads) {
            // pthread_cancel(conn->sender.thread); /* Doesn't seem to work without special libc */
        }
        if (conn->receiver.created && cancel_threads)
            // pthread_cancel(conn->receiver.thread);  /* Doesn't seem to work without special libc */
        if (conn->connectmx_created)
            pthread_mutex_destroy(&conn->connectmx);
        struct ms_conn* prev = conn->prev;
        free(conn);
        conn = prev;
    }
}

/*
** Try to create all required file descriptors, sockets, and threads.
** Returns pointer to linked list of structs on success. On error,
** NULL is returned and an error message is printed into msgbuf.
*/
static struct ms_conn* ms_create_conns(char* msgbuf, size_t msglen, const char* in_file, const char* out_file_fmt,
                                const char* host, const char* port, size_t num_conns, pthread_barrier_t* barrier,
                                int use_shutdown, int ignore_out)
{
    struct ms_conn* last = NULL;
    size_t in_len = 0;
    for (size_t i = 0; i < num_conns; ++i) {
        /* Setup shared data structure and add to linked list */
        struct ms_conn* conn = malloc(sizeof (struct ms_conn));
        conn->fd_in = conn->fd_out = conn->fd_sock = -1;
        conn->host = host;
        conn->port = port;
        conn->use_shutdown = use_shutdown;
        conn->ignore_out = ignore_out;
        conn->barrier = barrier;
        conn->in_len = in_len;
        conn->sender.created = 0;
        conn->receiver.created = 0;
        conn->connectmx_created = 0;
        conn->prev = last;
        last = conn;
        /* Open input file with requests to send */
        snprintf(conn->in_file, sizeof conn->in_file, in_file);
        if ((conn->fd_in = open(in_file, O_RDONLY)) < 0) {
            snprintf(msgbuf, msglen, "Cannot open input file '%s': %s", in_file, strerror(errno));
            goto failed;
        }
        if (i == 0) {
            /* Stat first file descriptor */
            struct stat st;
            if (fstat(conn->fd_in, &st) < 0) {
                snprintf(msgbuf, msglen, "Cannot stat input file '%s': %s", in_file, strerror(errno));
                goto failed;
            }
            in_len = st.st_size;
            conn->in_len = in_len;
        }
        /* Open output file to record responses */
        if (! ignore_out) {
            snprintf(conn->out_file, sizeof conn->out_file, out_file_fmt, (int)(i + 1));
            if ((conn->fd_out = open(conn->out_file, O_WRONLY|O_CREAT|O_TRUNC, 0666)) < 0) {
                snprintf(msgbuf, msglen, "Cannot open output file '%s': %s",
                    conn->out_file, strerror(errno));
                goto failed;
            }
        }
        /* Create mutex for blocking receiver thread */
        int err = pthread_mutex_init(&conn->connectmx, NULL);
        if (err) {
            snprintf(msgbuf, msglen, "pthread_mutex_init failed: %s", strerror(err));
            goto failed;
        }
        conn->connectmx_created = 1;
        /* Start worker threads with a small stack, since there might be many of them
           and they do not need much space anyways, as they mostly block on IO. Most
           stack space is probably needed by getaddrinfo. */
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setstacksize(&attr, 32 * 1024);
        err = pthread_create(&conn->sender.thread, &attr, (void*(*)(void*))ms_sender_thread, (void*)conn);
        if (err != 0) {
            snprintf(msgbuf, msglen, "Failed to start sender thread #%zu: %s", i, strerror(err));
            pthread_attr_destroy(&attr);
            goto failed;
        }
        conn->sender.created = 1;
        err = pthread_create(&conn->receiver.thread, &attr, (void*(*)(void*))ms_receiver_thread, (void*)conn);
        if (err != 0) {
            snprintf(msgbuf, msglen, "Failed to start receiver thread #%zu: %s", i, strerror(err));
            pthread_attr_destroy(&attr);
            goto failed;
        }
        conn->receiver.created = 1;
        pthread_attr_destroy(&attr);
    }
    return last;
failed:
    ms_destroy_conns(last, 1);
    return NULL;
}

/*
** Run multi-connection, multithreaded keep-alive sendfile benchmark.
** For every connection, the input file containing requests will be opened in read
** mode and an output file named will be created in write mode.
** Two threads per connection are started as well, with one using blocking sendfile()
** calls to send the requests, and the other one using blocking recv() and write()
** calls to store the responses in the output files.
**
** multi_sendfile(in_file, hostname, port, num_threads)
**   in_file (string)       Input file name.
**   out_file_fmt (string)  Format for output file names, e.g. responses-%d.txt
**   hostname (string)      Host name of target host.
**   port (string)          Port number or service name of target host.
**   num_conns (integer)    Number of concurrent connections to used.
**   use_shutdown (bool)    Call shutdown() if all data has been sent. This might cause problems.
**   ignore_out (bool)      Do not create output files. This improves performance.
** Returns a table with indices 1..num_conns, with entries representing the results of each
** connection. The entry is either a string with an error message, or a table with the keys
** total_sent (integer), total_received (integer), connect_start_ns, connect_end_ns,
** send_start_ns, send_end_ns, receive_start_ns, receive_end_ns (all double) with the total number
** of bytes sent and received and the timestamps recorded by the workder threads.
*/
static int lcf_multi_sendfile(lua_State* L)
{
    const char* in_file = luaL_checkstring(L, 1);
    const char* out_file_fmt = luaL_checkstring(L, 2);
    const char* host = luaL_checkstring(L, 3);
    const char* port = luaL_checkstring(L, 4);
    lua_Integer num_conns = luaL_checkinteger(L, 5);
    luaL_checktype(L, 6, LUA_TBOOLEAN);
    luaL_checktype(L, 7, LUA_TBOOLEAN);
    size_t max_conn = (UINT_MAX / 2) - 1;
    if (num_conns <= 0 || (size_t)num_conns > max_conn)
        return luaL_error(L, "number of connections must greater than zero and smaller than %zu", max_conn);
    int use_shutdown = lua_toboolean(L, 6);
    int ignore_out = lua_toboolean(L, 7);
    /* Use a barrier to ensure that threads start simultaneously, if possible */
    pthread_barrier_t barrier;
    int err = pthread_barrier_init(&barrier, NULL, (unsigned)(num_conns * 2 + 1));
    if (err)
        return luaL_error(L, "pthread_barrier_init failed: %s", strerror(err));
    /* Open sockets, files, start worker threads */
    char errmsg[8192];
    struct ms_conn* conns = ms_create_conns(
        errmsg, sizeof errmsg,
        in_file, out_file_fmt, host, port, num_conns,
        &barrier,
        use_shutdown, ignore_out
    );
    if (conns == NULL) {
        pthread_barrier_destroy(&barrier);
        lua_pushnil(L);
        lua_pushstring(L, errmsg);
        return 2;
    }
    /* Wait until all threads are blocked by the barrier, then start all */
    pthread_barrier_wait(&barrier);
    /* Join all threads and generate results table */
    lua_createtable(L, num_conns, 0);
    lua_Integer i = 0;
    for (struct ms_conn* c = conns; c != NULL; c = c->prev) {
        ++i;
        /* Join sender thread */
        pthread_join(c->sender.thread, NULL);
        if (! c->sender.successful) {
            /* Sender failed, receiver probably hangs - cancel it */
            // pthread_cancel(c->receiver.thread); /* Doesn't seem to work without special libc */
            lua_pushstring(L, c->sender.errmsg);
            lua_rawseti(L, -2, i);
            continue;
        } else {
            /* Join receiver thread */
            pthread_join(c->receiver.thread, NULL);
            if (! c->receiver.successful) {
                lua_pushstring(L, c->receiver.errmsg);
                lua_rawseti(L, -2, i);
                continue;
            }
        }
        /* Threads were successful, add table with results */
        lua_createtable(L, 0, 3);
        lua_pushinteger(L, c->in_len);
        lua_setfield(L, -2, "total_sent");
        lua_pushinteger(L, c->recv_total);
        lua_setfield(L, -2, "total_received");
        lua_pushnumber(L, (c->connect_start.tv_sec * 1.0e9 + c->connect_start.tv_nsec));
        lua_setfield(L, -2, "connect_start_ns");
        lua_pushnumber(L, (c->connect_end.tv_sec * 1.0e9 + c->connect_end.tv_nsec));
        lua_setfield(L, -2, "connect_end_ns");
        lua_pushnumber(L, (c->send_start.tv_sec * 1.0e9 + c->send_start.tv_nsec));
        lua_setfield(L, -2, "send_start_ns");
        lua_pushnumber(L, (c->send_end.tv_sec * 1.0e9 + c->send_end.tv_nsec));
        lua_setfield(L, -2, "send_end_ns");
        lua_pushnumber(L, (c->receive_start.tv_sec * 1.0e9 + c->receive_start.tv_nsec));
        lua_setfield(L, -2, "receive_start_ns");
        lua_pushnumber(L, (c->receive_end.tv_sec * 1.0e9 + c->receive_end.tv_nsec));
        lua_setfield(L, -2, "receive_end_ns");
        lua_rawseti(L, -2, i);
    }
    ms_destroy_conns(conns, 0);
    pthread_barrier_destroy(&barrier);
    return 1;
}

static int lcf_cputime_ns(lua_State* L)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double ns = (ts.tv_sec * 1.0e9 + ts.tv_nsec);
    lua_pushnumber(L, ns);
    return 1;
}

int main(int argc, char** argv)
{
    signal(SIGPIPE, SIG_IGN);
    /* Run Lua script with shell arguments */
    lua_State* L = luaL_newstate();
    if (luaL_loadfilex(L, "sockbiter.lua", "t") != LUA_OK) {
        fprintf(stderr, "Lua error: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return EXIT_FAILURE;
    }
    luaL_openlibs(L);
    lua_pushcfunction(L, lcf_cputime_ns);
    lua_setglobal(L, "cputime_ns");
    lua_pushcfunction(L, lcf_multi_sendfile);
    lua_setglobal(L, "multi_sendfile");
    lua_pushinteger(L, argc);
    lua_createtable(L, argc, 0);
    for (int i = 0; i < argc; ++i) {
        lua_pushstring(L, argv[i]);
        lua_rawseti(L, -2, i);
    }
    if (lua_pcall(L, 2, 1, 0) != LUA_OK) {
        fprintf(stderr, "Lua error: %s\n", lua_tostring(L, -1));
        lua_close(L);
        return EXIT_FAILURE;
    }
    int isnum = 0;
    lua_Integer exit_code = lua_tointegerx(L, -1, &isnum);
    lua_close(L);
    return isnum ? exit_code : EXIT_SUCCESS;
}

