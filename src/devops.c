/*
 * devops.c - デバイス操作 (設計書 5, 6, 7, 8, 13, 21 章)
 *
 * 設計書 21 章の原則をここで守る:
 *   「API が成功したことだけでは成功扱いにしない」
 * すべての rename は、実行後に必ず再列挙して実際の名前を取り直し、
 * 期待値と突き合わせてから結果コードを決める。
 */
#include "dnm.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <netcon.h>

/* ------------------------------------------------------------------ */
/* 実行時にだけ解決する API                                            */
/*                                                                     */
/* インポートライブラリの有無がツールチェーンで割れるものは、           */
/* リンクせず実行時に取る。実測でどちらも Windows 11 26200 に存在する。 */
/*                                                                     */
/*   DiUninstallDevice        newdev.dll   : MinGW の libnewdev.a に無い */
/*   NcIsValidConnectionName  netshell.dll : Windows SDK に .lib が無い  */
/* ------------------------------------------------------------------ */
typedef BOOL (WINAPI *PFN_DiUninstallDevice)(HWND, HDEVINFO, PSP_DEVINFO_DATA,
                                             DWORD, PBOOL);
typedef BOOL (WINAPI *PFN_NcIsValidConnectionName)(PCWSTR);

/* nci.dll : ネットワーク接続名の読み書き。
 * netsh interface set interface が内部で呼んでいるのがこれ。
 * ヘッダーも .lib も公開されていないので、シグネチャは自前で書く。
 * 戻り値は Win32 エラーコード (0 = 成功)。 */
typedef DWORD (WINAPI *PFN_NciGetConnectionName)(const GUID *, LPWSTR, DWORD, DWORD *);
typedef DWORD (WINAPI *PFN_NciSetConnectionName)(const GUID *, LPCWSTR);

static PFN_DiUninstallDevice get_di_uninstall_device(void)
{
    static PFN_DiUninstallDevice fn = NULL;
    static BOOL tried = FALSE;
    if (!tried) {
        HMODULE m = LoadLibraryW(L"newdev.dll");
        if (m) fn = (PFN_DiUninstallDevice)(void *)
                        GetProcAddress(m, "DiUninstallDevice");
        tried = TRUE;
    }
    return fn;
}

static PFN_NcIsValidConnectionName get_nc_is_valid_connection_name(void)
{
    static PFN_NcIsValidConnectionName fn = NULL;
    static BOOL tried = FALSE;
    if (!tried) {
        HMODULE m = LoadLibraryW(L"netshell.dll");
        if (m) fn = (PFN_NcIsValidConnectionName)(void *)
                        GetProcAddress(m, "NcIsValidConnectionName");
        tried = TRUE;
    }
    return fn;
}

/* nci.dll は遅延で 1 回だけ開く。2 つの関数をまとめて取る。 */
static BOOL get_nci(PFN_NciGetConnectionName *getFn, PFN_NciSetConnectionName *setFn)
{
    static PFN_NciGetConnectionName g = NULL;
    static PFN_NciSetConnectionName s = NULL;
    static BOOL tried = FALSE;
    if (!tried) {
        HMODULE m = LoadLibraryW(L"nci.dll");
        if (m) {
            g = (PFN_NciGetConnectionName)(void *)
                    GetProcAddress(m, "NciGetConnectionName");
            s = (PFN_NciSetConnectionName)(void *)
                    GetProcAddress(m, "NciSetConnectionName");
        }
        tried = TRUE;
    }
    *getFn = g;
    *setFn = s;
    return (g != NULL && s != NULL);
}

/* HRESULT を Win32 エラーに戻す。FACILITY_WIN32 の HRESULT
 * (0x8007xxxx) だけが対象で、それ以外は元の値をそのまま返せないので
 * 判定用に ERROR_INVALID_FUNCTION を返す。呼び手は生の HRESULT も持つ。 */
