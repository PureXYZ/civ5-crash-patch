// Where the parts that change exact places find them in each build of the game: the final Steam
// CivilizationV_DX11.exe, CivilizationV.exe (DirectX 9) and CivilizationV_Tablet.exe, all
// 1.0.3.279. The DX11 addresses come from the research (docs/RESEARCH-NOTES.md); the other two
// were found with portmap.py (the DX11 code at each address, relocations and branch targets
// masked, searched in the other exe) and by hand where it found nothing (see the notes below).
// Every part still checks the code bytes it relies on before it changes anything.
//
// Differences that matter:
// - Tablet: the render command stream's record allocator rounds each record up to 256 bytes.
// - DirectX 9: the render command stream is single-buffered (one set of arrays, groups 0x90
//   bytes, no frame flip that swaps halves; a reset function zeroes the counters instead), and
//   the third command appender has no list. The exe's aligned delete has FSBADeallocate inlined.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string.h>
#include "probe.h"

// texhandles.c: the DX11 texture handle pool's functions address it through `this`; these are the
// 4-byte operands that hold a pool offset past the refcounts, the handle cap or a memset size.
static const TexSite kTexSitesDx11[] = {
    // Release exe+0x1e8df0
    {0x1e8e29, 0xc044, TH_OFF}, {0x1e8e3b, 0xc048, TH_OFF},
    // Alloc exe+0x4c5cb0
    {0x4c5cce, 0x328b8, TH_OFF}, {0x4c5cd7, 0x328b4, TH_OFF}, {0x4c5cdd, 0x32898, TH_OFF}, {0x4c5d3b, 0x3000, TH_COUNT},
    // constructor exe+0x4c7280
    {0x4c728b, 0xc048, TH_OFF}, {0x4c72ae, 0x3208c, TH_OFF}, {0x4c72b4, 0x32090, TH_OFF}, {0x4c72ba, 0x32094, TH_OFF},
    {0x4c72c0, 0x32088, TH_OFF}, {0x4c72cb, 0x32898, TH_OFF}, {0x4c72d1, 0x3289c, TH_OFF}, {0x4c72d7, 0x328a4, TH_OFF},
    {0x4c72dd, 0x328a8, TH_OFF}, {0x4c72e3, 0x328a0, TH_OFF}, {0x4c72e9, 0x328b0, TH_OFF}, {0x4c72ef, 0x328b4, TH_OFF},
    {0x4c72f4, 0x24000, TH_ITEMBYTES}, {0x4c72fa, 0xe088, TH_OFF}, {0x4c7300, 0x328ac, TH_OFF}, {0x4c7306, 0x328b8, TH_OFF},
    {0x4c7315, 0xc044, TH_OFF}, {0x4c731f, 0xc000, TH_REFBYTES},
    // destructor exe+0x4c5430
    {0x4c5438, 0x32898, TH_OFF}, {0x4c5442, 0x328a4, TH_OFF}, {0x4c5448, 0x3289c, TH_OFF}, {0x4c544e, 0x328a0, TH_OFF},
    {0x4c545b, 0x32088, TH_OFF}, {0x4c5461, 0x32094, TH_OFF}, {0x4c5477, 0xe088, TH_OFF},
    // frame flush exe+0x4c33d0
    {0x4c33ee, 0xc044, TH_OFF}, {0x4c33f4, 0x3208c, TH_OFF}, {0x4c3405, 0xc048, TH_OFF}, {0x4c340e, 0xc04c, TH_OFF},
    {0x4c3414, 0x32088, TH_OFF}, {0x4c345a, 0xc04c, TH_OFF}, {0x4c3464, 0xc044, TH_OFF}, {0x4c3471, 0xc044, TH_OFF},
    // recycle exe+0x4c48d0
    {0x4c48f0, 0x3208c, TH_OFF}, {0x4c48f9, 0x32898, TH_OFF}, {0x4c4902, 0x32088, TH_OFF}, {0x4c4914, 0x3208c, TH_OFF},
    {0x4c491d, 0x3208c, TH_OFF}, {0x4c4930, 0x3208c, TH_OFF},
};

