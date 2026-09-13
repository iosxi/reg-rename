/*
 * dnm.h - Windows Device Name Manager 共通定義
 *
 * 設計書 17 章のデータモデルを C へ落としたもの。
 * FriendlyName は主キーにしない。InstanceId が唯一の識別子 (設計書 2.1)。
 */
#ifndef DNM_H
#define DNM_H

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#define CINTERFACE

#include <windows.h>
#include <setupapi.h>
#include <cfgmgr32.h>
#include <devpkey.h>
#include <commctrl.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

/* UNICODE は Win32 API を W 版に解決させるために必須。
 * これが無いと ListView_SetItemText 等が A 版になり、ワイド文字列を
 * ANSI として解釈して表示が壊れる。MinGW では -municode が定義するが、
 * MSVC では明示が要るので、どちらでも落ちないようここで確認しておく。 */
#ifndef UNICODE
#  error "UNICODE を定義してビルドしてください (MinGW: -municode / MSVC: /DUNICODE)"
#endif

/* MinGW の setupapi.h にはこの関数の宣言が無い (libsetupapi.a には実体あり)。
 * Windows SDK には宣言があるので、MinGW のときだけ補う。
 * 実測: nm --defined-only libsetupapi.a → __imp_SetupDiSetDevicePropertyW あり */
#ifdef __MINGW32__
WINSETUPAPI BOOL WINAPI SetupDiSetDevicePropertyW(
    HDEVINFO DeviceInfoSet, PSP_DEVINFO_DATA DeviceInfoData,
    const DEVPROPKEY *PropertyKey, DEVPROPTYPE PropertyType,
    const BYTE *PropertyBuffer, DWORD PropertyBufferSize, DWORD Flags);
#endif

#define DNM_MAX_NAME     256
#define DNM_MAX_AUDIO_EP 16

/* ------------------------------------------------------------------ */
/* デバイス種別 (設計書 17 章 DeviceKind)                               */
/* ------------------------------------------------------------------ */
typedef enum {
    DK_GenericPnP = 0,
    DK_AudioEndpoint,
    DK_NetworkAdapter,
    DK_Usb,
    DK_Bluetooth,
    DK_Hid,
    DK_Storage,
    DK_Display,
    DK_Other
} DeviceKind;

/* 保護理由 (設計書 11.2) */
typedef enum {
    PROT_NONE = 0,
    PROT_SYSTEM_CLASS,      /* System / Computer / Processor など  */
    PROT_ROOT_ENUMERATOR,   /* HTREE\ROOT, ACPI_HAL 等             */
    PROT_USB_ROOT_HUB,
    PROT_STORAGE_CONTROLLER,
    PROT_BOOT_VOLUME,
    PROT_LAST_INPUT,        /* 唯一のキーボード / マウス           */
    PROT_LAST_NETWORK       /* 唯一の接続中ネットワークアダプター  */
} ProtectReason;

/* オーディオエンドポイント (設計書 7 章)
 *
 * 「サウンド」画面に出る名前 (PKEY_Device_FriendlyName) は Windows が
 *   <deviceDesc> (<interfaceName>)
 * と組み立てた値で、書き込めない。実測 (Windows 11 / 2026-09-13):
 *   PKEY_Device_FriendlyName          SetValue = 0x80070005 (管理者でも同じ)
 *   PKEY_DeviceInterface_FriendlyName SetValue = 0x80070005
 *   PKEY_Device_DeviceDesc            SetValue = 0 (非管理者でも通る)
 * したがって利用者が変えられるのは deviceDesc の部分だけ。
 * 括弧の中はデバイス側の名前なので、そちらは PnP 名の変更で動く。 */