static DWORD hr_to_win32(HRESULT hr)
{
    if ((hr & 0xFFFF0000u) == (DWORD)(0x80070000u))
        return (DWORD)(hr & 0xFFFF);
    return ERROR_INVALID_FUNCTION;
}

static void res_init(OpResult *r)
{
    ZeroMemory(r, sizeof(*r));
    r->code = OPR_FAILED;
}

static void res_fail_win32(OpResult *r, DWORD err, const WCHAR *what)
{
    WCHAR msg[300];
    r->code = OPR_FAILED;
    r->win32Error = err;
    dnm_format_error(err, msg, sizeof(msg) / sizeof(WCHAR));
    _snwprintf(r->message, 512, L"%s に失敗しました。\n%s", what, msg);
    r->message[511] = 0;
}

static void res_fail_hr(OpResult *r, HRESULT hr, const WCHAR *what)
{
    WCHAR msg[300];
    r->code = OPR_FAILED;
    r->hr = hr;
    dnm_format_error((DWORD)hr, msg, sizeof(msg) / sizeof(WCHAR));
    _snwprintf(r->message, 512, L"%s に失敗しました。\nHRESULT 0x%08lX\n%s",
               what, (unsigned long)hr, msg);
    r->message[511] = 0;
}

/* ------------------------------------------------------------------ */
/* 再列挙 (設計書 13 章)                                               */
/* ------------------------------------------------------------------ */
void dnm_rescan_devices(void)
{
    DEVINST root = 0;
    if (CM_Locate_DevNodeW(&root, NULL, CM_LOCATE_DEVNODE_NORMAL) == CR_SUCCESS)
        CM_Reenumerate_DevNode(root, 0);
}

/* ------------------------------------------------------------------ */
/* 一般 PnP デバイスのリネーム (設計書 6 章)                            */
/*                                                                     */
/* 第一選択は SetupDiSetDevicePropertyW(DEVPKEY_Device_FriendlyName)。   */
/* 失敗したときだけ SPDRP_FRIENDLYNAME にフォールバックする。            */
/* レジストリ直接編集はしない (設計書 6.2)。                            */
/* ------------------------------------------------------------------ */
void dnm_rename_pnp(const DeviceInfo *d, const WCHAR *newName, OpResult *res)
{
    HDEVINFO h;
    SP_DEVINFO_DATA dd;
    BOOL ok = FALSE;
    DWORD err = 0;
    DWORD bytes;
    const WCHAR *api = L"";

    res_init(res);

    h = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        res_fail_win32(res, GetLastError(), L"デバイス情報セットの作成");
        return;
    }

    dd.cbSize = sizeof(dd);
    if (!SetupDiOpenDeviceInfoW(h, d->instanceId, NULL, 0, &dd)) {
        err = GetLastError();
        SetupDiDestroyDeviceInfoList(h);
        res_fail_win32(res, err, L"デバイスインスタンスのオープン");
        return;
    }

    bytes = (DWORD)((wcslen(newName) + 1) * sizeof(WCHAR));

    /* SDK の宣言は const PBYTE (= BYTE * const) で、const BYTE * ではない。
     * const BYTE * を渡すと MSVC が C4090 を出すので PBYTE へ落とす。
     * API 側が書き換えることはない。 */
    ok = SetupDiSetDevicePropertyW(h, &dd, &DEVPKEY_Device_FriendlyName,
                                   DEVPROP_TYPE_STRING, (PBYTE)newName,
                                   bytes, 0);
    api = L"SetupDiSetDeviceProperty";
    if (!ok) {
        err = GetLastError();
        /* フォールバック: 旧来の SPDRP_FRIENDLYNAME */
        ok = SetupDiSetDeviceRegistryPropertyW(h, &dd, SPDRP_FRIENDLYNAME,
                                               (const BYTE *)newName, bytes);
        if (ok) api = L"SetupDiSetDeviceRegistryProperty (fallback)";
        else    err = GetLastError();
    }
    SetupDiDestroyDeviceInfoList(h);

    if (!ok) {
        res_fail_win32(res, err, L"表示名の設定");
        /* 権限不足は原因がはっきりしているので、そう書く。
         * 「アクセスが拒否されました」だけでは何をすればよいか分からない。 */
        if (err == ERROR_ACCESS_DENIED || err == ERROR_ELEVATION_REQUIRED) {
            ElevationInfo ei;
            WCHAR add[300];
            dnm_get_elevation(&ei);
            _snwprintf(add, 300,
                       L"\n\nPnP デバイス名の変更には管理者権限が必要です。\n"
                       L"現在の状態: %s (整合性レベル: %s)",
                       ei.elevated ? L"昇格済み" : L"昇格していません",
                       dnm_integrity_text(ei.integrityRid));
            add[299] = 0;
            wcsncat(res->message, add, 511 - wcslen(res->message));
            res->message[511] = 0;
        }
        return;
    }

    /* --- ここから検証 (設計書 21 章) --------------------------------- */
    {
        DeviceInfo after;
        if (dnm_refetch(d->instanceId, &after)) {
            dnm_strcpy(res->actualName, DNM_MAX_NAME, after.displayName);
            free(after.hardwareIds);
            if (wcscmp(after.friendlyName, newName) == 0) {
                res->code = OPR_OK;
                _snwprintf(res->message, 512,
                           L"表示名を「%s」に変更しました。\n使用 API: %s",
                           newName, api);
            } else {
                res->code = OPR_APPLIED_NOT_KEPT;
                _snwprintf(res->message, 512,
                           L"Windows API では変更に成功しましたが、\n"
                           L"再取得すると表示名が「%s」になっています。\n\n"
                           L"このデバイスのドライバーまたは Windows のデバイス管理機構が\n"
                           L"名前を再生成している可能性があります。",
                           after.displayName);
            }
        } else {
            res->code = OPR_APPLIED_NOT_KEPT;
            dnm_strcpy(res->message, 512,
                       L"変更は成功しましたが、再取得でデバイスを見つけられませんでした。");
        }
        res->message[511] = 0;
    }
}

