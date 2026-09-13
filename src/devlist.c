/*
 * devlist.c - デバイス列挙 (設計書 4 章)
 *
 *   1. SetupAPI で全 PnP デバイスを取得する (未接続も含む)
 *   2. Core Audio のエンドポイントを取得し、Instance ID で PnP 行に紐づける
 *   3. ネットワーク接続を取得し、NetCfgInstanceId で PnP 行に紐づける
 *
 * 実測メモ: この PC では全 636 台のうち FriendlyName を持つのは 137 台
 * だけだった。DeviceDesc へのフォールバックが無いと大半が無名になる。
 */
#include "dnm.h"
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <netcon.h>

extern const GUID DNM_GUID_DEVCLASS_NET;
extern const GUID DNM_GUID_DEVCLASS_MEDIA;
extern const GUID DNM_GUID_DEVCLASS_AUDIOENDPOINT;
extern const GUID DNM_GUID_DEVCLASS_USB;
extern const GUID DNM_GUID_DEVCLASS_USBDEVICE;
extern const GUID DNM_GUID_DEVCLASS_BLUETOOTH;
extern const GUID DNM_GUID_DEVCLASS_HIDCLASS;
extern const GUID DNM_GUID_DEVCLASS_DISKDRIVE;
extern const GUID DNM_GUID_DEVCLASS_VOLUME;
extern const GUID DNM_GUID_DEVCLASS_SCSIADAPTER;
extern const GUID DNM_GUID_DEVCLASS_DISPLAY;
extern const GUID DNM_GUID_DEVCLASS_MONITOR;
extern const GUID DNM_GUID_DEVCLASS_SYSTEM;
extern const GUID DNM_GUID_DEVCLASS_COMPUTER;
extern const GUID DNM_GUID_DEVCLASS_PROCESSOR;

/* ------------------------------------------------------------------ */
/* プロパティ取得ヘルパー                                              */
/* ------------------------------------------------------------------ */
static BOOL prop_string(HDEVINFO h, PSP_DEVINFO_DATA d, const DEVPROPKEY *key,
                        WCHAR *out, size_t cap)
{
    DEVPROPTYPE type = 0;
    DWORD required = 0;
    out[0] = 0;
    if (!SetupDiGetDevicePropertyW(h, d, key, &type, (PBYTE)out,
                                   (DWORD)(cap * sizeof(WCHAR)), &required, 0)) {
        out[0] = 0;
        return FALSE;
    }
    if (type != DEVPROP_TYPE_STRING) { out[0] = 0; return FALSE; }
    out[cap - 1] = 0;
    return out[0] != 0;
}

static BOOL prop_guid(HDEVINFO h, PSP_DEVINFO_DATA d, const DEVPROPKEY *key, GUID *out)
{
    DEVPROPTYPE type = 0;
    DWORD required = 0;
    if (!SetupDiGetDevicePropertyW(h, d, key, &type, (PBYTE)out, sizeof(GUID),
                                   &required, 0))
        return FALSE;
    return type == DEVPROP_TYPE_GUID;
}

static BOOL prop_bool(HDEVINFO h, PSP_DEVINFO_DATA d, const DEVPROPKEY *key, BOOL *out)
{
    DEVPROPTYPE type = 0;
    DWORD required = 0;
    DEVPROP_BOOLEAN v = DEVPROP_FALSE;
    if (!SetupDiGetDevicePropertyW(h, d, key, &type, (PBYTE)&v, sizeof(v), &required, 0))
        return FALSE;
    if (type != DEVPROP_TYPE_BOOLEAN) return FALSE;
    *out = (v != DEVPROP_FALSE);
    return TRUE;
}