typedef struct {
    WCHAR endpointId[DNM_MAX_NAME];   /* IMMDevice::GetId                     */
    WCHAR friendlyName[DNM_MAX_NAME]; /* PKEY_Device_FriendlyName (読み取り専用) */
    WCHAR deviceDesc[DNM_MAX_NAME];   /* PKEY_Device_DeviceDesc (ここだけ書ける) */
    WCHAR interfaceName[DNM_MAX_NAME];/* PKEY_DeviceInterface_FriendlyName    */
    int   flow;                       /* 0 = eRender, 1 = eCapture            */
    DWORD state;                      /* DEVICE_STATE_*                       */
} AudioEndpointInfo;

/* ------------------------------------------------------------------ */
/* デバイス 1 台ぶん                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    WCHAR  instanceId[MAX_DEVICE_ID_LEN]; /* 主キー                  */
    WCHAR  displayName[DNM_MAX_NAME];     /* 画面表示用の実効名      */
    WCHAR  friendlyName[DNM_MAX_NAME];    /* DEVPKEY_Device_FriendlyName (空の場合あり) */
    WCHAR  deviceDesc[DNM_MAX_NAME];      /* DEVPKEY_Device_DeviceDesc */
    WCHAR  className[64];
    WCHAR  manufacturer[DNM_MAX_NAME];
    WCHAR  location[DNM_MAX_NAME];
    WCHAR *hardwareIds;                   /* REG_MULTI_SZ をそのまま保持 */
    DWORD  hardwareIdsBytes;

    GUID   classGuid;      BOOL hasClassGuid;
    GUID   containerId;    BOOL hasContainerId;

    WORD   vid, pid;       BOOL hasVidPid;

    BOOL   isPresent;
    BOOL   isHidden;
    ProtectReason protect;
    DeviceKind kind;
    DEVINST devInst;

    /* Audio: 紐づくエンドポイント (設計書 19.2 の association) */
    AudioEndpointInfo audio[DNM_MAX_AUDIO_EP];
    int    audioCount;

    /* Net: 紐づく接続 (設計書 8 章の Interface Alias) */
    GUID   netConnGuid;  BOOL hasNetConn;
    WCHAR  netAlias[DNM_MAX_NAME];
} DeviceInfo;

typedef struct {
    DeviceInfo *items;
    int         count;
    int         capacity;
} DeviceList;

/* 列挙オプション (設計書 4.2) */
typedef struct {
    BOOL  showPresent;
    BOOL  showAbsent;
    BOOL  showSystem;
    int   kindFilter;   /* -1 = すべて、それ以外は DeviceKind */
    WCHAR search[128];
} EnumOptions;

/* 接続名を今いくつ持っているのはどのアダプターか */
typedef struct {
    BOOL  found;
    WCHAR guid[64];              /* NetCfgInstanceId */
    WCHAR adapterDesc[DNM_MAX_NAME];   /* DriverDesc */
    WCHAR instanceId[MAX_DEVICE_ID_LEN];
} ConnNameOwner;

/* 操作結果 (設計書 21 章: API 成功と実表示維持を別々に判定する) */
typedef enum {
    OPR_OK = 0,             /* API 成功 + 再列挙後も期待どおり */
    OPR_APPLIED_NOT_KEPT,   /* API 成功だが再列挙で元に戻った   */
    OPR_FAILED,             /* API 失敗                        */
    OPR_SKIPPED             /* 保護等で実行しなかった          */
} OpResultCode;

typedef struct {
    OpResultCode code;
    DWORD        win32Error;
    HRESULT      hr;
    /* 旧アダプターを消して再実行する経路が増えたぶん、結果が長くなる。
     * 512 だと 1 回目の失敗だけで埋まって、その後の経過が消える。 */
    WCHAR        message[1024];
    WCHAR        actualName[DNM_MAX_NAME]; /* 再列挙後に実際に取れた名前 */
    /* 操作の直前に書き出したバックアップ .reg のフルパス (改行区切り)。
     * 結果ダイアログにそのまま出す。 */
    WCHAR        backupInfo[1024];
    /* 接続名が既に使われていて失敗した場合。UI 側が確認画面を出すために使う。 */
    BOOL          nameConflict;
    ConnNameOwner conflictOwner;
} OpResult;

