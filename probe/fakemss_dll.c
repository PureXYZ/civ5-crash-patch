// Stand-in for Miles (mss32.dll) for the audio guard test. A "sample" is
// { int magic; void *driver; int index; }; the real AIL_sample_status reads
// driver->table[index] like Miles does, so a released sample (driver NULL) faults.
#include <windows.h>
typedef struct { int magic; int *driver; int index; } Sample;
static int g_calls;
static int g_tbl[4] = {0, 1, 0, 0};
static Sample g_smp = {1, g_tbl, 1};
__declspec(dllexport) int __stdcall AIL_sample_status(Sample *s) { g_calls++; return s->driver[s->index] ? 4 : 8; }
__declspec(dllexport) void __stdcall AIL_set_sample_3D_position(Sample *s, float x, float y, float z) { g_calls++; s->driver[s->index] = (int)(x + y + z); }
__declspec(dllexport) Sample *__stdcall AIL_allocate_sample_handle(void *drv) { g_calls++; if (!drv) return NULL; g_smp.driver = g_tbl; return &g_smp; }
__declspec(dllexport) void __stdcall AIL_release_sample_handle(Sample *s) { g_calls++; s->driver = NULL; }
__declspec(dllexport) void __stdcall AIL_sample_volume_pan(Sample *s, float *vol, float *pan) { g_calls++; *vol = 0.5f; *pan = (float)s->driver[s->index]; }
__declspec(dllexport) int __stdcall fake_calls(void) { return g_calls; }
__declspec(dllexport) int __stdcall fake_slot(void) { return g_tbl[1]; }
// Sound log test: a sample "loads" unless its size is 0; a stream opens unless its name is empty.
static char g_stream;
static int g_started;
__declspec(dllexport) int __stdcall AIL_set_named_sample_file(Sample *s, const char *name, const void *image, unsigned size, int block) { g_calls++; return size != 0; }
__declspec(dllexport) void __stdcall AIL_start_sample(Sample *s) { g_calls++; g_started++; }
__declspec(dllexport) void *__stdcall AIL_open_stream(void *drv, const char *name, int mem) { g_calls++; return *name ? &g_stream : NULL; }
__declspec(dllexport) void __stdcall AIL_start_stream(void *stream) { g_calls++; g_started++; }
__declspec(dllexport) const char *__stdcall AIL_last_error(void) { return "fake error"; }
__declspec(dllexport) int __stdcall fake_started(void) { return g_started; }