/* REG_MULTI_SZ をそのまま確保して返す */
static WCHAR *prop_multisz(HDEVINFO h, PSP_DEVINFO_DATA d, const DEVPROPKEY *key,
                           DWORD *outBytes)
{
    DEVPROPTYPE type = 0;
    DWORD required = 0;
    BYTE *buf;

    *outBytes = 0;
    SetupDiGetDevicePropertyW(h, d, key, &type, NULL, 0, &required, 0);
    if (required == 0) return NULL;

    buf = (BYTE *)calloc(1, required + 2 * sizeof(WCHAR));
    if (!buf) return NULL;
    if (!SetupDiGetDevicePropertyW(h, d, key, &type, buf, required, &required, 0)) {
        free(buf);
        return NULL;
    }
    *outBytes = required;
    return (WCHAR *)buf;
}

/* MULTI_SZ を巡回する */
const WCHAR *dnm_multisz_next(const WCHAR *cur)
{
    if (!cur || !*cur) return NULL;
    cur += wcslen(cur) + 1;
    return *cur ? cur : NULL;
}

/* ------------------------------------------------------------------ */
/* 種別判定 (設計書 4.3)                                               */
/* ------------------------------------------------------------------ */
static DeviceKind classify(const GUID *cls, BOOL hasCls, const WCHAR *instanceId)
{
    if (hasCls) {
        if (IsEqualGUID(cls, &DNM_GUID_DEVCLASS_NET))          return DK_NetworkAdapter;
        if (IsEqualGUID(cls, &DNM_GUID_DEVCLASS_MEDIA))        return DK_AudioEndpoint;
        if (IsEqualGUID(cls, &DNM_GUID_DEVCLASS_AUDIOENDPOINT))return DK_AudioEndpoint;
        if (IsEqualGUID(cls, &DNM_GUID_DEVCLASS_BLUETOOTH))    return DK_Bluetooth;
        if (IsEqualGUID(cls, &DNM_GUID_DEVCLASS_HIDCLASS))     return DK_Hid;
        if (IsEqualGUID(cls, &DNM_GUID_DEVCLASS_USB) ||
            IsEqualGUID(cls, &DNM_GUID_DEVCLASS_USBDEVICE))    return DK_Usb;
        if (IsEqualGUID(cls, &DNM_GUID_DEVCLASS_DISKDRIVE) ||
            IsEqualGUID(cls, &DNM_GUID_DEVCLASS_VOLUME) ||
            IsEqualGUID(cls, &DNM_GUID_DEVCLASS_SCSIADAPTER))  return DK_Storage;
        if (IsEqualGUID(cls, &DNM_GUID_DEVCLASS_DISPLAY) ||
            IsEqualGUID(cls, &DNM_GUID_DEVCLASS_MONITOR))      return DK_Display;
        if (IsEqualGUID(cls, &DNM_GUID_DEVCLASS_SYSTEM) ||
            IsEqualGUID(cls, &DNM_GUID_DEVCLASS_COMPUTER) ||
            IsEqualGUID(cls, &DNM_GUID_DEVCLASS_PROCESSOR))    return DK_Other;
    }
    if (dnm_starts_with_i(instanceId, L"USB\\"))       return DK_Usb;
    if (dnm_starts_with_i(instanceId, L"BTHENUM\\") ||
        dnm_starts_with_i(instanceId, L"BTHLE"))       return DK_Bluetooth;
    return DK_GenericPnP;
}

/* Hardware ID から VID/PID を拾う (設計書 Should: VID/PID 表示) */
static void parse_vid_pid(DeviceInfo *d)
{
    const WCHAR *p = d->hardwareIds;
    d->hasVidPid = FALSE;
    while (p && *p) {
        const WCHAR *v = wcsstr(p, L"VID_");
        if (!v) v = wcsstr(p, L"vid_");
        if (v) {
            const WCHAR *pp = wcsstr(v, L"PID_");
            if (!pp) pp = wcsstr(v, L"pid_");
            if (pp) {
                unsigned int vid = 0, pid = 0;
                if (swscanf(v + 4, L"%4x", &vid) == 1 &&
                    swscanf(pp + 4, L"%4x", &pid) == 1) {
                    d->vid = (WORD)vid;
                    d->pid = (WORD)pid;
                    d->hasVidPid = TRUE;
                    return;
                }
            }
        }
        p = dnm_multisz_next(p);
    }
}

