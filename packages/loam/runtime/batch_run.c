/**
 * batch_run.c — run a batch program's cases by forking, for `LOAM_BATCH` builds.
 *
 * `nob test` builds its fixtures into one binary. Starting that binary once per
 * fixture costs a shell, an exec and dyld every time; forking it before any module
 * has initialized costs almost nothing and hands each case the same pristine
 * process. The generated `main` calls this first, and a child returns from it to
 * carry on as one ordinary case.
 *
 * `LOAM_SERVE` names a manifest of `<case>\t<id>\t<label>\t<golden or empty>`
 * lines. A case's output goes to `<manifest dir>/<id>.out`. One that exits 0, and
 * matches its golden if it has one, prints `ok   <label>` and adds its id to
 * `<manifest dir>/passed`. Anything else is left out, for the caller to rerun and
 * report: this file never decides that a case failed.
 */
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Past this a case has hung; the caller's rerun reports it. */
#define CASE_SECONDS 120

typedef struct {
    char *case_id, *id, *label, *want;
} Line;

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)n + 1);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    if (buf) {
        buf[n] = 0;
        *len = (size_t)n;
    }
    return buf;
}

static int same_file(const char *a, const char *b) {
    size_t an = 0, bn = 0;
    char *x = slurp(a, &an), *y = slurp(b, &bn);
    int same = x && y && an == bn && memcmp(x, y, an) == 0;
    free(x);
    free(y);
    return same;
}

static char *field(char **at) {
    char *start = *at, *tab = strchr(start, '\t');
    if (tab) {
        *tab = 0;
        *at = tab + 1;
    } else {
        *at = start + strlen(start);
    }
    return start;
}

/* The child's half: become case `l`. */
static void enter_case(const Line *l, const char *dir) {
    char out[1200];
    snprintf(out, sizeof out, "%s/%s.out", dir, l->id);
    int fd = open(out, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    int nul = open("/dev/null", O_RDONLY);
    if (fd < 0 || nul < 0) _exit(1);
    dup2(nul, 0);
    dup2(fd, 1);
    dup2(fd, 2);
    close(fd);
    close(nul);
    setenv("LOAM_CASE", l->case_id, 1);
    unsetenv("LOAM_SERVE");
    alarm(CASE_SECONDS);
}

/** Run the manifest. 0 when finished, in the parent; -1 in a child, or when this
    process is not serving, so `main` carries on. */
int loam_batch_serve(void) {
    const char *manifest = getenv("LOAM_SERVE");
    const char *jobs_env = getenv("LOAM_SERVE_JOBS");
    int jobs = jobs_env ? atoi(jobs_env) : 1;
    size_t len = 0;
    char dir[1024];
    if (!manifest) return -1;
    if (jobs < 1) jobs = 1;
    snprintf(dir, sizeof dir, "%s", manifest);
    char *slash = strrchr(dir, '/');
    if (slash) *slash = 0;
    char *text = slurp(manifest, &len);
    if (!text) return 1;

    int cap = 1;
    for (size_t i = 0; i < len; i++) cap += text[i] == '\n';
    Line *lines = calloc((size_t)cap, sizeof(Line));
    pid_t *pids = calloc((size_t)cap, sizeof(pid_t));
    int n = 0;
    for (char *p = text; *p;) {
        char *nl = strchr(p, '\n');
        if (nl) *nl = 0;
        char *at = p;
        lines[n].case_id = field(&at);
        lines[n].id = field(&at);
        lines[n].label = field(&at);
        lines[n].want = field(&at);
        n++;
        if (!nl) break;
        p = nl + 1;
    }

    char passed[1200];
    snprintf(passed, sizeof passed, "%s/passed", dir);
    FILE *res = fopen(passed, "w");
    if (!res) return 1;

    int next = 0, running = 0;
    while (next < n || running > 0) {
        if (next < n && running < jobs) {
            fflush(NULL);
            pid_t pid = fork();
            if (pid == 0) {
                fclose(res);
                enter_case(&lines[next], dir);
                return -1;
            }
            if (pid < 0) break;
            pids[next++] = pid;
            running++;
            continue;
        }
        int st = 0;
        pid_t done = wait(&st);
        if (done < 0) break;
        running--;
        for (int i = 0; i < next; i++) {
            if (pids[i] != done) continue;
            char out[1200];
            snprintf(out, sizeof out, "%s/%s.out", dir, lines[i].id);
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0 &&
                (!lines[i].want[0] || same_file(lines[i].want, out))) {
                fprintf(res, "%s\n", lines[i].id);
                fprintf(stderr, "ok   %s\n", lines[i].label);
            }
            break;
        }
    }
    fclose(res);
    return 0;
}
