// The settings file's report in the events log: which civprobe.ini is read, every setting in
// effect, and the lines that don't do what they look like.
//
// Windows' profile functions read only the first [probe] section and the first line of each key,
// take a word ("yes", "on") as 0 and stop a number at a thousands separator (16,384 reads 16), all
// without a sign. A player who wrote any of these would see the setting do nothing, or the
// opposite. The report names each case. Windows hands back the section with comment lines
// dropped and the spaces around "=" removed, but keeps lines that aren't key=value.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdlib.h>
#include <string.h>
#include "probe.h"

// Enabled=0 turns the patch off. Only a real 0 does: Windows also reads a word or an empty value
// as 0.
int settings_enabled(const WCHAR *ini)
{
    WCHAR v[16];
    GetPrivateProfileStringW(L"probe", L"Enabled", L"1", v, 16, ini);
    const WCHAR *p = v;
    while (*p == L' ' || *p == L'\t') p++;
    return GetPrivateProfileIntW(L"probe", L"Enabled", 1, ini) != 0 || *p != L'0';
}

// A word players write for on or off.
static int is_word(const char *v)
{
    static const char *const kWords[] = {"on", "off", "yes", "no", "true", "false", "enabled", "disabled"};
    for (size_t i = 0; i < sizeof kWords / sizeof *kWords; i++)
        if (!_stricmp(v, kWords[i])) return 1;
    return 0;
}

// Every setting the DLL reads. build.sh stops the build when a name read in the source is missing.
static int is_known(const char *key)
{
    static const char *const kKnown[] = {
        "AllocBigBytes", "AllocCrt", "AllocReportSeconds", "AllocSampleShift", "AllocTracking",
        "AudioGuard", "AudioGuardTrack", "CommandStreamCap", "CommandStreamCmdSpare",
        "CommandStreamGroupSpare", "CommandStreamMB", "CommandStreamSlackKB",
        "CommandStreamSpareCallers", "CommandStreamSplit", "CommandStreamStats",
        "CommandStreamTestRets", "CrtFailureLog", "D3DStats", "DbHeapMimalloc", "Enabled",
        "FluidPools", "GameCoreIntervalMs", "GpuWatch", "HangRepeatSeconds", "HangSeconds",
        "HeapHistogram", "HeapStats", "IntervalMs", "KeepSessions", "LastRunCheck",
        "LowMemoryWarnInGame", "LowMemoryWarnMB", "LuaGcCrt", "LuaGcModule", "LuaGcSliceUs",
        "LuaGcStepMB", "LuaHeapMimalloc", "LuaUnrefGuard", "MapGridReserve", "MiArenaMB",
        "MiLargePadBytes", "MiMalloc", "MiMaxKB", "MiPurgeMs", "MsgQueueGuard", "PeakReportStepMB",
        "PeriodicSnapshotMinutes", "ReserveLog", "ReserveLogMinKB", "SkipIntroPlayer",
        "SmallBlockFallback", "SoundLog", "Telemetry", "TerrainBaseSlots", "TextureHandles",
        "TopDown", "TopDownViews", "UiDirtyCoalesceMs", "UiQueueFrameMs", "UiQueuePasses",
        "UiQueueShare", "UiQueueStats", "UiTeamMetCoalesceMs", "Watch", "WatchCallerRva",
        "WatchGlobalExport", "WatchGlobalRva", "WatchLen", "WatchMode", "WatchOffset", "WatchSize",
    };
    for (size_t i = 0; i < sizeof kKnown / sizeof *kKnown; i++)
        if (!_stricmp(key, kKnown[i])) return 1;
    return 0;
}

// Digits in groups of three after the first: 16,384, 16 384 or 16.384.
static int grouped(const char *v)
{
    int lead = 0;
    while (v[lead] >= '0' && v[lead] <= '9') lead++;
    if (lead < 1 || lead > 3 || !v[lead]) return 0;
    for (v += lead; *v; v += 4) {
        if (*v != ',' && *v != ' ' && *v != '.') return 0;
        for (int i = 1; i <= 3; i++)
            if (v[i] < '0' || v[i] > '9') return 0;
    }
    return 1;
}

void settings_report(const WCHAR *ini, int beside, int inner)
{
    if (!beside && !inner) {
        log_event("settings: none (all defaults)");
        return;
    }
    if (beside)
        log_event("settings: civprobe.ini in the game folder%s",
                  inner ? " (civprobe\\civprobe.ini is ignored while it is there)" : "");
    else
        log_event("settings: civprobe\\civprobe.ini");

    static WCHAR buf[16384];
    DWORD n = GetPrivateProfileSectionNamesW(buf, 16384, ini);
    int sections = 0;
    for (const WCHAR *s = buf; s < buf + n && *s; s += lstrlenW(s) + 1)
        if (!lstrcmpiW(s, L"probe")) sections++;
    if (!sections) {
        log_event("settings: the file has no [probe] line, so none of its settings count");
        return;
    }
    if (sections > 1)
        log_event("settings: the file has %d [probe] lines; only the settings under the first one count", sections);

    n = GetPrivateProfileSectionW(L"probe", buf, 16384, ini);
    int set = 0;
    for (const WCHAR *s = buf; s < buf + n && *s; s += lstrlenW(s) + 1) {
        // The first 160 characters (at most 480 UTF-8 bytes); a longer line ends in "...".
        char line[512];
        int wn = lstrlenW(s), cut = wn > 160;
        int bn = WideCharToMultiByte(CP_UTF8, 0, s, cut ? 160 : wn, line, sizeof line - 4, NULL, NULL);
        strcpy(line + bn, cut ? "..." : "");
        const WCHAR *eq = wcschr(s, L'=');
        if (!eq || eq == s) {
            log_event("settings: \"%s\" isn't a setting (Name=value), so it doesn't count", line);
            continue;
        }
        size_t klen = (size_t)(eq - s);
        int twice = 0;
        for (const WCHAR *t = buf; t < s && !twice; t += lstrlenW(t) + 1) {
            const WCHAR *te = wcschr(t, L'=');
            twice = te && (size_t)(te - t) == klen && !_wcsnicmp(t, s, klen);
        }
        char *leq = strchr(line, '=');
        const char *key = line, *raw = leq ? leq + 1 : "";
        if (leq) *leq = 0;
        if (twice) {
            log_event("settings: %s=%s doesn't count: %s is already set above it", key, raw, key);
            continue;
        }
        if (!is_known(key)) {
            log_event("settings: %s=%s doesn't count: no setting has that name (check the spelling)", key, raw);
            continue;
        }
        set++;
        log_event("settings: %s=%s", key, raw);
        // The value without a comment after it and without spaces at its ends.
        char v[64];
        size_t vlen = strcspn(raw, ";");
        while (vlen && (raw[vlen - 1] == ' ' || raw[vlen - 1] == '\t')) vlen--;
        if (vlen >= sizeof v) continue;
        memcpy(v, raw, vlen);
        v[vlen] = 0;
        if (is_word(v) && !_stricmp(key, "Enabled"))
            log_event("settings:   \"%s\" isn't a number, so the fix stays on; Enabled=0 turns it off", v);
        else if (is_word(v))
            log_event("settings:   \"%s\" isn't a number, so it counts as 0 (off); write 1 for on, 0 for off", v);
        else if (grouped(v))
            log_event("settings:   %s counts as %ld; write numbers without commas or spaces", v, strtol(v, NULL, 10));
    }
    if (!set) log_event("settings: nothing set (lines that start with ; are off)");
}
