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
    ElevationInfo ei;
    dnm_get_elevation(&ei);
    return ei.elevated;
}

/* 「管理者権限で実行中」が本当かどうかは、TokenIsElevated だけでは
 * 利用者に確かめようがない。判断材料が 3 つに分かれているためで、
 * どれが効いているかは PC ごとに違う。
 *
 *   1. exe のマニフェストが何を要求しているか (requireAdministrator か)
 *   2. UAC の設定 (EnableLUA / ConsentPromptBehaviorAdmin)
 *   3. 実際に得られたトークン (TokenIsElevated / 整合性レベル)
 *
 * 1 が requireAdministrator なら、ダブルクリックでも Windows が昇格させる。
 * つまり「そのまま起動したのに管理者と出る」のは設計どおりで、
 * プロンプトが出るかどうかだけが 2 で決まる。
 * この 3 つを全部出せば、どの PC でも利用者自身が確かめられる。 */
void dnm_get_elevation(ElevationInfo *ei)
{
    HANDLE tok = NULL;
    DWORD cb = 0;

    ZeroMemory(ei, sizeof(*ei));
    ei->integrityRid = (DWORD)-1;

    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) {
        TOKEN_ELEVATION el;
        TOKEN_ELEVATION_TYPE et;
        DWORD need = 0;

        if (GetTokenInformation(tok, TokenElevation, &el, sizeof(el), &cb))
            ei->elevated = el.TokenIsElevated ? TRUE : FALSE;
        if (GetTokenInformation(tok, TokenElevationType, &et, sizeof(et), &cb))
            ei->elevationType = (int)et;

        GetTokenInformation(tok, TokenIntegrityLevel, NULL, 0, &need);
        if (need) {
            BYTE *buf = (BYTE *)malloc(need);
            if (buf) {
                if (GetTokenInformation(tok, TokenIntegrityLevel, buf, need, &need)) {
                    TOKEN_MANDATORY_LABEL *ml = (TOKEN_MANDATORY_LABEL *)buf;
                    UCHAR *cnt = GetSidSubAuthorityCount(ml->Label.Sid);
                    if (cnt && *cnt > 0)
                        ei->integrityRid =
                            *GetSidSubAuthority(ml->Label.Sid, (DWORD)(*cnt - 1));
                }
                free(buf);
            }
        }
        CloseHandle(tok);
    }

    {   /* 実効トークンが Administrators を持っているか。
         * 制限付きトークンでは FALSE になる (それが正しい)。 */
        SID_IDENTIFIER_AUTHORITY nt = SECURITY_NT_AUTHORITY;
        PSID admins = NULL;
        BOOL member = FALSE;
        if (AllocateAndInitializeSid(&nt, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                     DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0,
                                     &admins)) {
            if (CheckTokenMembership(NULL, admins, &member))
                ei->inAdminGroup = member;
            FreeSid(admins);
        }
    }
}

/* 自分自身に埋め込まれた RT_MANIFEST を読んで、要求している実行レベルを返す。
 *
 * ここを実行時に見るのには理由がある。MinGW-w64 の spec は実行ファイルに
 * 必ず asInvoker の default-manifest.o を足すので、対処しないと exe に
 * マニフェストが 2 つ入り、どちらが効くか読めなくなる。
 * 埋め込まれている現物を数えて出せば、ビルドが壊れていればそこで分かる。 */
void dnm_get_manifest_info(ManifestInfo *mi)
{
    HRSRC r;
    HGLOBAL g;
    const char *p;
    DWORD size;

    ZeroMemory(mi, sizeof(mi[0]));
    dnm_strcpy(mi->requestedLevel, 32, L"(マニフェスト無し)");

    r = FindResourceW(NULL, MAKEINTRESOURCEW(1), (LPCWSTR)RT_MANIFEST);
    if (!r) return;
    mi->count = 1;   /* 既定の言語で 1 件見つかった */

    size = SizeofResource(NULL, r);
    g = LoadResource(NULL, r);
    if (!g || size == 0) return;
    p = (const char *)LockResource(g);
    if (!p) return;

    /* マニフェストは UTF-8 のプレーンテキスト。
     *
     * ここで「requireAdministrator という語を探す」とやってはいけない。
     * app-uitest.manifest の日本語コメントに
     *   「requireAdministrator を asInvoker にしてある」
     * と書いてあるため、asInvoker のビルドを requireAdministrator と
     * 誤判定する (実測でそうなった)。
     * requestedExecutionLevel の level 属性の値だけを読む。 */
    {
        DWORD i, j;
        for (i = 0; i + 23 < size; i++) {
            if (memcmp(p + i, "requestedExecutionLevel", 23) != 0) continue;

            for (j = i + 23; j + 7 < size && j < i + 200; j++) {
                if (memcmp(p + j, "level=\"", 7) != 0) continue;
                {
                    DWORD s = j + 7, n = 0;
                    char val[32];
                    while (s + n < size && p[s + n] != '"' && n < sizeof(val) - 1) {
                        val[n] = p[s + n];
                        n++;
                    }
                    val[n] = 0;
                    MultiByteToWideChar(CP_UTF8, 0, val, -1, mi->requestedLevel, 32);
                    mi->requestedLevel[31] = 0;
                    return;
                }
            }
        }
    }
    dnm_strcpy(mi->requestedLevel, 32, L"(実行レベルの記述無し)");
}

