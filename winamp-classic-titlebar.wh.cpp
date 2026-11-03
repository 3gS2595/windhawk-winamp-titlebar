// ==WindhawkMod==
// @id              winamp-classic-titlebar
// @name            Winamp Classic Title Bar
// @description     Replaces standard window title bars with a thin Winamp/MilkDrop-style caption (centered uppercase title, engraved grooves, beveled gradient, small min/max/close buttons)
// @version         0.11
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

The buttons are run entirely by the mod (hover, press, release), so what you
click is exactly what you see, and Windows' own full-size caption buttons are
never drawn over the thin bar.

Apps that draw their own title bar (Blender, Chrome, VS Code, etc.) are left
untouched, because they never route their non-client painting through
`DefWindowProc`.

**This is a starting point.** Non-client painting under DWM has quirks — expect
to tune the height/colors/button geometry for your setup.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- captionHeight: 18
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
#include <stdlib.h>
#include <string.h>
#include <string>
#include <unordered_set>
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

// ---- unload safety ----
//
// When Windhawk swaps in a new build it unloads this DLL from every running
// process. Anything still pointing into it at that moment crashes the app, so:
// every window we subclass is tracked and un-subclassed on its own thread,
// threads currently inside our code are counted, and if anything is left over
// the DLL pins itself in memory instead of being unloaded (see Wh_ModUninit).
static const UINT_PTR kSubclassId = 0x57A3;
static volatile LONG g_unloading;   // set once unloading starts: no new subclasses
static volatile LONG g_inFlight;    // threads currently running our subclass proc / hooks

struct InFlight {
    InFlight()  { InterlockedIncrement(&g_inFlight); }
    ~InFlight() { InterlockedDecrement(&g_inFlight); }
};

static SRWLOCK g_trackLock = SRWLOCK_INIT;
static std::unordered_set<HWND> g_tracked;   // windows that currently have our subclass

static void Track(HWND hWnd) {
    AcquireSRWLockExclusive(&g_trackLock);
    g_tracked.insert(hWnd);
    ReleaseSRWLockExclusive(&g_trackLock);
}

static void Untrack(HWND hWnd) {
    AcquireSRWLockExclusive(&g_trackLock);
    g_tracked.erase(hWnd);
    ReleaseSRWLockExclusive(&g_trackLock);
}

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

static void FillSolid(HDC hdc, int l, int t, int r, int b, COLORREF c) {
    RECT rc = { l, t, r, b };
    HBRUSH br = CreateSolidBrush(c);
    FillRect(hdc, &rc, br);
    DeleteObject(br);
}

// Should we take over this window's title bar?
static bool ShouldCustomize(HWND hWnd) {
    LONG style = GetWindowLong(hWnd, GWL_STYLE);
    if (!(style & WS_CAPTION)) return false;   // no standard caption
    if (style & WS_CHILD) return false;
    // Real top-level windows only. Message-only windows (parent HWND_MESSAGE)
    // often carry WS_CAPTION too (XAML timer windows, PARSEC_CLIP, Chrome's
    // message window...) but are never shown, receive messages constantly and
    // are invisible to EnumWindows: subclassing them is what crashed apps when
    // 0.9 was unloaded.
    if (GetAncestor(hWnd, GA_PARENT) != GetDesktopWindow()) return false;
    wchar_t cls[128] = {0};
    GetClassNameW(hWnd, cls, 127);
    for (const auto& ex : g.excluded)
        if (_wcsicmp(cls, ex.c_str()) == 0) return false;
    return true;
}

// ---- geometry ----
//
// Every rect here is WINDOW-relative (origin = top-left of the whole window,
// frame included), the same space WM_NCHITTEST points are converted into, so
// what gets painted and what gets hit-tested can't drift apart.

// The full-width caption band we paint.
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

