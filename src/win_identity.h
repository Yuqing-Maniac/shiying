#ifndef SHIYING_WIN_IDENTITY_H
#define SHIYING_WIN_IDENTITY_H

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <propsys.h>
#include <wchar.h>

#define SY_WIN_APP_ID L"YuqingManiac.Shiying"
#define SY_WIN_TITLE L"映刻录屏"
#define SY_WIN_EXE SY_WIN_TITLE L".exe"

static const struct { const wchar_t *title, *exe; } sy_legacy_entries[] = {
    {L"寸光录屏", L"寸光录屏.exe"},
    {L"拾影录屏", L"拾影录屏.exe"},
};
#define SY_WIN_RESOURCE_TITLE 201

enum { SY_SHELL_COMMAND = 2, SY_SHELL_ICON = 3, SY_SHELL_NAME = 4,
       SY_SHELL_ID = 5, SY_SHELL_PREVENT_PIN = 9 };

static IPropertyStore *sy_window_properties;

static PROPERTYKEY sy_shell_key(DWORD id) {
    PROPERTYKEY key = {{0x9f4c2855, 0x9f79, 0x4b39, {0xa8, 0xd0, 0xe1, 0xd4, 0x2d, 0xe1, 0xd5, 0xf3}}, id};
    return key;
}

static HRESULT sy_set_shell_string(IPropertyStore *store, DWORD id, const wchar_t *text) {
    PROPERTYKEY key = sy_shell_key(id);
    PROPVARIANT value = {0};
    if (text) {
        value.vt = VT_LPWSTR;
        value.pwszVal = (wchar_t *)text;
    }
    return store->lpVtbl->SetValue(store, &key, &value);
}

static int sy_legacy_exe(const wchar_t *name) {
    for (size_t i = 0; i < ARRAYSIZE(sy_legacy_entries); i++)
        if (!_wcsicmp(name, sy_legacy_entries[i].exe)) return 1;
    return 0;
}

static int sy_module_path(wchar_t *exe, size_t cap) {
    DWORD len = GetModuleFileNameW(NULL, exe, (DWORD)cap);
    return len && len < cap && !wcschr(exe, L'"');
}

