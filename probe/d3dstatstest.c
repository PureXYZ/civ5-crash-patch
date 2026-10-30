// d3dstats test: hot-patches the real d3d11.dll's Map and UpdateSubresource (and, at the first
// update, CreateBuffer / CreateTexture2D), makes known calls, and prints the report. Built with
// d3dstats.c -DD3DSTATS_TEST; needs a D3D11 GPU.
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <stdarg.h>
#include <stdio.h>

int d3dstats_test_patch(ID3D11DeviceContext *ctx);
void d3dstats_report(void);
void log_event(const char *fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    vprintf(fmt, a);
    va_end(a);
    printf("\n");
}
const char *module_at(const void *addr, ULONG_PTR *base) { (void)addr; (void)base; return NULL; }

int main(void)
{
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    if (FAILED(D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx))) return 1;
    if (!d3dstats_test_patch(ctx)) { printf("not hot-patchable\n"); return 1; }
    static BYTE src[4 << 20];  // covers the 3 MB buffer's initial data
    D3D11_BUFFER_DESC dd = {256 << 10, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0};
    ID3D11Buffer *b;
    ID3D11Device_CreateBuffer(dev, &dd, NULL, &b);  // before the device is patched: not counted
    for (int i = 0; i < 10; i++) ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)b, 0, NULL, src, 0, 0);
    D3D11_BOX box = {0, 0, 0, 4096, 1, 1};
    ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)b, 0, &box, src, 0, 0);
    D3D11_BUFFER_DESC dyn = {3 << 20, D3D11_USAGE_DYNAMIC, D3D11_BIND_VERTEX_BUFFER, D3D11_CPU_ACCESS_WRITE, 0, 0};
    ID3D11Buffer *d;
    D3D11_SUBRESOURCE_DATA init = {src, 0, 0};
    ID3D11Device_CreateBuffer(dev, &dyn, &init, &d);  // counted: 3 MB, initial data
    for (int i = 0; i < 5; i++) {
        D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)d, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
            memset(m.pData, i, 1024);
            ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)d, 0);
        }
    }
    printf("expected: 10 whole 256 KB updates + 1 with a box, 1 distinct buffer; 5 WRITE_DISCARD maps of 3 MB; 1 buffer created (3 MB, initial data)\n");
    d3dstats_report();
    return 0;
}