// The part of the band that's actually on screen. A maximized window also hangs
// off the left/right monitor edges by the frame size, so the title and buttons
// are inset by that much (otherwise the close button sits half off-screen).
static RECT CaptionContentRect(HWND hWnd, const RECT& cap) {
    RECT rc = cap;
    if (IsZoomed(hWnd)) { rc.left += FrameX(); rc.right -= FrameX(); }
    return rc;
}

enum { BTN_MIN, BTN_MAX, BTN_CLOSE, BTN_COUNT };
static const int kButtonHit[BTN_COUNT] = { HTMINBUTTON, HTMAXBUTTON, HTCLOSE };

struct CaptionButtons {
    RECT rc[BTN_COUNT];       // hit + paint area; empty when the button isn't shown
    bool enabled[BTN_COUNT];
};

static bool IsButtonHit(WPARAM ht) {
    return ht == HTMINBUTTON || ht == HTMAXBUTTON || ht == HTCLOSE;
}

// Same rule Windows uses for its own close button: disabled by CS_NOCLOSE, or
// when SC_CLOSE is greyed out of / removed from the window's system menu.
static bool IsCloseEnabled(HWND hWnd) {
    if (GetClassLongPtrW(hWnd, GCL_STYLE) & CS_NOCLOSE) return false;
    HMENU sys = GetSystemMenu(hWnd, FALSE);
    if (!sys) return true;
    UINT state = GetMenuState(sys, SC_CLOSE, MF_BYCOMMAND);
    if (state == (UINT)-1) return false;
    return !(state & (MF_GRAYED | MF_DISABLED));
}

// Which buttons a window gets also follows the stock rules: none without
// WS_SYSMENU, close only for tool windows, and min/max shown as a pair when
// either style bit is set (the missing one drawn disabled).
static void LayoutButtons(HWND hWnd, const RECT& cap, CaptionButtons* b) {
    for (int i = 0; i < BTN_COUNT; i++) { SetRectEmpty(&b->rc[i]); b->enabled[i] = false; }
    LONG style = GetWindowLong(hWnd, GWL_STYLE);
    LONG exStyle = GetWindowLong(hWnd, GWL_EXSTYLE);
    if (!(style & WS_SYSMENU)) return;
    bool tool = (exStyle & WS_EX_TOOLWINDOW) != 0;
    bool hasMin = !tool && (style & WS_MINIMIZEBOX);
    bool hasMax = !tool && (style & WS_MAXIMIZEBOX);

    RECT content = CaptionContentRect(hWnd, cap);
    int bw = g.captionHeight > 20 ? g.captionHeight : 20;   // full caption height = hit area, no gaps
    int right = content.right - 4;
    b->rc[BTN_CLOSE] = { right - bw, cap.top, right, cap.bottom };
    b->enabled[BTN_CLOSE] = IsCloseEnabled(hWnd);
    right -= bw;
    if (hasMin || hasMax) {
        b->rc[BTN_MAX] = { right - bw, cap.top, right, cap.bottom }; right -= bw;
        b->rc[BTN_MIN] = { right - bw, cap.top, right, cap.bottom };
        b->enabled[BTN_MAX] = hasMax;
        b->enabled[BTN_MIN] = hasMin;
    }
}

// Enabled caption button under a screen point, as an HT* code (0 = none).
static int ButtonAtScreenPoint(HWND hWnd, POINT pt) {
    RECT wr; GetWindowRect(hWnd, &wr);
    POINT lp = { pt.x - wr.left, pt.y - wr.top };
    CaptionButtons b;
    LayoutButtons(hWnd, CaptionRect(hWnd), &b);
    for (int i = 0; i < BTN_COUNT; i++)
        if (b.enabled[i] && PtInRect(&b.rc[i], lp)) return kButtonHit[i];
    return 0;
}

// Screen position of the mouse message being handled.
static POINT MessageScreenPos() {
    DWORD pos = GetMessagePos();
    return { GET_X_LPARAM(pos), GET_Y_LPARAM(pos) };
}

