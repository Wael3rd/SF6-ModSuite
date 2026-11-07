// report_capture_test - report::capture on a real D3D12 swap chain of its own: no game, no REFramework, nothing on disk but the JPEGs asked for.
//
//   report_capture_test [--hw] [--out <folder>]
//
// A hidden window, a device (WARP, Windows' software adapter, by default, so it never touches the GPU of a game running beside it; --hw the
// real one), a flip-model swap chain and its own queue, as the game has them. Each case clears the back buffer to four known quadrants, asks
// a picture, calls present() where REFramework would call it, presents, and reads the JPEG back through WIC.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "report/capture.hpp"
#include "report/report_logic.hpp"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "ole32.lib")

using Microsoft::WRL::ComPtr;
namespace cap = report::capture;

static int g_checks = 0, g_failed = 0;
static void check(bool ok, const std::string& what) {
    ++g_checks;
    if (!ok) { ++g_failed; printf("FAILED: %s\n", what.c_str()); }
}

struct Env {
    ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> dev;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12Fence> fence;
    HANDLE ev = nullptr;
    UINT64 fence_at = 0;
    HWND wnd = nullptr;
    void wait() {
        queue->Signal(fence.Get(), ++fence_at);
        fence->SetEventOnCompletion(fence_at, ev);
        WaitForSingleObject(ev, 10000);
    }
};
static Env E;
static std::string g_out;

static bool init(bool hw) {
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&E.factory)))) return false;
    if (hw) {
        if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&E.dev)))) return false;
    } else {
        ComPtr<IDXGIAdapter> warp;
        if (FAILED(E.factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)))) return false;
        if (FAILED(D3D12CreateDevice(warp.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&E.dev)))) return false;
    }
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(E.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&E.queue)))) return false;
    if (FAILED(E.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&E.fence)))) return false;
    E.ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"report_capture_test";
    RegisterClassW(&wc);
    E.wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);   // never shown
    return E.wnd != nullptr;
}

struct Px { int b, g, r; };
struct Decoded { UINT w = 0, h = 0; std::vector<uint8_t> bgr; };

static bool decode(const std::vector<uint8_t>& jpeg, Decoded* d) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return false;
    ComPtr<IWICStream> s;
    if (FAILED(f->CreateStream(&s)) || FAILED(s->InitializeFromMemory(const_cast<BYTE*>(jpeg.data()), (DWORD)jpeg.size()))) return false;
    ComPtr<IWICBitmapDecoder> dec;
    if (FAILED(f->CreateDecoderFromStream(s.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec))) return false;
    GUID fmt{};
    if (FAILED(dec->GetContainerFormat(&fmt)) || fmt != GUID_ContainerFormatJpeg) return false;
    ComPtr<IWICBitmapFrameDecode> fr;
    if (FAILED(dec->GetFrame(0, &fr))) return false;
    fr->GetSize(&d->w, &d->h);
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(f->CreateFormatConverter(&conv)) || FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat24bppBGR, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return false;
    d->bgr.resize((size_t)d->w * d->h * 3);
    return SUCCEEDED(conv->CopyPixels(nullptr, d->w * 3, (UINT)d->bgr.size(), d->bgr.data()));
}
static Px at(const Decoded& d, UINT x, UINT y) {
    const uint8_t* p = d.bgr.data() + (size_t)y * d.w * 3 + (size_t)x * 3;
    return { p[0], p[1], p[2] };
}
static bool near_px(Px a, Px b, int tol) { return std::abs(a.b - b.b) <= tol && std::abs(a.g - b.g) <= tol && std::abs(a.r - b.r) <= tol; }

// Four quadrants: red, green / blue, a grey; `one` is what 1.0 means for this format
static void draw_and_present_with_capture(IDXGISwapChain3* chain, UINT w, UINT h, bool capture, double* ms_copy) {
    ComPtr<ID3D12Resource> back;
    chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> heap;
    E.dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = heap->GetCPUDescriptorHandleForHeapStart();
    E.dev->CreateRenderTargetView(back.Get(), nullptr, rtv);
    ComPtr<ID3D12CommandAllocator> alloc;
    E.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ComPtr<ID3D12GraphicsCommandList> list;
    E.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list));
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = back.Get();
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
    list->ResourceBarrier(1, &b);
    const float colours[4][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 0.5f, 0.5f, 0.5f, 1 } };
    const LONG mw = (LONG)w / 2, mh = (LONG)h / 2;
    const D3D12_RECT rects[4] = { { 0, 0, mw, mh }, { mw, 0, (LONG)w, mh }, { 0, mh, mw, (LONG)h }, { mw, mh, (LONG)w, (LONG)h } };
    for (int i = 0; i < 4; ++i) list->ClearRenderTargetView(rtv, colours[i], 1, &rects[i]);
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    list->ResourceBarrier(1, &b);
    list->Close();
    ID3D12CommandList* lists[] = { list.Get() };
    E.queue->ExecuteCommandLists(1, lists);
    if (capture) {
        const auto t0 = std::chrono::steady_clock::now();
        cap::present(E.dev.Get(), chain, E.queue.Get());     // where REFramework's on_present runs
        if (ms_copy) *ms_copy = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    }
    chain->Present(0, 0);
    E.wait();
}

