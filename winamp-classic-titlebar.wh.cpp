// ==WindhawkMod==
// @id              winamp-classic-titlebar
// @name            Winamp Classic Title Bar
// @description     Replaces standard window title bars with a thin Winamp/MilkDrop-style caption (centered uppercase title, engraved grooves, beveled gradient, small min/max/close buttons)
// @version         0.9
// @author          you
// @github          https://github.com/yourname
// @include         *
// @exclude         winamp.exe
// @compilerOptions -lgdi32 -luser32 -lcomctl32 -ldwmapi
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
# Winamp Classic Title Bar

Repaints the *standard* (OS-drawn) window title bar to look like the classic
Winamp / MilkDrop skin: a thin near-black bar with a beveled blue/purple
gradient, centered uppercase title text flanked by engraved grooves, and small
minimize / maximize / close buttons on the right.

Apps that draw their own title bar (Blender, Chrome, VS Code, etc.) are left
untouched, because they never route their non-client painting through
`DefWindowProc`.

**This is a starting point.** Non-client painting under DWM has quirks — expect
to tune the height/colors/button geometry for your setup.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- captionHeight: 22
  $name: Title bar height (px)
- borderSize: 6
  $name: Side/bottom border thickness (px)
  $description: Also the window-resize grab width. Thinner looks more Winamp-like but is harder to grab.
- bgTop: "16161f"
  $name: Gradient top color (RRGGBB)
- bgBottom: "08080c"
  $name: Gradient bottom color (RRGGBB)
- titleColor: "c8cddc"
  $name: Title text color (RRGGBB)
- accent: "5a4a8c"
  $name: Bevel/accent color (RRGGBB)
- excludedClasses:
  - "ApplicationFrameWindow"
  - "Windows.UI.Core.CoreWindow"
  - "WinUIDesktopWin32WindowClass"
  - "Chrome_WidgetWin_1"
  - "Chrome_WidgetWin_0"
  - "MozillaWindowClass"
  - "XamlExplorerHostIslandWindow"
  - "CASCADIA_HOSTING_WINDOW_CLASS"
  $name: Excluded window classes
  $description: Windows of these classes keep their own title bar (apps that draw custom frames). Add a class here if the mod breaks an app.
*/
// ==/WindhawkModSettings==

#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <windhawk_utils.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <vector>

// ---- settings ----
struct {
    int captionHeight;
    int borderSize;
    COLORREF bgTop;
    COLORREF bgBottom;
    COLORREF titleColor;
    COLORREF accent;
    std::vector<std::wstring> excluded;
} g;

// Top inset (frame metric) used by NCCALCSIZE / painting.
static int FrameX() { return GetSystemMetrics(SM_CXSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER); }
static int FrameY() { return GetSystemMetrics(SM_CYSIZEFRAME) + GetSystemMetrics(SM_CXPADDEDBORDER); }

// ---- helpers ----

// Parse an "RRGGBB" string into a COLORREF (0x00BBGGRR for GDI).
static COLORREF ParseColor(PCWSTR s, COLORREF fallback) {
    if (!s || !*s) return fallback;
    wchar_t* end = nullptr;
    unsigned long v = wcstoul(s, &end, 16);
    if (end == s) return fallback;
    BYTE r = (v >> 16) & 0xFF, gg = (v >> 8) & 0xFF, b = v & 0xFF;
    return RGB(r, gg, b);
}

static COLORREF Lerp(COLORREF a, COLORREF b, double t) {
    int ra = GetRValue(a), ga = GetGValue(a), ba = GetBValue(a);
    int rb = GetRValue(b), gb = GetGValue(b), bb = GetBValue(b);
    return RGB((int)(ra + (rb - ra) * t),
               (int)(ga + (gb - ga) * t),
               (int)(ba + (bb - ba) * t));
}

static COLORREF Dim(COLORREF c, double f) {
    return RGB((int)(GetRValue(c) * f), (int)(GetGValue(c) * f), (int)(GetBValue(c) * f));
}