// ---- button hover / press state ----
//
// DefWindowProc must never see a mouse message aimed at one of our buttons. If
// it does, it hot-tracks, tooltips and press-tracks its OWN caption button,
// which it places using the stock (much taller) caption metrics: that is the
// big grey close button that appeared below the thin bar, and the reason a
// click only registered where the two buttons overlapped. So the mod runs the
// buttons itself and fires the same WM_SYSCOMMAND the stock button would.
//
// Mouse capture and message dispatch are per-thread, so this state is too.
struct ButtonTracking {
    HWND hotWnd;        // window whose button is under the pointer
    int  hotHit;        // HTMINBUTTON / HTMAXBUTTON / HTCLOSE
    HWND pressWnd;      // window whose button is held down (it has the capture)
    int  pressHit;
    bool pressInside;   // pointer is still over the held button
};
static thread_local ButtonTracking t_btn;

enum ButtonLook { LOOK_NORMAL, LOOK_HOT, LOOK_PRESSED, LOOK_DISABLED };

static ButtonLook LookFor(HWND hWnd, int hit, bool enabled) {
    if (!enabled) return LOOK_DISABLED;
    if (t_btn.pressWnd == hWnd)
        return (t_btn.pressHit == hit && t_btn.pressInside) ? LOOK_PRESSED : LOOK_NORMAL;
    if (t_btn.hotWnd == hWnd && t_btn.hotHit == hit) return LOOK_HOT;
    return LOOK_NORMAL;
}

// ---- painting ----

static void Bevel(HDC hdc, const RECT& f, COLORREF topLeft, COLORREF bottomRight) {
    FillSolid(hdc, f.left, f.top, f.right - 1, f.top + 1, topLeft);
    FillSolid(hdc, f.left, f.top, f.left + 1, f.bottom - 1, topLeft);
    FillSolid(hdc, f.left, f.bottom - 1, f.right, f.bottom, bottomRight);
    FillSolid(hdc, f.right - 1, f.top, f.right, f.bottom, bottomRight);
}

// Glyphs are drawn pixel-exact in an n x n box at (x, y); n is odd so the X
// has a true center pixel.
static void DrawGlyph(HDC hdc, int x, int y, int n, int hit, bool zoomed, COLORREF c) {
    if (hit == HTMINBUTTON) {
        FillSolid(hdc, x, y + n - 1, x + n, y + n, c);
    } else if (hit == HTMAXBUTTON && !zoomed) {
        FillSolid(hdc, x, y, x + n, y + 2, c);                 // thick top edge
        FillSolid(hdc, x, y, x + 1, y + n, c);
        FillSolid(hdc, x + n - 1, y, x + n, y + n, c);
        FillSolid(hdc, x, y + n - 1, x + n, y + n, c);
    } else if (hit == HTMAXBUTTON) {                            // restore: two boxes
        int s = n - 2;
        FillSolid(hdc, x + 2, y, x + n, y + 1, c);              // back box: top,
        FillSolid(hdc, x + n - 1, y, x + n, y + s, c);          // right,
        FillSolid(hdc, x + 2, y, x + 3, y + 2, c);              // left stub,
        FillSolid(hdc, x + s, y + s - 1, x + n, y + s, c);      // bottom stub
        FillSolid(hdc, x, y + 2, x + s, y + 3, c);              // front box
        FillSolid(hdc, x, y + 2, x + 1, y + n, c);
        FillSolid(hdc, x + s - 1, y + 2, x + s, y + n, c);
        FillSolid(hdc, x, y + n - 1, x + s, y + n, c);
    } else {                                                    // close: X
        for (int i = 0; i < n; i++) {
            SetPixelV(hdc, x + i, y + i, c);
            SetPixelV(hdc, x + n - 1 - i, y + i, c);
        }
    }
}

