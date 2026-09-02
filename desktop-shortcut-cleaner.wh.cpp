// ==WindhawkMod==
// @id              desktop-shortcut-cleaner
// @name            Desktop Shortcut Cleaner
// @description     Customize desktop icon labels and hide shortcut arrows on the Windows desktop only, without affecting File Explorer.
// @version         1.0.0
// @author          ELSEGuy
// @github          https://github.com/ELSEGuy
// @license         MIT
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -lshlwapi -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
Desktop Shortcut Cleaner for Windows 11.

Features:
- Hides text only for shortcuts on the Windows desktop.
- Optional toggle to hide the names of every desktop icon.
- Hides the standard shortcut arrow only on the desktop.
- Real folders and normal files keep their names unless "Hide all desktop icon text" is enabled.
- File Explorer remains unchanged.

Default settings:
- Hide desktop shortcut text: ON
- Hide all desktop icon text: OFF
- Hide desktop shortcut arrows: ON

Credits:
Based in part on desktop icon text-hiding code by kivsak.
The desktop detection and icon-label hiding approach were adapted from their
script, which hides the names of all desktop icons.
This project extends that approach with its own shortcut-only label filtering.
The desktop-only shortcut-arrow handling is implemented separately by this
project.
*/
// ==/WindhawkModReadme==

