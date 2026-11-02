// Settings file test (probe.c, settings.c). The parent runs itself as a child with civprobe.ini
// only in the civprobe folder, in both places, only next to the DLL and in neither, then with files
// next to the DLL that hold the mistakes the report names: a setting left behind its ;, a word or
// a grouped number for a value, a setting twice, a line without =, a misspelled name, a 200-character
// line, two [probe] lines, no [probe] line, Enabled=no (stays on) and Enabled=0 (no log). The files
// set different IntervalMs values, so the "interval=" of the child's proxying line shows which one
// counted. The parent puts the shared test ini back after each child.
#include <windows.h>
#include <stdio.h>
#include <string.h>

static int g_ok = 1;
static void check(int good, const char *what) { printf("%-58s %s\n", what, good ? "ok" : "WRONG"); if (!good) g_ok = 0; }

static size_t load(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) return (size_t)-1;
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}

// text NULL: delete the file.
static int put(const char *path, const char *text, size_t n)
{
    if (!text) return DeleteFileA(path) || GetLastError() == ERROR_FILE_NOT_FOUND;
    FILE *f = fopen(path, "wb");
    if (!f) { printf("cannot write %s\n", path); return 0; }
    fwrite(text, 1, n, f);
    fclose(f);
    return 1;
}

static void newest_log(const char *dir, char *name)
{
    char pat[MAX_PATH];
    snprintf(pat, sizeof pat, "%s\\civprobe\\events_*.log", dir);
    name[0] = 0;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do if (strcmp(fd.cFileName, name) > 0) strcpy(name, fd.cFileName); while (FindNextFileA(h, &fd));
    FindClose(h);
}

static char g_exe[MAX_PATH], g_dir[MAX_PATH], g_beside[MAX_PATH], g_inner[MAX_PATH];
static char g_log[1 << 20];
static int g_fresh;  // the child wrote a new events log
static char g_savedBeside[65536], g_savedInner[65536];
static size_t g_nBeside, g_nInner;

// Puts the shared test ini back (and a settings file beside the test exes, if there was one) after
// each child, so a run stopped between children leaves no test file that other tests would read.
static void restore(void)
{
    put(g_beside, g_nBeside == (size_t)-1 ? NULL : g_savedBeside, g_nBeside);
    put(g_inner, g_nInner == (size_t)-1 ? NULL : g_savedInner, g_nInner);
}

static const char kBeside[] = "[probe]\r\nIntervalMs=750\r\n";
static const char kInner[] = "[probe]\r\nIntervalMs=1250\r\n";

// Runs the child with the two files as given (NULL: none) and reads its events log into g_log.
static int run(const char *beside, const char *inner)
{
    if (!put(g_beside, beside, beside ? strlen(beside) : 0) || !put(g_inner, inner, inner ? strlen(inner) : 0))
        return 0;
    char before[MAX_PATH], after[MAX_PATH];
    newest_log(g_dir, before);
    Sleep(1100);  // a later second than the previous launch: the child's events log sorts last
    char cmd[MAX_PATH + 16];
    snprintf(cmd, sizeof cmd, "\"%s\" child", g_exe);
    STARTUPINFOA si = {sizeof si};
    PROCESS_INFORMATION pi;
    DWORD code = 1;
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi)) {
        if (WaitForSingleObject(pi.hProcess, 30000) != WAIT_OBJECT_0) TerminateProcess(pi.hProcess, 1);
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    newest_log(g_dir, after);
    g_fresh = after[0] && strcmp(after, before) != 0;
    g_log[0] = 0;
    if (g_fresh) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%s\\civprobe\\%s", g_dir, after);
        size_t n = load(path, g_log, sizeof g_log - 1);
        g_log[n == (size_t)-1 ? 0 : n] = 0;
    }
    restore();
    return code == 0;
}