/* ------------------------------------------------------------------ */
/* 1 デバイスぶんの読み取り                                            */
/* ------------------------------------------------------------------ */
static void read_device(HDEVINFO h, PSP_DEVINFO_DATA dd, DeviceInfo *d)
{
    ULONG status = 0, problem = 0;
    DWORD required = 0;

    d->devInst = dd->DevInst;

    if (!SetupDiGetDeviceInstanceIdW(h, dd, d->instanceId,
                                     (DWORD)(sizeof(d->instanceId) / sizeof(WCHAR)),
                                     &required))
        d->instanceId[0] = 0;

    prop_string(h, dd, &DEVPKEY_Device_FriendlyName, d->friendlyName,
                sizeof(d->friendlyName) / sizeof(WCHAR));
    prop_string(h, dd, &DEVPKEY_Device_DeviceDesc, d->deviceDesc,
                sizeof(d->deviceDesc) / sizeof(WCHAR));
    prop_string(h, dd, &DEVPKEY_Device_Class, d->className,
                sizeof(d->className) / sizeof(WCHAR));
    prop_string(h, dd, &DEVPKEY_Device_Manufacturer, d->manufacturer,
                sizeof(d->manufacturer) / sizeof(WCHAR));
    prop_string(h, dd, &DEVPKEY_Device_LocationInfo, d->location,
                sizeof(d->location) / sizeof(WCHAR));

    /* 実効表示名: FriendlyName → DeviceDesc → Instance ID の順で埋める。
     * 実測では 636 台中 137 台しか FriendlyName を持たない。 */
    if (d->friendlyName[0])
        dnm_strcpy(d->displayName, DNM_MAX_NAME, d->friendlyName);
    else if (d->deviceDesc[0])
        dnm_strcpy(d->displayName, DNM_MAX_NAME, d->deviceDesc);
    else
        dnm_strcpy(d->displayName, DNM_MAX_NAME, d->instanceId);

    d->hasClassGuid = prop_guid(h, dd, &DEVPKEY_Device_ClassGuid, &d->classGuid);
    if (!d->hasClassGuid && !IsEqualGUID(&dd->ClassGuid, &GUID_NULL)) {
        d->classGuid = dd->ClassGuid;
        d->hasClassGuid = TRUE;
    }
    d->hasContainerId = prop_guid(h, dd, &DEVPKEY_Device_ContainerId, &d->containerId);

    d->hardwareIds = prop_multisz(h, dd, &DEVPKEY_Device_HardwareIds,
                                  &d->hardwareIdsBytes);
    parse_vid_pid(d);

    /* 接続状態。DEVPKEY_Device_IsPresent があればそれを信じ、
     * 無ければ devnode status が取れるかどうかで判定する。 */
    if (CM_Get_DevNode_Status(&status, &problem, dd->DevInst, 0) == CR_SUCCESS) {
        d->isPresent = TRUE;
        d->isHidden  = (status & DN_NO_SHOW_IN_DM) ? TRUE : FALSE;
    } else {
        d->isPresent = FALSE;
        d->isHidden  = FALSE;
    }
    prop_bool(h, dd, &DEVPKEY_Device_IsPresent, &d->isPresent);

    d->kind = classify(&d->classGuid, d->hasClassGuid, d->instanceId);
}

/* ------------------------------------------------------------------ */
/* Audio エンドポイントの紐づけ (設計書 7 章 / 19.2)                    */
/*                                                                     */
/* エンドポイントから PnP デバイスへ辿る経路を実機で 2 通り測った結果:  */
/*                                                                     */
/*   PKEY_DeviceNode_Path 経由     62 / 63                             */
/*   SWD\MMDEVAPI devnode の親経由  8 / 63                             */
/*   両方成立した 8 件はすべて一致                                      */
/*                                                                     */
/* よって前者を主、後者を補助にする。                                    */
/* PKEY_Device_InstanceId は S_OK を返すが値が空 (VT_EMPTY) なので使えない。*/
/* ------------------------------------------------------------------ */

