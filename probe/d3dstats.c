// What the game asks of D3D11 ([probe] D3DStats, default 0; diagnostic, changes nothing).
//
// In a late-game session the GPU driver made ~30 upload allocations a second, and the game made
// ~41 whole-buffer UpdateSubresource calls a second (session 20261030_122543), but a local test of in-use buffers barely allocates. This counts what the game's
// buffers and calls actually look like: UpdateSubresource and Map on the context, CreateBuffer and
// CreateTexture2D on the device, each by resource size, usage, bind flags and the game code that
// called it. The four D3D11 functions are hot-patched in d3d11.dll (Microsoft's 2-byte no-op entry
// and 5 spare bytes before it), found through the game's context table, which is only read
// (writing that table entry instead made the game abort, sessions 20261030_121139 / _121552; the device's two are patched from the main
// thread at the first upload, through ID3D11DeviceContext::GetDevice). Each wrapper only counts and
// calls the original.
#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <string.h>
#include "probe.h"

// GameExe: d3dContext (the game's D3D11 context pointer, DX11 exe+0x2c10584), d3dFlushUse (a
// use of it, exe+0x4c12fb), d3dFlushVB / d3dFlushIB (the upload flush's Map return addresses).
#define SLOT_MAP 14
#define SLOT_UPDATE 48
#define SLOT_CREATE_BUFFER 3
#define SLOT_CREATE_TEX2D 5