/* ------------------------------------------------------------------ */
/* オーディオエンドポイントのリネーム (設計書 7.3)                      */
/*                                                                     */
/* 実測: OpenPropertyStore(STGM_READWRITE) は非管理者でも S_OK を返す。  */
/* ただし SetValue/Commit が実際に永続するかは別問題なので、書き込み後   */
/* に必ず読み直して確認する。                                           */
/* ------------------------------------------------------------------ */
void dnm_rename_audio_endpoint(const WCHAR *endpointId, const WCHAR *newName,
                               OpResult *res)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IPropertyStore *ps = NULL;
    PROPVARIANT pv;
    HRESULT hr;

    res_init(res);

    hr = CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                          &IID_IMMDeviceEnumerator, (void **)&en);
    if (FAILED(hr)) { res_fail_hr(res, hr, L"MMDeviceEnumerator の生成"); return; }

    hr = IMMDeviceEnumerator_GetDevice(en, endpointId, &dev);
    if (FAILED(hr)) {
        IMMDeviceEnumerator_Release(en);
        res_fail_hr(res, hr, L"オーディオエンドポイントの取得");
        return;
    }

    hr = IMMDevice_OpenPropertyStore(dev, STGM_READWRITE, &ps);
    if (FAILED(hr)) {
        IMMDevice_Release(dev);
        IMMDeviceEnumerator_Release(en);
        res_fail_hr(res, hr,
                    L"プロパティストアの書き込みオープン\n"
                    L"(管理者権限で実行しているか確認してください)");
        return;
    }

    PropVariantInit(&pv);
    pv.vt = VT_LPWSTR;
    pv.pwszVal = (LPWSTR)newName;   /* Clear しないので所有権は移さない */
    hr = IPropertyStore_SetValue(ps, &PKEY_Device_FriendlyName, &pv);
    if (SUCCEEDED(hr))
        hr = IPropertyStore_Commit(ps);
    pv.vt = VT_EMPTY;
    pv.pwszVal = NULL;

    IPropertyStore_Release(ps);
    ps = NULL;

    if (FAILED(hr)) {
        IMMDevice_Release(dev);
        IMMDeviceEnumerator_Release(en);
        res_fail_hr(res, hr, L"エンドポイント表示名の書き込み");
        return;
    }

    /* --- 読み直して検証 --------------------------------------------- */
    if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
        PROPVARIANT chk;
        PropVariantInit(&chk);
        if (SUCCEEDED(IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &chk)) &&
            chk.vt == VT_LPWSTR && chk.pwszVal) {
            dnm_strcpy(res->actualName, DNM_MAX_NAME, chk.pwszVal);
            if (wcscmp(chk.pwszVal, newName) == 0) {
                res->code = OPR_OK;
                _snwprintf(res->message, 512,
                           L"オーディオエンドポイント名を「%s」に変更しました。", newName);
            } else {
                res->code = OPR_APPLIED_NOT_KEPT;
                _snwprintf(res->message, 512,
                           L"書き込みは成功しましたが、読み直すと「%s」のままです。\n"
                           L"Windows がこのプロパティを管理している可能性があります。",
                           chk.pwszVal);
            }
        } else {
            res->code = OPR_APPLIED_NOT_KEPT;
            dnm_strcpy(res->message, 512, L"書き込み後の読み直しに失敗しました。");
        }
        PropVariantClear(&chk);
        IPropertyStore_Release(ps);
    } else {
        res->code = OPR_APPLIED_NOT_KEPT;
        dnm_strcpy(res->message, 512, L"書き込み後の検証ができませんでした。");
    }
    res->message[511] = 0;

    IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
}