static int has(const char *what) { return strstr(g_log, what) != NULL; }

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    GradientFill(NULL, NULL, 0, NULL, 0, 0);  // pulls in msimg32 (the probe)
    if (argc > 1) return 0;                   // child: the probe has started and logged by now

    GetModuleFileNameA(NULL, g_exe, MAX_PATH);
    strcpy(g_dir, g_exe);
    *strrchr(g_dir, '\\') = 0;
    snprintf(g_beside, sizeof g_beside, "%s\\civprobe.ini", g_dir);
    snprintf(g_inner, sizeof g_inner, "%s\\civprobe\\civprobe.ini", g_dir);
    g_nBeside = load(g_beside, g_savedBeside, sizeof g_savedBeside);
    g_nInner = load(g_inner, g_savedInner, sizeof g_savedInner);

    check(run(NULL, kInner) && g_fresh, "civprobe folder only: child ran, log read");
    check(has("] settings: civprobe\\civprobe.ini\r\n"), "  logged as civprobe\\civprobe.ini");
    check(has("] settings: IntervalMs=1250\r\n") && has(" interval=1250ms "), "  its IntervalMs listed and used");

    check(run(kBeside, kInner) && g_fresh, "both: child ran, log read");
    check(has("] settings: civprobe.ini in the game folder (civprobe\\civprobe.ini is ignored"),
          "  the game folder's, the other logged as ignored");
    check(has(" interval=750ms "), "  the game folder's IntervalMs used");

    check(run(kBeside, NULL) && g_fresh, "game folder only: child ran, log read");
    check(has("] settings: civprobe.ini in the game folder\r\n"), "  logged as the game folder's");
    check(has("] settings: IntervalMs=750\r\n") && has(" interval=750ms "), "  its IntervalMs listed and used");

    check(run(NULL, NULL) && g_fresh, "neither: child ran, log read");
    check(has("] settings: none (all defaults)\r\n"), "  logged as none");
    check(has(" interval=2000ms "), "  the default IntervalMs used");

    char mistakes[512], longKey[201];
    memset(longKey, 'K', 200);
    longKey[200] = 0;
    snprintf(mistakes, sizeof mistakes,
             "[probe]\r\n;Telemetry=1\r\nIntervalMs=750\r\nEnabled=no\r\nHangSeconds=yes\r\n"
             "TextureHandles=16,384\r\nintervalms=500\r\nTelemetry:1\r\nTerrainBaseSlot=14000\r\n%s=1\r\n", longKey);
    check(run(mistakes, NULL) && g_fresh, "mistakes: child ran, Enabled=no left the fix on");
    check(has("] settings:   \"no\" isn't a number, so the fix stays on; Enabled=0 turns it off\r\n"),
          "  Enabled=no logged");
    check(has("] settings: HangSeconds=yes\r\n") && has("] settings:   \"yes\" isn't a number, so it counts as 0 (off)"),
          "  a word for a value logged");
    check(has("] settings:   16,384 counts as 16; write numbers without commas or spaces\r\n"),
          "  a grouped number logged with what it counts as");
    check(has("] settings: intervalms=500 doesn't count: intervalms is already set above it\r\n") &&
          has(" interval=750ms "), "  a setting set twice: the first counts, the second logged");
    check(has("] settings: \"Telemetry:1\" isn't a setting (Name=value), so it doesn't count\r\n"),
          "  a line without = logged");
    check(has("] settings: TerrainBaseSlot=14000 doesn't count: no setting has that name (check the spelling)\r\n"),
          "  a misspelled name logged");
    char cut[256];
    snprintf(cut, sizeof cut, "] settings: %.160s...= doesn't count: no setting has that name", longKey);
    check(has(cut), "  a 200-character line cut at 160 and logged");
    check(!has("] settings: Telemetry") && !has("] settings: ;"), "  a setting behind its ; not listed");
    check(!has("] settings: nothing set"), "  not called empty");

    check(run("[probe]\r\n;Telemetry=1\r\n", NULL) && g_fresh, "only ; lines: child ran, log read");
    check(has("] settings: nothing set (lines that start with ; are off)\r\n"), "  logged as nothing set");

    check(run("[probe]\r\nIntervalMs=750\r\n[Probe]\r\nIntervalMs=500\r\n", NULL) && g_fresh,
          "two [probe] lines: child ran, log read");
    check(has("] settings: the file has 2 [probe] lines; only the settings under the first one count\r\n"),
          "  logged");
    check(has(" interval=750ms "), "  the first section's IntervalMs used");

    check(run("IntervalMs=750\r\n", NULL) && g_fresh, "no [probe] line: child ran, log read");
    check(has("] settings: the file has no [probe] line, so none of its settings count\r\n"), "  logged");
    check(has(" interval=2000ms "), "  the default IntervalMs used");

    check(run("[probe]\r\nEnabled=0\r\n", NULL), "Enabled=0: child ran");
    check(!g_fresh, "  the fix stayed off: no events log");

    printf("%s\n", g_ok ? "ALL PASSED" : "FAILURES");
    return g_ok ? 0 : 1;
}
