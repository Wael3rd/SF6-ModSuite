#include "report/capture.hpp"

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <objidl.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <thread>

#include "report/report_logic.hpp"
#include "report/pixels.hpp"

// No import library of D3D12, DXGI or WIC: only their objects' methods are called (the device comes from the game, WIC through COM), so the
// plugin loads nothing more at the game's start. ole32 only.
#pragma comment(lib, "ole32.lib")

namespace report::capture {
namespace {

using Microsoft::WRL::ComPtr;
namespace px = report::pixels;

constexpr DWORD kFenceWaitMs = 3000;
// CLSID_WICImagingFactory, GUID_ContainerFormatJpeg, GUID_WICPixelFormat24bppBGR: their definitions live in a library not linked here
constexpr CLSID kWicFactory = { 0xcacaf262, 0x9370, 0x4615, { 0xa1, 0x3b, 0x9f, 0x55, 0x39, 0xda, 0x4c, 0x0a } };
constexpr GUID kJpegContainer = { 0x19e4a5aa, 0x5662, 0x4fc5, { 0xa0, 0xc0, 0x17, 0x58, 0x02, 0x8e, 0x10, 0x57 } };
constexpr GUID kBgr24 = { 0x6fddc324, 0x4e03, 0x4bfe, { 0xb1, 0x85, 0x3d, 0x77, 0x76, 0x8d, 0xc9, 0x0c } };

std::atomic<bool> g_want{false};
std::atomic<uint64_t> g_gen{0};                 // a new request or a discard: what is on its way is no longer wanted
std::atomic<int> g_state{(int)State::None};
std::mutex g_mx;                                // g_jpeg, g_why
std::vector<uint8_t> g_jpeg;
std::string g_why;
void (*g_sink)(const std::string&) = nullptr;

void logf(const std::string& s) { if (g_sink) g_sink(s); }

// What the copy leaves for the thread: every object it must outlive
struct Copy {
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> readback;
    HANDLE ev = nullptr;
    int w = 0, h = 0;
    size_t pitch = 0, total = 0;                // the buffer's size: its last row is not padded to the pitch
    px::Format format = px::Format::Unknown;
    DXGI_FORMAT dxgi = DXGI_FORMAT_UNKNOWN;
    uint64_t gen = 0;
};

// The output's HDR10 mode tells a 10-bit swap chain's meaning; DXGI has no getter for the chain's own colour space
bool output_is_pq(IDXGISwapChain3* chain) {
    ComPtr<IDXGIOutput> out;
    if (FAILED(chain->GetContainingOutput(&out)) || !out) return false;
    ComPtr<IDXGIOutput6> out6;
    if (FAILED(out.As(&out6)) || !out6) return false;
    DXGI_OUTPUT_DESC1 d{};
    return SUCCEEDED(out6->GetDesc1(&d)) && d.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

px::Format format_of(DXGI_FORMAT f, IDXGISwapChain3* chain) {
    switch (f) {
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_TYPELESS: return px::Format::RGBA8;
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM: case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return px::Format::BGRA8;
        case DXGI_FORMAT_R10G10B10A2_UNORM: case DXGI_FORMAT_R10G10B10A2_TYPELESS: return output_is_pq(chain) ? px::Format::RGB10A2_PQ : px::Format::RGB10A2;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: case DXGI_FORMAT_R16G16B16A16_TYPELESS: return px::Format::RGBA16F;
        default: return px::Format::Unknown;
    }
}

// The render thread. "" = the copy is queued, *c holds it.
std::string copy_frame(ID3D12Device* dev, IDXGISwapChain* raw_chain, ID3D12CommandQueue* queue, Copy* c) {
    if (!dev || !raw_chain || !queue) return "no device, swap chain or queue from REFramework";
    ComPtr<IDXGISwapChain3> chain;
    if (FAILED(raw_chain->QueryInterface(IID_PPV_ARGS(&chain)))) return "the swap chain is not a DXGI 1.4 one";
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return "the queue given is not a direct one";
    ComPtr<ID3D12Resource> back;
    HRESULT hr = chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&back));
    if (FAILED(hr)) return "GetBuffer failed";
    const D3D12_RESOURCE_DESC desc = back->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 || desc.Width > 16384 || desc.Height > 16384 || !desc.Width || !desc.Height)
        return "the back buffer is not a plain 2D texture";
    c->dxgi = desc.Format;
    c->format = format_of(desc.Format, chain.Get());
    if (c->format == px::Format::Unknown) return "back buffer format " + std::to_string((int)desc.Format) + " not handled";

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT foot{};
    UINT rows = 0;
    UINT64 row_bytes = 0, total = 0;
    dev->GetCopyableFootprints(&desc, 0, 1, 0, &foot, &rows, &row_bytes, &total);
    if (!total) return "GetCopyableFootprints gave nothing";
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf{};
    buf.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf.Width = total;
    buf.Height = 1;
    buf.DepthOrArraySize = 1;
    buf.MipLevels = 1;
    buf.SampleDesc.Count = 1;
    buf.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(dev->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buf, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&c->readback)))) return "no readback buffer";
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&c->alloc)))) return "no command allocator";
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, c->alloc.Get(), nullptr, IID_PPV_ARGS(&c->list)))) return "no command list";
    if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&c->fence)))) return "no fence";
    c->ev = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!c->ev) return "no event";

    D3D12_RESOURCE_BARRIER bar{};
    bar.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    bar.Transition.pResource = back.Get();
    bar.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    c->list->ResourceBarrier(1, &bar);
    D3D12_TEXTURE_COPY_LOCATION dst{}, src{};
    dst.pResource = c->readback.Get();
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint = foot;
    src.pResource = back.Get();
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    c->list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    bar.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    bar.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
    c->list->ResourceBarrier(1, &bar);
    if (FAILED(c->list->Close())) return "the command list did not close";
    ID3D12CommandList* lists[] = { c->list.Get() };
    queue->ExecuteCommandLists(1, lists);
    if (FAILED(queue->Signal(c->fence.Get(), 1))) return "Signal failed";
    c->w = (int)desc.Width;
    c->h = (int)desc.Height;
    c->pitch = foot.Footprint.RowPitch;
    c->total = (size_t)total;
    return "";
}

