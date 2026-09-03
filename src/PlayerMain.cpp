#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#pragma comment(lib,"uxtheme.lib")
#include <mfapi.h>
#include <wrl/client.h>
#include <chrono>
#include <filesystem>
#include <string>
#include <sstream>
#include <algorithm>
#include <memory>
#include <cmath>
#include <utility>
#include <iterator>
#include <cstdint>
#include <vector>
#include <cwctype>
#include <cstdlib>
#include "VideoDecoder.h"
#include "D3D12Renderer.h"
#include "FramePipeline.h"
#include "AudioPlayer.h"
#include "Localization.h"
#include "CubeLUT.h"
#include "LabelStamp.h"
#include "Log.h"
#include "AppIdentity.h"
#include "AppPaths.h"
#include "TextEncoding.h"
#include "ChildProcess.h"

using Clock = std::chrono::steady_clock;
using Microsoft::WRL::ComPtr;
static constexpr int CONTROL_H = 108; // slim player bar: transport + status + timeline
static constexpr int SIDE_W    = 212; // right control panel (grouped functions)

static const wchar_t* kVideoPatterns =
    L"*.mp4;*.m4v;*.mov;*.mkv;*.webm;*.avi;*.wmv;*.asf;*.flv;*.f4v;"
    L"*.ts;*.m2ts;*.mts;*.mpg;*.mpeg;*.mpe;*.vob;*.ogv;*.ogg;*.3gp;*.3g2;"
    L"*.mxf;*.nut;*.rm;*.rmvb;*.divx;*.dv;*.y4m;*.ivf;*.hevc;*.h265;*.h264;*.264;*.av1;*.vp9;"
    // Still images decode as one-frame videos through the same FFmpeg pipe and
    // run through the full pipeline; Frame exports the processed result.
    L"*.png;*.jpg;*.jpeg;*.webp;*.bmp;*.tif;*.tiff;*.avif;*.jxl";

enum : UINT {
    IDM_OPEN=100, IDM_EXIT,
    IDM_PLAY=200, IDM_STOP, IDM_BACK10, IDM_FWD10, IDM_MUTE,
    IDM_DLSS=300, IDM_VIEW_FINAL, IDM_VIEW_INPUT, IDM_VIEW_MV, IDM_VIEW_DEPTH, IDM_VIEW_MASK, IDM_DEPTH_MODE,
    IDM_LUT_LOAD=320, IDM_LUT_CLEAR, IDM_MOTION_ZERO=324, IDM_MOTION_GLOBAL, IDM_MOTION_EST,
    IDM_FX_LUT=340, IDM_FX_SHARPEN, IDM_FX_TONE, IDM_FX_FLOW, IDM_FX_DEPTHMAP, IDM_FX_MASK, IDM_FX_BYPASS, IDM_FX_LANCZOS4K, IDM_HELP=600, IDM_PANEL_LEFT,
    IDM_NRMODEL_A=350, IDM_NRMODEL_B, IDM_NRMODEL_C, IDM_SHOT,
    IDM_QUALITY_AUTO=330, IDM_QUALITY_QUALITY, IDM_QUALITY_BALANCED, IDM_QUALITY_PERFORMANCE, IDM_QUALITY_ULTRAPERF, IDM_QUALITY_DLAA,
    IDM_ASPECT_FIT=400, IDM_ASPECT_FILL, IDM_FULLSCREEN,
    IDM_LANG_BASE=500
};


static constexpr int HK_PLAY_PAUSE = 9001;
static constexpr int HK_BACK_10 = 9002;
static constexpr int HK_FORWARD_10 = 9003;
static constexpr int HK_MUTE = 9004;
static constexpr int HK_DLSS = 9005;
static constexpr int HK_MEDIA_PLAY_PAUSE = 9006;


// How small the movie is decoded relative to the output before the neural pass
// redraws it at output size. This used to be an NGX quality enum handed to the
// renderer, but the neural pass runs 1:1 and takes no mode: the decode scale is
// the only thing the setting ever chose. The names stay the DLSS ones because
// that is what the menu, the CLI and every other DLSS tool call these ratios.
enum class DecodeScale { Native, Quality, Balanced, Performance, UltraPerformance };

struct AppOptions {
    uint32_t maxW=3840, maxH=2160;
    DecodeScale scale=DecodeScale::Quality;
    bool scaleExplicit=false;
    std::wstring file;
    std::wstring exportPath; // when set, run headless: --input file -> --export exportPath
};

static AppOptions ParseArgs() {
    AppOptions o; int argc=0; LPWSTR* argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    if (!argv) return o;
    for(int i=1;i<argc;++i) {
        std::wstring a=argv[i];
        if(a==L"--output" && i+1<argc) {
            std::wstring v=argv[++i]; auto x=v.find(L'x'); if(x==std::wstring::npos) x=v.find(L'X');
            if(x!=std::wstring::npos) { o.maxW=std::max(64,_wtoi(v.substr(0,x).c_str())); o.maxH=std::max(64,_wtoi(v.substr(x+1).c_str())); }
        } else if(a==L"--quality" && i+1<argc) {
            std::wstring q=argv[++i]; std::transform(q.begin(),q.end(),q.begin(),::towlower);
            if(q==L"auto") { o.scaleExplicit=false; }
            else {
                o.scaleExplicit=true;
                if(q==L"performance"||q==L"perf") o.scale=DecodeScale::Performance;
                else if(q==L"balanced") o.scale=DecodeScale::Balanced;
                else if(q==L"ultra-performance"||q==L"ultraperf") o.scale=DecodeScale::UltraPerformance;
                else if(q==L"dlaa") o.scale=DecodeScale::Native;
                else o.scale=DecodeScale::Quality;
            }
        } else if(a==L"--input" && i+1<argc) {
            o.file=argv[++i];
        } else if(a==L"--export" && i+1<argc) {
            o.exportPath=argv[++i];
        } else if(!a.empty() && a[0]!=L'-') o.file=a;
    }
    LocalFree(argv); return o;
}

static std::wstring PickVideoFileFallback(HWND owner, const Localizer& loc) {
    wchar_t path[32768]{};
    std::wstring filter;
    filter += loc.Get(L"dialog.all_ffmpeg"); filter.push_back(L'\0');
    filter += L"*.*"; filter.push_back(L'\0');
    filter += loc.Get(L"dialog.supported"); filter.push_back(L'\0');
    filter += kVideoPatterns; filter.push_back(L'\0');
    filter += loc.Get(L"dialog.all"); filter.push_back(L'\0');
    filter += L"*.*"; filter.push_back(L'\0'); filter.push_back(L'\0');
    const std::wstring title = loc.Get(L"dialog.title");
    OPENFILENAMEW o{}; o.lStructSize=sizeof(o); o.hwndOwner=owner; o.lpstrFile=path; o.nMaxFile=static_cast<DWORD>(std::size(path));
    o.lpstrFilter=filter.c_str(); o.nFilterIndex=1; o.lpstrTitle=title.c_str();
    o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_EXPLORER|OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&o)?path:L"";
}

static std::wstring PickVideoFile(HWND owner, const Localizer& loc) {
    ComPtr<IFileOpenDialog> dlg;
    HRESULT hr=CoCreateInstance(CLSID_FileOpenDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dlg));
    if(SUCCEEDED(hr) && dlg) {
        const std::wstring allFfmpeg=loc.Get(L"dialog.all_ffmpeg"), supported=loc.Get(L"dialog.supported"), all=loc.Get(L"dialog.all"), title=loc.Get(L"dialog.title");
        COMDLG_FILTERSPEC specs[3]={{allFfmpeg.c_str(),L"*.*"},{supported.c_str(),kVideoPatterns},{all.c_str(),L"*.*"}};
        dlg->SetFileTypes(3,specs); dlg->SetFileTypeIndex(1); dlg->SetTitle(title.c_str());
        FILEOPENDIALOGOPTIONS opts{}; if(SUCCEEDED(dlg->GetOptions(&opts))) dlg->SetOptions(opts|FOS_FORCEFILESYSTEM|FOS_FILEMUSTEXIST|FOS_PATHMUSTEXIST);
        hr=dlg->Show(owner);
        if(hr==HRESULT_FROM_WIN32(ERROR_CANCELLED)) return L"";
        if(SUCCEEDED(hr)) {
            ComPtr<IShellItem> item; if(SUCCEEDED(dlg->GetResult(&item)) && item) {
                PWSTR p=nullptr; if(SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH,&p)) && p) {
                    std::wstring result(p); CoTaskMemFree(p); return result;
                }
            }
        }
    }
    return PickVideoFileFallback(owner,loc);
}

static std::wstring TimeText(double sec) {
    if(!std::isfinite(sec)||sec<0) sec=0; int s=int(sec+0.5),h=s/3600; s%=3600; int m=s/60; s%=60; wchar_t b[64];
    if(h) swprintf_s(b,L"%d:%02d:%02d",h,m,s); else swprintf_s(b,L"%02d:%02d",m,s); return b;
}

// Nordic-botanical palette: moss-slate ground, warm grey-green ink, one fern
// accent and one dried-grass amber reserved for busy/working states. Type
// (Bricolage Grotesque + IBM Plex Sans) follows the project's artifact styling.
namespace Pal {
    // Flat, minimal: two grounds (content, sidebar), one pill fill, one green.
    // No hairlines anywhere - spacing separates, the accent signals state.
    constexpr COLORREF Bg         = RGB(21,26,23);    // #151A17 content ground (video column)
    constexpr COLORREF Surface    = RGB(27,34,30);    // #1B221E sidebar ground
    constexpr COLORREF Raised     = RGB(37,45,40);    // #252D28 pills
    constexpr COLORREF Hover      = RGB(47,57,51);    // #2F3933 pill hover
    constexpr COLORREF Line       = RGB(42,52,46);    // #2A342E slider / timeline tracks
    constexpr COLORREF Ink        = RGB(233,237,234); // #E9EDEA primary text
    constexpr COLORREF Ink2       = RGB(176,187,180); // #B0BBB4 pill labels
    constexpr COLORREF Muted      = RGB(119,132,125); // #77847D section labels, status
    constexpr COLORREF Accent     = RGB(94,192,135);  // #5EC087 green: active state, progress
    constexpr COLORREF AccentSoft = RGB(27,53,39);    // #1B3527 active pill fill
    constexpr COLORREF Amber      = RGB(217,160,53);  // #D9A035 busy text
    constexpr COLORREF AmberSoft  = RGB(63,50,24);    // #3F3218 busy fill
    // Quiet color-coding for button categories in the control panel.
    constexpr COLORREF BlueInk    = RGB(139,178,214); // Picture / DLSS controls
    constexpr COLORREF VioletInk  = RGB(187,168,219); // Export & Jobs
    constexpr COLORREF GreenInk   = RGB(150,205,160); // Effects
    constexpr COLORREF TealInk    = RGB(126,196,196); // Motion
    constexpr COLORREF SandInk    = RGB(214,196,150); // Inspect
}

// ---- Self-provisioning payload ---------------------------------------------
// The exe carries its runtime set (NGX dlls + ffmpeg/ffprobe + the headless
// exporter + UI fonts) as RCDATA resources (src/SmackMyRezUp.rc - ids must match
// this table) and extracts whatever is MISSING beside the exe at startup.
// Third-party files are never overwritten, so manual swaps (e.g. a different
// nvngx_dlssnr build or ffmpeg) always survive. The exporter and the fonts are
// this project's own and must match the player that launches them, so those
// ARE replaced when the copy on disk differs in size from the embedded one -
// otherwise a new player dropped beside an old install keeps driving a stale
// exporter with flags it does not understand.
// ---- One-line text prompt ---------------------------------------------------
// A modal "label + edit + Generate/Cancel" box built from an in-memory dialog
// template, so it needs no resource-script entry. Returns false on Cancel.
struct PromptState{const wchar_t* title;const wchar_t* label;std::wstring text;const wchar_t* choiceLabel;const std::vector<std::wstring>* choices;int choice;};
static INT_PTR CALLBACK PromptProc(HWND h,UINT m,WPARAM w,LPARAM l){
    if(m==WM_INITDIALOG){
        auto* st=reinterpret_cast<PromptState*>(l);SetWindowLongPtrW(h,GWLP_USERDATA,l);
        SetWindowTextW(h,st->title);SetDlgItemTextW(h,100,st->label);SetDlgItemTextW(h,101,st->text.c_str());
        if(st->choices){
            SetDlgItemTextW(h,102,st->choiceLabel);
            for(const auto& c:*st->choices)SendDlgItemMessageW(h,103,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(c.c_str()));
            SendDlgItemMessageW(h,103,CB_SETCURSEL,WPARAM(std::clamp(st->choice,0,int(st->choices->size())-1)),0);
        } else {ShowWindow(GetDlgItem(h,102),SW_HIDE);ShowWindow(GetDlgItem(h,103),SW_HIDE);}
        SendDlgItemMessageW(h,101,EM_SETSEL,0,-1);SetFocus(GetDlgItem(h,101));return FALSE;
    }
    if(m==WM_COMMAND){
        const int id=LOWORD(w);
        if(id==IDOK){auto* st=reinterpret_cast<PromptState*>(GetWindowLongPtrW(h,GWLP_USERDATA));wchar_t buf[1024]{};GetDlgItemTextW(h,101,buf,1024);st->text=buf;
            if(st->choices)st->choice=int(SendDlgItemMessageW(h,103,CB_GETCURSEL,0,0));EndDialog(h,IDOK);return TRUE;}
        if(id==IDCANCEL){EndDialog(h,IDCANCEL);return TRUE;}
    }
    return FALSE;
}
// Text prompt with an optional dropdown (choices/choice): the dropdown row is
// present in the template and simply hidden when no choices are given.
static bool PromptText(HWND owner,const wchar_t* title,const wchar_t* label,std::wstring& text,
                       const wchar_t* choiceLabel=nullptr,const std::vector<std::wstring>* choices=nullptr,int* choice=nullptr){
    // DLGTEMPLATE, then DWORD-aligned DLGITEMTEMPLATEs; sizes in dialog units.
    std::vector<WORD> t;
    auto w=[&](WORD v){t.push_back(v);};
    auto dw=[&](DWORD v){w(WORD(v&0xFFFFu));w(WORD(v>>16));};
    auto str=[&](const wchar_t* s){do{w(WORD(*s));}while(*s++);};
    auto align=[&](){while(t.size()%2)w(0);};
    const bool hasChoice=choices&&!choices->empty();
    const WORD dlgH=hasChoice?86:64,btnY=hasChoice?64:42;
    dw(DS_MODALFRAME|DS_SETFONT|DS_CENTER|WS_POPUP|WS_CAPTION|WS_SYSMENU);dw(0);w(6);w(0);w(0);w(300);w(dlgH);
    w(0);w(0);str(L"");w(9);str(L"Segoe UI");
    auto item=[&](DWORD style,short x,short y,short cx,short cy,WORD id,WORD cls,const wchar_t* txt){
        align();dw(WS_CHILD|WS_VISIBLE|style);dw(0);w(WORD(x));w(WORD(y));w(WORD(cx));w(WORD(cy));w(id);w(0xFFFF);w(cls);str(txt);w(0);};
    item(SS_LEFT,8,7,284,10,100,0x0082,L"");                                   // label
    item(WS_BORDER|WS_TABSTOP|ES_AUTOHSCROLL,8,20,284,13,101,0x0081,L"");     // edit
    item(SS_LEFT,8,44,60,10,102,0x0082,L"");                                   // dropdown label
    item(CBS_DROPDOWNLIST|WS_VSCROLL|WS_TABSTOP,70,41,222,80,103,0x0085,L"");  // dropdown (cy includes the list)
    item(BS_DEFPUSHBUTTON|WS_TABSTOP,186,btnY,52,14,IDOK,0x0080,L"Generate");
    item(BS_PUSHBUTTON|WS_TABSTOP,242,btnY,50,14,IDCANCEL,0x0080,L"Cancel");
    PromptState st{title,label,text,choiceLabel?choiceLabel:L"",hasChoice?choices:nullptr,choice?*choice:0};
    const INT_PTR r=DialogBoxIndirectParamW(GetModuleHandleW(nullptr),reinterpret_cast<LPCDLGTEMPLATEW>(t.data()),owner,PromptProc,reinterpret_cast<LPARAM>(&st));
    if(r!=IDOK)return false;
    text=st.text;if(choice&&hasChoice)*choice=st.choice;return true;
}

struct PayloadEntry{int id;const wchar_t* rel;bool refresh;};
static constexpr PayloadEntry kPayload[]={
    // 9003 (nvngx_dlss.dll, DLSS SR) was dropped: the direct-NR pipeline
    // upscales with a resize before the 1:1 neural pass and never creates the
    // SR feature, so the 59 MB runtime was dead weight in the exe.
    {9004,L"nvngx_dlssnr.dll",false},
    {9014,L"ffmpeg.exe",false},{9015,L"ffprobe.exe",false},{9016,smru::kExporterExeW,true},
    {9017,L"fonts\\BricolageGrotesque.ttf",true},{9018,L"fonts\\IBMPlexSans-Regular.ttf",true},
    {9019,L"help.html",true},   // the manual (docs/help.html), opened by Help / F1
    // The Python guide generators: refreshed with the player, since the flags
    // it passes them must match. AppPaths finds tools\ beside the exe first.
    {9020,L"tools\\smru_env.py",true},{9021,L"tools\\make_flow_video.py",true},
    {9022,L"tools\\make_depth_video.py",true},{9023,L"tools\\make_mask_video.py",true},
    {9024,L"tools\\run_seedvr.py",true},{9025,L"tools\\depth_probe.py",true},
    {9026,L"tools\\trt_runtime.py",true},
    // Sample grades: extracted once and left alone, so an edited copy survives.
    {9030,L"luts\\identity.cube",false},{9031,L"luts\\cine_teal_orange.cube",false},
    {9032,L"luts\\cine_film_warm.cube",false},{9033,L"luts\\cine_bleach_bypass.cube",false},
};

static int ProvisionPayload(){
    const std::filesystem::path dir=smru::paths::ExeDirectory();
    int extracted=0;
    for(const auto& pf:kPayload){
        std::error_code ec;
        const std::filesystem::path dst=dir/pf.rel;
        const bool present=std::filesystem::exists(dst,ec);
        if(present&&!pf.refresh)continue;
        HRSRC hr=FindResourceW(nullptr,MAKEINTRESOURCEW(pf.id),RT_RCDATA);
        if(!hr)continue; // built without the payload (dev build) - just skip
        HGLOBAL hg=LoadResource(nullptr,hr);
        const void* data=hg?LockResource(hg):nullptr;
        const DWORD sz=SizeofResource(nullptr,hr);
        if(!data||!sz)continue;
        if(present&&std::filesystem::file_size(dst,ec)==sz){
            // Same size is not the same build: two exporter builds a day apart
            // came out at exactly 356,864 bytes. Compare the bytes (at most a
            // few hundred KB for the refreshable entries) before deciding.
            std::vector<char> cur(sz);
            HANDLE rf=CreateFileW(dst.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
            bool same=false;
            if(rf!=INVALID_HANDLE_VALUE){DWORD rd=0;same=ReadFile(rf,cur.data(),sz,&rd,nullptr)&&rd==sz&&memcmp(cur.data(),data,sz)==0;CloseHandle(rf);}
            if(same)continue;
        }
        std::filesystem::create_directories(dst.parent_path(),ec);
        // CREATE_ALWAYS only for refreshable entries; a locked file (an exporter
        // still running) just fails the open and is left alone.
        HANDLE f=CreateFileW(dst.c_str(),GENERIC_WRITE,0,nullptr,pf.refresh?CREATE_ALWAYS:CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(f==INVALID_HANDLE_VALUE)continue; // raced/locked - leave it be
        DWORD wr=0;const BOOL okw=WriteFile(f,data,sz,&wr,nullptr);CloseHandle(f);
        if(!okw||wr!=sz){DeleteFileW(dst.c_str());continue;}
        ++extracted;
    }
    return extracted;
}

// ---- Dark menus ------------------------------------------------------------
// Popup menus: uxtheme ordinal 135 SetPreferredAppMode(ForceDark) + 136
// FlushMenuThemes (undocumented but stable since Win10 1809). The menu BAR
// ignores app dark mode, so it is painted by hand via the WM_UAHDRAWMENU*
// messages the system sends when UAH owner-draw kicks in.
#define WM_UAHDRAWMENU 0x0091
#define WM_UAHDRAWMENUITEM 0x0092
typedef union { struct {DWORD cx,cy;} rgsizeBar[2]; struct {DWORD cx,cy;} rgsizePopup[4]; } UAHMENUITEMMETRICS;
typedef struct { DWORD rgcx[4]; DWORD fUpdateMaxWidths:2; } UAHMENUPOPUPMETRICS;
typedef struct { HMENU hmenu; HDC hdc; DWORD dwFlags; } UAHMENU;
typedef struct { int iPosition; UAHMENUITEMMETRICS umim; UAHMENUPOPUPMETRICS umpm; } UAHMENUITEM;
typedef struct { DRAWITEMSTRUCT dis; UAHMENU um; UAHMENUITEM umi; } UAHDRAWMENUITEM;

static void EnableDarkMenus(){
    if(HMODULE ux=LoadLibraryW(L"uxtheme.dll")){
        using SetModeFn=int(WINAPI*)(int);
        using VoidFn=void(WINAPI*)();
        if(auto f=reinterpret_cast<SetModeFn>(GetProcAddress(ux,MAKEINTRESOURCEA(135))))f(2); // ForceDark
        if(auto f=reinterpret_cast<VoidFn>(GetProcAddress(ux,MAKEINTRESOURCEA(136))))f();    // FlushMenuThemes
    }
}

class PlayerApp {
public:
    explicit PlayerApp(AppOptions o):m_opt(std::move(o)){}
    ~PlayerApp(){SaveVideoSettings();UnregisterOverlayHotkeys();Unload(); if(m_font)DeleteObject(m_font); if(m_fontSmall)DeleteObject(m_fontSmall); if(m_fontTitle)DeleteObject(m_fontTitle); if(m_fontHead)DeleteObject(m_fontHead); if(m_fontIcon)DeleteObject(m_fontIcon); if(m_exportProc)CloseHandle(m_exportProc); if(m_exportErrRead)CloseHandle(m_exportErrRead); if(m_flowProc)CloseHandle(m_flowProc); if(m_depthMapProc)CloseHandle(m_depthMapProc); if(m_maskProc)CloseHandle(m_maskProc); if(m_svrProc)CloseHandle(m_svrProc); if(m_svrOutRead)CloseHandle(m_svrOutRead); /* running background jobs keep going detached */}

    bool Create(HINSTANCE hi) {
        m_loc.Initialize();
        LoadVideoSettings();
        INITCOMMONCONTROLSEX icc{sizeof(icc),ICC_BAR_CLASSES};InitCommonControlsEx(&icc);
        WNDCLASSW r{}; r.style=CS_DBLCLKS|CS_OWNDC; r.lpfnWndProc=RenderWndProcStatic; r.hInstance=hi; r.lpszClassName=smru::kWndClassRenderSurface; r.hCursor=LoadCursor(nullptr,IDC_ARROW); r.hbrBackground=nullptr; RegisterClassW(&r);
        WNDCLASSW v{}; v.lpfnWndProc=ViewportWndProcStatic; v.hInstance=hi; v.lpszClassName=smru::kWndClassViewport; v.hCursor=LoadCursor(nullptr,IDC_ARROW); v.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH); RegisterClassW(&v);
        WNDCLASSW w{}; w.lpfnWndProc=WndProcStatic; w.hInstance=hi; w.lpszClassName=smru::kWndClassMain; w.hCursor=LoadCursor(nullptr,IDC_ARROW); w.hbrBackground=CreateSolidBrush(Pal::Bg); RegisterClassW(&w);
        RECT rc{0,0,1440,880}; AdjustWindowRect(&rc,WS_OVERLAPPEDWINDOW,TRUE);
        const std::wstring appTitle=m_loc.Get(L"app.title");
        m_hwnd=CreateWindowExW(WS_EX_ACCEPTFILES,w.lpszClassName,appTitle.c_str(),WS_OVERLAPPEDWINDOW|WS_VISIBLE|WS_CLIPCHILDREN,CW_USEDEFAULT,CW_USEDEFAULT,rc.right-rc.left,rc.bottom-rc.top,nullptr,CreateMenuBar(),hi,this);
        if(!m_hwnd) return false;
        RegisterOverlayHotkeys();
        BOOL dark=TRUE; DwmSetWindowAttribute(m_hwnd,20,&dark,sizeof(dark)); DWORD corner=2; DwmSetWindowAttribute(m_hwnd,33,&corner,sizeof(corner));
        // Custom chrome (Win11; silently ignored on Win10): caption painted in the
        // app background so the titlebar blends into the UI, border in the panel
        // line color, caption text in the UI ink.
        {COLORREF cap=Pal::Bg;DwmSetWindowAttribute(m_hwnd,35,&cap,sizeof(cap));
         COLORREF bord=Pal::Line;DwmSetWindowAttribute(m_hwnd,34,&bord,sizeof(bord));
         COLORREF txt=Pal::Ink;DwmSetWindowAttribute(m_hwnd,36,&txt,sizeof(txt));}
        m_viewport=CreateWindowExW(0,v.lpszClassName,nullptr,WS_CHILD|WS_CLIPCHILDREN|WS_CLIPSIBLINGS,0,0,100,100,m_hwnd,nullptr,hi,nullptr);
        m_renderWnd=CreateWindowExW(WS_EX_ACCEPTFILES,smru::kWndClassRenderSurface,nullptr,WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS,0,0,100,100,m_viewport,nullptr,hi,this);
        // Nordic type: Bricolage Grotesque headings + IBM Plex Sans UI, loaded
        // privately from fonts\ beside the exe (nothing installed system-wide);
        // Segoe UI stays the fallback when a file is missing.
        const std::filesystem::path fdir=smru::paths::ExeDirectory()/L"fonts";
        auto loadFont=[&](const wchar_t* file){auto p=fdir/file;return std::filesystem::exists(p)&&AddFontResourceExW(p.c_str(),FR_PRIVATE,nullptr)>0;};
        const wchar_t* uiFace=loadFont(L"IBMPlexSans-Regular.ttf")?L"IBM Plex Sans":L"Segoe UI";
        const wchar_t* headFace=loadFont(L"BricolageGrotesque.ttf")?L"Bricolage Grotesque":L"Segoe UI";
        m_font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,uiFace);
        m_fontSmall=CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,uiFace);
        m_fontTitle=CreateFontW(-42,0,0,0,FW_BOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,headFace);
        m_fontHead=CreateFontW(-18,0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,headFace);
        // Toolbar glyphs: Segoe Fluent Icons (Win11) with the MDL2 fallback (Win10);
        // both fonts share the same codepoints, so the glyph table below works on either.
        {auto makeIcon=[&](const wchar_t* face){return CreateFontW(-14,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH|FF_DONTCARE,face);};
         auto hasGlyph=[&](HFONT f)->bool{HDC dc=GetDC(m_hwnd);auto of=SelectObject(dc,f);wchar_t ch=0xE768;WORD gi=0;GetGlyphIndicesW(dc,&ch,1,&gi,GGI_MARK_NONEXISTING_GLYPHS);SelectObject(dc,of);ReleaseDC(m_hwnd,dc);return gi!=0xFFFF;};
         m_fontIcon=makeIcon(L"Segoe Fluent Icons");
         if(!hasGlyph(m_fontIcon)){DeleteObject(m_fontIcon);m_fontIcon=makeIcon(L"Segoe MDL2 Assets");}
         if(!hasGlyph(m_fontIcon)){DeleteObject(m_fontIcon);m_fontIcon=nullptr;}}
        DragAcceptFiles(m_hwnd,TRUE); DragAcceptFiles(m_renderWnd,TRUE); ShowWindow(m_viewport,SW_HIDE); Layout(); UpdateTitle();
        if(!m_opt.file.empty()) Load(m_opt.file); // No startup file picker: the player opens idle by default.
        return true;
    }

    void Tick() {
        PollExport();
        PollFlowGen();
        PollDepthMapGen();
        PollMaskGen();
        PollSeedVR();
        if(m_seekPending) {
            const double target=m_pendingSeekSec; const bool resume=m_seekResumePlaying;
            m_seekPending=false; PerformSeek(target,resume); return;
        }
        // Loop: when playback ran off the end (no next frame), restart from 0.
        // Keeps short clips cycling so the lazily arming NR runtime becomes
        // visible in the preview without any user action.
        if(m_loop&&m_loaded&&!m_playing&&!m_haveNext&&!m_seeking&&m_decoder.DurationSeconds()>0){
            RequestSeek(0,true);return;
        }
        if(m_loaded&&!m_playing&&!m_seeking&&m_renderer&&!m_compareDrag&&!m_zoomPan&&!m_zoomRectDrag){
            // Interactive drags present themselves at mouse-move rate; skip the
            // capped tick present then so there is no redundant double-present.
            const auto nowClock=Clock::now();
            if(std::chrono::duration<double>(nowClock-m_lastStaticPresent).count()>=1.0/60.0){
                m_renderer->PresentCurrent();
                m_lastStaticPresent=nowClock;
            }
        }
        if(!m_loaded||!m_playing||!m_haveNext||m_seeking) return;
        double now=Position(); const double frameDur=1.0/std::max(1.0,m_decoder.FrameRate());
        bool dropped=false;
        while(m_haveNext) {
            double due=double(m_next.timestamp100ns)*1e-7;
            if(now-due <= std::max(0.085,frameDur*2.25)) break;
            VideoFrame skip=std::move(m_next); (void)skip; ++m_droppedFrames; dropped=true;
            if(!m_decoder.ReadNext(m_next)){m_haveNext=false;break;}
        }
        if(dropped&&m_pipeline){m_pipeline->ResetHistory();m_guideReset=true;m_dlssReset=true;}
        if(!m_haveNext){m_playing=false;m_audio.Pause(true);InvalidateRect(m_hwnd,nullptr,FALSE);return;}
        double due=double(m_next.timestamp100ns)*1e-7;
        if(now+0.001<due) return;
        if(RenderVideoFrame(m_next,m_next.discontinuity||m_guideReset)) {
            ++m_fpsWindowFrames;
            const auto fpsNow=Clock::now();
            const double fpsElapsed=std::chrono::duration<double>(fpsNow-m_fpsWindowStart).count();
            if(fpsElapsed>=0.75){m_submitFps=double(m_fpsWindowFrames)/fpsElapsed;m_fpsWindowFrames=0;m_fpsWindowStart=fpsNow;}
        }
        m_currentSec=due; m_guideReset=false; m_dlssReset=false;
        if(!m_decoder.ReadNext(m_next)){m_haveNext=false;m_playing=false;m_audio.Pause(true);}
        if((++m_uiTick%15)==0) UpdateTitle();
        InvalidateRect(m_hwnd,nullptr,FALSE);
    }

    bool Running()const{return m_running;}
    bool NeedsRealtimeTick()const{return m_loaded;}
    DWORD TickSleepMs()const{return (m_loaded&&!m_playing&&!m_seekPending&&!m_seeking)?8u:0u;}