// Should we take over this window's title bar?
static bool ShouldCustomize(HWND hWnd) {
    LONG style = GetWindowLong(hWnd, GWL_STYLE);
    if (!(style & WS_CAPTION)) return false;   // no standard caption
    if (style & WS_CHILD) return false;
    wchar_t cls[128] = {0};
    GetClassNameW(hWnd, cls, 127);
    for (const auto& ex : g.excluded)
        if (_wcsicmp(cls, ex.c_str()) == 0) return false;
    return true;
}

// Caption rect in WINDOW-relative coords (origin = top-left of the whole window,
// including the frame border).
static RECT CaptionRect(HWND hWnd) {
    RECT wr; GetWindowRect(hWnd, &wr);
    int topOff = IsZoomed(hWnd) ? FrameY() : 0;  // maximized windows hang off-screen by the frame
    RECT cap;
    cap.left   = 0;
    cap.right  = wr.right - wr.left;
    cap.top    = topOff;
    cap.bottom = topOff + g.captionHeight;
    return cap;
}

// Compute the three button rects from the caption rect.
static void ButtonRects(const RECT& cap, RECT* outMin, RECT* outMax, RECT* outClose) {
    int bw = 20;                       // wider, easier to click
    int top = cap.top;
    int bottom = cap.bottom;           // full caption height = full hit area, no gaps
    int right = cap.right - 4;
    *outClose = { right - bw, top, right, bottom }; right -= bw;
    *outMax   = { right - bw, top, right, bottom }; right -= bw;
    *outMin   = { right - bw, top, right, bottom };
}

static void DrawButtonGlyph(HDC hdc, const RECT& r, int kind, COLORREF fg) {
    HPEN pen = CreatePen(PS_SOLID, 1, fg);
    HPEN old = (HPEN)SelectObject(hdc, pen);
    int cx = (r.left + r.right) / 2;
    int cy = (r.top + r.bottom) / 2;
    if (kind == 0) {            // minimize: short underline
        MoveToEx(hdc, cx - 3, cy + 2, NULL); LineTo(hdc, cx + 4, cy + 2);
    } else if (kind == 1) {     // maximize: small box
        Rectangle(hdc, cx - 4, cy - 3, cx + 4, cy + 3);
    } else {                    // close: X
        MoveToEx(hdc, cx - 3, cy - 3, NULL); LineTo(hdc, cx + 4, cy + 4);
        MoveToEx(hdc, cx + 3, cy - 3, NULL); LineTo(hdc, cx - 4, cy + 4);
    }
    SelectObject(hdc, old);
    DeleteObject(pen);
}