/* ------------------------------------------------------------------ */
/* ネットワーク接続名のリネーム (設計書 8.1)                            */
/*                                                                     */
/* 経路を 2 つ持ち、順に試す。Windows のビルドによってどちらが通るかが   */
/* 変わるためで、片方だけに賭けない。                                   */
/*                                                                     */
/*   1. nci.dll の NciSetConnectionName                                 */
/*      netsh interface set interface が内部で呼んでいるもの。          */
/*      Vista 以降ずっとあり、接続名の実体をここが書く。                */
/*   2. INetConnection::Rename                                          */
/*      設計書が想定していた経路。                                      */
/*                                                                     */
/* 実測 (Windows 11 26200 / 2026-09-13):                                */
/*   INetConnection::Rename は非管理者でも管理者として実行しても        */
/*   0x800702E4 (ERROR_ELEVATION_REQUIRED) を返し、昇格しても通らない。 */
/*   (TokenIsElevated=1 / 整合性レベル 高 を確認したうえでの結果)       */
/*   NciSetConnectionName は非管理者で 5 (ACCESS_DENIED)、管理者で 0。   */
/*   後者では Get-NetAdapter の InterfaceAlias まで追従した。           */
/*                                                                     */
/*   ただしこれは 26200 での測定値で、23H2 など他のビルドで             */
/*   INetConnection::Rename が通らないと決まったわけではない。          */
/*   だから 1 が駄目なら 2 も試し、どちらの結果も利用者に見せる。       */
/*                                                                     */
/* レジストリ (Control\Network\{class}\{guid}\Connection\Name) の直接    */
/* 書き換えは選ばない。実測で値は変わるが InterfaceAlias が追従せず、   */
/* 名前が二重管理になる。                                               */
/* ------------------------------------------------------------------ */
static DWORD rename_net_via_netconnection(const DeviceInfo *d, const WCHAR *newName,
                                          HRESULT *hrOut);