static void DrawCaptionButton(HDC hdc, const RECT& r, int hit, ButtonLook look, bool zoomed,
                              COLORREF top, COLORREF bottom, COLORREF accent, COLORREF fg) {
    // Small square face centered in the (larger) hit rect, Winamp-sized.
    int w = r.right - r.left, h = r.bottom - r.top;
    int s = h - 7;
    if (s > 17) s = 17;
    if (s > w - 4) s = w - 4;
    if (s < 7) s = 7;
    if (!(s & 1)) s--;
    RECT f;
    f.left = r.left + (w - s) / 2;
    f.top  = r.top + (h - s) / 2;
    f.right = f.left + s;
    f.bottom = f.top + s;

    COLORREF glyph = fg;
    int shift = 0;
    if (look == LOOK_HOT) {
        FillSolid(hdc, f.left, f.top, f.right, f.bottom, Lerp(top, accent, 0.5));
        Bevel(hdc, f, Lerp(accent, RGB(255, 255, 255), 0.35), RGB(0, 0, 0));
        glyph = Lerp(fg, RGB(255, 255, 255), 0.5);
    } else if (look == LOOK_PRESSED) {
        FillSolid(hdc, f.left, f.top, f.right, f.bottom, Lerp(bottom, accent, 0.25));
        Bevel(hdc, f, RGB(0, 0, 0), Lerp(accent, RGB(255, 255, 255), 0.2));
        shift = 1;
    } else if (look == LOOK_DISABLED) {
        glyph = Lerp(top, fg, 0.3);
    }

    int n = s - 4;
    if (n > 9) n = 9;
    if (!(n & 1)) n--;
    int gx = f.left + (s - n) / 2 + shift;
    int gy = f.top + (s - n) / 2 + shift;
    DrawGlyph(hdc, gx, gy, n, hit, zoomed, glyph);
}

