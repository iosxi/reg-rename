/*
 * history.c - 操作履歴 / バックアップ (設計書 12 章)
 *
 * %LOCALAPPDATA%\DeviceNameManager\history.json に JSON 配列として追記する。
 * 追記は「末尾の ] を探して上書きする」方式なので、パーサーを持たずに
 * 正しい JSON を保てる。
 */
#include "dnm.h"
#include <shlobj.h>

extern const WCHAR *dnm_multisz_next(const WCHAR *cur);

/* ------------------------------------------------------------------ */
/* UTF-8 変換 + JSON エスケープ                                        */
/* ------------------------------------------------------------------ */
static void json_write_escaped(FILE *f, const WCHAR *s)
{
    char *utf8;
    int need;
    const unsigned char *p;

    if (!s) s = L"";
    need = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
    if (need <= 0) { fputs("\"\"", f); return; }
    utf8 = (char *)malloc((size_t)need);
    if (!utf8) { fputs("\"\"", f); return; }
    WideCharToMultiByte(CP_UTF8, 0, s, -1, utf8, need, NULL, NULL);

    fputc('"', f);
    for (p = (const unsigned char *)utf8; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f);  break;
        case '\r': fputs("\\r", f);  break;
        case '\t': fputs("\\t", f);  break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", *p);
            else           fputc(*p, f);
        }
    }
    fputc('"', f);
    free(utf8);
}

static void json_field(FILE *f, const char *name, const WCHAR *value, BOOL comma)
{
    fprintf(f, "    \"%s\": ", name);
    json_write_escaped(f, value);
    fputs(comma ? ",\n" : "\n", f);
}

/* ISO 8601 (ローカル時刻 + オフセット) */
static void iso_timestamp(WCHAR *buf, size_t cap)
{
    SYSTEMTIME lt;
    TIME_ZONE_INFORMATION tz;
    LONG biasMin;
    int sign;
    DWORD r;

    GetLocalTime(&lt);
    r = GetTimeZoneInformation(&tz);
    biasMin = tz.Bias;
    if (r == TIME_ZONE_ID_DAYLIGHT) biasMin += tz.DaylightBias;
    else if (r == TIME_ZONE_ID_STANDARD) biasMin += tz.StandardBias;

    /* Bias は UTC = ローカル + Bias なので符号が逆 */
    sign = (biasMin <= 0) ? '+' : '-';
    if (biasMin < 0) biasMin = -biasMin;

    _snwprintf(buf, cap, L"%04d-%02d-%02dT%02d:%02d:%02d%c%02ld:%02ld",
               lt.wYear, lt.wMonth, lt.wDay, lt.wHour, lt.wMinute, lt.wSecond,
               sign, (long)(biasMin / 60), (long)(biasMin % 60));
    buf[cap - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* ファイル                                                            */
/* ------------------------------------------------------------------ */
void dnm_history_path(WCHAR *buf, size_t cap)
{
    WCHAR dir[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_LOCAL_APPDATA, NULL, 0, dir)))
        dnm_strcpy(dir, MAX_PATH, L".");
    _snwprintf(buf, cap, L"%s\\DeviceNameManager", dir);
    buf[cap - 1] = 0;
    CreateDirectoryW(buf, NULL);
    wcsncat(buf, L"\\history.json", cap - wcslen(buf) - 1);
}

/*
 * 追記用にファイルを開く。
 * 既存ファイルがあれば末尾の "\n]\n" を削って ",\n" を書ける状態にする。
 * 戻り値が NULL でなければ、呼び出し側はオブジェクト本体を書いて
 * history_close() を呼ぶ。
 */
