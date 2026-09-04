#include "D3D12Renderer.h"
#include "Log.h"
#include "SuperRes.h"
#include <d3dcompiler.h>
#include <d3d12sdklayers.h>
#include <vector>
#include <algorithm>
#include <cstring>
#include <cmath>

using Microsoft::WRL::ComPtr;

static bool HR(HRESULT hr, const char* what) {
    if (FAILED(hr)) { LOG(what << " failed hr=0x" << std::hex << hr); return false; }
    return true;
}
static D3D12_HEAP_PROPERTIES HeapProps(D3D12_HEAP_TYPE type) {
    D3D12_HEAP_PROPERTIES p{}; p.Type=type; p.CreationNodeMask=1; p.VisibleNodeMask=1; return p;
}
static D3D12_RESOURCE_DESC Tex2D(DXGI_FORMAT fmt,uint32_t w,uint32_t h,D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{}; d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h;
    d.DepthOrArraySize=1; d.MipLevels=1; d.Format=fmt; d.SampleDesc={1,0}; d.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN; d.Flags=flags; return d;
}
// SMRU_MASK_PROBE=<format>[:<channels>] picks the resource format the
// ControlMask is created in and what each of its channels carries, so the
// runtime's expectations can be probed without a rebuild. Formats: r8 rg8
// rgba8 r16 rg16 rgba16 r16f rg16f rgba16f r32f rg32f rgba32f. Channels: up to
// four of 0 (constant 0), 1 (constant 1), u (the mask value), i (its inverse);
// default u111. Diagnostic only; the shipped layout is r8:u.
static void ReadMaskProbe(DXGI_FORMAT& fmt, float ch[4]) {
    wchar_t buf[64]{};
    if (GetEnvironmentVariableW(L"SMRU_MASK_PROBE", buf, 64) == 0) return;
    std::wstring v(buf);
    std::wstring f = v, c;
    if (const auto p = v.find(L':'); p != std::wstring::npos) { f = v.substr(0, p); c = v.substr(p + 1); }
    struct { const wchar_t* name; DXGI_FORMAT fmt; } table[] = {
        {L"r8", DXGI_FORMAT_R8_UNORM}, {L"rg8", DXGI_FORMAT_R8G8_UNORM}, {L"rgba8", DXGI_FORMAT_R8G8B8A8_UNORM},
        {L"r16", DXGI_FORMAT_R16_UNORM}, {L"rg16", DXGI_FORMAT_R16G16_UNORM}, {L"rgba16", DXGI_FORMAT_R16G16B16A16_UNORM},
        {L"r16f", DXGI_FORMAT_R16_FLOAT}, {L"rg16f", DXGI_FORMAT_R16G16_FLOAT}, {L"rgba16f", DXGI_FORMAT_R16G16B16A16_FLOAT},
        {L"r32f", DXGI_FORMAT_R32_FLOAT}, {L"rg32f", DXGI_FORMAT_R32G32_FLOAT}, {L"rgba32f", DXGI_FORMAT_R32G32B32A32_FLOAT},
    };
    for (const auto& t : table) if (f == t.name) fmt = t.fmt;
    for (size_t k = 0; k < 4 && k < c.size(); ++k)
        ch[k] = c[k] == L'0' ? 0.0f : c[k] == L'1' ? 1.0f : c[k] == L'i' ? 3.0f : 2.0f;
}
static const char* FormatName(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_R8_UNORM: return "R8_UNORM"; case DXGI_FORMAT_R8G8_UNORM: return "R8G8_UNORM";
        case DXGI_FORMAT_R8G8B8A8_UNORM: return "R8G8B8A8_UNORM"; case DXGI_FORMAT_R16_UNORM: return "R16_UNORM";
        case DXGI_FORMAT_R16G16_UNORM: return "R16G16_UNORM"; case DXGI_FORMAT_R16G16B16A16_UNORM: return "R16G16B16A16_UNORM";
        case DXGI_FORMAT_R16_FLOAT: return "R16_FLOAT"; case DXGI_FORMAT_R16G16_FLOAT: return "R16G16_FLOAT";
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return "R16G16B16A16_FLOAT"; case DXGI_FORMAT_R32_FLOAT: return "R32_FLOAT";
        case DXGI_FORMAT_R32G32_FLOAT: return "R32G32_FLOAT"; case DXGI_FORMAT_R32G32B32A32_FLOAT: return "R32G32B32A32_FLOAT";
        default: return "?";
    }
}
static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{}; x.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION; x.Transition.pResource=r;
    x.Transition.StateBefore=a; x.Transition.StateAfter=b; x.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES; return x;
}

D3D12Renderer::~D3D12Renderer() {
    WaitGPU();
    for (uint32_t i=0;i<FrameCount;++i) {
        if (m_upload[i] && m_uploadMapped[i]) m_upload[i]->Unmap(0,nullptr);
        if (m_guideUpload[i] && m_guideMapped[i]) m_guideUpload[i]->Unmap(0,nullptr);
        if (m_extDepthUpload[i] && m_extDepthMapped[i]) m_extDepthUpload[i]->Unmap(0,nullptr);
        if (m_extFlowUpload[i] && m_extFlowMapped[i]) m_extFlowUpload[i]->Unmap(0,nullptr);
        m_uploadMapped[i]=nullptr;
        m_guideMapped[i]=nullptr;
        m_extDepthMapped[i]=nullptr;
        m_extFlowMapped[i]=nullptr;
    }
    m_nr.reset();
    m_nrLoaded = false;
    if (m_fenceEvent) CloseHandle(m_fenceEvent);
}

bool D3D12Renderer::Initialize(HWND hwnd,uint32_t sourceW,uint32_t sourceH,uint32_t outputW,uint32_t outputH,uint32_t gridW,uint32_t gridH) {
    m_hwnd=hwnd; m_sourceW=sourceW; m_sourceH=sourceH; m_outputW=outputW; m_outputH=outputH; m_gridW=gridW; m_gridH=gridH;
    if(!m_gridW||!m_gridH)return false;
    ReadMaskProbe(m_maskFormat,m_maskChannels);
    if(!CreateDeviceAndSwapchain(hwnd) || !CreateHeapsAndBackbuffers() || !CreatePipelines()) return false;
    if(!InitializeDLSS() && !m_srActive) {
        LOG("DLSS unavailable; using D3D12 scaler fallback.");
        m_renderW=std::max(1u,outputW*2u/3u); m_renderH=std::max(1u,outputH*2u/3u);
    }
    if(!CreateVideoResources()) return false;
    if(!CreateOverlayLabels()) LOG("Overlay labels unavailable; A/B labels will not draw.");
    LOG("Guide contract: compact CPU optical-flow grid expanded on GPU into full R16G16_FLOAT MVs + R8 bias; depth is written directly into the same R32_TYPELESS/D32_FLOAT resource passed to NGX; temporal reset only on discontinuities.");
    return true;
}

bool D3D12Renderer::CreateDeviceAndSwapchain(HWND hwnd) {
    UINT ff=0;
    // SMRU_D3D_DEBUG=1 turns the D3D12 debug layer on in ANY build (a Debug
    // configuration cannot link: the NGX static lib is built against the release
    // CRT). Whatever the layer flags is logged at every GPU wait.
    const bool wantDebug=GetEnvironmentVariableW(L"SMRU_D3D_DEBUG",nullptr,0)>0;
    if(wantDebug){ ComPtr<ID3D12Debug> dbg; if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) { dbg->EnableDebugLayer(); ff|=DXGI_CREATE_FACTORY_DEBUG; } }
    if(!HR(CreateDXGIFactory2(ff,IID_PPV_ARGS(&m_factory)),"CreateDXGIFactory2")) return false;
    ComPtr<IDXGIAdapter1> fallback;
    for(UINT i=0;;++i){
        ComPtr<IDXGIAdapter1>a; if(m_factory->EnumAdapterByGpuPreference(i,DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,IID_PPV_ARGS(&a))==DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); if(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE) continue;
        if(FAILED(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_12_0,_uuidof(ID3D12Device),nullptr))) continue;
        if(!fallback) fallback=a; if(d.VendorId==0x10DE){m_adapter=a;break;}
    }
    if(!m_adapter)m_adapter=fallback; if(!m_adapter){LOG("No D3D12 hardware adapter.");return false;}
    DXGI_ADAPTER_DESC1 ad{};m_adapter->GetDesc1(&ad);LOG("D3D12 adapter vendor=0x"<<std::hex<<ad.VendorId<<" device=0x"<<ad.DeviceId);
    if(!HR(D3D12CreateDevice(m_adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&m_device)),"D3D12CreateDevice"))return false;
    if(wantDebug){
        if(FAILED(m_device.As(&m_infoQueue)))m_infoQueue.Reset();
        LOG(m_infoQueue?"D3D12 debug layer active; its messages are logged at every GPU wait.":"D3D12 debug layer unavailable (install the Windows Graphics Tools feature).");
    }
    D3D12_COMMAND_QUEUE_DESC q{};q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    if(!HR(m_device->CreateCommandQueue(&q,IID_PPV_ARGS(&m_queue)),"CreateCommandQueue"))return false;
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&m_allocators[i])),"CreateCommandAllocator"))return false;
    }
    for(uint32_t i=0;i<FrameCount;++i) {
        if(!HR(m_device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,m_allocators[i].Get(),nullptr,IID_PPV_ARGS(&m_cmds[i])),"CreateCommandList"))return false;
        m_cmds[i]->Close();
    }
    BOOL tearing=FALSE;if(SUCCEEDED(m_factory->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,&tearing,sizeof(tearing))))m_allowTearing=tearing==TRUE;
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=m_outputW;sd.Height=m_outputH;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;sd.SampleDesc={1,0};sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount=FrameCount;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;sd.Scaling=DXGI_SCALING_STRETCH;sd.AlphaMode=DXGI_ALPHA_MODE_IGNORE;sd.Flags=m_allowTearing?DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING:0;
    ComPtr<IDXGISwapChain1>sc1;if(!HR(m_factory->CreateSwapChainForHwnd(m_queue.Get(),hwnd,&sd,nullptr,nullptr,&sc1),"CreateSwapChainForHwnd"))return false;
    m_factory->MakeWindowAssociation(hwnd,DXGI_MWA_NO_ALT_ENTER);sc1.As(&m_swapchain);
    if(m_swapchain) m_swapchain->SetMaximumFrameLatency(2);
    if(!HR(m_device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&m_fence)),"CreateFence"))return false;
    m_fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);return m_fenceEvent!=nullptr;
}

bool D3D12Renderer::CreateHeapsAndBackbuffers(){
    D3D12_DESCRIPTOR_HEAP_DESC rh{};rh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;rh.NumDescriptors=FrameCount+12; // +7 = zero MV texture, +8..+10 = NR Smooth output + delta pair, +11 = neural result (render size)
    if(!HR(m_device->CreateDescriptorHeap(&rh,IID_PPV_ARGS(&m_rtvHeap)),"Create RTV heap"))return false;m_rtvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    for(uint32_t i=0;i<FrameCount;++i){if(!HR(m_swapchain->GetBuffer(i,IID_PPV_ARGS(&m_backbuffers[i])),"Get backbuffer"))return false;m_device->CreateRenderTargetView(m_backbuffers[i].Get(),nullptr,RTV(i));}
    D3D12_DESCRIPTOR_HEAP_DESC sh{};sh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;sh.NumDescriptors=27;sh.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE; // slot 16 = overlay label atlas, 17 = decoded source alias for the A/B "pure original" side, 18..25 = the two NR Smooth t1..t4 tables, 26 = neural result (render size)
    if(!HR(m_device->CreateDescriptorHeap(&sh,IID_PPV_ARGS(&m_srvHeap)),"Create SRV heap"))return false;
    m_srvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_DESCRIPTOR_HEAP_DESC dh{};dh.Type=D3D12_DESCRIPTOR_HEAP_TYPE_DSV;dh.NumDescriptors=1;
    if(!HR(m_device->CreateDescriptorHeap(&dh,IID_PPV_ARGS(&m_dsvHeap)),"Create DSV heap"))return false;
    m_dsvInc=m_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    return true;
}