static void PaintCaption(HWND hWnd, bool active) {
    HDC hdc = GetWindowDC(hWnd);
    if (!hdc) return;

    bool zoomed = IsZoomed(hWnd);
    RECT cap = CaptionRect(hWnd);
    int h = cap.bottom - cap.top;
    int cw = cap.right - cap.left;
    if (h <= 0 || cw <= 0) { ReleaseDC(hWnd, hdc); return; }

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
        int blr = zoomed ? FrameX() : g.borderSize;
        int bb  = zoomed ? FrameY() : g.borderSize;
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

    // The caption is composed off-screen and blitted in one go, so the
    // frequent hover/press repaints don't flicker.
    HDC dc = CreateCompatibleDC(hdc);
    HBITMAP bmp = dc ? CreateCompatibleBitmap(hdc, cw, h) : NULL;
    if (!bmp) {
        if (dc) DeleteDC(dc);
        ReleaseDC(hWnd, hdc);
        return;
    }
    HGDIOBJ oldBmp = SelectObject(dc, bmp);
    SetViewportOrgEx(dc, -cap.left, -cap.top, NULL);   // keep drawing in window coords

    // gradient fill (row by row)
    for (int y = cap.top; y < cap.bottom; y++) {
        double t = (double)(y - cap.top) / h;
        FillSolid(dc, cap.left, y, cap.right, y + 1, Lerp(top, bottom, t));
    }

    // bevel: light line on top, shadow on bottom
    FillSolid(dc, cap.left, cap.top, cap.right, cap.top + 1, Lerp(accent, RGB(255, 255, 255), 0.25));
    FillSolid(dc, cap.left, cap.bottom - 1, cap.right, cap.bottom, RGB(0, 0, 0));

    // title text (centered between the left edge and the first button, uppercase)
    wchar_t buf[256] = {0};
    GetWindowTextW(hWnd, buf, 255);
    CharUpperW(buf);

    HFONT font = CreateFontW(-(g.captionHeight - 9), 0, 0, 0, FW_BOLD,
                             FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, FF_SWISS, L"Tahoma");
    HFONT oldFont = (HFONT)SelectObject(dc, font);
    SetBkMode(dc, TRANSPARENT);

    RECT content = CaptionContentRect(hWnd, cap);
    CaptionButtons btns;
    LayoutButtons(hWnd, cap, &btns);
    int titleRight = content.right - 6;
    for (int i = 0; i < BTN_COUNT; i++)
        if (!IsRectEmpty(&btns.rc[i]) && btns.rc[i].left - 6 < titleRight)
            titleRight = btns.rc[i].left - 6;
    RECT titleRect = { content.left + 6, cap.top, titleRight, cap.bottom };

    int avail = titleRect.right - titleRect.left;
    if (avail > 0) {
        int len = (int)wcslen(buf);
        SIZE ts; GetTextExtentPoint32W(dc, buf, len, &ts);
        int textW = ts.cx < avail ? ts.cx : avail;   // long titles get an ellipsis, never run into the buttons
        int textX = (titleRect.left + titleRect.right) / 2 - textW / 2;
        int textY = cap.top + (h - ts.cy) / 2;
        int midY = (cap.top + cap.bottom) / 2;

        // engraved grooves flanking the text (dark line + light line below)
        COLORREF grooveLight = Lerp(accent, RGB(255, 255, 255), 0.35);
        if (textX - 6 > titleRect.left) {
            FillSolid(dc, titleRect.left, midY, textX - 6, midY + 1, RGB(0, 0, 0));
            FillSolid(dc, titleRect.left, midY + 1, textX - 6, midY + 2, grooveLight);
        }
        if (textX + textW + 6 < titleRect.right) {
            FillSolid(dc, textX + textW + 6, midY, titleRect.right, midY + 1, RGB(0, 0, 0));
            FillSolid(dc, textX + textW + 6, midY + 1, titleRect.right, midY + 2, grooveLight);
        }

        // title text with subtle shadow
        UINT fmt = DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS | DT_LEFT | DT_TOP;
        RECT shadowR = { textX + 1, textY + 1, textX + textW + 1, textY + ts.cy + 1 };
        RECT textR   = { textX, textY, textX + textW, textY + ts.cy };
        SetTextColor(dc, RGB(0, 0, 0));
        DrawTextW(dc, buf, len, &shadowR, fmt);
        SetTextColor(dc, title);
        DrawTextW(dc, buf, len, &textR, fmt);
    }

    SelectObject(dc, oldFont);
    DeleteObject(font);

    // buttons
    for (int i = 0; i < BTN_COUNT; i++) {
        if (IsRectEmpty(&btns.rc[i])) continue;
        int hit = kButtonHit[i];
        DrawCaptionButton(dc, btns.rc[i], hit, LookFor(hWnd, hit, btns.enabled[i]), zoomed,
                          top, bottom, accent, title);
    }

    SetViewportOrgEx(dc, 0, 0, NULL);
    BitBlt(hdc, cap.left, cap.top, cw, h, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldBmp);
    DeleteObject(bmp);
    DeleteDC(dc);
    ReleaseDC(hWnd, hdc);
}

static void RepaintCaption(HWND hWnd) {
    if (IsWindow(hWnd)) PaintCaption(hWnd, GetForegroundWindow() == hWnd);
}

// ---- button hover / press tracking ----

static void SetHotButton(HWND hWnd, int hit) {
    if (t_btn.pressWnd) return;                     // mid-press: the press owns the look
    if (!hit && t_btn.hotWnd != hWnd) return;       // nothing of this window's to clear
    if (t_btn.hotWnd == hWnd && t_btn.hotHit == hit) return;
    HWND prev = t_btn.hotWnd;
    t_btn.hotWnd = hit ? hWnd : NULL;
    t_btn.hotHit = hit;
    if (prev && prev != hWnd) RepaintCaption(prev);
    RepaintCaption(hWnd);
}