static int sy_win_entry(wchar_t *exe, size_t cap) {
    if (!sy_module_path(exe, cap)) return 0;
    wchar_t *name = wcsrchr(exe, L'\\');
    if (!name) return 0;
    name++;
    int legacy = sy_legacy_exe(name);
    if (!legacy && _wcsicmp(name, SY_WIN_EXE)) return 0;
    wchar_t temp[MAX_PATH + 1];
    DWORD temp_len = GetTempPathW(ARRAYSIZE(temp), temp);
    if (temp_len && temp_len < ARRAYSIZE(temp) && !_wcsnicmp(exe, temp, temp_len)) return 0;
    for (const wchar_t *part = exe; *part; part++)
        if (*part == L'\\' && !_wcsnicmp(part, L"\\_work\\", 7)) return 0;
    if (!legacy) return 1;

    if ((size_t)(name - exe) + ARRAYSIZE(SY_WIN_EXE) > cap) return 0;
    wcscpy(name, SY_WIN_EXE);
    DWORD attributes = GetFileAttributesW(exe);
    return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

static int sy_win_launch_canonical(void) {
    wchar_t actual[1024], canonical[1024];
    if (!sy_module_path(actual, ARRAYSIZE(actual)) ||
        !sy_win_entry(canonical, ARRAYSIZE(canonical)) ||
        !_wcsicmp(actual, canonical)) return 0;
    return (INT_PTR)ShellExecuteW(NULL, L"open", canonical, NULL, NULL, SW_SHOWNORMAL) > 32;
}

static void sy_win_identity_init(void) {
    SetCurrentProcessExplicitAppUserModelID(SY_WIN_APP_ID);
}

static void sy_win_identity_set(HWND window) {
    if (FAILED(SHGetPropertyStoreForWindow(window, &IID_IPropertyStore, (void **)&sy_window_properties))) return;
    IPropertyStore *store = sy_window_properties;
    wchar_t exe[1024], command[1030], display[1040], icon[1040];
    if (sy_win_entry(exe, sizeof exe / sizeof exe[0])) {
        swprintf(command, sizeof command / sizeof command[0], L"\"%ls\"", exe);
        swprintf(display, sizeof display / sizeof display[0], L"@%ls,-%d", exe, SY_WIN_RESOURCE_TITLE);
        swprintf(icon, sizeof icon / sizeof icon[0], L"%ls,-1", exe);
        sy_set_shell_string(store, SY_SHELL_COMMAND, command);
        sy_set_shell_string(store, SY_SHELL_NAME, display);
        sy_set_shell_string(store, SY_SHELL_ICON, icon);
    } else {
        PROPERTYKEY key = sy_shell_key(SY_SHELL_PREVENT_PIN);
        PROPVARIANT value = {0};
        value.vt = VT_BOOL;
        value.boolVal = VARIANT_TRUE;
        store->lpVtbl->SetValue(store, &key, &value);
    }
    sy_set_shell_string(store, SY_SHELL_ID, SY_WIN_APP_ID);
}

static void sy_win_identity_clear(void) {
    if (!sy_window_properties) return;
    sy_set_shell_string(sy_window_properties, SY_SHELL_ID, NULL);
    sy_set_shell_string(sy_window_properties, SY_SHELL_COMMAND, NULL);
    sy_set_shell_string(sy_window_properties, SY_SHELL_NAME, NULL);
    sy_set_shell_string(sy_window_properties, SY_SHELL_ICON, NULL);
    sy_set_shell_string(sy_window_properties, SY_SHELL_PREVENT_PIN, NULL);
    sy_window_properties->lpVtbl->Release(sy_window_properties);
    sy_window_properties = NULL;
}

static int sy_path_missing(const wchar_t *file) {
    if (GetFileAttributesW(file) != INVALID_FILE_ATTRIBUTES) return 0;
    DWORD code = GetLastError();
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND;
}

static int sy_same_install(const wchar_t *target, const wchar_t *directory) {
    const wchar_t *separator = wcsrchr(target, L'\\');
    size_t length = wcslen(directory);
    if (length && directory[length - 1] == L'\\') length--;
    return separator && (size_t)(separator - target) == length &&
           !_wcsnicmp(target, directory, length);
}

/* Returns 1 for a link to this install, 0 for a missing link or another install. */
static int sy_win_shortcut(const wchar_t *file, const wchar_t *exe, const wchar_t *directory,
                           int create, int *standard) {
    DWORD attr = GetFileAttributesW(file);
    int exists = attr != INVALID_FILE_ATTRIBUTES;
    if (standard) *standard = 0;
    if (!exists && (!create || !sy_path_missing(file))) return !create ? 0 : -1;
    if (exists && (attr & FILE_ATTRIBUTE_DIRECTORY)) return -1;

    IShellLinkW *link = NULL;
    IPersistFile *persist = NULL;
    IPropertyStore *store = NULL;
    int updated = 0;
    HRESULT hr = CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, &IID_IShellLinkW, (void **)&link);
    if (SUCCEEDED(hr)) hr = link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&persist);
    if (SUCCEEDED(hr)) hr = link->lpVtbl->QueryInterface(link, &IID_IPropertyStore, (void **)&store);
    if (SUCCEEDED(hr) && exists) {
        hr = persist->lpVtbl->Load(persist, file, STGM_READWRITE);
        if (SUCCEEDED(hr)) {
            wchar_t target[1024] = {0}, args[1024] = {0};
            link->lpVtbl->GetPath(link, target, ARRAYSIZE(target), NULL, 0);
            HRESULT args_result = link->lpVtbl->GetArguments(link, args, ARRAYSIZE(args));
            if (standard) *standard = SUCCEEDED(args_result) && !args[0];
            const wchar_t *name = wcsrchr(target, L'\\');
            PROPERTYKEY key = sy_shell_key(SY_SHELL_ID);
            PROPVARIANT value = {0};
            store->lpVtbl->GetValue(store, &key, &value);
            int owned = value.vt == VT_LPWSTR && value.pwszVal && !wcscmp(value.pwszVal, SY_WIN_APP_ID);
            int another_app = value.vt != VT_EMPTY && !owned;
            PropVariantClear(&value);
            int recognized = name && (!_wcsicmp(name + 1, SY_WIN_EXE) || sy_legacy_exe(name + 1));
            int same_directory = recognized && sy_same_install(target, directory);
            if (another_app || (!owned && !same_directory)) {
                if (create) hr = E_ACCESSDENIED;
                goto done;
            }
            if (!create && !same_directory && (!recognized || !sy_path_missing(target))) goto done;
            if (owned && !_wcsicmp(target, exe)) {
                wchar_t work[1024] = {0}, icon[1024] = {0}, description[256] = {0};
                int index = -1;
                link->lpVtbl->GetWorkingDirectory(link, work, ARRAYSIZE(work));
                link->lpVtbl->GetIconLocation(link, icon, ARRAYSIZE(icon), &index);
                link->lpVtbl->GetDescription(link, description, ARRAYSIZE(description));
                if (!_wcsicmp(work, directory) && !_wcsicmp(icon, exe) && index == 0 &&
                    !wcscmp(description, SY_WIN_TITLE)) {
                    updated = 1;
                    goto done;
                }
            }
        }
    }
    if (SUCCEEDED(hr)) hr = link->lpVtbl->SetPath(link, exe);
    if (SUCCEEDED(hr)) hr = link->lpVtbl->SetWorkingDirectory(link, directory);
    if (SUCCEEDED(hr)) hr = link->lpVtbl->SetIconLocation(link, exe, 0);
    if (SUCCEEDED(hr)) hr = link->lpVtbl->SetDescription(link, SY_WIN_TITLE);
    if (SUCCEEDED(hr)) hr = sy_set_shell_string(store, SY_SHELL_ID, SY_WIN_APP_ID);
    if (SUCCEEDED(hr)) hr = store->lpVtbl->Commit(store);
    if (SUCCEEDED(hr)) {
        hr = persist->lpVtbl->Save(persist, file, TRUE);
        updated = SUCCEEDED(hr);
        if (updated && !exists && standard) *standard = 1;
    }