bool D3D12Renderer::CreatePipelines(){
    const char* hlsl=R"(
Texture2D T:register(t0); SamplerState S:register(s0);
Texture3D LUT:register(t1);
Texture2D Aux0:register(t2);
Texture2D Aux1:register(t3);
Texture2D Aux2:register(t4);
Texture2D OverlayLabel:register(t5);   // 512x64 atlas: left half "DLSS OFF", right half "DLSS ON"
Texture2D MaskLayers:register(t6);     // packed segmentation layers, one per channel (present overlay)
cbuffer Params:register(b0){
    float2 JitterUV;
    float2 Misc;    // convert passes: x = LUT strength, y = LUT size N
    float4 ColorA; // present: brightness, contrast, saturation, gamma | convert: xyz = LUT domain scale, w = pre-sharpen amount
    float4 ColorB; // present: temperature, tint                     | convert: xyz = LUT domain offset
    float4 Extra;  // present: x = compare mode, y = split position, z = post-sharpen amount
    // Mask layers (guide expansion and the present overlay): A = background
    // structure, background tone, layer count, overlay on; B = layers 0 and 1
    // as (structure, tone) pairs; C = layers 2 and 3; D = per-layer enables.
    float4 MaskA; float4 MaskB; float4 MaskC; float4 MaskD;
}
struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};
V VS(uint id:SV_VertexID){float2 uv=float2((id<<1)&2,id&2);V o;o.uv=uv;o.p=float4(uv.x*2-1,1-uv.y*2,0,1);return o;}
float3 SRGBToLinear(float3 c){float3 lo=c/12.92;float3 hi=pow(max((c+0.055)/1.055,0),2.4);return lerp(hi,lo,step(c,0.04045));}
float3 LinearToSRGB(float3 c){c=max(c,0);float3 lo=c*12.92;float3 hi=1.055*pow(c,1.0/2.4)-0.055;return saturate(lerp(hi,lo,step(c,0.0031308)));}
// Inspection zoom (PRESENT-stage shaders only): JitterUV carries the uv offset
// and Misc.y the scale of the magnified sub-rect. Render-stage passes keep
// their own meanings for these constants and never call this.
float2 ZoomUV(float2 uv){float s=Misc.y>0.0?Misc.y:1.0;return JitterUV+uv*s;}
// Composites one atlas sub-rect [uMin..uMax] of the label texture at a SCREEN
// pixel rectangle (posPx,sizePx), alpha-blended. Screen-space, so labels stay put
// while the content zooms. uv is screen uv, (tw,th) the target size in pixels.
float3 DrawLabel(float3 dst,float2 uv,float tw,float th,float2 posPx,float2 sizePx,float uMin,float uMax,float vMin,float vMax){
    float2 lp=(uv*float2(tw,th)-posPx)/sizePx;   // 0..1 inside the label rect
    if(lp.x<0.0||lp.x>1.0||lp.y<0.0||lp.y>1.0) return dst;
    float4 t=OverlayLabel.SampleLevel(S,float2(lerp(uMin,uMax,lp.x),lerp(vMin,vMax,lp.y)),0);
    return lerp(dst,t.rgb,t.a);
}
// Optional pre-DLSS unsharp mask (amount in ColorA.w): a small, clamped detail
// boost on the decoded frame so the neural pass has more micro-contrast to bite
// into. The overshoot clamp keeps halos in check.
float3 SampleSharp(float2 uv){
    float3 c=T.SampleLevel(S,uv,0).rgb;
    if(ColorA.w>0.001){
        float tw,th;T.GetDimensions(tw,th);
        float2 px=float2(1.0/max(tw,1.0),1.0/max(th,1.0));
        float3 b=0.25*(T.SampleLevel(S,uv+float2(px.x,0),0).rgb+T.SampleLevel(S,uv-float2(px.x,0),0).rgb
                      +T.SampleLevel(S,uv+float2(0,px.y),0).rgb+T.SampleLevel(S,uv-float2(0,px.y),0).rgb);
        float3 d=clamp(c-b,-0.12,0.12);
        c=saturate(c+ColorA.w*d);
    }
    return c;
}
float4 PSConvert(V i):SV_Target{float3 c=SampleSharp(i.uv+JitterUV);return float4(SRGBToLinear(c),1);}
// Creative LUT applied in gamma space (where .cube grades expect to operate),
// then linearized for DLSS. Hardware trilinear filtering does the 3D interpolation.
float4 PSConvertLUT(V i):SV_Target{
    float3 c=SampleSharp(i.uv+JitterUV);
    float n=max(Misc.y,2.0);
    float3 t=saturate(c*ColorA.xyz+ColorB.xyz);
    float3 g=LUT.SampleLevel(S,t*((n-1.0)/n)+0.5/n,0).rgb;
    c=lerp(c,saturate(g),saturate(Misc.x));
    return float4(SRGBToLinear(c),1);
}
float3 ApplyVideoAdjustments(float3 c){
    float brightness=ColorA.x;
    float contrast=max(ColorA.y,0.0);
    float saturation=max(ColorA.z,0.0);
    float gamma=max(ColorA.w,0.05);
    float temperature=clamp(ColorB.x,-1.0,1.0);
    float tint=clamp(ColorB.y,-1.0,1.0);

    c=max(c,0.0);
    c*=exp2(brightness);
    c=(c-0.18)*contrast+0.18;
    float l=dot(c,float3(0.2126,0.7152,0.0722));
    c=lerp(l.xxx,c,saturation);
    c*=float3(1.0+0.12*temperature,1.0,1.0-0.12*temperature);
    c*=float3(1.0+0.05*tint,1.0-0.10*tint,1.0+0.05*tint);
    c=pow(max(c,0.0),1.0/gamma);
    return c;
}
// Post-DLSS unsharp mask (Extra.z) on the neural output: the NR models denoise
// while redrawing, and the pre-sharpen cannot restore acutance lost INSIDE the
// pass. Clamped overshoot keeps halos in check, applied in linear space.
float3 SampleOutSharp(float2 uv){
    float3 c=T.SampleLevel(S,uv,0).rgb;
    if(Extra.z>0.001){
        float tw,th;T.GetDimensions(tw,th);
        float2 px=float2(1.0/max(tw,1.0),1.0/max(th,1.0));
        float3 b=0.25*(T.SampleLevel(S,uv+float2(px.x,0),0).rgb+T.SampleLevel(S,uv-float2(px.x,0),0).rgb
                      +T.SampleLevel(S,uv+float2(0,px.y),0).rgb+T.SampleLevel(S,uv-float2(0,px.y),0).rgb);
        float3 d=clamp(c-b,-0.10,0.10);
        c=max(c+Extra.z*d,0);
    }
    return c;
}
// A/B compare separator (present stage): Extra.x = 0 off / 1 vertical split /
// 2 horizontal split, Extra.y = separator position in SCREEN uv. The "after"
// (full pipeline) and "before" (Aux2 = the NR input, m_dlssColor, linear) are
// both sampled at the SAME content uv so the two halves stay aligned under zoom;
// only the split coordinate is screen-space. A thin line + a grab dot mark it.
// Preview overlay of the segmentation layers (MaskA.w): every enabled layer
// tints the picture in its own colour, in the order the Masks panel lists
// them. Present stage only; the exporter never raises the flag.
float3 MaskOverlay(float3 c,float2 uv){
    if(MaskA.w<0.5)return c;
    float4 L=MaskLayers.SampleLevel(S,uv,0)*MaskD;
    c=lerp(c,float3(0.20,0.85,0.45),L.r*0.5);
    c=lerp(c,float3(0.35,0.55,1.00),L.g*0.5);
    c=lerp(c,float3(1.00,0.85,0.25),L.b*0.5);
    c=lerp(c,float3(0.95,0.35,0.85),L.a*0.5);
    return c;
}
float4 PSPresent(V i):SV_Target{
    float2 uv=ZoomUV(i.uv);
    float3 c=SampleOutSharp(uv);c=ApplyVideoAdjustments(c);
    float3 after=MaskOverlay(LinearToSRGB(c),uv);
    int cmp=(int)(Extra.x+0.5);
    if(cmp>0){
        // Aux2 in compare mode is the decoded source: 8-bit sRGB-encoded
        // already, so it is shown as-is (no linear decode).
        float3 before=Aux2.SampleLevel(S,uv,0).rgb;
        float sep=Extra.y;
        float coord=(cmp==1)?i.uv.x:i.uv.y;    // split along screen axis
        float3 outc=(coord<sep)?before:after;
        // Bold separator so it reads on any content: a white core line with a dark
        // halo, and a solid round grab handle with a grip that shows the drag axis.
        float tw,th;T.GetDimensions(tw,th);
        const float axisPx=(cmp==1)?max(tw,1.0):max(th,1.0);
        const float dl=abs(coord-sep)*axisPx;      // distance to the line, in pixels
        if(dl<2.5) outc=float3(0.97,0.97,0.97);
        else if(dl<4.5) outc=lerp(outc,float3(0,0,0),0.45);
        // Handle at the centre of the line.
        float2 hc=(cmp==1)?float2(sep,0.5):float2(0.5,sep);
        float2 g=(i.uv-hc)*float2(tw,th);          // pixel offset from handle centre
        const float rr=length(g);
        if(rr<16.0){
            outc=(rr>13.0)?float3(0,0,0):float3(0.97,0.97,0.97);   // dark rim + white disc
            // Grip bars across the drag axis (horizontal bars for a vertical split).
            if(cmp==1){ if(abs(g.x)<6.0 && abs(abs(g.y)-3.5)<1.2) outc=float3(0.15,0.15,0.15); }
            else      { if(abs(g.y)<6.0 && abs(abs(g.x)-3.5)<1.2) outc=float3(0.15,0.15,0.15); }
        }
        // Corner labels: "DLSS OFF" over the before side, "DLSS ON" over the after
        // side. Sized as a fraction of the frame (the backbuffer is output res and
        // is scaled down for display, so fixed pixels would look tiny). 4:1 atlas.
        const float lwx=tw*0.15; const float2 lsz=float2(lwx,lwx*0.25); const float mg=tw*0.016;
        // Effects-state indicator (Misc.x in compare mode: 1 = FX ON, 2 = FX
        // BYPASS), a smaller pill next to the DLSS ON label.
        const float2 isz=lsz*0.62; const float ig=mg*0.45;
        const float iu0=Misc.x>1.5?0.5:0.0;
        if(cmp==1){
            outc=DrawLabel(outc,i.uv,tw,th,float2(mg,mg),lsz,0.0,0.5,0.0,0.5);            // OFF top-left
            outc=DrawLabel(outc,i.uv,tw,th,float2(tw-lsz.x-mg,mg),lsz,0.5,1.0,0.0,0.5);   // ON top-right
            if(Misc.x>0.5)outc=DrawLabel(outc,i.uv,tw,th,float2(tw-isz.x-mg,mg+lsz.y+ig),isz,iu0,iu0+0.5,0.5,1.0);
        } else {
            outc=DrawLabel(outc,i.uv,tw,th,float2(mg,mg),lsz,0.0,0.5,0.0,0.5);            // OFF top-left
            outc=DrawLabel(outc,i.uv,tw,th,float2(mg,th-lsz.y-mg),lsz,0.5,1.0,0.0,0.5);   // ON bottom-left
            if(Misc.x>0.5)outc=DrawLabel(outc,i.uv,tw,th,float2(mg,th-lsz.y-mg-isz.y-ig),isz,iu0,iu0+0.5,0.5,1.0);
        }
        return float4(outc,1);
    }
    return float4(after,1);
}
// Direct DLSS-NR bracket passes: the NR model consumes/produces sRGB-ENCODED LDR
// values staged in plain FP16 textures (the NR runtime's contract), so the
// linear pipeline is converted on the way in and restored on the way out.
float4 PSEncodeSRGB(V i):SV_Target{return float4(LinearToSRGB(T.SampleLevel(S,i.uv,0).rgb),1);}
float4 PSDecodeSRGB(V i):SV_Target{return float4(SRGBToLinear(T.SampleLevel(S,i.uv,0).rgb),1);}
// Low-frequency pair: one MRT draw downsampling the DLSS input (t0) and output
// (t2) with a small tap cluster; sampled back bilinearly these act as the blur
// terms of the tone-preserve recombine.
struct DownOut{float4 a:SV_Target0;float4 b:SV_Target1;};
DownOut PSDownPair(V i){
    DownOut o;
    float tw,th;T.GetDimensions(tw,th);float2 pa=float2(4.0/max(tw,1.0),4.0/max(th,1.0));
    float aw,ah;Aux0.GetDimensions(aw,ah);float2 pb=float2(4.0/max(aw,1.0),4.0/max(ah,1.0));
    float3 a=0.25*(T.SampleLevel(S,i.uv+pa,0).rgb+T.SampleLevel(S,i.uv-pa,0).rgb
                  +T.SampleLevel(S,i.uv+float2(pa.x,-pa.y),0).rgb+T.SampleLevel(S,i.uv+float2(-pa.x,pa.y),0).rgb);
    float3 b=0.25*(Aux0.SampleLevel(S,i.uv+pb,0).rgb+Aux0.SampleLevel(S,i.uv-pb,0).rgb
                  +Aux0.SampleLevel(S,i.uv+float2(pb.x,-pb.y),0).rgb+Aux0.SampleLevel(S,i.uv+float2(-pb.x,pb.y),0).rgb);
    o.a=float4(a,1);o.b=float4(b,1);return o;
}
// Tone-preserve (one pass for the live preview AND the exporter): keep the NR luma detail,
// restore the original tone/colors by Misc.x. t0=NR output, t2=low(ref),
// t3=low(out), t4=full-res original. Math in gamma space like the exporter.
float4 PSPresentTone(V i):SV_Target{
    float2 uv=ZoomUV(i.uv);
    float3 o=LinearToSRGB(SampleOutSharp(uv));
    float3 r=LinearToSRGB(Aux2.SampleLevel(S,uv,0).rgb);
    float3 lr=LinearToSRGB(Aux0.SampleLevel(S,uv,0).rgb);
    float3 lo=LinearToSRGB(Aux1.SampleLevel(S,uv,0).rgb);
    float3 lw=float3(0.2126,0.7152,0.0722);
    float delta=(dot(lr,lw)-dot(r,lw))+(dot(o,lw)-dot(lo,lw));
    float3 c=lerp(o,saturate(r+delta),saturate(Misc.x));
    float3 lin=SRGBToLinear(c);
    lin=ApplyVideoAdjustments(lin);
    return float4(MaskOverlay(LinearToSRGB(lin),uv),1);
}
)" R"(
// NR Smooth: temporal EMA over the NR CONTRIBUTION (output - input, per
// channel, in sRGB units - the tuning of the exporter's original CPU pass),
// MOTION COMPENSATED: last frame's smoothed delta is fetched where this pixel
// WAS (t4 = MVs, current -> previous, DLSS-input pixels; ColorA.zw scale them
// to output pixels) and blended in by ColorA.x. Smoothing only the delta keeps
// the underlying video crisp while the neural layer settles; a per-pixel
// outlier reset (ColorA.y) drops the history on cuts and fast changes.
// t0 = NR output (linear), t2 = NR input (linear), t3 = previous delta,
// Misc.y = 1 while that previous delta is valid.
// RT0 = smoothed picture (linear), RT1 = this frame's smoothed delta.
struct SmoothOut{float4 c:SV_Target0;float4 d:SV_Target1;};
SmoothOut PSSmooth(V i){
    float3 o=LinearToSRGB(T.SampleLevel(S,i.uv,0).rgb);
    float3 r=LinearToSRGB(Aux0.SampleLevel(S,i.uv,0).rgb);
    float3 d=o-r;
    float3 sm=d;
    if(Misc.y>0.5){
        float tw,th;T.GetDimensions(tw,th);
        float2 mv=Aux2.SampleLevel(S,i.uv,0).rg*ColorA.zw;
        float2 puv=i.uv+mv/float2(max(tw,1.0),max(th,1.0));
        if(all(puv>=0.0)&&all(puv<=1.0)){
            float3 wp=Aux1.SampleLevel(S,puv,0).rgb;
            float3 ema=wp*ColorA.x+d*(1.0-ColorA.x);
            sm=(abs(d-wp)>ColorA.y)?d:ema;
        }
    }
    SmoothOut res;
    res.c=float4(SRGBToLinear(saturate(r+sm)),1);
    res.d=float4(sm,1);
    return res;
}
// MV debug view: the Middlebury / RAFT flow_viz colour wheel, so the picture
// reads like every published optical-flow render - white where nothing moves,
// hue = direction, colour depth = magnitude relative to this frame's peak
// (Extra.w, measured on the CPU per frame and floored at 1 px so a static shot
// does not blow sub-pixel noise up to full colour). Our field is current ->
// previous while flow_viz takes previous -> current, so the angle is taken on
// the negated field: the same footage gets the same hues as a RAFT render.
float3 FlowWheel(float k){ // k in [0,55): the six Middlebury segments RY YG GC CB BM MR
    if(k<15.0)return float3(1,k/15.0,0);
    if(k<21.0)return float3(1-(k-15.0)/6.0,1,0);
    if(k<25.0)return float3(0,1,(k-21.0)/4.0);
    if(k<36.0)return float3(0,1-(k-25.0)/11.0,1);
    if(k<49.0)return float3((k-36.0)/13.0,0,1);
    return float3(1,0,1-(k-49.0)/6.0);
}
float4 PSMotion(V i):SV_Target{
    float2 m=T.SampleLevel(S,ZoomUV(i.uv),0).rg;
    float rad=saturate(length(m)/max(Extra.w,1e-3));
    float a=atan2(m.y,m.x)/3.14159265;   // -1..1 = flow_viz's atan2(-v,-u) of the forward field
    float3 c=FlowWheel((a+1.0)*0.5*54.0);
    return float4(1.0-rad*(1.0-c),1);
}
float4 PSDepth(V i):SV_Target{float d=saturate(T.SampleLevel(S,ZoomUV(i.uv),0).r);d=pow(d,0.7);return float4(d,d,d,1);}
// Mask view: the ControlMask as the runtime reads it - red = master weight,
// green = tone weight, blue = structure weight (white = full effect).
float4 PSMaskView(V i):SV_Target{return float4(saturate(T.SampleLevel(S,ZoomUV(i.uv),0).rgb),1);}
    // Depth comes directly from compact-guide B and is written through SV_Depth into
    // the exact typeless/D32 resource that NGX receives later in the frame.
    float PSWriteDepth(V i):SV_Depth{return saturate(T.SampleLevel(S,i.uv+JitterUV,0).b);}
    // External model-estimated depth: a full-res single-channel texture (R16),
    // 0 = near, 1 = far, replacing the compact-grid heuristic proxy.
    float PSWriteDepthExt(V i):SV_Depth{return saturate(T.SampleLevel(S,i.uv+JitterUV,0).r);}
    struct GuideOut{float2 mv:SV_Target0;float4 bias:SV_Target1;};
    // ControlMask channel layout (SMRU_MASK_PROBE): Extra holds one code per
    // channel of the mask texture - 0 = constant 0, 1 = constant 1, 2 = the
    // mask value, 3 = its inverse. Single-channel formats keep .x only.
    float4 MaskChannels(float u){
        float4 c=Extra;float4 r;
        [unroll]for(int k=0;k<4;++k)r[k]=c[k]<0.5?0.0:(c[k]<1.5?1.0:(c[k]<2.5?u:1.0-u));
        return r;
    }
    // The ControlMask the NR runtime reads (measured): R = master weight,
    // G = tone weight, B = structure weight, A ignored.
    // Without a mask video (ColorA.y = 0) the block matcher's per-block
    // confidence is the source, shaped by ColorA.x: 0 = as measured, 1 =
    // white (process everywhere), 2 = inverted; MaskChannels lays it out.
    // With segmentation layers bound in Aux1 (one per channel, soft edges
    // kept) every pixel starts at the background weights (MaskA.xy) and each
    // enabled layer paints its own weights over them by its alpha, later
    // layers on top. The runtime's structure response is dead below ~0.3
    // and near-linear above, so the slider is mapped onto that range; the
    // master is raised wherever anything is asked for, and only a pixel
    // that wants neither structure nor tone is switched off entirely.
    float4 ControlMask(float2 uv,float w){
        if(ColorA.y<0.5){float u=w>=0.5?1.0:0.0;float m=ColorA.x;u=m>=1.5?1.0-u:(m>=0.5?1.0:u);return MaskChannels(u);}
        float4 L=saturate(Aux1.SampleLevel(S,uv,0))*MaskD;
        float st=MaskA.x,tn=MaskA.y;
        st=lerp(st,MaskB.x,L.r);tn=lerp(tn,MaskB.y,L.r);
        st=lerp(st,MaskB.z,L.g);tn=lerp(tn,MaskB.w,L.g);
        st=lerp(st,MaskC.x,L.b);tn=lerp(tn,MaskC.y,L.b);
        st=lerp(st,MaskC.z,L.a);tn=lerp(tn,MaskC.w,L.a);
        float b=st>0.001?0.3+0.7*st:0.0;
        return float4(saturate(max(st,tn)*4.0),tn,b,1.0);
    }
    // The expanded field is always the estimated one so the MV debug view can
    // show it; Motion=Zero is honoured at NGX bind time with a zero texture.
    GuideOut PSExpandGuides(V i){float4 g=T.SampleLevel(S,i.uv+JitterUV,0);GuideOut o;o.mv=g.xy;o.bias=ControlMask(i.uv+JitterUV,g.w);return o;}
    // External per-pixel flow (_flow.mp4): R/G encode [-range..range] source px
    // around mid-code. Misc.xy carries decode-scale = 2*range * (render / source)
    // so the MV lands in DLSS-input pixels. Bias mask still comes from the grid.
    GuideOut PSExpandGuidesExt(V i){
        float4 g=T.SampleLevel(S,i.uv+JitterUV,0);
        float2 e=Aux0.SampleLevel(S,i.uv+JitterUV,0).rg;
        GuideOut o;o.mv=(e-0.5)*float2(Misc.x,Misc.y);o.bias=ControlMask(i.uv+JitterUV,g.w);return o;
    }
)";
    UINT flags=D3DCOMPILE_OPTIMIZATION_LEVEL3;ComPtr<ID3DBlob>vs,convert,convertLut,present,motion,depth,depthWrite,depthWriteExt,expand,maskView,err;
    auto C=[&](const char*entry,const char*target,ComPtr<ID3DBlob>&out)->bool{err.Reset();HRESULT hr=D3DCompile(hlsl,strlen(hlsl),nullptr,nullptr,nullptr,entry,target,flags,0,&out,&err);if(FAILED(hr)){if(err)LOG((char*)err->GetBufferPointer());return false;}return true;};
    ComPtr<ID3DBlob>downPair,presentTone,expandExt,encodeSrgb,decodeSrgb,smooth;
    if(!C("VS","vs_5_1",vs)||!C("PSConvert","ps_5_1",convert)||!C("PSConvertLUT","ps_5_1",convertLut)||!C("PSPresent","ps_5_1",present)||!C("PSPresentTone","ps_5_1",presentTone)||!C("PSDownPair","ps_5_1",downPair)||!C("PSSmooth","ps_5_1",smooth)||!C("PSMotion","ps_5_1",motion)||!C("PSDepth","ps_5_1",depth)||!C("PSMaskView","ps_5_1",maskView)||!C("PSWriteDepth","ps_5_1",depthWrite)||!C("PSWriteDepthExt","ps_5_1",depthWriteExt)||!C("PSExpandGuides","ps_5_1",expand)||!C("PSExpandGuidesExt","ps_5_1",expandExt)||!C("PSEncodeSRGB","ps_5_1",encodeSrgb)||!C("PSDecodeSRGB","ps_5_1",decodeSrgb))return false;
    D3D12_DESCRIPTOR_RANGE range{};range.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;range.NumDescriptors=1;range.BaseShaderRegister=0;
    D3D12_DESCRIPTOR_RANGE lutRange{};lutRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;lutRange.NumDescriptors=4;lutRange.BaseShaderRegister=1;
    D3D12_DESCRIPTOR_RANGE ovRange{};ovRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;ovRange.NumDescriptors=1;ovRange.BaseShaderRegister=5; // t5 = overlay label atlas
    D3D12_DESCRIPTOR_RANGE mlRange{};mlRange.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV;mlRange.NumDescriptors=1;mlRange.BaseShaderRegister=6; // t6 = packed mask layers
    D3D12_ROOT_PARAMETER rp[5]{};rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[0].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[0].DescriptorTable.NumDescriptorRanges=1;rp[0].DescriptorTable.pDescriptorRanges=&range;
    rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;rp[1].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[1].Constants.Num32BitValues=32;rp[1].Constants.ShaderRegister=0;
    rp[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[2].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[2].DescriptorTable.NumDescriptorRanges=1;rp[2].DescriptorTable.pDescriptorRanges=&lutRange;
    rp[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[3].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[3].DescriptorTable.NumDescriptorRanges=1;rp[3].DescriptorTable.pDescriptorRanges=&ovRange;
    rp[4].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;rp[4].ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;rp[4].DescriptorTable.NumDescriptorRanges=1;rp[4].DescriptorTable.pDescriptorRanges=&mlRange;
    D3D12_STATIC_SAMPLER_DESC smp{};smp.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;smp.AddressU=smp.AddressV=smp.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;smp.ShaderRegister=0;smp.ShaderVisibility=D3D12_SHADER_VISIBILITY_PIXEL;smp.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rs{};rs.NumParameters=5;rs.pParameters=rp;rs.NumStaticSamplers=1;rs.pStaticSamplers=&smp;rs.Flags=D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob>sig;if(!HR(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&sig,&err),"SerializeRootSignature"))return false;
    if(!HR(m_device->CreateRootSignature(0,sig->GetBufferPointer(),sig->GetBufferSize(),IID_PPV_ARGS(&m_rootSig)),"CreateRootSignature"))return false;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};p.pRootSignature=m_rootSig.Get();p.VS={vs->GetBufferPointer(),vs->GetBufferSize()};p.PS={convert->GetBufferPointer(),convert->GetBufferSize()};
    p.BlendState.RenderTarget[0].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.BlendState.RenderTarget[1].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.BlendState.RenderTarget[2].RenderTargetWriteMask=D3D12_COLOR_WRITE_ENABLE_ALL;
    p.SampleMask=UINT_MAX;p.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;p.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;p.RasterizerState.DepthClipEnable=TRUE;
    p.DepthStencilState.DepthEnable=FALSE;p.DepthStencilState.StencilEnable=FALSE;p.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;p.NumRenderTargets=1;p.SampleDesc={1,0};
    p.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoConvert)),"Create convert PSO"))return false;
    p.PS={convertLut->GetBufferPointer(),convertLut->GetBufferSize()};
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoConvertLUT)),"Create convert+LUT PSO"))return false;
    p.PS={present->GetBufferPointer(),present->GetBufferSize()};
    p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoPresent)),"Create present PSO"))return false;
    p.PS={presentTone->GetBufferPointer(),presentTone->GetBufferSize()};
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoPresentTone)),"Create tone-preserve present PSO"))return false;
    // Direct-NR bracket PSOs: both stage through FP16 (encoded values), so the RTV
    // format matches m_nrColor / m_dlssOutput.
    p.PS={encodeSrgb->GetBufferPointer(),encodeSrgb->GetBufferSize()};p.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoToSRGB)),"Create ToSRGB PSO"))return false;
    p.PS={decodeSrgb->GetBufferPointer(),decodeSrgb->GetBufferSize()};
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoFromSRGB)),"Create FromSRGB PSO"))return false;
    p.RTVFormats[0]=DXGI_FORMAT_R8G8B8A8_UNORM;
    p.PS={motion->GetBufferPointer(),motion->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoMotionDebug)),"Create MV debug PSO"))return false;
    p.PS={depth->GetBufferPointer(),depth->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthDebug)),"Create depth debug PSO"))return false;
    p.PS={maskView->GetBufferPointer(),maskView->GetBufferSize()};if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoMaskDebug)),"Create mask view PSO"))return false;
    p.PS={expand->GetBufferPointer(),expand->GetBufferSize()};p.NumRenderTargets=2;p.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;p.RTVFormats[1]=m_maskFormat;p.RTVFormats[2]=DXGI_FORMAT_UNKNOWN;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoExpandGuides)),"Create GPU guide expansion PSO"))return false;
    p.PS={expandExt->GetBufferPointer(),expandExt->GetBufferSize()};
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoExpandGuidesExt)),"Create external-flow guide expansion PSO"))return false;
    p.PS={downPair->GetBufferPointer(),downPair->GetBufferSize()};p.RTVFormats[0]=DXGI_FORMAT_R16G16B16A16_FLOAT;p.RTVFormats[1]=DXGI_FORMAT_R16G16B16A16_FLOAT;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDownPair)),"Create low-frequency pair PSO"))return false;
    // NR Smooth shares the pair layout: two FP16 targets (smoothed picture, delta).
    p.PS={smooth->GetBufferPointer(),smooth->GetBufferSize()};
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoSmooth)),"Create NR Smooth PSO"))return false;
    p.RTVFormats[0]=DXGI_FORMAT_R16G16_FLOAT;p.RTVFormats[1]=m_maskFormat;
    p.PS={depthWrite->GetBufferPointer(),depthWrite->GetBufferSize()};
    p.NumRenderTargets=0;p.RTVFormats[0]=DXGI_FORMAT_UNKNOWN;p.RTVFormats[1]=DXGI_FORMAT_UNKNOWN;p.RTVFormats[2]=DXGI_FORMAT_UNKNOWN;p.DSVFormat=DXGI_FORMAT_D32_FLOAT;
    p.DepthStencilState.DepthEnable=TRUE;p.DepthStencilState.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL;p.DepthStencilState.DepthFunc=D3D12_COMPARISON_FUNC_ALWAYS;p.DepthStencilState.StencilEnable=FALSE;
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthWrite)),"Create real depth-buffer PSO"))return false;
    p.PS={depthWriteExt->GetBufferPointer(),depthWriteExt->GetBufferSize()};
    if(!HR(m_device->CreateGraphicsPipelineState(&p,IID_PPV_ARGS(&m_psoDepthWriteExt)),"Create external depth-buffer PSO"))return false;
    return true;
}

