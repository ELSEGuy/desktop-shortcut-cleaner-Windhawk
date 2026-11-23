// ==WindhawkMod==
// @id              desktop-shortcut-cleaner
// @name            Desktop Shortcut Cleaner
// @description     Customize desktop icon labels and hide shortcut arrows on the Windows desktop only, without affecting File Explorer.
// @version         1.1.0
// @author          ELSEGuy
// @github          https://github.com/ELSEGuy
// @license         MIT
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -lshell32 -lshlwapi -lcomctl32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
Desktop Shortcut Cleaner for Windows 11.

Features:
- Hides text only for shortcuts on the Windows desktop.
- Optional toggle to hide the names of every desktop icon.
- Hides the standard shortcut arrow only on the desktop.
- Hides the blue/yellow UAC (admin) shield on desktop shortcuts, and keeps it
  hidden after refreshing the desktop.
- Real folders and normal files keep their names unless "Hide all desktop icon text" is enabled.
- File Explorer remains unchanged.

Default settings:
- Hide desktop shortcut text: ON
- Hide all desktop icon text: OFF
- Hide desktop shortcut arrows: ON
- Hide UAC shield on desktop shortcuts: ON

Note on the UAC shield: Windows bakes the shield into the shortcut's icon
image (it is not a draw-time overlay like the arrow). The mod removes it at
the source for shortcuts stored in your Desktop folder or the Public Desktop
folder. Because of that, those same shortcuts also show no shield if you open
the Desktop folder in File Explorer. Shortcuts anywhere else keep their shield.
Shortcuts still run as administrator; only the icon changes.

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

- hide_shield: true
  $name: Hide UAC shield on desktop shortcuts
  $description: Remove the blue/yellow admin shield from shortcuts in the Desktop and Public Desktop folders. Stays hidden after refreshing the desktop. The shortcut still runs as administrator.
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
    bool hide_shield;
} g_settings;

std::wstring g_userDesktopDir;
std::wstring g_publicDesktopDir;

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
// UAC shield removal
//
// The shield is not a draw-time overlay. When the shell asks a shortcut for
// its icon (IExtractIconW::GetIconLocation), the shortcut returns GIL_SHIELD
// if its target needs elevation, and the shell then composites the shield
// into a separate system-image-list entry. Stripping it in IImageList::Draw
// therefore can't work, and any desktop refresh re-extracts the icon and
// brings the shield back.
//
// Fix it at the source: hook the ShellLink object's GetIconLocation and, for
// shortcuts stored in a desktop folder, replace GIL_SHIELD with
// GIL_FORCENOSHIELD.
//
// IExtractIconW vtable slots:
//   0 QueryInterface
//   1 AddRef
//   2 Release
//   3 GetIconLocation
//   4 Extract
// ---------------------------------------------------------------------------

#ifndef GIL_SHIELD
#define GIL_SHIELD 0x0200
#endif

#ifndef GIL_FORCENOSHIELD
#define GIL_FORCENOSHIELD 0x0400
#endif

static const GUID kClsidShellLink = {
    0x00021401, 0x0000, 0x0000,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};

static const GUID kIidExtractIconW = {
    0x000214FA, 0x0000, 0x0000,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};

static const GUID kIidPersistFile = {
    0x0000010B, 0x0000, 0x0000,
    {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};

static const GUID kFolderIdDesktop = {
    0xB4BFCC3A, 0xDB2C, 0x424C,
    {0xB0, 0x29, 0x7F, 0xE9, 0x9A, 0x87, 0xC6, 0x41}
};

static const GUID kFolderIdPublicDesktop = {
    0xC4AA340D, 0xF20F, 0x4863,
    {0xAF, 0xEF, 0xF8, 0x7E, 0xF2, 0xE6, 0xBA, 0x25}
};

static std::wstring GetKnownFolder(REFKNOWNFOLDERID id) {
    std::wstring result;
    PWSTR path = nullptr;

    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_DEFAULT, nullptr, &path)) &&
        path) {
        result = path;
    }

    CoTaskMemFree(path);
    return result;
}

static void ResolveDesktopFolders() {
    g_userDesktopDir = GetKnownFolder(kFolderIdDesktop);
    g_publicDesktopDir = GetKnownFolder(kFolderIdPublicDesktop);

    Wh_Log(L"Desktop folders: user=%s public=%s",
           g_userDesktopDir.c_str(),
           g_publicDesktopDir.c_str());
}

static bool IsDirectlyInFolder(const std::wstring& filePath,
                               const std::wstring& folder) {
    if (folder.empty() || filePath.size() <= folder.size() + 1) {
        return false;
    }

    if (_wcsnicmp(filePath.c_str(), folder.c_str(), folder.size()) != 0) {
        return false;
    }

    if (filePath[folder.size()] != L'\\') {
        return false;
    }

    // Only items directly inside the folder, not in subfolders.
    return filePath.find(L'\\', folder.size() + 1) == std::wstring::npos;
}