/* 値は "{1}.USB\VID_262A&PID_9023&MI_01\8&63A6124&0&0001" の形。
 * 先頭の "{N}." を落とすと PnP の Instance ID になる。実体は guids.c。 */
extern const PROPERTYKEY PKEY_DeviceNode_Path;

/* SWD\MMDEVAPI\<endpoint id> の親 devnode を引く (補助経路) */
static BOOL endpoint_owner_via_swd(const WCHAR *endpointId, WCHAR *out, size_t cap)
{
    HDEVINFO h;
    SP_DEVINFO_DATA dd;
    WCHAR inst[MAX_DEVICE_ID_LEN];
    DEVPROPTYPE type;
    DWORD required;
    BOOL ok = FALSE;

    _snwprintf(inst, MAX_DEVICE_ID_LEN, L"SWD\\MMDEVAPI\\%s", endpointId);
    inst[MAX_DEVICE_ID_LEN - 1] = 0;

    h = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    dd.cbSize = sizeof(dd);
    if (SetupDiOpenDeviceInfoW(h, inst, NULL, 0, &dd))
        ok = SetupDiGetDevicePropertyW(h, &dd, &DEVPKEY_Device_Parent, &type,
                                       (PBYTE)out, (DWORD)(cap * sizeof(WCHAR)),
                                       &required, 0);
    SetupDiDestroyDeviceInfoList(h);
    return ok;
}

/* エンドポイントの状態から、一覧に載せる優先度を決める。
 * 1 台の HD Audio コントローラーが十数個のエンドポイントを持つことがあるので、
 * 使われているものから先に埋める。 */
static int endpoint_priority(DWORD state)
{
    switch (state) {
    case DEVICE_STATE_ACTIVE:     return 0;
    case DEVICE_STATE_UNPLUGGED:  return 1;
    case DEVICE_STATE_DISABLED:   return 2;
    default:                      return 3;   /* NOTPRESENT */
    }
}