/* 実行中プロセスの権限。TokenIsElevated だけだと
 * 「なぜそう表示されるのか」が利用者に分からないので、根拠ごと持つ。 */
typedef struct {
    BOOL  elevated;      /* TokenElevation.TokenIsElevated */
    int   elevationType; /* TOKEN_ELEVATION_TYPE: 1=Default 2=Full 3=Limited */
    BOOL  inAdminGroup;  /* 実効トークンが Administrators を持つか */
    DWORD integrityRid;  /* SECURITY_MANDATORY_*_RID。取れなければ (DWORD)-1 */
} ElevationInfo;

/* 自分に埋め込まれている RT_MANIFEST の中身 */
typedef struct {
    WCHAR requestedLevel[32]; /* requireAdministrator / asInvoker など */
    int   count;              /* 見つかったマニフェストの数。2 以上はビルド異常 */
} ManifestInfo;

/* UAC の設定。読めなければ -1 */
typedef struct {
    int enableLua;
    int consentPromptBehaviorAdmin;
} UacPolicy;

/* 設定 (実行ファイルと同じフォルダの DeviceNameManager.ini) */
typedef struct {
    BOOL  backupEnabled;
    WCHAR backupDir[MAX_PATH];
} Config;

/* ------------------------------------------------------------------ */
/* util.c                                                              */
/* ------------------------------------------------------------------ */
void        dnm_list_init(DeviceList *list);
void        dnm_list_free(DeviceList *list);
DeviceInfo *dnm_list_add(DeviceList *list);
DeviceInfo *dnm_list_find(DeviceList *list, const WCHAR *instanceId);

void  dnm_strcpy(WCHAR *dst, size_t cap, const WCHAR *src);
BOOL  dnm_icontains(const WCHAR *hay, const WCHAR *needle);
BOOL  dnm_starts_with_i(const WCHAR *s, const WCHAR *prefix);
void  dnm_format_error(DWORD err, WCHAR *buf, size_t cap);
void  dnm_guid_to_string(const GUID *g, WCHAR *buf, size_t cap);
const WCHAR *dnm_kind_name(DeviceKind k);
const WCHAR *dnm_protect_reason_text(ProtectReason r);
/* 同じ製品が複数あるときの見分け用 (接続名 / エンドポイント名) */
const WCHAR *dnm_alias_text(const DeviceInfo *d);
BOOL  dnm_is_elevated(void);
void  dnm_get_elevation(ElevationInfo *ei);
void  dnm_get_manifest_info(ManifestInfo *mi);
void  dnm_get_uac_policy(UacPolicy *up);
const WCHAR *dnm_integrity_text(DWORD rid);
void  dnm_elevation_status_text(WCHAR *buf, size_t cap);
/* 権限まわりを全部まとめた診断テキスト (失敗ダイアログと権限診断で共用) */
void  dnm_privilege_report(WCHAR *buf, size_t cap);
BOOL  dnm_enable_privilege(const WCHAR *name);
/* 連番付き名称からベース名を推定する (設計書 10 章)。断定はしない */
void  dnm_guess_base_name(const WCHAR *current, WCHAR *out, size_t cap);

/* ------------------------------------------------------------------ */
/* config.c                                                            */
/* ------------------------------------------------------------------ */
void dnm_exe_dir(WCHAR *buf, size_t cap);
void dnm_config_path(WCHAR *buf, size_t cap);
void dnm_default_backup_dir(WCHAR *buf, size_t cap);
void dnm_config_load(Config *c);
BOOL dnm_config_save(const Config *c);