// Under SEH: the pointers come from REFramework at its start, and a device may be lost since. No C++ object here: copy_frame's unwind.
void copy_call(void* dev, void* chain, void* queue, Copy* c, std::string* why) {
    *why = copy_frame(static_cast<ID3D12Device*>(dev), static_cast<IDXGISwapChain*>(chain), static_cast<ID3D12CommandQueue*>(queue), c);
}
bool copy_guarded(void* dev, void* chain, void* queue, Copy* c, std::string* why) {
    __try {
        copy_call(dev, chain, queue, c, why);
        return why->empty();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void publish_failure(uint64_t gen, const std::string& why) {
    {
        std::lock_guard lk(g_mx);
        if (gen != g_gen.load()) return;
        g_why = why;
        g_jpeg.clear();
        g_state.store((int)State::Failed);
    }
    logf("bug report: no picture: " + why);
}

std::string encode_step(const uint8_t* bgr, int w, int h, size_t pitch, float quality, std::vector<uint8_t>* out) {
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(kWicFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return "no WIC";
    ComPtr<IStream> stream;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream))) return "no stream";
    ComPtr<IWICBitmapEncoder> enc;
    if (FAILED(f->CreateEncoder(kJpegContainer, nullptr, &enc))) return "no JPEG encoder";
    if (FAILED(enc->Initialize(stream.Get(), WICBitmapEncoderNoCache))) return "the encoder did not start";
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    if (FAILED(enc->CreateNewFrame(&frame, &props))) return "no frame";
    PROPBAG2 name{};
    wchar_t quality_name[] = L"ImageQuality";
    name.pstrName = quality_name;
    VARIANT v{};
    v.vt = VT_R4;
    v.fltVal = quality;
    if (props) props->Write(1, &name, &v);
    if (FAILED(frame->Initialize(props.Get()))) return "the frame did not start";
    if (FAILED(frame->SetSize((UINT)w, (UINT)h))) return "SetSize failed";
    WICPixelFormatGUID fmt = kBgr24;
    if (FAILED(frame->SetPixelFormat(&fmt)) || fmt != kBgr24) return "the encoder wants another pixel format";
    if (FAILED(frame->WritePixels((UINT)h, (UINT)pitch, (UINT)(pitch * (size_t)h), const_cast<BYTE*>(bgr)))) return "WritePixels failed";
    if (FAILED(frame->Commit()) || FAILED(enc->Commit())) return "Commit failed";
    HGLOBAL mem = nullptr;
    if (FAILED(GetHGlobalFromStream(stream.Get(), &mem)) || !mem) return "no memory from the stream";
    STATSTG st{};
    if (FAILED(stream->Stat(&st, STATFLAG_NONAME))) return "Stat failed";
    const void* p = GlobalLock(mem);
    if (!p) return "GlobalLock failed";
    out->assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + (size_t)st.cbSize.QuadPart);
    GlobalUnlock(mem);
    return "";
}

