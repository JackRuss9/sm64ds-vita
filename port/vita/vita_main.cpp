// PS Vita launcher for the port's smoke gates.
//
// Each gate's own main() is compiled as sm64ds_test_main() (-Dmain=... on that
// one file only). This wrapper gives it what a console app has on Windows: a
// stdout/stderr it can print to. On the Vita that is a log file you open with
// VitaShell, because a homebrew app has no console of its own.
//
// Output:
//   ux0:data/sm64ds/<gate>.log   -- everything the gate printed, unbuffered,
//                                   so a crash still leaves the lines before it
//   ux0:data/sm64ds/results.txt  -- one PASS/FAIL line per run, appended
//
// The log is opened ONCE here. The test's printf/fprintf reach it through
// vita_log.h, force-included into the test file: on the console neither
// freopen() nor assigning stdout/stderr redirects printf in VitaSDK's newlib
// (tested on hardware: only the launcher's own lines reached the file).
//
// SM64DS_LOG_DIR can be overridden at build time to test this wrapper on a
// desktop or under QEMU, where "ux0:" does not exist.
#include <stdio.h>
#include <sys/stat.h>

#ifndef SM64DS_GATE_NAME
#error "define SM64DS_GATE_NAME (e.g. gate1)"
#endif
#ifndef SM64DS_LOG_DIR
#define SM64DS_LOG_DIR "ux0:data/sm64ds"
#endif

int sm64ds_test_main();   // the gate's renamed main(void)

// VitaSDK's newlib reads this to size the C heap (malloc). From gate 4a on the
// gates hold the DS regions (~14 MB) plus the whole ROM image (16 MB) at once,
// so the size is set explicitly instead of trusting the default. Ignored by
// other C libraries (desktop/QEMU test builds).
extern "C" { int _newlib_heap_size_user = 96 * 1024 * 1024; }
extern "C" { FILE *sm64ds_log_file = nullptr; }   // read by vita_log.h

static void append_result(const char *line_status, int rc)
{
    if (FILE *r = fopen(SM64DS_LOG_DIR "/results.txt", "a")) {
        fprintf(r, "%s: %s (exit %d)\n", SM64DS_GATE_NAME, line_status, rc);
        fclose(r);
    }
}

int main()
{
    mkdir("ux0:data", 0777);       // exists on every console; harmless if so
    mkdir(SM64DS_LOG_DIR, 0777);

    FILE *log = fopen(SM64DS_LOG_DIR "/" SM64DS_GATE_NAME ".log", "w");
    if (!log) {
        append_result("NO-LOG", -1);
        return 0;
    }
    setvbuf(log, nullptr, _IONBF, 0);   // nothing waits in a buffer if it faults

    fprintf(log, "== SM64DS port, %s on PS Vita ==\n", SM64DS_GATE_NAME);

    sm64ds_log_file = log;
    const int rc = sm64ds_test_main();
    sm64ds_log_file = nullptr;
    fprintf(log, "== %s finished: exit code %d (%s) ==\n",
            SM64DS_GATE_NAME, rc, rc == 0 ? "PASS" : "FAIL");
    fclose(log);

    append_result(rc == 0 ? "PASS" : "FAIL", rc);
    return 0;
}