static FILE *history_open_append(BOOL *firstEntry)
{
    WCHAR path[MAX_PATH];
    FILE *f;

    dnm_history_path(path, MAX_PATH);
    *firstEntry = TRUE;

    f = _wfopen(path, L"r+b");
    if (f) {
        long size;
        fseek(f, 0, SEEK_END);
        size = ftell(f);
        /* 末尾から ']' を探して、その位置に上書きする */
        while (size > 0) {
            int ch;
            fseek(f, size - 1, SEEK_SET);
            ch = fgetc(f);
            if (ch == ']') {
                fseek(f, size - 1, SEEK_SET);
                *firstEntry = FALSE;
                return f;
            }
            if (ch != '\n' && ch != '\r' && ch != ' ' && ch != '\t') break;
            size--;
        }
        /* 壊れている / 空 → 作り直す */
        fclose(f);
    }

    f = _wfopen(path, L"wb");
    if (f) fputs("[\n", f);
    return f;
}

static void history_close(FILE *f)
{
    fputs("]\n", f);
    fclose(f);
}

static void write_common(FILE *f, const DeviceInfo *d)
{
    WCHAR ts[64], guid[64];

    iso_timestamp(ts, 64);
    json_field(f, "timestamp",   ts, TRUE);
    json_field(f, "instanceId",  d->instanceId, TRUE);
    json_field(f, "displayName", d->displayName, TRUE);
    json_field(f, "className",   d->className, TRUE);

    if (d->hasClassGuid) { dnm_guid_to_string(&d->classGuid, guid, 64); }
    else                 { dnm_strcpy(guid, 64, L""); }
    json_field(f, "classGuid", guid, TRUE);

    if (d->hasContainerId) { dnm_guid_to_string(&d->containerId, guid, 64); }
    else                   { dnm_strcpy(guid, 64, L""); }
    json_field(f, "containerId", guid, TRUE);

    json_field(f, "manufacturer", d->manufacturer, TRUE);
    json_field(f, "location",     d->location, TRUE);

    /* Hardware IDs は配列で残す。復元判断の材料になる */
    fputs("    \"hardwareIds\": [", f);
    {
        const WCHAR *p;
        BOOL first = TRUE;
        for (p = d->hardwareIds; p && *p; p = dnm_multisz_next(p)) {
            if (!first) fputs(", ", f);
            json_write_escaped(f, p);
            first = FALSE;
        }
    }
    fputs("],\n", f);

    fprintf(f, "    \"isPresent\": %s,\n", d->isPresent ? "true" : "false");
}

static const char *result_code_name(OpResultCode c)
{
    switch (c) {
    case OPR_OK:              return "ok";
    case OPR_APPLIED_NOT_KEPT:return "applied_not_kept";
    case OPR_SKIPPED:         return "skipped";
    default:                  return "failed";
    }
}

void dnm_history_log_rename(const DeviceInfo *d, const WCHAR *target,
                            const WCHAR *oldName, const WCHAR *newName,
                            const OpResult *res)
{
    BOOL first = TRUE;
    FILE *f = history_open_append(&first);
    if (!f) return;

    if (!first) fputs(",\n", f);
    fputs("  {\n", f);
    fputs("    \"operation\": \"rename\",\n", f);
    json_field(f, "target", target, TRUE);   /* pnp / audio_endpoint / net_alias */
    write_common(f, d);
    json_field(f, "oldFriendlyName", oldName, TRUE);
    json_field(f, "newFriendlyName", newName, TRUE);
    json_field(f, "actualNameAfter", res->actualName, TRUE);
    fprintf(f, "    \"result\": \"%s\"\n", result_code_name(res->code));
    fputs("  }\n", f);

    history_close(f);
}

void dnm_history_log_remove(const DeviceInfo *d, const OpResult *res)
{
    BOOL first = TRUE;
    FILE *f = history_open_append(&first);
    if (!f) return;

    if (!first) fputs(",\n", f);
    fputs("  {\n", f);
    fputs("    \"operation\": \"remove\",\n", f);
    write_common(f, d);
    fprintf(f, "    \"result\": \"%s\"\n", result_code_name(res->code));
    fputs("  }\n", f);

    history_close(f);
}