typedef void(STDMETHODCALLTYPE *update_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT, const D3D11_BOX *, const void *, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *map_t)(ID3D11DeviceContext *, ID3D11Resource *, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE *);
typedef HRESULT(STDMETHODCALLTYPE *cbuf_t)(ID3D11Device *, const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
typedef HRESULT(STDMETHODCALLTYPE *ctex_t)(ID3D11Device *, const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);
static update_t real_update;
static map_t real_map;
static cbuf_t real_cbuf;
static ctex_t real_ctex;
static BYTE *g_exe;
static const GameExe *g_g;
static void *g_rejected;
static int g_ctxPatched;
static volatile LONG g_devPatched;

// Size buckets: <=1K, <=4K, <=16K, <=64K, <=256K, <=1M, <=4M, more.
#define NB 8
static int bucket(UINT n) { return n <= 1024 ? 0 : n <= 4096 ? 1 : n <= 16384 ? 2 : n <= 65536 ? 3 : n <= 262144 ? 4 : n <= 1048576 ? 5 : n <= 4194304 ? 6 : 7; }
static const char *kBucket[NB] = {"<=1K", "<=4K", "<=16K", "<=64K", "<=256K", "<=1M", "<=4M", ">4M"};

typedef struct { volatile LONG n; volatile LONGLONG bytes; } Cnt;
static void add(Cnt *c, UINT bytes) { InterlockedIncrement(&c->n); InterlockedExchangeAdd64(&c->bytes, bytes); }

// Buffer updates: by size (whole buffer / with a box), and by usage+bind.
static Cnt g_updBuf[NB], g_updBufBox, g_updTex, g_updOther;
// Map: by type (1 READ .. 5 WRITE_NO_OVERWRITE) for buffers, by size for WRITE_DISCARD buffers; textures.
static Cnt g_mapType[6], g_mapDiscard[NB], g_mapTex;
// Creation: buffers by size, with / without initial data; textures.
static Cnt g_cbuf[NB], g_cbufInit, g_ctex, g_ctexInit;

// "Kinds": usage (0-3) x bind flags, for buffers updated, discard-mapped and created.
#define KINDS 32
typedef struct { volatile LONG key; Cnt c; } Kind;
static Kind g_kUpd[KINDS], g_kMap[KINDS], g_kCre[KINDS];
static void kind(Kind *k, UINT usage, UINT bind, UINT cpu, UINT bytes)
{
    LONG key = (LONG)(1 | usage << 1 | (cpu ? 1 : 0) << 3 | bind << 4);
    for (int i = 0; i < KINDS; i++) {
        LONG cur = k[i].key;
        if (!cur) cur = InterlockedCompareExchange(&k[i].key, key, 0);
        if (!cur || cur == key) { add(&k[i].c, bytes); return; }
    }
}

// Call sites (the game code that called), per category.
#define SITES 12
typedef struct { void *volatile ret; Cnt c; } Site;
static Site g_sUpd[SITES], g_sMap[SITES], g_sCre[SITES];
static void site(Site *s, void *ret, UINT bytes)
{
    for (int i = 0; i < SITES; i++) {
        void *cur = s[i].ret;
        if (!cur) cur = InterlockedCompareExchangePointer((PVOID volatile *)&s[i].ret, ret, NULL);
        if (!cur || cur == ret) { add(&s[i].c, bytes); return; }
    }
}

// Distinct buffers updated (a rough set; a freed and reused pointer counts once).
#define SET 8192
static void *volatile g_set[SET];
static volatile LONG g_distinct, g_setFull;
static void note_buffer(void *p)
{
    ULONG_PTR h = ((ULONG_PTR)p >> 4) * 2654435761u;
    for (int i = 0; i < 64; i++) {
        void *volatile *slot = &g_set[(h + i) & (SET - 1)];
        void *cur = *slot;
        if (cur == p) return;
        if (!cur && !InterlockedCompareExchangePointer((PVOID volatile *)slot, p, NULL)) { InterlockedIncrement(&g_distinct); return; }
    }
    InterlockedIncrement(&g_setFull);
}

// The game's two flush loops (exe+0x4c1319 vertex buffers, exe+0x4c13d8 index buffers) keep the
// bytes they're about to copy into the mapped buffer at [esi - 4], esi being their list entry. The
// Map hot-patch goes through map_entry, which saves the caller's esi (per thread: every thread's
// Map calls come through here) before the C wrapper runs.
static __thread ULONG_PTR t_callerEsi;
void __cdecl d3dstats_save_esi(ULONG_PTR esi) __asm__("d3dstats_save_esi");
void __cdecl d3dstats_save_esi(ULONG_PTR esi)
{
    t_callerEsi = esi;  // clobbers only eax/ecx/edx, free at Map's entry (its arguments are on the stack)
}
void *d3dstats_hmap __asm__("d3dstats_hmap");
__attribute__((naked)) static void map_entry(void)
{
    __asm__("pushl %esi\n\tcall d3dstats_save_esi\n\taddl $4, %esp\n\tjmp *d3dstats_hmap");
}
static Cnt g_wrote[2][NB], g_wroteBuf[2];  // per site: bytes written by size; sum of the buffers' sizes
static volatile LONG g_wroteBad;
#define DYN 64
static struct { void *volatile p; volatile LONG width, n; } g_dyn[2][DYN];  // distinct buffers per site
static void note_dyn(int k, void *p, UINT width)
{
    for (int i = 0; i < DYN; i++) {
        void *cur = g_dyn[k][i].p;
        if (!cur) cur = InterlockedCompareExchangePointer((PVOID volatile *)&g_dyn[k][i].p, p, NULL);
        if (!cur || cur == p) { g_dyn[k][i].width = (LONG)width; InterlockedIncrement(&g_dyn[k][i].n); return; }
    }
}

static int hotpatch(BYTE *fn, void *hook, void **real);
static HRESULT STDMETHODCALLTYPE h_cbuf(ID3D11Device *, const D3D11_BUFFER_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Buffer **);
static HRESULT STDMETHODCALLTYPE h_ctex(ID3D11Device *, const D3D11_TEXTURE2D_DESC *, const D3D11_SUBRESOURCE_DATA *, ID3D11Texture2D **);

// Main thread, first upload or map: patch the device's two creation functions.
static void patch_device(ID3D11DeviceContext *ctx)
{
    if (g_devPatched || InterlockedExchange(&g_devPatched, 1)) return;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext_GetDevice(ctx, &dev);
    if (!dev) return;
    void **vt = *(void ***)dev;
    int a = hotpatch((BYTE *)vt[SLOT_CREATE_BUFFER], (void *)h_cbuf, (void **)&real_cbuf);
    int b = hotpatch((BYTE *)vt[SLOT_CREATE_TEX2D], (void *)h_ctex, (void **)&real_ctex);
    ID3D11Device_Release(dev);
    log_event("d3dstats: device %p: CreateBuffer %s, CreateTexture2D %s", (void *)dev, a ? "hot-patched" : "NOT patchable",
              b ? "hot-patched" : "NOT patchable");
}

static void STDMETHODCALLTYPE h_update(ID3D11DeviceContext *ctx, ID3D11Resource *dst, UINT sub, const D3D11_BOX *box,
                                       const void *src, UINT rowPitch, UINT depthPitch)
{
    void *ret = __builtin_return_address(0);
    if (!g_devPatched) patch_device(ctx);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (dst) ID3D11Resource_GetType(dst, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc((ID3D11Buffer *)dst, &d);
        UINT n = box ? box->right - box->left : d.ByteWidth;
        if (box) add(&g_updBufBox, n);
        else add(&g_updBuf[bucket(n)], n);
        kind(g_kUpd, d.Usage, d.BindFlags, d.CPUAccessFlags, n);
        site(g_sUpd, ret, n);
        note_buffer(dst);
    } else if (dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        add(&g_updTex, 0);
    } else {
        add(&g_updOther, 0);
    }
    real_update(ctx, dst, sub, box, src, rowPitch, depthPitch);
}

static HRESULT STDMETHODCALLTYPE h_map(ID3D11DeviceContext *ctx, ID3D11Resource *res, UINT sub, D3D11_MAP type, UINT flags,
                                       D3D11_MAPPED_SUBRESOURCE *out)
{
    void *ret = __builtin_return_address(0);
    const DWORD *entry = (const DWORD *)t_callerEsi;
    if (!g_devPatched) patch_device(ctx);
    D3D11_RESOURCE_DIMENSION dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    if (res) ID3D11Resource_GetType(res, &dim);
    if (dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d;
        ID3D11Buffer_GetDesc((ID3D11Buffer *)res, &d);
        int k = !g_g ? -1 : ret == g_exe + g_g->d3dFlushVB ? 0 : ret == g_exe + g_g->d3dFlushIB ? 1 : -1;
        if (k >= 0 && type == D3D11_MAP_WRITE_DISCARD) {
            ULONG_PTR base = 0;
            const char *m = module_at(entry, &base);
            if (m && (BYTE *)base == g_exe && entry[-1] <= d.ByteWidth) {
                add(&g_wrote[k][bucket(entry[-1])], entry[-1]);
                add(&g_wroteBuf[k], d.ByteWidth);
                note_dyn(k, res, d.ByteWidth);
            } else {
                InterlockedIncrement(&g_wroteBad);
            }
        }
        if ((UINT)type < 6) add(&g_mapType[type], d.ByteWidth);
        if (type == D3D11_MAP_WRITE_DISCARD) {
            add(&g_mapDiscard[bucket(d.ByteWidth)], d.ByteWidth);
            kind(g_kMap, d.Usage, d.BindFlags, d.CPUAccessFlags, d.ByteWidth);
            site(g_sMap, ret, d.ByteWidth);
        }
    } else {
        add(&g_mapTex, 0);
    }
    return real_map(ctx, res, sub, type, flags, out);
}

static HRESULT STDMETHODCALLTYPE h_cbuf(ID3D11Device *dev, const D3D11_BUFFER_DESC *d, const D3D11_SUBRESOURCE_DATA *init,
                                        ID3D11Buffer **out)
{
    if (d) {
        add(&g_cbuf[bucket(d->ByteWidth)], d->ByteWidth);
        if (init) add(&g_cbufInit, d->ByteWidth);
        kind(g_kCre, d->Usage, d->BindFlags, d->CPUAccessFlags, d->ByteWidth);
        site(g_sCre, __builtin_return_address(0), d->ByteWidth);
    }
    return real_cbuf(dev, d, init, out);
}

static HRESULT STDMETHODCALLTYPE h_ctex(ID3D11Device *dev, const D3D11_TEXTURE2D_DESC *d, const D3D11_SUBRESOURCE_DATA *init,
                                        ID3D11Texture2D **out)
{
    add(&g_ctex, 0);
    if (init) add(&g_ctexInit, 0);
    return real_ctex(dev, d, init, out);
}

// The 2-byte no-op entry becomes a short jump to a jump (to the wrapper) in the 5 bytes before it;
// the original continues at fn + 2.
static int hotpatch(BYTE *fn, void *hook, void **real)
{
    if (!fn || fn[0] != 0x8B || fn[1] != 0xFF) return 0;
    for (int i = 1; i <= 5; i++)
        if (fn[-i] != 0xCC && fn[-i] != 0x90) return 0;
    DWORD old;
    if (!VirtualProtect(fn - 5, 7, PAGE_EXECUTE_READWRITE, &old)) return 0;
    *real = fn + 2;
    fn[-5] = 0xE9;
    *(LONG *)(fn - 4) = (LONG)((BYTE *)hook - fn);
    __atomic_store_n((volatile SHORT *)fn, (SHORT)0xF9EB, __ATOMIC_SEQ_CST);
    VirtualProtect(fn - 5, 7, old, &old);
    FlushInstructionCache(GetCurrentProcess(), fn - 5, 7);
    return 1;
}

#ifdef D3DSTATS_TEST
int d3dstats_test_patch(ID3D11DeviceContext *ctx)
{
    d3dstats_hmap = (void *)h_map;
    void **vt = *(void ***)ctx;
    g_ctxPatched = hotpatch((BYTE *)vt[SLOT_UPDATE], (void *)h_update, (void **)&real_update) &&
                   hotpatch((BYTE *)vt[SLOT_MAP], (void *)map_entry, (void **)&real_map);
    return g_ctxPatched;
}
#endif

const char *d3dstats_check(BYTE *exe, const GameExe *g)
{
    if (!g->d3dContext) return "no D3D11";
    const BYTE *use = exe + g->d3dFlushUse;
    if (use[0] != 0xA1 || *(BYTE *const *)(use + 1) != exe + g->d3dContext || use[5] != 0x8B || use[6] != 0x08) return "context use";
    if (exe[g->d3dFlushVB - 2] != 0xFF || exe[g->d3dFlushIB - 2] != 0xFF) return "flush calls";
    return NULL;
}

void d3dstats_init(const WCHAR *ini)
{
    if (!GetPrivateProfileIntW(L"probe", L"D3DStats", 0, ini)) return;
    BYTE *exe = (BYTE *)GetModuleHandleW(NULL);
    const GameExe *g = game_exe(exe);
    if (!g || d3dstats_check(exe, g)) {
        log_event("d3dstats: the game's context pointer isn't where expected (a D3D11 exe only); off");
        return;
    }
    g_g = g;
    g_exe = exe;
    d3dstats_hmap = (void *)h_map;
    log_event("d3dstats: counting the game's D3D11 uploads, maps and resource creation (diagnostic)");
}

// Sampler thread.
void d3dstats_tick(void)
{
    if (!g_exe || g_ctxPatched) return;
    void *ctx = *(void *volatile *)(g_exe + g_g->d3dContext);
    if (!ctx || ctx == g_rejected) return;
    void **vt = *(void **volatile *)ctx;
    if (!vt) return;
    ULONG_PTR b1 = 0, b2 = 0;
    const char *m1 = module_at(vt[SLOT_UPDATE], &b1), *m2 = module_at(vt[SLOT_MAP], &b2);
    if (!m1 || !m2 || _stricmp(m1, "d3d11.dll") || _stricmp(m2, "d3d11.dll") ||
        !hotpatch((BYTE *)vt[SLOT_MAP], (void *)map_entry, (void **)&real_map)) {
        g_rejected = ctx;
        log_event("d3dstats: the game's context %p isn't D3D11's own or not hot-patchable; off", ctx);
        return;
    }
    if (!hotpatch((BYTE *)vt[SLOT_UPDATE], (void *)h_update, (void **)&real_update)) {
        g_rejected = ctx;
        log_event("d3dstats: UpdateSubresource isn't hot-patchable; only Map counted");
    }
    g_ctxPatched = 1;
    log_event("d3dstats: hot-patched Map and UpdateSubresource (d3d11.dll+0x%lx, +0x%lx)", (unsigned long)((ULONG_PTR)vt[SLOT_MAP] - b2),
              (unsigned long)((ULONG_PTR)vt[SLOT_UPDATE] - b1));
}

static void line_buckets(const char *what, const Cnt *c)
{
    char buf[512];
    int len = 0;
    for (int i = 0; i < NB; i++)
        if (c[i].n) len += snprintf(buf + len, sizeof buf - len, "%s %s %ld (%.0f MB)", len ? "," : "", kBucket[i], c[i].n, c[i].bytes / 1048576.0);
    if (len) log_event("d3dstats: %s by size:%s", what, buf);
}

static void line_kinds(const char *what, const Kind *k)
{
    for (int i = 0; i < KINDS && k[i].key; i++) {
        LONG key = k[i].key;
        static const char *usage[] = {"DEFAULT", "IMMUTABLE", "DYNAMIC", "STAGING"};
        log_event("d3dstats: %s: usage %s, bind 0x%lx%s: %ld calls, %.0f MB", what, usage[(key >> 1) & 3],
                  (unsigned long)(key >> 4), (key >> 3) & 1 ? ", CPU access" : "", k[i].c.n, k[i].c.bytes / 1048576.0);
    }
}

static void line_sites(const char *what, const Site *s)
{
    for (int i = 0; i < SITES && s[i].ret; i++) {
        ULONG_PTR base = 0;
        const char *m = module_at(s[i].ret, &base);
        log_event("d3dstats: %s from %s+0x%lx: %ld calls, %.0f MB", what, m ? m : "?", (unsigned long)((ULONG_PTR)s[i].ret - base),
                  s[i].c.n, s[i].c.bytes / 1048576.0);
    }
}

void d3dstats_report(void)
{
    if (!g_ctxPatched) return;
    log_event("d3dstats: UpdateSubresource: %ld whole buffers, %ld buffers with a box (%.0f MB), %ld textures, %ld other; "
              "%ld distinct buffers updated%s",
              g_updBuf[0].n + g_updBuf[1].n + g_updBuf[2].n + g_updBuf[3].n + g_updBuf[4].n + g_updBuf[5].n + g_updBuf[6].n +
                  g_updBuf[7].n,
              g_updBufBox.n, g_updBufBox.bytes / 1048576.0, g_updTex.n, g_updOther.n, g_distinct, g_setFull ? " (set full)" : "");
    line_buckets("whole-buffer updates", g_updBuf);
    line_kinds("buffer updates", g_kUpd);
    line_sites("buffer updates", g_sUpd);
    log_event("d3dstats: Map on buffers: READ %ld, WRITE %ld, READ_WRITE %ld, WRITE_DISCARD %ld (%.0f MB), WRITE_NO_OVERWRITE %ld; "
              "on textures %ld",
              g_mapType[1].n, g_mapType[2].n, g_mapType[3].n, g_mapType[4].n, g_mapType[4].bytes / 1048576.0, g_mapType[5].n, g_mapTex.n);
    line_buckets("WRITE_DISCARD maps", g_mapDiscard);
    line_kinds("WRITE_DISCARD maps", g_kMap);
    line_sites("WRITE_DISCARD maps", g_sMap);
    static const char *flushName[2] = {"vertex-buffer flush (exe+0x4c1319)", "index-buffer flush (exe+0x4c13d8)"};
    for (int k = 0; k < 2; k++) {
        LONG n = 0;
        LONGLONG w = 0;
        for (int i = 0; i < NB; i++) { n += g_wrote[k][i].n; w += g_wrote[k][i].bytes; }
        if (!n) continue;
        log_event("d3dstats: %s: %ld discard maps wrote %.0f MB into buffers of %.0f MB in all (%.1f%%)", flushName[k], n,
                  w / 1048576.0, g_wroteBuf[k].bytes / 1048576.0, g_wroteBuf[k].bytes ? 100.0 * w / g_wroteBuf[k].bytes : 0);
        char what[64];
        snprintf(what, sizeof what, "%s, bytes written", k ? "index flush" : "vertex flush");
        line_buckets(what, g_wrote[k]);
        char buf[600];
        int len = 0;
        for (int i = 0; i < DYN && g_dyn[k][i].p && len < (int)sizeof buf - 40; i++)
            len += snprintf(buf + len, sizeof buf - len, "%s %.2f MB x%ld", len ? "," : "", g_dyn[k][i].width / 1048576.0, g_dyn[k][i].n);
        log_event("d3dstats: %s, buffers (size x maps):%s", k ? "index flush" : "vertex flush", buf);
    }
    if (g_wroteBad) log_event("d3dstats: %ld flush maps without a readable entry", g_wroteBad);
    LONG nc = 0;
    for (int i = 0; i < NB; i++) nc += g_cbuf[i].n;
    if (!real_cbuf || !real_ctex)
        log_event("d3dstats: %s not counted (not hot-patched)",
                  !real_cbuf && !real_ctex ? "CreateBuffer and CreateTexture2D" : !real_cbuf ? "CreateBuffer" : "CreateTexture2D");
    log_event("d3dstats: CreateBuffer %ld (%ld with initial data, %.0f MB), CreateTexture2D %ld (%ld with initial data)", nc,
              g_cbufInit.n, g_cbufInit.bytes / 1048576.0, g_ctex.n, g_ctexInit.n);
    line_buckets("buffers created", g_cbuf);
    line_kinds("buffers created", g_kCre);
    line_sites("buffers created", g_sCre);
}