static void PaintCaption(HWND hWnd, bool active) {
    HDC hdc = GetWindowDC(hWnd);
    if (!hdc) return;

    RECT cap = CaptionRect(hWnd);
    int h = cap.bottom - cap.top;
    if (h <= 0) { ReleaseDC(hWnd, hdc); return; }

    double f = active ? 1.0 : 0.6;
    COLORREF top    = Dim(g.bgTop, f);
    COLORREF bottom = Dim(g.bgBottom, f);
    COLORREF accent = Dim(g.accent, f);
    COLORREF title  = Dim(g.titleColor, active ? 1.0 : 0.7);

    // Paint the left/right/bottom borders ourselves. With DWM non-client
    // rendering disabled, nothing else fills these regions, so they'd show
    // through as white otherwise.
    {
        RECT wr; GetWindowRect(hWnd, &wr);
        int W = wr.right - wr.left;
        int H = wr.bottom - wr.top;
        int blr = IsZoomed(hWnd) ? FrameX() : g.borderSize;
        int bb  = IsZoomed(hWnd) ? FrameY() : g.borderSize;
        HBRUSH fb = CreateSolidBrush(bottom);
        RECT leftR  = { 0,       cap.bottom, blr,    H };
        RECT rightR = { W - blr, cap.bottom, W,      H };
        RECT botR   = { 0,       H - bb,     W,      H };
        FillRect(hdc, &leftR, fb);
        FillRect(hdc, &rightR, fb);
        FillRect(hdc, &botR, fb);
        DeleteObject(fb);
        // subtle inner highlight along the client edge (bevel)
        HPEN edge = CreatePen(PS_SOLID, 1, Lerp(bottom, RGB(255, 255, 255), 0.12));
        HPEN oe = (HPEN)SelectObject(hdc, edge);
        MoveToEx(hdc, blr - 1, cap.bottom, NULL);     LineTo(hdc, blr - 1, H - bb);       // left
        MoveToEx(hdc, W - blr, cap.bottom, NULL);     LineTo(hdc, W - blr, H - bb);       // right
        MoveToEx(hdc, blr - 1, H - bb, NULL);         LineTo(hdc, W - blr + 1, H - bb);   // bottom
        SelectObject(hdc, oe);
        DeleteObject(edge);
    }

    // gradient fill (row by row)
    for (int y = cap.top; y < cap.bottom; y++) {
        double t = (double)(y - cap.top) / h;
        RECT row = { cap.left, y, cap.right, y + 1 };
        HBRUSH b = CreateSolidBrush(Lerp(top, bottom, t));
        FillRect(hdc, &row, b);
        DeleteObject(b);
    }

    // bevel: light line on top, shadow on bottom
    HPEN lightPen = CreatePen(PS_SOLID, 1, Lerp(accent, RGB(255,255,255), 0.25));
    HPEN darkPen  = CreatePen(PS_SOLID, 1, RGB(0,0,0));
    HPEN oldPen = (HPEN)SelectObject(hdc, lightPen);
    MoveToEx(hdc, cap.left, cap.top, NULL);  LineTo(hdc, cap.right, cap.top);
    SelectObject(hdc, darkPen);
    MoveToEx(hdc, cap.left, cap.bottom - 1, NULL); LineTo(hdc, cap.right, cap.bottom - 1);
    SelectObject(hdc, oldPen);
    DeleteObject(lightPen);
    DeleteObject(darkPen);

    // title text (centered, uppercase)
    wchar_t buf[256] = {0};
    GetWindowTextW(hWnd, buf, 255);
    CharUpperW(buf);

    HFONT font = CreateFontW(-(g.captionHeight - 9), 0, 0, 0, FW_BOLD,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FF_SWISS, L"Tahoma");
    HFONT oldFont = (HFONT)SelectObject(hdc, font);
    SetBkMode(hdc, TRANSPARENT);

    RECT minR, maxR, closeR;
    ButtonRects(cap, &minR, &maxR, &closeR);

    RECT titleRect = { cap.left + 6, cap.top, minR.left - 6, cap.bottom };
    SIZE ts; GetTextExtentPoint32W(hdc, buf, (int)wcslen(buf), &ts);
    int centerX = (titleRect.left + titleRect.right) / 2;
    int textX = centerX - ts.cx / 2;
    int midY = (cap.top + cap.bottom) / 2;

    // engraved grooves flanking the text (dark line + light line below)
    HPEN gDark  = CreatePen(PS_SOLID, 1, RGB(0,0,0));
    HPEN gLight = CreatePen(PS_SOLID, 1, Lerp(accent, RGB(255,255,255), 0.35));
    SelectObject(hdc, gDark);
    MoveToEx(hdc, titleRect.left, midY, NULL);     LineTo(hdc, textX - 6, midY);
    MoveToEx(hdc, textX + ts.cx + 6, midY, NULL);  LineTo(hdc, titleRect.right, midY);
    SelectObject(hdc, gLight);
    MoveToEx(hdc, titleRect.left, midY + 1, NULL);     LineTo(hdc, textX - 6, midY + 1);
    MoveToEx(hdc, textX + ts.cx + 6, midY + 1, NULL);  LineTo(hdc, titleRect.right, midY + 1);
    SelectObject(hdc, oldPen);
    DeleteObject(gDark);
    DeleteObject(gLight);

    // title text with subtle shadow
    SetTextColor(hdc, RGB(0,0,0));
    TextOutW(hdc, textX + 1, cap.top + (h - ts.cy) / 2 + 1, buf, (int)wcslen(buf));
    SetTextColor(hdc, title);
    TextOutW(hdc, textX, cap.top + (h - ts.cy) / 2, buf, (int)wcslen(buf));

    SelectObject(hdc, oldFont);
    DeleteObject(font);

    // buttons
    DrawButtonGlyph(hdc, minR, 0, title);
    DrawButtonGlyph(hdc, maxR, 1, title);
    DrawButtonGlyph(hdc, closeR, 2, title);

    ReleaseDC(hWnd, hdc);
}