static void BeginButtonPress(HWND hWnd, int hit) {
    t_btn.hotWnd = NULL;
    t_btn.hotHit = 0;
    t_btn.pressWnd = hWnd;
    t_btn.pressHit = hit;
    t_btn.pressInside = true;
    SetCapture(hWnd);   // follow the drag even off the button, like a real button
    RepaintCaption(hWnd);
}

static void UpdateButtonPress(HWND hWnd) {
    bool inside = ButtonAtScreenPoint(hWnd, MessageScreenPos()) == t_btn.pressHit;
    if (inside != t_btn.pressInside) {
        t_btn.pressInside = inside;
        RepaintCaption(hWnd);
    }
}

// Ends a press. On a real release the button fires if the pointer is still on it.
static void EndButtonPress(HWND hWnd, bool released) {
    int hit = t_btn.pressHit;
    bool fire = released && ButtonAtScreenPoint(hWnd, MessageScreenPos()) == hit;
    t_btn.pressWnd = NULL;
    t_btn.pressHit = 0;
    t_btn.pressInside = false;
    if (GetCapture() == hWnd) ReleaseCapture();
    RepaintCaption(hWnd);
    if (!fire) return;

    WPARAM cmd = hit == HTCLOSE     ? SC_CLOSE
               : hit == HTMINBUTTON ? SC_MINIMIZE
               : IsZoomed(hWnd)     ? SC_RESTORE
                                    : SC_MAXIMIZE;
    POINT pt = MessageScreenPos();
    PostMessageW(hWnd, WM_SYSCOMMAND, cmd, MAKELPARAM(pt.x, pt.y));
}

// The HT* code of a non-client mouse message, if it's aimed at one of OUR
// buttons. Hit codes from anywhere else (e.g. an app's own button in its
// client area) aren't ours and keep their default handling.
static int OurButton(HWND hWnd, WPARAM ht, LPARAM lParam) {
    if (!IsButtonHit(ht)) return 0;
    POINT pt = { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
    return ButtonAtScreenPoint(hWnd, pt) == (int)ht ? (int)ht : 0;
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
                                     UINT_PTR uIdSubclass, DWORD_PTR dwRefData) {
    InFlight inFlight;

    // Remove ourselves before the window is gone, and once unloading has
    // started (this also cleans up any window the unload pass couldn't reach).
    if (msg == WM_NCDESTROY || g_unloading) {
        RemoveWindowSubclass(hWnd, CaptionSubclassProc, kSubclassId);
        Untrack(hWnd);
        if (t_btn.hotWnd == hWnd) { t_btn.hotWnd = NULL; t_btn.hotHit = 0; }
        if (t_btn.pressWnd == hWnd) { t_btn.pressWnd = NULL; t_btn.pressHit = 0; }
        return DefSubclassProc(hWnd, msg, wParam, lParam);
    }

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
        if (PtInRect(&cap, lp)) {
            int hit = ButtonAtScreenPoint(hWnd, pt);
            return hit ? hit : HTCAPTION;   // disabled buttons just drag, like the bar
        }
        break;  // borders -> default (resize)
    }

    // -- caption buttons: handled here, never by DefWindowProc (see above) --

    case WM_NCMOUSEMOVE: {
        int hit = OurButton(hWnd, wParam, lParam);
        SetHotButton(hWnd, hit);
        if (hit) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE | TME_NONCLIENT, hWnd, 0 };
            TrackMouseEvent(&tme);   // so we hear when the pointer leaves the bar
            return 0;                // no stock hot-tracking, no "Close" tooltip
        }
        break;
    }

    case WM_NCMOUSELEAVE:
        SetHotButton(hWnd, 0);
        break;

    case WM_NCMOUSEHOVER:
        if (OurButton(hWnd, wParam, lParam)) return 0;
        break;

    case WM_NCLBUTTONDOWN:
    case WM_NCLBUTTONDBLCLK: {   // a quick second click arrives as a double-click
        int hit = OurButton(hWnd, wParam, lParam);
        if (hit) {
            BeginButtonPress(hWnd, hit);
            return 0;
        }
        break;
    }

    case WM_NCLBUTTONUP:
        if (OurButton(hWnd, wParam, lParam)) return 0;
        break;

    // While a button is held we own the capture, so the pointer arrives as
    // client-area messages; keep them from the app.
    case WM_MOUSEMOVE:
        if (t_btn.pressWnd == hWnd) {
            UpdateButtonPress(hWnd);
            return 0;
        }
        break;

    case WM_LBUTTONUP:
        if (t_btn.pressWnd == hWnd) {
            EndButtonPress(hWnd, true);
            return 0;
        }
        break;

    case WM_CAPTURECHANGED:   // capture taken away (e.g. a dialog popped up): cancel
        if (t_btn.pressWnd == hWnd && (HWND)lParam != hWnd) EndButtonPress(hWnd, false);
        break;
    }
    return DefSubclassProc(hWnd, msg, wParam, lParam);
}

