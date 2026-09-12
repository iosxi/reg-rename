/*
 * util.c - 文字列 / リスト / 権限まわりの小道具
 */
#include "dnm.h"

/* ------------------------------------------------------------------ */
/* DeviceList                                                          */
/* ------------------------------------------------------------------ */
void dnm_list_init(DeviceList *list)
{
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

void dnm_list_free(DeviceList *list)
{
    int i;
    for (i = 0; i < list->count; i++)
        free(list->items[i].hardwareIds);
    free(list->items);
    dnm_list_init(list);
}

DeviceInfo *dnm_list_add(DeviceList *list)
{
    if (list->count == list->capacity) {
        int cap = list->capacity ? list->capacity * 2 : 128;
        DeviceInfo *p = (DeviceInfo *)realloc(list->items, (size_t)cap * sizeof(DeviceInfo));
        if (!p) return NULL;
        list->items = p;
        list->capacity = cap;
    }
    {
        DeviceInfo *d = &list->items[list->count++];
        ZeroMemory(d, sizeof(*d));
        return d;
    }
}

DeviceInfo *dnm_list_find(DeviceList *list, const WCHAR *instanceId)
{
    int i;
    for (i = 0; i < list->count; i++)
        if (_wcsicmp(list->items[i].instanceId, instanceId) == 0)
            return &list->items[i];
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 文字列                                                              */
/* ------------------------------------------------------------------ */
void dnm_strcpy(WCHAR *dst, size_t cap, const WCHAR *src)
{
    if (!dst || cap == 0) return;
    if (!src) { dst[0] = 0; return; }
    wcsncpy(dst, src, cap - 1);
    dst[cap - 1] = 0;
}

BOOL dnm_icontains(const WCHAR *hay, const WCHAR *needle)
{
    size_t nl;
    if (!needle || !needle[0]) return TRUE;
    if (!hay || !hay[0]) return FALSE;
    nl = wcslen(needle);
    for (; *hay; hay++)
        if (_wcsnicmp(hay, needle, nl) == 0)
            return TRUE;
    return FALSE;
}

BOOL dnm_starts_with_i(const WCHAR *s, const WCHAR *prefix)
{
    if (!s || !prefix) return FALSE;
    return _wcsnicmp(s, prefix, wcslen(prefix)) == 0;
}

void dnm_format_error(DWORD err, WCHAR *buf, size_t cap)
{
    WCHAR *msg = NULL;
    DWORD n = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL, err, 0, (LPWSTR)&msg, 0, NULL);
    if (n && msg) {
        /* 末尾の改行を落とす */
        while (n > 0 && (msg[n - 1] == L'\r' || msg[n - 1] == L'\n')) msg[--n] = 0;
        _snwprintf(buf, cap, L"%s (0x%08lX)", msg, (unsigned long)err);
    } else {
        _snwprintf(buf, cap, L"エラー 0x%08lX", (unsigned long)err);
    }
    buf[cap - 1] = 0;
    if (msg) LocalFree(msg);
}

void dnm_guid_to_string(const GUID *g, WCHAR *buf, size_t cap)
{
    _snwprintf(buf, cap, L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
               (unsigned long)g->Data1, g->Data2, g->Data3,
               g->Data4[0], g->Data4[1], g->Data4[2], g->Data4[3],
               g->Data4[4], g->Data4[5], g->Data4[6], g->Data4[7]);
    buf[cap - 1] = 0;
}

const WCHAR *dnm_kind_name(DeviceKind k)
{
    switch (k) {
    case DK_GenericPnP:    return L"PnP";
    case DK_AudioEndpoint: return L"Audio";
    case DK_NetworkAdapter:return L"Net";
    case DK_Usb:           return L"USB";
    case DK_Bluetooth:     return L"Bluetooth";
    case DK_Hid:           return L"HID";
    case DK_Storage:       return L"Storage";
    case DK_Display:       return L"Display";
    default:               return L"Other";
    }
}

const WCHAR *dnm_protect_reason_text(ProtectReason r)
{
    switch (r) {
    case PROT_SYSTEM_CLASS:      return L"システムデバイスのため";
    case PROT_ROOT_ENUMERATOR:   return L"ルート列挙子配下のため";
    case PROT_USB_ROOT_HUB:      return L"USB ルートハブのため";
    case PROT_STORAGE_CONTROLLER:return L"ストレージコントローラーのため";
    case PROT_BOOT_VOLUME:       return L"ブート / システムボリューム関連のため";
    case PROT_LAST_INPUT:        return L"唯一の入力デバイスのため";
    case PROT_LAST_NETWORK:      return L"唯一の接続中ネットワークアダプターのため";
    default:                     return L"";
    }
}

/* ------------------------------------------------------------------ */
/* 権限                                                                */
/* ------------------------------------------------------------------ */
BOOL dnm_is_elevated(void)
{
    HANDLE tok = NULL;
    TOKEN_ELEVATION el;
    DWORD cb = 0;
    BOOL ok = FALSE;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &cb))
            ok = el.TokenIsElevated ? TRUE : FALSE;
        CloseHandle(tok);
    }
    return ok;
}

