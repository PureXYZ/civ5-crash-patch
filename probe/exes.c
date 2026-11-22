// Where the parts that change exact places find them in each build of the game: Steam's
// CivilizationV_DX11.exe, CivilizationV.exe (DirectX 9) and CivilizationV_Tablet.exe, all
// 1.0.3.279, as built in 2014 (the game's last patch) and as rebuilt in 2026 (Steam build
// 26577625, 2026-11-22). The 2014 DX11 addresses come from the research (docs/RESEARCH-NOTES.md);
// the other two were found with portmap.py (the DX11 code at each address, relocations and branch
// targets masked, searched in the other exe) and by hand where it found nothing (see the notes
// below). Every part still checks the code bytes it relies on before it changes anything.
//
// The 2026 exes give the same version and have the same code in a new order, without Steam's CEG
// checksums (cegall.py finds none of its loops or stubs). Their DX11 addresses were found by the
// byte patterns of each part's check; the other two with portmap.py from the 2026 DX11 exe, and by
// pattern where it found nothing or several places. One difference in the code: the terrain
// hand-over keeps its count in eax and its array in ecx (terrain.c takes both forms). The 2026
// exes don't raise the 2014 ones' NULL write at start (gameFault 0).
//
// Differences that matter:
// - Tablet: the render command stream's record allocator rounds each record up to 256 bytes.
// - DirectX 9: the render command stream is single-buffered (one set of arrays, groups 0x90
//   bytes, no frame flip that swaps halves; a reset function zeroes the counters instead), and
//   the third command appender has no list. The exe's aligned delete has FSBADeallocate inlined.
// - fluidpool (DX11 only): the other two have the same fluid fog code at portmap's addresses
//   (tablet 0x372490 / 0x16568d, DX9 0x99900 / 0x3acb7d), but there one or both of the places it
//   changes are in code Steam's CEG checksums (the complete scan of all 284 range loops). The
//   2026 tablet and DX9 exes keep it off as well: it hasn't run in them.
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

// The same operands in the 2026 DX11 exe (each at the same offset in its function).
static const TexSite kTexSitesDx11b[] = {
    // Release exe+0x5a540
    {0x5a579, 0xc044, TH_OFF}, {0x5a58b, 0xc048, TH_OFF},
    // Alloc exe+0x42bd50
    {0x42bd6e, 0x328b8, TH_OFF}, {0x42bd77, 0x328b4, TH_OFF}, {0x42bd7d, 0x32898, TH_OFF}, {0x42bddb, 0x3000, TH_COUNT},
    // constructor exe+0x42d300
    {0x42d30b, 0xc048, TH_OFF}, {0x42d32e, 0x3208c, TH_OFF}, {0x42d334, 0x32090, TH_OFF}, {0x42d33a, 0x32094, TH_OFF},
    {0x42d340, 0x32088, TH_OFF}, {0x42d34b, 0x32898, TH_OFF}, {0x42d351, 0x3289c, TH_OFF}, {0x42d357, 0x328a4, TH_OFF},
    {0x42d35d, 0x328a8, TH_OFF}, {0x42d363, 0x328a0, TH_OFF}, {0x42d369, 0x328b0, TH_OFF}, {0x42d36f, 0x328b4, TH_OFF},
    {0x42d374, 0x24000, TH_ITEMBYTES}, {0x42d37a, 0xe088, TH_OFF}, {0x42d380, 0x328ac, TH_OFF}, {0x42d386, 0x328b8, TH_OFF},
    {0x42d395, 0xc044, TH_OFF}, {0x42d39f, 0xc000, TH_REFBYTES},
    // destructor exe+0x42b4d0
    {0x42b4d8, 0x32898, TH_OFF}, {0x42b4e2, 0x328a4, TH_OFF}, {0x42b4e8, 0x3289c, TH_OFF}, {0x42b4ee, 0x328a0, TH_OFF},
    {0x42b4fb, 0x32088, TH_OFF}, {0x42b501, 0x32094, TH_OFF}, {0x42b517, 0xe088, TH_OFF},
    // frame flush exe+0x429480
    {0x42949e, 0xc044, TH_OFF}, {0x4294a4, 0x3208c, TH_OFF}, {0x4294b5, 0xc048, TH_OFF}, {0x4294be, 0xc04c, TH_OFF},
    {0x4294c4, 0x32088, TH_OFF}, {0x42950a, 0xc04c, TH_OFF}, {0x429514, 0xc044, TH_OFF}, {0x429521, 0xc044, TH_OFF},
    // recycle exe+0x42a970
    {0x42a990, 0x3208c, TH_OFF}, {0x42a999, 0x32898, TH_OFF}, {0x42a9a2, 0x32088, TH_OFF}, {0x42a9b4, 0x3208c, TH_OFF},
    {0x42a9bd, 0x3208c, TH_OFF}, {0x42a9d0, 0x3208c, TH_OFF},
};