bool D3D12Renderer::InitializeDLSS(){
    auto* cmd=m_cmds[0].Get();
    m_allocators[0]->Reset();cmd->Reset(m_allocators[0].Get(),nullptr);
    const bool ok=NR().Load(m_device.Get());
    m_nrLoaded=ok;
    if(ok){m_renderW=m_outputW;m_renderH=m_outputH;}   // NR is 1:1, no upscale
    // DLSS Super Resolution: only when asked for and the output is larger than
    // the source. The neural pass then runs at SOURCE size and SR reconstructs
    // the output from the frame history (same MVs, depth and mask). Created on
    // the list the NR load uses, so it is flushed before its first evaluate.
    m_srActive=false;
    if(m_srRequested&&(m_outputW>m_sourceW||m_outputH>m_sourceH)){
        if(!m_sr)m_sr=std::make_unique<SuperRes>();
        const bool loaded=m_sr->Available()||m_sr->Load(m_device.Get(),NR().CoreInitialised());
        if(loaded&&m_sr->EnsureFeature(cmd,m_sourceW,m_sourceH,m_outputW,m_outputH)){
            // The SR host may have nudged the source into a mode's input range;
            // the convert pass resizes to whatever it chose.
            m_renderW=m_sr->RenderW();m_renderH=m_sr->RenderH();m_srActive=true;
            LOG("DLSS SR active: neural pass at "<<m_renderW<<"x"<<m_renderH<<" (source "<<m_sourceW<<"x"<<m_sourceH<<"), SR to "<<m_outputW<<"x"<<m_outputH<<" ("<<m_sr->QualityName()<<")");
        } else LOG("DLSS SR requested but unavailable for "<<m_sourceW<<"x"<<m_sourceH<<" -> "<<m_outputW<<"x"<<m_outputH<<"; upscaling with the resize instead.");
    }
    cmd->Close();ID3D12CommandList*l[]={cmd};m_queue->ExecuteCommandLists(1,l);WaitGPU();return ok;
}