static void attach_audio_endpoints(DeviceList *list)
{
    IMMDeviceEnumerator *en = NULL;
    IMMDeviceCollection *col = NULL;
    UINT n = 0, i;

    if (FAILED(CoCreateInstance(&CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &IID_IMMDeviceEnumerator, (void **)&en)))
        return;

    if (SUCCEEDED(IMMDeviceEnumerator_EnumAudioEndpoints(en, eAll,
                                                         DEVICE_STATEMASK_ALL, &col))) {
        int pass;
        IMMDeviceCollection_GetCount(col, &n);

        /* 優先度の高いものから 4 巡して埋める (ACTIVE → … → NOTPRESENT) */
        for (pass = 0; pass <= 3; pass++)
        for (i = 0; i < n; i++) {
            IMMDevice *dev = NULL;
            IPropertyStore *ps = NULL;
            LPWSTR epId = NULL;
            PROPVARIANT pvName, pvPath, pvDesc, pvIface;
            DWORD state = 0;
            WCHAR ownerId[MAX_DEVICE_ID_LEN];
            DeviceInfo *owner = NULL;

            if (FAILED(IMMDeviceCollection_Item(col, i, &dev))) continue;
            IMMDevice_GetState(dev, &state);
            if (endpoint_priority(state) != pass) {
                IMMDevice_Release(dev);
                continue;
            }
            IMMDevice_GetId(dev, &epId);

            PropVariantInit(&pvName);
            PropVariantInit(&pvPath);
            PropVariantInit(&pvDesc);
            PropVariantInit(&pvIface);
            if (SUCCEEDED(IMMDevice_OpenPropertyStore(dev, STGM_READ, &ps))) {
                IPropertyStore_GetValue(ps, &PKEY_Device_FriendlyName, &pvName);
                IPropertyStore_GetValue(ps, &PKEY_DeviceNode_Path, &pvPath);
                /* 表示名は組み立て結果なので、その素も取っておく。
                 * 書き換えられるのは DeviceDesc のほうだけ (実測)。 */
                IPropertyStore_GetValue(ps, &PKEY_Device_DeviceDesc, &pvDesc);
                IPropertyStore_GetValue(ps, &PKEY_DeviceInterface_FriendlyName, &pvIface);
            }

            ownerId[0] = 0;
            if (pvPath.vt == VT_LPWSTR && pvPath.pwszVal) {
                const WCHAR *p = pvPath.pwszVal;
                if (p[0] == L'{') {
                    const WCHAR *dot = wcschr(p, L'.');
                    if (dot) p = dot + 1;   /* 先頭の "{1}." を落とす */
                }
                dnm_strcpy(ownerId, MAX_DEVICE_ID_LEN, p);
            }
            if (!ownerId[0] && epId)
                endpoint_owner_via_swd(epId, ownerId, MAX_DEVICE_ID_LEN);

            if (ownerId[0]) owner = dnm_list_find(list, ownerId);

            if (owner && owner->audioCount < DNM_MAX_AUDIO_EP) {
                AudioEndpointInfo *ep = &owner->audio[owner->audioCount++];
                IMMEndpoint *endp = NULL;

                dnm_strcpy(ep->endpointId, DNM_MAX_NAME, epId ? epId : L"");
                dnm_strcpy(ep->friendlyName, DNM_MAX_NAME,
                           pvName.vt == VT_LPWSTR ? pvName.pwszVal : L"");
                dnm_strcpy(ep->deviceDesc, DNM_MAX_NAME,
                           pvDesc.vt == VT_LPWSTR ? pvDesc.pwszVal : L"");
                dnm_strcpy(ep->interfaceName, DNM_MAX_NAME,
                           pvIface.vt == VT_LPWSTR ? pvIface.pwszVal : L"");
                ep->state = state;
                ep->flow = 0;
                /* 出力 / 入力の別は IMMEndpoint からしか取れない */
                if (SUCCEEDED(IMMDevice_QueryInterface(dev, &IID_IMMEndpoint,
                                                       (void **)&endp))) {
                    EDataFlow flow = eRender;
                    IMMEndpoint_GetDataFlow(endp, &flow);
                    ep->flow = (flow == eCapture) ? 1 : 0;
                    IMMEndpoint_Release(endp);
                }
                owner->kind = DK_AudioEndpoint;
            }

            PropVariantClear(&pvName);
            PropVariantClear(&pvPath);
            PropVariantClear(&pvDesc);
            PropVariantClear(&pvIface);
            if (ps) IPropertyStore_Release(ps);
            if (epId) CoTaskMemFree(epId);
            IMMDevice_Release(dev);
        }
        IMMDeviceCollection_Release(col);
    }
    IMMDeviceEnumerator_Release(en);
}

/* ------------------------------------------------------------------ */
/* ネットワーク接続の紐づけ (設計書 8 章)                              */
/*                                                                     */
/* PnP デバイス側のドライバーキーにある NetCfgInstanceId が、           */
/* ネットワーク接続の GUID と一致する。これが両者の確実な対応付け。      */
/* ------------------------------------------------------------------ */
static BOOL read_netcfg_instance_id(HDEVINFO h, PSP_DEVINFO_DATA dd, GUID *out)
{
    HKEY key;
    WCHAR buf[64];
    DWORD cb = sizeof(buf), type = 0;
    LONG r;

    key = SetupDiOpenDevRegKey(h, dd, DICS_FLAG_GLOBAL, 0, DIREG_DRV, KEY_READ);
    if (key == INVALID_HANDLE_VALUE) return FALSE;

    r = RegQueryValueExW(key, L"NetCfgInstanceId", NULL, &type, (LPBYTE)buf, &cb);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_SZ) return FALSE;

    buf[(sizeof(buf) / sizeof(WCHAR)) - 1] = 0;
    return SUCCEEDED(CLSIDFromString(buf, out));
}