/* デバイス削除には SeLoadDriverPrivilege が要る (devcon と同じ) */
BOOL dnm_enable_privilege(const WCHAR *name)
{
    HANDLE tok = NULL;
    TOKEN_PRIVILEGES tp;
    BOOL ok = FALSE;

    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return FALSE;

    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (LookupPrivilegeValueW(NULL, name, &tp.Privileges[0].Luid)) {
        AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
        ok = (GetLastError() == ERROR_SUCCESS);
    }
    CloseHandle(tok);
    return ok;
}

/* ------------------------------------------------------------------ */
/* ベース名推定 (設計書 10 章)                                          */
/*                                                                     */
/* 「数字を消せば元の名前」と断定してはいけないので、あくまで初期値の   */
/* 提案にとどめる。ユーザーが編集して確定する前提。                     */
/* ------------------------------------------------------------------ */
static BOOL is_all_digits(const WCHAR *s, const WCHAR *end)
{
    if (s >= end) return FALSE;
    for (; s < end; s++)
        if (*s < L'0' || *s > L'9') return FALSE;
    return TRUE;
}

void dnm_guess_base_name(const WCHAR *current, WCHAR *out, size_t cap)
{
    size_t len;
    const WCHAR *p;

    dnm_strcpy(out, cap, current);
    len = wcslen(out);
    if (len == 0) return;

    /* 末尾の空白を落とす */
    while (len > 0 && (out[len - 1] == L' ' || out[len - 1] == L'\t')) out[--len] = 0;
    if (len == 0) return;

    /* パターン 1: "USB DAC (2)" — 括弧つき連番 */
    if (out[len - 1] == L')') {
        p = out + len - 2;
        while (p > out && *p != L'(') p--;
        if (*p == L'(' && is_all_digits(p + 1, out + len - 1) && p > out) {
            size_t cut = (size_t)(p - out);
            out[cut] = 0;
            while (cut > 0 && out[cut - 1] == L' ') out[--cut] = 0;
            return;
        }
    }

    /* パターン 2: "Headphones #2" — シャープつき連番 */
    p = out + len;
    while (p > out && *(p - 1) >= L'0' && *(p - 1) <= L'9') p--;
    if (p < out + len && p > out && *(p - 1) == L'#') {
        size_t cut = (size_t)(p - 1 - out);
        out[cut] = 0;
        while (cut > 0 && out[cut - 1] == L' ') out[--cut] = 0;
        return;
    }

    /* パターン 3: "Ethernet 2" — 空白 + 数字。ただし "Ethernet" の様に
     * 数字が製品名の一部であることも多いので、直前が空白のときだけ削る */
    if (p < out + len && p > out && *(p - 1) == L' ') {
        size_t cut = (size_t)(p - 1 - out);
        /* 全体が数字だけになる場合 ("2" 等) は削らない */
        if (cut > 0) {
            out[cut] = 0;
            while (cut > 0 && out[cut - 1] == L' ') out[--cut] = 0;
        }
    }
}