done:
    if (store) store->lpVtbl->Release(store);
    if (persist) persist->lpVtbl->Release(persist);
    if (link) link->lpVtbl->Release(link);
    return FAILED(hr) ? -1 : updated;
}

static int sy_win_shortcuts_in(const wchar_t *destination, const wchar_t *exe,
                               const wchar_t *directory, int create) {
    wchar_t file[MAX_PATH + 32];
    swprintf(file, ARRAYSIZE(file), L"%ls\\%ls.lnk", destination, SY_WIN_TITLE);
    int standard = 0;
    int current = sy_win_shortcut(file, exe, directory, 0, &standard);
    for (size_t old = 0; old < ARRAYSIZE(sy_legacy_entries); old++) {
        int migrate = current == 0 && sy_path_missing(file);
        if (!migrate && !(current == 1 && standard)) break;
        wchar_t legacy[MAX_PATH + 32];
        swprintf(legacy, ARRAYSIZE(legacy), L"%ls\\%ls.lnk", destination, sy_legacy_entries[old].title);
        int old_standard = 0;
        if (sy_win_shortcut(legacy, exe, directory, 0, &old_standard) <= 0) continue;
        if (migrate) {
            if (MoveFileW(legacy, file)) { current = 1; standard = old_standard; }
        } else if (old_standard) {
            /* Both verified links now launch the same product with the default arguments. */
            DeleteFileW(legacy);
        }
    }
    if (current < 0) return -1;
    if (create && current == 0 && sy_win_shortcut(file, exe, directory, 1, NULL) < 0) return -1;
    return 0;
}

static int sy_win_install_shortcuts(int create) {
    wchar_t exe[1024], directory[1024];
    if (!sy_win_entry(exe, ARRAYSIZE(exe))) return -1;
    wcscpy(directory, exe);
    wchar_t *separator = wcsrchr(directory, L'\\');
    separator[separator == directory + 2 && directory[1] == L':' ? 1 : 0] = 0;
    const int folders[] = { CSIDL_DESKTOPDIRECTORY, CSIDL_PROGRAMS };
    int result = 0;
    for (size_t i = 0; i < ARRAYSIZE(folders); i++) {
        wchar_t destination[MAX_PATH];
        if (FAILED(SHGetFolderPathW(NULL, folders[i], NULL, SHGFP_TYPE_CURRENT, destination)) ||
            sy_win_shortcuts_in(destination, exe, directory, create) < 0) result = -1;
    }
    return result;
}

static int sy_win_shortcuts(HWND parent) {
    int result = sy_win_install_shortcuts(1);
    if (result) MessageBoxW(parent, L"无法创建部分或全部快捷方式。请先将" SY_WIN_TITLE L"解压到固定目录，再检查已有快捷方式和写入权限。", SY_WIN_TITLE, MB_OK | MB_ICONWARNING);
    return result;
}

#endif
