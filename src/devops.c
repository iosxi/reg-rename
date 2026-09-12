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
/* PowerShell を叩かず INetConnection::Rename を使う。                  */
/* ------------------------------------------------------------------ */
void dnm_rename_net_alias(const DeviceInfo *d, const WCHAR *newName, OpResult *res)
{
    INetConnectionManager *mgr = NULL;
    IEnumNetConnection *e = NULL;
    INetConnection *c = NULL;
    ULONG got = 0;
    HRESULT hr;
    BOOL found = FALSE;

    res_init(res);

    if (!d->hasNetConn) {
        res->code = OPR_SKIPPED;
        dnm_strcpy(res->message, 512,
                   L"このデバイスに対応するネットワーク接続が見つかりません。");
        return;
    }

    {   /* 取れないときは検証を飛ばす。Rename 自体がエラーを返すので実害はない */
        PFN_NcIsValidConnectionName isValid = get_nc_is_valid_connection_name();
        if (isValid && !isValid(newName)) {
            res->code = OPR_FAILED;
            dnm_strcpy(res->message, 512,
                       L"ネットワーク接続名として使えない文字が含まれています。");
            return;
        }
    }

    hr = CoCreateInstance(&CLSID_ConnectionManager, NULL, CLSCTX_ALL,
                          &IID_INetConnectionManager, (void **)&mgr);
    if (FAILED(hr)) { res_fail_hr(res, hr, L"ネットワーク接続マネージャーの生成"); return; }

    hr = INetConnectionManager_EnumConnections(mgr, NCME_DEFAULT, &e);
    if (FAILED(hr)) {
        INetConnectionManager_Release(mgr);
        res_fail_hr(res, hr, L"ネットワーク接続の列挙");
        return;
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

    if (!found) {
        INetConnectionManager_Release(mgr);
        res->code = OPR_FAILED;
        dnm_strcpy(res->message, 512, L"対応するネットワーク接続を列挙できませんでした。");
        return;
    }
    if (FAILED(hr)) {
        INetConnectionManager_Release(mgr);
        /* 名前競合 (設計書 8.2) は分かりやすく伝える */
        if (hr == HRESULT_FROM_WIN32(ERROR_ALREADY_EXISTS) ||
            hr == HRESULT_FROM_WIN32(ERROR_DUP_NAME)) {
            res->code = OPR_FAILED;
            res->hr = hr;
            _snwprintf(res->message, 512,
                       L"「%s」はすでに他のネットワーク接続が使っています。\n"
                       L"旧アダプターを先に削除してから再実行してください。", newName);
            res->message[511] = 0;
        } else {
            res_fail_hr(res, hr, L"ネットワーク接続名の変更");
        }
        return;
    }

    /* --- 検証: 列挙し直して名前を確認 -------------------------------- */
    res->code = OPR_APPLIED_NOT_KEPT;
    dnm_strcpy(res->message, 512, L"変更後の確認ができませんでした。");
    if (SUCCEEDED(INetConnectionManager_EnumConnections(mgr, NCME_DEFAULT, &e))) {
        while (IEnumNetConnection_Next(e, 1, &c, &got) == S_OK && got == 1) {
            NETCON_PROPERTIES *p = NULL;
            if (SUCCEEDED(INetConnection_GetProperties(c, &p)) && p) {
                if (IsEqualGUID(&p->guidId, &d->netConnGuid)) {
                    dnm_strcpy(res->actualName, DNM_MAX_NAME,
                               p->pszwName ? p->pszwName : L"");
                    if (p->pszwName && wcscmp(p->pszwName, newName) == 0) {
                        res->code = OPR_OK;
                        _snwprintf(res->message, 512,
                                   L"ネットワーク接続名を「%s」に変更しました。", newName);
                    } else {
                        _snwprintf(res->message, 512,
                                   L"API は成功しましたが、再取得すると「%s」のままです。",
                                   p->pszwName ? p->pszwName : L"(不明)");
                    }
                    res->message[511] = 0;
                }
                if (p->pszwName)       CoTaskMemFree(p->pszwName);
                if (p->pszwDeviceName) CoTaskMemFree(p->pszwDeviceName);
                CoTaskMemFree(p);
            }
            INetConnection_Release(c);
            c = NULL;
        }
        IEnumNetConnection_Release(e);
    }
    INetConnectionManager_Release(mgr);
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