/* UAC の設定。どちらも読めなければ -1 のままにする。 */
void dnm_get_uac_policy(UacPolicy *up)
{
    HKEY k = NULL;
    up->enableLua = -1;
    up->consentPromptBehaviorAdmin = -1;

    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE,
                      L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Policies\\System",
                      0, KEY_READ, &k) != ERROR_SUCCESS)
        return;
    {
        DWORD v = 0, cb = sizeof(v), type = 0;
        if (RegQueryValueExW(k, L"EnableLUA", NULL, &type, (LPBYTE)&v, &cb)
                == ERROR_SUCCESS && type == REG_DWORD)
            up->enableLua = (int)v;
        cb = sizeof(v);
        if (RegQueryValueExW(k, L"ConsentPromptBehaviorAdmin", NULL, &type,
                             (LPBYTE)&v, &cb) == ERROR_SUCCESS && type == REG_DWORD)
            up->consentPromptBehaviorAdmin = (int)v;
    }
    RegCloseKey(k);
}

/* 権限まわりの全体像を 1 つの文字列にまとめる。
 * 失敗ダイアログにも、権限診断にも、同じものを出す。 */
void dnm_privilege_report(WCHAR *buf, size_t cap)
{
    ElevationInfo ei;
    ManifestInfo  mi;
    UacPolicy     up;
    WCHAR uacLua[64], uacPrompt[96];

    dnm_get_elevation(&ei);
    dnm_get_manifest_info(&mi);
    dnm_get_uac_policy(&up);

    if (up.enableLua < 0)
        dnm_strcpy(uacLua, 64, L"(読めません)");
    else
        _snwprintf(uacLua, 64, L"%d (%s)", up.enableLua,
                   up.enableLua ? L"UAC 有効" : L"UAC 無効");

    if (up.consentPromptBehaviorAdmin < 0)
        dnm_strcpy(uacPrompt, 96, L"(読めません)");
    else
        _snwprintf(uacPrompt, 96, L"%d (%s)", up.consentPromptBehaviorAdmin,
                   up.consentPromptBehaviorAdmin == 0
                       ? L"確認せずに昇格するため UAC の画面は出ません"
                       : L"昇格時に UAC の確認が出ます");

    _snwprintf(buf, cap,
        L"[この実行ファイルが要求している権限]\n"
        L"  マニフェスト: %s\n"
        L"  埋め込み数  : %d %s\n"
        L"\n"
        L"[UAC の設定]\n"
        L"  EnableLUA                  = %s\n"
        L"  ConsentPromptBehaviorAdmin = %s\n"
        L"\n"
        L"[実際に得られたトークン]\n"
        L"  TokenIsElevated   = %d\n"
        L"  TokenElevationType= %d (1=既定 2=完全 3=制限付き)\n"
        L"  Administrators    = %d\n"
        L"  整合性レベル      = %s",
        mi.requestedLevel,
        mi.count,
        mi.count > 1 ? L"← 2 つ以上は異常。ビルドを見直してください" : L"",
        uacLua, uacPrompt,
        (int)ei.elevated, ei.elevationType, (int)ei.inAdminGroup,
        dnm_integrity_text(ei.integrityRid));
    buf[cap - 1] = 0;
}

const WCHAR *dnm_integrity_text(DWORD rid)
{
    if (rid == (DWORD)-1)                          return L"不明";
    if (rid >= SECURITY_MANDATORY_SYSTEM_RID)      return L"システム";
    if (rid >= SECURITY_MANDATORY_HIGH_RID)        return L"高";
    if (rid >= SECURITY_MANDATORY_MEDIUM_RID)      return L"中";
    if (rid >= SECURITY_MANDATORY_LOW_RID)         return L"低";
    return L"最低";
}

/* ステータスバー用の一行。何を根拠にそう言っているかまで書く。 */
void dnm_elevation_status_text(WCHAR *buf, size_t cap)
{
    ElevationInfo ei;
    dnm_get_elevation(&ei);

    if (ei.elevated) {
        _snwprintf(buf, cap,
                   L"管理者権限で実行中 (昇格済み / 整合性レベル: %s)",
                   dnm_integrity_text(ei.integrityRid));
    } else if (ei.elevationType == TokenElevationTypeLimited) {
        /* 管理者アカウントだが昇格していない。昇格すれば通る。 */
        _snwprintf(buf, cap,
                   L"管理者権限なし (制限付きトークン / 整合性レベル: %s)"
                   L"  変更・削除は失敗します",
                   dnm_integrity_text(ei.integrityRid));
    } else {
        _snwprintf(buf, cap,
                   L"管理者権限なし (整合性レベル: %s)  変更・削除は失敗します",
                   dnm_integrity_text(ei.integrityRid));
    }
    buf[cap - 1] = 0;
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