static const GameExe kExes[] = {
    {
        .name = "DX11", .timestamp = 0x546cd0a8, .imageSize = 0x6519000,
        .gameFault = 0x523dcb,
        .streamHalves = 2, .streamCtor = 0x4ce150, .ctorRets = {0x4ce19a, 0x4ce1c5, 0x4ce1e5},
        .streamSplit = 0x4ce1e8, .streamHalvesSite = 0x4ce20a, .streamBump = 0x4cfdc0,
        .bounds = {0x4ccdf9, 0x4cce7b, 0x4ccefd, 0x4cdb30, 0x4cdc36, 0x4cdc9e, 0x4cdd0e, 0x4cdd6e},
        .flipCall = 0x4bd9a7, .flip = 0x4cdd80,
        .spareCallers = {0x1c9940, 0x1c9962, 0x1c9969}, .pairPush = {0x1c992a, 0x1c9942},
        .queueObj = 0x160e400, .queuePass = 0x59d20, .queueFlip = 0x90df0, .queuePost = 0xc230,
        .queueWalk = 0x2a5992, .queueMainSite = 0xb4062,
        .uiProcess = 0x385af0, .uiOwner = 0x2610200, .uiLocked = 0x29e0447, .uiGame = 0x8c8e74,
        .uiMode = 0x2610f80, .uiSite = 0xb3fe1, .uiRet1 = 0x41db0d, .uiRet2 = 0x41dc8d,
        .uiDispatch = 0x23aba0, .uiListeners = 0x23e530, .uiRegDirty = 0xc583c, .uiRegTeamMet = 0x4a4ec,
        .gridReserve = 0x76018, .gridReserveFn = 0x24c620, .grids = 0x991270, .gridFree = 0x413cf0,
        .luaHeapVar = 0x894b1c, .luaAlloc = 0x2ce440,
        .coreSite = 0x3258b3, .coreAcc = 0x8cf91c,
        .luaLock = 0x5042a0, .luaUnlock = 0x5042b0,
        .d3dContext = 0x2c10584, .d3dFlushUse = 0x4c12fb, .d3dFlushVB = 0x4c1319, .d3dFlushIB = 0x4c13d8,
        .texPool = 0x3d3e2c0, .texRecords = 0x3d70b80, .texPoolFixups = 50, .texRecFixups = 52,
        .texSiteCount = sizeof kTexSitesDx11 / sizeof kTexSitesDx11[0], .texSites = kTexSitesDx11,
        .terrainObj = 0x9165e0, .terrainPoolCall = 0x76967, .terrainInit = 0x2b8760, .terrainHandCount = 0x1bd079,
        .terrainHandArray = 0x1bd086, .terrainRelease = 0x2ccbfc, .terrainMgrLoad = 0x11b090, .terrainAlloc = 0x1c60b0,
    },
    {
        // portmap.py; the record allocator (rounds up to 256 bytes), the Lua lock wrappers and the
        // TeamMet / data-dirty registrations (and so the generic listener handler) by hand.
        .name = "tablet", .timestamp = 0x546cd5f8, .imageSize = 0x651b000,
        .gameFault = 0x525a4b,
        .streamHalves = 2, .recordRound = 1, .streamCtor = 0x4d0220, .ctorRets = {0x4d026a, 0x4d0295, 0x4d02b5},
        .streamSplit = 0x4d02b8, .streamHalvesSite = 0x4d02da, .streamBump = 0x4d1eb0,
        .bounds = {0x4ceeb9, 0x4cef3b, 0x4cefbd, 0x4cfbf0, 0x4cfcf6, 0x4cfd5e, 0x4cfdce, 0x4cfe2e},
        .flipCall = 0x4bf4a7, .flip = 0x4cfe40,
        .spareCallers = {0x4cae0, 0x4cb02, 0x4cb09}, .pairPush = {0x4caca, 0x4cae2},
        .queueObj = 0x160fe80, .queuePass = 0xeef60, .queueFlip = 0xace00, .queuePost = 0x1c3950,
        .queueWalk = 0x398102, .queueMainSite = 0x37c152,
        .uiProcess = 0x2f8360, .uiOwner = 0x2611c80, .uiLocked = 0x29e242f, .uiGame = 0x8ca8f0,
        .uiMode = 0x2612a00, .uiSite = 0x37c0d1, .uiRet1 = 0x41e92d, .uiRet2 = 0x41eaad,
        .uiDispatch = 0xd7640, .uiListeners = 0x3b82e0, .uiRegDirty = 0x2471bc, .uiRegTeamMet = 0xeaa3c,
        .gridReserve = 0x247398, .gridReserveFn = 0x330d50, .grids = 0x992cf0, .gridFree = 0x414c80,
        .luaHeapVar = 0x896b1c, .luaAlloc = 0x3aaed0,
        .coreSite = 0xd4f83, .coreAcc = 0x8d139c,
        .luaLock = 0x505f60, .luaUnlock = 0x505f70,
        .d3dContext = 0x2c12584, .d3dFlushUse = 0x4c337b, .d3dFlushVB = 0x4c3399, .d3dFlushIB = 0x4c3458,
    },
    {
        // portmap.py; the command stream (a different layout), the aligned delete, the Lua lock
        // wrappers and the TeamMet / data-dirty registrations by hand. No D3D11.
        .name = "DX9", .timestamp = 0x546ccb59, .imageSize = 0x5a74000,
        .gameFault = 0x4f9bcb,
        .streamHalves = 1, .streamCtor = 0x4ed490, .ctorRets = {0x4ed4d8, 0x4ed501, 0x4ed51e},
        .streamBump = 0x4ef140,
        .bounds = {0x4ec289, 0x4ec30b, 0x4ec38c, 0x4ecee3, 0x4ed01a, 0x4ed08d, 0x4ed110, 0x4ed17d},
        .streamReset = 0x4eca40,
        .spareCallers = {0x1f1a50, 0x1f1a72, 0x1f1a79}, .pairPush = {0x1f1a3a, 0x1f1a52},
        .queueObj = 0x15fe200, .queuePass = 0x2022d0, .queueFlip = 0x385420, .queuePost = 0x98a50,
        .queueWalk = 0x20f62, .queueMainSite = 0x84819,
        .uiProcess = 0x396030, .uiOwner = 0x2600000, .uiLocked = 0x29d0247, .uiGame = 0x8b8cf4,
        .uiMode = 0x2600d80, .uiSite = 0x84798, .uiRet1 = 0x4265ed, .uiRet2 = 0x42676d,
        .uiDispatch = 0x2492d0, .uiListeners = 0x328e00, .uiRegDirty = 0x390ac, .uiRegTeamMet = 0x6de6c,
        .gridReserve = 0x292f8, .gridReserveFn = 0x61570, .grids = 0x9810c0, .gridFree = 0x41cdc0,
        .luaHeapVar = 0x884b1c, .luaAlloc = 0x10b280,
        .coreSite = 0x2e6b43, .coreAcc = 0x8bf764,
        .luaLock = 0x4f78a0, .luaUnlock = 0x4f78b0,
    },
};