static bool wait_ready(double* ms) {
    const auto t0 = std::chrono::steady_clock::now();
    for (;;) {
        const cap::State s = cap::state();
        if (s == cap::State::Ready || s == cap::State::Failed) break;
        if (std::chrono::steady_clock::now() - t0 > std::chrono::seconds(20)) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (ms) *ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return cap::state() == cap::State::Ready;
}

static void one_format(DXGI_FORMAT fmt, const char* name, UINT w, UINT h, UINT want_w, UINT want_h, Px red, Px green, Px blue, Px grey, int tol) {
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = w; sd.Height = h;
    sd.Format = fmt;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SampleDesc.Count = 1;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> c1;
    const std::string tag = std::string(name) + " " + std::to_string(w) + "x" + std::to_string(h);
    if (FAILED(E.factory->CreateSwapChainForHwnd(E.queue.Get(), E.wnd, &sd, nullptr, nullptr, &c1))) { check(false, tag + ": no swap chain"); return; }
    ComPtr<IDXGISwapChain3> chain;
    c1.As(&chain);
    draw_and_present_with_capture(chain.Get(), w, h, false, nullptr);   // a frame before: not the one asked
    check(cap::state() == cap::State::None || cap::state() == cap::State::Ready || cap::state() == cap::State::Failed, tag + ": no picture without a request");
    cap::request();
    check(cap::state() == cap::State::Waiting && !cap::settled(cap::state()), tag + ": the request waits for the Present");
    double ms_copy = 0, ms_all = 0;
    draw_and_present_with_capture(chain.Get(), w, h, true, &ms_copy);
    check(cap::settled(cap::state()), tag + ": settled at the Present");
    const bool ok = wait_ready(&ms_all);
    check(ok, tag + ": ready (" + cap::why() + ")");
    if (ok) {
        const std::vector<uint8_t> jpeg = cap::take();
        check(!jpeg.empty() && jpeg.size() <= report::kJpegMaxBytes, tag + ": a JPEG within its cap");
        check(cap::take().empty(), tag + ": moved out: nothing stays");
        Decoded d;
        check(decode(jpeg, &d), tag + ": WIC reads it as a JPEG");
        check(d.w == want_w && d.h == want_h, tag + ": size " + std::to_string(d.w) + "x" + std::to_string(d.h));
        if (d.w && d.h) {
            const UINT x1 = d.w / 4, x2 = d.w * 3 / 4, y1 = d.h / 4, y2 = d.h * 3 / 4;
            const Px q[4] = { at(d, x1, y1), at(d, x2, y1), at(d, x1, y2), at(d, x2, y2) };
            char b[200];
            std::snprintf(b, sizeof b, "%s: quadrants R(%d,%d,%d) G(%d,%d,%d) B(%d,%d,%d) grey(%d,%d,%d)", tag.c_str(), q[0].r, q[0].g, q[0].b, q[1].r, q[1].g, q[1].b, q[2].r, q[2].g, q[2].b, q[3].r, q[3].g, q[3].b);
            check(near_px(q[0], red, tol) && near_px(q[1], green, tol) && near_px(q[2], blue, tol) && near_px(q[3], grey, tol), b);
        }
        printf("  %-32s present() %.2f ms, then %.0f ms to the JPEG, %zu bytes\n", tag.c_str(), ms_copy, ms_all, jpeg.size());
        if (!g_out.empty()) {
            std::ofstream f(g_out + "\\capture_" + name + "_" + std::to_string(w) + ".jpg", std::ios::binary);
            f.write(reinterpret_cast<const char*>(jpeg.data()), (std::streamsize)jpeg.size());
        }
    }
    cap::discard();
    E.wait();
}

int main(int argc, char** argv) {
    bool hw = false;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--hw")) hw = true;
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) g_out = argv[++i];
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (!init(hw)) { printf("no D3D12 device here: nothing tested\n"); return 2; }
    DXGI_ADAPTER_DESC ad{};
    ComPtr<IDXGIAdapter> ada;
    { ComPtr<IDXGIFactory4> f; LUID l = E.dev->GetAdapterLuid(); if (SUCCEEDED(E.factory->EnumAdapterByLuid(l, IID_PPV_ARGS(&ada)))) ada->GetDesc(&ad); }
#ifdef SALT
    printf("salt %d\n", (int)SALT);                     // (a new binary each try: Smart App Control judges each file)
#endif
    printf("adapter: %ls\n", ad.Description);

    // nothing asked: present() touches nothing, whatever it is given
    cap::present(nullptr, nullptr, nullptr);
    check(cap::state() == cap::State::None, "no request: present() does nothing");

    const Px red{ 0, 0, 255 }, green{ 0, 255, 0 }, blue{ 255, 0, 0 }, grey{ 128, 128, 128 };
    one_format(DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8", 1280, 720, 1280, 720, red, green, blue, grey, 14);
    one_format(DXGI_FORMAT_B8G8R8A8_UNORM, "BGRA8", 1920, 1080, 1920, 1080, red, green, blue, grey, 14);
    one_format(DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8", 3840, 2160, 1920, 1080, red, green, blue, grey, 14);
    one_format(DXGI_FORMAT_R8G8B8A8_UNORM, "RGBA8", 3440, 1440, 1920, 804, red, green, blue, grey, 14);
    one_format(DXGI_FORMAT_R10G10B10A2_UNORM, "RGB10A2", 1280, 720, 1280, 720, red, green, blue, grey, 14);
    // scRGB: linear 1.0 through the shoulder is 0.53, sRGB 192; 0.5 is 0.34, sRGB 158: the channels' order and their level
    const Px s_red{ 0, 0, 192 }, s_green{ 0, 192, 0 }, s_blue{ 192, 0, 0 }, s_grey{ 158, 158, 158 };
    one_format(DXGI_FORMAT_R16G16B16A16_FLOAT, "RGBA16F", 1280, 720, 1280, 720, s_red, s_green, s_blue, s_grey, 20);

    // a discard while the picture is being made: nothing arrives after it
    {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 1920; sd.Height = 1080; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = 2; sd.SampleDesc.Count = 1; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> c1;
        if (SUCCEEDED(E.factory->CreateSwapChainForHwnd(E.queue.Get(), E.wnd, &sd, nullptr, nullptr, &c1))) {
            ComPtr<IDXGISwapChain3> chain;
            c1.As(&chain);
            cap::request();
            draw_and_present_with_capture(chain.Get(), 1920, 1080, true, nullptr);
            cap::discard();
            std::this_thread::sleep_for(std::chrono::seconds(2));
            check(cap::state() == cap::State::None && cap::take().empty(), "a picture on its way when the menu goes: dropped");
            // a request forgotten before its Present: no copy at all
            cap::request();
            cap::discard();
            draw_and_present_with_capture(chain.Get(), 1920, 1080, true, nullptr);
            check(cap::state() == cap::State::None, "a request discarded: the Present copies nothing");
            E.wait();
        }
    }

    // failures leave a reason, and no crash
    cap::request();
    cap::present(nullptr, nullptr, nullptr);
    check(cap::state() == cap::State::Failed && !cap::why().empty(), "no renderer data: failed, with its reason: " + cap::why());
    void* garbage[8] = {};                               // a swap chain that is no object: an access violation under SEH
    cap::request();
    cap::present(E.dev.Get(), garbage, E.queue.Get());
    check(cap::state() == cap::State::Failed && !cap::why().empty(), "a dead pointer: caught: " + cap::why());
    cap::discard();

    // the worst picture: noise at 1080p stays under its cap (quality, then half the size)
    {
        std::vector<uint8_t> noise(1920 * 3 * 1080);
        uint32_t x = 2463534242u;
        for (auto& b : noise) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; b = (uint8_t)x; }
        std::vector<uint8_t> j;
        std::string why;
        const bool ok = cap::encode_jpeg(noise.data(), 1920, 1080, 1920 * 3, report::kJpegMaxBytes, &j, &why);
        check(ok && !j.empty() && j.size() <= report::kJpegMaxBytes, "noise at 1080p: under the cap (" + std::to_string(j.size()) + " bytes)");
    }

    printf("report_capture_test: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