void dnm_rename_net_alias(const DeviceInfo *d, const WCHAR *newName, OpResult *res)
{
    PFN_NciGetConnectionName nciGet = NULL;
    PFN_NciSetConnectionName nciSet = NULL;
    DWORD nciRc = (DWORD)-1;      /* -1 = 試していない */
    DWORD ncRc  = (DWORD)-1;
    HRESULT ncHr = S_OK;
    BOOL done = FALSE;
    const WCHAR *usedApi = L"";

    res_init(res);

    if (!d->hasNetConn) {
        res->code = OPR_SKIPPED;
        dnm_strcpy(res->message, 512,
                   L"このデバイスに対応するネットワーク接続が見つかりません。");
        return;
    }

    {   /* 取れないときは検証を飛ばす。設定側がエラーを返すので実害はない */
        PFN_NcIsValidConnectionName isValid = get_nc_is_valid_connection_name();
        if (isValid && !isValid(newName)) {
            res->code = OPR_FAILED;
            dnm_strcpy(res->message, 512,
                       L"ネットワーク接続名として使えない文字が含まれています。");
            return;
        }
    }

    /* --- 経路 1: NciSetConnectionName -------------------------------- */
    if (get_nci(&nciGet, &nciSet)) {
        nciRc = nciSet(&d->netConnGuid, newName);
        if (nciRc == ERROR_SUCCESS) { done = TRUE; usedApi = L"NciSetConnectionName"; }
    }

    /* --- 経路 2: INetConnection::Rename ------------------------------- */
    /* 名前の衝突は経路を変えても直らないので、そこで打ち切る。 */
    if (!done && nciRc != ERROR_ALREADY_EXISTS && nciRc != ERROR_DUP_NAME) {
        ncRc = rename_net_via_netconnection(d, newName, &ncHr);
        if (ncRc == ERROR_SUCCESS) { done = TRUE; usedApi = L"INetConnection::Rename"; }
    }

    if (!done) {
        WCHAR tried[300];
        WCHAR nciMsg[64], ncMsg[64];

        if (nciRc == (DWORD)-1) dnm_strcpy(nciMsg, 64, L"nci.dll を読み込めず未実行");
        else                    _snwprintf(nciMsg, 64, L"%lu", (unsigned long)nciRc);
        if (ncRc == (DWORD)-1)  dnm_strcpy(ncMsg, 64, L"未実行");
        else                    _snwprintf(ncMsg, 64, L"HRESULT 0x%08lX",
                                           (unsigned long)ncHr);

        _snwprintf(tried, 300,
                   L"\n\n試した経路:\n"
                   L"  NciSetConnectionName  : %s\n"
                   L"  INetConnection::Rename: %s",
                   nciMsg, ncMsg);
        tried[299] = 0;

        /* 名前競合 (設計書 8.2) は分かりやすく伝える */
        if (nciRc == ERROR_ALREADY_EXISTS || nciRc == ERROR_DUP_NAME ||
            ncHr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ||
            ncHr == HRESULT_FROM_WIN32(ERROR_DUP_NAME)) {
            res->code = OPR_FAILED;
            res->win32Error = nciRc;
            _snwprintf(res->message, 512,
                       L"「%s」はすでに他のネットワーク接続が使っています。\n"
                       L"旧アダプターを先に削除してから再実行してください。", newName);
            res->message[511] = 0;
            return;
        }

        /* 権限不足のときは、判断材料をそのまま出す。
         * 「管理者として実行したのに権限エラー」の切り分けはこれが要る。 */
        if (nciRc == ERROR_ACCESS_DENIED || nciRc == ERROR_ELEVATION_REQUIRED ||
            ncRc  == ERROR_ACCESS_DENIED || ncRc  == ERROR_ELEVATION_REQUIRED) {
            WCHAR report[600];
            dnm_privilege_report(report, 600);
            res->code = OPR_FAILED;
            res->win32Error = (nciRc != (DWORD)-1) ? nciRc : ncRc;
            _snwprintf(res->message, 512,
                       L"ネットワーク接続名の変更が権限不足で拒否されました。%s\n\n%s",
                       tried, report);
            res->message[511] = 0;
            return;
        }

        res_fail_win32(res, (nciRc != (DWORD)-1) ? nciRc : ncRc,
                       L"ネットワーク接続名の変更");
        wcsncat(res->message, tried, 511 - wcslen(res->message));
        res->message[511] = 0;
        return;
    }

    /* --- 検証: 読み直して名前を確認 (設計書 21 章) -------------------- */
    {
        WCHAR back[DNM_MAX_NAME];
        DWORD need = 0;
        back[0] = 0;
        if (nciGet &&
            nciGet(&d->netConnGuid, back, (DWORD)sizeof(back), &need) == ERROR_SUCCESS) {
            dnm_strcpy(res->actualName, DNM_MAX_NAME, back);
            if (wcscmp(back, newName) == 0) {
                res->code = OPR_OK;
                _snwprintf(res->message, 512,
                           L"ネットワーク接続名を「%s」に変更しました。\n"
                           L"使用 API: %s", newName, usedApi);
            } else {
                res->code = OPR_APPLIED_NOT_KEPT;
                _snwprintf(res->message, 512,
                           L"API は成功しましたが、読み直すと「%s」のままです。", back);
            }
        } else {
            /* 読み直せないときは成功を断定しない (設計書 21 章) */
            res->code = OPR_APPLIED_NOT_KEPT;
            _snwprintf(res->message, 512,
                       L"%s は成功を返しましたが、変更後の確認ができませんでした。",
                       usedApi);
        }
        res->message[511] = 0;
    }
}