bool D3D12Renderer::CreateUploadForTexture(const D3D12_RESOURCE_DESC&desc,ComPtr<ID3D12Resource>&upload,uint8_t*&mapped,D3D12_PLACED_SUBRESOURCE_FOOTPRINT&fp,uint32_t&rows,uint64_t&rowBytes,uint64_t&total,const char*name){
    m_device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowBytes,&total);D3D12_RESOURCE_DESC b{};b.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;b.Width=total;b.Height=1;b.DepthOrArraySize=1;b.MipLevels=1;b.SampleDesc={1,0};b.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto hp=HeapProps(D3D12_HEAP_TYPE_UPLOAD);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&b,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),name))return false;D3D12_RANGE r{0,0};return HR(upload->Map(0,&r,reinterpret_cast<void**>(&mapped)),"Map upload resource");
}

bool D3D12Renderer::SetLUT(std::vector<float> rgbTriples, uint32_t size,
                           const float domainMin[3], const float domainMax[3]) {
    if (size < 2 || size > 256 || rgbTriples.size() != size_t(size) * size * size * 3) return false;
    // Expand to RGBA32F texels (red fastest in .cube order -> x axis of the 3D texture).
    m_lutData.assign(size_t(size) * size * size * 4, 1.0f);
    for (size_t i = 0, n = size_t(size) * size * size; i < n; ++i) {
        m_lutData[i * 4 + 0] = rgbTriples[i * 3 + 0];
        m_lutData[i * 4 + 1] = rgbTriples[i * 3 + 1];
        m_lutData[i * 4 + 2] = rgbTriples[i * 3 + 2];
    }
    for (int c = 0; c < 3; ++c) {
        const float lo = domainMin ? domainMin[c] : 0.0f;
        const float hi = domainMax ? domainMax[c] : 1.0f;
        const float span = (hi - lo) != 0.0f ? (hi - lo) : 1.0f;
        m_lutDomainScale[c] = 1.0f / span;
        m_lutDomainOffset[c] = -lo / span;
    }
    m_lutSize = size;
    m_lutUploaded = false;
    return true;
}