static void attach_net_aliases(DeviceList *list)
{
    INetConnectionManager *mgr = NULL;
    IEnumNetConnection *e = NULL;
    INetConnection *c = NULL;
    ULONG got = 0;

    if (FAILED(CoCreateInstance(&CLSID_ConnectionManager, NULL, CLSCTX_ALL,
                                &IID_INetConnectionManager, (void **)&mgr)))
        return;

    if (SUCCEEDED(INetConnectionManager_EnumConnections(mgr, NCME_DEFAULT, &e))) {
        while (IEnumNetConnection_Next(e, 1, &c, &got) == S_OK && got == 1) {
            NETCON_PROPERTIES *p = NULL;
            if (SUCCEEDED(INetConnection_GetProperties(c, &p)) && p) {
                int i;
                for (i = 0; i < list->count; i++) {
                    DeviceInfo *d = &list->items[i];
                    if (d->hasNetConn && IsEqualGUID(&d->netConnGuid, &p->guidId)) {
                        dnm_strcpy(d->netAlias, DNM_MAX_NAME,
                                   p->pszwName ? p->pszwName : L"");
                        break;
                    }
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
/* 公開 API                                                            */
/* ------------------------------------------------------------------ */
BOOL dnm_enumerate(DeviceList *out)
{
    HDEVINFO h;
    SP_DEVINFO_DATA dd;
    DWORD i;

    dnm_list_free(out);
    dnm_list_init(out);

    /* DIGCF_PRESENT を付けないので未接続デバイスも入る (設計書 4.2) */
    h = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_ALLCLASSES);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    dd.cbSize = sizeof(dd);
    for (i = 0; SetupDiEnumDeviceInfo(h, i, &dd); i++) {
        DeviceInfo *d = dnm_list_add(out);
        if (!d) break;
        read_device(h, &dd, d);
        if (d->kind == DK_NetworkAdapter)
            d->hasNetConn = read_netcfg_instance_id(h, &dd, &d->netConnGuid);
    }
    SetupDiDestroyDeviceInfoList(h);

    attach_audio_endpoints(out);
    attach_net_aliases(out);
    dnm_apply_protection(out);
    return TRUE;
}

BOOL dnm_refetch(const WCHAR *instanceId, DeviceInfo *out)
{
    HDEVINFO h;
    SP_DEVINFO_DATA dd;
    BOOL ok = FALSE;

    ZeroMemory(out, sizeof(*out));
    h = SetupDiCreateDeviceInfoList(NULL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;

    dd.cbSize = sizeof(dd);
    if (SetupDiOpenDeviceInfoW(h, instanceId, NULL, 0, &dd)) {
        read_device(h, &dd, out);
        if (out->kind == DK_NetworkAdapter)
            out->hasNetConn = read_netcfg_instance_id(h, &dd, &out->netConnGuid);
        ok = TRUE;
    }
    SetupDiDestroyDeviceInfoList(h);
    return ok;
}

BOOL dnm_device_matches(const DeviceInfo *d, const EnumOptions *opt)
{
    if (d->isPresent  && !opt->showPresent) return FALSE;
    if (!d->isPresent && !opt->showAbsent)  return FALSE;

    /* システムデバイスは既定で隠す。数が多く、用途にも関係しないため */
    if (!opt->showSystem && d->protect != PROT_NONE) return FALSE;

    if (opt->kindFilter >= 0 && (int)d->kind != opt->kindFilter) return FALSE;

    if (opt->search[0]) {
        if (!dnm_icontains(d->displayName, opt->search) &&
            !dnm_icontains(d->instanceId,  opt->search) &&
            !dnm_icontains(d->className,   opt->search) &&
            !dnm_icontains(d->netAlias,    opt->search) &&
            !dnm_icontains(d->manufacturer, opt->search))
            return FALSE;
    }
    return TRUE;
}
