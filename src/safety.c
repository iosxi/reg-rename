/*
 * safety.c - 削除を禁止するデバイスの判定 (設計書 11.2)
 *
 * 「削除できない」ではなく「UI から削除操作を禁止する」。
 * 判定は 2 段階で、まず単体で決まるものを見てから、
 * 「唯一の入力デバイス」「唯一の接続中ネットワークアダプター」のように
 * 一覧全体を見ないと決まらないものを処理する。
 */
#include "dnm.h"

extern const GUID DNM_GUID_DEVCLASS_SYSTEM;
extern const GUID DNM_GUID_DEVCLASS_COMPUTER;
extern const GUID DNM_GUID_DEVCLASS_PROCESSOR;
extern const GUID DNM_GUID_DEVCLASS_HDC;
extern const GUID DNM_GUID_DEVCLASS_SCSIADAPTER;
extern const GUID DNM_GUID_DEVCLASS_VOLUME;
extern const GUID DNM_GUID_DEVCLASS_DISKDRIVE;
extern const GUID DNM_GUID_DEVCLASS_KEYBOARD;
extern const GUID DNM_GUID_DEVCLASS_MOUSE;
extern const GUID DNM_GUID_DEVCLASS_NET;

extern const WCHAR *dnm_multisz_next(const WCHAR *cur);

static BOOL has_hardware_id_prefix(const DeviceInfo *d, const WCHAR *prefix)
{
    const WCHAR *p;
    for (p = d->hardwareIds; p && *p; p = dnm_multisz_next(p))
        if (dnm_starts_with_i(p, prefix))
            return TRUE;
    return FALSE;
}

/* 単体で決まる保護 */
static ProtectReason classify_single(const DeviceInfo *d)
{
    /* ルート列挙子配下 — PnP ツリーの根に近く、消すと復旧が難しい */
    if (dnm_starts_with_i(d->instanceId, L"HTREE\\"))          return PROT_ROOT_ENUMERATOR;
    if (dnm_starts_with_i(d->instanceId, L"ACPI_HAL\\"))       return PROT_ROOT_ENUMERATOR;
    if (dnm_starts_with_i(d->instanceId, L"ROOT\\ACPI_HAL"))   return PROT_ROOT_ENUMERATOR;
    if (dnm_starts_with_i(d->instanceId, L"ACPI\\PNP0A"))      return PROT_ROOT_ENUMERATOR; /* PCI root complex */
    if (dnm_starts_with_i(d->instanceId, L"ROOT\\VOLUMESNAPSHOT")) return PROT_BOOT_VOLUME;
    if (dnm_starts_with_i(d->instanceId, L"ROOT\\SYSTEM"))     return PROT_SYSTEM_CLASS;
    if (dnm_starts_with_i(d->instanceId, L"ROOT\\BASICDISPLAY")) return PROT_SYSTEM_CLASS;
    if (dnm_starts_with_i(d->instanceId, L"ROOT\\BASICRENDER"))  return PROT_SYSTEM_CLASS;

    /* USB ルートハブ */
    if (has_hardware_id_prefix(d, L"USB\\ROOT_HUB"))           return PROT_USB_ROOT_HUB;

    if (d->hasClassGuid) {
        if (IsEqualGUID(&d->classGuid, &DNM_GUID_DEVCLASS_SYSTEM) ||
            IsEqualGUID(&d->classGuid, &DNM_GUID_DEVCLASS_COMPUTER) ||
            IsEqualGUID(&d->classGuid, &DNM_GUID_DEVCLASS_PROCESSOR))
            return PROT_SYSTEM_CLASS;

        if (IsEqualGUID(&d->classGuid, &DNM_GUID_DEVCLASS_HDC) ||
            IsEqualGUID(&d->classGuid, &DNM_GUID_DEVCLASS_SCSIADAPTER))
            return PROT_STORAGE_CONTROLLER;

        /* ボリュームは接続中のものだけ保護する。未接続の残骸は整理対象 */
        if (IsEqualGUID(&d->classGuid, &DNM_GUID_DEVCLASS_VOLUME) && d->isPresent)
            return PROT_BOOT_VOLUME;
    }
    return PROT_NONE;
}

static BOOL is_class(const DeviceInfo *d, const GUID *g)
{
    return d->hasClassGuid && IsEqualGUID(&d->classGuid, g);
}

void dnm_apply_protection(DeviceList *list)
{
    int i;
    int keyboards = 0, mice = 0, nets = 0;
    int lastKeyboard = -1, lastMouse = -1, lastNet = -1;

    for (i = 0; i < list->count; i++) {
        DeviceInfo *d = &list->items[i];
        d->protect = classify_single(d);

        if (d->protect != PROT_NONE || !d->isPresent) continue;

        if (is_class(d, &DNM_GUID_DEVCLASS_KEYBOARD)) { keyboards++; lastKeyboard = i; }
        if (is_class(d, &DNM_GUID_DEVCLASS_MOUSE))    { mice++;      lastMouse = i; }
        /* ネットワークは物理アダプターのみ数える。Interface Alias を
         * 持っているものを物理 (= 接続として見えている) とみなす */
        if (is_class(d, &DNM_GUID_DEVCLASS_NET) && d->hasNetConn && d->netAlias[0]) {
            nets++; lastNet = i;
        }
    }

    if (keyboards == 1 && lastKeyboard >= 0)
        list->items[lastKeyboard].protect = PROT_LAST_INPUT;
    if (mice == 1 && lastMouse >= 0)
        list->items[lastMouse].protect = PROT_LAST_INPUT;
    if (nets == 1 && lastNet >= 0)
        list->items[lastNet].protect = PROT_LAST_NETWORK;

    /* 起動ディスク: システムドライブを載せている DiskDrive を保護する。
     * 厳密な対応付けは重いので、接続中の DiskDrive はすべて保護扱いにする。 */
    for (i = 0; i < list->count; i++) {
        DeviceInfo *d = &list->items[i];
        if (d->protect == PROT_NONE && d->isPresent &&
            is_class(d, &DNM_GUID_DEVCLASS_DISKDRIVE))
            d->protect = PROT_BOOT_VOLUME;
    }
}