void finish_on_thread(Copy c) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    std::string why;
    std::vector<uint8_t> bgr, jpeg;
    int dw = 0, dh = 0;
    size_t dpitch = 0;
    bool com = false;
    try {
        if (FAILED(c.fence->SetEventOnCompletion(1, c.ev))) why = "SetEventOnCompletion failed";
        else if (WaitForSingleObject(c.ev, kFenceWaitMs) != WAIT_OBJECT_0) {
            // the GPU may still write to it: kept alive for good rather than freed under it
            c.readback->AddRef(); c.alloc->AddRef(); c.list->AddRef(); c.fence->AddRef();
            c.ev = nullptr;
            why = "the GPU copy did not end within 3 s";
        } else {
            px::scaled_size(c.w, c.h, &dw, &dh);
            dpitch = (((size_t)dw * 3) + 3) & ~(size_t)3;
            bgr.resize(dpitch * (size_t)dh);
            void* data = nullptr;
            D3D12_RANGE range{ 0, c.total };
            if (FAILED(c.readback->Map(0, &range, &data)) || !data) why = "the readback buffer did not map";
            else {
                if (!px::to_bgr(static_cast<const uint8_t*>(data), c.pitch, c.w, c.h, c.format, dw, dh, bgr.data(), dpitch)) why = "the conversion refused the format";
                D3D12_RANGE none{ 0, 0 };
                c.readback->Unmap(0, &none);
            }
        }
        if (c.ev) CloseHandle(c.ev);
        c.ev = nullptr;
        c.readback.Reset(); c.list.Reset(); c.alloc.Reset(); c.fence.Reset();     // the 8 to 33 MB go back now
        if (why.empty() && c.gen == g_gen.load()) {
            const HRESULT ci = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            com = SUCCEEDED(ci);
            if (!encode_jpeg(bgr.data(), dw, dh, dpitch, kJpegMaxBytes, &jpeg, &why) && why.empty()) why = "the JPEG stayed over its size";
        }
    } catch (...) {
        why = "the conversion faulted";
    }
    if (com) CoUninitialize();
    if (!why.empty()) { publish_failure(c.gen, why); return; }
    std::lock_guard lk(g_mx);
    if (c.gen != g_gen.load()) return;                  // discarded meanwhile: the bytes die here
    g_jpeg = std::move(jpeg);
    g_why.clear();
    g_state.store((int)State::Ready);
    char b[160];
    std::snprintf(b, sizeof b, "bug report: picture ready: back buffer %d x %d, format %d (%s), %d x %d JPEG, %zu bytes", c.w, c.h, (int)c.dxgi,
                  c.format == px::Format::RGB10A2_PQ ? "HDR10, tone mapped" : c.format == px::Format::RGBA16F ? "scRGB, tone mapped" : "SDR", dw, dh, g_jpeg.size());
    logf(b);
}

}  // namespace

void set_log(void (*sink)(const std::string&)) { g_sink = sink; }

void request() {
    std::lock_guard lk(g_mx);
    g_gen.fetch_add(1);
    g_jpeg.clear();
    g_why.clear();
    g_state.store((int)State::Waiting);
    g_want.store(true, std::memory_order_release);
}

void present(void* device, void* swap_chain, void* queue) {
    if (!g_want.load(std::memory_order_acquire)) return;
    if (!g_want.exchange(false)) return;
    const uint64_t gen = g_gen.load();
    auto c = std::make_unique<Copy>();
    c->gen = gen;
    std::string why;
    if (!copy_guarded(device, swap_chain, queue, c.get(), &why)) {
        if (c->ev) CloseHandle(c->ev);
        c->ev = nullptr;
        publish_failure(gen, why.empty() ? "the copy of the frame faulted" : why);
        return;
    }
    {
        std::lock_guard lk(g_mx);
        if (gen == g_gen.load()) g_state.store((int)State::Encoding);
    }
    try {
        std::thread([p = c.release()] { std::unique_ptr<Copy> own(p); finish_on_thread(std::move(*own)); }).detach();
    } catch (...) {
        publish_failure(gen, "no thread for the picture");
    }
}

void discard() {
    std::lock_guard lk(g_mx);
    g_gen.fetch_add(1);
    g_want.store(false);
    std::vector<uint8_t>().swap(g_jpeg);
    g_why.clear();
    g_state.store((int)State::None);
}

State state() { return (State)g_state.load(); }

std::vector<uint8_t> take() {
    std::lock_guard lk(g_mx);
    std::vector<uint8_t> out;
    if ((State)g_state.load() == State::Ready) out.swap(g_jpeg);
    return out;
}

std::string why() {
    std::lock_guard lk(g_mx);
    return g_why;
}

bool encode_jpeg(const uint8_t* bgr, int w, int h, size_t pitch, size_t max_bytes, std::vector<uint8_t>* out, std::string* why) {
    static const float kQualities[] = { 0.82f, 0.70f, 0.55f, 0.40f };
    out->clear();
    for (const float q : kQualities) {
        std::vector<uint8_t> j;
        const std::string e = encode_step(bgr, w, h, pitch, q, &j);
        if (!e.empty()) { if (why) *why = e; return false; }
        if (j.size() <= max_bytes) { out->swap(j); return true; }
    }
    if (w < 64 || h < 64) return false;                 // a picture this busy at this size: half its size, once more
    const int hw = w / 2, hh = h / 2;
    const size_t hp = (((size_t)hw * 3) + 3) & ~(size_t)3;
    std::vector<uint8_t> half(hp * (size_t)hh);
    for (int y = 0; y < hh; ++y) {
        const uint8_t* s0 = bgr + (size_t)y * 2 * pitch;
        const uint8_t* s1 = s0 + pitch;
        uint8_t* d = half.data() + (size_t)y * hp;
        for (int x = 0; x < hw; ++x, s0 += 6, s1 += 6)
            for (int k = 0; k < 3; ++k) *d++ = (uint8_t)((s0[k] + s0[k + 3] + s1[k] + s1[k + 3] + 2) / 4);
    }
    return encode_jpeg(half.data(), hw, hh, hp, max_bytes, out, why);
}

}  // namespace report::capture