bool D3D12Renderer::CreateVideoResources(){
    auto hp=HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    // Deep input keeps float sources un-quantized on their way into DLSS; the
    // shaders sample normalized floats either way, so only the format changes.
    const DXGI_FORMAT srcFmt=m_deepInput?DXGI_FORMAT_R16G16B16A16_UNORM:DXGI_FORMAT_B8G8R8A8_UNORM;
    auto src=Tex2D(srcFmt,m_sourceW,m_sourceH,D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&src,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_decodedTexture)),"Create decoded texture"))return false;
    m_decodedTexture->SetName(L"Video_Decoded_BGRA_sRGB");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(src,m_upload[i],m_uploadMapped[i],fp,rows,rowBytes,total,"Create video upload"))return false;
        if(i==0){m_uploadFootprint=fp;m_numRows=rows;m_rowSize=rowBytes;m_uploadBytes=total;}
    }
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;
    srv.Format=srcFmt;m_device->CreateShaderResourceView(m_decodedTexture.Get(),&srv,SRVCPU(0));
    // Alias at slot 17: the A/B compare binds table t1..t4 at base 14 so t4 is
    // the DECODED ORIGINAL (bypass side), untouched by LUT/sharpen/NR.
    m_device->CreateShaderResourceView(m_decodedTexture.Get(),&srv,SRVCPU(17));

    D3D12_CLEAR_VALUE cv{};cv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;auto col=Tex2D(cv.Format,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&col,D3D12_RESOURCE_STATE_RENDER_TARGET,&cv,IID_PPV_ARGS(&m_dlssColor)),"Create DLSS color"))return false;m_dlssColor->SetName(L"DLSS_Color_Input_Linear_FP16");m_device->CreateRenderTargetView(m_dlssColor.Get(),nullptr,RTV(FrameCount));
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(4));

    auto mot=Tex2D(DXGI_FORMAT_R16G16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&mot,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_motion)),"Create motion guide"))return false;
    m_motion->SetName(L"DLSS_MotionVectors_CurrentToPrevious_RG16F");srv.Format=DXGI_FORMAT_R16G16_FLOAT;m_device->CreateShaderResourceView(m_motion.Get(),&srv,SRVCPU(2));m_device->CreateRenderTargetView(m_motion.Get(),nullptr,RTV(FrameCount+1));
    // Zero-vector twin of m_motion for Motion=Zero: NGX is bound to this one while
    // m_motion keeps the estimated field the MV debug view shows.
    D3D12_CLEAR_VALUE zcv{};zcv.Format=DXGI_FORMAT_R16G16_FLOAT;
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&mot,D3D12_RESOURCE_STATE_RENDER_TARGET,&zcv,IID_PPV_ARGS(&m_motionZero)),"Create zero motion guide"))return false;
    m_motionZero->SetName(L"DLSS_MotionVectors_Zero_RG16F");m_device->CreateRenderTargetView(m_motionZero.Get(),nullptr,RTV(FrameCount+7));m_motionZeroCleared=false;

    // One depth resource, two views: D32_FLOAT DSV for the depth writes and
    // R32_FLOAT SRV for the debug view. The same resource is bound to the NR
    // evaluate as DLSSNR.Depth when guide bit 2 is set.
    D3D12_CLEAR_VALUE dcv{};dcv.Format=DXGI_FORMAT_D32_FLOAT;dcv.DepthStencil.Depth=1.0f;dcv.DepthStencil.Stencil=0;
    auto dep=Tex2D(DXGI_FORMAT_R32_TYPELESS,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&dep,D3D12_RESOURCE_STATE_DEPTH_WRITE,&dcv,IID_PPV_ARGS(&m_depth)),"Create unified DLSS depth"))return false;
    m_depth->SetName(L"DLSS_Depth_R32_TYPELESS_D32_DSV_R32_SRV");
    srv.Format=DXGI_FORMAT_R32_FLOAT;m_device->CreateShaderResourceView(m_depth.Get(),&srv,SRVCPU(3));
    D3D12_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;m_device->CreateDepthStencilView(m_depth.Get(),&dsv,DSV());

    auto bias=Tex2D(m_maskFormat,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bias,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_biasCurrent)),"Create BiasCurrentColor mask"))return false;
    m_biasCurrent->SetName(L"DLSS_ControlMask");srv.Format=m_maskFormat;m_device->CreateShaderResourceView(m_biasCurrent.Get(),&srv,SRVCPU(5));m_device->CreateRenderTargetView(m_biasCurrent.Get(),nullptr,RTV(FrameCount+2));
    auto grid=Tex2D(DXGI_FORMAT_R32G32B32A32_FLOAT,m_gridW,m_gridH,D3D12_RESOURCE_FLAG_NONE);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&grid,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_guideGrid)),"Create compact temporal guide grid"))return false;
    m_guideGrid->SetName(L"DLSS_CompactGuideGrid_RGBA32F");
    for(uint32_t i=0;i<FrameCount;++i) {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; uint32_t rows=0; uint64_t rowBytes=0,total=0;
        if(!CreateUploadForTexture(grid,m_guideUpload[i],m_guideMapped[i],fp,rows,rowBytes,total,"Create compact guide upload"))return false;
        if(i==0){m_guideFootprint=fp;m_guideRows=rows;m_guideRowSize=rowBytes;m_guideUploadBytes=total;}
    }
    srv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;m_device->CreateShaderResourceView(m_guideGrid.Get(),&srv,SRVCPU(6));
    auto out=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_outputW,m_outputH,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS|D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&out,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&m_dlssOutput)),"Create DLSS output"))return false;
    m_dlssOutput->SetName(L"DLSS_Output_Linear_FP16_UAV");
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssOutput.Get(),&srv,SRVCPU(1));
    m_device->CreateRenderTargetView(m_dlssOutput.Get(),nullptr,RTV(FrameCount+6));
    // Direct DLSS-NR staging pair (FP16 carrying sRGB-encoded values - the NR
    // runtime's own contract; an _SRGB-typed texture would be linearised on read
    // and cannot carry the UAV flag the runtime's output needs).
    {
        auto nrc=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&nrc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&m_nrColor)),"Create NR color"))return false;
        m_nrColor->SetName(L"NR_Color_sRGBEncoded_FP16");
        m_device->CreateRenderTargetView(m_nrColor.Get(),nullptr,RTV(FrameCount+5));
        auto nro=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&nro,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&m_nrOut)),"Create NR out"))return false;
        m_nrOut->SetName(L"NR_Output_sRGBEncoded_FP16_UAV");
        srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_nrOut.Get(),&srv,SRVCPU(15));
        // The neural result decoded back to linear, at render size: what the
        // smoother works on and what the output stage (SR or a copy) reads.
        auto nrl=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&nrl,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&m_nrLinear)),"Create NR linear"))return false;
        m_nrLinear->SetName(L"NR_Result_Linear_FP16");
        m_device->CreateShaderResourceView(m_nrLinear.Get(),&srv,SRVCPU(26));
        m_device->CreateRenderTargetView(m_nrLinear.Get(),nullptr,RTV(FrameCount+11));
    }

    if(m_useExtDepth){
        auto edt=Tex2D(DXGI_FORMAT_R16_UNORM,m_sourceW,m_sourceH,D3D12_RESOURCE_FLAG_NONE);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&edt,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_extDepthTex)),"Create external depth texture"))return false;
        m_extDepthTex->SetName(L"External_ModelDepth_R16");
        for(uint32_t i=0;i<FrameCount;++i){
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint32_t rows=0;uint64_t rowBytes=0,total=0;
            if(!CreateUploadForTexture(edt,m_extDepthUpload[i],m_extDepthMapped[i],fp,rows,rowBytes,total,"Create external depth upload"))return false;
            if(i==0){m_extDepthFootprint=fp;m_extDepthRows=rows;m_extDepthRowSize=rowBytes;m_extDepthUploadBytes=total;}
        }
        srv.Format=DXGI_FORMAT_R16_UNORM;m_device->CreateShaderResourceView(m_extDepthTex.Get(),&srv,SRVCPU(8));
        LOG("External model depth armed: R16 " << m_sourceW << "x" << m_sourceH << " replaces the heuristic depth proxy when supplied per frame.");
    } else {
        // Keep heap slot 8 initialized (it can sit inside a bound-but-unused range).
        srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(8));
    }
    // Tone-preserve support: low-frequency pair (1/8 output res) + a full-res
    // reference view of the DLSS input, laid out consecutively for the t1..t4 table.
    m_lowW=std::max(1u,m_outputW/8u);m_lowH=std::max(1u,m_outputH/8u);
    auto lowDesc=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_lowW,m_lowH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&lowDesc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_lowRef)),"Create lowRef"))return false;
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&lowDesc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_lowOut)),"Create lowOut"))return false;
    m_lowRef->SetName(L"TonePreserve_LowRef");m_lowOut->SetName(L"TonePreserve_LowOut");
    m_device->CreateRenderTargetView(m_lowRef.Get(),nullptr,RTV(FrameCount+3));
    m_device->CreateRenderTargetView(m_lowOut.Get(),nullptr,RTV(FrameCount+4));
    srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_device->CreateShaderResourceView(m_lowRef.Get(),&srv,SRVCPU(9));
    m_device->CreateShaderResourceView(m_lowOut.Get(),&srv,SRVCPU(10));
    m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(11));
    // Slot 12 = external flow (below); 13/14 initialized fallbacks inside t1..t4 ranges.
    m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(12));
    m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(13));
    m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(14));
    // NR Smooth: the smoothed picture (copied back over the neural result so
    // the output stage reads the one texture) and a ping-pong pair carrying
    // last frame's smoothed NR delta, all at render size. One t1..t4 table per
    // "previous" delta: t2 = NR input, t3 = previous delta, t4 = motion vectors.
    auto smoothDesc=Tex2D(DXGI_FORMAT_R16G16B16A16_FLOAT,m_renderW,m_renderH,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&smoothDesc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&m_smoothOut)),"Create NR Smooth output"))return false;
    m_smoothOut->SetName(L"NRSmooth_Output_Linear_FP16");
    m_device->CreateRenderTargetView(m_smoothOut.Get(),nullptr,RTV(FrameCount+8));
    for(uint32_t i=0;i<2;++i){
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&smoothDesc,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&m_smoothDelta[i])),"Create NR Smooth delta"))return false;
        m_smoothDelta[i]->SetName(i?L"NRSmooth_Delta1_FP16":L"NRSmooth_Delta0_FP16");
        m_device->CreateRenderTargetView(m_smoothDelta[i].Get(),nullptr,RTV(FrameCount+9+i));
        const uint32_t base=18+i*4;
        m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(base));
        m_device->CreateShaderResourceView(m_dlssColor.Get(),&srv,SRVCPU(base+1));
        m_device->CreateShaderResourceView(m_smoothDelta[i].Get(),&srv,SRVCPU(base+2));
        D3D12_SHADER_RESOURCE_VIEW_DESC msrv=srv;msrv.Format=DXGI_FORMAT_R16G16_FLOAT;
        m_device->CreateShaderResourceView(m_motion.Get(),&msrv,SRVCPU(base+3));
    }
    m_smoothCur=0;m_smoothHasHistory=false;
    if(m_useExtFlow){
        auto flt=Tex2D(DXGI_FORMAT_R16G16B16A16_UNORM,m_sourceW,m_sourceH,D3D12_RESOURCE_FLAG_NONE);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&flt,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_extFlowTex)),"Create external flow texture"))return false;
        m_extFlowTex->SetName(L"External_ModelFlow_RGBA16");
        for(uint32_t i=0;i<FrameCount;++i){
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint32_t rows=0;uint64_t rowBytes=0,total=0;
            if(!CreateUploadForTexture(flt,m_extFlowUpload[i],m_extFlowMapped[i],fp,rows,rowBytes,total,"Create external flow upload"))return false;
            if(i==0){m_extFlowFootprint=fp;m_extFlowRows=rows;m_extFlowRowSize=rowBytes;m_extFlowUploadBytes=total;}
        }
        srv.Format=DXGI_FORMAT_R16G16B16A16_UNORM;m_device->CreateShaderResourceView(m_extFlowTex.Get(),&srv,SRVCPU(12));
        LOG("External model flow armed: RGBA16 " << m_sourceW << "x" << m_sourceH << " replaces the CPU block-matcher MV field when supplied per frame.");
    }
    if(m_useExtMask){
        // Slot 13 = t3 (Aux1) inside the t1..t4 table; it otherwise holds a fallback view.
        auto mkt=Tex2D(DXGI_FORMAT_B8G8R8A8_UNORM,m_sourceW,m_sourceH,D3D12_RESOURCE_FLAG_NONE);
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&mkt,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_extMaskTex)),"Create external mask texture"))return false;
        m_extMaskTex->SetName(L"External_ModelMask_BGRA");
        for(uint32_t i=0;i<FrameCount;++i){
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint32_t rows=0;uint64_t rowBytes=0,total=0;
            if(!CreateUploadForTexture(mkt,m_extMaskUpload[i],m_extMaskMapped[i],fp,rows,rowBytes,total,"Create external mask upload"))return false;
            if(i==0){m_extMaskFootprint=fp;m_extMaskRows=rows;m_extMaskRowSize=rowBytes;m_extMaskUploadBytes=total;}
        }
        srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;m_device->CreateShaderResourceView(m_extMaskTex.Get(),&srv,SRVCPU(13));
        LOG("External model mask armed: BGRA " << m_sourceW << "x" << m_sourceH << " replaces the block-matcher uncertainty as the ControlMask source when supplied per frame.");
    }
    if(m_lutSize){
        const uint32_t N=m_lutSize;
        D3D12_RESOURCE_DESC ld{};ld.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D;ld.Width=N;ld.Height=N;ld.DepthOrArraySize=UINT16(N);ld.MipLevels=1;ld.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;ld.SampleDesc={1,0};ld.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&ld,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_lutTexture)),"Create LUT texture"))return false;
        m_lutTexture->SetName(L"Creative_LUT_3D_RGBA32F");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT lfp{};uint32_t lrows=0;uint64_t lrowBytes=0,ltotal=0;
        m_device->GetCopyableFootprints(&ld,0,1,0,&lfp,&lrows,&lrowBytes,&ltotal);
        D3D12_RESOURCE_DESC lb{};lb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;lb.Width=ltotal;lb.Height=1;lb.DepthOrArraySize=1;lb.MipLevels=1;lb.SampleDesc={1,0};lb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        auto uhp=HeapProps(D3D12_HEAP_TYPE_UPLOAD);
        if(!HR(m_device->CreateCommittedResource(&uhp,D3D12_HEAP_FLAG_NONE,&lb,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m_lutUpload)),"Create LUT upload"))return false;
        uint8_t* lmapped=nullptr;D3D12_RANGE lr{0,0};
        if(!HR(m_lutUpload->Map(0,&lr,reinterpret_cast<void**>(&lmapped)),"Map LUT upload"))return false;
        // For a 3D texture GetCopyableFootprints returns one footprint whose rows
        // span every slice: row index = z*N + y, each RowPitch apart.
        const size_t tightRow=size_t(N)*16u;
        for(uint32_t z=0;z<N;++z)
            for(uint32_t y=0;y<N;++y)
                memcpy(lmapped+lfp.Offset+size_t(lfp.Footprint.RowPitch)*(size_t(z)*N+y),
                       m_lutData.data()+(size_t(z)*N+y)*size_t(N)*4,tightRow);
        m_lutUpload->Unmap(0,nullptr);
        m_lutFootprint=lfp;
        D3D12_SHADER_RESOURCE_VIEW_DESC lsrv{};lsrv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;lsrv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;lsrv.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;lsrv.Texture3D.MipLevels=1;
        m_device->CreateShaderResourceView(m_lutTexture.Get(),&lsrv,SRVCPU(7));
        LOG("Creative LUT armed: " << N << "x" << N << "x" << N << " strength=" << m_lutStrength);
    }
    LOG("DLSS resource contract ready: Color=R16G16B16A16_FLOAT " << m_renderW << "x" << m_renderH
        << ", MV=R16G16_FLOAT " << m_renderW << "x" << m_renderH
        << ", Depth=R32_TYPELESS resource / D32_FLOAT DSV / R32_FLOAT SRV " << m_renderW << "x" << m_renderH
        << ", ControlMask=" << FormatName(m_maskFormat) << " " << m_renderW << "x" << m_renderH
        << ", Output=R16G16B16A16_FLOAT UAV " << m_outputW << "x" << m_outputH
        << ", CompactGrid=R32G32B32A32_FLOAT " << m_gridW << "x" << m_gridH << " -> GPU MV/bias expansion + direct SV_Depth write");
    return true;
}