private:
    std::wstring T(const wchar_t* key)const{return m_loc.Get(key);}

    std::filesystem::path SettingsPath()const{return smru::paths::SettingsFile();}

    float ReadIniFloat(const wchar_t* section,const wchar_t* key,float fallback)const{
        wchar_t def[64]{},buf[128]{};swprintf_s(def,L"%.6f",fallback);
        const auto path=SettingsPath();GetPrivateProfileStringW(section,key,def,buf,static_cast<DWORD>(std::size(buf)),path.c_str());
        wchar_t* end=nullptr;double v=wcstod(buf,&end);return (end&&end!=buf&&std::isfinite(v))?float(v):fallback;
    }

    void WriteIniFloat(const wchar_t* section,const wchar_t* key,float value)const{
        wchar_t buf[64]{};swprintf_s(buf,L"%.6f",value);const auto path=SettingsPath();WritePrivateProfileStringW(section,key,buf,path.c_str());
    }

    // Pipeline/UI state lives in [SmackMyRezUp]. Values an older build wrote to
    // the legacy [DLSS] section are read as a fallback and migrate on the next
    // save, so nobody loses their setup when upgrading.
    bool TryReadIniFloat(const wchar_t* section,const wchar_t* key,float& out)const{
        wchar_t buf[128]{};const auto path=SettingsPath();
        GetPrivateProfileStringW(section,key,L"",buf,static_cast<DWORD>(std::size(buf)),path.c_str());
        if(!buf[0])return false;
        wchar_t* end=nullptr;const double v=wcstod(buf,&end);
        if(!(end&&end!=buf&&std::isfinite(v)))return false;
        out=float(v);return true;
    }
    float ReadSetting(const wchar_t* key,float fallback)const{
        float v=fallback;
        if(TryReadIniFloat(smru::kSettingsSection,key,v))return v;
        if(TryReadIniFloat(smru::kLegacySettingsSection,key,v))return v;
        return fallback;
    }
    std::wstring ReadSettingString(const wchar_t* key)const{
        for(const wchar_t* section:{smru::kSettingsSection,smru::kLegacySettingsSection}){
            const std::wstring v=smru::paths::SettingsString(section,key);
            if(!v.empty())return v;
        }
        return {};
    }
    void WriteSetting(const wchar_t* key,float value)const{WriteIniFloat(smru::kSettingsSection,key,value);}

    void LoadVideoSettings(){
        m_colorSettings.brightness=std::clamp(ReadIniFloat(smru::kAdjustmentsSection,L"Brightness",0.0f),-2.0f,2.0f);
        m_colorSettings.contrast=std::clamp(ReadIniFloat(smru::kAdjustmentsSection,L"Contrast",1.0f),0.0f,3.0f);
        m_colorSettings.saturation=std::clamp(ReadIniFloat(smru::kAdjustmentsSection,L"Saturation",1.0f),0.0f,3.0f);
        m_colorSettings.gamma=std::clamp(ReadIniFloat(smru::kAdjustmentsSection,L"Gamma",1.0f),0.25f,3.0f);
        m_colorSettings.temperature=std::clamp(ReadIniFloat(smru::kAdjustmentsSection,L"Temperature",0.0f),-1.0f,1.0f);
        m_colorSettings.tint=std::clamp(ReadIniFloat(smru::kAdjustmentsSection,L"Tint",0.0f),-1.0f,1.0f);
        m_toneMix=std::clamp(ReadSetting(L"ToneMix",0.0f),0.0f,1.0f);
        m_sharpen=std::clamp(ReadSetting(L"Sharpen",0.0f),0.0f,1.0f);
        m_postSharpen=std::clamp(ReadSetting(L"PostSharpen",0.0f),0.0f,1.0f);
        m_lanczos4K=ReadSetting(L"Lanczos4K",0.0f)>0.5f;
        m_lutStrength=std::clamp(ReadSetting(L"LutStrength",1.0f),0.0f,1.0f);
        m_svrStrength=std::clamp(ReadSetting(L"SvrStrength",0.7f),0.0f,1.0f);
        m_motionMode=std::clamp(int(ReadSetting(L"Motion",0.0f)),0,2);
        m_fxLut=ReadSetting(L"FxLut",1.0f)>0.5f;
        m_fxSharpen=ReadSetting(L"FxSharpen",1.0f)>0.5f;
        m_fxTone=ReadSetting(L"FxTone",1.0f)>0.5f;
        m_fxFlow=ReadSetting(L"FxFlow",1.0f)>0.5f;
        m_fxDepth=ReadSetting(L"FxDepth",1.0f)>0.5f;
        // Off by default: binding ControlMask changes the output on its own
        // (measured), so it is an explicit A/B decision, never a surprise.
        m_fxMask=ReadSetting(L"FxMask",0.0f)>0.5f;
        m_maskPrompt=ReadSettingString(L"MaskPrompt");
        m_panelLeft=ReadSetting(L"PanelLeft",0.0f)>0.5f;
        m_maskSensitivity=std::clamp(int(ReadSetting(L"MaskSensitivity",1.0f)),0,2);
        m_nrSmooth=std::clamp(ReadSetting(L"NrSmooth",0.0f),0.0f,1.0f);
        m_exportRes=std::clamp(int(ReadSetting(L"ExportRes",0.0f)),0,3);
        m_exportCodec=std::clamp(int(ReadSetting(L"ExportCodec",0.0f)),0,2);
        {const int open=int(ReadSetting(L"PanelOpen",49.0f));
         for(int g=0;g<7;++g)m_groupOpen[g]=(open>>g)&1;}
        m_nrModelPick=int(ReadSetting(L"NrModel",-1.0f));if(m_nrModelPick<-1||m_nrModelPick>2)m_nrModelPick=-1;
        m_nrIntensity=std::min(1.0f,std::max(0.0f,ReadSetting(L"NrIntensity",1.0f)));
        m_nrLocalStructure=std::min(2.0f,std::max(0.0f,ReadSetting(L"NrLocalStructure",1.0f)));
        m_nrSkinStructure=std::min(2.0f,std::max(-1.0f,ReadSetting(L"NrSkinStructure",-1.0f)));
        if(m_nrSkinStructure<0.0f)m_nrSkinStructure=-1.0f; // only -1 is the "follow local" sentinel
        m_nrAutoMask=ReadSetting(L"NrAutoMask",1.0f)>0.5f;
        m_lutPath=ReadSettingString(L"LutPath");
    }

    void SaveVideoSettings()const{
        WriteIniFloat(smru::kAdjustmentsSection,L"Brightness",m_colorSettings.brightness);
        WriteIniFloat(smru::kAdjustmentsSection,L"Contrast",m_colorSettings.contrast);
        WriteIniFloat(smru::kAdjustmentsSection,L"Saturation",m_colorSettings.saturation);
        WriteIniFloat(smru::kAdjustmentsSection,L"Gamma",m_colorSettings.gamma);
        WriteIniFloat(smru::kAdjustmentsSection,L"Temperature",m_colorSettings.temperature);
        WriteIniFloat(smru::kAdjustmentsSection,L"Tint",m_colorSettings.tint);
        WriteSetting(L"ToneMix",m_toneMix);
        WriteSetting(L"Sharpen",m_sharpen);
        WriteSetting(L"PostSharpen",m_postSharpen);
        WriteSetting(L"Lanczos4K",m_lanczos4K?1.0f:0.0f);
        WriteSetting(L"LutStrength",m_lutStrength);
        WriteSetting(L"SvrStrength",m_svrStrength);
        WriteSetting(L"Motion",float(m_motionMode));
        WriteSetting(L"FxLut",m_fxLut?1.0f:0.0f);
        WriteSetting(L"FxSharpen",m_fxSharpen?1.0f:0.0f);
        WriteSetting(L"FxTone",m_fxTone?1.0f:0.0f);
        WriteSetting(L"FxFlow",m_fxFlow?1.0f:0.0f);
        WriteSetting(L"FxDepth",m_fxDepth?1.0f:0.0f);
        WriteSetting(L"FxMask",m_fxMask?1.0f:0.0f);
        WriteSetting(L"PanelLeft",m_panelLeft?1.0f:0.0f);
        WriteSetting(L"MaskSensitivity",float(m_maskSensitivity));
        WritePrivateProfileStringW(smru::kSettingsSection,L"MaskPrompt",m_maskPrompt.c_str(),SettingsPath().c_str());
        WriteSetting(L"NrSmooth",m_nrSmooth);
        WriteSetting(L"ExportRes",float(m_exportRes));
        WriteSetting(L"ExportCodec",float(m_exportCodec));
        {int open=0;for(int g=0;g<7;++g)if(m_groupOpen[g])open|=1<<g;
         WriteSetting(L"PanelOpen",float(open));}
        WriteSetting(L"NrModel",float(m_nrModelPick));
        WriteSetting(L"NrIntensity",m_nrIntensity);
        WriteSetting(L"NrLocalStructure",m_nrLocalStructure);
        WriteSetting(L"NrSkinStructure",m_nrSkinStructure);
        WriteSetting(L"NrAutoMask",m_nrAutoMask?1.0f:0.0f);
        WritePrivateProfileStringW(smru::kSettingsSection,L"LutPath",m_lutPath.c_str(),SettingsPath().c_str());
    }

    // The NR knobs as the renderer wants them. -1 ("no pick") means style 0;
    // the renderer fills in reset, the guide bindings and the MV scale itself.
    NeuralEngine::Settings CurrentNRSettings()const{
        return {.style=m_nrModelPick>=0?uint32_t(m_nrModelPick):0u,
                .intensity=m_nrIntensity,.localStructure=m_nrLocalStructure,
                .skinStructure=m_nrSkinStructure,.autoMask=m_nrAutoMask};
    }

    // Effective values honor the per-effect enables and the Bypass-All toggle,
    // so any addition can be A/B'd live (values are kept, only application gates).
    float FxTone()const{return (m_bypassFX||!m_fxTone)?0.0f:m_toneMix;}
    float FxSharpen()const{return (m_bypassFX||!m_fxSharpen)?0.0f:m_sharpen;}
    float FxPostSharpen()const{return m_bypassFX?0.0f:m_postSharpen;}
    float FxNrSmooth()const{return m_bypassFX?0.0f:m_nrSmooth;}
    float FxLutStrength()const{return (m_bypassFX||!m_fxLut)?0.0f:m_lutStrength;}
    bool FxFlowOn()const{return !m_bypassFX&&m_fxFlow;}
    bool FxDepthOn()const{return !m_bypassFX&&m_fxDepth;}
    bool FxMaskOn()const{return !m_bypassFX&&m_fxMask;}

    void ApplyVideoAdjustments(bool refreshPaused=true){
        if(m_renderer){
            m_renderer->SetColorSettings(m_colorSettings);
            m_renderer->SetToneMix(FxTone());
            m_renderer->SetPreSharpen(FxSharpen());
            m_renderer->SetPostSharpen(FxPostSharpen());
            m_renderer->SetNRSmooth(FxNrSmooth());
            m_renderer->SetLUTStrength(FxLutStrength());
            m_renderer->SetExternalFlowEnabled(FxFlowOn());
            m_renderer->SetExternalDepthEnabled(FxDepthOn());
            // ControlMask is bound only on request (MaskNR) and only when a
            // segmentation mask is attached: merely binding it changes the
            // output more than the mask content does (measured), so the A/B
            // is between "mask bound" and "MVec only", never a surprise.
            m_renderer->SetNRGuideMask(1u|((FxMaskOn()&&m_maskLoaded)?4u:0u));
            // Zero motion mode zeroes the FINAL MV field on the GPU too, so an
            // attached _flow.mp4 cannot bypass "zero vectors" into the NR MVec.
            m_renderer->SetMVFieldScale(m_motionMode==0?0.0f:1.0f);
            m_renderer->SetFxBypassIndicator(m_bypassFX);
            // Direct-NR knobs are read per evaluate, so this is fully live.
            m_renderer->SetNRSettings(CurrentNRSettings());
            // While paused, NR/pre-sharpen live in the RENDER pass, so a
            // plain re-present would show nothing: re-run the current frame through
            // the full pipeline instead so every slider is live on a still frame.
            if(refreshPaused&&!m_playing&&!m_seeking){
                if(m_loaded&&!m_lastFrame.bgra.empty())RenderVideoFrame(m_lastFrame,false);
                else m_renderer->PresentCurrent();
            }
        }
    }

    void InvalidateControls(){
        if(!m_hwnd)return;RECT c{};GetClientRect(m_hwnd,&c);
        if(!m_loaded){InvalidateRect(m_hwnd,nullptr,FALSE);return;}
        RECT bar{VideoX0(c),std::max<LONG>(0,c.bottom-CONTROL_H),VideoX1(c),c.bottom};InvalidateRect(m_hwnd,&bar,FALSE);
        RECT panel{PanelX0(c),0,PanelX0(c)+SIDE_W,c.bottom};InvalidateRect(m_hwnd,&panel,FALSE);
    }

    static std::wstring SignedValue(float v,const wchar_t* suffix=L""){
        wchar_t b[64]{};swprintf_s(b,L"%+.2f%ls",v,suffix);return b;
    }

    static std::wstring PlainValue(float v,const wchar_t* suffix=L""){
        wchar_t b[64]{};swprintf_s(b,L"%.2f%ls",v,suffix);return b;
    }

    static LRESULT CALLBACK WndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        return a?a->WndProc(h,m,w,l):DefWindowProcW(h,m,w,l);
    }
    static LRESULT CALLBACK ViewportWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        switch(m){
        case WM_ERASEBKGND:return 1;
        case WM_PAINT:{
            PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);RECT r{};GetClientRect(h,&r);
            FillRect(dc,&r,(HBRUSH)GetStockObject(BLACK_BRUSH));EndPaint(h,&ps);return 0;
        }}
        return DefWindowProcW(h,m,w,l);
    }

    static LRESULT CALLBACK RenderWndProcStatic(HWND h,UINT m,WPARAM w,LPARAM l) {
        PlayerApp* a=nullptr;
        if(m==WM_NCCREATE){auto* cs=reinterpret_cast<CREATESTRUCTW*>(l);a=static_cast<PlayerApp*>(cs->lpCreateParams);SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(a));}
        else a=reinterpret_cast<PlayerApp*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(a){
            if(m==WM_ERASEBKGND)return 1;
            if(m==WM_PAINT){PAINTSTRUCT ps{};BeginPaint(h,&ps);EndPaint(h,&ps);return 0;}
            // Compare mode takes over left-drag to move the separator; otherwise
            // left-drag pans a zoomed view.
            if(m==WM_LBUTTONDOWN){SetFocus(a->m_hwnd);if(a->m_compareOn)a->BeginCompareDrag(h,l);else a->BeginZoomPan(h,l);return 0;}
            if(m==WM_LBUTTONUP){a->EndCompareDrag(h);a->EndZoomPan(h);return 0;}
            if(m==WM_LBUTTONDBLCLK){a->ToggleFullscreen();return 0;}
            if(m==WM_RBUTTONDOWN){a->BeginZoomRect(h,l);return 0;}
            if(m==WM_RBUTTONUP){a->EndZoomRect(h,l);return 0;}
            if(m==WM_MOUSEMOVE){if(a->m_compareDrag&&GetCapture()==h)a->UpdateCompareFromCursor(h,GET_X_LPARAM(l),GET_Y_LPARAM(l));else a->ZoomVideoMouseMove(h,l);return 0;}
            if(m==WM_CAPTURECHANGED){a->m_zoomRectDrag=false;a->m_zoomPan=false;a->m_compareDrag=false;return 0;}
            if(m==WM_MOUSEWHEEL||m==WM_KEYDOWN||m==WM_SYSKEYDOWN)return SendMessageW(a->m_hwnd,m,w,l);
            if(m==WM_DROPFILES)return SendMessageW(a->m_hwnd,m,w,l); // main window owns DragFinish().
        }
        return DefWindowProcW(h,m,w,l);
    }

    HMENU CreateMenuBar() {
        HMENU bar=CreateMenu(),file=CreatePopupMenu(),play=CreatePopupMenu(),video=CreatePopupMenu(),dlss=CreatePopupMenu(),quality=CreatePopupMenu(),language=CreatePopupMenu();
        auto add=[&](HMENU m,UINT id,const wchar_t* key){std::wstring s=T(key);AppendMenuW(m,MF_STRING,id,s.c_str());};
        add(file,IDM_OPEN,L"menu.open"); AppendMenuW(file,MF_SEPARATOR,0,nullptr); add(file,IDM_EXIT,L"menu.exit");
        add(play,IDM_PLAY,L"menu.playpause"); add(play,IDM_STOP,L"menu.stop"); add(play,IDM_BACK10,L"menu.back10"); add(play,IDM_FWD10,L"menu.forward10"); add(play,IDM_MUTE,L"menu.mute");
        add(video,IDM_ASPECT_FIT,L"menu.aspectfit"); add(video,IDM_ASPECT_FILL,L"menu.aspectfill"); AppendMenuW(video,MF_SEPARATOR,0,nullptr);
        add(video,IDM_VIEW_FINAL,L"menu.final"); add(video,IDM_VIEW_INPUT,L"menu.input"); add(video,IDM_VIEW_MV,L"menu.mv"); add(video,IDM_VIEW_DEPTH,L"menu.depth"); add(video,IDM_VIEW_MASK,L"menu.mask"); AppendMenuW(video,MF_SEPARATOR,0,nullptr); add(video,IDM_FULLSCREEN,L"menu.fullscreen");
        AppendMenuW(video,MF_SEPARATOR,0,nullptr); AppendMenuW(video,MF_STRING|(m_panelLeft?MF_CHECKED:MF_UNCHECKED),IDM_PANEL_LEFT,L"Control panel on the left");
        add(quality,IDM_QUALITY_AUTO,L"menu.quality_auto"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_QUALITY,L"Quality"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_BALANCED,L"Balanced"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_PERFORMANCE,L"Performance"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_ULTRAPERF,L"Ultra Performance"); AppendMenuW(quality,MF_STRING,IDM_QUALITY_DLAA,L"DLAA");
        add(dlss,IDM_DLSS,L"menu.dlss_toggle"); std::wstring qualityName=T(L"menu.quality"); AppendMenuW(dlss,MF_POPUP,reinterpret_cast<UINT_PTR>(quality),qualityName.c_str());
        // Effects menu: every addition individually toggleable for A/B, plus a
        // bypass-all on 'B'. Values are kept; only the application is gated.
        HMENU fx=CreatePopupMenu();
        auto chk=[&](UINT id,bool on,const wchar_t* text){AppendMenuW(fx,MF_STRING|(on?MF_CHECKED:MF_UNCHECKED),id,text);};
        AppendMenuW(fx,MF_STRING|(m_bypassFX?MF_CHECKED:MF_UNCHECKED),IDM_FX_BYPASS,L"Bypass ALL additions\tB");
        AppendMenuW(fx,MF_SEPARATOR,0,nullptr);
        chk(IDM_FX_LUT,m_fxLut,L"LUT");
        chk(IDM_FX_SHARPEN,m_fxSharpen,L"Pre-Sharpen");
        chk(IDM_FX_TONE,m_fxTone,L"NR Tone Mix");
        chk(IDM_FX_FLOW,m_fxFlow,L"RAFT Flow (_flow.mp4)");
        chk(IDM_FX_DEPTHMAP,m_fxDepth,L"Depth Map (_depth.mp4)");
        chk(IDM_FX_MASK,m_fxMask,L"Segmentation Mask -> NR (_mask.mp4)");
        AppendMenuW(fx,MF_SEPARATOR,0,nullptr);
        AppendMenuW(fx,MF_STRING|(m_lanczos4K?MF_CHECKED:MF_UNCHECKED),IDM_FX_LANCZOS4K,L"4K via Lanczos + DLAA (not DLSS SR)");
        AppendMenuW(fx,MF_SEPARATOR,0,nullptr);
        AppendMenuW(fx,MF_STRING,IDM_LUT_LOAD,L"Load LUT (.cube)...");
        AppendMenuW(fx,MF_STRING|(m_lutPath.empty()?MF_GRAYED:MF_UNCHECKED),IDM_LUT_CLEAR,L"Clear LUT");
        HMENU motion=CreatePopupMenu();
        AppendMenuW(motion,MF_STRING|(m_motionMode==0?MF_CHECKED:0),IDM_MOTION_ZERO,L"Zero (cleanest)");
        AppendMenuW(motion,MF_STRING|(m_motionMode==1?MF_CHECKED:0),IDM_MOTION_GLOBAL,L"Global pan");
        AppendMenuW(motion,MF_STRING|(m_motionMode==2?MF_CHECKED:0),IDM_MOTION_EST,L"Estimated (full flow)");
        AppendMenuW(fx,MF_POPUP,reinterpret_cast<UINT_PTR>(motion),L"Motion Vectors (fallback)");
        HMENU nrmodel=CreatePopupMenu();
        // Direct engine: style is a live per-evaluate parameter (0 = A when unset).
        const int curStyle=(m_nrModelPick>=0)?m_nrModelPick:0;
        AppendMenuW(nrmodel,MF_STRING|(curStyle==0?MF_CHECKED:0),IDM_NRMODEL_A,L"Model A (strongest uplift)");
        AppendMenuW(nrmodel,MF_STRING|(curStyle==1?MF_CHECKED:0),IDM_NRMODEL_B,L"Model B (subtle)");
        AppendMenuW(nrmodel,MF_STRING|(curStyle==2?MF_CHECKED:0),IDM_NRMODEL_C,L"Model C (contrasty)");
        AppendMenuW(fx,MF_POPUP,reinterpret_cast<UINT_PTR>(nrmodel),L"NR Model");
        m_languageCodes.clear();
        const auto packs=m_loc.AvailableLanguages();
        for(size_t i=0;i<packs.size()&&i<100;++i){
            m_languageCodes.push_back(packs[i].code);
            UINT flags=MF_STRING|(m_loc.Code()==packs[i].code?MF_CHECKED:MF_UNCHECKED);
            AppendMenuW(language,flags,IDM_LANG_BASE+static_cast<UINT>(i),packs[i].name.c_str());
        }
        std::wstring sFile=T(L"menu.file"),sPlay=T(L"menu.playback"),sVideo=T(L"menu.video"),sDlss=T(L"menu.dlss"),sLang=T(L"menu.language");
        AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(file),sFile.c_str()); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(play),sPlay.c_str()); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(video),sVideo.c_str()); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(dlss),sDlss.c_str()); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(fx),L"Effects"); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(language),sLang.c_str());
        HMENU help=CreatePopupMenu(); AppendMenuW(help,MF_STRING,IDM_HELP,L"Manual\tF1"); AppendMenuW(bar,MF_POPUP,reinterpret_cast<UINT_PTR>(help),L"Help");
        return bar;
    }

    void ApplyLanguage(const std::wstring& code) {
        m_loc.SetLanguage(code,true); HMENU old=GetMenu(m_hwnd),fresh=CreateMenuBar(); SetMenu(m_hwnd,fresh); DrawMenuBar(m_hwnd); if(old)DestroyMenu(old); UpdateTitle(); InvalidateRect(m_hwnd,nullptr,TRUE);
    }

    bool Load(const std::wstring& path) {
        if(path.empty())return false;
        Unload();
        if(!m_decoder.Open(path)){std::wstring e=T(L"error.decode"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);return false;}
        m_dar=m_decoder.DisplayAspectRatio(); if(!std::isfinite(m_dar)||m_dar<0.2)m_dar=double(m_decoder.Width())/std::max(1u,m_decoder.Height());
        auto [ow,oh]=OutputForAspect(m_dar,m_opt.maxW,m_opt.maxH);
        m_activeScale = m_opt.scaleExplicit ? m_opt.scale : AutoDecodeScale(m_decoder.NativeWidth(),m_decoder.NativeHeight(),ow,oh,m_decoder.FrameRate());
        LOG("Decode scale policy: " << (m_opt.scaleExplicit?"explicit":"auto-realtime") << " -> " << DecodeScaleNameA(m_activeScale));
        const auto [decodeW,decodeH]=RecommendedDecodeSize(m_decoder.NativeWidth(),m_decoder.NativeHeight(),ow,oh,m_activeScale);
        if((decodeW!=m_decoder.Width()||decodeH!=m_decoder.Height()) && !m_decoder.SetDecodeSize(decodeW,decodeH))
            LOG("Realtime decode scaling unavailable; continuing at native decoder resolution.");
        const auto [guideW,guideH]=TemporalGuideGenerator::AnalysisGrid(m_decoder.Width(),m_decoder.Height(),m_decoder.FrameRate());
        m_flowLoaded=false;m_haveFlowFrame=false;m_flowDecoder.Close();
        {const std::wstring fp=FlowVideoPathFor(path);
         if(std::filesystem::exists(fp)&&m_flowDecoder.Open(fp)){
            if(m_flowDecoder.Width()!=m_decoder.Width()||m_flowDecoder.Height()!=m_decoder.Height())
                m_flowDecoder.SetDecodeSize(m_decoder.Width(),m_decoder.Height());
            if(m_flowDecoder.Width()==m_decoder.Width()&&m_flowDecoder.Height()==m_decoder.Height()){
                m_flowLoaded=true;LOG("RAFT flow video attached to live playback.");
            } else {m_flowDecoder.Close();LOG("Flow video size mismatch; live attach skipped.");}
         }}
        // Depth-map live attach (restored): <stem>_depth.mp4 (Depth Anything,
        // bright = near) feeds the renderer's external-depth resource. Runtimes
        // that ignore depth are unaffected; the Depth debug view shows the map.
        m_depthLoaded=false;m_haveDepthFrame=false;m_depthVideoDecoder.Close();
        {const std::wstring dp=DepthVideoPathFor(path);
         if(std::filesystem::exists(dp)&&m_depthVideoDecoder.Open(dp)){
            if(m_depthVideoDecoder.Width()!=m_decoder.Width()||m_depthVideoDecoder.Height()!=m_decoder.Height())
                m_depthVideoDecoder.SetDecodeSize(m_decoder.Width(),m_decoder.Height());
            if(m_depthVideoDecoder.Width()==m_decoder.Width()&&m_depthVideoDecoder.Height()==m_decoder.Height()){
                m_depthLoaded=true;LOG("Depth map video attached to live playback.");
            } else {m_depthVideoDecoder.Close();LOG("Depth video size mismatch; live attach skipped.");}
         }}
        // Segmentation-mask live attach: <stem>_mask.mp4 (white = process here)
        // replaces the block-matcher uncertainty as the ControlMask source. It
        // only reaches the runtime when the mask guide bit is also bound.
        m_maskLoaded=false;m_haveMaskFrame=false;m_maskDecoder.Close();
        {const std::wstring mp=MaskVideoPathFor(path);
         if(std::filesystem::exists(mp)&&m_maskDecoder.Open(mp)){
            if(m_maskDecoder.Width()!=m_decoder.Width()||m_maskDecoder.Height()!=m_decoder.Height())
                m_maskDecoder.SetDecodeSize(m_decoder.Width(),m_decoder.Height());
            if(m_maskDecoder.Width()==m_decoder.Width()&&m_maskDecoder.Height()==m_decoder.Height()){
                m_maskLoaded=true;LOG("Segmentation mask video attached to live playback.");
            } else {m_maskDecoder.Close();LOG("Mask video size mismatch; live attach skipped.");}
         }}
        ShowWindow(m_viewport,SW_SHOW); Layout();
        m_renderer=std::make_unique<D3D12Renderer>();
        m_renderer->SetNRSettings(CurrentNRSettings());
        if(m_flowLoaded)m_renderer->EnableExternalFlow();
        if(m_depthLoaded)m_renderer->EnableExternalDepth();
        if(m_maskLoaded)m_renderer->EnableExternalMask();
        m_lutLoaded=false;
        if(!m_lutPath.empty()){
            // A saved LUT path that no longer exists (the checkout moved, a
            // folder was renamed) is looked up by file name in the project's
            // luts\ folder and the setting is healed, so the grade survives
            // instead of silently dropping out of the preview - and out of
            // every export, which used to abort on the dead path.
            std::error_code lec;
            if(!std::filesystem::is_regular_file(m_lutPath,lec)){
                const std::filesystem::path dir=smru::paths::LutDirectory();
                const std::filesystem::path alt=dir.empty()?std::filesystem::path():dir/std::filesystem::path(m_lutPath).filename();
                if(!alt.empty()&&std::filesystem::is_regular_file(alt,lec)){
                    LOG("Player LUT path healed: "<<smru::text::WideToUtf8(m_lutPath)<<" -> "<<smru::text::WideToUtf8(alt.wstring()));
                    m_lutPath=alt.wstring();SaveVideoSettings();
                }
            }
            // LUT texture is created during Initialize, so load it first.
            std::vector<float> lut;uint32_t lutSize=0;float dmin[3],dmax[3];
            m_lutLoaded=LoadCubeLUT(m_lutPath,lut,lutSize,dmin,dmax)&&m_renderer->SetLUT(std::move(lut),lutSize,dmin,dmax);
            if(m_lutLoaded){
                m_renderer->SetLUTStrength(m_lutStrength);
                LOG("Player LUT loaded: "<<lutSize<<"^3");
            } else {
                LOG("Player LUT failed to load ("<<smru::text::WideToUtf8(m_lutPath)<<"); continuing without it. Exports will not receive it either.");
            }
        }
        if(!m_renderer->Initialize(m_renderWnd,m_decoder.Width(),m_decoder.Height(),ow,oh,guideW,guideH)){std::wstring e=T(L"error.renderer"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);m_renderer.reset();m_decoder.Close();ShowWindow(m_viewport,SW_HIDE);return false;}
        m_pipeline=std::make_unique<FramePipeline>(*m_renderer);
        m_pipeline->SetPolicy({.srcW=m_decoder.Width(),.srcH=m_decoder.Height(),.fps=m_decoder.FrameRate()});
        m_pipeline->Guides().SetDepthMode(m_depthMode);
        ApplyVideoAdjustments(false);
        VideoFrame first; if(!m_decoder.ReadNext(first)){std::wstring e=T(L"error.frame"),cap=T(L"app.title");MessageBoxW(m_hwnd,e.c_str(),cap.c_str(),MB_ICONERROR);Unload();return false;}
        m_pipeline->ResetHistory();m_guideReset=true;m_dlssReset=true;RenderVideoFrame(first,true);m_currentSec=double(first.timestamp100ns)*1e-7;
        m_haveNext=m_decoder.ReadNext(m_next);m_audio.Start(path,m_currentSec);m_audio.SetVolume(m_muted?0.0f:m_volume);m_playing=true;m_playStartSec=m_currentSec;m_playStart=Clock::now();m_loaded=true;m_path=path;m_droppedFrames=0;m_uiTick=0;m_seekPending=false;m_seeking=false;m_fpsWindowStart=Clock::now();m_fpsWindowFrames=0;m_submitFps=0.0;
        UpdateTitle();Layout();InvalidateRect(m_hwnd,nullptr,TRUE);return true;
    }

    void Unload() {
        m_zoom=1.0f;m_zoomCX=0.5f;m_zoomCY=0.5f;m_zoomRectDrag=false;m_zoomPan=false;
        m_compareOn=false;m_compareDrag=false;m_compareOrient=1;m_comparePos=0.5f;
        m_seekPending=false;m_seeking=false;m_audio.Stop(); if(m_renderer){m_renderer->WaitGPU();m_pipeline.reset();m_renderer.reset();} m_decoder.Close();m_flowDecoder.Close();m_flowLoaded=false;m_haveFlowFrame=false;m_depthVideoDecoder.Close();m_depthLoaded=false;m_haveDepthFrame=false;m_maskDecoder.Close();m_maskLoaded=false;m_haveMaskFrame=false;m_haveNext=false;m_next=VideoFrame{};m_loaded=false;m_playing=false;m_currentSec=0;m_path.clear();
        if(m_viewport)ShowWindow(m_viewport,SW_HIDE); UpdateTitle(); if(m_hwnd)InvalidateRect(m_hwnd,nullptr,TRUE);
    }

    bool RenderVideoFrame(const VideoFrame& f,bool resetGuide) {
        if(!m_renderer||!m_pipeline)return false;
        if(&f!=&m_lastFrame)m_lastFrame=f; // kept for the compare-screenshot button
        // Sidecar guides are separate movies decoded alongside this one: advance
        // each to the frame that covers this timestamp (within half a frame).
        FramePipeline::Sidecars sc;
        const int64_t half=int64_t(0.5e7/std::max(1.0,m_decoder.FrameRate()));
        const size_t px=size_t(m_decoder.Width())*m_decoder.Height();
        auto advance=[&](VideoDecoder& dec,VideoFrame& frame,bool& have)->bool{
            while(!have||frame.timestamp100ns+half<f.timestamp100ns){
                if(!dec.ReadNext(frame))break;
                have=true;
                if(frame.timestamp100ns+half>=f.timestamp100ns)break;
            }
            return have&&frame.bgra.size()>=px*4;
        };
        if(m_flowLoaded&&advance(m_flowDecoder,m_flowFrame,m_haveFlowFrame)){
            sc.flow=m_flowFrame.bgra.data();sc.flowBytes=m_flowFrame.bgra.size();
        }
        if(m_maskLoaded&&advance(m_maskDecoder,m_maskFrame,m_haveMaskFrame)){
            sc.mask=m_maskFrame.bgra.data();sc.maskBytes=m_maskFrame.bgra.size();
        }
        if(m_depthLoaded&&advance(m_depthVideoDecoder,m_depthVideoFrame,m_haveDepthFrame)){
            DepthGrayToR16(m_depthVideoFrame.bgra.data(),m_decoder.Width(),m_decoder.Height(),m_depthPlane);
            sc.depth=m_depthPlane.data();sc.depthBytes=m_depthPlane.size();
        }
        // Zero mode leaves the estimated field in the grid so the MV debug view
        // can show it; the renderer hands NGX a zero texture (SetMVFieldScale).
        m_pipeline->SetMotionMode(m_motionMode==1?FramePipeline::MotionMode::GlobalPan
                                 :m_motionMode==0?FramePipeline::MotionMode::Zero
                                                 :FramePipeline::MotionMode::Estimated);
        const bool reset=m_dlssReset||resetGuide;
        if(!m_pipeline->Render(f.bgra.data(),f.bgra.size(),reset,sc))return false;
        // A reset leaves the neural history empty, and the first evaluation after
        // it is the soft one - the exporter discards two such warm-up renders
        // before it keeps a frame. Playback converges it over the next frames; a
        // STILL (open, a paused seek, a toggle while paused) would sit on that
        // soft first pass until something happened to force a re-render, which is
        // what made a paused preview look like the neural pass was off.
        if(reset&&!m_playing){
            for(int i=0;i<kStillWarmup;++i)
                if(!m_pipeline->Render(f.bgra.data(),f.bgra.size(),false,sc))return false;
        }
        return true;
    }
    static constexpr int kStillWarmup=2;   // matches the exporter's --warmup default

    // The decode size a preset asks for: a fraction of the output box.
    static double ScaleFactor(DecodeScale s) {
        switch(s){
        case DecodeScale::Balanced:return 0.58;
        case DecodeScale::Performance:return 0.50;
        case DecodeScale::UltraPerformance:return 1.0/3.0;
        default:return 2.0/3.0;
        }
    }

    static std::pair<uint32_t,uint32_t> RecommendedDecodeSize(uint32_t nw,uint32_t nh,uint32_t ow,uint32_t oh,DecodeScale q) {
        if(!nw||!nh||!ow||!oh||q==DecodeScale::Native)return{nw,nh};
        const double scale=ScaleFactor(q);
        uint32_t tw=std::max(2u,uint32_t(std::lround(double(ow)*scale))&~1u);
        uint32_t th=std::max(2u,uint32_t(std::lround(double(oh)*scale))&~1u);
        // Never decode-upscale a smaller movie just to feed DLSS. The renderer/NGX
        // policy will preserve the genuine reconstruction distance for low-res sources.
        if(uint64_t(nw)*nh<=uint64_t(tw)*th)return{nw,nh};
        return{tw,th};
    }

    static DecodeScale AutoDecodeScale(uint32_t sw,uint32_t sh,uint32_t ow,uint32_t oh,double fps) {
        if(!sw||!sh||!ow||!oh) return DecodeScale::Quality;
        const double scale=std::sqrt((double(sw)*double(sh))/(double(ow)*double(oh)));
        // Realtime policy: when the movie already matches the output resolution,
        // decoding at native size makes the neural pass redraw every output pixel
        // with no reconstruction distance. Auto instead decodes smaller so there is
        // a genuine upscale. 4K high-frame-rate video starts at Balanced; otherwise
        // Quality. Users can still explicitly select DLAA (native) from the menu.
        if(scale>=0.90) {
            const uint64_t outPixels=uint64_t(ow)*uint64_t(oh);
            if(outPixels>=uint64_t(3840)*2160 && fps>=45.0) return DecodeScale::Balanced;
            return DecodeScale::Quality;
        }
        struct C{double s;DecodeScale q;};
        const C cands[]={{2.0/3.0,DecodeScale::Quality},{0.58,DecodeScale::Balanced},{0.50,DecodeScale::Performance},{1.0/3.0,DecodeScale::UltraPerformance}};
        double best=1e9;DecodeScale q=DecodeScale::Quality;
        for(const auto& c:cands){double e=std::abs(std::log(std::max(scale,0.05)/c.s));if(e<best){best=e;q=c.q;}}
        return q;
    }
    static const char* DecodeScaleNameA(DecodeScale q){switch(q){case DecodeScale::Performance:return "Performance";case DecodeScale::Balanced:return "Balanced";case DecodeScale::UltraPerformance:return "UltraPerf";case DecodeScale::Native:return "DLAA";default:return "Quality";}}

    static std::pair<uint32_t,uint32_t> OutputForAspect(double dar,uint32_t maxW,uint32_t maxH) {
        double box=double(maxW)/maxH;uint32_t w,h;if(dar>=box){w=maxW;h=uint32_t(std::lround(double(w)/dar));}else{h=maxH;w=uint32_t(std::lround(double(h)*dar));}
        w=std::max(64u,w&~1u);h=std::max(64u,h&~1u);return{w,h};
    }

    double Position() const {
        if(!m_loaded)return 0;if(!m_playing)return m_currentSec;
        double audio=m_audio.PositionSeconds();
        if(audio>=0.0){double d=m_decoder.DurationSeconds();return d>0?std::clamp(audio,0.0,d):audio;}
        double s=m_playStartSec+std::chrono::duration<double>(Clock::now()-m_playStart).count();double d=m_decoder.DurationSeconds();return d>0?std::clamp(s,0.0,d):std::max(0.0,s);
    }

    double ClampSeek(double sec)const{double dur=m_decoder.DurationSeconds();if(dur>0)return std::clamp(sec,0.0,dur);return std::max(0.0,sec);}

    void RequestSeek(double sec) {
        const bool resume=m_seekPending?m_seekResumePlaying:m_playing; RequestSeek(sec,resume);
    }

    void RequestSeek(double sec,bool resumeAfter) {
        if(!m_loaded)return; sec=ClampSeek(sec);
        if(!m_seekPending) m_currentSec=Position();
        m_pendingSeekSec=sec;m_seekResumePlaying=resumeAfter;m_seekPending=true;m_playing=false;m_audio.Pause(true);m_seekPreview=sec;InvalidateRect(m_hwnd,nullptr,FALSE);
    }

    bool PerformSeek(double sec,bool resumeAfter) {
        if(!m_loaded||m_seeking)return false;m_seeking=true;sec=ClampSeek(sec);LOG("Seek begin target="<<sec<<" resume="<<resumeAfter);
        // Seek is deliberately transactional and performed from Tick(), never from a mouse message.
        // Shut down the audio producer first, wait for GPU work, then restart the video decoder.
        m_audio.Stop(); if(m_renderer)m_renderer->WaitGPU(); m_haveNext=false;m_next=VideoFrame{};
        auto readAt=[&](double target,VideoFrame& frame)->bool{
            if(!m_decoder.SeekSeconds(target))return false;
            if(m_decoder.ReadNext(frame))return true;
            const double dur=m_decoder.DurationSeconds(),fd=1.0/std::max(1.0,m_decoder.FrameRate());
            if(dur>0.0&&target>0.0){const double safe=std::max(0.0,std::min(target,dur-fd*1.5));if(safe<target&&m_decoder.SeekSeconds(safe)&&m_decoder.ReadNext(frame))return true;}
            return false;
        };
        if(m_flowLoaded){m_flowDecoder.SeekSeconds(sec);m_haveFlowFrame=false;}
        if(m_depthLoaded){m_depthVideoDecoder.SeekSeconds(sec);m_haveDepthFrame=false;}
        if(m_maskLoaded){m_maskDecoder.SeekSeconds(sec);m_haveMaskFrame=false;}
        VideoFrame f; bool got=readAt(sec,f);
        if(!got){
            LOG("Seek decoder restart failed; reopening the same file for recovery.");
            m_decoder.Close(); if(m_decoder.Open(m_path))got=readAt(sec,f);
        }
        if(!got){
            LOG("Seek failed without crashing; playback remains paused.");m_playing=false;m_seeking=false;m_currentSec=sec;UpdateTitle();InvalidateRect(m_hwnd,nullptr,FALSE);return false;
        }
        if(m_pipeline)m_pipeline->ResetHistory();
        m_guideReset=true;m_dlssReset=true;
        if(!RenderVideoFrame(f,true)){LOG("Seek frame render failed.");m_playing=false;m_seeking=false;return false;}
        m_currentSec=double(f.timestamp100ns)*1e-7;m_haveNext=m_decoder.ReadNext(m_next);
        const bool audioOk=m_audio.Start(m_path,m_currentSec);if(audioOk){m_audio.SetVolume(m_muted?0.0f:m_volume);m_audio.Pause(!resumeAfter);}else LOG("Seek: no audio stream/output; using steady-clock video pacing.");
        m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=resumeAfter&&m_haveNext;m_guideReset=false;m_dlssReset=false;m_seeking=false;UpdateTitle();InvalidateRect(m_hwnd,nullptr,FALSE);LOG("Seek complete actual="<<m_currentSec);return true;
    }

    void SetPaused(bool pause){if(!m_loaded||m_seeking)return;if(pause==!m_playing)return;if(pause){m_currentSec=Position();m_playing=false;m_audio.Pause(true);}else{if(!m_haveNext&&m_decoder.DurationSeconds()>0){RequestSeek(0,true);return;}m_playStartSec=m_currentSec;m_playStart=Clock::now();m_playing=true;m_audio.Pause(false);}InvalidateRect(m_hwnd,nullptr,FALSE);}
    void TogglePause(){SetPaused(m_playing);}
    void StopPlayback(){RequestSeek(0,false);}

    void UpdateTitle(){
        if(!m_hwnd)return; if(!m_loaded||!m_renderer){SetWindowTextW(m_hwnd,T(L"app.title").c_str());return;}
        std::wstringstream s;s<<smru::kProductNameW<<L" "<<smru::kVersionW<<L" | source "<<m_decoder.NativeWidth()<<L"x"<<m_decoder.NativeHeight();if(m_decoder.Width()!=m_decoder.NativeWidth()||m_decoder.Height()!=m_decoder.NativeHeight())s<<L" decode "<<m_decoder.Width()<<L"x"<<m_decoder.Height();s<<L" | NR "<<m_renderer->OutputW()<<L"x"<<m_renderer->OutputH()<<L" | "<<m_decoder.BackendName()<<L" | "<<(m_renderer->DLSSFeatureCreated()?L"NR ACTIVE":(m_renderer->DLSSAvailable()?L"NR READY":L"NR OFF"))<<L" | "<<m_renderer->DLSSEvaluations()<<L" frames";SetWindowTextW(m_hwnd,s.str().c_str());
    }

    // Control panel side. The panel is a full-height column on the left or the
    // right (PanelLeft in the ini, Video menu); the video and its bar - transport,
    // time, status, timeline, volume - fill the other column, so the controls
    // sit under the picture rather than across the whole window. Every layout
    // and hit-test below derives from these three.
    LONG PanelX0(const RECT&c)const{return m_panelLeft?0:std::max<LONG>(0,c.right-SIDE_W);}
    LONG VideoX0(const RECT&)const{return m_panelLeft?SIDE_W:0;}
    LONG VideoX1(const RECT&c)const{return m_panelLeft?c.right:std::max<LONG>(0,c.right-SIDE_W);}

    void Layout(){
        if(!m_hwnd||!m_viewport||!m_renderWnd)return;RECT c{};GetClientRect(m_hwnd,&c);int W=static_cast<int>(std::max<LONG>(1,c.right-c.left)),H=static_cast<int>(std::max<LONG>(1,c.bottom-c.top));
        if(!m_loaded){MoveWindow(m_viewport,0,0,W,H,TRUE);return;}
        const int areaW=std::max(1,W-SIDE_W);
        int areaH=std::max(1,H-CONTROL_H);MoveWindow(m_viewport,int(VideoX0(c)),0,areaW,areaH,TRUE);double ar=m_dar>0?m_dar:16.0/9.0;double areaAr=double(areaW)/areaH;int rw=0,rh=0;
        if(m_fill){if(areaAr>ar){rw=areaW;rh=int(std::lround(areaW/ar));}else{rh=areaH;rw=int(std::lround(areaH*ar));}}else{if(areaAr>ar){rh=areaH;rw=int(std::lround(areaH*ar));}else{rw=areaW;rh=int(std::lround(areaW/ar));}}
        SetWindowPos(m_renderWnd,nullptr,(areaW-rw)/2,(areaH-rh)/2,std::max(1,rw),std::max(1,rh),SWP_NOZORDER|SWP_NOACTIVATE);
        InvalidateRect(m_viewport,nullptr,FALSE);InvalidateControls();
    }

    // One authoritative control layout, consumed by Paint and MouseDown alike so
    // they can never disagree. Two regions:
    //  - slim bottom bar: icon-only transport, centered (Open | -10 Play Stop +10
    //    Loop | Mute | Full), volume right, time+status+timeline below;
    //  - right panel: captioned framed groups of measured icon+text pills (never
    //    truncated) plus the draggable Levels sliders.
    // Index -> action (OnButton) is fixed.
    // 0 Open,1 -10,2 Play,3 Stop,4 +10,5 Mute,6 DLSS,7 Aspect,8 Color,
    // 9 GenDepth,10 MV,11 Full,12 Export,13 Compare,14 4K,15 Split,16 Loop,17 GenFlow,
    // 18 Bypass,19 LUT,20 Sharp,21 DepthMap (A/B model depth),22 Tone,23 Depth view,
    // 24 Flow,25 Shot,26 Mask view,
    // 27 Load LUT (.cube picker),28/29/30 NR model A/B/C (direct-engine style 0..2),
    // 31/32/33 motion-vector mode select (zero / global pan / estimated field),
    // 34 SeedVR job, 35/36/37 inspection zoom out / in / reset-to-fit,
    // 38 A/B vertical split, 39 A/B horizontal split,
    // 43 GenMask (text-prompted segmentation job), 44 MaskNR (bind the mask
    // into the neural pass, A/B), 45 Reset (every look control to defaults).
    static constexpr int kBtnCount=47;   // 46 = NR Auto Mask (was the Image adjustments window button)
    // (kept for reference)   // 40 = export resolution cycle, 41 = export codec cycle, 42 = save processed frame
    static constexpr int kBarOrder[8]={0, 1,2,3,4,16, 5, 11};
    static constexpr int kBarGroup[8]={0, 1,1,1,1,1,  2, 3};
    static constexpr int kPanelG0[]={6,7,10,23,26,28,29,30,46};
    static constexpr int kPanelG1[]={12,13,14,15,40,41,25,42,17,9,43,34};
    static constexpr int kPanelG2[]={31,32,33};
    static constexpr int kPanelG3[]={18,19,20,22,24,21,44,27,45};
    static constexpr int kPanelG4[]={38,39,35,36,37};
    struct PanelGroup{const wchar_t* caption;const int* items;int count;};
    static constexpr PanelGroup kPanelGroups[5]={
        {L"Picture",kPanelG0,9},{L"Export & Jobs",kPanelG1,12},
        {L"Motion",kPanelG2,3},{L"Effects",kPanelG3,9},
        {L"Inspect",kPanelG4,5}};
    struct BarFrame{RECT r;const wchar_t* caption;int group;COLORREF tint;};
    RECT m_btnRect[kBtnCount]{};
    std::vector<BarFrame> m_barFrames;
    // Draggable sliders in the panel: Levels (the neural and effect strengths)
    // and Color (the picture adjustments, applied after the neural pass).
    // Tracks are normalized 0..1; kSliderMin/Max map them to the real ranges
    // (Structure 0..2, Brightness -2..+2 EV, ...). Skin's leftmost third is
    // the -1 sentinel: follow Structure.
    static constexpr int kSliderCount=15;
    static constexpr int kColorSliderFirst=9;   // sliders 9.. form the Color group
    static constexpr const wchar_t* kSliderLabel[kSliderCount]={L"NR",L"Struct",L"Sharp",L"Post",L"Tone",L"Smooth",L"LUT",L"SVR",L"Skin",
                                                                L"Bright",L"Contr",L"Sat",L"Gamma",L"Temp",L"Tint"};
    static constexpr float kSliderMin[kSliderCount]={0,0,0,0,0,0,0,0,-1, -2,0,0,0.25f,-1,-1};
    static constexpr float kSliderMax[kSliderCount]={1,2,1,1,1,1,1,1, 2,  2,3,3,3,    1, 1};
    RECT m_sliderRect[kSliderCount]{};RECT m_sliderTrack[kSliderCount]{};
    int m_dragSlider=-1;
    HWND m_tip=nullptr;int m_tipId=-1;
    int m_pressBtn=-1;   // armed on press, fires on release while still over
    // Default: Picture, Inspect and Levels expanded; Export & Jobs, Motion,
    // Effects and Color start collapsed (bitmask 49, persisted as PanelOpen).
    bool m_groupOpen[7]={true,false,false,false,true,true,false};
    int m_exportRes=0;   // 0 = preview size, 1 = 3840 long side, 2 = 7680 long side, 3 = native (source size, 1:1 neural pass)
    int m_exportCodec=0; // 0 = x264, 1 = HEVC NVENC, 2 = AV1 NVENC
    int m_panelScroll=0,m_panelContentH=0;

    float SliderRaw(int s)const{
        switch(s){
        case 0:return m_nrIntensity;case 1:return m_nrLocalStructure;
        case 2:return m_sharpen;case 3:return m_postSharpen;
        case 4:return m_toneMix;case 5:return m_nrSmooth;case 6:return m_lutStrength;
        case 7:return m_svrStrength;case 8:return m_nrSkinStructure;
        case 9:return m_colorSettings.brightness;case 10:return m_colorSettings.contrast;
        case 11:return m_colorSettings.saturation;case 12:return m_colorSettings.gamma;
        case 13:return m_colorSettings.temperature;case 14:return m_colorSettings.tint;
        }
        return 0.0f;
    }
    float GetSliderVal(int s)const{
        const float span=kSliderMax[s]-kSliderMin[s];
        return span>0.0f?(SliderRaw(s)-kSliderMin[s])/span:SliderRaw(s);
    }
    // The chip above a dragged or hovered knob: the real value, signed where
    // the range crosses zero, "Local" for the Skin sentinel.
    std::wstring SliderValueText(int s)const{
        const float v=SliderRaw(s);
        if(s==8&&v<0.0f)return L"Local";
        return kSliderMin[s]<0.0f?SignedValue(v):PlainValue(v);
    }
    void SetSliderVal(int s,float v){
        v=kSliderMin[s]+std::clamp(v,0.0f,1.0f)*(kSliderMax[s]-kSliderMin[s]);
        // Dragging a level clearly means "apply this effect": arm its A/B toggle
        // so the slider always has a visible result (Bypass still overrides all).
        switch(s){
        case 0:m_nrIntensity=v;break;
        case 1:m_nrLocalStructure=v;break;
        case 2:m_sharpen=v;if(v>0.001f)m_fxSharpen=true;break;
        case 3:m_postSharpen=v;break;
        case 4:m_toneMix=v;if(v>0.001f)m_fxTone=true;break;
        case 5:m_nrSmooth=v;break;
        case 6:m_lutStrength=v;if(v>0.001f)m_fxLut=true;break;
        case 7:m_svrStrength=v;break; // applies to the next SeedVR job
        // Skin: the leftmost third snaps to the -1 sentinel (follow Structure);
        // 0 means skin structure OFF, which is deliberately distinct.
        case 8:m_nrSkinStructure=v<0.0f?-1.0f:v;break;
        case 9:m_colorSettings.brightness=v;break;
        case 10:m_colorSettings.contrast=v;break;
        case 11:m_colorSettings.saturation=v;break;
        case 12:m_colorSettings.gamma=v;break;
        case 13:m_colorSettings.temperature=v;break;
        case 14:m_colorSettings.tint=v;break;
        }
        ApplyVideoAdjustments(true);InvalidateControls();
    }
    bool IsBarBtn(int i)const{
        switch(i){case 0:case 1:case 2:case 3:case 4:case 5:case 11:case 16:return true;}return false;
    }
    void SetSliderFromX(int s,int x){
        const RECT&t=m_sliderTrack[s];const LONG span=(t.right>t.left)?(t.right-t.left):LONG(1);
        SetSliderVal(s,float(std::clamp(double(LONG(x)-t.left)/double(span),0.0,1.0)));
    }
    bool BtnIconOnly(int i)const{
        switch(i){case 0:case 1:case 2:case 3:case 4:case 5:case 11:case 16:return true;}return false;
    }
    // Category tints: 1 Picture (blue), 2 Export & Jobs (violet), 3 Effects
    // (green), 4 Motion (teal), 5 Inspect (sand). Mirrors the group captions.
    int BtnRole(int i)const{
        switch(i){
        case 6:case 7:case 10:case 23:case 26:case 28:case 29:case 30:case 46:return 1;
        case 9:case 12:case 13:case 14:case 15:case 17:case 25:case 34:case 40:case 41:case 42:case 43:return 2;
        case 18:case 19:case 20:case 21:case 22:case 24:case 27:case 44:case 45:return 3;
        case 31:case 32:case 33:return 4;
        case 35:case 36:case 37:case 38:case 39:return 5;
        }
        return 0;
    }

    std::wstring ExportBtnLabel(const wchar_t* idle,int kind)const{
        if(!m_exportProc||m_exportKind!=kind)return idle;
        return m_exportPct>=0?std::to_wstring(m_exportPct)+L"%":L"...";
    }
    std::wstring BtnLabel(int i)const{
        switch(i){
        case 0:return T(L"button.open");
        case 1:return L"-10";
        case 2:return m_playing?T(L"button.pause"):T(L"button.play");
        case 3:return T(L"button.stop");
        case 4:return L"+10";
        case 5:return m_muted?T(L"button.sound"):T(L"button.mute");
        case 6:return m_renderer&&m_renderer->DLSSEnabled()?L"DLSS ON":L"DLSS OFF";
        case 7:return m_fill?T(L"button.crop"):T(L"button.aspect");
        case 46:return L"AutoMask";
        case 9:return m_depthMapProc?L"Depth...":L"GenDepth";
        case 10:return L"MV";
        case 11:return T(L"button.full");
        case 12:return ExportBtnLabel(L"Export",0);
        case 13:return ExportBtnLabel(L"Compare",1);
        case 14:return ExportBtnLabel(L"4K",2);
        case 15:return ExportBtnLabel(L"Split",3);
        case 16:return L"Loop";
        case 17:return m_flowProc?L"Flow...":L"GenFlow";
        case 43:return m_maskProc?L"Mask...":L"GenMask";
        case 44:return L"MaskNR";
        case 45:return L"Reset";
        case 18:return L"Bypass";
        case 19:return L"LUT";
        case 20:return L"Sharp";
        case 21:return L"DepthMap";
        case 22:return L"Tone";
        case 23:return L"Depth";
        case 24:return L"Flow";
        case 26:return L"Mask";
        case 34:return m_svrProc?SeedVRButtonCaption():L"SeedVR";
        case 40:return m_exportRes==3?L"Res Native":m_exportRes==2?L"Res 8K":m_exportRes==1?L"Res 4K":L"Res Auto";
        case 41:return m_exportCodec==2?L"AV1":m_exportCodec==1?L"HEVC":L"H.264";
        case 42:return L"Frame";
        case 25:return L"Shot";
        case 27:{
            if(m_lutPath.empty())return L"Load LUT...";
            std::wstring s=std::filesystem::path(m_lutPath).stem().wstring();
            if(s.size()>12)s=s.substr(0,12)+L"...";
            return L"LUT: "+s;}
        case 28:return L"NR A";
        case 29:return L"NR B";
        case 30:return L"NR C";
        case 31:return L"Zero";
        case 32:return L"Pan";
        case 33:return L"Model";
        case 35:return L"Zoom \x2212"; // minus sign
        case 36:return L"Zoom +";
        case 37:return m_zoom<0.999f?(std::wstring(L"Fit (")+FormatZoom()+L")"):L"Fit";
        case 38:return L"A/B \x2194";   // vertical divider, drag left/right
        case 39:return L"A/B \x2195";   // horizontal divider, drag up/down
        }
        return L"";
    }
    std::wstring FormatZoom()const{wchar_t b[32]{};swprintf_s(b,L"%.1fx",m_zoom>0.0f?1.0f/m_zoom:1.0f);return b;}
    wchar_t BtnIcon(int i)const{
        switch(i){
        case 0:return L'\xE8E5';case 1:return L'\xEB9E';
        case 2:return m_playing?L'\xE769':L'\xE768';
        case 3:return L'\xE71A';case 4:return L'\xEB9D';
        case 5:return m_muted?L'\xE74F':L'\xE767';
        case 7:return L'\xE7A8';
        case 11:return m_fullscreen?L'\xE73F':L'\xE740';
        case 12:return L'\xE898';case 16:return L'\xE8EE';case 25:return L'\xE722';
        }
        return 0;
    }
    bool BtnActive(int i)const{
        switch(i){
        case 2:return m_playing;
        case 5:return m_muted;
        case 6:return m_renderer&&m_renderer->DLSSEnabled();
        case 46:return m_nrAutoMask;
        case 10:return m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::MotionVectors;
        case 23:return m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::Depth;
        case 26:return m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::BiasMask;
        case 11:return m_fullscreen;
        case 16:return m_loop;
        case 18:return m_bypassFX;
        case 19:return m_fxLut&&!m_bypassFX;
        case 20:return m_fxSharpen&&!m_bypassFX;
        case 21:return m_fxDepth&&!m_bypassFX;
        case 44:return m_fxMask&&!m_bypassFX;
        case 22:return m_fxTone&&!m_bypassFX;
        case 24:return m_fxFlow&&!m_bypassFX;
        case 28:return m_nrModelPick<=0; // unset (-1) resolves to style 0 = A
        case 29:return m_nrModelPick==1;
        case 30:return m_nrModelPick==2;
        case 31:return m_motionMode==0;
        case 32:return m_motionMode==1;
        case 33:return m_motionMode==2;
        case 37:return m_zoom<0.999f; // reset highlights while zoomed
        case 38:return m_compareOn&&m_compareOrient==1;
        case 39:return m_compareOn&&m_compareOrient==2;
        }
        return false;
    }
    bool BtnBusy(int i)const{
        switch(i){
        case 12:return m_exportProc&&m_exportKind==0;
        case 13:return m_exportProc&&m_exportKind==1;
        case 14:return m_exportProc&&m_exportKind==2;
        case 15:return m_exportProc&&m_exportKind==3;
        case 9:return m_depthMapProc!=nullptr;
        case 17:return m_flowProc!=nullptr;
        case 43:return m_maskProc!=nullptr;
        case 34:return m_svrProc!=nullptr;
        }
        return false;
    }

    // Lays out both regions from the CURRENT labels (states change text). Bar:
    // icon-only 40px pills, centered over the video area; text-width fallback if
    // the icon fonts are missing. Panel: per group, a caption line then measured
    // pills flowing left-to-right with wrap, one frame per group; then Levels.
    void LayoutBar(HDC dc){
        RECT c{};GetClientRect(m_hwnd,&c);
        m_barFrames.clear();
        auto measure=[&](int i)->int{
            if(BtnIconOnly(i)&&BtnIcon(i)&&m_fontIcon)return 34;
            const std::wstring t=BtnLabel(i);
            SIZE ts{};auto of=SelectObject(dc,m_font);GetTextExtentPoint32W(dc,t.c_str(),int(t.size()),&ts);SelectObject(dc,of);
            return ts.cx+20+((BtnIcon(i)&&m_fontIcon&&!BtnIconOnly(i))?18:0);
        };
        // Bottom bar: transport cluster centered over the video area.
        {
            int w[8]{},total=0;
            for(int p=0;p<8;++p){w[p]=measure(kBarOrder[p]);total+=w[p]+(p?6:0);}
            for(int p=1;p<8;++p)if(kBarGroup[p]!=kBarGroup[p-1])total+=14;
            const int barW=std::max(1,int(c.right)-SIDE_W);
            int x=int(VideoX0(c))+std::max(10,(barW-total)/2);
            const int top=int(c.bottom)-CONTROL_H+9;
            for(int p=0;p<8;++p){
                if(p){x+=6;if(kBarGroup[p]!=kBarGroup[p-1])x+=14;}
                m_btnRect[kBarOrder[p]]=RECT{x,top,x+w[p],top+28};
                x+=w[p];
            }
        }
        // Right panel: captioned framed groups on a UNIFORM two-column grid -
        // every button is the same size (long labels ellipsize; tooltips carry
        // the full text). Each group collapses/expands by clicking its caption
        // (chevron shows the state); collapse state persists in the ini.
        {
            const int px0=int(PanelX0(c))+12,px1=int(PanelX0(c))+SIDE_W-12;
            const int gut=6;
            const int colW=(px1-px0-12-gut)/2;
            int y=8-m_panelScroll;
            static constexpr COLORREF kGroupTint[7]={Pal::BlueInk,Pal::VioletInk,Pal::TealInk,Pal::GreenInk,Pal::SandInk,Pal::Ink2,Pal::BlueInk};
            for(int gi=0;gi<5;++gi){
                const PanelGroup&g=kPanelGroups[gi];
                const int frameTop=y;
                y+=24; // caption row: label, +/- and the rule under it (also the collapse hit row)
                if(m_groupOpen[gi]){
                    int col=0;
                    for(int n=0;n<g.count;++n){
                        const int i=g.items[n];
                        const int x=px0+6+(col?colW+gut:0);
                        m_btnRect[i]=RECT{x,y,x+colW,y+22};
                        if(++col==2){col=0;y+=26;}
                    }
                    if(col)y+=26;
                    y+=2;
                } else {
                    for(int n=0;n<g.count;++n)m_btnRect[g.items[n]]=RECT{0,0,0,0};
                }
                m_barFrames.push_back({RECT{px0-6,frameTop,px1+6,y},g.caption,gi,kGroupTint[gi]});
                y+=12;   // spacing is the only group separator now
            }
            // Two slider groups: Levels (the neural and effect strengths) and
            // Color (the picture adjustments the old Image adjustments window held).
            for(int sg=0;sg<2;++sg){
                const int group=5+sg,first=sg?kColorSliderFirst:0,last=sg?kSliderCount:kColorSliderFirst;
                if(sg)y+=12;
                const int frameTop=y;y+=22;
                if(m_groupOpen[group]){
                    for(int s=first;s<last;++s){
                        m_sliderRect[s]=RECT{px0+6,y,px1-6,y+20};
                        m_sliderTrack[s]=RECT{px0+64,y+7,px1-14,y+13};   // label column fits "Smooth"
                        y+=22;
                    }
                    y+=6;
                } else {
                    for(int s=first;s<last;++s){m_sliderRect[s]=RECT{0,0,0,0};m_sliderTrack[s]=RECT{0,0,0,0};}
                }
                m_barFrames.push_back({RECT{px0-6,frameTop,px1+6,y},sg?L"Color":L"Levels",group,kGroupTint[group]});
            }
            m_panelContentH=y+m_panelScroll+8;
            // A resize or a side switch must never leave the panel scrolled
            // past its content: clamp here, the next paint lays out from it.
            m_panelScroll=std::clamp(m_panelScroll,0,std::max(0,m_panelContentH-int(c.bottom)));
        }
    }

    // ---- Hover tooltips over the custom-drawn controls ----------------------
    // One TTF_SUBCLASS tool whose rect follows whatever button/slider the mouse
    // is over; text is served via TTN_GETDISPINFO from TipText().
    void EnsureTip(){
        if(m_tip)return;
        m_tip=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP|TTS_NOPREFIX,
                              CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,CW_USEDEFAULT,m_hwnd,nullptr,GetModuleHandleW(nullptr),nullptr);
        if(!m_tip)return;
        TOOLINFOW ti{};ti.cbSize=sizeof(ti);ti.uFlags=TTF_SUBCLASS;ti.hwnd=m_hwnd;ti.uId=1;ti.lpszText=LPSTR_TEXTCALLBACKW;
        SendMessageW(m_tip,TTM_ADDTOOLW,0,(LPARAM)&ti);
        SendMessageW(m_tip,TTM_SETMAXTIPWIDTH,0,340);
        SendMessageW(m_tip,TTM_SETDELAYTIME,TTDT_INITIAL,400);
        SendMessageW(m_tip,TTM_SETDELAYTIME,TTDT_AUTOPOP,12000);
    }
    void UpdateHoverTip(int x,int y){
        int id=-1;RECT r{};
        if(m_loaded&&m_dragSlider<0&&!m_dragSeek&&!m_dragVolume){
            for(int i=0;i<kBtnCount&&id<0;++i)
                if(m_btnRect[i].right>m_btnRect[i].left&&PtIn(m_btnRect[i],x,y)){id=i;r=m_btnRect[i];}
            if(id<0)for(int s=0;s<kSliderCount;++s)
                if(PtIn(m_sliderRect[s],x,y)){id=100+s;r=m_sliderRect[s];break;}
        }
        if(id==m_tipId)return;
        m_tipId=id;EnsureTip();if(!m_tip)return;
        TOOLINFOW ti{};ti.cbSize=sizeof(ti);ti.hwnd=m_hwnd;ti.uId=1;ti.rect=r;
        SendMessageW(m_tip,TTM_NEWTOOLRECTW,0,(LPARAM)&ti);
        SendMessageW(m_tip,TTM_POP,0,0); // hide the previous tip so the new text re-arms
    }
    const wchar_t* TipText(int id)const{
        switch(id){
        case 0:return L"Open a video file";
        case 1:return L"Back 10 seconds";
        case 2:return L"Play / pause";
        case 3:return L"Stop playback";
        case 4:return L"Forward 10 seconds";
        case 5:return L"Mute / unmute";
        case 6:return L"Toggle the DLSS neural pass on or off";
        case 7:return L"Aspect: fit the frame or crop-fill the window";
        case 46:return L"NR Auto Mask: the model's own character mask (A/B). The Struct and Skin levels apply only while it is on.";
        case 9:return L"Generate a Depth Anything V2 depth map (<name>_depth.mp4) beside the movie. Playback and exports attach it automatically.";
        case 10:return L"View the motion-vector field fed to the neural pass (colors = direction, brightness = speed)";
        case 11:return L"Fullscreen";
        case 12:return L"Export the movie through the neural pipeline at the preview resolution";
        case 13:return L"Export original and processed side by side with labels";
        case 14:return L"Export with a 3840 long side (auto quality). Enable '4K via Lanczos' in the FX menu for a crisp pre-scale.";
        case 15:return L"Export one movie, left half original, right half processed";
        case 16:return L"Loop playback";
        case 17:return L"Generate RAFT optical flow (<name>_flow.mp4) beside the movie. Used when Motion is set to Estimated.";
        case 45:return L"Reset every look control to its default: colour adjustments, all levels, the neural knobs, effect toggles on, Bypass off, Motion Zero. Keeps the loaded LUT, the model pick and the layout.";
        case 43:return L"Generate a text-prompted segmentation mask (SAM 3, or Grounding DINO + SAM 2.1 when SAM 3 is not available): <name>_mask.mp4 beside the movie plus one _mask_<phrase>.mp4 per phrase. White = process here. Press Mask to view it, MaskNR to bind it.";
        case 44:return m_maskLoaded?L"Bind the segmentation mask (<name>_mask.mp4) into the neural pass as ControlMask (A/B toggle). Compare against this OFF, never against a blank mask: binding alone changes the output.":L"Bind the segmentation mask into the neural pass - needs <name>_mask.mp4 beside the movie (GenMask).";
        case 18:return L"Bypass ALL effect additions for a clean A/B (key: B)";
        case 19:return L"Apply the loaded .cube LUT (A/B toggle)";
        case 20:return L"Pre-sharpen: micro-contrast boost on the input BEFORE the neural pass (A/B toggle)";
        case 21:return L"Use the model depth map (<name>_depth.mp4) instead of the built-in depth proxy (A/B toggle). This runtime currently ignores depth - kept for newer builds.";
        case 22:return L"Tone preserve: keep the neural detail but restore the original tone and colors (A/B toggle)";
        case 23:return L"View the depth guide fed to the neural pass (bright = near)";
        case 24:return L"Use the RAFT flow video as the motion-vector field when Motion is Estimated (A/B toggle)";
        case 25:return L"Save a side-by-side comparison shot of the current frame";
        case 26:return L"View the temporal uncertainty mask (bright = unreliable motion / disocclusion)";
        case 27:return m_lutPath.empty()?L"Load a .cube LUT file":L"Load a different .cube LUT. Right-click to remove the loaded LUT.";
        case 28:return L"Neural model A: strongest uplift";
        case 29:return L"Neural model B: subtle";
        case 30:return L"Neural model C: contrasty";
        case 31:return L"Motion: zero vectors - the cleanest, most stable option for most footage";
        case 32:return L"Motion: one global pan vector per frame (robust median)";
        case 33:return L"Motion: full estimated per-block field (RAFT flow when attached)";
        case 34:return L"Run SeedVR restoration (writes <name>_svr.mp4, then offers to load/export it). Strength comes from the SVR level below.";
        case 35:return L"Zoom out";
        case 36:return L"Zoom in (Ctrl+wheel also zooms at the cursor)";
        case 37:return L"Reset zoom to fit";
        case 38:return L"A/B compare with a draggable vertical split: pure original left, processed right";
        case 39:return L"A/B compare with a draggable horizontal split: pure original top, processed bottom";
        case 100:return L"Neural pass intensity: wet/dry blend against the original";
        case 101:return L"Structure: detail redraw strength (0..2). High values can boil on noisy sources.";
        case 102:return L"Pre-sharpen amount (before the neural pass)";
        case 103:return L"Post-sharpen amount (after the neural pass). Subtle at fit zoom - judge at 1:1.";
        case 104:return L"Tone preserve mix: 0 = raw neural output, 1 = original tone/colors fully restored";
        case 105:return L"NR Smooth: motion-compensated temporal smoothing of the neural delta, live and in exports.";
        case 106:return L"LUT strength";
        case 107:return L"SeedVR restoration strength (applies to the next SeedVR job)";
        case 108:return L"NR Skin: structure strength on skin (0..2). Leftmost third = follow Struct; 0 = off on skin.";
        case 109:return L"Brightness in EV stops (-2..+2), applied after the neural pass";
        case 110:return L"Contrast (0..3) around middle grey";
        case 111:return L"Saturation (0..3)";
        case 112:return L"Gamma (0.25..3)";
        case 113:return L"Colour temperature: -1 cool .. +1 warm";
        case 114:return L"Tint: -1 green .. +1 magenta";
        case 40:return L"Export resolution for Export/Compare/Split: preview size, 3840 or 7680 long side, or Native (the source size - no upscale, the neural pass runs 1:1 on the original pixels). The neural pass runs per output pixel, so 8K is slow and VRAM-hungry.";
        case 41:return L"Export encoder: H.264 (x264, most compatible), HEVC or AV1 (NVENC hardware - strongly recommended for 4K/8K).";
        case 42:return L"Save the current frame as a PNG with all modifications applied (<name>_dlss5_frame_N.png). Works on paused video and imported still images.";
        }
        return L"";
    }

    RECT TimelineRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{VideoX0(c)+18,c.bottom-24,VideoX1(c)-18,c.bottom-14};}
    RECT VolumeRect()const{RECT c{};GetClientRect(m_hwnd,&c);return RECT{VideoX1(c)-185,c.bottom-88,VideoX1(c)-95,c.bottom-80};}
    RECT EmptyOpenRect()const{RECT c{};GetClientRect(m_hwnd,&c);int cx=(c.left+c.right)/2,cy=(c.top+c.bottom)/2;return RECT{cx-95,cy+46,cx+95,cy+88};}
    bool PtIn(const RECT&r,int x,int y)const{return x>=r.left&&x<r.right&&y>=r.top&&y<r.bottom;}

    // Flat nordic pills: no border, quiet ink; active = frozen-blue soft fill
    // with accent text, busy = amber pair (reserved for running background jobs).
    // icon: Segoe Fluent/MDL2 codepoint. iconOnly pills draw the glyph alone
    // (label kept as text fallback if no icon font); text pills are sized by
    // LayoutBar to fit icon+label exactly, so nothing ever truncates.
    // role tints idle ink: 0 neutral, 1 picture (blue), 2 export/tools (violet).
    void DrawButton(HDC dc,const RECT&rin,const std::wstring&text,bool active=false,bool busy=false,wchar_t icon=0,bool iconOnly=false,int role=0,bool pressedIn=false){
        const bool pressed=pressedIn&&PtIn(rin,m_mouseX,m_mouseY); // dragging off cancels the press visual too
        RECT r=rin;
        if(pressed){r.left+=1;r.top+=1;r.right-=1;r.bottom-=1;}   // gentle press inset
        bool hover=PtIn(r,m_mouseX,m_mouseY);
        // Monochrome labels; only state carries colour (green active, amber
        // busy). Icon-only transport buttons sit bare on the ground until they
        // are hovered, pressed or active - the bar reads as icons, not chips.
        (void)role;
        const bool bare=iconOnly&&!pressed&&!busy&&!active&&!hover;
        COLORREF fill=pressed?Pal::AccentSoft:busy?Pal::AmberSoft:active?Pal::AccentSoft:hover?Pal::Hover:Pal::Raised;
        COLORREF ink=pressed?Pal::Accent:busy?Pal::Amber:active?Pal::Accent:hover?Pal::Ink:Pal::Ink2;
        if(!bare){
            HBRUSH b=CreateSolidBrush(fill);auto ob=SelectObject(dc,b),op=SelectObject(dc,GetStockObject(NULL_PEN));
            RoundRect(dc,r.left,r.top,r.right+1,r.bottom+1,14,14);
            SelectObject(dc,ob);SelectObject(dc,op);DeleteObject(b);
        }
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,ink);
        const bool useIcon=icon&&m_fontIcon;
        if(useIcon&&iconOnly){
            auto of=SelectObject(dc,m_fontIcon);const wchar_t g[2]={icon,0};RECT t=r;
            DrawTextW(dc,g,1,&t,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,of);
            return;
        }
        auto of=SelectObject(dc,m_font);
        SIZE ts{};GetTextExtentPoint32W(dc,text.c_str(),int(text.size()),&ts);
        const int iconW=useIcon?18:0;
        const int x=int(r.left)+std::max(0,int((r.right-r.left)-(ts.cx+iconW))/2);
        if(useIcon){
            SelectObject(dc,m_fontIcon);const wchar_t g[2]={icon,0};
            RECT ir{x,r.top,x+16,r.bottom};DrawTextW(dc,g,1,&ir,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_NOCLIP);
            SelectObject(dc,m_font);
        }
        RECT t{x+iconW,r.top,r.right-4,r.bottom};
        DrawTextW(dc,text.c_str(),-1,&t,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);
        SelectObject(dc,of);
    }

    void Paint(){
        // Double-buffered: everything is drawn into an offscreen bitmap and blitted
        // once. Unbuffered GDI repaints (hover invalidations fire on every mouse
        // move) are what made the control bar flicker; the video itself renders on
        // the GPU into its own child window and never flickered.
        PAINTSTRUCT ps{};HDC wdc=BeginPaint(m_hwnd,&ps);RECT c{};GetClientRect(m_hwnd,&c);
        const int W=std::max<LONG>(1,c.right),H=std::max<LONG>(1,c.bottom);
        HDC dc=CreateCompatibleDC(wdc);HBITMAP bmp=CreateCompatibleBitmap(wdc,W,H);HGDIOBJ obmp=SelectObject(dc,bmp);
        PaintInto(dc,c);
        BitBlt(wdc,ps.rcPaint.left,ps.rcPaint.top,std::max<LONG>(0,ps.rcPaint.right-ps.rcPaint.left),std::max<LONG>(0,ps.rcPaint.bottom-ps.rcPaint.top),dc,ps.rcPaint.left,ps.rcPaint.top,SRCCOPY);
        SelectObject(dc,obmp);DeleteObject(bmp);DeleteDC(dc);
        EndPaint(m_hwnd,&ps);
    }

    void PaintInto(HDC dc,const RECT& c){
        if(!m_loaded){
            HBRUSH bg=CreateSolidBrush(Pal::Bg);FillRect(dc,&c,bg);DeleteObject(bg);SetBkMode(dc,TRANSPARENT);
            RECT title{40,(c.bottom/2)-96,c.right-40,(c.bottom/2)-30};SetTextColor(dc,Pal::Ink);auto of=SelectObject(dc,m_fontTitle);std::wstring tt=T(L"idle.title");DrawTextW(dc,tt.c_str(),-1,&title,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            RECT sub{40,(c.bottom/2)-22,c.right-40,(c.bottom/2)+19};SetTextColor(dc,Pal::Muted);SelectObject(dc,m_fontSmall);std::wstring ss=T(L"idle.subtitle");DrawTextW(dc,ss.c_str(),-1,&sub,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,of);
            DrawButton(dc,EmptyOpenRect(),T(L"idle.open"),true);return;
        }
        {HBRUSH fill=CreateSolidBrush(Pal::Bg);FillRect(dc,&c,fill);DeleteObject(fill);}
        // The bar shares the video column's ground - one flat surface under
        // the picture, no dividing line.
        RECT bar{VideoX0(c),c.bottom-CONTROL_H,VideoX1(c),c.bottom};{HBRUSH bg=CreateSolidBrush(Pal::Bg);FillRect(dc,&bar,bg);DeleteObject(bg);}
        LayoutBar(dc);
        // Control panel ground (full height, left or right) + hairline on the
        // edge that faces the video.
        RECT panel{PanelX0(c),0,PanelX0(c)+SIDE_W,c.bottom};
        {HBRUSH pb=CreateSolidBrush(Pal::Surface);FillRect(dc,&panel,pb);DeleteObject(pb);}   // flat: the tone step is the edge
        // Panel content is wheel-scrollable: clip so it never bleeds into the bar.
        const int panelClip=SaveDC(dc);
        IntersectClipRect(dc,panel.left+1,panel.top,panel.right,panel.bottom);
        // Groups are separated by spacing alone: no frames. Each caption is a
        // quiet uppercase, letter-spaced label with a small chevron for the
        // collapse state (the reference look's "SUBSCRIPTIONS" treatment).
        {SetBkMode(dc,TRANSPARENT);auto of=SelectObject(dc,m_fontSmall);
         const int oldExtra=SetTextCharacterExtra(dc,1);
         for(const BarFrame&f:m_barFrames)if(f.caption){
            const bool open=f.group>=0&&f.group<7&&m_groupOpen[f.group];
            const bool capHover=PtIn(RECT{f.r.left,f.r.top,f.r.right,f.r.top+24},m_mouseX,m_mouseY);
            SetTextColor(dc,capHover?Pal::Ink:Pal::Muted);
            std::wstring cap=f.caption;std::transform(cap.begin(),cap.end(),cap.begin(),::towupper);
            // Label on the left, ellipsized before it can reach the +/- on the
            // right; a hairline under the heading closes the row.
            RECT cr{f.r.left+8,f.r.top+3,f.r.right-24,f.r.top+19};
            DrawTextW(dc,cap.c_str(),-1,&cr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS|DT_NOPREFIX);   // NOPREFIX: "Export & Jobs" keeps its ampersand
            RECT sr{f.r.right-22,f.r.top+3,f.r.right-8,f.r.top+19};
            DrawTextW(dc,open?L"\u2212":L"+",-1,&sr,DT_RIGHT|DT_VCENTER|DT_SINGLELINE|DT_NOPREFIX);
            RECT rule{f.r.left+8,f.r.top+21,f.r.right-8,f.r.top+22};
            HBRUSH rb=CreateSolidBrush(Pal::Line);FillRect(dc,&rule,rb);DeleteObject(rb);}
         SetTextCharacterExtra(dc,oldExtra);
         SelectObject(dc,of);}
        for(int i=0;i<kBtnCount;++i)if(!IsBarBtn(i)&&m_btnRect[i].right>m_btnRect[i].left)DrawButton(dc,m_btnRect[i],BtnLabel(i),BtnActive(i),BtnBusy(i),BtnIcon(i),BtnIconOnly(i),BtnRole(i),m_pressBtn==i);
        // Levels and Color: draggable sliders.
        // While a slider is dragged (or hovered), its label swaps to the live
        // numeric value so tuning is never blind.
        {auto of=SelectObject(dc,m_fontSmall);SetBkMode(dc,TRANSPARENT);
         for(int s=0;s<kSliderCount;++s){
            const RECT&wr=m_sliderRect[s];const RECT&tk=m_sliderTrack[s];
            const bool showVal=(s==m_dragSlider)||(m_dragSlider<0&&PtIn(wr,m_mouseX,m_mouseY));
            // The label never changes: the live value floats in a chip above
            // the knob instead, so you always see both what and how much.
            SetTextColor(dc,showVal?Pal::Ink:Pal::Ink2);
            RECT lr{wr.left,wr.top,tk.left-6,wr.bottom};
            DrawTextW(dc,kSliderLabel[s],-1,&lr,DT_LEFT|DT_VCENTER|DT_SINGLELINE);
            const int cy=(tk.top+tk.bottom)/2;
            RECT track{tk.left,cy-1,tk.right,cy+1};HBRUSH tb=CreateSolidBrush(Pal::Line);FillRect(dc,&track,tb);DeleteObject(tb);
            RECT done=track;done.right=track.left+LONG((track.right-track.left)*std::clamp(GetSliderVal(s),0.0f,1.0f));
            HBRUSH db=CreateSolidBrush(Pal::Accent);FillRect(dc,&done,db);DeleteObject(db);
            const int kr=(s==m_dragSlider)?6:showVal?5:4;   // thin track, small knob; grows a little on hover/drag
            HBRUSH kb=CreateSolidBrush(s==m_dragSlider?Pal::Accent:Pal::Ink);auto okb=SelectObject(dc,kb),okp=SelectObject(dc,GetStockObject(NULL_PEN));
            Ellipse(dc,done.right-kr,cy-kr,done.right+kr+1,cy+kr+1);SelectObject(dc,okb);SelectObject(dc,okp);DeleteObject(kb);
            if(showVal){
                const std::wstring vtxt=SliderValueText(s);const wchar_t* vbuf=vtxt.c_str();
                SIZE vs{};GetTextExtentPoint32W(dc,vbuf,int(wcslen(vbuf)),&vs);
                RECT chip{done.right-vs.cx/2-6,cy-kr-17,done.right+vs.cx/2+6,cy-kr-3};
                if(chip.left<wr.left){chip.right+=wr.left-chip.left;chip.left=wr.left;}
                if(chip.right>wr.right){chip.left-=chip.right-wr.right;chip.right=wr.right;}
                HBRUSH cb=CreateSolidBrush(Pal::Hover);auto ocb=SelectObject(dc,cb),ocp=SelectObject(dc,GetStockObject(NULL_PEN));
                RoundRect(dc,chip.left,chip.top,chip.right+1,chip.bottom+1,8,8);SelectObject(dc,ocb);SelectObject(dc,ocp);DeleteObject(cb);
                SetTextColor(dc,s==m_dragSlider?Pal::Accent:Pal::Ink);DrawTextW(dc,vbuf,-1,&chip,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            }}
         SelectObject(dc,of);}
        RestoreDC(dc,panelClip);
        for(int i=0;i<kBtnCount;++i)if(IsBarBtn(i))DrawButton(dc,m_btnRect[i],BtnLabel(i),BtnActive(i),BtnBusy(i),BtnIcon(i),BtnIconOnly(i),BtnRole(i),m_pressBtn==i);
        if(m_exportProc){
            // Live export progress strip along the top edge of the control bar.
            const int pw=m_exportPct>0?int(double(c.right)*m_exportPct/100.0):std::max(24,int(c.right)/20);
            RECT prog{0,bar.top,pw,bar.top+3};
            HBRUSH pb=CreateSolidBrush(Pal::Amber);FillRect(dc,&prog,pb);DeleteObject(pb);
        }
        RECT vr=VolumeRect();HPEN vp=CreatePen(PS_SOLID,2,Pal::Line);auto op=SelectObject(dc,vp);MoveToEx(dc,vr.left,(vr.top+vr.bottom)/2,nullptr);LineTo(dc,vr.right,(vr.top+vr.bottom)/2);SelectObject(dc,op);DeleteObject(vp);
        int vx=vr.left+int((vr.right-vr.left)*(m_muted?0.0f:m_volume));
        {HBRUSH vb=CreateSolidBrush(Pal::Ink);auto ovb=SelectObject(dc,vb),ovp=SelectObject(dc,GetStockObject(NULL_PEN));Ellipse(dc,vx-4,(vr.top+vr.bottom)/2-4,vx+5,(vr.top+vr.bottom)/2+5);SelectObject(dc,ovb);SelectObject(dc,ovp);DeleteObject(vb);}
        double shown=m_dragSeek?m_seekPreview:(m_seekPending?m_pendingSeekSec:Position());RECT tr=TimelineRect();
        {RECT track{tr.left,(tr.top+tr.bottom)/2-2,tr.right,(tr.top+tr.bottom)/2+2};HBRUSH tb=CreateSolidBrush(Pal::Line);FillRect(dc,&track,tb);DeleteObject(tb);
         double d2=m_decoder.DurationSeconds(),f=d2>0?std::clamp(shown/d2,0.0,1.0):0;RECT done=track;done.right=done.left+int((done.right-done.left)*f);
         HBRUSH db=CreateSolidBrush(Pal::Accent);FillRect(dc,&done,db);DeleteObject(db);
         int kx=done.right;HBRUSH kb=CreateSolidBrush(Pal::Ink);auto okb=SelectObject(dc,kb),okp=SelectObject(dc,GetStockObject(NULL_PEN));Ellipse(dc,kx-6,(tr.top+tr.bottom)/2-6,kx+7,(tr.top+tr.bottom)/2+7);SelectObject(dc,okb);SelectObject(dc,okp);DeleteObject(kb);}
        double d=m_decoder.DurationSeconds();
        SetBkMode(dc,TRANSPARENT);SetTextColor(dc,Pal::Ink2);auto of=SelectObject(dc,m_fontSmall);std::wstring time=TimeText(shown)+L" / "+TimeText(d);TextOutW(dc,int(VideoX0(c))+18,c.bottom-50,time.c_str(),int(time.size()));
        std::wstringstream st;if(m_seeking||m_seekPending)st<<T(L"status.seeking")<<L"  |  ";
        if(m_exportProc){st<<L"EXPORTING ";if(m_exportPct>=0)st<<m_exportPct<<L"% (frame "<<m_exportFrames<<L"/~"<<m_exportTotal<<L")";else st<<L"starting...";st<<L"  |  ";}
        if(m_svrProc){st<<L"SEEDVR "<<SeedVRStatusText()<<L"  |  ";}
        // The MV view shows the estimated field even in Motion=Zero; make clear
        // that NR is still being fed zero vectors in that mode.
        if(m_motionMode==0&&m_renderer&&m_renderer->GetDebugView()==D3D12Renderer::DebugView::MotionVectors)st<<L"MV VIEW: estimated field shown; Motion is Zero, so NR receives zero vectors  |  ";
        if(m_bypassFX)st<<L"FX BYPASS (B)  |  ";
        // Engine truth: which NR model is live, and how many frames it has run.
        if(!m_renderer->DLSSEnabled())st<<L"NR OFF";
        else if(m_renderer->DLSSFeatureCreated())
            st<<L"NR Model "<<(m_nrModelPick==1?L"B":m_nrModelPick==2?L"C":L"A")<<L" ACTIVE \u00B7 "<<m_renderer->DLSSEvaluations()<<L" frames";
        else st<<L"NR starting...";
        st<<L"  |  "<<m_decoder.NativeWidth()<<L"x"<<m_decoder.NativeHeight();
        if(m_decoder.Width()!=m_decoder.NativeWidth()||m_decoder.Height()!=m_decoder.NativeHeight())st<<L" -> "<<m_decoder.Width()<<L"x"<<m_decoder.Height();
        st<<L" -> "<<m_renderer->OutputW()<<L"x"<<m_renderer->OutputH();
        st<<L"  |  fps "<<int(std::lround(m_submitFps))<<L"/"<<int(std::lround(m_decoder.FrameRate()));
        if(m_droppedFrames)st<<L" \u00B7 drop "<<m_droppedFrames;
        if(m_zoom<0.999f)st<<L"  |  ZOOM "<<std::fixed<<std::setprecision(1)<<(1.0f/m_zoom)<<L"x (drag to pan, right-click resets)";
        if(m_compareOn)st<<L"  |  A/B "<<(m_compareOrient==1?L"vertical":L"horizontal")<<L" (drag the split)";
        std::wstring status=st.str();SetTextColor(dc,Pal::Muted);RECT sr{VideoX0(c)+145,c.bottom-53,VideoX1(c)-24,c.bottom-34};DrawTextW(dc,status.c_str(),-1,&sr,DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);SetTextColor(dc,Pal::Ink2);std::wstring vol=m_muted?T(L"status.muted"):(T(L"status.volume")+L" "+std::to_wstring(int(m_volume*100))+L"%");TextOutW(dc,vr.right+8,vr.top-6,vol.c_str(),int(vol.size()));SelectObject(dc,of);
    }

    void RegisterOverlayHotkeys(){
        // WM_HOTKEY is posted by Windows independently of the swapchain WndProc.
        auto reg=[&](int id,UINT mods,UINT vk,const char* name){if(!RegisterHotKey(m_hwnd,id,mods|MOD_NOREPEAT,vk))LOG("Overlay hotkey unavailable: "<<name<<" winerr="<<GetLastError());};
        reg(HK_PLAY_PAUSE,MOD_CONTROL|MOD_ALT,VK_SPACE,"Ctrl+Alt+Space");
        reg(HK_BACK_10,MOD_CONTROL|MOD_ALT,VK_LEFT,"Ctrl+Alt+Left");
        reg(HK_FORWARD_10,MOD_CONTROL|MOD_ALT,VK_RIGHT,"Ctrl+Alt+Right");
        reg(HK_MUTE,MOD_CONTROL|MOD_ALT,'M',"Ctrl+Alt+M");
        reg(HK_DLSS,MOD_CONTROL|MOD_ALT,'D',"Ctrl+Alt+D");
        if(!RegisterHotKey(m_hwnd,HK_MEDIA_PLAY_PAUSE,MOD_NOREPEAT,VK_MEDIA_PLAY_PAUSE))LOG("Media Play/Pause hotkey unavailable winerr="<<GetLastError());
    }
    void UnregisterOverlayHotkeys(){if(!m_hwnd)return;for(int id:{HK_PLAY_PAUSE,HK_BACK_10,HK_FORWARD_10,HK_MUTE,HK_DLSS,HK_MEDIA_PLAY_PAUSE})UnregisterHotKey(m_hwnd,id);}
    void HandleHotkey(int id){
        switch(id){case HK_PLAY_PAUSE:case HK_MEDIA_PLAY_PAUSE:TogglePause();break;case HK_BACK_10:RequestSeek(Position()-10);break;case HK_FORWARD_10:RequestSeek(Position()+10);break;case HK_MUTE:ToggleMute();break;case HK_DLSS:ToggleDLSS();break;}
    }

    void OpenFromDialog(){auto p=PickVideoFile(m_hwnd,m_loc);if(!p.empty())Load(p);}

    // Export the loaded movie through SmackMyRezUpExport.exe (the headless exporter that
    // ships beside this player). Runs as a background child process so playback
    // stays live; the busy button shows live percent until it finishes.
    // All kinds export WHAT THE PREVIEW SHOWS: the same DLSS output resolution and
    // quality mode the live renderer uses, raw NR tone, estimated motion vectors.
    // kind: 0 = plain export, 1 = compare (original | processed side by side),
    //       2 = 4K (forces a 3840 long side regardless of the preview box),
    //       3 = split (one frame, left half original, right half processed).
    void StartExport(int kind){
        if(!m_loaded||m_path.empty()||m_exportProc||!m_renderer)return;
        const std::filesystem::path tool=smru::paths::ExeDirectory()/smru::kExporterExeW;
        if(!std::filesystem::exists(tool)){MessageBoxW(m_hwnd,(std::wstring(smru::kExporterExeW)+L" was not found next to the player.").c_str(),T(L"app.title").c_str(),MB_ICONERROR);return;}
        uint32_t upW=m_renderer->OutputW(),upH=m_renderer->OutputH();
        // Res selector (Export & Jobs): Export/Compare/Split render at the
        // chosen long side instead of the preview box. The 4K button keeps
        // forcing 3840.
        if(kind!=2&&m_exportRes>0){
            const uint32_t nw=m_decoder.NativeWidth(),nh=m_decoder.NativeHeight();
            if(nw&&nh){
                // Native keeps the source size, so the neural pass redraws the
                // original pixels 1:1 - sharper than upscaling and scaling back
                // down.
                const double longSide=m_exportRes==3?double(std::max(nw,nh)):(m_exportRes==2?7680.0:3840.0);
                const double s=longSide/double(std::max(nw,nh));
                upW=uint32_t(std::lround(nw*s))&~1u;upH=uint32_t(std::lround(nh*s))&~1u;
            }
        }
        if(kind==2){
            const uint32_t nw=m_decoder.NativeWidth(),nh=m_decoder.NativeHeight();
            if(!nw||!nh)return;
            const double s=3840.0/double(std::max(nw,nh));
            upW=uint32_t(std::lround(nw*s))&~1u;upH=uint32_t(std::lround(nh*s))&~1u;
        }
        std::filesystem::path in(m_path);
        const wchar_t* suffix=kind==1?smru::kExportCompareSuffix:(kind==2?smru::kExport4KSuffix:(kind==3?smru::kExportSplitSuffix:smru::kExportSuffix));
        std::wstring defName=in.stem().wstring()+suffix;
        wchar_t out[32768]{};wcsncpy_s(out,defName.c_str(),_TRUNCATE);
        OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.hwndOwner=m_hwnd;o.lpstrFile=out;o.nMaxFile=static_cast<DWORD>(std::size(out));
        o.lpstrFilter=L"MP4 video\0*.mp4\0All files\0*.*\0\0";o.nFilterIndex=1;o.lpstrDefExt=L"mp4";
        std::wstring initialDir=in.parent_path().wstring();o.lpstrInitialDir=initialDir.empty()?nullptr:initialDir.c_str();
        o.Flags=OFN_OVERWRITEPROMPT|OFN_PATHMUSTEXIST|OFN_EXPLORER|OFN_NOCHANGEDIR;
        if(!GetSaveFileNameW(&o))return;
        std::wstringstream cmd;
        cmd<<L"\""<<tool.wstring()<<L"\" --input \""<<m_path<<L"\" --export \""<<out<<L"\"";
        if(kind==1)cmd<<L" --compare";
        if(kind==3)cmd<<L" --split";
        cmd<<L" --output-size "<<upW<<L"x"<<upH;
        // Forward the player's current EFFECTIVE look (per-effect enables and
        // Bypass-All respected) so the export matches exactly what the preview shows.
        if(FxTone()>0.0f)cmd<<L" --tone preserve --tone-mix "<<std::fixed<<std::setprecision(2)<<FxTone();
        else cmd<<L" --tone nr";
        cmd<<L" --sharpen "<<std::fixed<<std::setprecision(2)<<FxSharpen();
        cmd<<L" --post-sharpen "<<std::fixed<<std::setprecision(2)<<FxPostSharpen();
        // Lanczos pre-scale applies to any upscaling export when the toggle is
        // on: a crisp deterministic scaler in front of the NR detail pass,
        // instead of the bilinear default.
        if(m_lanczos4K&&(upW>m_decoder.NativeWidth()||upH>m_decoder.NativeHeight()))cmd<<L" --scaler lanczos";
        if(FxNrSmooth()>0.001f)cmd<<L" --nr-smooth "<<std::fixed<<std::setprecision(2)<<FxNrSmooth();
        cmd<<L" --mv "<<(m_motionMode==0?L"zero":(m_motionMode==1?L"global":L"estimated"));
        cmd<<L" --codec "<<(m_exportCodec==2?L"av1":m_exportCodec==1?L"hevc":L"x264");
        // Direct-engine NR knobs: exports mirror exactly what the preview shows.
        cmd<<L" --nr-style "<<(m_nrModelPick>=0?m_nrModelPick:0)
           <<L" --nr-intensity "<<std::fixed<<std::setprecision(2)<<m_nrIntensity
           <<L" --nr-structure "<<std::fixed<<std::setprecision(2)<<m_nrLocalStructure
           <<L" --nr-skin "<<std::fixed<<std::setprecision(2)<<m_nrSkinStructure
           <<L" --nr-automask "<<(m_nrAutoMask?L"on":L"off");
        // Only a LUT the preview actually loaded is forwarded: the export must
        // match what is on screen, and a dead path would abort the exporter.
        if(m_lutLoaded&&FxLutStrength()>0.0f)cmd<<L" --lut \""<<m_lutPath<<L"\" --lut-strength "<<std::fixed<<std::setprecision(2)<<FxLutStrength();
        {const std::wstring fpv=FlowVideoPathFor(m_path);
         if(FxFlowOn()&&std::filesystem::exists(fpv))cmd<<L" --flow-video \""<<fpv<<L"\"";}
        // Depth sidecar (Depth Anything V2, <stem>_depth.mp4): reattached so the
        // exporter writes real model depth into the NGX depth resource whenever
        // the file exists and the Depth A/B toggle is on.
        {const std::wstring dpv=DepthVideoPathFor(m_path);
         if(FxDepthOn()&&std::filesystem::exists(dpv))cmd<<L" --depth-video \""<<dpv<<L"\"";}
        // Segmentation mask (<stem>_mask.mp4): forwarded, and bound into the NR
        // pass, only while MaskNR is on - the export matches the preview A/B.
        {const std::wstring mpv=MaskVideoPathFor(m_path);
         if(FxMaskOn()&&std::filesystem::exists(mpv))cmd<<L" --mask-video \""<<mpv<<L"\" --nr-guides mv,mask";}
        // The Color window and the DLSS toggle: forwarded whenever they differ
        // from the defaults, so the export is exactly what the preview showed.
        {const D3D12Renderer::ColorSettings d{};const auto&cs=m_colorSettings;
         auto fwd=[&](const wchar_t* flag,float v,float def){if(std::fabs(v-def)>0.0005f)cmd<<L" "<<flag<<L" "<<std::fixed<<std::setprecision(3)<<v;};
         fwd(L"--brightness",cs.brightness,d.brightness);fwd(L"--contrast",cs.contrast,d.contrast);fwd(L"--saturation",cs.saturation,d.saturation);
         fwd(L"--gamma",cs.gamma,d.gamma);fwd(L"--temperature",cs.temperature,d.temperature);fwd(L"--tint",cs.tint,d.tint);}
        if(!m_renderer->DLSSEnabled())cmd<<L" --dlss off";
        // The exporter writes its own log into its CWD; keep the child in the
        // temp folder so nothing lands beside the movie or the player.
        wchar_t tmpDir[MAX_PATH]{};GetTempPathW(MAX_PATH,tmpDir);
        // Capture the exporter's output so PollExport can parse "[smru] frame N"
        // lines into a live progress bar. The pipe is drained every Tick.
        smru::proc::Options spawn;
        spawn.out=smru::proc::Stdio::Pipe;spawn.err=smru::proc::Stdio::Pipe;
        spawn.workingDir=tmpDir[0]?tmpDir:nullptr;
        smru::proc::Child exporter;
        if(!smru::proc::Spawn(cmd.str(),spawn,exporter)){
            MessageBoxW(m_hwnd,L"Could not start the export process.",T(L"app.title").c_str(),MB_ICONERROR);return;
        }
        m_exportProc=exporter.process;m_exportErrRead=exporter.stdOut;m_exportKind=kind;m_exportOutPath=out;
        m_exportBuf.clear();m_exportFrames=0;m_exportPct=-1;
        const double dur=m_decoder.DurationSeconds(),fr=m_decoder.FrameRate();
        m_exportTotal=uint64_t(std::max(1.0,dur>0?dur*std::max(1.0,fr):1.0));
        // The full command line is the only way to tell afterwards which look
        // an export was actually asked for (the exporter's own log is in %TEMP%).
        LOG("GUI export started kind="<<kind<<" total_frames_estimate="<<m_exportTotal<<" cmd="<<smru::text::WideToUtf8(cmd.str()));
        InvalidateControls();
    }

    static std::wstring FlowVideoPathFor(const std::wstring& moviePath){
        std::filesystem::path in(moviePath);
        return (in.parent_path()/(in.stem().wstring()+smru::kSidecarFlowSuffix)).wstring();
    }

    static std::wstring DepthVideoPathFor(const std::wstring& moviePath){
        std::filesystem::path in(moviePath);
        return (in.parent_path()/(in.stem().wstring()+smru::kSidecarDepthSuffix)).wstring();
    }

    static std::wstring MaskVideoPathFor(const std::wstring& moviePath){
        std::filesystem::path in(moviePath);
        return (in.parent_path()/(in.stem().wstring()+smru::kSidecarMaskSuffix)).wstring();
    }

    // Locates the Python interpreter and a script from the tools folder (see
    // AppPaths.h) and tells the user exactly what is missing when either
    // cannot be found.
    bool ResolveTool(const wchar_t* scriptName,const wchar_t* what,std::filesystem::path& python,std::filesystem::path& script){
        python=smru::paths::PythonExecutable();
        if(python.empty()){
            MessageBoxW(m_hwnd,(std::wstring(what)+L" needs a Python with torch installed (ComfyUI's embedded python.exe works).\nSet "
                +smru::kEnvPython+L", or [Tools] Python= in "+smru::kSettingsFile+L".").c_str(),T(L"app.title").c_str(),MB_ICONERROR);
            return false;
        }
        const std::filesystem::path tools=smru::paths::ToolsDirectory();
        std::error_code ec;
        script=tools.empty()?std::filesystem::path():tools/scriptName;
        if(script.empty()||!std::filesystem::is_regular_file(script,ec)){
            MessageBoxW(m_hwnd,(std::wstring(what)+L" needs tools\\"+scriptName+L".\nKeep the tools folder beside the exe or the checkout, or set "+smru::kEnvToolsDir+L".").c_str(),T(L"app.title").c_str(),MB_ICONERROR);
            return false;
        }
        return true;
    }

    // Runs Depth Anything V2 Small (the project's default depth model) through
    // the configured Python, producing <stem>_depth.mp4 (grayscale, bright =
    // near) beside the movie. Live playback and exports auto-attach that file.
    void StartDepthMapGen(){
        if(!m_loaded||m_path.empty()||m_depthMapProc)return;
        std::filesystem::path python,script;
        if(!ResolveTool(smru::kToolDepthScript,L"The depth generator",python,script))return;
        std::wstringstream cmd;
        cmd<<L"\""<<python.wstring()<<L"\" \""<<script.wstring()<<L"\" \""<<m_path<<L"\"";
        wchar_t tmpDir[MAX_PATH]{};GetTempPathW(MAX_PATH,tmpDir);
        smru::proc::Options spawn;spawn.workingDir=tmpDir[0]?tmpDir:nullptr;
        smru::proc::Child job;
        if(!smru::proc::Spawn(cmd.str(),spawn,job)){
            MessageBoxW(m_hwnd,L"Could not start the depth map generator.",T(L"app.title").c_str(),MB_ICONERROR);return;
        }
        m_depthMapProc=job.process;m_depthMapForPath=m_path;
        LOG("Depth Anything depth-map generation started.");
        InvalidateControls();
    }

    void PollDepthMapGen(){
        if(!m_depthMapProc)return;
        if(WaitForSingleObject(m_depthMapProc,0)!=WAIT_OBJECT_0)return;
        DWORD ec=1;GetExitCodeProcess(m_depthMapProc,&ec);
        CloseHandle(m_depthMapProc);m_depthMapProc=nullptr;
        const std::wstring dpv=DepthVideoPathFor(m_depthMapForPath);
        if(ec==0&&std::filesystem::exists(dpv)&&m_loaded&&m_path==m_depthMapForPath){
            // External-depth GPU resources only exist when the renderer was
            // initialized with them, so a reload is the clean way to attach.
            LOG("Depth map ready; reloading to attach it to live playback.");
            ReloadKeepingPosition();
        } else {
            LOG("Depth map generation finished ec="<<ec);
        }
        InvalidateControls();
    }

    // Runs RAFT optical flow on the loaded movie through the configured Python,
    // producing <stem>_flow.mp4 beside it. Live playback and exports auto-attach
    // that file as the per-pixel MV field.
    void StartFlowGen(){
        if(!m_loaded||m_path.empty()||m_flowProc)return;
        std::filesystem::path python,script;
        if(!ResolveTool(smru::kToolFlowScript,L"The flow generator",python,script))return;
        std::wstringstream cmd;
        cmd<<L"\""<<python.wstring()<<L"\" \""<<script.wstring()<<L"\" \""<<m_path<<L"\" --size 512";
        wchar_t tmpDir[MAX_PATH]{};GetTempPathW(MAX_PATH,tmpDir);
        smru::proc::Options spawn;spawn.workingDir=tmpDir[0]?tmpDir:nullptr;
        smru::proc::Child job;
        if(!smru::proc::Spawn(cmd.str(),spawn,job)){
            MessageBoxW(m_hwnd,L"Could not start the flow generator.",T(L"app.title").c_str(),MB_ICONERROR);return;
        }
        m_flowProc=job.process;
        LOG("RAFT flow generation started.");
        InvalidateControls();
    }

    void PollFlowGen(){
        if(!m_flowProc)return;
        if(WaitForSingleObject(m_flowProc,0)!=WAIT_OBJECT_0)return;
        DWORD ec=1;GetExitCodeProcess(m_flowProc,&ec);
        CloseHandle(m_flowProc);m_flowProc=nullptr;
        InvalidateControls();
        const std::wstring fp=FlowVideoPathFor(m_path);
        const bool okGen=(ec==0&&std::filesystem::exists(fp));
        std::wstring msg=okGen
            ?(L"RAFT flow created:\n"+fp+L"\n\nIt is now attached to playback - press MV to see the field live.\nExports attach it automatically. It replaces the block-matcher motion\nvectors (the one guide that actually matters).")
            :(L"Flow generation failed (exit "+std::to_wstring(ec)+L"). First run downloads the model - check the network.");
        MessageBoxW(m_hwnd,msg.c_str(),T(L"app.title").c_str(),okGen?MB_ICONINFORMATION:MB_ICONERROR);
        if(okGen)ReloadKeepingPosition(); // re-load so the live renderer picks the map up
    }

    // Text-prompted segmentation through the configured Python: SAM 3 segments
    // from the phrase directly, and the tool falls back to Grounding DINO +
    // SAM 2.1 when SAM 3's gated weights are not reachable. <stem>_mask.mp4 beside the
    // movie, plus one <stem>_mask_<phrase>.mp4 per phrase for later per-object
    // control. The union attaches to playback and exports; MaskNR binds it.
    void StartMaskGen(){
        if(!m_loaded||m_path.empty()||m_maskProc)return;
        std::filesystem::path python,script;
        if(!ResolveTool(smru::kToolMaskScript,L"The mask generator",python,script))return;
        std::wstring prompt=m_maskPrompt.empty()?L"person. face.":m_maskPrompt;
        // Both backends take the same knob: how eagerly a match is accepted
        // (SAM 3's instance score, or Grounding DINO's box score). Chosen in the
        // prompt box and remembered.
        static const std::vector<std::wstring> kSensitivity={
            L"Strict  -  only confident matches",
            L"Balanced  -  the model's own default",
            L"Loose  -  more matches, more false positives"};
        static const wchar_t* kSensitivityFlags[3]={L" --threshold 0.45",L" --threshold 0.30",L" --threshold 0.20"};
        int sensitivity=std::clamp(m_maskSensitivity,0,2);
        if(!PromptText(m_hwnd,L"Generate segmentation mask",
                       L"What to mask - phrases separated by periods, e.g.  person. face. hair.",prompt,
                       L"Sensitivity",&kSensitivity,&sensitivity))return;
        prompt.erase(std::remove(prompt.begin(),prompt.end(),L'"'),prompt.end()); // goes inside quotes on the command line
        if(prompt.find_first_not_of(L" .,")==std::wstring::npos)return;
        m_maskPrompt=prompt;m_maskSensitivity=std::clamp(sensitivity,0,2);SaveVideoSettings();
        std::wstringstream cmd;
        cmd<<L"\""<<python.wstring()<<L"\" \""<<script.wstring()<<L"\" \""<<m_path<<L"\" --prompt \""<<prompt<<L"\" --layers"<<kSensitivityFlags[m_maskSensitivity];
        wchar_t tmpDir[MAX_PATH]{};GetTempPathW(MAX_PATH,tmpDir);
        smru::proc::Options spawn;spawn.workingDir=tmpDir[0]?tmpDir:nullptr;
        smru::proc::Child job;
        if(!smru::proc::Spawn(cmd.str(),spawn,job)){
            MessageBoxW(m_hwnd,L"Could not start the mask generator.",T(L"app.title").c_str(),MB_ICONERROR);return;
        }
        m_maskProc=job.process;m_maskForPath=m_path;
        LOG("Segmentation mask generation started: prompt="<<smru::text::WideToUtf8(prompt));
        InvalidateControls();
    }

    void PollMaskGen(){
        if(!m_maskProc)return;
        if(WaitForSingleObject(m_maskProc,0)!=WAIT_OBJECT_0)return;
        DWORD ec=1;GetExitCodeProcess(m_maskProc,&ec);
        CloseHandle(m_maskProc);m_maskProc=nullptr;
        InvalidateControls();
        const std::wstring mp=MaskVideoPathFor(m_maskForPath);
        const bool ok=(ec==0&&std::filesystem::exists(mp));
        if(ok){
            LOG("Segmentation mask ready; reloading to attach it.");
            // The mask texture only exists when the renderer was initialized
            // with it (same as depth/flow), so a reload is the clean attach.
            if(m_loaded&&m_path==m_maskForPath)ReloadKeepingPosition();
            MessageBoxW(m_hwnd,(L"Mask created:\n"+mp+L"\n\nPress Mask to see it, and MaskNR to bind it into the neural pass (A/B).\nOne extra _mask_<phrase>.mp4 was written per phrase.").c_str(),T(L"app.title").c_str(),MB_ICONINFORMATION);
        } else {
            MessageBoxW(m_hwnd,(L"Mask generation failed (exit "+std::to_wstring(ec)+L").\n\nThe generator uses SAM 3 and falls back to Grounding DINO + SAM 2.1 when SAM 3's gated weights are not reachable, so this is usually a failed first-run download (about 1.7 GB either way) or a missing package: it needs transformers, plus sam2 for the fallback.\n\nFor SAM 3 itself, ask for access at huggingface.co/facebook/sam3 and sign in (hf auth login, or set HF_TOKEN).").c_str(),T(L"app.title").c_str(),MB_ICONERROR);
        }
    }

    // Help / F1: the manual is help.html beside the exe (self-extracted from
    // the payload) or docs\help.html in a checkout; it opens in the browser.
    void OpenHelp(){
        std::error_code ec;
        std::filesystem::path page=smru::paths::ExeDirectory()/L"help.html";
        if(!std::filesystem::is_regular_file(page,ec)){
            const std::filesystem::path docs=smru::paths::FindProjectDir(L"docs");
            page=docs.empty()?std::filesystem::path():docs/L"help.html";
        }
        if(page.empty()||!std::filesystem::is_regular_file(page,ec)){
            MessageBoxW(m_hwnd,L"help.html was not found beside the player or in the checkout's docs folder.",T(L"app.title").c_str(),MB_ICONERROR);return;
        }
        ShellExecuteW(m_hwnd,L"open",page.c_str(),nullptr,nullptr,SW_SHOWNORMAL);
    }

    // Every look control back to a fresh install's defaults: colour
    // adjustments, levels, neural knobs, effect toggles on, Bypass off, Motion
    // Zero. The loaded LUT file, the model pick and the layout are kept.
    void ResetAll(){
        m_colorSettings={};
        m_toneMix=0.0f;m_sharpen=0.0f;m_postSharpen=0.0f;m_nrSmooth=0.0f;
        m_lutStrength=1.0f;m_svrStrength=0.7f;
        m_nrIntensity=1.0f;m_nrLocalStructure=1.0f;m_nrSkinStructure=-1.0f;m_nrAutoMask=true;
        m_fxLut=true;m_fxSharpen=true;m_fxTone=true;m_fxFlow=true;m_fxDepth=true;m_fxMask=false;m_bypassFX=false;
        m_motionMode=0;
        SaveVideoSettings();ApplyVideoAdjustments(true);RefreshMenu();InvalidateControls();
        LOG("Reset: all look controls back to defaults.");
    }

    static std::wstring SeedVRPathFor(const std::wstring& moviePath){
        std::filesystem::path in(moviePath);
        return (in.parent_path()/(in.stem().wstring()+smru::kSidecarSvrSuffix)).wstring();
    }

    // A quick, bounded pre-flight question to one of the tools: the answer as
    // one trimmed line of text, ready to drop into a message box.
    static bool RunToolCapture(const std::wstring& cmdline,std::wstring& output,DWORD& exitCode,DWORD timeoutMs){
        std::string bytes;
        if(!smru::proc::RunCapture(cmdline,bytes,exitCode,{.timeoutMs=timeoutMs})){output.clear();return false;}
        while(!bytes.empty()&&(bytes.back()=='\n'||bytes.back()=='\r'))bytes.pop_back();
        output=smru::text::Utf8ToWide(bytes);
        return true;
    }

    // The SeedVR job reports its phase on "[seedvr] ..." lines (run_seedvr.py)
    // and the pack's tqdm bars supply a percent ("42%|####"). The first run
    // downloads ~9 GB of weights before any restoration happens, so the button
    // and the status bar say which of the phases is going on.
    void DrainSeedVROutput(){
        if(!m_svrOutRead)return;
        bool changed=false;
        for(;;){
            DWORD avail=0;
            if(!PeekNamedPipe(m_svrOutRead,nullptr,0,nullptr,&avail,nullptr)||!avail)break;
            char buf[4096];DWORD got=0;
            if(!ReadFile(m_svrOutRead,buf,std::min<DWORD>(avail,DWORD(sizeof(buf))),&got,nullptr)||!got)break;
            for(DWORD i=0;i<got;++i)m_svrBuf.push_back(buf[i]=='\r'?'\n':buf[i]);
            changed=true;
        }
        if(!changed)return;
        if(m_svrBuf.size()>16384)m_svrBuf.erase(0,m_svrBuf.size()-8192);
        static constexpr char kTag[]="[seedvr] ";
        const size_t tag=m_svrBuf.rfind(kTag);
        if(tag!=std::string::npos){
            const size_t body=tag+(sizeof(kTag)-1),end=m_svrBuf.find('\n',body);
            if(end!=std::string::npos){ // only complete lines
                const std::string line=m_svrBuf.substr(body,end-body);
                const std::wstring status=smru::text::Utf8ToWide(line);
                if(status!=m_svrStatus){
                    m_svrStatus=status;m_svrPct=-1;
                    if(line.rfind("downloading model",0)==0)m_svrPhase=SvrPhase::Downloading;
                    else if(line.rfind("validating model",0)==0||line.rfind("Validating",0)==0)m_svrPhase=SvrPhase::Validating;
                    else if(line.rfind("model ready",0)==0||line.rfind("restoring",0)==0)m_svrPhase=SvrPhase::Restoring;
                    else if(line.rfind("finalizing",0)==0)m_svrPhase=SvrPhase::Finalizing;
                }
            }
        }
        const size_t pct=m_svrBuf.rfind("%|");
        if(pct!=std::string::npos&&(tag==std::string::npos||pct>tag)){
            size_t b=pct;while(b>0&&m_svrBuf[b-1]>='0'&&m_svrBuf[b-1]<='9')--b;
            if(b<pct)m_svrPct=std::clamp(atoi(m_svrBuf.c_str()+b),0,100);
        }
        InvalidateControls();
    }

    std::wstring SeedVRButtonCaption()const{
        const std::wstring pct=m_svrPct>=0?L" "+std::to_wstring(m_svrPct)+L"%":std::wstring(L"...");
        switch(m_svrPhase){
        case SvrPhase::Downloading:return L"DL"+pct;
        case SvrPhase::Validating:return L"Verify...";
        case SvrPhase::Finalizing:return L"Mux...";
        default:return L"SVR"+pct;
        }
    }

    std::wstring SeedVRStatusText()const{
        std::wstring phase;
        switch(m_svrPhase){
        case SvrPhase::Downloading:phase=L"downloading model";break;
        case SvrPhase::Validating:phase=L"verifying model";break;
        case SvrPhase::Restoring:phase=L"restoring";break;
        case SvrPhase::Finalizing:phase=L"finalizing";break;
        default:phase=L"starting";break;
        }
        if(m_svrPct>=0)phase+=L" "+std::to_wstring(m_svrPct)+L"%";
        std::wstring detail=m_svrStatus;if(detail.size()>72)detail=detail.substr(0,72)+L"...";
        return detail.empty()?phase:phase+L" - "+detail;
    }

    // SeedVR2 restoration pre-pass: diffusion-restores the loaded movie at source
    // resolution (recovered real detail), then the restored
    // file goes through DLSS/NR. Runs offline through the configured Python; the
    // first run downloads ~9 GB of weights as its own reported phase.
    void StartSeedVR(){
        if(!m_loaded||m_path.empty()||m_svrProc)return;
        const std::wstring svr=SeedVRPathFor(m_path);
        if(std::filesystem::exists(svr)){
            if(MessageBoxW(m_hwnd,(L"A restored version already exists:\n"+svr+L"\n\nOpen it instead of re-running SeedVR?").c_str(),T(L"app.title").c_str(),MB_YESNO|MB_ICONQUESTION)==IDYES){Load(svr);return;}
        }
        std::filesystem::path python,script;
        if(!ResolveTool(smru::kToolSeedVRScript,L"SeedVR",python,script))return;
        const float svrStrength=std::clamp(m_svrStrength,0.0f,1.0f);
        WriteSetting(L"SvrStrength",svrStrength); // make the key visible/editable
        // First-run check: are the weights in place? run_seedvr.py --check answers
        // without importing torch, so the confirmation can say up front that a
        // download comes first instead of the job silently stalling on it.
        std::wstring weightsNote;
        {std::wstring reply;DWORD rc=0;
         if(RunToolCapture(L"\""+python.wstring()+L"\" \""+script.wstring()+L"\" --check",reply,rc,20000)&&rc==3)
             weightsNote=L"The SeedVR weights are not downloaded yet ("+reply+L").\nThe first run downloads and verifies them (~9 GB) before restoring; the SeedVR button and the status bar show the progress.\n\n";}
        std::wstringstream ask;
        ask<<weightsNote<<L"Run the SeedVR2 restoration pass on this movie?\n\nUpscales to ~1080p short side and restores detail at strength "
           <<std::fixed<<std::setprecision(2)<<svrStrength
           <<L"\n(tune via the SVR slider in the Levels group; 1.0 = full SeedVR look).\nOutput keeps the source frame rate and audio. Expect a few minutes.";
        if(MessageBoxW(m_hwnd,ask.str().c_str(),T(L"app.title").c_str(),MB_YESNO|MB_ICONQUESTION)!=IDYES)return;
        std::wstringstream cmd;
        cmd<<L"\""<<python.wstring()<<L"\" \""<<script.wstring()<<L"\" \""<<m_path<<L"\" --strength "<<std::fixed<<std::setprecision(2)<<svrStrength;
        wchar_t tmpDir[MAX_PATH]{};GetTempPathW(MAX_PATH,tmpDir);
        // Capture the job's stdout/stderr so DrainSeedVROutput can show the phase.
        smru::proc::Options spawn;
        spawn.out=smru::proc::Stdio::Pipe;spawn.err=smru::proc::Stdio::Pipe;
        spawn.workingDir=tmpDir[0]?tmpDir:nullptr;
        smru::proc::Child job;
        if(!smru::proc::Spawn(cmd.str(),spawn,job)){
            MessageBoxW(m_hwnd,L"Could not start SeedVR.",T(L"app.title").c_str(),MB_ICONERROR);return;
        }
        m_svrProc=job.process;m_svrOutRead=job.stdOut;m_svrForPath=m_path;
        m_svrBuf.clear();m_svrStatus.clear();m_svrPhase=SvrPhase::Starting;m_svrPct=-1;
        LOG("SeedVR restoration started"<<(weightsNote.empty()?"":" (weights will be downloaded first)")<<".");
        InvalidateControls();
    }

    void PollSeedVR(){
        if(!m_svrProc)return;
        DrainSeedVROutput();
        if(WaitForSingleObject(m_svrProc,0)!=WAIT_OBJECT_0)return;
        DrainSeedVROutput();
        DWORD ec=1;GetExitCodeProcess(m_svrProc,&ec);
        CloseHandle(m_svrProc);m_svrProc=nullptr;
        if(m_svrOutRead){CloseHandle(m_svrOutRead);m_svrOutRead=nullptr;}
        const std::wstring lastStatus=m_svrStatus;
        m_svrBuf.clear();m_svrStatus.clear();m_svrPhase=SvrPhase::Starting;m_svrPct=-1;
        LOG("SeedVR job finished ec="<<ec);
        InvalidateControls();
        const std::wstring svr=SeedVRPathFor(m_svrForPath);
        if(ec==0&&std::filesystem::exists(svr)){
            // One-click chain: the restored clip goes straight through the DLSS/NR
            // export with the player's current settings (the exporter waits for the
            // NR runtime to arm on its own), so SeedVR + DLSS needs no manual
            // re-setup. "No" just opens it for tuning; Cancel leaves the file be.
            const int pick=MessageBoxW(m_hwnd,(L"SeedVR restoration finished:\n"+svr+
                L"\n\nYES: open it AND run the DLSS/NR export on it now (current settings).\n"
                L"NO: just open it for tuning (export through DLSS yourself).\n"
                L"CANCEL: do nothing.").c_str(),T(L"app.title").c_str(),MB_YESNOCANCEL|MB_ICONINFORMATION);
            if(pick==IDYES){if(Load(svr))StartExport(0);}
            else if(pick==IDNO)Load(svr);
        } else {
            MessageBoxW(m_hwnd,(L"SeedVR failed (exit "+std::to_wstring(ec)+L").\nLast status: "+(lastStatus.empty()?L"(no output)":lastStatus)+
                L"\n\nIf the weight download did not finish, click SeedVR again - it resumes where it stopped.").c_str(),T(L"app.title").c_str(),MB_ICONERROR);
        }
    }

    void PollExport(){
        if(!m_exportProc)return;
        // Drain the exporter's stderr and parse the newest "[smru] frame N" line.
        if(m_exportErrRead){
            for(;;){
                DWORD avail=0;
                if(!PeekNamedPipe(m_exportErrRead,nullptr,0,nullptr,&avail,nullptr)||!avail)break;
                char buf[2048];DWORD got=0;
                if(!ReadFile(m_exportErrRead,buf,std::min<DWORD>(avail,DWORD(sizeof(buf))),&got,nullptr)||!got)break;
                m_exportBuf.append(buf,got);
                if(m_exportBuf.size()>8192)m_exportBuf.erase(0,m_exportBuf.size()-4096);
            }
            static constexpr char kFrameMarker[]=SMRU_LOG_TAG " frame ";
            const size_t pos=m_exportBuf.rfind(kFrameMarker);
            if(pos!=std::string::npos){
                const uint64_t fr=_strtoui64(m_exportBuf.c_str()+pos+(sizeof(kFrameMarker)-1),nullptr,10);
                if(fr>m_exportFrames){
                    m_exportFrames=fr;
                    const int pct=int(std::min<uint64_t>(99,(fr*100)/std::max<uint64_t>(1,m_exportTotal)));
                    if(pct!=m_exportPct){m_exportPct=pct;InvalidateControls();}
                }
            }
        }
        if(WaitForSingleObject(m_exportProc,0)!=WAIT_OBJECT_0)return;
        DWORD ec=1;GetExitCodeProcess(m_exportProc,&ec);
        CloseHandle(m_exportProc);m_exportProc=nullptr;
        if(m_exportErrRead){CloseHandle(m_exportErrRead);m_exportErrRead=nullptr;}
        // The exporter's own words, minus the per-frame progress ticks: the
        // last few of them are the reason for a failure, so they go into the
        // log and the failure box instead of a "see the log" pointer only.
        std::string tail;
        {std::istringstream in(m_exportBuf);std::string line;std::vector<std::string> kept;
         while(std::getline(in,line)){if(!line.empty()&&line.back()=='\r')line.pop_back();
             if(line.empty()||line.find(SMRU_LOG_TAG " frame ")!=std::string::npos)continue;
             kept.push_back(line);if(kept.size()>6)kept.erase(kept.begin());}
         for(const auto& l:kept){tail+=l;tail+='\n';}}
        m_exportBuf.clear();m_exportPct=-1;m_exportFrames=0;
        InvalidateControls();
        if(ec!=0){
            LOG("GUI export failed exit="<<ec<<" last exporter output:\n"<<tail);
            MessageBoxW(m_hwnd,(L"Export failed (exit "+std::to_wstring(ec)+L").\n\n"+smru::text::Utf8ToWide(tail)+L"\nFull log: "+smru::kExporterLogFileW+L" in the temp folder.").c_str(),T(L"app.title").c_str(),MB_ICONERROR);
            return;
        }
        if(tail.find("WARNING")!=std::string::npos)LOG("GUI export finished with warnings:\n"<<tail);
        // DLSS-first chain: a finished plain/4K export can go straight into the
        // SeedVR restoration pass (compare/split outputs are stitched frames and
        // make no sense to restore). Default button is NO - SeedVR on a 4K
        // export is slow, so an accidental Enter must not start it.
        if((m_exportKind==0||m_exportKind==2)&&std::filesystem::exists(m_exportOutPath)){
            if(MessageBoxW(m_hwnd,(L"Export finished:\n"+m_exportOutPath+
                   L"\n\nAlso run the SeedVR restoration pass on it now?\n(Chains DLSS -> SeedVR; slow at 4K. It opens the export first.)").c_str(),
                   T(L"app.title").c_str(),MB_YESNO|MB_DEFBUTTON2|MB_ICONINFORMATION)==IDYES){
                if(Load(m_exportOutPath))StartSeedVR();
            }
            return;
        }
        MessageBoxW(m_hwnd,(L"Export finished:\n"+m_exportOutPath).c_str(),T(L"app.title").c_str(),MB_ICONINFORMATION);
    }
    void MouseDown(int x,int y){
        SetFocus(m_hwnd);if(!m_loaded){if(PtIn(EmptyOpenRect(),x,y))OpenFromDialog();return;}if(m_seeking)return;
        RECT tr=TimelineRect();if(PtIn(tr,x,y)){m_dragSeek=true;m_seekPreview=SecondsFromX(x);SetCapture(m_hwnd);InvalidateRect(m_hwnd,nullptr,FALSE);return;}RECT vr=VolumeRect();if(PtIn(vr,x,y)){m_muted=false;m_dragVolume=true;SetCapture(m_hwnd);SetVolumeFromX(x);return;}
        {HDC dc=GetDC(m_hwnd);if(dc){LayoutBar(dc);ReleaseDC(m_hwnd,dc);}}
        RECT cw{};GetClientRect(m_hwnd,&cw);
        // Panel hits only count inside the panel column, so scrolled-away
        // items can never swallow clicks meant for the bar/timeline.
        const bool inPanelArea=x>=PanelX0(cw)&&x<PanelX0(cw)+SIDE_W;
        if(inPanelArea)for(const BarFrame&f:m_barFrames){
            if(f.group<0||f.group>=7)continue;
            RECT cap{f.r.left,f.r.top,f.r.right,f.r.top+24};
            if(PtIn(cap,x,y)){m_groupOpen[f.group]=!m_groupOpen[f.group];SaveVideoSettings();InvalidateControls();return;}
        }
        if(inPanelArea)for(int s=0;s<kSliderCount;++s){
            if(m_sliderRect[s].right<=m_sliderRect[s].left)continue;
            RECT hit{m_sliderTrack[s].left-8,m_sliderRect[s].top,m_sliderTrack[s].right+8,m_sliderRect[s].bottom};
            if(PtIn(hit,x,y)){m_dragSlider=s;SetCapture(m_hwnd);SetSliderFromX(s,x);return;}
        }
        for(int i=0;i<kBtnCount;++i){
            if(!IsBarBtn(i)&&!inPanelArea)continue;
            if(PtIn(m_btnRect[i],x,y)){
                // Standard press feel: arm on press, fire on release while
                // still over the button (drag off to cancel).
                m_pressBtn=i;SetCapture(m_hwnd);InvalidateControls();
                return;
            }
        }
    }
    void OnButton(int i){
        switch(i){
        case 0:OpenFromDialog();break;
        case 1:RequestSeek(Position()-10);break;
        case 2:TogglePause();break;
        case 3:StopPlayback();break;
        case 4:RequestSeek(Position()+10);break;
        case 5:ToggleMute();break;
        case 6:ToggleDLSS();break;
        case 7:m_fill=!m_fill;Layout();break;
        case 46:m_nrAutoMask=!m_nrAutoMask;ApplyVideoAdjustments(true);SaveVideoSettings();break;
        case 9:StartDepthMapGen();break;
        case 10:ToggleDebug(D3D12Renderer::DebugView::MotionVectors);break;
        case 11:ToggleFullscreen();break;
        case 12:StartExport(0);break;
        case 13:StartExport(1);break;
        case 14:StartExport(2);break;
        case 15:StartExport(3);break;
        case 16:m_loop=!m_loop;InvalidateControls();break;
        case 17:StartFlowGen();break;
        case 43:StartMaskGen();break;
        case 44:m_fxMask=!m_fxMask;FxChanged();break;
        case 45:ResetAll();break;
        case 18:ToggleBypass();break;
        case 19:m_fxLut=!m_fxLut;FxChanged();break;
        case 20:m_fxSharpen=!m_fxSharpen;FxChanged();break;
        case 21:m_fxDepth=!m_fxDepth;FxChanged();break;
        case 22:m_fxTone=!m_fxTone;FxChanged();break;
        case 23:ToggleDebug(D3D12Renderer::DebugView::Depth);break;
        case 24:m_fxFlow=!m_fxFlow;FxChanged();break;
        case 26:ToggleDebug(D3D12Renderer::DebugView::BiasMask);break;
        case 34:StartSeedVR();break;
        case 40:m_exportRes=(m_exportRes+1)%4;SaveVideoSettings();InvalidateControls();break;
        case 41:m_exportCodec=(m_exportCodec+1)%3;SaveVideoSettings();InvalidateControls();break;
        case 42:SaveProcessedFrame();break;
        case 25:TakeCompareShot();break;
        case 27:PickAndLoadLUT();break;
        case 28:SelectNRModel(0);break;
        case 29:SelectNRModel(1);break;
        case 30:SelectNRModel(2);break;
        case 31:case 32:case 33:
            m_motionMode=i-31;SaveVideoSettings();RefreshMenu();ApplyVideoAdjustments(true);InvalidateControls();break;
        case 35:ZoomStep(1.4f);break;   // zoom out (larger sub-rect)
        case 36:ZoomStep(1.0f/1.4f);break; // zoom in
        case 37:ResetZoom();break;
        case 38:SetCompareOrient(1);break;
        case 39:SetCompareOrient(2);break;
        }
    }
    // Step zoom around the current center (panel buttons; gestures still work).
    void ZoomStep(float f){
        if(!m_loaded||!m_renderer)return;
        m_zoom=std::clamp(m_zoom*f,0.05f,1.0f);
        if(m_zoom>0.999f){m_zoom=1.0f;m_zoomCX=0.5f;m_zoomCY=0.5f;}
        ClampZoomCenter();ApplyZoom();
    }
    double SecondsFromX(int x)const{RECT r=TimelineRect();const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);double t=double(LONG(x)-r.left)/double(span);return std::clamp(t,0.0,1.0)*m_decoder.DurationSeconds();}
    void SetVolumeFromX(int x){RECT r=VolumeRect();const LONG span=(r.right>r.left)?(r.right-r.left):LONG(1);m_volume=float(std::clamp(double(LONG(x)-r.left)/double(span),0.0,1.0));m_audio.SetVolume(m_volume);InvalidateControls();}
    void ToggleMute(){m_muted=!m_muted;m_audio.SetVolume(m_muted?0.0f:m_volume);InvalidateControls();}
    // Paused: a plain re-present would keep showing whatever the LAST render
    // produced (the input, if the pass was off), so the still is rendered again
    // through the full pipeline - warm-up included - and the reset is consumed.
    void ToggleDLSS(){
        if(!m_renderer)return;
        m_renderer->SetDLSS(!m_renderer->DLSSEnabled());m_dlssReset=true;
        if(!m_playing){
            if(m_loaded&&!m_lastFrame.bgra.empty()){RenderVideoFrame(m_lastFrame,true);m_dlssReset=false;}
            else m_renderer->PresentCurrent();
        }
        InvalidateControls();
    }

    // ---- Inspection zoom -----------------------------------------------------
    void ApplyZoom(){
        if(!m_renderer)return;
        m_renderer->SetZoom(m_zoomCX,m_zoomCY,m_zoom);
        if(!m_playing&&!m_seeking)m_renderer->PresentCurrent(); // present-stage only: no re-render needed
        InvalidateControls();
    }
    void ClampZoomCenter(){
        m_zoomCX=std::clamp(m_zoomCX,m_zoom*0.5f,1.0f-m_zoom*0.5f);
        m_zoomCY=std::clamp(m_zoomCY,m_zoom*0.5f,1.0f-m_zoom*0.5f);
    }
    void ResetZoom(){m_zoom=1.0f;m_zoomCX=0.5f;m_zoomCY=0.5f;ApplyZoom();}
    void BeginZoomRect(HWND h,LPARAM l){
        if(!m_loaded||!m_renderer)return;
        m_zoomRectDrag=true;m_zoomStart={GET_X_LPARAM(l),GET_Y_LPARAM(l)};m_zoomLast=m_zoomStart;SetCapture(h);
    }
    void EndZoomRect(HWND h,LPARAM l){
        if(!m_zoomRectDrag)return;
        m_zoomRectDrag=false;if(GetCapture()==h)ReleaseCapture();
        if(!m_loaded||!m_renderer)return;
        const POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        RECT rc{};GetClientRect(h,&rc);
        const float w=float(std::max<LONG>(1,rc.right)),ht=float(std::max<LONG>(1,rc.bottom));
        const float dx=std::fabs(float(p.x-m_zoomStart.x))/w,dy=std::fabs(float(p.y-m_zoomStart.y))/ht;
        if(dx<0.01f&&dy<0.01f){ResetZoom();return;} // plain right-click
        // The drawn rect is in the CURRENT view, so compose with the active zoom;
        // max(dx,dy) keeps the magnification uniform (window is aspect-fitted).
        const float offX=std::clamp(m_zoomCX-m_zoom*0.5f,0.0f,1.0f-m_zoom);
        const float offY=std::clamp(m_zoomCY-m_zoom*0.5f,0.0f,1.0f-m_zoom);
        m_zoomCX=offX+((m_zoomStart.x+p.x)*0.5f/w)*m_zoom;
        m_zoomCY=offY+((m_zoomStart.y+p.y)*0.5f/ht)*m_zoom;
        m_zoom=std::max(0.05f,std::max(std::max(dx,dy),0.02f)*m_zoom);
        ClampZoomCenter();ApplyZoom();
    }
    void BeginZoomPan(HWND h,LPARAM l){
        if(m_loaded&&m_zoom<0.999f){m_zoomPan=true;m_zoomLast={GET_X_LPARAM(l),GET_Y_LPARAM(l)};SetCapture(h);}
    }
    void EndZoomPan(HWND h){if(m_zoomPan){m_zoomPan=false;if(GetCapture()==h)ReleaseCapture();}}
    void ZoomVideoMouseMove(HWND h,LPARAM l){
        const POINT p{GET_X_LPARAM(l),GET_Y_LPARAM(l)};
        if(m_zoomRectDrag&&GetCapture()==h){m_zoomLast=p;return;}
        if(m_zoomPan&&GetCapture()==h&&m_zoom<0.999f){
            RECT rc{};GetClientRect(h,&rc);
            const float w=float(std::max<LONG>(1,rc.right)),ht=float(std::max<LONG>(1,rc.bottom));
            m_zoomCX-=float(p.x-m_zoomLast.x)/w*m_zoom;
            m_zoomCY-=float(p.y-m_zoomLast.y)/ht*m_zoom;
            ClampZoomCenter();m_zoomLast=p;ApplyZoom();
        }
    }
    void ZoomWheel(const POINT& client,short delta){
        if(!m_loaded||!m_renderer)return;
        RECT rc{};GetClientRect(m_renderWnd,&rc);
        const float w=float(std::max<LONG>(1,rc.right)),ht=float(std::max<LONG>(1,rc.bottom));
        const float offX=std::clamp(m_zoomCX-m_zoom*0.5f,0.0f,1.0f-m_zoom);
        const float offY=std::clamp(m_zoomCY-m_zoom*0.5f,0.0f,1.0f-m_zoom);
        const float ux=offX+(float(client.x)/w)*m_zoom,uy=offY+(float(client.y)/ht)*m_zoom;
        const float nz=std::clamp(m_zoom*(delta>0?1.0f/1.25f:1.25f),0.05f,1.0f);
        // Keep the uv under the cursor stationary while the scale changes.
        m_zoomCX=ux+(m_zoomCX-ux)*(nz/m_zoom);
        m_zoomCY=uy+(m_zoomCY-uy)*(nz/m_zoom);
        m_zoom=nz;
        if(m_zoom>0.999f){m_zoom=1.0f;m_zoomCX=0.5f;m_zoomCY=0.5f;}
        ClampZoomCenter();ApplyZoom();
    }

    // ---- A/B compare separator ----------------------------------------------
    // Two buttons pick the orientation (1 = vertical divider, drag left/right;
    // 2 = horizontal divider, drag up/down); clicking the active one turns it off.
    void SetCompareOrient(int o){
        if(m_compareOn&&m_compareOrient==o) m_compareOn=false; // toggle the active one off
        else { m_compareOn=true; m_compareOrient=o; if(m_comparePos<0.02f||m_comparePos>0.98f)m_comparePos=0.5f; }
        PushCompare();
        if(!m_playing&&!m_seeking&&m_renderer)m_renderer->PresentCurrent(); // immediate feedback on click
        InvalidateControls();
    }
    // Push state to the renderer WITHOUT a blocking present: while paused, the
    // 60 fps static-present tick shows it; while playing, the next frame does.
    // Dragging therefore stays smooth no matter how fast the mouse moves.
    void PushCompare(){ if(m_renderer)m_renderer->SetCompare(m_compareOn?m_compareOrient:0,m_comparePos); }
    void BeginCompareDrag(HWND h,LPARAM l){
        if(!m_compareOn||!m_loaded)return;
        m_compareDrag=true;SetCapture(h);
        UpdateCompareFromCursor(h,GET_X_LPARAM(l),GET_Y_LPARAM(l));
    }
    void EndCompareDrag(HWND h){if(m_compareDrag){m_compareDrag=false;if(GetCapture()==h)ReleaseCapture();}}
    void UpdateCompareFromCursor(HWND h,int x,int y){
        RECT rc{};GetClientRect(h,&rc);
        const float w=float(std::max<LONG>(1,rc.right)),ht=float(std::max<LONG>(1,rc.bottom));
        m_comparePos=std::clamp((m_compareOrient==1)?float(x)/w:float(y)/ht,0.0f,1.0f);
        PushCompare();
        // Re-composite immediately at mouse-move rate. This is the cheap present
        // pass (two texture reads, no decode/NR), so on any modern GPU it tracks
        // the cursor with no perceptible lag. The tick's 60 fps present is skipped
        // while dragging (see Tick) so there is no double-present.
        if(m_renderer)m_renderer->PresentCurrent();
        InvalidateControls();
    }
    void SetDecodeScale(bool automatic,DecodeScale q){m_opt.scaleExplicit=!automatic;m_opt.scale=q;ReloadKeepingPosition();}
    void ReloadKeepingPosition(){if(m_loaded&&!m_path.empty()){std::wstring p=m_path;double keep=Position();bool wasPlaying=m_playing;if(Load(p))RequestSeek(keep,wasPlaying);}}
    void RefreshMenu(){HMENU old=GetMenu(m_hwnd),fresh=CreateMenuBar();SetMenu(m_hwnd,fresh);DrawMenuBar(m_hwnd);if(old)DestroyMenu(old);}
    void FxChanged(){SaveVideoSettings();ApplyVideoAdjustments(true);RefreshMenu();InvalidateControls();}
    // Pick a .cube grade ("tone") and reload keeping position, since the LUT
    // texture is created at renderer init. Shared by the menu and the panel button.
    void PickAndLoadLUT(){
        wchar_t p[32768]{};OPENFILENAMEW o{};o.lStructSize=sizeof(o);o.hwndOwner=m_hwnd;o.lpstrFile=p;o.nMaxFile=static_cast<DWORD>(std::size(p));
        o.lpstrFilter=L"Cube LUT\0*.cube\0All files\0*.*\0\0";o.nFilterIndex=1;
        const std::wstring lutDir=smru::paths::LutDirectory().wstring();o.lpstrInitialDir=lutDir.empty()?nullptr:lutDir.c_str();
        o.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST|OFN_EXPLORER|OFN_NOCHANGEDIR;
        if(GetOpenFileNameW(&o)){m_lutPath=p;m_fxLut=true;SaveVideoSettings();RefreshMenu();ReloadKeepingPosition();}
    }
    void ToggleBypass(){m_bypassFX=!m_bypassFX;ApplyVideoAdjustments(true);RefreshMenu();InvalidateControls();}
    // Unload the .cube grade (FX menu "Clear LUT", or right-click the LUT
    // button in the panel). Reload is needed because the LUT texture is
    // created at renderer init.
    void ClearLUT(){
        if(m_lutPath.empty())return;
        m_lutPath.clear();SaveVideoSettings();RefreshMenu();ReloadKeepingPosition();
    }

    // Selecting an NR model: style is a live per-evaluate parameter on the direct
    // engine, so it applies to the very next frame (a paused frame re-renders).
    void SelectNRModel(int style){
        m_nrModelPick=style;SaveVideoSettings();
        ApplyVideoAdjustments(true);RefreshMenu();InvalidateControls();
    }

    // Compare screenshot: the current frame re-rendered through the full pipeline
    // next to the plain source (bilinear at output size), labeled, saved as PNG
    // beside the movie via ffmpeg.
    // Pipes tightly packed RGBA to ffmpeg as a single PNG. Returns false and
    // shows a message box on failure.
    bool SavePngRGBA(const uint8_t* rgba,uint32_t w,uint32_t h,const std::wstring& out){
        const std::wstring ffmpeg=smru::paths::FfmpegExe().wstring();
        if(ffmpeg.empty()){MessageBoxW(m_hwnd,L"ffmpeg.exe was not found next to the player.",T(L"app.title").c_str(),MB_ICONERROR);return false;}
        std::wstringstream cmd;
        cmd<<L"\""<<ffmpeg<<L"\" -y -hide_banner -loglevel error -f rawvideo -pixel_format rgba -video_size "<<w<<L"x"<<h<<L" -i - -frames:v 1 \""<<out<<L"\"";
        smru::proc::Options spawn;spawn.in=smru::proc::Stdio::Pipe;spawn.pipeBytes=1<<22;
        smru::proc::Child enc;
        if(!smru::proc::Spawn(cmd.str(),spawn,enc)){MessageBoxW(m_hwnd,L"ffmpeg spawn failed",T(L"app.title").c_str(),MB_ICONERROR);return false;}
        DWORD written=0;const uint8_t* p=rgba;size_t left=size_t(w)*h*4;
        while(left){DWORD chunk=DWORD(std::min<size_t>(left,1u<<20));if(!WriteFile(enc.stdIn,p,chunk,&written,nullptr)||!written)break;p+=written;left-=written;}
        CloseHandle(enc.stdIn);enc.stdIn=nullptr; // ffmpeg finishes the file on EOF
        WaitForSingleObject(enc.process,INFINITE);
        DWORD ec=1;GetExitCodeProcess(enc.process,&ec);
        smru::proc::Close(enc);
        return ec==0&&std::filesystem::exists(out);
    }

    // Saves the CURRENT frame as a PNG with every modification applied (the
    // full pipeline output the preview shows, minus zoom/compare overlays).
    void SaveProcessedFrame(){
        if(!m_loaded||!m_renderer||m_lastFrame.bgra.empty()){MessageBoxW(m_hwnd,L"No frame available yet.",T(L"app.title").c_str(),MB_ICONINFORMATION);return;}
        m_renderer->EnableExport(true);
        const bool ok=RenderVideoFrame(m_lastFrame,false);
        std::vector<uint8_t> with=m_renderer->ExportRGBA();
        m_renderer->EnableExport(false);
        const uint32_t ow=m_renderer->OutputW(),oh=m_renderer->OutputH();
        if(!ok||with.size()<size_t(ow)*oh*4){MessageBoxW(m_hwnd,L"Could not capture the processed frame.",T(L"app.title").c_str(),MB_ICONERROR);return;}
        std::filesystem::path in(m_path);
        std::wstring out;
        for(int n=1;n<1000;++n){
            out=(in.parent_path()/(in.stem().wstring()+L"_dlss5_frame_"+std::to_wstring(n)+L".png")).wstring();
            if(!std::filesystem::exists(out))break;
        }
        if(SavePngRGBA(with.data(),ow,oh,out))
            MessageBoxW(m_hwnd,(L"Frame saved:\n"+out).c_str(),T(L"app.title").c_str(),MB_ICONINFORMATION);
    }

    void TakeCompareShot(){
        if(!m_loaded||!m_renderer||m_lastFrame.bgra.empty()){MessageBoxW(m_hwnd,L"No frame available yet.",T(L"app.title").c_str(),MB_ICONINFORMATION);return;}
        m_renderer->EnableExport(true);
        const bool ok=RenderVideoFrame(m_lastFrame,false);
        std::vector<uint8_t> with=m_renderer->ExportRGBA();
        m_renderer->EnableExport(false);
        const uint32_t ow=m_renderer->OutputW(),oh=m_renderer->OutputH(),sw=m_decoder.Width(),sh=m_decoder.Height();
        if(!ok||with.size()<size_t(ow)*oh*4){MessageBoxW(m_hwnd,L"Could not capture the processed frame.",T(L"app.title").c_str(),MB_ICONERROR);return;}
        std::vector<uint8_t> ref(size_t(ow)*oh*4);
        for(uint32_t y=0;y<oh;++y){                              // bilinear BGRA->RGBA upscale
            const float fy=std::min((y+0.5f)*sh/oh-0.5f,float(sh-1));
            const uint32_t y0=uint32_t(std::max(0.0f,std::floor(fy))),y1=std::min(y0+1,sh-1);
            const float ty=std::clamp(fy-float(y0),0.0f,1.0f);
            for(uint32_t x=0;x<ow;++x){
                const float fx=std::min((x+0.5f)*sw/ow-0.5f,float(sw-1));
                const uint32_t x0=uint32_t(std::max(0.0f,std::floor(fx))),x1=std::min(x0+1,sw-1);
                const float tx=std::clamp(fx-float(x0),0.0f,1.0f);
                const uint8_t* p00=m_lastFrame.bgra.data()+(size_t(y0)*sw+x0)*4;
                const uint8_t* p01=m_lastFrame.bgra.data()+(size_t(y0)*sw+x1)*4;
                const uint8_t* p10=m_lastFrame.bgra.data()+(size_t(y1)*sw+x0)*4;
                const uint8_t* p11=m_lastFrame.bgra.data()+(size_t(y1)*sw+x1)*4;
                uint8_t* dst=ref.data()+(size_t(y)*ow+x)*4;
                for(int c=0;c<3;++c){const int sc=2-c;
                    const float a=p00[sc]*(1-tx)+p01[sc]*tx,b=p10[sc]*(1-tx)+p11[sc]*tx;
                    dst[c]=uint8_t(std::clamp(a*(1-ty)+b*ty+0.5f,0.0f,255.0f));}
                dst[3]=255;
            }
        }
        const uint32_t div=4,encW=ow*2+div;
        std::vector<uint8_t> stitched(size_t(encW)*oh*4,0);
        for(uint32_t y=0;y<oh;++y){
            uint8_t* row=stitched.data()+size_t(y)*encW*4;
            memcpy(row,ref.data()+size_t(y)*ow*4,size_t(ow)*4);
            for(uint32_t x=ow;x<ow+div;++x){row[x*4+0]=row[x*4+1]=row[x*4+2]=56;row[x*4+3]=255;}
            memcpy(row+size_t(ow+div)*4,with.data()+size_t(y)*ow*4,size_t(ow)*4);
        }
        const uint32_t ls=std::clamp(oh/240u,2u,8u);
        StampLabel(stitched.data(),encW,oh,16,16,smru::kLabelOriginal,ls);
        StampLabel(stitched.data(),encW,oh,ow+div+16,16,smru::kLabelProcessed,ls);
        std::filesystem::path in(m_path);
        std::wstring out;
        for(int n=1;n<1000;++n){
            out=(in.parent_path()/(in.stem().wstring()+smru::kSnapshotInfix+std::to_wstring(n)+L".png")).wstring();
            if(!std::filesystem::exists(out))break;
        }
        if(SavePngRGBA(stitched.data(),encW,oh,out))
            MessageBoxW(m_hwnd,(L"Comparison saved:\n"+out).c_str(),T(L"app.title").c_str(),MB_ICONINFORMATION);
    }

    void ToggleDepthMode(){
        m_depthMode=m_depthMode==TemporalGuideGenerator::DepthMode::Estimated?TemporalGuideGenerator::DepthMode::Flat:TemporalGuideGenerator::DepthMode::Estimated;
        if(m_pipeline)m_pipeline->Guides().SetDepthMode(m_depthMode);
        m_guideReset=true;m_dlssReset=true;UpdateTitle();
    }
    void SetDebug(D3D12Renderer::DebugView v){if(m_renderer){m_renderer->SetDebugView(v);if(!m_playing)m_renderer->PresentCurrent();InvalidateControls();}}
    void ToggleDebug(D3D12Renderer::DebugView v){if(!m_renderer)return;m_renderer->SetDebugView(m_renderer->GetDebugView()==v?D3D12Renderer::DebugView::Final:v);if(!m_playing)m_renderer->PresentCurrent();InvalidateControls();}
    void ToggleFullscreen(){if(!m_fullscreen){m_savedStyle=GetWindowLongW(m_hwnd,GWL_STYLE);GetWindowRect(m_hwnd,&m_savedRect);MONITORINFO mi{sizeof(mi)};GetMonitorInfoW(MonitorFromWindow(m_hwnd,MONITOR_DEFAULTTONEAREST),&mi);SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle&~(WS_CAPTION|WS_THICKFRAME|WS_MINIMIZEBOX|WS_MAXIMIZEBOX|WS_SYSMENU));SetWindowPos(m_hwnd,HWND_TOP,mi.rcMonitor.left,mi.rcMonitor.top,mi.rcMonitor.right-mi.rcMonitor.left,mi.rcMonitor.bottom-mi.rcMonitor.top,SWP_FRAMECHANGED);m_fullscreen=true;}else{SetWindowLongW(m_hwnd,GWL_STYLE,m_savedStyle);SetWindowPos(m_hwnd,nullptr,m_savedRect.left,m_savedRect.top,m_savedRect.right-m_savedRect.left,m_savedRect.bottom-m_savedRect.top,SWP_NOZORDER|SWP_FRAMECHANGED);m_fullscreen=false;}Layout();}

    LRESULT WndProc(HWND h,UINT m,WPARAM w,LPARAM l){
        switch(m){
        case WM_ERASEBKGND:return 1;
        case WM_UAHDRAWMENU:{
            auto* pm=reinterpret_cast<UAHMENU*>(l);
            MENUBARINFO mbi{};mbi.cbSize=sizeof(mbi);
            if(!GetMenuBarInfo(h,OBJID_MENU,0,&mbi))return 0;
            RECT rw{};GetWindowRect(h,&rw);RECT r=mbi.rcBar;OffsetRect(&r,-rw.left,-rw.top);
            HBRUSH b=CreateSolidBrush(Pal::Surface);FillRect(pm->hdc,&r,b);DeleteObject(b);
            return 0;}
        case WM_UAHDRAWMENUITEM:{
            auto* di=reinterpret_cast<UAHDRAWMENUITEM*>(l);
            wchar_t txt[256]{};MENUITEMINFOW mii{};mii.cbSize=sizeof(mii);mii.fMask=MIIM_STRING;mii.dwTypeData=txt;mii.cch=255;
            GetMenuItemInfoW(di->um.hmenu,di->umi.iPosition,TRUE,&mii);
            const bool hot=(di->dis.itemState&(ODS_HOTLIGHT|ODS_SELECTED))!=0;
            HBRUSH b=CreateSolidBrush(hot?Pal::Hover:Pal::Surface);FillRect(di->um.hdc,&di->dis.rcItem,b);DeleteObject(b);
            SetBkMode(di->um.hdc,TRANSPARENT);SetTextColor(di->um.hdc,hot?Pal::Ink:Pal::Ink2);
            DrawTextW(di->um.hdc,txt,-1,&di->dis.rcItem,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
            return 0;}
        case WM_NCPAINT:case WM_NCACTIVATE:{
            // Overpaint the 1px light line the theme draws under the menu bar.
            const LRESULT res=DefWindowProcW(h,m,w,l);
            MENUBARINFO mbi{};mbi.cbSize=sizeof(mbi);
            if(GetMenuBarInfo(h,OBJID_MENU,0,&mbi)){
                RECT rw{};GetWindowRect(h,&rw);
                RECT line{mbi.rcBar.left-rw.left,mbi.rcBar.bottom-rw.top,mbi.rcBar.right-rw.left,mbi.rcBar.bottom-rw.top+1};
                HDC dc=GetWindowDC(h);HBRUSH b=CreateSolidBrush(Pal::Surface);FillRect(dc,&line,b);DeleteObject(b);ReleaseDC(h,dc);
            }
            return res;}
        case WM_DESTROY:m_running=false;PostQuitMessage(0);return 0;
        case WM_CLOSE:DestroyWindow(h);return 0;
        case WM_SIZE:Layout();return 0;
        case WM_PAINT:Paint();return 0;
        case WM_MOUSEMOVE:m_mouseX=GET_X_LPARAM(l);m_mouseY=GET_Y_LPARAM(l);if(m_dragSeek&&GetCapture()==h)m_seekPreview=SecondsFromX(m_mouseX);if(m_dragVolume&&GetCapture()==h)SetVolumeFromX(m_mouseX);if(m_dragSlider>=0&&GetCapture()==h)SetSliderFromX(m_dragSlider,m_mouseX);UpdateHoverTip(m_mouseX,m_mouseY);InvalidateControls();return 0;
        case WM_NOTIFY:{
            auto* nh=reinterpret_cast<NMHDR*>(l);
            if(m_tip&&nh->hwndFrom==m_tip&&nh->code==TTN_GETDISPINFOW){
                auto* di=reinterpret_cast<NMTTDISPINFOW*>(l);
                di->lpszText=const_cast<wchar_t*>(TipText(m_tipId));
                return 0;
            }
            break;}
        case WM_LBUTTONDOWN:MouseDown(GET_X_LPARAM(l),GET_Y_LPARAM(l));return 0;
        case WM_LBUTTONUP:if(m_dragSeek){double target=m_seekPreview;m_dragSeek=false;if(GetCapture()==h)ReleaseCapture();RequestSeek(target);}else if(m_dragVolume){m_dragVolume=false;if(GetCapture()==h)ReleaseCapture();}else if(m_dragSlider>=0){m_dragSlider=-1;if(GetCapture()==h)ReleaseCapture();SaveVideoSettings();}else if(m_pressBtn>=0){const int b=m_pressBtn;m_pressBtn=-1;if(GetCapture()==h)ReleaseCapture();if(PtIn(m_btnRect[b],GET_X_LPARAM(l),GET_Y_LPARAM(l)))OnButton(b);InvalidateControls();}return 0;
        case WM_CAPTURECHANGED:if(m_dragSeek){m_dragSeek=false;InvalidateRect(m_hwnd,nullptr,FALSE);}if(m_dragVolume)m_dragVolume=false;if(m_dragSlider>=0)m_dragSlider=-1;if(m_pressBtn>=0){m_pressBtn=-1;InvalidateControls();}return 0;
        case WM_RBUTTONUP:{
            // Right-click on the LUT button removes the loaded LUT.
            const int x=GET_X_LPARAM(l),y=GET_Y_LPARAM(l);
            if(m_loaded&&!m_lutPath.empty()&&m_btnRect[27].right>m_btnRect[27].left&&PtIn(m_btnRect[27],x,y)){ClearLUT();return 0;}
            break;}
        case WM_DROPFILES:{HDROP d=reinterpret_cast<HDROP>(w);wchar_t p[32768]{};UINT count=DragQueryFileW(d,0xFFFFFFFF,nullptr,0);if(count>0&&DragQueryFileW(d,0,p,static_cast<UINT>(std::size(p))))Load(p);DragFinish(d);return 0;}
        case WM_MOUSEWHEEL:{
            if(!m_loaded)return 0;
            if(GET_KEYSTATE_WPARAM(w)&MK_CONTROL){
                // Ctrl+wheel over the video: inspection zoom at the cursor.
                POINT vp2{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(m_renderWnd,&vp2);
                RECT rr{};GetClientRect(m_renderWnd,&rr);
                if(vp2.x>=0&&vp2.y>=0&&vp2.x<rr.right&&vp2.y<rr.bottom){
                    ZoomWheel(vp2,GET_WHEEL_DELTA_WPARAM(w));return 0;
                }
            }
            POINT pt{GET_X_LPARAM(l),GET_Y_LPARAM(l)};ScreenToClient(h,&pt);
            RECT cw{};GetClientRect(h,&cw);
            if(pt.x>=PanelX0(cw)&&pt.x<PanelX0(cw)+SIDE_W){
                // Wheel over the control panel scrolls it; elsewhere it stays volume.
                const int panelH=std::max(1,int(cw.bottom));
                const int maxScroll=std::max(0,m_panelContentH-panelH);
                m_panelScroll=std::clamp(m_panelScroll-(GET_WHEEL_DELTA_WPARAM(w)/WHEEL_DELTA)*40,0,maxScroll);
                InvalidateControls();return 0;
            }
            m_muted=false;float step=(GET_WHEEL_DELTA_WPARAM(w)>0)?0.05f:-0.05f;m_volume=std::clamp(m_volume+step,0.0f,1.0f);m_audio.SetVolume(m_volume);InvalidateControls();return 0;}
        case WM_COMMAND:HandleCommand(LOWORD(w));return 0;
        case WM_HOTKEY:HandleHotkey(int(w));return 0;
        case WM_KEYDOWN:
            if(w==VK_F1){OpenHelp();return 0;}
            if((GetKeyState(VK_CONTROL)&0x8000)&&w=='O'){OpenFromDialog();return 0;}if(w==VK_SPACE){TogglePause();return 0;}if(w==VK_LEFT){RequestSeek(Position()-10);return 0;}if(w==VK_RIGHT){RequestSeek(Position()+10);return 0;}if(w==VK_F11){ToggleFullscreen();return 0;}if(w=='D'){ToggleDLSS();return 0;}if(w=='B'){ToggleBypass();return 0;}if(w=='G'){ToggleDepthMode();return 0;}if(w=='M'){ToggleMute();return 0;}if(w=='1'){SetDebug(D3D12Renderer::DebugView::Final);return 0;}if(w=='2'){SetDebug(D3D12Renderer::DebugView::Input);return 0;}if(w=='3'){SetDebug(D3D12Renderer::DebugView::MotionVectors);return 0;}if(w=='4'){SetDebug(D3D12Renderer::DebugView::Depth);return 0;}if(w=='5'){SetDebug(D3D12Renderer::DebugView::BiasMask);return 0;}if(w==VK_ESCAPE&&m_fullscreen){ToggleFullscreen();return 0;}break;
        }
        return DefWindowProcW(h,m,w,l);
    }

    void HandleCommand(UINT id){
        const UINT langEnd=IDM_LANG_BASE+static_cast<UINT>(m_languageCodes.size());if(id>=IDM_LANG_BASE && id<langEnd){ApplyLanguage(m_languageCodes[id-IDM_LANG_BASE]);return;}
        switch(id){
        case IDM_OPEN:OpenFromDialog();break;case IDM_EXIT:DestroyWindow(m_hwnd);break;case IDM_PLAY:TogglePause();break;case IDM_STOP:StopPlayback();break;case IDM_BACK10:RequestSeek(Position()-10);break;case IDM_FWD10:RequestSeek(Position()+10);break;case IDM_MUTE:ToggleMute();break;case IDM_DLSS:ToggleDLSS();break;
        case IDM_QUALITY_AUTO:SetDecodeScale(true,DecodeScale::Quality);break;case IDM_QUALITY_QUALITY:SetDecodeScale(false,DecodeScale::Quality);break;case IDM_QUALITY_BALANCED:SetDecodeScale(false,DecodeScale::Balanced);break;case IDM_QUALITY_PERFORMANCE:SetDecodeScale(false,DecodeScale::Performance);break;case IDM_QUALITY_ULTRAPERF:SetDecodeScale(false,DecodeScale::UltraPerformance);break;case IDM_QUALITY_DLAA:SetDecodeScale(false,DecodeScale::Native);break;
        case IDM_FX_BYPASS:ToggleBypass();break;
        case IDM_FX_LUT:m_fxLut=!m_fxLut;FxChanged();break;
        case IDM_FX_SHARPEN:m_fxSharpen=!m_fxSharpen;FxChanged();break;
        case IDM_FX_TONE:m_fxTone=!m_fxTone;FxChanged();break;
        case IDM_FX_FLOW:m_fxFlow=!m_fxFlow;FxChanged();break;
        case IDM_FX_DEPTHMAP:m_fxDepth=!m_fxDepth;FxChanged();break;
        case IDM_FX_MASK:m_fxMask=!m_fxMask;FxChanged();break;
        case IDM_HELP:OpenHelp();break;
        case IDM_PANEL_LEFT:m_panelLeft=!m_panelLeft;m_panelScroll=0;SaveVideoSettings();Layout();RefreshMenu();InvalidateRect(m_hwnd,nullptr,TRUE);break;
        case IDM_FX_LANCZOS4K:m_lanczos4K=!m_lanczos4K;SaveVideoSettings();RefreshMenu();break;
        case IDM_NRMODEL_A:SelectNRModel(0);break;
        case IDM_NRMODEL_B:SelectNRModel(1);break;
        case IDM_NRMODEL_C:SelectNRModel(2);break;
        case IDM_SHOT:TakeCompareShot();break;
        case IDM_LUT_LOAD:PickAndLoadLUT();break;
        case IDM_LUT_CLEAR:ClearLUT();break;
        case IDM_MOTION_ZERO:m_motionMode=0;SaveVideoSettings();RefreshMenu();ApplyVideoAdjustments(true);break;
        case IDM_MOTION_GLOBAL:m_motionMode=1;SaveVideoSettings();RefreshMenu();ApplyVideoAdjustments(true);break;
        case IDM_MOTION_EST:m_motionMode=2;SaveVideoSettings();RefreshMenu();ApplyVideoAdjustments(true);break;
        case IDM_VIEW_FINAL:SetDebug(D3D12Renderer::DebugView::Final);break;case IDM_VIEW_INPUT:SetDebug(D3D12Renderer::DebugView::Input);break;case IDM_VIEW_MV:SetDebug(D3D12Renderer::DebugView::MotionVectors);break;case IDM_VIEW_DEPTH:SetDebug(D3D12Renderer::DebugView::Depth);break;case IDM_VIEW_MASK:SetDebug(D3D12Renderer::DebugView::BiasMask);break;case IDM_DEPTH_MODE:ToggleDepthMode();break;case IDM_ASPECT_FIT:m_fill=false;Layout();break;case IDM_ASPECT_FILL:m_fill=true;Layout();break;case IDM_FULLSCREEN:ToggleFullscreen();break;
        }
    }

    AppOptions m_opt;Localizer m_loc;std::vector<std::wstring> m_languageCodes;D3D12Renderer::ColorSettings m_colorSettings{};DecodeScale m_activeScale=DecodeScale::Quality;HWND m_hwnd=nullptr,m_viewport=nullptr,m_renderWnd=nullptr;HFONT m_font=nullptr,m_fontSmall=nullptr,m_fontTitle=nullptr,m_fontHead=nullptr,m_fontIcon=nullptr;
    bool m_running=true,m_loaded=false,m_playing=false,m_haveNext=false,m_fill=false,m_fullscreen=false,m_dragSeek=false,m_dragVolume=false,m_muted=false,m_seekPending=false,m_seekResumePlaying=false,m_seeking=false;
    LONG m_savedStyle=0;RECT m_savedRect{};double m_dar=16.0/9.0,m_currentSec=0,m_playStartSec=0,m_seekPreview=0,m_pendingSeekSec=0;float m_volume=1.0f;int m_mouseX=-999,m_mouseY=-999;
    Clock::time_point m_playStart=Clock::now(),m_fpsWindowStart=Clock::now(),m_lastStaticPresent=Clock::now();double m_submitFps=0.0;uint64_t m_fpsWindowFrames=0;std::wstring m_path;VideoDecoder m_decoder;VideoFrame m_next;std::unique_ptr<D3D12Renderer>m_renderer;std::unique_ptr<FramePipeline>m_pipeline;TemporalGuideGenerator::DepthMode m_depthMode=TemporalGuideGenerator::DepthMode::Estimated;AudioPlayer m_audio;
    bool m_guideReset=true,m_dlssReset=true;uint64_t m_droppedFrames=0,m_uiTick=0;
    HANDLE m_exportProc=nullptr;HANDLE m_exportErrRead=nullptr;int m_exportKind=0;std::wstring m_exportOutPath;
    HANDLE m_flowProc=nullptr;
    HANDLE m_svrProc=nullptr;std::wstring m_svrForPath;
    // Live SeedVR job state, parsed from the child's "[seedvr] ..." lines and the
    // pack's tqdm bars (see DrainSeedVROutput).
    enum class SvrPhase{Starting,Downloading,Validating,Restoring,Finalizing};
    HANDLE m_svrOutRead=nullptr;std::string m_svrBuf;std::wstring m_svrStatus;SvrPhase m_svrPhase=SvrPhase::Starting;int m_svrPct=-1;
    VideoDecoder m_flowDecoder;VideoFrame m_flowFrame;
    bool m_flowLoaded=false,m_haveFlowFrame=false;
    VideoDecoder m_depthVideoDecoder;VideoFrame m_depthVideoFrame;
    bool m_depthLoaded=false,m_haveDepthFrame=false;
    VideoDecoder m_maskDecoder;VideoFrame m_maskFrame;
    bool m_maskLoaded=false,m_haveMaskFrame=false;
    std::vector<uint8_t> m_depthPlane;
    HANDLE m_depthMapProc=nullptr;std::wstring m_depthMapForPath;
    HANDLE m_maskProc=nullptr;std::wstring m_maskForPath,m_maskPrompt;bool m_fxMask=false;
    int m_maskSensitivity=1;  // GenMask match threshold: 0 strict, 1 balanced, 2 loose (MaskSensitivity in the ini)
    bool m_panelLeft=false;   // control panel column on the left (PanelLeft in the ini)
    bool m_loop=true;
    float m_toneMix=0.0f,m_sharpen=0.0f,m_postSharpen=0.0f,m_lutStrength=1.0f,m_svrStrength=0.7f,m_nrSmooth=0.0f;int m_motionMode=0;std::wstring m_lutPath;bool m_lutLoaded=false;   // the preview actually applied m_lutPath
    bool m_fxLut=true,m_fxSharpen=true,m_fxTone=true,m_fxFlow=true,m_fxDepth=true,m_bypassFX=false,m_lanczos4K=false;
    int m_nrModelPick=-1;VideoFrame m_lastFrame;
    // Inspection zoom (uv-space center + scale; 1 = off). Right-drag a rect on
    // the video to zoom in, left-drag pans, Ctrl+wheel zooms at the cursor,
    // plain right-click resets. Present-stage only; never baked into exports.
    float m_zoom=1.0f,m_zoomCX=0.5f,m_zoomCY=0.5f;
    bool m_zoomRectDrag=false,m_zoomPan=false;POINT m_zoomStart{},m_zoomLast{};
    // A/B compare separator: orient 1 = vertical split (drag left/right), 2 =
    // horizontal split (drag up/down). Position is screen 0..1. While armed,
    // left-drag on the video moves the separator and its drag direction chooses
    // the orientation.
    bool m_compareOn=false;int m_compareOrient=1;float m_comparePos=0.5f;
    bool m_compareDrag=false;
    // Direct DLSS-NR per-evaluate knobs (live-settable; -1 skin = follow local).
    float m_nrIntensity=1.0f,m_nrLocalStructure=1.0f,m_nrSkinStructure=-1.0f;bool m_nrAutoMask=true;
    std::string m_exportBuf;uint64_t m_exportFrames=0,m_exportTotal=1;int m_exportPct=-1;
};

// --input/--export on the player is a shortcut to the headless exporter that
// ships beside it - the same tool the GUI's Export button spawns. Running a
// second, thinner copy of the pipeline inside the player only ever produced a
// worse file (no LUT, no colour settings, no effects, no scene-cut warmup), so
// this forwards instead of duplicating.
static int RunExport(const AppOptions& opt){
    if(opt.file.empty()||opt.exportPath.empty()){LOG("export: need --input and --export");return 2;}
    const std::filesystem::path tool=smru::paths::ExeDirectory()/smru::kExporterExeW;
    std::error_code ec;
    if(!std::filesystem::is_regular_file(tool,ec)){LOG("export: "<<smru::text::WideToUtf8(smru::kExporterExeW)<<" not found next to the player");return 3;}
    const std::wstring cmd=smru::proc::Quote(tool.wstring())+L" --input "+smru::proc::Quote(opt.file)+
                           L" --export "+smru::proc::Quote(opt.exportPath);
    LOG("export: forwarding to "<<smru::text::WideToUtf8(cmd));
    smru::proc::Options spawn;
    spawn.out=smru::proc::Stdio::Console;spawn.err=smru::proc::Stdio::Console;
    smru::proc::Child child;
    if(!smru::proc::Spawn(cmd,spawn,child)){LOG("export: could not start the exporter");return 4;}
    WaitForSingleObject(child.process,INFINITE);
    DWORD rc=1;GetExitCodeProcess(child.process,&rc);
    smru::proc::Close(child);
    LOG("export: exporter finished rc="<<rc);
    return int(rc);
}

int WINAPI wWinMain(HINSTANCE hi,HINSTANCE,LPWSTR,int){
    Log::SetFileName(smru::kPlayerLogFile);
    // Extract any missing runtime files from the embedded payload. No relaunch
    // guard any more: nothing in the payload needs to hook process start.
    ProvisionPayload();
    // ffmpeg/ffprobe for the children, and the model cache the Python tools
    // download into - see smru::paths::PublishChildEnvironment.
    {const std::filesystem::path models=smru::paths::PublishChildEnvironment();
     if(models.empty())LOG("Model cache: leaving the per-user default (already set, or the app folder is not writable).");
     else LOG("Model cache: "<<smru::text::WideToUtf8(models.wstring())<<" (copied with the app)");}
    EnableDarkMenus();
    if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED|COINIT_DISABLE_OLE1DDE)))return 1;if(FAILED(MFStartup(MF_VERSION,MFSTARTUP_FULL))){CoUninitialize();return 1;}AppOptions opt=ParseArgs();if(!opt.exportPath.empty()){int rc=RunExport(opt);MFShutdown();CoUninitialize();return rc;}PlayerApp app(opt);if(!app.Create(hi)){MFShutdown();CoUninitialize();return 1;}MSG msg{};bool quit=false;while(app.Running()&&!quit){while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){quit=true;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}if(quit)break;app.Tick();if(app.NeedsRealtimeTick())Sleep(app.TickSleepMs());else WaitMessage();}MFShutdown();CoUninitialize();return 0;}