/* 経路 2。成功なら ERROR_SUCCESS、失敗なら Win32 エラーを返す。
 * 生の HRESULT は hrOut に入れて、呼び手がそのまま表示できるようにする。 */
static DWORD rename_net_via_netconnection(const DeviceInfo *d, const WCHAR *newName,
                                          HRESULT *hrOut)
{
    INetConnectionManager *mgr = NULL;
    IEnumNetConnection *e = NULL;
    INetConnection *c = NULL;
    ULONG got = 0;
    HRESULT hr = E_FAIL;
    BOOL found = FALSE;

    *hrOut = E_FAIL;

    hr = CoCreateInstance(&CLSID_ConnectionManager, NULL, CLSCTX_ALL,
                          &IID_INetConnectionManager, (void **)&mgr);
    if (FAILED(hr)) { *hrOut = hr; return hr_to_win32(hr); }

    hr = INetConnectionManager_EnumConnections(mgr, NCME_DEFAULT, &e);
    if (FAILED(hr)) {
        INetConnectionManager_Release(mgr);
        *hrOut = hr;
        return hr_to_win32(hr);
    }

    while (!found && IEnumNetConnection_Next(e, 1, &c, &got) == S_OK && got == 1) {
        NETCON_PROPERTIES *p = NULL;
        if (SUCCEEDED(INetConnection_GetProperties(c, &p)) && p) {
            if (IsEqualGUID(&p->guidId, &d->netConnGuid)) {
                found = TRUE;
                hr = INetConnection_Rename(c, newName);
            }
            if (p->pszwName)       CoTaskMemFree(p->pszwName);
            if (p->pszwDeviceName) CoTaskMemFree(p->pszwDeviceName);
            CoTaskMemFree(p);
        }
        INetConnection_Release(c);
        c = NULL;
    }
    IEnumNetConnection_Release(e);
    INetConnectionManager_Release(mgr);

    if (!found) {
        *hrOut = HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
        return ERROR_NOT_FOUND;
    }
    *hrOut = hr;
    return SUCCEEDED(hr) ? ERROR_SUCCESS : hr_to_win32(hr);
}