// Bakes the A/B labels once into a 768x192 RGBA atlas via GDI: row 0 holds
// "DLSS OFF" / "DLSS ON", row 1 the effects-state indicator "FX ON" /
// "FX BYPASS". Uploaded to an SRV at heap slot 16 for the present pass to
// composite. GDI leaves alpha 0, so a manual alpha pass gives the pill and
// text opacity and keeps the rest transparent.
bool D3D12Renderer::CreateOverlayLabels(){
    const uint32_t W=768,H=192;  // 2x2 grid of 384x96 cells (4:1), high-res so large draws stay crisp
    BITMAPINFO bi{}; bi.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth=int(W); bi.bmiHeader.biHeight=-int(H); bi.bmiHeader.biPlanes=1;
    bi.bmiHeader.biBitCount=32; bi.bmiHeader.biCompression=BI_RGB;
    void* bits=nullptr; HDC screen=GetDC(nullptr);
    HBITMAP dib=CreateDIBSection(screen,&bi,DIB_RGB_COLORS,&bits,nullptr,0);
    ReleaseDC(nullptr,screen);
    if(!dib||!bits){ if(dib)DeleteObject(dib); return false; }
    memset(bits,0,size_t(W)*H*4);
    HDC dc=CreateCompatibleDC(nullptr); auto oldbmp=SelectObject(dc,dib);
    SetBkMode(dc,TRANSPARENT);
    HFONT font=CreateFontW(-52,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                           OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    auto oldfont=SelectObject(dc,font);
    HBRUSH pill=CreateSolidBrush(RGB(18,20,18));
    auto oldbr=SelectObject(dc,pill); auto oldpen=SelectObject(dc,GetStockObject(NULL_PEN));
    RoundRect(dc,10,10,374,86,24,24);      // left pill  (OFF), 0..384 region
    RoundRect(dc,394,10,758,86,24,24);     // right pill (ON),  384..768 region
    SelectObject(dc,oldbr);SelectObject(dc,oldpen);DeleteObject(pill);
    SetTextColor(dc,RGB(242,242,242));
    RECT r1{10,10,374,86}; DrawTextW(dc,L"DLSS OFF",-1,&r1,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    RECT r2{394,10,758,86}; DrawTextW(dc,L"DLSS ON",-1,&r2,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    // Row 1: effects-state indicator cells, drawn smaller inside the same 4:1
    // cells so the shader can composite them at a reduced size.
    HFONT fontFx=CreateFontW(-44,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,L"Segoe UI");
    SelectObject(dc,fontFx);
    HBRUSH pill2=CreateSolidBrush(RGB(18,20,18));
    auto ob2=SelectObject(dc,pill2);auto op2=SelectObject(dc,GetStockObject(NULL_PEN));
    RoundRect(dc,10,106,374,182,24,24);
    RoundRect(dc,394,106,758,182,24,24);
    SelectObject(dc,ob2);SelectObject(dc,op2);DeleteObject(pill2);
    SetTextColor(dc,RGB(140,225,150));
    RECT r3{10,106,374,182}; DrawTextW(dc,L"FX ON",-1,&r3,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    SetTextColor(dc,RGB(235,190,120));
    RECT r4{394,106,758,182}; DrawTextW(dc,L"FX BYPASS",-1,&r4,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    GdiFlush();
    SelectObject(dc,oldfont);DeleteObject(font);DeleteObject(fontFx);SelectObject(dc,oldbmp);DeleteDC(dc);
    // Alpha pass on the BGRA DIB: untouched black -> transparent, bright -> text
    // (opaque), else -> pill (translucent).
    uint8_t* srcpx=static_cast<uint8_t*>(bits);
    for(uint32_t i=0;i<W*H;++i){
        const uint8_t b=srcpx[i*4+0],g=srcpx[i*4+1],r=srcpx[i*4+2];
        const uint32_t lum=(r*54u+g*183u+b*19u)>>8;
        srcpx[i*4+3]=(r==0&&g==0&&b==0)?0:(lum>150?255:205);
    }
    auto hp=HeapProps(D3D12_HEAP_TYPE_DEFAULT);
    auto td=Tex2D(DXGI_FORMAT_B8G8R8A8_UNORM,W,H,D3D12_RESOURCE_FLAG_NONE);
    if(!HR(m_device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_labelAtlas)),"Create label atlas")){DeleteObject(dib);return false;}
    m_labelAtlas->SetName(L"Overlay_Labels_DLSS_OnOff");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};uint32_t rows=0;uint64_t rowBytes=0,total=0;
    m_device->GetCopyableFootprints(&td,0,1,0,&fp,&rows,&rowBytes,&total);
    D3D12_RESOURCE_DESC ub{};ub.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;ub.Width=total;ub.Height=1;ub.DepthOrArraySize=1;ub.MipLevels=1;ub.SampleDesc={1,0};ub.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto uhp=HeapProps(D3D12_HEAP_TYPE_UPLOAD);
    if(!HR(m_device->CreateCommittedResource(&uhp,D3D12_HEAP_FLAG_NONE,&ub,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&m_labelUpload)),"Create label upload")){DeleteObject(dib);return false;}
    uint8_t* mapped=nullptr;D3D12_RANGE rr{0,0};
    if(!HR(m_labelUpload->Map(0,&rr,reinterpret_cast<void**>(&mapped)),"Map label upload")){DeleteObject(dib);return false;}
    for(uint32_t y=0;y<H;++y) memcpy(mapped+fp.Offset+size_t(fp.Footprint.RowPitch)*y,srcpx+size_t(y)*W*4,size_t(W)*4);
    m_labelUpload->Unmap(0,nullptr);
    DeleteObject(dib);
    auto* cmd=m_cmds[0].Get();
    m_allocators[0]->Reset();cmd->Reset(m_allocators[0].Get(),nullptr);
    D3D12_TEXTURE_COPY_LOCATION d{};d.pResource=m_labelAtlas.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION s{};s.pResource=m_labelUpload.Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=fp;
    cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
    Barrier(cmd,m_labelAtlas.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->Close();ID3D12CommandList*l[]={cmd};m_queue->ExecuteCommandLists(1,l);WaitGPU();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Texture2D.MipLevels=1;srv.Format=DXGI_FORMAT_B8G8R8A8_UNORM;
    m_device->CreateShaderResourceView(m_labelAtlas.Get(),&srv,SRVCPU(16));
    return true;
}

void D3D12Renderer::CopyMappedRows(uint8_t*mapped,const D3D12_PLACED_SUBRESOURCE_FOOTPRINT&fp,const void*src,size_t tight,uint32_t rows){const uint8_t*s=static_cast<const uint8_t*>(src);for(uint32_t y=0;y<rows;++y)memcpy(mapped+fp.Offset+size_t(fp.Footprint.RowPitch)*y,s+tight*y,tight);}

float D3D12Renderer::Halton(uint32_t index,uint32_t base){float f=1.0f,r=0.0f;while(index){f/=float(base);r+=f*float(index%base);index/=base;}return r;}

void D3D12Renderer::SetMaskLayers(const MaskLayer* layers,uint32_t count,float bgStructure,float bgTone){
    m_maskLayerCount=std::min(count,kMaxMaskLayers);
    for(uint32_t k=0;k<kMaxMaskLayers;++k)m_maskLayers[k]=(layers&&k<m_maskLayerCount)?layers[k]:MaskLayer{};
    m_maskBgStructure=std::clamp(bgStructure,0.0f,1.0f);m_maskBgTone=std::clamp(bgTone,0.0f,1.0f);
}
// The 16 root constants behind MaskA..MaskD (see the shader's cbuffer).
void D3D12Renderer::MaskParams(float*o,bool overlay)const{
    o[0]=m_maskBgStructure;o[1]=m_maskBgTone;o[2]=float(m_maskLayerCount);o[3]=overlay?1.0f:0.0f;
    for(uint32_t k=0;k<kMaxMaskLayers;++k){
        const MaskLayer&l=m_maskLayers[k];
        o[4+k*2]=std::clamp(l.structure,0.0f,1.0f);o[5+k*2]=std::clamp(l.tone,0.0f,1.0f);
        o[12+k]=(k<m_maskLayerCount&&l.enabled)?1.0f:0.0f;
    }
}

// Peak motion-vector magnitude of a frame in DLSS-input pixels, for the MV
// debug view: like RAFT's flow_viz it scales colour by the frame's own maximum.
// The flow frame is sampled every 4th pixel; the compact grid is read whole.
static float PeakFlowRGBA16(const uint8_t*rgba16,uint32_t w,uint32_t h,float scaleX,float scaleY){
    const uint16_t*p=reinterpret_cast<const uint16_t*>(rgba16);float best=0.0f;
    for(uint32_t y=0;y<h;y+=4){const uint16_t*row=p+size_t(y)*w*4u;
        for(uint32_t x=0;x<w;x+=4){const float dx=(float(row[size_t(x)*4u])/65535.0f-0.5f)*scaleX,dy=(float(row[size_t(x)*4u+1])/65535.0f-0.5f)*scaleY;best=std::max(best,dx*dx+dy*dy);}}
    return std::sqrt(best);
}
static float PeakFlowGrid(const float*rgba32f,uint32_t w,uint32_t h){
    float best=0.0f;for(size_t i=0,n=size_t(w)*h;i<n;++i){const float dx=rgba32f[i*4],dy=rgba32f[i*4+1];best=std::max(best,dx*dx+dy*dy);}
    return std::sqrt(best);
}
bool D3D12Renderer::RenderFrame(const uint8_t*bgra,size_t bytes,const float*guideGridRGBA32F,size_t guideBytes,uint32_t gridW,uint32_t gridH,bool temporalReset,const uint8_t*extDepthR16,size_t extDepthBytes,const uint8_t*extFlowRGBA16,size_t extFlowBytes,const uint8_t*extMaskBGRA,size_t extMaskBytes){
    // Deep input frames and external flow are RGBA16 (8 bytes/px); the mask stays BGRA8.
    const size_t videoRow=size_t(m_sourceW)*(m_deepInput?8u:4u),flowRow=size_t(m_sourceW)*8u,maskRow=size_t(m_sourceW)*4u,guideRow=size_t(m_gridW)*sizeof(float)*4u,depthRow=size_t(m_sourceW)*2u;
    if(!bgra||bytes<videoRow*m_sourceH||!guideGridRGBA32F||gridW!=m_gridW||gridH!=m_gridH||guideBytes<guideRow*m_gridH)return false;
    const bool haveExtDepth=m_useExtDepth&&m_extDepthTex&&extDepthR16&&extDepthBytes>=depthRow*m_sourceH;
    const bool haveExtFlow=m_useExtFlow&&m_extFlowTex&&extFlowRGBA16&&extFlowBytes>=flowRow*m_sourceH;
    const bool haveExtMask=m_useExtMask&&m_extMaskTex&&extMaskBGRA&&extMaskBytes>=maskRow*m_sourceH;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot)) return false;
    CopyMappedRows(m_uploadMapped[slot],m_uploadFootprint,bgra,videoRow,m_sourceH);
    CopyMappedRows(m_guideMapped[slot],m_guideFootprint,guideGridRGBA32F,guideRow,m_gridH);
    if(haveExtDepth)CopyMappedRows(m_extDepthMapped[slot],m_extDepthFootprint,extDepthR16,depthRow,m_sourceH);
    if(haveExtFlow)CopyMappedRows(m_extFlowMapped[slot],m_extFlowFootprint,extFlowRGBA16,flowRow,m_sourceH);
    if(haveExtMask)CopyMappedRows(m_extMaskMapped[slot],m_extMaskFootprint,extMaskBGRA,maskRow,m_sourceH);
    if(!HR(m_allocators[slot]->Reset(),"Reset frame allocator")) return false;
    auto* cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset frame command list")) return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);

    if(m_lutTexture&&!m_lutUploaded){
        D3D12_TEXTURE_COPY_LOCATION ld{};ld.pResource=m_lutTexture.Get();ld.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION ls{};ls.pResource=m_lutUpload.Get();ls.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;ls.PlacedFootprint=m_lutFootprint;
        cmd->CopyTextureRegion(&ld,0,0,0,&ls,nullptr);
        Barrier(cmd,m_lutTexture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_lutUploaded=true;
    }

    if(!m_sourceInCopyDest)Barrier(cmd,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION d{};d.pResource=m_decodedTexture.Get();d.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;D3D12_TEXTURE_COPY_LOCATION s{};s.pResource=m_upload[slot].Get();s.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;s.PlacedFootprint=m_uploadFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);Barrier(cmd,m_decodedTexture.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_sourceInCopyDest=false;

    if(!m_gridInCopyDest)Barrier(cmd,m_guideGrid.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    d.pResource=m_guideGrid.Get();s.pResource=m_guideUpload[slot].Get();s.PlacedFootprint=m_guideFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
    Barrier(cmd,m_guideGrid.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_gridInCopyDest=false;

    if(haveExtDepth){
        if(!m_extDepthInCopyDest)Barrier(cmd,m_extDepthTex.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        d.pResource=m_extDepthTex.Get();s.pResource=m_extDepthUpload[slot].Get();s.PlacedFootprint=m_extDepthFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        Barrier(cmd,m_extDepthTex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_extDepthInCopyDest=false;
        m_extDepthValid=true;
    }
    if(haveExtFlow){
        if(!m_extFlowInCopyDest)Barrier(cmd,m_extFlowTex.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        d.pResource=m_extFlowTex.Get();s.pResource=m_extFlowUpload[slot].Get();s.PlacedFootprint=m_extFlowFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        Barrier(cmd,m_extFlowTex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_extFlowInCopyDest=false;
        m_extFlowValid=true;
    }
    if(haveExtMask){
        if(!m_extMaskInCopyDest)Barrier(cmd,m_extMaskTex.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        d.pResource=m_extMaskTex.Get();s.pResource=m_extMaskUpload[slot].Get();s.PlacedFootprint=m_extMaskFootprint;cmd->CopyTextureRegion(&d,0,0,0,&s,nullptr);
        Barrier(cmd,m_extMaskTex.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);m_extMaskInCopyDest=false;
        m_extMaskValid=true;
    }

    // Jitter contract (see JitterMode in the header). The sample position drives the
    // spatial lookup of the color input and all guide buffers; the motion-vector
    // VALUES themselves remain unjittered. (Direct NR consumes colour only, so the
    // reported-offset half of the old contract is gone; the resample stays for A/B.)
    float sampleJX=0.0f, sampleJY=0.0f;
    if(m_jitterMode!=JitterMode::Zero){
        sampleJX=Halton(uint32_t(m_framesPresented%1024)+1,2)-0.5f;
        sampleJY=Halton(uint32_t(m_framesPresented%1024)+1,3)-0.5f;
    }
    const float jitterUVX=sampleJX/float(m_renderW), jitterUVY=sampleJY/float(m_renderH);

    // GPU-expand the compact CPU optical-flow/mask analysis to exact DLSS input
    // resolution. Depth is deliberately NOT mirrored through a color RT anymore:
    // it is written directly into the same typeless depth resource that NGX receives.
    if(!m_guidesInRT){Barrier(cmd,m_motion.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(cmd,m_biasCurrent.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);}m_guidesInRT=true;
    if(!m_motionZeroCleared){const float zero4[4]={0,0,0,0};cmd->ClearRenderTargetView(RTV(FrameCount+7),zero4,0,nullptr);Barrier(cmd,m_motionZero.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_motionZeroCleared=true;}
    D3D12_VIEWPORT gvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT gsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&gvp);cmd->RSSetScissorRects(1,&gsc);
    D3D12_CPU_DESCRIPTOR_HANDLE grt[2]={RTV(FrameCount+1),RTV(FrameCount+2)};cmd->OMSetRenderTargets(2,grt,FALSE,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    // ColorA.y tells the expansion shader to read the model mask (Aux1 = slot 13)
    // instead of the block-matcher uncertainty. Table 2 is bound unconditionally
    // so t1..t4 are valid on both expansion paths.
    const bool useExtMask=m_extMaskValid;
    float guideParams[32]={jitterUVX,jitterUVY,0,0,float(m_nrMaskMode),useExtMask?1.0f:0.0f,0,0,0,0,0,0,
                           m_maskChannels[0],m_maskChannels[1],m_maskChannels[2],m_maskChannels[3]};
    MaskParams(guideParams+16,false);
    cmd->SetGraphicsRootDescriptorTable(2,SRVGPU(11));
    if(m_extFlowValid&&m_extFlowEnabled){
        // Model flow replaces the block-matcher MV field; decode scale maps the
        // [-range..range] source-px encoding into DLSS-input pixels.
        cmd->SetPipelineState(m_psoExpandGuidesExt.Get());
        cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(6)); // t2 = flow texture (table 2 bound above)
        guideParams[2]=2.0f*m_extFlowRange*float(m_renderW)/float(std::max(1u,m_sourceW));
        guideParams[3]=2.0f*m_extFlowRange*float(m_renderH)/float(std::max(1u,m_sourceH));
        if(!m_exportMode&&haveExtFlow)m_mvVisMax=PeakFlowRGBA16(extFlowRGBA16,m_sourceW,m_sourceH,guideParams[2],guideParams[3]);
    } else {
        cmd->SetPipelineState(m_psoExpandGuides.Get());
        cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(6));
        if(!m_exportMode)m_mvVisMax=PeakFlowGrid(guideGridRGBA32F,m_gridW,m_gridH);
    }
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRoot32BitConstants(1,32,guideParams,0);cmd->DrawInstanced(3,1,0,0);
    Barrier(cmd,m_motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);Barrier(cmd,m_biasCurrent.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_guidesInRT=false;

    // Populate the exact depth resource passed to NGX. The resource is R32_TYPELESS,
    // viewed as D32_FLOAT while writing and R32_FLOAT while sampling/debugging.
    if(!m_depthInWrite)Barrier(cmd,m_depth.Get(),DepthGuideReadState,D3D12_RESOURCE_STATE_DEPTH_WRITE);m_depthInWrite=true;
    D3D12_VIEWPORT dvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT dsc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&dvp);cmd->RSSetScissorRects(1,&dsc);
    auto dsvh=DSV();cmd->OMSetRenderTargets(0,nullptr,FALSE,&dsvh);cmd->ClearDepthStencilView(dsvh,D3D12_CLEAR_FLAG_DEPTH,1.0f,0,0,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());
    // Once a model-estimated depth frame has been uploaded, it replaces the
    // heuristic proxy (and keeps being reused if a later frame omits depth).
    if(m_extDepthValid&&m_extDepthEnabled){cmd->SetPipelineState(m_psoDepthWriteExt.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(8));}
    else{cmd->SetPipelineState(m_psoDepthWrite.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(6));}
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRoot32BitConstants(1,4,guideParams,0);cmd->DrawInstanced(3,1,0,0);
    Barrier(cmd,m_depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,DepthGuideReadState);m_depthInWrite=false;

    if(!m_colorInRT)Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_RENDER_TARGET);m_colorInRT=true;
    D3D12_VIEWPORT vp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT sc{0,0,LONG(m_renderW),LONG(m_renderH)};cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&sc);
    auto crt=RTV(FrameCount);cmd->OMSetRenderTargets(1,&crt,FALSE,nullptr);const float black[4]={0,0,0,1};cmd->ClearRenderTargetView(crt,black,0,nullptr);cmd->SetGraphicsRootSignature(m_rootSig.Get());
    const bool useLut=m_lutTexture&&m_lutUploaded&&m_lutStrength>0.0f;
    cmd->SetPipelineState(useLut?m_psoConvertLUT.Get():m_psoConvert.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(0));
    {
        if(useLut)cmd->SetGraphicsRootDescriptorTable(2,SRVGPU(7));
        float cparams[16]={jitterUVX,jitterUVY,m_lutStrength,float(m_lutSize),
                           m_lutDomainScale[0],m_lutDomainScale[1],m_lutDomainScale[2],m_preSharpen,
                           m_lutDomainOffset[0],m_lutDomainOffset[1],m_lutDomainOffset[2],0,
                           0,0,0,0};
        cmd->SetGraphicsRoot32BitConstants(1,16,cparams,0);
    }
    cmd->DrawInstanced(3,1,0,0);Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,GuideReadState);m_colorInRT=false;

    ++m_framesPresented;

    // Create the NR feature on an open command list, submit that list, and only
    // then evaluate on a fresh list. The NR runtime requires the createFeature
    // list to be submitted and waited on before the first evaluate (NeuralEngine).
    bool needFeatureFlush = false;
    if (DLSSEnabled() && !DLSSFeatureCreated()) {
        needFeatureFlush = m_nr->EnsureFeature(cmd, m_renderW, m_renderH);
        temporalReset = true;
    }
    if (needFeatureFlush) {
        if (!HR(cmd->Close(), "Close command list after NGX CreateFeature")) return false;
        ID3D12CommandList* initLists[] = { cmd };
        m_queue->ExecuteCommandLists(1, initLists);
        WaitGPU();
        if (!HR(m_allocators[slot]->Reset(), "Reset allocator after NGX CreateFeature")) return false;
        if (!HR(cmd->Reset(m_allocators[slot].Get(), nullptr), "Reset command list after NGX CreateFeature")) return false;
        ID3D12DescriptorHeap* postCreateHeaps[] = { m_srvHeap.Get() };
        cmd->SetDescriptorHeaps(1, postCreateHeaps);
        LOG("NGX feature creation flushed before EvaluateFeature; temporal history reset.");
    }

    bool used=false;
    if (DLSSEnabled() && DLSSFeatureCreated()) {
        // Bracket the NR evaluate with the sRGB encode/decode passes; everything
        // else (tone-preserve, present) still reads linear m_dlssOutput.
        Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cmd,m_nrColor.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        D3D12_VIEWPORT nvp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT nsc{0,0,LONG(m_renderW),LONG(m_renderH)};
        cmd->RSSetViewports(1,&nvp);cmd->RSSetScissorRects(1,&nsc);
        auto nrc=RTV(FrameCount+5);cmd->OMSetRenderTargets(1,&nrc,FALSE,nullptr);
        cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoToSRGB.Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));cmd->DrawInstanced(3,1,0,0);
        Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,GuideReadState);
        Barrier(cmd,m_nrColor.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Barrier(cmd,m_nrOut.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // Guide binding: the MV/bias textures sit in GuideReadState and depth in
        // DepthGuideReadState here - all NGX-consumable - so the same per-frame
        // guides the debug views show are handed to the NR evaluate directly.
        // Motion=Zero swaps in the all-zero twin: the debug view keeps showing
        // the estimated field, NR receives zero vectors. So does a frame with
        // no correspondence to its predecessor (SetMotionInvalid).
        ID3D12Resource* mvForNR=(m_mvFieldScale>0.0f&&!m_motionInvalid)?m_motion.Get():m_motionZero.Get();
        NeuralEngine::Settings nr=m_nrSettings;
        nr.reset=temporalReset;
        nr.mvec=(m_nrGuideMask&1u)?mvForNR:nullptr;
        nr.depth=(m_nrGuideMask&2u)?m_depth.Get():nullptr;
        nr.controlMask=(m_nrGuideMask&4u)?m_biasCurrent.Get():nullptr;
        nr.mvScaleX=nr.mvScaleY=m_nrMVScale;
        ++m_nrEvaluations;
        used=m_nr->Evaluate(cmd,m_nrColor.Get(),m_nrOut.Get(),m_renderW,m_renderH,nr);
        // The evaluate leaves the command list with NGX's own descriptor heaps,
        // root signature and topology (documented; the debug layer flags every
        // later draw otherwise). Restore ours before the first pass that follows.
        {ID3D12DescriptorHeap*postEvalHeaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,postEvalHeaps);}
        cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        Barrier(cmd,m_nrOut.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        // Decode the result back to linear at render size; the output stage
        // below takes it to the output size (DLSS SR, or a copy at 1:1).
        Barrier(cmd,m_nrLinear.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        cmd->RSSetViewports(1,&nvp);cmd->RSSetScissorRects(1,&nsc);
        auto ort=RTV(FrameCount+11);cmd->OMSetRenderTargets(1,&ort,FALSE,nullptr);
        cmd->SetPipelineState(m_psoFromSRGB.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(15));cmd->DrawInstanced(3,1,0,0);
        Barrier(cmd,m_nrLinear.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    m_lastDLSSUsed=used;
    RecordNRSmooth(cmd,used,temporalReset);
    // Output stage: DLSS SR reconstructs the output size from the neural result
    // (or from the plain input with the neural pass off); without SR the render
    // size IS the output size and the result is handed over as it is.
    const bool produced=RecordOutput(cmd,used,temporalReset,sampleJX,sampleJY);
    m_lastProduced=produced;
    // A/B compare separator: shows the NR input (before) against the full pipeline
    // (after) split by a draggable line. Takes over the Final present, so tone
    // preserve steps aside while it is active. Never in export.
    const bool compareOn=!m_exportMode&&m_compareMode>0&&m_debugView==DebugView::Final&&produced;
    // Live tone-preserve: render the low-frequency pair, then present through the
    // recombine shader. Only meaningful when the DLSS/NR output was produced.
    const bool toneActive=used&&produced&&!compareOn&&m_toneMix>0.0f&&m_debugView==DebugView::Final&&m_psoPresentTone&&m_lowRef;
    if(toneActive){
        if(!m_lowInRT){Barrier(cmd,m_lowRef.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);Barrier(cmd,m_lowOut.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);}
        m_lowInRT=true;
        Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        D3D12_VIEWPORT lvp{0,0,float(m_lowW),float(m_lowH),0,1};D3D12_RECT lsc{0,0,LONG(m_lowW),LONG(m_lowH)};
        cmd->RSSetViewports(1,&lvp);cmd->RSSetScissorRects(1,&lsc);
        D3D12_CPU_DESCRIPTOR_HANDLE lrt[2]={RTV(FrameCount+3),RTV(FrameCount+4)};
        cmd->OMSetRenderTargets(2,lrt,FALSE,nullptr);
        cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoDownPair.Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));
        cmd->SetGraphicsRootDescriptorTable(2,SRVGPU(0));
        cmd->DrawInstanced(3,1,0,0);
        Barrier(cmd,m_lowRef.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cmd,m_lowOut.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_lowInRT=false;
        // m_dlssColor stays pixel-readable for the tone present (t4); restored below.
    }
    uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);cmd->ClearRenderTargetView(brt,black,0,nullptr);cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const bool applyColor=(m_debugView==DebugView::Final);
    const ColorSettings cs=applyColor?m_colorSettings:ColorSettings{};
    const float zs=m_exportMode?1.0f:m_zoomScale;
    const float zx=std::clamp(m_zoomCX-zs*0.5f,0.0f,1.0f-zs);
    const float zy=std::clamp(m_zoomCY-zs*0.5f,0.0f,1.0f-zs);
    float presentParams[32]={zx,zy,compareOn?(m_fxBypassIndicator?2.0f:1.0f):(toneActive?m_toneMix:0.0f),zs,cs.brightness,cs.contrast,cs.saturation,cs.gamma,cs.temperature,cs.tint,0,0,
                             compareOn?float(m_compareMode):0.0f,m_comparePos,applyColor?m_postSharpen:0.0f,std::max(m_mvVisMax,1.0f)};
    MaskParams(presentParams+16,m_maskOverlay&&!m_exportMode&&m_extMaskValid&&m_debugView==DebugView::Final);
    cmd->SetGraphicsRoot32BitConstants(1,32,presentParams,0);
    cmd->SetGraphicsRootDescriptorTable(4,SRVGPU(13)); // t6 = packed mask layers (a fallback view when none are armed)
    if(m_labelAtlas)cmd->SetGraphicsRootDescriptorTable(3,SRVGPU(16)); // t5 overlay labels (PSPresent)
    // DLSS inputs stay shader-readable for NGX. Only the texture selected for the
    // debug/fallback presentation pass is temporarily made pixel-shader readable.
    ID3D12Resource* debugPixelResource=nullptr;
    D3D12_RESOURCE_STATES debugBefore=GuideReadState;
    switch(m_debugView){
        case DebugView::MotionVectors:debugPixelResource=m_motion.Get();cmd->SetPipelineState(m_psoMotionDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(2));break;
        case DebugView::Depth:debugPixelResource=m_depth.Get();debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(3));break;
        case DebugView::BiasMask:debugPixelResource=m_biasCurrent.Get();cmd->SetPipelineState(m_psoMaskDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(5));break;
        case DebugView::Input:debugPixelResource=m_dlssColor.Get();cmd->SetPipelineState(m_psoPresent.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));break;
        default:
            if(compareOn){
                // before = the DECODED ORIGINAL via the t1..t4 table at base 14
                // (t4 = slot 17 = decoded source): the left side is a true
                // bypass, not the LUT/sharpen-processed NR input.
                Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                cmd->SetPipelineState(m_psoPresent.Get());
                cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
                cmd->SetGraphicsRootDescriptorTable(2,SRVGPU(14));
            } else if(toneActive){
                cmd->SetPipelineState(m_psoPresentTone.Get());
                cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
                cmd->SetGraphicsRootDescriptorTable(2,SRVGPU(8)); // t2=lowRef, t3=lowOut, t4=full-res ref
            } else {
                cmd->SetPipelineState(m_psoPresent.Get());
                if(produced)cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
                else{debugPixelResource=m_dlssColor.Get();cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));}
            }
            break;
    }
    if(debugPixelResource)Barrier(cmd,debugPixelResource,debugBefore,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->DrawInstanced(3,1,0,0);
    if(debugPixelResource)Barrier(cmd,debugPixelResource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,debugBefore);
    if(toneActive||compareOn)Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,GuideReadState);
    if(m_exportMode){
        if(!m_exportReadback){
            D3D12_RESOURCE_DESC bbd=m_backbuffers[bi]->GetDesc();
            uint32_t erows=0; uint64_t erowb=0,etotal=0;
            m_device->GetCopyableFootprints(&bbd,0,1,0,&m_exportFootprint,&erows,&erowb,&etotal);
            D3D12_HEAP_PROPERTIES rhp{}; rhp.Type=D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width=etotal; rd.Height=1; rd.DepthOrArraySize=1; rd.MipLevels=1; rd.Format=DXGI_FORMAT_UNKNOWN; rd.SampleDesc={1,0}; rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            HR(m_device->CreateCommittedResource(&rhp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&m_exportReadback)),"Create export readback buffer");
        }
        Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=m_exportReadback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=m_exportFootprint;
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=m_backbuffers[bi].Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex=0;
        cmd->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);
    } else {
        Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    }
    if(!HR(cmd->Close(),"Close frame command list")) return false;
    ID3D12CommandList*ls[]={cmd};m_queue->ExecuteCommandLists(1,ls);
    HRESULT phr=m_swapchain->Present(0,m_allowTearing?DXGI_PRESENT_ALLOW_TEARING:0);
    if(FAILED(phr)){LOG("Present failed hr=0x"<<std::hex<<phr);return false;}
    SignalFrameSlot(slot);
    m_frameSlot=(slot+1u)%FrameCount;
    if(m_exportMode && !m_exportReadback) LOG("Export readback buffer missing; no frame will be exported.");
    if(m_exportMode && m_exportReadback){
        WaitGPU();
        // The readback buffer is RowPitch*(H-1) + tight last row; a read range of
        // RowPitch*H overruns it and Map rejects the range whenever W*4 is not
        // 256-aligned (e.g. 480-wide frames).
        void* mapped=nullptr; D3D12_RANGE rr{0,size_t(m_exportFootprint.Footprint.RowPitch)*(m_outputH-1)+size_t(m_outputW)*4};
        if(FAILED(m_exportReadback->Map(0,&rr,&mapped))) LOG("Export readback Map failed.");
        else{
            m_exportRGBA.resize(size_t(m_outputW)*m_outputH*4);
            const uint8_t* srcp=(const uint8_t*)mapped;
            for(uint32_t y=0;y<m_outputH;++y)
                memcpy(m_exportRGBA.data()+size_t(y)*m_outputW*4, srcp+size_t(y)*m_exportFootprint.Footprint.RowPitch, size_t(m_outputW)*4);
            D3D12_RANGE wr{0,0}; m_exportReadback->Unmap(0,&wr);
        }
    }
    return true;
}

