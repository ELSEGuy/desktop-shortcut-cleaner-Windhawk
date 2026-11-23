// ==WindhawkMod==
// @id              desktop-shortcut-cleaner
// @name            Desktop Shortcut Cleaner
// @description     Customize desktop icon labels, shortcut arrows, and administrator shields on the Windows desktop without changing File Explorer.
// @version         1.1.2
// @author          ELSEGuy
// @github          https://github.com/ELSEGuy
// @license         MIT
// @include         explorer.exe
// @architecture    x86-64
// @compilerOptions -lole32 -lshlwapi -lcomctl32 -lshell32 -ladvapi32 -luser32
// ==/WindhawkMod==

// ==WindhawkModReadme==
/*
Desktop Shortcut Cleaner for Windows 11.

Features:
- Hides text only for shortcuts on the Windows desktop.
- Optional toggle to hide the names of every desktop icon.
- Hides the standard shortcut arrow only on the desktop.
- Hides the UAC (admin) shield on desktop shortcuts whose target uses the
  "Run this program as an administrator" compatibility setting, by drawing
  the raw icon instead. Stays hidden after refreshing the desktop.
- File Explorer remains unchanged.

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

- hide_admin_shields: true
  $name: Hide desktop administrator shields
  $description: On the desktop only, draw the raw icon for shortcuts whose target uses the "Run as administrator" compatibility setting. The shortcut still runs as administrator.

*/
// ==/WindhawkModSettings==

#include <windhawk_utils.h>

#include <windows.h>
#include <commctrl.h>
#include <commoncontrols.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <uxtheme.h>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// ---------------------------------------------------------------------------
// Settings / state
// ---------------------------------------------------------------------------

struct {
    bool hide_text;
    bool hide_all_text;
    bool hide_arrows;
    bool hide_shields;
} g_settings;

thread_local bool g_isDrawingDesktop = false;

std::unordered_set<std::wstring> g_shortcutNames;
DWORD g_lastShortcutCacheTick = 0;

UINT g_linkOverlayMask = 0;
int g_linkOverlaySlot = 0;

HIMAGELIST g_cleanShieldImageList = nullptr;
std::unordered_map<int, int> g_cleanIconBySystemImage;
DWORD g_lastShieldCacheTick = 0;
HIMAGELIST g_cachedDesktopSystemImageList = nullptr;
int g_cachedDesktopIconWidth = 0;
int g_cachedDesktopIconHeight = 0;
int g_cachedDesktopItemCount = -1;
// System-image index of every desktop item when the map was last built.
// Explorer assigns real icon indexes asynchronously after a refresh, so the
// map must be rebuilt whenever any of these change, not only the item count.
std::vector<int> g_cachedDesktopImageSignature;

// Timer used to retry a shield-map rebuild that was throttled, so the map is
// corrected even if nothing repaints the desktop afterwards.
constexpr UINT_PTR kShieldRefreshTimerId = 0x57484B53;
constexpr UINT kShieldRefreshThrottleMs = 250;
constexpr UINT kShieldRefreshRetryMs = 300;

SRWLOCK g_cleanShieldCacheLock = SRWLOCK_INIT;
volatile LONG g_shieldCacheUpdateInProgress = 0;
volatile LONG g_shieldCacheGeneration = 0;
volatile LONG g_isUnloading = 0;

thread_local bool g_isDrawingCleanShieldIcon = false;

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

                STRRET str = {};
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
            pidl = nullptr;
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
    g_linkOverlaySlot = overlayIndex;
    Wh_Log(L"Standard link overlay index=%d mask=0x%X",
           overlayIndex,
           g_linkOverlayMask);

    return true;
}

// ---------------------------------------------------------------------------
// Raw-icon cache for RUNASADMIN desktop shortcuts
//
// Explorer's UAC shield is already present in the cached system-image-list
// bitmap for this compatibility case. Instead of modifying that shared cache,
// this experiment extracts the shortcut's raw icon and substitutes it only
// while the desktop ListView is painting.
// ---------------------------------------------------------------------------