// ---- adding / removing the subclass on any thread ----
//
// comctl32 subclassing only works from the thread that owns the window, so for
// other threads the request rides a WM_CALLWNDPROC hook on that thread. A hook
// sees the message before ANY window procedure, so removal can't be swallowed
// by an app's own subclass sitting above ours.

struct SubclassOp {
    bool install;
    BOOL result;
};

static UINT SubclassOpMsg() {
    static UINT msg = RegisterWindowMessageW(L"WinampClassicTitlebar_SubclassOp");
    return msg;
}

// Runs on the window's own thread.
static BOOL ApplySubclassOp(HWND hWnd, bool install) {
    if (install) {
        if (g_unloading || !SetWindowSubclass(hWnd, CaptionSubclassProc, kSubclassId, 0)) return FALSE;
        Track(hWnd);
        return TRUE;
    }
    BOOL removed = RemoveWindowSubclass(hWnd, CaptionSubclassProc, kSubclassId);
    Untrack(hWnd);
    return removed;
}

static LRESULT CALLBACK SubclassOpHook(int code, WPARAM wParam, LPARAM lParam) {
    InFlight inFlight;
    if (code == HC_ACTION) {
        const CWPSTRUCT* cwp = (const CWPSTRUCT*)lParam;
        // wParam carries this build's hook address, so another build of the
        // mod that's still in memory ignores our requests (and vice versa).
        if (cwp->message == SubclassOpMsg() && cwp->wParam == (WPARAM)&SubclassOpHook) {
            SubclassOp* op = (SubclassOp*)cwp->lParam;
            op->result = ApplySubclassOp(cwp->hwnd, op->install);
        }
    }
    return CallNextHookEx(NULL, code, wParam, lParam);
}

// Adds/removes our subclass on a window owned by any thread of this process.
// Sets *timedOut if the owning thread didn't answer in time.
static BOOL RunSubclassOp(HWND hWnd, bool install, bool* timedOut) {
    DWORD tid = GetWindowThreadProcessId(hWnd, NULL);
    if (!tid) return FALSE;
    if (tid == GetCurrentThreadId()) return ApplySubclassOp(hWnd, install);

    HHOOK hook = SetWindowsHookExW(WH_CALLWNDPROC, SubclassOpHook, NULL, tid);
    if (!hook) return FALSE;
    // On the heap: if the thread answers after we stop waiting, the hook still
    // has valid memory to write to. Leaked in that (rare) case.
    SubclassOp* op = new SubclassOp{ install, FALSE };
    DWORD_PTR ignored = 0;
    LRESULT sent = SendMessageTimeoutW(hWnd, SubclassOpMsg(), (WPARAM)&SubclassOpHook, (LPARAM)op,
                                       SMTO_NORMAL | SMTO_ABORTIFHUNG, 2000, &ignored);
    DWORD err = sent ? 0 : GetLastError();
    UnhookWindowsHookEx(hook);
    if (!sent && err == ERROR_TIMEOUT) {
        if (timedOut) *timedOut = true;
        return FALSE;
    }
    BOOL result = op->result;
    delete op;
    return result;
}