// NR Smooth (PSSmooth): runs once the NR output is linear in m_nrLinear
// (render size), writes the smoothed picture plus this frame's delta, then
// copies the picture back over m_nrLinear for the output stage. History lives
// in the delta pair and is dropped on a temporal reset, when the pass is off,
// or when NR did not run.
void D3D12Renderer::RecordNRSmooth(ID3D12GraphicsCommandList*cmd,bool used,bool temporalReset){
    if(!used||!m_smoothOut||!m_psoSmooth||m_nrSmooth<=0.001f){m_smoothHasHistory=false;return;}
    const bool history=m_smoothHasHistory&&!temporalReset;
    const uint32_t cur=m_smoothCur,prev=cur^1u;
    Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd,m_motion.Get(),GuideReadState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd,m_smoothDelta[cur].Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_VIEWPORT vp{0,0,float(m_renderW),float(m_renderH),0,1};D3D12_RECT sc{0,0,LONG(m_renderW),LONG(m_renderH)};
    cmd->RSSetViewports(1,&vp);cmd->RSSetScissorRects(1,&sc);
    D3D12_CPU_DESCRIPTOR_HANDLE rts[2]={RTV(FrameCount+8),RTV(FrameCount+9+cur)};
    cmd->OMSetRenderTargets(2,rts,FALSE,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->SetPipelineState(m_psoSmooth.Get());
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    // Misc.y = history valid; ColorA = keep, outlier threshold (48/255, the
    // tuning of the original CPU pass), MV scale (everything is render size).
    const float params[16]={0,0, 0,history?1.0f:0.0f,
                            m_nrSmooth,48.0f/255.0f,1.0f,1.0f,
                            0,0,0,0, 0,0,0,0};
    cmd->SetGraphicsRoot32BitConstants(1,16,params,0);
    cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(26));          // t0 = the neural result
    cmd->SetGraphicsRootDescriptorTable(2,SRVGPU(18+prev*4));   // t2..t4: input, previous delta, MVs
    cmd->DrawInstanced(3,1,0,0);
    Barrier(cmd,m_smoothDelta[cur].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd,m_motion.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,GuideReadState);
    Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,GuideReadState);
    Barrier(cmd,m_smoothOut.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd,m_nrLinear.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(m_nrLinear.Get(),m_smoothOut.Get());
    Barrier(cmd,m_nrLinear.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd,m_smoothOut.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
    m_smoothCur=prev;
    m_smoothHasHistory=true;
}

bool D3D12Renderer::PresentCurrent(){
    if(!m_swapchain||!m_queue||!m_rootSig)return false;
    const uint32_t slot=m_frameSlot%FrameCount;
    if(!WaitForFrameSlot(slot))return false;
    if(!HR(m_allocators[slot]->Reset(),"Reset static-present allocator"))return false;
    auto* cmd=m_cmds[slot].Get();
    if(!HR(cmd->Reset(m_allocators[slot].Get(),nullptr),"Reset static-present command list"))return false;
    ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);

    const float black[4]={0,0,0,1};
    uint32_t bi=m_swapchain->GetCurrentBackBufferIndex();
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_VIEWPORT ovp{0,0,float(m_outputW),float(m_outputH),0,1};
    D3D12_RECT osc{0,0,LONG(m_outputW),LONG(m_outputH)};
    cmd->RSSetViewports(1,&ovp);cmd->RSSetScissorRects(1,&osc);
    auto brt=RTV(bi);cmd->OMSetRenderTargets(1,&brt,FALSE,nullptr);cmd->ClearRenderTargetView(brt,black,0,nullptr);
    cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    const bool applyColor=(m_debugView==DebugView::Final);
    const ColorSettings cs=applyColor?m_colorSettings:ColorSettings{};
    const bool compareOn=!m_exportMode&&m_compareMode>0&&m_debugView==DebugView::Final&&m_lastProduced;
    const bool toneActive=m_lastDLSSUsed&&m_lastProduced&&!compareOn&&m_toneMix>0.0f&&m_debugView==DebugView::Final&&m_psoPresentTone&&m_lowRef&&!m_lowInRT;
    const float zs=m_exportMode?1.0f:m_zoomScale;
    const float zx=std::clamp(m_zoomCX-zs*0.5f,0.0f,1.0f-zs);
    const float zy=std::clamp(m_zoomCY-zs*0.5f,0.0f,1.0f-zs);
    float presentParams[32]={zx,zy,compareOn?(m_fxBypassIndicator?2.0f:1.0f):(toneActive?m_toneMix:0.0f),zs,cs.brightness,cs.contrast,cs.saturation,cs.gamma,cs.temperature,cs.tint,0,0,
                             compareOn?float(m_compareMode):0.0f,m_comparePos,applyColor?m_postSharpen:0.0f,std::max(m_mvVisMax,1.0f)};
    MaskParams(presentParams+16,m_maskOverlay&&!m_exportMode&&m_extMaskValid&&m_debugView==DebugView::Final);
    cmd->SetGraphicsRoot32BitConstants(1,32,presentParams,0);
    cmd->SetGraphicsRootDescriptorTable(4,SRVGPU(13)); // t6 = packed mask layers (a fallback view when none are armed)
    if(m_labelAtlas)cmd->SetGraphicsRootDescriptorTable(3,SRVGPU(16)); // t5 overlay labels (PSPresent)

    ID3D12Resource* debugPixelResource=nullptr;
    D3D12_RESOURCE_STATES debugBefore=GuideReadState;
    if(toneActive||compareOn)Barrier(cmd,m_dlssColor.Get(),GuideReadState,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    switch(m_debugView){
        case DebugView::MotionVectors:debugPixelResource=m_motion.Get();cmd->SetPipelineState(m_psoMotionDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(2));break;
        case DebugView::Depth:debugPixelResource=m_depth.Get();debugBefore=DepthGuideReadState;cmd->SetPipelineState(m_psoDepthDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(3));break;
        case DebugView::BiasMask:debugPixelResource=m_biasCurrent.Get();cmd->SetPipelineState(m_psoMaskDebug.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(5));break;
        case DebugView::Input:debugPixelResource=m_dlssColor.Get();cmd->SetPipelineState(m_psoPresent.Get());cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));break;
        default:
            if(compareOn){
                cmd->SetPipelineState(m_psoPresent.Get());
                cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
                cmd->SetGraphicsRootDescriptorTable(2,SRVGPU(14)); // t4 = slot 17 = decoded original (bypass side)
            } else if(toneActive){
                cmd->SetPipelineState(m_psoPresentTone.Get());
                cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
                cmd->SetGraphicsRootDescriptorTable(2,SRVGPU(8));
            } else {
                cmd->SetPipelineState(m_psoPresent.Get());
                if(m_lastProduced)cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(1));
                else{debugPixelResource=m_dlssColor.Get();cmd->SetGraphicsRootDescriptorTable(0,SRVGPU(4));}
            }
            break;
    }
    if(debugPixelResource)Barrier(cmd,debugPixelResource,debugBefore,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    cmd->DrawInstanced(3,1,0,0);
    if(debugPixelResource)Barrier(cmd,debugPixelResource,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,debugBefore);
    if(toneActive||compareOn)Barrier(cmd,m_dlssColor.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,GuideReadState);
    Barrier(cmd,m_backbuffers[bi].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);
    if(!HR(cmd->Close(),"Close static-present command list"))return false;
    ID3D12CommandList*ls[]={cmd};m_queue->ExecuteCommandLists(1,ls);
    HRESULT phr=m_swapchain->Present(0,m_allowTearing?DXGI_PRESENT_ALLOW_TEARING:0);
    if(FAILED(phr)){LOG("Static Present failed hr=0x"<<std::hex<<phr);return false;}
    SignalFrameSlot(slot);m_frameSlot=(slot+1u)%FrameCount;
    return true;
}