/* ------------------------------------------------------------------ */
/* backup.c                                                            */
/* ------------------------------------------------------------------ */
void dnm_regpath_pnp(const WCHAR *instanceId, WCHAR *buf, size_t cap);
void dnm_regpath_netconn(const GUID *netCfgInstanceId, WCHAR *buf, size_t cap);
void dnm_regpath_audio(const WCHAR *endpointId, int flow, WCHAR *buf, size_t cap);
/* 変更の直前に呼ぶ。書き出せたら TRUE と outPath にフルパスを返す。 */
BOOL dnm_backup_reg_key(const WCHAR *regPath, const WCHAR *tag, const WCHAR *label,
                        WCHAR *outPath, size_t outCap, WCHAR *errOut, size_t errCap);

/* ------------------------------------------------------------------ */
/* devlist.c                                                           */
/* ------------------------------------------------------------------ */
BOOL dnm_enumerate(DeviceList *out);
BOOL dnm_device_matches(const DeviceInfo *d, const EnumOptions *opt);
/* 単一デバイスを再取得する (設計書 21 章の検証用) */
BOOL dnm_refetch(const WCHAR *instanceId, DeviceInfo *out);

/* ------------------------------------------------------------------ */
/* devops.c                                                            */
/* ------------------------------------------------------------------ */
void dnm_rescan_devices(void);
/* 接続名を今持っているアダプターを探す。exclude は自分自身の除外用 (NULL 可) */
void dnm_find_conn_name_owner(const WCHAR *name, const GUID *exclude,
                              ConnNameOwner *out);
void dnm_rename_pnp(const DeviceInfo *d, const WCHAR *newName, OpResult *res);
void dnm_rename_audio_endpoint(const WCHAR *endpointId, const WCHAR *newName, OpResult *res);
void dnm_rename_net_alias(const DeviceInfo *d, const WCHAR *newName, OpResult *res);
void dnm_remove_device(const DeviceInfo *d, OpResult *res);

/* ------------------------------------------------------------------ */
/* matching.c (設計書 20 章)                                           */
/* ------------------------------------------------------------------ */
typedef struct {
    int   index;        /* DeviceList 内の添字 */
    int   score;
    /* Hardware ID が一致したか。実測すると、複合デバイス (キーボード兼
     * マウス等) は Container ID / VID/PID / Class が揃うため 80 点に届く。
     * それらと「同じ製品の別インスタンス」を分けられるのがこの条件なので、
     * 既定チェックの可否判断に使う。 */
    BOOL  hardwareIdMatch;
    WCHAR reasons[256];
} MatchCandidate;

int dnm_find_candidates(const DeviceList *list, int targetIndex,
                        MatchCandidate *out, int maxOut);

/* ------------------------------------------------------------------ */
/* safety.c (設計書 11.2)                                              */
/* ------------------------------------------------------------------ */
void dnm_apply_protection(DeviceList *list);

/* ------------------------------------------------------------------ */
/* history.c (設計書 12 章)                                            */
/* ------------------------------------------------------------------ */
void dnm_history_path(WCHAR *buf, size_t cap);
void dnm_history_log_rename(const DeviceInfo *d, const WCHAR *target,
                            const WCHAR *oldName, const WCHAR *newName,
                            const OpResult *res);
void dnm_history_log_remove(const DeviceInfo *d, const OpResult *res);

/* ------------------------------------------------------------------ */
/* ui_dlg.c                                                            */
/* ------------------------------------------------------------------ */
BOOL dnm_dlg_rename(HWND parent, DeviceInfo *d);
BOOL dnm_dlg_remove_confirm(HWND parent, const DeviceInfo *d);
void dnm_dlg_details(HWND parent, const DeviceInfo *d);
BOOL dnm_dlg_cleanup(HWND parent, DeviceList *list, int targetIndex);
void dnm_dlg_settings(HWND parent);

/* ui_main.c が公開するもの */
extern HINSTANCE g_hInst;
void dnm_status(const WCHAR *fmt, ...);

#endif /* DNM_H */