static void TrySubclass(HWND hWnd) {
    if (g_unloading || !ShouldCustomize(hWnd)) return;
    if (!RunSubclassOp(hWnd, true, NULL)) return;

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
    // Force a frame recompute so our thin caption applies right away. For
    // another thread's window, post it rather than block on a busy app.
    UINT async = GetWindowThreadProcessId(hWnd, NULL) == GetCurrentThreadId() ? 0 : SWP_ASYNCWINDOWPOS;
    SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED | async);
}

// Ribbon-framework apps repeatedly extend the glass frame into the client area.
// Neutralize it for our windows so no glass band appears under the caption.
using DwmExtendFrameIntoClientArea_t = HRESULT(WINAPI*)(HWND, const MARGINS*);
DwmExtendFrameIntoClientArea_t DwmExtendFrameIntoClientArea_Original;
HRESULT WINAPI DwmExtendFrameIntoClientArea_Hook(HWND hWnd, const MARGINS* m) {
    InFlight inFlight;
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
    InFlight inFlight;
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
    InFlight inFlight;
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

BOOL Wh_ModInit() {
    // Keep comctl32 loaded for the life of the process. Removing our subclass
    // can't always unhook comctl32's dispatcher from a window (e.g. when the app
    // subclassed the window after us), and in apps that only loaded comctl32
    // because of this mod, unloading the mod would unload comctl32 out from
    // under those windows: that crashed WezTerm, sihost and others on reload.
    HMODULE comctl;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                       (LPCWSTR)(void*)&SetWindowSubclass, &comctl);

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

void Wh_ModBeforeUninit() {
    InterlockedExchange(&g_unloading, 1);   // no new subclasses while hooks come off
}

void Wh_ModUninit() {
    InterlockedExchange(&g_unloading, 1);

    // Un-subclass exactly the windows we subclassed (EnumWindows-based cleanup
    // misses any window that isn't top-level), and hand frames back to DWM.
    std::vector<HWND> windows;
    AcquireSRWLockShared(&g_trackLock);
    windows.assign(g_tracked.begin(), g_tracked.end());
    ReleaseSRWLockShared(&g_trackLock);

    bool timedOut = false;
    int removed = 0;
    for (HWND hWnd : windows) {
        if (!IsWindow(hWnd)) { Untrack(hWnd); continue; }
        if (!RunSubclassOp(hWnd, false, &timedOut)) continue;
        removed++;
        DWMNCRENDERINGPOLICY policy = DWMNCRP_USEWINDOWSTYLE;
        DwmSetWindowAttribute(hWnd, DWMWA_NCRENDERING_POLICY, &policy, sizeof(policy));
        SetWindowPos(hWnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED |
                     SWP_ASYNCWINDOWPOS);
    }

    // If any thread is still inside our code, or any window still has our
    // subclass, unloading would crash the app: stay in memory instead (those
    // windows drop the subclass themselves on their next message). Check the
    // in-flight count first: a thread finishes Track() before it leaves.
    bool busy = InterlockedCompareExchange(&g_inFlight, 0, 0) != 0;
    AcquireSRWLockShared(&g_trackLock);
    size_t left = g_tracked.size();
    ReleaseSRWLockShared(&g_trackLock);
    bool pinned = false;
    if (busy || left || timedOut) {
        HMODULE self;
        pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    (LPCWSTR)(void*)&CaptionSubclassProc, &self);
    }
    Wh_Log(L"Unload: removed from %d windows, %d left, busy=%d, timedOut=%d, kept in memory=%d",
           removed, (int)left, busy, timedOut, pinned);
}