static const GUID kCLSID_ShellLink = {
    0x00021401,
    0x0000,
    0x0000,
    {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};

static const GUID kIID_IShellLinkW = {
    0x000214f9,
    0x0000,
    0x0000,
    {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};

static const GUID kIID_IPersistFile = {
    0x0000010b,
    0x0000,
    0x0000,
    {0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}
};

static std::wstring ExpandPath(const WCHAR* path) {
    if (!path || !path[0]) {
        return {};
    }

    WCHAR expanded[32768];
    DWORD length = ExpandEnvironmentStringsW(
        path,
        expanded,
        ARRAYSIZE(expanded));

    if (length > 0 && length <= ARRAYSIZE(expanded)) {
        return expanded;
    }

    return path;
}

static bool RegistryLayerContainsRunAsAdmin(
    HKEY root,
    REGSAM view,
    const std::wstring& targetPath) {

    static constexpr WCHAR kLayersKey[] =
        L"Software\\Microsoft\\Windows NT\\CurrentVersion\\"
        L"AppCompatFlags\\Layers";

    HKEY key = nullptr;
    if (RegOpenKeyExW(
            root,
            kLayersKey,
            0,
            KEY_QUERY_VALUE | view,
            &key) != ERROR_SUCCESS) {
        return false;
    }

    WCHAR data[2048] = {};
    DWORD type = 0;
    DWORD bytes = sizeof(data);

    LSTATUS status = RegQueryValueExW(
        key,
        targetPath.c_str(),
        nullptr,
        &type,
        reinterpret_cast<BYTE*>(data),
        &bytes);

    RegCloseKey(key);

    if (status != ERROR_SUCCESS ||
        (type != REG_SZ && type != REG_EXPAND_SZ)) {
        return false;
    }

    data[ARRAYSIZE(data) - 1] = L'\0';
    return StrStrIW(data, L"RUNASADMIN") != nullptr;
}

static bool TargetUsesRunAsAdminLayer(const std::wstring& targetPath) {
    if (targetPath.empty()) {
        return false;
    }

    if (RegistryLayerContainsRunAsAdmin(
            HKEY_CURRENT_USER,
            0,
            targetPath) ||
        RegistryLayerContainsRunAsAdmin(
            HKEY_LOCAL_MACHINE,
            0,
            targetPath)) {
        return true;
    }

#ifdef _WIN64
    if (RegistryLayerContainsRunAsAdmin(
            HKEY_CURRENT_USER,
            KEY_WOW64_32KEY,
            targetPath) ||
        RegistryLayerContainsRunAsAdmin(
            HKEY_LOCAL_MACHINE,
            KEY_WOW64_32KEY,
            targetPath)) {
        return true;
    }
#endif

    return false;
}

static bool ReadShortcutIconInfo(
    const std::wstring& shortcutPath,
    std::wstring* targetPath,
    std::wstring* iconPath,
    int* iconIndex) {

    IShellLinkW* shellLink = nullptr;
    HRESULT result = CoCreateInstance(
        kCLSID_ShellLink,
        nullptr,
        CLSCTX_INPROC_SERVER,
        kIID_IShellLinkW,
        reinterpret_cast<void**>(&shellLink));

    if (FAILED(result) || !shellLink) {
        return false;
    }

    IPersistFile* persistFile = nullptr;
    result = shellLink->QueryInterface(
        kIID_IPersistFile,
        reinterpret_cast<void**>(&persistFile));

    if (FAILED(result) || !persistFile) {
        shellLink->Release();
        return false;
    }

    result = persistFile->Load(shortcutPath.c_str(), STGM_READ);
    persistFile->Release();

    if (FAILED(result)) {
        shellLink->Release();
        return false;
    }

    WCHAR target[MAX_PATH] = {};
    result = shellLink->GetPath(
        target,
        ARRAYSIZE(target),
        nullptr,
        SLGP_RAWPATH);

    if (FAILED(result) || !target[0]) {
        shellLink->Release();
        return false;
    }

    WCHAR icon[MAX_PATH] = {};
    int index = 0;
    if (FAILED(shellLink->GetIconLocation(
            icon,
            ARRAYSIZE(icon),
            &index))) {
        icon[0] = L'\0';
        index = 0;
    }

    shellLink->Release();

    *targetPath = ExpandPath(target);
    *iconPath = icon[0] ? ExpandPath(icon) : *targetPath;
    *iconIndex = icon[0] ? index : 0;

    if (PathIsRelativeW(iconPath->c_str())) {
        WCHAR shortcutDirectory[MAX_PATH];
        wcsncpy_s(
            shortcutDirectory,
            ARRAYSIZE(shortcutDirectory),
            shortcutPath.c_str(),
            _TRUNCATE);
        PathRemoveFileSpecW(shortcutDirectory);

        WCHAR combined[MAX_PATH];
        if (PathCombineW(
                combined,
                shortcutDirectory,
                iconPath->c_str())) {
            *iconPath = combined;
        }
    }

    return true;
}

static bool ShellDisplayNameToString(
    IShellFolder* folder,
    LPCITEMIDLIST pidl,
    SHGDNF flags,
    std::wstring* value) {

    STRRET str = {};
    if (FAILED(folder->GetDisplayNameOf(pidl, flags, &str))) {
        return false;
    }

    WCHAR buffer[32768];
    if (FAILED(StrRetToBufW(
            &str,
            pidl,
            buffer,
            ARRAYSIZE(buffer)))) {
        return false;
    }

    *value = buffer;
    return true;
}

static HICON ExtractRawShortcutIcon(
    const std::wstring& iconPath,
    int iconIndex,
    const std::wstring& targetPath,
    int width,
    int height) {

    HICON icon = nullptr;
    UINT resourceId = 0;

    UINT extracted = PrivateExtractIconsW(
        iconPath.c_str(),
        iconIndex,
        width,
        height,
        &icon,
        &resourceId,
        1,
        LR_DEFAULTCOLOR);

    if (extracted == 1 && icon) {
        return icon;
    }

    if (_wcsicmp(iconPath.c_str(), targetPath.c_str()) != 0) {
        icon = nullptr;
        extracted = PrivateExtractIconsW(
            targetPath.c_str(),
            0,
            width,
            height,
            &icon,
            &resourceId,
            1,
            LR_DEFAULTCOLOR);

        if (extracted == 1 && icon) {
            return icon;
        }
    }

    return nullptr;
}

class ScopedShieldCacheUpdate {
public:
    ScopedShieldCacheUpdate()
        : m_acquired(InterlockedCompareExchange(
              &g_shieldCacheUpdateInProgress,
              1,
              0) == 0) {
    }

    ~ScopedShieldCacheUpdate() {
        if (m_acquired) {
            InterlockedExchange(&g_shieldCacheUpdateInProgress, 0);
        }
    }

    bool IsAcquired() const {
        return m_acquired;
    }

private:
    bool m_acquired;
};

static void ClearShieldedShortcutIconCache() {
    InterlockedIncrement(&g_shieldCacheGeneration);

    AcquireSRWLockExclusive(&g_cleanShieldCacheLock);

    HIMAGELIST oldImageList = g_cleanShieldImageList;
    g_cleanShieldImageList = nullptr;
    g_cleanIconBySystemImage.clear();
    g_lastShieldCacheTick = 0;
    g_cachedDesktopSystemImageList = nullptr;
    g_cachedDesktopIconWidth = 0;
    g_cachedDesktopIconHeight = 0;
    g_cachedDesktopItemCount = -1;
    g_cachedDesktopImageSignature.clear();

    ReleaseSRWLockExclusive(&g_cleanShieldCacheLock);

    if (oldImageList) {
        ImageList_Destroy(oldImageList);
    }
}

static std::vector<int> GetDesktopImageSignature(HWND desktopView) {
    int itemCount = ListView_GetItemCount(desktopView);

    std::vector<int> signature;
    signature.reserve(itemCount > 0 ? itemCount : 0);

    for (int itemIndex = 0; itemIndex < itemCount; itemIndex++) {
        LVITEMW item = {};
        item.mask = LVIF_IMAGE;
        item.iItem = itemIndex;

        if (!SendMessageW(desktopView,
                          LVM_GETITEMW,
                          0,
                          reinterpret_cast<LPARAM>(&item))) {
            item.iImage = -1;
        }

        signature.push_back(item.iImage);
    }

    return signature;
}

static bool ShieldIconCacheNeedsRefresh(HWND desktopView) {
    HIMAGELIST currentImageList =
        ListView_GetImageList(desktopView, LVSIL_NORMAL);
    int currentWidth = 0;
    int currentHeight = 0;

    if (!currentImageList ||
        !ImageList_GetIconSize(
            currentImageList,
            &currentWidth,
            &currentHeight)) {
        return true;
    }

    int currentItemCount = ListView_GetItemCount(desktopView);

    AcquireSRWLockShared(&g_cleanShieldCacheLock);
    bool needsRefresh =
        !g_cleanShieldImageList ||
        currentImageList != g_cachedDesktopSystemImageList ||
        currentWidth != g_cachedDesktopIconWidth ||
        currentHeight != g_cachedDesktopIconHeight ||
        currentItemCount != g_cachedDesktopItemCount;
    ReleaseSRWLockShared(&g_cleanShieldCacheLock);

    if (needsRefresh) {
        return true;
    }

    std::vector<int> signature = GetDesktopImageSignature(desktopView);

    AcquireSRWLockShared(&g_cleanShieldCacheLock);
    needsRefresh = signature != g_cachedDesktopImageSignature;
    ReleaseSRWLockShared(&g_cleanShieldCacheLock);

    return needsRefresh;
}

// Returns false if the rebuild was skipped (throttled or already running),
// so the caller can retry later.
static bool UpdateShieldedShortcutIconCache(bool force = false) {
    if (InterlockedCompareExchange(&g_isUnloading, 0, 0) != 0) {
        return true;
    }

    ScopedShieldCacheUpdate updateGuard;
    if (!updateGuard.IsAcquired()) {
        return false;
    }

    DWORD now = GetTickCount();

    AcquireSRWLockShared(&g_cleanShieldCacheLock);
    DWORD lastUpdate = g_lastShieldCacheTick;
    ReleaseSRWLockShared(&g_cleanShieldCacheLock);

    if (!force &&
        lastUpdate != 0 &&
        now - lastUpdate < kShieldRefreshThrottleMs) {
        return false;
    }

    LONG cacheGeneration =
        InterlockedCompareExchange(&g_shieldCacheGeneration, 0, 0);

    AcquireSRWLockExclusive(&g_cleanShieldCacheLock);
    g_lastShieldCacheTick = now;
    ReleaseSRWLockExclusive(&g_cleanShieldCacheLock);

    HWND desktopView = GetDesktopFolderView();
    if (!desktopView) {
        return true;
    }

    HIMAGELIST systemImageList =
        ListView_GetImageList(desktopView, LVSIL_NORMAL);

    int iconWidth = 0;
    int iconHeight = 0;
    if (!systemImageList ||
        !ImageList_GetIconSize(
            systemImageList,
            &iconWidth,
            &iconHeight) ||
        iconWidth <= 0 ||
        iconHeight <= 0) {
        Wh_Log(L"Couldn't determine the desktop icon size");
        return true;
    }

    HIMAGELIST freshImageList = ImageList_Create(
        iconWidth,
        iconHeight,
        ILC_COLOR32 | ILC_MASK,
        1,
        4);

    if (!freshImageList) {
        Wh_Log(L"Couldn't create the raw desktop icon list");
        return true;
    }

    // Preserve any normal overlay requested by the desktop draw call. The
    // shortcut arrow is deliberately omitted when that setting is enabled.
    auto systemImageListInterface =
        reinterpret_cast<IImageList*>(systemImageList);

    for (int overlaySlot = 1; overlaySlot <= 15; overlaySlot++) {
        if (g_settings.hide_arrows &&
            overlaySlot == g_linkOverlaySlot) {
            continue;
        }

        int overlayImageIndex = -1;
        if (FAILED(systemImageListInterface->GetOverlayImage(
                overlaySlot,
                &overlayImageIndex)) ||
            overlayImageIndex < 0) {
            continue;
        }

        HICON overlayIcon = ImageList_GetIcon(
            systemImageList,
            overlayImageIndex,
            ILD_TRANSPARENT);

        if (!overlayIcon) {
            continue;
        }

        int privateOverlayIndex = ImageList_AddIcon(
            freshImageList,
            overlayIcon);
        DestroyIcon(overlayIcon);

        if (privateOverlayIndex >= 0) {
            ImageList_SetOverlayImage(
                freshImageList,
                privateOverlayIndex,
                overlaySlot);
        }
    }

    std::unordered_map<std::wstring, int> cleanIndexByName;

    IShellFolder* desktopFolder = nullptr;
    if (SUCCEEDED(SHGetDesktopFolder(&desktopFolder)) &&
        desktopFolder) {

        IEnumIDList* enumerator = nullptr;
        if (SUCCEEDED(desktopFolder->EnumObjects(
                nullptr,
                SHCONTF_FOLDERS | SHCONTF_NONFOLDERS,
                &enumerator)) &&
            enumerator) {

            LPITEMIDLIST pidl = nullptr;
            while (enumerator->Next(1, &pidl, nullptr) == S_OK) {
                SFGAOF attributes = SFGAO_LINK;
                LPCITEMIDLIST pidlConst = pidl;

                bool isLink =
                    SUCCEEDED(desktopFolder->GetAttributesOf(
                        1,
                        &pidlConst,
                        &attributes)) &&
                    (attributes & SFGAO_LINK);

                if (isLink) {
                    std::wstring displayName;
                    std::wstring shortcutPath;

                    if (ShellDisplayNameToString(
                            desktopFolder,
                            pidl,
                            SHGDN_NORMAL,
                            &displayName) &&
                        ShellDisplayNameToString(
                            desktopFolder,
                            pidl,
                            SHGDN_FORPARSING,
                            &shortcutPath)) {

                        std::wstring targetPath;
                        std::wstring iconPath;
                        int iconIndex = 0;

                        if (ReadShortcutIconInfo(
                                shortcutPath,
                                &targetPath,
                                &iconPath,
                                &iconIndex) &&
                            TargetUsesRunAsAdminLayer(targetPath)) {

                            Wh_Log(
                                L"RUNASADMIN desktop shortcut: "
                                L"item=\"%ls\" target=\"%ls\" "
                                L"icon=\"%ls\" index=%d",
                                displayName.c_str(),
                                targetPath.c_str(),
                                iconPath.c_str(),
                                iconIndex);

                            HICON rawIcon = ExtractRawShortcutIcon(
                                iconPath,
                                iconIndex,
                                targetPath,
                                iconWidth,
                                iconHeight);

                            if (rawIcon) {
                                int cleanIndex =
                                    ImageList_AddIcon(
                                        freshImageList,
                                        rawIcon);
                                DestroyIcon(rawIcon);

                                if (cleanIndex >= 0) {
                                    cleanIndexByName[displayName] =
                                        cleanIndex;
                                }
                            } else {
                                Wh_Log(
                                    L"Couldn't extract the raw icon for "
                                    L"item=\"%ls\"",
                                    displayName.c_str());
                            }
                        }
                    }
                }

                CoTaskMemFree(pidl);
                pidl = nullptr;
            }

            enumerator->Release();
        }

        desktopFolder->Release();
    }

    std::unordered_map<int, int> freshBySystemImage;
    std::vector<int> freshSignature;
    int itemCount = ListView_GetItemCount(desktopView);
    freshSignature.reserve(itemCount > 0 ? itemCount : 0);

    for (int itemIndex = 0; itemIndex < itemCount; itemIndex++) {
        WCHAR itemName[MAX_PATH] = {};
        LVITEMW textItem = {};
        textItem.iSubItem = 0;
        textItem.pszText = itemName;
        textItem.cchTextMax = ARRAYSIZE(itemName);

        SendMessageW(
            desktopView,
            LVM_GETITEMTEXTW,
            itemIndex,
            reinterpret_cast<LPARAM>(&textItem));

        LVITEMW item = {};
        item.mask = LVIF_IMAGE;
        item.iItem = itemIndex;
        item.iSubItem = 0;

        if (!SendMessageW(
                desktopView,
                LVM_GETITEMW,
                0,
                reinterpret_cast<LPARAM>(&item))) {
            item.iImage = -1;
        }

        freshSignature.push_back(item.iImage);

        auto cleanIt = cleanIndexByName.find(itemName);
        if (cleanIt == cleanIndexByName.end()) {
            continue;
        }

        if (item.iImage >= 0) {
            freshBySystemImage[item.iImage] = cleanIt->second;

            Wh_Log(
                L"Mapped desktop system image=%d to raw image=%d "
                L"for item=\"%ls\"",
                item.iImage,
                cleanIt->second,
                itemName);
        }
    }

    AcquireSRWLockExclusive(&g_cleanShieldCacheLock);

    if (InterlockedCompareExchange(&g_isUnloading, 0, 0) != 0 ||
        cacheGeneration != InterlockedCompareExchange(
            &g_shieldCacheGeneration,
            0,
            0)) {
        ReleaseSRWLockExclusive(&g_cleanShieldCacheLock);
        ImageList_Destroy(freshImageList);
        return true;
    }

    HIMAGELIST oldImageList = g_cleanShieldImageList;
    g_cleanShieldImageList = freshImageList;
    g_cleanIconBySystemImage = std::move(freshBySystemImage);
    g_cachedDesktopSystemImageList = systemImageList;
    g_cachedDesktopIconWidth = iconWidth;
    g_cachedDesktopIconHeight = iconHeight;
    g_cachedDesktopItemCount = itemCount;
    g_cachedDesktopImageSignature = std::move(freshSignature);
    UINT replacementCount =
        static_cast<UINT>(g_cleanIconBySystemImage.size());

    ReleaseSRWLockExclusive(&g_cleanShieldCacheLock);

    if (oldImageList) {
        ImageList_Destroy(oldImageList);
    }

    Wh_Log(
        L"Raw shield-icon cache ready: replacements=%u size=%dx%d",
        replacementCount,
        iconWidth,
        iconHeight);

    return true;
}

// ---------------------------------------------------------------------------
// IImageList::Draw hook for the already-working desktop shortcut-arrow feature
// ---------------------------------------------------------------------------

using IImageList_Draw_t =
    HRESULT(STDMETHODCALLTYPE*)(IImageList*, IMAGELISTDRAWPARAMS*);

IImageList_Draw_t IImageList_Draw_Original = nullptr;

HRESULT STDMETHODCALLTYPE IImageList_Draw_Hook(
    IImageList* self,
    IMAGELISTDRAWPARAMS* drawParams) {

    if (g_isDrawingDesktop &&
        g_settings.hide_shields &&
        !g_isDrawingCleanShieldIcon &&
        drawParams) {

        AcquireSRWLockShared(&g_cleanShieldCacheLock);

        auto cleanIt = g_cleanIconBySystemImage.find(drawParams->i);

        bool isDesktopSystemImageList =
            self == reinterpret_cast<IImageList*>(
                        g_cachedDesktopSystemImageList) ||
            drawParams->himl == g_cachedDesktopSystemImageList;

        if (g_cleanShieldImageList &&
            isDesktopSystemImageList &&
            cleanIt != g_cleanIconBySystemImage.end()) {
            IMAGELISTDRAWPARAMS copy = *drawParams;
            copy.himl = g_cleanShieldImageList;
            copy.i = cleanIt->second;

            if (g_settings.hide_arrows &&
                g_linkOverlayMask != 0 &&
                (copy.fStyle & ILD_OVERLAYMASK) ==
                    g_linkOverlayMask) {
                copy.fStyle &= ~ILD_OVERLAYMASK;
            }

            // ImageList_DrawIndirect dispatches to the implementation that
            // owns the private image list. Calling the system-image-list
            // trampoline with this private object is invalid and was the
            // cause of the previous Explorer crash loop.
            g_isDrawingCleanShieldIcon = true;
            BOOL drawn = ImageList_DrawIndirect(&copy);
            g_isDrawingCleanShieldIcon = false;

            ReleaseSRWLockShared(&g_cleanShieldCacheLock);
            return drawn ? S_OK : E_FAIL;
        }

        ReleaseSRWLockShared(&g_cleanShieldCacheLock);
    }

    if (g_isDrawingDesktop &&
        g_settings.hide_arrows &&
        g_linkOverlayMask != 0 &&
        drawParams &&
        (drawParams->fStyle & ILD_OVERLAYMASK) ==
            g_linkOverlayMask) {

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
                Wh_Log(
                    L"Using the desktop ListView's IImageList::Draw target");
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
         g_settings.hide_arrows ||
         g_settings.hide_shields)) {

        if (g_settings.hide_text && !g_settings.hide_all_text) {
            UpdateShortcutCache();
        }

        if (g_settings.hide_shields &&
            ShieldIconCacheNeedsRefresh(hWnd)) {
            UpdateShieldedShortcutIconCache();

            // Check again shortly whether or not the rebuild ran: it may have
            // been throttled, or Explorer may still be swapping placeholder
            // icons for real ones. Without this, a stale map would only be
            // fixed by the next repaint (e.g. hovering an icon).
            SetTimer(hWnd, kShieldRefreshTimerId, kShieldRefreshRetryMs,
                     nullptr);
        }

        bool previous = g_isDrawingDesktop;
        g_isDrawingDesktop = true;

        LRESULT result =
            DefSubclassProc(hWnd, uMsg, wParam, lParam);

        g_isDrawingDesktop = previous;
        return result;
    }

    if (uMsg == WM_TIMER && wParam == kShieldRefreshTimerId) {
        KillTimer(hWnd, kShieldRefreshTimerId);

        if (g_settings.hide_shields &&
            InterlockedCompareExchange(&g_isUnloading, 0, 0) == 0 &&
            ShieldIconCacheNeedsRefresh(hWnd)) {
            if (UpdateShieldedShortcutIconCache()) {
                InvalidateRect(hWnd, nullptr, FALSE);
            } else {
                SetTimer(hWnd, kShieldRefreshTimerId, kShieldRefreshRetryMs,
                         nullptr);
            }
        }

        return 0;
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

using DrawThemeTextEx_t = HRESULT(WINAPI*)(
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
        ClearShieldedShortcutIconCache();

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

    g_settings.hide_shields =
        Wh_GetIntSetting(L"hide_admin_shields") != 0;
}

BOOL Wh_ModInit() {
    InterlockedExchange(&g_isUnloading, 0);
    LoadSettings();

    WindhawkUtils::SetFunctionHook(
        CreateWindowExW,
        CreateWindowExW_Hook,
        &CreateWindowExW_Original);

    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        if (auto proc = reinterpret_cast<DrawTextW_t>(
                GetProcAddress(user32, "DrawTextW"))) {
            WindhawkUtils::SetFunctionHook(
                proc,
                DrawTextW_Hook,
                &DrawTextW_Original);
        }
    }

    if (HMODULE uxTheme = GetModuleHandleW(L"uxtheme.dll")) {
        if (auto proc = reinterpret_cast<DrawThemeTextEx_t>(
                GetProcAddress(uxTheme, "DrawThemeTextEx"))) {
            WindhawkUtils::SetFunctionHook(
                proc,
                DrawThemeTextEx_Hook,
                &DrawThemeTextEx_Original);
        }
    }

    if (g_settings.hide_arrows ||
        g_settings.hide_shields) {
        ResolveLinkOverlayMask();
    }

    if (g_settings.hide_arrows ||
        g_settings.hide_shields) {
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

        if (g_settings.hide_shields) {
            UpdateShieldedShortcutIconCache(true);
        }

        InvalidateRect(desktop, nullptr, TRUE);
        UpdateWindow(desktop);
    }

}

void Wh_ModUninit() {
    InterlockedExchange(&g_isUnloading, 1);

    if (HWND desktop = GetDesktopFolderView()) {
        WindhawkUtils::RemoveWindowSubclassFromAnyThread(
            desktop,
            DesktopListSubclass);

        InvalidateRect(desktop, nullptr, TRUE);
    }

    ClearShieldedShortcutIconCache();
}

BOOL Wh_ModSettingsChanged(BOOL* reload) {
    *reload = TRUE;
    return TRUE;
}