// ==WindhawkModSettings==
/*
- hide_text: true
  $name: Hide desktop shortcut text
  $description: Hide text only for shortcuts on the desktop. Folder and normal-file names stay visible.

- hide_all_text: false
  $name: Hide all desktop icon text
  $description: Hide the names of every icon on the desktop. This overrides the shortcut-only text option.

- hide_arrows: true
  $name: Hide desktop shortcut arrows
  $description: Hide the standard shortcut arrow only while desktop icons are being painted. File Explorer stays unchanged.
*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <string>
#include <unordered_set>

// ---------------------------------------------------------------------------
// Settings / state
// ---------------------------------------------------------------------------

struct {
    bool hide_text;
    bool hide_all_text;
    bool hide_arrows;
} g_settings;

thread_local bool g_isDrawingDesktop = false;

std::unordered_set<std::wstring> g_shortcutNames;
DWORD g_lastShortcutCacheTick = 0;

UINT g_linkOverlayMask = 0;

// ---------------------------------------------------------------------------
// Desktop window helpers
// ---------------------------------------------------------------------------

static HWND FindChild(HWND parent, LPCWSTR cls, LPCWSTR win) {
    if (!parent) {
        return nullptr;
    }

    return FindWindowExW(parent, nullptr, cls, win);
}

static bool IsDesktopFolderView(HWND hWnd) {
    WCHAR buf[64];

    if (!GetClassNameW(hWnd, buf, ARRAYSIZE(buf)) ||
        _wcsicmp(buf, L"SysListView32")) {
        return false;
    }

    if (!GetWindowTextW(hWnd, buf, ARRAYSIZE(buf)) ||
        _wcsicmp(buf, L"FolderView")) {
        return false;
    }

    HWND defView = GetAncestor(hWnd, GA_PARENT);
    if (!defView) {
        return false;
    }

    if (!GetClassNameW(defView, buf, ARRAYSIZE(buf)) ||
        _wcsicmp(buf, L"SHELLDLL_DefView")) {
        return false;
    }

    HWND top = GetAncestor(defView, GA_PARENT);
    if (!top) {
        return false;
    }

    if ((!GetClassNameW(top, buf, ARRAYSIZE(buf)) ||
         _wcsicmp(buf, L"Progman")) &&
        top != GetShellWindow()) {
        return false;
    }

    return true;
}

static HWND GetDesktopFolderView() {
    HWND shell = GetShellWindow();
    if (!shell) {
        return nullptr;
    }

    HWND defView = FindChild(shell, L"SHELLDLL_DefView", nullptr);
    if (!defView) {
        return nullptr;
    }

    HWND listView = FindChild(defView, L"SysListView32", L"FolderView");
    if (!listView) {
        return nullptr;
    }

    DWORD pid = 0;
    if (!GetWindowThreadProcessId(listView, &pid) ||
        pid != GetCurrentProcessId()) {
        return nullptr;
    }

    return listView;
}

// ---------------------------------------------------------------------------
// Shortcut-name cache for text hiding
// ---------------------------------------------------------------------------

static void UpdateShortcutCache() {
    DWORD now = GetTickCount();

    if (g_lastShortcutCacheTick != 0 &&
        now - g_lastShortcutCacheTick < 500) {
        return;
    }

    g_lastShortcutCacheTick = now;

    std::unordered_set<std::wstring> fresh;

    IShellFolder* desktop = nullptr;
    if (FAILED(SHGetDesktopFolder(&desktop)) || !desktop) {
        return;
    }

    IEnumIDList* enumerator = nullptr;
    if (SUCCEEDED(desktop->EnumObjects(
            nullptr,
            SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
            &enumerator)) &&
        enumerator) {

        LPITEMIDLIST pidl = nullptr;

        while (enumerator->Next(1, &pidl, nullptr) == S_OK) {
            SFGAOF attributes = SFGAO_LINK;

           LPCITEMIDLIST pidlConst = pidl;

            if (SUCCEEDED(desktop->GetAttributesOf(
                    1,
                    &pidlConst,
                    &attributes)) &&
                (attributes & SFGAO_LINK)) {

                STRRET str;
                if (SUCCEEDED(desktop->GetDisplayNameOf(
                        pidl,
                        SHGDN_NORMAL,
                        &str))) {

                    WCHAR name[MAX_PATH];
                    if (SUCCEEDED(StrRetToBufW(
                            &str,
                            pidl,
                            name,
                            ARRAYSIZE(name)))) {
                        fresh.insert(name);
                    }
                }
            }

            CoTaskMemFree(pidl);
        }

        enumerator->Release();
    }

    desktop->Release();
    g_shortcutNames = std::move(fresh);
}

static bool ShouldHideLabel(LPCWSTR text, int cch) {
    if (!text) {
        return false;
    }

    if (g_settings.hide_all_text) {
        return true;
    }

    if (!g_settings.hide_text) {
        return false;
    }

    std::wstring value(
        text,
        cch == -1 ? wcslen(text) : static_cast<size_t>(cch));

    return g_shortcutNames.count(value) != 0;
}

// ---------------------------------------------------------------------------
// Standard Windows shortcut-overlay identification
// ---------------------------------------------------------------------------

#ifndef IDO_SHGIOI_LINK
#define IDO_SHGIOI_LINK 0x0FFFFFFE
#endif

static bool ResolveLinkOverlayMask() {
    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32) {
        Wh_Log(L"shell32.dll is not loaded");
        return false;
    }

    using SHGetIconOverlayIndexW_t = int(WINAPI*)(LPCWSTR, int);

    auto getOverlayIndex =
        reinterpret_cast<SHGetIconOverlayIndexW_t>(
            GetProcAddress(shell32, "SHGetIconOverlayIndexW"));

    if (!getOverlayIndex) {
        Wh_Log(L"SHGetIconOverlayIndexW wasn't found");
        return false;
    }

    int overlayIndex = getOverlayIndex(nullptr, IDO_SHGIOI_LINK);
    if (overlayIndex <= 0) {
        Wh_Log(L"Couldn't resolve the standard link overlay index: %d",
               overlayIndex);
        return false;
    }

    g_linkOverlayMask = INDEXTOOVERLAYMASK(overlayIndex);
    Wh_Log(L"Standard link overlay index=%d mask=0x%X",
           overlayIndex,
           g_linkOverlayMask);

    return true;
}

// ---------------------------------------------------------------------------
// IImageList::Draw hook
//
// IImageList derives from IUnknown.
// Vtable slots:
//   0 QueryInterface
//   1 AddRef
//   2 Release
//   3 Add
//   4 ReplaceIcon
//   5 SetOverlayImage
//   6 Replace
//   7 AddMasked
//   8 Draw
// ---------------------------------------------------------------------------

using IImageList_Draw_t =
    HRESULT(STDMETHODCALLTYPE*)(IImageList*, IMAGELISTDRAWPARAMS*);

IImageList_Draw_t IImageList_Draw_Original = nullptr;

HRESULT STDMETHODCALLTYPE IImageList_Draw_Hook(
    IImageList* self,
    IMAGELISTDRAWPARAMS* drawParams) {

    if (g_isDrawingDesktop &&
        g_settings.hide_arrows &&
        g_linkOverlayMask != 0 &&
        drawParams &&
        (drawParams->fStyle & ILD_OVERLAYMASK) == g_linkOverlayMask) {

        IMAGELISTDRAWPARAMS copy = *drawParams;
        copy.fStyle &= ~ILD_OVERLAYMASK;

        return IImageList_Draw_Original(self, &copy);
    }

    return IImageList_Draw_Original(self, drawParams);
}

static IImageList_Draw_t GetDesktopImageListDrawTarget() {
    if (HWND desktop = GetDesktopFolderView()) {
        HIMAGELIST imageListHandle =
            ListView_GetImageList(desktop, LVSIL_NORMAL);

        if (imageListHandle) {
            auto imageList =
                reinterpret_cast<IImageList*>(imageListHandle);

            void** vtable =
                *reinterpret_cast<void***>(imageList);

            if (vtable && vtable[8]) {
                Wh_Log(L"Using the desktop ListView's IImageList::Draw target");
                return reinterpret_cast<IImageList_Draw_t>(vtable[8]);
            }
        }
    }

    HMODULE shell32 = GetModuleHandleW(L"shell32.dll");
    if (!shell32) {
        return nullptr;
    }

    using SHGetImageList_t =
        HRESULT(WINAPI*)(int, REFIID, void**);

    auto getImageList =
        reinterpret_cast<SHGetImageList_t>(
            GetProcAddress(shell32, "SHGetImageList"));

    if (!getImageList) {
        return nullptr;
    }

    static const GUID iidImageList = {
        0x46eb5926,
        0x582e,
        0x4017,
        {0x9f, 0xdf, 0xe8, 0x99, 0x8d, 0xaa, 0x09, 0x50}
    };

    IImageList* imageList = nullptr;

    if (FAILED(getImageList(
            0,
            iidImageList,
            reinterpret_cast<void**>(&imageList))) ||
        !imageList) {
        return nullptr;
    }

    void** vtable = *reinterpret_cast<void***>(imageList);
    IImageList_Draw_t target = nullptr;

    if (vtable && vtable[8]) {
        target = reinterpret_cast<IImageList_Draw_t>(vtable[8]);
    }

    imageList->Release();

    if (target) {
        Wh_Log(L"Using SHGetImageList IImageList::Draw fallback target");
    }

    return target;
}

// ---------------------------------------------------------------------------
// Desktop subclass
// ---------------------------------------------------------------------------

LRESULT CALLBACK DesktopListSubclass(
    HWND hWnd,
    UINT uMsg,
    WPARAM wParam,
    LPARAM lParam,
    DWORD_PTR) {

    if ((uMsg == WM_PAINT || uMsg == WM_PRINTCLIENT) &&
        (g_settings.hide_text ||
         g_settings.hide_all_text ||
         g_settings.hide_arrows)) {

        if (g_settings.hide_text && !g_settings.hide_all_text) {
            UpdateShortcutCache();
        }

        bool previous = g_isDrawingDesktop;
        g_isDrawingDesktop = true;

        LRESULT result =
            DefSubclassProc(hWnd, uMsg, wParam, lParam);

        g_isDrawingDesktop = previous;
        return result;
    }

    return DefSubclassProc(hWnd, uMsg, wParam, lParam);
}

// ---------------------------------------------------------------------------
// Text drawing hooks
// ---------------------------------------------------------------------------

using DrawTextW_t = decltype(&DrawTextW);
DrawTextW_t DrawTextW_Original = nullptr;

int WINAPI DrawTextW_Hook(
    HDC hdc,
    LPCWSTR text,
    int cchText,
    LPRECT rect,
    UINT format) {

    if (g_isDrawingDesktop &&
        text &&
        ShouldHideLabel(text, cchText)) {
        return DrawTextW_Original(hdc, L"", 0, rect, format);
    }

    return DrawTextW_Original(hdc, text, cchText, rect, format);
}

using DrawThemeTextEx_t =
    HRESULT(WINAPI*)(
        HTHEME,
        HDC,
        int,
        int,
        LPCWSTR,
        int,
        DWORD,
        LPRECT,
        const void*);

DrawThemeTextEx_t DrawThemeTextEx_Original = nullptr;

HRESULT WINAPI DrawThemeTextEx_Hook(
    HTHEME theme,
    HDC hdc,
    int partId,
    int stateId,
    LPCWSTR text,
    int cchText,
    DWORD flags,
    LPRECT rect,
    const void* options) {

    if (g_isDrawingDesktop &&
        text &&
        ShouldHideLabel(text, cchText)) {
        return DrawThemeTextEx_Original(
            theme,
            hdc,
            partId,
            stateId,
            L"",
            0,
            flags,
            rect,
            options);
    }

    return DrawThemeTextEx_Original(
        theme,
        hdc,
        partId,
        stateId,
        text,
        cchText,
        flags,
        rect,
        options);
}

// ---------------------------------------------------------------------------
// Catch desktop ListView recreation
// ---------------------------------------------------------------------------

using CreateWindowExW_t = decltype(&CreateWindowExW);
CreateWindowExW_t CreateWindowExW_Original = nullptr;

HWND WINAPI CreateWindowExW_Hook(
    DWORD exStyle,
    LPCWSTR className,
    LPCWSTR windowName,
    DWORD style,
    int x,
    int y,
    int width,
    int height,
    HWND parent,
    HMENU menu,
    HINSTANCE instance,
    PVOID param) {

    HWND hWnd = CreateWindowExW_Original(
        exStyle,
        className,
        windowName,
        style,
        x,
        y,
        width,
        height,
        parent,
        menu,
        instance,
        param);

    if (hWnd && IsDesktopFolderView(hWnd)) {
        WindhawkUtils::SetWindowSubclassFromAnyThread(
            hWnd,
            DesktopListSubclass,
            0);
    }

    return hWnd;
}

// ---------------------------------------------------------------------------
// Settings / lifecycle
// ---------------------------------------------------------------------------

static void LoadSettings() {
    g_settings.hide_text =
        Wh_GetIntSetting(L"hide_text") != 0;

    g_settings.hide_all_text =
        Wh_GetIntSetting(L"hide_all_text") != 0;

    g_settings.hide_arrows =
        Wh_GetIntSetting(L"hide_arrows") != 0;
}

BOOL Wh_ModInit() {
    LoadSettings();

    WindhawkUtils::SetFunctionHook(
        CreateWindowExW,
        CreateWindowExW_Hook,
        &CreateWindowExW_Original);

    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto proc =
                reinterpret_cast<DrawTextW_t>(
                    GetProcAddress(user32, "DrawTextW"))) {
            WindhawkUtils::SetFunctionHook(
                proc,
                DrawTextW_Hook,
                &DrawTextW_Original);
        }
    }

    if (HMODULE uxTheme = GetModuleHandleW(L"uxtheme.dll")) {
        if (auto proc =
                reinterpret_cast<DrawThemeTextEx_t>(
                    GetProcAddress(uxTheme, "DrawThemeTextEx"))) {
            WindhawkUtils::SetFunctionHook(
                proc,
                DrawThemeTextEx_Hook,
                &DrawThemeTextEx_Original);
        }
    }

    if (g_settings.hide_arrows) {
        ResolveLinkOverlayMask();

        if (IImageList_Draw_t drawTarget =
                GetDesktopImageListDrawTarget()) {
            WindhawkUtils::SetFunctionHook(
                drawTarget,
                IImageList_Draw_Hook,
                &IImageList_Draw_Original);
        } else {
            Wh_Log(L"Couldn't resolve IImageList::Draw target");
        }
    }

    return TRUE;
}

void Wh_ModAfterInit() {
    if (HWND desktop = GetDesktopFolderView()) {
        WindhawkUtils::SetWindowSubclassFromAnyThread(
            desktop,
            DesktopListSubclass,
            0);

        InvalidateRect(desktop, nullptr, TRUE);
        UpdateWindow(desktop);
    }
}

void Wh_ModUninit() {
    if (HWND desktop = GetDesktopFolderView()) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(
            desktop,
            DesktopListSubclass);

        InvalidateRect(desktop, nullptr, TRUE);
    }
}

BOOL Wh_ModSettingsChanged(BOOL* reload) {
    *reload = TRUE;
    return TRUE;
}