const GameExe *game_exe_dx11(void) { return &kExes[0]; }

const GameExe *game_exe(const BYTE *exe)
{
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(exe + ((const IMAGE_DOS_HEADER *)exe)->e_lfanew);
    for (size_t i = 0; i < sizeof kExes / sizeof kExes[0]; i++)
        if (nt->FileHeader.TimeDateStamp == kExes[i].timestamp && nt->OptionalHeader.SizeOfImage == kExes[i].imageSize)
            return &kExes[i];
    return NULL;
}

// The exe's import slot for `name` (any DLL), found through the import names, so it works whether
// or not the imports are bound (a mapped exe file in the self-test isn't).
BYTE *exe_import_slot(BYTE *exe, const char *name)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(exe + dir.VirtualAddress); dir.Size && d->Name; d++) {
        if (!d->OriginalFirstThunk) continue;
        IMAGE_THUNK_DATA32 *names = (IMAGE_THUNK_DATA32 *)(exe + d->OriginalFirstThunk);
        for (DWORD i = 0; names[i].u1.AddressOfData; i++) {
            if (names[i].u1.Ordinal & IMAGE_ORDINAL_FLAG32) continue;
            if (!strcmp((const char *)((IMAGE_IMPORT_BY_NAME *)(exe + names[i].u1.AddressOfData))->Name, name))
                return exe + d->FirstThunk + 4 * i;
        }
    }
    return NULL;
}

// The target of the E8/E9 rel32 at p (p itself inside the image), or NULL.
BYTE *exe_branch_target(BYTE *exe, const BYTE *p)
{
    IMAGE_NT_HEADERS32 *nt = (IMAGE_NT_HEADERS32 *)(exe + ((IMAGE_DOS_HEADER *)exe)->e_lfanew);
    if (p < exe || p + 5 > exe + nt->OptionalHeader.SizeOfImage || (p[0] != 0xE8 && p[0] != 0xE9)) return NULL;
    return (BYTE *)p + 5 + *(const LONG *)(p + 1);
}