static const GameExe kExes[] = {
    {
        .name = "DX11", .build = "2014", .timestamp = 0x546cd0a8, .imageSize = 0x6519000,
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
        .uiRegAlert = 0x24c94c, .uiAlertCall = 0x2bd9b9,
        .gridReserve = 0x76018, .gridReserveFn = 0x24c620, .grids = 0x991270, .gridFree = 0x413cf0,
        .luaHeapVar = 0x894b1c, .luaAlloc = 0x2ce440,
        .coreSite = 0x3258b3, .coreAcc = 0x8cf91c,
        .luaLock = 0x5042a0, .luaUnlock = 0x5042b0,
        .d3dContext = 0x2c10584, .d3dFlushUse = 0x4c12fb, .d3dFlushVB = 0x4c1319, .d3dFlushIB = 0x4c13d8,
        .texPool = 0x3d3e2c0, .texRecords = 0x3d70b80, .texPoolFixups = 50, .texRecFixups = 52,
        .texSiteCount = sizeof kTexSitesDx11 / sizeof kTexSitesDx11[0], .texSites = kTexSitesDx11,
        .terrainObj = 0x9165e0, .terrainPoolCall = 0x76967, .terrainInit = 0x2b8760, .terrainHandCount = 0x1bd079,
        .terrainHandArray = 0x1bd086, .terrainRelease = 0x2ccbfc, .terrainSort = 0x3ba990, .terrainLists = 0x865e0,
        .terrainMgrLoad = 0x11b090, .terrainAlloc = 0x1c60b0,
        .introCreate = 0x21a3f4, .introSkipCopy = 0x135742, .skipIntro = 0x8cf0c5,
        .fluidInit = 0x28f780, .fluidSeed = 0x17eeb0, .fluidReset = 0x109b2a, .fluidStepCall = 0x109c5d,
        .fluidJob = 0x1eb5b0, .fluidStepFn = 0x2b9f60, .fluidPassFn = 0x381a20, .fluidMerge = 0xd07d0, .fluidFree = 0x3658c3,
    },
    {
        // portmap.py; the record allocator (rounds up to 256 bytes), the Lua lock wrappers and the
        // TeamMet / data-dirty registrations (and so the generic listener handler) by hand.
        .name = "tablet", .build = "2014", .timestamp = 0x546cd5f8, .imageSize = 0x651b000,
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
        .uiRegAlert = 0x3af55c, .uiAlertCall = 0x24bbe9,
        .gridReserve = 0x247398, .gridReserveFn = 0x330d50, .grids = 0x992cf0, .gridFree = 0x414c80,
        .luaHeapVar = 0x896b1c, .luaAlloc = 0x3aaed0,
        .coreSite = 0xd4f83, .coreAcc = 0x8d139c,
        .luaLock = 0x505f60, .luaUnlock = 0x505f70,
        .d3dContext = 0x2c12584, .d3dFlushUse = 0x4c337b, .d3dFlushVB = 0x4c3399, .d3dFlushIB = 0x4c3458,
        .introCreate = 0x2ca374, .introSkipCopy = 0x2b1ed2, .skipIntro = 0x8d0b45,
        // No fluidpool: its seeding (0x372490) lies in CEG-checked code here.
    },
    {
        // portmap.py; the command stream (a different layout), the aligned delete, the Lua lock
        // wrappers and the TeamMet / data-dirty registrations by hand. No D3D11.
        .name = "DX9", .build = "2014", .timestamp = 0x546ccb59, .imageSize = 0x5a74000,
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
        .uiRegAlert = 0x874ec, .uiAlertCall = 0x27f879,
        .gridReserve = 0x292f8, .gridReserveFn = 0x61570, .grids = 0x9810c0, .gridFree = 0x41cdc0,
        .luaHeapVar = 0x884b1c, .luaAlloc = 0x10b280,
        .coreSite = 0x2e6b43, .coreAcc = 0x8bf764,
        .luaLock = 0x4f78a0, .luaUnlock = 0x4f78b0,
        .introCreate = 0x1f7e24, .introSkipCopy = 0x2742e2, .skipIntro = 0x8bef0d,
        // No fluidpool: its seeding (0x99900) and step launch call (0x3acb7d) lie in CEG-checked code here.
    },
    {
        .name = "DX11", .build = "2026", .timestamp = 0x6af3837d, .imageSize = 0x63a2000,
        .streamHalves = 2, .streamCtor = 0x4341d0, .ctorRets = {0x43421a, 0x434245, 0x434265},
        .streamSplit = 0x434268, .streamHalvesSite = 0x43428a, .streamBump = 0x435e40,
        .bounds = {0x432e79, 0x432efb, 0x432f7d, 0x433bb0, 0x433cb6, 0x433d1e, 0x433d8e, 0x433dee},
        .flipCall = 0x423a67, .flip = 0x433e00,
        .spareCallers = {0x281340, 0x281362, 0x281369}, .pairPush = {0x28132a, 0x281342},
        // queuePost: the record type 0x49 post, one of 14 with these bytes (all use the same import).
        .queueObj = 0x1493980, .queuePass = 0x958b0, .queueFlip = 0x93b50, .queuePost = 0x2f8000,
        .queueWalk = 0x93bc2, .queueMainSite = 0x9f7c2,
        .uiProcess = 0x9d900, .uiOwner = 0x2495780, .uiLocked = 0x28712d7, .uiGame = 0x749338,
        .uiMode = 0x2496500, .uiSite = 0x9f741, .uiRet1 = 0x69f9d, .uiRet2 = 0x6a11d,
        .uiDispatch = 0x9b6c0, .uiListeners = 0x1f3a50, .uiRegDirty = 0x1f70dc, .uiRegTeamMet = 0x1f6cdc,
        .uiRegAlert = 0x1f775c, .uiAlertCall = 0x1f9099,
        .gridReserve = 0x98c88, .gridReserveFn = 0x111030, .grids = 0x816820, .gridFree = 0x5e080,
        .luaHeapVar = 0x725adc, .luaAlloc = 0x376060,
        .coreSite = 0xcf8b3, .coreAcc = 0x754e5c,
        .luaLock = 0x46a060, .luaUnlock = 0x46a070,
        .d3dContext = 0x2aa1444, .d3dFlushUse = 0x4273ab, .d3dFlushVB = 0x4273c9, .d3dFlushIB = 0x427488,
        .texPool = 0x3bcf180, .texRecords = 0x3c01a40, .texPoolFixups = 50, .texRecFixups = 52,
        .texSiteCount = sizeof kTexSitesDx11b / sizeof kTexSitesDx11b[0], .texSites = kTexSitesDx11b,
        .terrainObj = 0x79bb90, .terrainPoolCall = 0x995d7, .terrainInit = 0x121b90, .terrainHandCount = 0x122586,
        .terrainHandArray = 0x122593, .terrainRelease = 0x120a3c, .terrainSort = 0x134010, .terrainLists = 0x131130,
        .terrainMgrLoad = 0x11b070, .terrainAlloc = 0x132480,
        .introCreate = 0xd5434, .introSkipCopy = 0x97732, .skipIntro = 0x74f5f5,
        .fluidInit = 0x1b3010, .fluidSeed = 0x1aeef0, .fluidReset = 0x1b249a, .fluidStepCall = 0x1b25cd,
        .fluidJob = 0x1b0e90, .fluidStepFn = 0x1b0520, .fluidPassFn = 0x1af410, .fluidMerge = 0x1b2040, .fluidFree = 0x1b22a3,
    },
    {
        .name = "tablet", .build = "2026", .timestamp = 0x6af385ba, .imageSize = 0x63a5000,
        .streamHalves = 2, .recordRound = 1, .streamCtor = 0x436250, .ctorRets = {0x43629a, 0x4362c5, 0x4362e5},
        .streamSplit = 0x4362e8, .streamHalvesSite = 0x43630a, .streamBump = 0x437ee0,
        .bounds = {0x434ee9, 0x434f6b, 0x434fed, 0x435c20, 0x435d26, 0x435d8e, 0x435dfe, 0x435e5e},
        .flipCall = 0x4254a7, .flip = 0x435e70,
        .spareCallers = {0x281550, 0x281572, 0x281579}, .pairPush = {0x28153a, 0x281552},
        .queueObj = 0x1496980, .queuePass = 0x95880, .queueFlip = 0x93b90, .queuePost = 0x2f99e0,
        .queueWalk = 0x93c02, .queueMainSite = 0x9f432,
        .uiProcess = 0x9d570, .uiOwner = 0x2498780, .uiLocked = 0x28742df, .uiGame = 0x74c338,
        .uiMode = 0x2499500, .uiSite = 0x9f3b1, .uiRet1 = 0x6a02d, .uiRet2 = 0x6a1ad,
        .uiDispatch = 0x9b330, .uiListeners = 0x1ed7b0, .uiRegDirty = 0x1f736c, .uiRegTeamMet = 0x1f6f6c,
        .uiRegAlert = 0x1f79ec, .uiAlertCall = 0x1f9329,
        .gridReserve = 0x98898, .gridReserveFn = 0x110d90, .grids = 0x819820, .gridFree = 0x5e2d0,
        .luaHeapVar = 0x728adc, .luaAlloc = 0x377ef0,
        .coreSite = 0xcf7a3, .coreAcc = 0x757e5c,
        .luaLock = 0x46bbc0, .luaUnlock = 0x46bbd0,
        .d3dContext = 0x2aa4444, .d3dFlushUse = 0x42937b, .d3dFlushVB = 0x429399, .d3dFlushIB = 0x429458,
        .introCreate = 0xd5304, .introSkipCopy = 0x97342, .skipIntro = 0x7525f5,
    },
    {
        // One set of command stream arrays, as in the 2014 DX9 exe. No D3D11.
        .name = "DX9", .build = "2026", .timestamp = 0x6af3818f, .imageSize = 0x58ff000,
        .streamHalves = 1, .streamCtor = 0x4530d0, .ctorRets = {0x453118, 0x453141, 0x45315e},
        .streamBump = 0x454d80,
        .bounds = {0x451ec9, 0x451f4b, 0x451fcc, 0x452b23, 0x452c5a, 0x452ccd, 0x452d50, 0x452dbd},
        .streamReset = 0x452680,
        .spareCallers = {0x2b0050, 0x2b0072, 0x2b0079}, .pairPush = {0x2b003a, 0x2b0052},
        .queueObj = 0x1483800, .queuePass = 0xc7710, .queueFlip = 0xc59e0, .queuePost = 0x326a70,
        .queueWalk = 0xc5a52, .queueMainSite = 0xd0c19,
        .uiProcess = 0xced50, .uiOwner = 0x2485600, .uiLocked = 0x2861077, .uiGame = 0x739238,
        .uiMode = 0x2486380, .uiSite = 0xd0b98, .uiRet1 = 0x7e87d, .uiRet2 = 0x7e9fd,
        .uiDispatch = 0xccd60, .uiListeners = 0x21c4d0, .uiRegDirty = 0x22648c, .uiRegTeamMet = 0x22608c,
        .uiRegAlert = 0x226b0c, .uiAlertCall = 0x228459,
        .gridReserve = 0xca468, .gridReserveFn = 0x1412c0, .grids = 0x806680, .gridFree = 0x73050,
        .luaHeapVar = 0x715adc, .luaAlloc = 0x3a53a0,
        .coreSite = 0x100843, .coreAcc = 0x744d0c,
        .luaLock = 0x45d500, .luaUnlock = 0x45d510,
        .introCreate = 0x1063f4, .introSkipCopy = 0xc8f12, .skipIntro = 0x73f4a5,
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