/* ------------------------------------------------------------------ */
/* デバイスインスタンスの削除 (設計書 5.2)                              */
/*                                                                     */
/* 第一候補は設計書どおり DiUninstallDevice。再起動が必要かどうかを      */
/* 返してくれるぶん、こちらのほうが利用者に伝えられる情報が多い。        */
/* newdev.dll から実行時に取るので、インポートライブラリの有無に         */
/* 左右されない。取れなければ SetupDiCallClassInstaller(DIF_REMOVE) に   */
/* 落ちる。どちらもドライバーパッケージは削除しない (設計書 5.4)。       */
/* ------------------------------------------------------------------ */
void dnm_remove_device(const DeviceInfo *d, OpResult *res)
{
    HDEVINFO h;
    SP_DEVINFO_DATA dd;
    SP_REMOVEDEVICE_PARAMS rp;
    PFN_DiUninstallDevice diUninstall;
    BOOL needReboot = FALSE;
    const WCHAR *usedApi = L"";
    DWORD err = 0;

    res_init(res);

    if (d->protect != PROT_NONE) {
        res->code = OPR_SKIPPED;
        _snwprintf(res->message, 512, L"このデバイスは削除できません。\n理由: %s",
                   dnm_protect_reason_text(d->protect));
        res->message[511] = 0;
        return;
    }

    dnm_enable_privilege(SE_LOAD_DRIVER_NAME);

    h = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        res_fail_win32(res, GetLastError(), L"デバイス情報セットの作成");
        return;
    }

    dd.cbSize = sizeof(dd);
    if (!SetupDiOpenDeviceInfoW(h, d->instanceId, NULL, 0, &dd)) {
        err = GetLastError();
        SetupDiDestroyDeviceInfoList(h);
        res_fail_win32(res, err, L"デバイスインスタンスのオープン");
        return;
    }

    diUninstall = get_di_uninstall_device();
    if (diUninstall) {
        usedApi = L"DiUninstallDevice";
        if (!diUninstall(NULL, h, &dd, 0, &needReboot)) {
            err = GetLastError();
            SetupDiDestroyDeviceInfoList(h);
            res_fail_win32(res, err, L"デバイスの削除 (DiUninstallDevice)");
            return;
        }
    } else {
        usedApi = L"SetupDiCallClassInstaller(DIF_REMOVE)";

        ZeroMemory(&rp, sizeof(rp));
        rp.ClassInstallHeader.cbSize = sizeof(SP_CLASSINSTALL_HEADER);
        rp.ClassInstallHeader.InstallFunction = DIF_REMOVE;
        rp.Scope    = DI_REMOVEDEVICE_GLOBAL;
        rp.HwProfile = 0;

        if (!SetupDiSetClassInstallParamsW(h, &dd, &rp.ClassInstallHeader, sizeof(rp))) {
            err = GetLastError();
            SetupDiDestroyDeviceInfoList(h);
            res_fail_win32(res, err, L"削除パラメーターの設定");
            return;
        }
        if (!SetupDiCallClassInstaller(DIF_REMOVE, h, &dd)) {
            err = GetLastError();
            SetupDiDestroyDeviceInfoList(h);
            res_fail_win32(res, err, L"デバイスの削除");
            return;
        }
    }
    SetupDiDestroyDeviceInfoList(h);

    /* --- 検証: 本当に消えたか ---------------------------------------- */
    {
        DeviceInfo after;
        const WCHAR *rebootNote =
            needReboot ? L"\n\nこの削除を完了するには再起動が必要です。" : L"";

        if (dnm_refetch(d->instanceId, &after)) {
            free(after.hardwareIds);
            res->code = OPR_APPLIED_NOT_KEPT;
            _snwprintf(res->message, 512,
                       L"削除 API は成功しましたが、デバイスがまだ残っています。\n"
                       L"使用 API: %s%s",
                       usedApi,
                       needReboot ? L"\n\n再起動すると削除が完了します。"
                                  : L"\n\n再起動で解消するか確認してください。");
        } else {
            res->code = OPR_OK;
            _snwprintf(res->message, 512,
                       L"「%s」を削除しました。\n使用 API: %s%s",
                       d->displayName, usedApi, rebootNote);
        }
        res->message[511] = 0;
    }
}
