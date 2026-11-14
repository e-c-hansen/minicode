// procmem-linux — print every process this user can see, one per line:
//   pid <TAB> parent pid <TAB> systemd scope <TAB> PSS (bytes) <TAB> program <TAB> command line
//
// Used by scripts/membench-linux.sh to add up what an editor costs. PSS, the
// Pss: line of /proc/<pid>/smaps_rollup, counts each shared page once, split
// between the processes mapping it, so WebKitGTK's and Chrome's shared
// libraries are not counted over and over the way summing RSS does. The
// Mac's tools/procmem-mac.c reports the physical footprint instead.
// The scope is the last component of the process's cgroup when it is a
// systemd scope (a snap's or an app's), else "-".
// A process whose smaps_rollup cannot be read (another user's) is left out.
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void clean(char *s) {
    for (; *s; s++) if (*s == '\t' || *s == '\n' || (unsigned char)*s < 32) *s = ' ';
}

int main(void) {
    DIR *d = opendir("/proc");
    if (!d) return 1;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!isdigit((unsigned char)e->d_name[0])) continue;
        char path[512], line[4096];
        snprintf(path, sizeof path, "/proc/%s/smaps_rollup", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        long pss = -1;
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "Pss:", 4)) { pss = atol(line + 4); break; }
        fclose(f);
        if (pss < 0) continue;
        snprintf(path, sizeof path, "/proc/%s/stat", e->d_name);
        f = fopen(path, "r");
        if (!f) continue;
        char *got = fgets(line, sizeof line, f);
        fclose(f);
        if (!got) continue;
        char *rp = strrchr(line, ')');   // the name may hold spaces and parentheses
        if (!rp) continue;
        long ppid = 0;
        char state;
        if (sscanf(rp + 1, " %c %ld", &state, &ppid) != 2) continue;
        char comm[256] = "";
        char *lp = strchr(line, '(');
        if (lp && lp < rp) { size_t n = rp - lp - 1; if (n > 255) n = 255; memcpy(comm, lp + 1, n); comm[n] = 0; }
        char prog[512] = "";
        snprintf(path, sizeof path, "/proc/%s/exe", e->d_name);
        ssize_t n = readlink(path, prog, sizeof prog - 1);
        if (n <= 0) snprintf(prog, sizeof prog, "%s", comm); else prog[n] = 0;
        char cmd[1024] = "";
        snprintf(path, sizeof path, "/proc/%s/cmdline", e->d_name);
        f = fopen(path, "r");
        if (f) {
            size_t m = fread(cmd, 1, sizeof cmd - 1, f);
            fclose(f);
            for (size_t i = 0; i < m; i++) if (!cmd[i]) cmd[i] = ' ';
            cmd[m] = 0;
        }
        char scope[256] = "-";
        snprintf(path, sizeof path, "/proc/%s/cgroup", e->d_name);
        f = fopen(path, "r");
        if (f) {
            if (fgets(line, sizeof line, f)) {
                line[strcspn(line, "\n")] = 0;
                char *sl = strrchr(line, '/');
                char *name = sl ? sl + 1 : line;
                size_t L = strlen(name);
                if (L > 6 && !strcmp(name + L - 6, ".scope")) snprintf(scope, sizeof scope, "%s", name);
            }
            fclose(f);
        }
        clean(prog); clean(cmd);
        printf("%s\t%ld\t%s\t%ld\t%s\t%s\n", e->d_name, ppid, scope, pss * 1024, prog, cmd);
    }
    closedir(d);
    return 0;
}