void D3D12Renderer::Barrier(ID3D12GraphicsCommandList*cmd,ID3D12Resource*res,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b){if(a==b)return;auto x=Transition(res,a,b);cmd->ResourceBarrier(1,&x);}
bool D3D12Renderer::WaitForFrameSlot(uint32_t slot){
    if(slot>=FrameCount||!m_fence||!m_fenceEvent)return false;
    const uint64_t v=m_frameFence[slot];
    if(v && m_fence->GetCompletedValue()<v){
        if(FAILED(m_fence->SetEventOnCompletion(v,m_fenceEvent)))return false;
        WaitForSingleObject(m_fenceEvent,INFINITE);
    }
    return true;
}
void D3D12Renderer::SignalFrameSlot(uint32_t slot){
    if(slot>=FrameCount||!m_queue||!m_fence)return;
    const uint64_t v=++m_fenceValue;
    if(SUCCEEDED(m_queue->Signal(m_fence.Get(),v)))m_frameFence[slot]=v;
}
// Output stage. With DLSS SR the neural result (or the plain input, with the
// neural pass off) at render size is reconstructed to the output size; the SR
// runtime reads the guides in the states the neural pass gets them in. Without
// SR, render == output and the picture is copied over. False = m_dlssOutput
// holds nothing for this frame and the present shows the render-size input.
bool D3D12Renderer::RecordOutput(ID3D12GraphicsCommandList*cmd,bool used,bool temporalReset,float jitterX,float jitterY){
    if(m_srActive&&m_sr&&m_sr->FeatureReady()){
        if(used)Barrier(cmd,m_nrLinear.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(!m_outputInUAV)Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        SuperRes::Inputs in;
        in.color=used?m_nrLinear.Get():m_dlssColor.Get();
        in.output=m_dlssOutput.Get();in.depth=m_depth.Get();
        // Always the measured field: SR needs real motion to accumulate history
        // (Motion = Zero is a neural-pass measure, honoured at the NR bind only).
        // A frame without a valid predecessor still gets zero vectors.
        in.motion=m_motionInvalid?m_motionZero.Get():m_motion.Get();in.bias=m_biasCurrent.Get();
        // The convert pass sampled the source at +j: matched reports the offset
        // DLSS has to undo (-j), legacy the historical wrong sign, zero nothing.
        const float sgn=m_jitterMode==JitterMode::Matched?-1.0f:(m_jitterMode==JitterMode::Legacy?1.0f:0.0f);
        in.jitterX=sgn*jitterX;in.jitterY=sgn*jitterY;
        in.frameTimeMs=m_frameTimeMs;in.reset=temporalReset;
        const bool ok=m_sr->Evaluate(cmd,in);
        {ID3D12DescriptorHeap*heaps[]={m_srvHeap.Get()};cmd->SetDescriptorHeaps(1,heaps);}
        cmd->SetGraphicsRootSignature(m_rootSig.Get());cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        if(used)Barrier(cmd,m_nrLinear.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        m_outputInUAV=false;
        return ok;
    }
    if(!used)return false;
    Barrier(cmd,m_nrLinear.Get(),D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(cmd,m_dlssOutput.Get(),m_outputInUAV?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
    cmd->CopyResource(m_dlssOutput.Get(),m_nrLinear.Get());
    Barrier(cmd,m_dlssOutput.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Barrier(cmd,m_nrLinear.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_outputInUAV=false;
    return true;
}

const char* D3D12Renderer::SuperResQuality()const{return m_srActive&&m_sr?m_sr->QualityName():"";}

void D3D12Renderer::LogDebugLayerMessages(){
    if(!m_infoQueue)return;
    const UINT64 n=m_infoQueue->GetNumStoredMessages();
    for(UINT64 i=0;i<n;++i){
        SIZE_T len=0;
        if(FAILED(m_infoQueue->GetMessage(i,nullptr,&len))||!len)continue;
        std::vector<uint8_t> buf(len);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(buf.data());
        if(FAILED(m_infoQueue->GetMessage(i,m,&len)))continue;
        if(m->Severity>D3D12_MESSAGE_SEVERITY_WARNING)continue;   // info/message noise
        LOG("D3D12 debug layer ["<<(m->Severity==D3D12_MESSAGE_SEVERITY_WARNING?"warning":"ERROR")<<"] "<<m->pDescription);
    }
    m_infoQueue->ClearStoredMessages();
}

void D3D12Renderer::WaitGPU(){
    if(!m_queue||!m_fence||!m_fenceEvent)return;
    uint64_t v=++m_fenceValue;m_queue->Signal(m_fence.Get(),v);
    if(m_fence->GetCompletedValue()<v){m_fence->SetEventOnCompletion(v,m_fenceEvent);WaitForSingleObject(m_fenceEvent,INFINITE);}
    LogDebugLayerMessages();
}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::RTV(uint32_t i)const{auto h=m_rtvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_rtvInc;return h;}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::DSV()const{return m_dsvHeap->GetCPUDescriptorHandleForHeapStart();}
D3D12_CPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVCPU(uint32_t i)const{auto h=m_srvHeap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(i)*m_srvInc;return h;}
D3D12_GPU_DESCRIPTOR_HANDLE D3D12Renderer::SRVGPU(uint32_t i)const{auto h=m_srvHeap->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(i)*m_srvInc;return h;}