// ---- subclassing ----
//
// We subclass each standard window directly (rather than hooking DefWindowProc)
// so we also catch apps like File Explorer whose own window procedure draws the
// non-client area without delegating to DefWindowProc.

// Ribbon-framework apps (File Explorer, Paint) leave a strip of their OWN client
// area unpainted above the ribbon dock — the reserved caption band that used to
// be glass. Returns that band's height (0 for normal windows, so they're
// untouched). The band runs from client-top down to the top of the ribbon dock.
static int ClientTopBandHeight(HWND hWnd) {
    for (HWND c = GetWindow(hWnd, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        if (!IsWindowVisible(c)) continue;
        char cls[64] = ""; GetClassNameA(c, cls, 64);
        if (strstr(cls, "Ribbon")) {   // e.g. UIRibbonCommandBarDock
            RECT rr; GetWindowRect(c, &rr);
            POINT p = { rr.left, rr.top }; ScreenToClient(hWnd, &p);
            return (p.y > 0 && p.y < 200) ? p.y : 0;
        }
    }
    return 0;
}

static void FillTopBand(HWND hWnd, HDC hdc) {
    int bandH = ClientTopBandHeight(hWnd);
    if (bandH <= 0) return;
    RECT cr; GetClientRect(hWnd, &cr);
    RECT band = { 0, 0, cr.right, bandH };
    HBRUSH b = CreateSolidBrush(g.bgBottom);
    FillRect(hdc, &band, b);
    DeleteObject(b);
}

LRESULT CALLBACK CaptionSubclassProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                     DWORD_PTR dwRefData) {
    switch (msg) {
    case WM_ERASEBKGND: {
        LRESULT r = DefSubclassProc(hWnd, msg, wParam, lParam);
        FillTopBand(hWnd, (HDC)wParam);   // cover the ribbon caption band
        return r ? r : 1;
    }

    case WM_PAINT: {
        // Ribbon apps repaint that band white; let them paint, then cover it.
        LRESULT r = DefSubclassProc(hWnd, msg, wParam, lParam);
        if (ClientTopBandHeight(hWnd) > 0) {
            HDC hdc = GetDC(hWnd);
            FillTopBand(hWnd, hdc);
            ReleaseDC(hWnd, hdc);
        }
        return r;
    }

    case WM_NCCALCSIZE:
        if (wParam) {
            NCCALCSIZE_PARAMS* p = (NCCALCSIZE_PARAMS*)lParam;
            RECT* rc = &p->rgrc[0];   // in: proposed window rect -> out: client rect
            // Maximized windows hang off-screen by the real frame size, so use it
            // then; otherwise use our thin custom border.
            bool zoomed = IsZoomed(hWnd);
            int blr = zoomed ? FrameX() : g.borderSize;
            int bb  = zoomed ? FrameY() : g.borderSize;
            rc->left   += blr;
            rc->right  -= blr;
            rc->bottom -= bb;
            rc->top    += g.captionHeight;
            if (zoomed) rc->top += FrameY();  // maximized: account for off-screen frame
            return 0;
        }
        break;

    case WM_NCPAINT:
        PaintCaption(hWnd, GetForegroundWindow() == hWnd);
        return 0;

    // Undocumented themed-caption redraw messages — suppress so the default
    // caption never paints over ours.
    case 0x00AE:  // WM_NCUAHDRAWCAPTION
    case 0x00AF:  // WM_NCUAHDRAWFRAME
        return 0;

    case WM_NCACTIVATE:
        PaintCaption(hWnd, wParam != 0);
        return TRUE;   // prevent default caption redraw

    case WM_SETTEXT: {
        LRESULT lr = DefSubclassProc(hWnd, msg, wParam, lParam);
        PaintCaption(hWnd, GetForegroundWindow() == hWnd);
        return lr;
    }

    case WM_NCHITTEST: {
        POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
        RECT wr; GetWindowRect(hWnd, &wr);
        POINT lp = { pt.x - wr.left, pt.y - wr.top };  // window-relative
        RECT cap = CaptionRect(hWnd);
        if (lp.y >= cap.top && lp.y < cap.bottom && lp.x >= cap.left && lp.x < cap.right) {
            RECT minR, maxR, closeR;
            ButtonRects(cap, &minR, &maxR, &closeR);
            if (PtInRect(&closeR, lp)) return HTCLOSE;
            if (PtInRect(&maxR, lp))   return HTMAXBUTTON;
            if (PtInRect(&minR, lp))   return HTMINBUTTON;
            return HTCAPTION;   // drag
        }
        break;  // borders -> default (resize)
    }
    }
    // The subclass is auto-removed on WM_NCDESTROY by Windhawk's wrapper.
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

static void TrySubclass(HWND hWnd) {
    if (!ShouldCustomize(hWnd)) return;
    if (WindhawkUtils::SetWindowSubclassFromAnyThread(hWnd, CaptionSubclassProc, 0)) {
        // Stop DWM from compositing its own caption/frame on top of our painting.
        // This is what made File Explorer ignore the custom caption even though
        // our subclass was receiving the non-client messages.
        DWMNCRENDERINGPOLICY policy = DWMNCRP_DISABLED;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        // Collapse any glass frame the app already extended into the client
        // (Ribbon apps like Paint / File Explorer do this for their toolbar),
        // otherwise that region renders as a white/transparent band.
        MARGINS zero = { 0, 0, 0, 0 };
        DwmExtendFrameIntoClientArea(hWnd, &zero);
        // Force a frame recompute so our thin caption applies right away.
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
}

// Ribbon-framework apps repeatedly extend the glass frame into the client area.
// Neutralize it for our windows so no glass band appears under the caption.
using DwmExtendFrameIntoClientArea_t = HRESULT(WINAPI*)(HWND, const MARGINS*);
DwmExtendFrameIntoClientArea_t DwmExtendFrameIntoClientArea_Original;
HRESULT WINAPI DwmExtendFrameIntoClientArea_Hook(HWND hWnd, const MARGINS* m) {
    if (ShouldCustomize(hWnd)) {
        MARGINS zero = { 0, 0, 0, 0 };
        return DwmExtendFrameIntoClientArea_Original(hWnd, &zero);
    }
    return DwmExtendFrameIntoClientArea_Original(hWnd, m);
}

// ---- hooks: subclass every standard window as it's created ----

using CreateWindowExW_t = HWND(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int,
                                        HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExW_t CreateWindowExW_Original;
HWND WINAPI CreateWindowExW_Hook(DWORD ex, LPCWSTR cls, LPCWSTR name, DWORD style,
                                 int x, int y, int w, int h, HWND parent, HMENU menu,
                                 HINSTANCE inst, LPVOID param) {
    HWND hWnd = CreateWindowExW_Original(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (hWnd) TrySubclass(hWnd);
    return hWnd;
}

using CreateWindowExA_t = HWND(WINAPI*)(DWORD, LPCSTR, LPCSTR, DWORD, int, int, int, int,
                                        HWND, HMENU, HINSTANCE, LPVOID);
CreateWindowExA_t CreateWindowExA_Original;
HWND WINAPI CreateWindowExA_Hook(DWORD ex, LPCSTR cls, LPCSTR name, DWORD style,
                                 int x, int y, int w, int h, HWND parent, HMENU menu,
                                 HINSTANCE inst, LPVOID param) {
    HWND hWnd = CreateWindowExA_Original(ex, cls, name, style, x, y, w, h, parent, menu, inst, param);
    if (hWnd) TrySubclass(hWnd);
    return hWnd;
}

// ---- mod lifecycle ----

void LoadSettings() {
    g.captionHeight = Wh_GetIntSetting(L"captionHeight");
    if (g.captionHeight < 12) g.captionHeight = 12;
    if (g.captionHeight > 40) g.captionHeight = 40;

    g.borderSize = Wh_GetIntSetting(L"borderSize");
    if (g.borderSize < 1) g.borderSize = 1;
    if (g.borderSize > 16) g.borderSize = 16;

    PCWSTR s;
    s = Wh_GetStringSetting(L"bgTop");      g.bgTop      = ParseColor(s, RGB(0x16,0x16,0x1f)); Wh_FreeStringSetting(s);
    s = Wh_GetStringSetting(L"bgBottom");   g.bgBottom   = ParseColor(s, RGB(0x08,0x08,0x0c)); Wh_FreeStringSetting(s);
    s = Wh_GetStringSetting(L"titleColor"); g.titleColor = ParseColor(s, RGB(0xc8,0xcd,0xdc)); Wh_FreeStringSetting(s);
    s = Wh_GetStringSetting(L"accent");     g.accent     = ParseColor(s, RGB(0x5a,0x4a,0x8c)); Wh_FreeStringSetting(s);

    g.excluded.clear();
    for (int i = 0; ; i++) {
        PCWSTR cls = Wh_GetStringSetting(L"excludedClasses[%d]", i);
        bool empty = (!cls || !*cls);
        if (!empty) g.excluded.push_back(cls);
        Wh_FreeStringSetting(cls);
        if (empty) break;
    }
}

// Subclass all of this process's existing top-level windows.
static BOOL CALLBACK SubclassEnumProc(HWND hWnd, LPARAM) {
    DWORD pid; GetWindowThreadProcessId(hWnd, &pid);
    if (pid == GetCurrentProcessId()) TrySubclass(hWnd);
    return TRUE;
}

// Remove our subclass and revert frames (used on unload).
static BOOL CALLBACK UnsubclassEnumProc(HWND hWnd, LPARAM) {
    DWORD pid; GetWindowThreadProcessId(hWnd, &pid);
    if (pid == GetCurrentProcessId()) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(hWnd, CaptionSubclassProc);
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    return TRUE;
}

BOOL Wh_ModInit() {
    LoadSettings();
    Wh_SetFunctionHook((void*)CreateWindowExW, (void*)CreateWindowExW_Hook,
                       (void**)&CreateWindowExW_Original);
    Wh_SetFunctionHook((void*)CreateWindowExA, (void*)CreateWindowExA_Hook,
                       (void**)&CreateWindowExA_Original);
    Wh_SetFunctionHook((void*)DwmExtendFrameIntoClientArea, (void*)DwmExtendFrameIntoClientArea_Hook,
                       (void**)&DwmExtendFrameIntoClientArea_Original);
    return TRUE;
}

void Wh_ModAfterInit() {
    EnumWindows(SubclassEnumProc, 0);   // catch windows that already exist
}

void Wh_ModSettingsChanged() {
    LoadSettings();
    EnumWindows(SubclassEnumProc, 0);   // re-apply height/colors to open windows
}

void Wh_ModUninit() {
    EnumWindows(UnsubclassEnumProc, 0); // revert everything to default
}
