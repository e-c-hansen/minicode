// procmem — print every process this user can see, one per line:
//   pid <TAB> parent pid <TAB> responsible pid <TAB> physical footprint (bytes) <TAB> name
//
// Used by scripts/membench.sh to add up what an editor costs. The physical
// footprint is the number Activity Monitor calls "Memory": resident plus
// compressed, without shared framework pages counted over and over the way
// summing RSS does. The responsible pid is how macOS ties a helper started
// by launchd (WebKit's page renderers, for one) back to the app it works for,
// since such a helper's parent is launchd, not the app.
#include <dlfcn.h>
#include <libproc.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/proc_info.h>
#include <sys/resource.h>

typedef pid_t (*resp_fn)(pid_t);

int main(void) {
    resp_fn responsible =
        (resp_fn)dlsym(RTLD_DEFAULT, "responsibility_get_pid_responsible_for_pid");
    int n = proc_listallpids(NULL, 0);
    if (n <= 0) return 1;
    pid_t *pids = calloc((size_t)n * 2, sizeof(pid_t));
    n = proc_listallpids(pids, (int)(n * 2 * sizeof(pid_t)));
    for (int i = 0; i < n; i++) {
        pid_t pid = pids[i];
        if (pid <= 0) continue;
        struct proc_bsdinfo bsd;
        if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bsd, sizeof bsd) != sizeof bsd)
            continue;
        struct rusage_info_v4 ru;
        if (proc_pid_rusage(pid, RUSAGE_INFO_V4, (rusage_info_t *)&ru) != 0)
            continue;   // another user's process
        char name[PROC_PIDPATHINFO_MAXSIZE];
        if (proc_pidpath(pid, name, sizeof name) <= 0)
            snprintf(name, sizeof name, "%s", bsd.pbi_comm);
        pid_t r = responsible ? responsible(pid) : pid;
        printf("%d\t%d\t%d\t%llu\t%s\n", pid, bsd.pbi_ppid, r,
               (unsigned long long)ru.ri_phys_footprint, name);
    }
    free(pids);
    return 0;
}