static bool IsDesktopShortcut(IExtractIconW* self) {
    IPersistFile* persistFile = nullptr;

    if (FAILED(self->QueryInterface(
            kIidPersistFile,
            reinterpret_cast<void**>(&persistFile))) ||
        !persistFile) {
        return false;
    }

    bool result = false;
    LPOLESTR fileName = nullptr;

    if (persistFile->GetCurFile(&fileName) == S_OK && fileName) {
        std::wstring path = fileName;
        result = IsDirectlyInFolder(path, g_userDesktopDir) ||
                 IsDirectlyInFolder(path, g_publicDesktopDir);
    }

    CoTaskMemFree(fileName);
    persistFile->Release();

    return result;
}

using ExtractIconW_GetIconLocation_t =
    HRESULT(STDMETHODCALLTYPE*)(IExtractIconW*,
                                UINT,
                                PWSTR,
                                UINT,
                                int*,
                                UINT*);

ExtractIconW_GetIconLocation_t ShellLink_GetIconLocation_Original = nullptr;

HRESULT STDMETHODCALLTYPE ShellLink_GetIconLocation_Hook(
    IExtractIconW* self,
    UINT flags,
    PWSTR iconFile,
    UINT iconFileSize,
    int* index,
    UINT* outFlags) {

    HRESULT hr = ShellLink_GetIconLocation_Original(
        self, flags, iconFile, iconFileSize, index, outFlags);

    if (SUCCEEDED(hr) &&
        g_settings.hide_shield &&
        outFlags &&
        IsDesktopShortcut(self)) {
        // Set GIL_FORCENOSHIELD even when GIL_SHIELD wasn't returned, so
        // the shell doesn't add a shield on its own either.
        *outFlags = (*outFlags & ~GIL_SHIELD) | GIL_FORCENOSHIELD;
    }

    return hr;
}

static ExtractIconW_GetIconLocation_t GetShellLinkGetIconLocationTarget() {
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ExtractIconW_GetIconLocation_t target = nullptr;
    IExtractIconW* extractIcon = nullptr;

    if (SUCCEEDED(CoCreateInstance(
            kClsidShellLink,
            nullptr,
            CLSCTX_INPROC_SERVER,
            kIidExtractIconW,
            reinterpret_cast<void**>(&extractIcon))) &&
        extractIcon) {

        void** vtable = *reinterpret_cast<void***>(extractIcon);
        if (vtable && vtable[3]) {
            target = reinterpret_cast<ExtractIconW_GetIconLocation_t>(
                vtable[3]);
        }

        extractIcon->Release();
    } else {
        Wh_Log(L"Couldn't create a ShellLink IExtractIconW instance");
    }

    if (SUCCEEDED(initHr)) {
        CoUninitialize();
    }

    return target;
}

// Tell the shell to re-extract the icon of every shortcut in the desktop
// folders, so the change applies without a manual refresh.
static void NotifyDesktopShortcutsChanged() {
    for (const std::wstring* folder : {&g_userDesktopDir, &g_publicDesktopDir}) {
        if (folder->empty()) {
            continue;
        }

        std::wstring pattern = *folder + L"\\*.lnk";

        WIN32_FIND_DATAW findData;
        HANDLE find = FindFirstFileW(pattern.c_str(), &findData);
        if (find == INVALID_HANDLE_VALUE) {
            continue;
        }

        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }

            std::wstring path = *folder + L"\\" + findData.cFileName;
            SHChangeNotify(SHCNE_UPDATEITEM,
                           SHCNF_PATHW | SHCNF_FLUSHNOWAIT,
                           path.c_str(),
                           nullptr);
        } while (FindNextFileW(find, &findData));

        FindClose(find);
    }
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

    g_settings.hide_shield =
        Wh_GetIntSetting(L"hide_shield") != 0;
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

    if (g_settings.hide_shield) {
        ResolveDesktopFolders();

        if (ExtractIconW_GetIconLocation_t target =
                GetShellLinkGetIconLocationTarget()) {
            WindhawkUtils::SetFunctionHook(
                target,
                ShellLink_GetIconLocation_Hook,
                &ShellLink_GetIconLocation_Original);
        } else {
            Wh_Log(L"Couldn't resolve ShellLink GetIconLocation target");
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

    if (g_settings.hide_shield) {
        NotifyDesktopShortcutsChanged();
    }
}

void Wh_ModUninit() {
    if (HWND desktop = GetDesktopFolderView()) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(
            desktop,
            DesktopListSubclass);

        InvalidateRect(desktop, nullptr, TRUE);
    }

    if (g_settings.hide_shield) {
        // Let the shields come back when the mod is disabled.
        NotifyDesktopShortcutsChanged();
    }
}

BOOL Wh_ModSettingsChanged(BOOL* reload) {
    *reload = TRUE;
    return TRUE;
}