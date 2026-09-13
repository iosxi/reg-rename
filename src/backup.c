/*
 * backup.c - 変更前のレジストリキーを .reg に書き出す
 *
 * 名前の変更・デバイスの削除を行う「直前」に、対象のレジストリキーを
 * エクスポートする。書き出したファイルのフルパスは呼び出し側に返し、
 * 結果ダイアログに出す (本人の指定)。
 *
 * エクスポートは reg.exe に任せる。自前でレジストリを走査して .reg の
 * テキスト形式を組み立てることもできるが、REG_MULTI_SZ や REG_EXPAND_SZ の
 * hex(7) / hex(2) 表記、UTF-16LE + BOM、既定値の @= 表記などを取り違えると
 * 「バックアップがあるのに戻せない」という最悪の壊れ方をする。
 * reg.exe はどの Windows にも入っていて、出力はダブルクリックで戻せる。
 *
 * 実測 (2026-09-13): 非管理者のまま以下 3 種のキーで exit=0 になり、
 * 正しい .reg が出力された。
 *   HKLM\SYSTEM\CurrentControlSet\Enum\PCI\...            (14568 バイト)
 *   HKLM\...\Control\Network\{class}\{guid}\Connection    (648 バイト)
 *   HKLM\SOFTWARE\...\MMDevices\Audio\Render              (1405580 バイト)
 */
#include "dnm.h"

#define NET_CLASS_GUID_STR L"{4D36E972-E325-11CE-BFC1-08002BE10318}"

/* ------------------------------------------------------------------ */
/* 対象キーのパスを組み立てる                                          */
/* ------------------------------------------------------------------ */
void dnm_regpath_pnp(const WCHAR *instanceId, WCHAR *buf, size_t cap)
{
    _snwprintf(buf, cap, L"HKLM\\SYSTEM\\CurrentControlSet\\Enum\\%s", instanceId);
    buf[cap - 1] = 0;
}

void dnm_regpath_netconn(const GUID *netCfgInstanceId, WCHAR *buf, size_t cap)
{
    WCHAR g[64];
    dnm_guid_to_string(netCfgInstanceId, g, 64);
    _snwprintf(buf, cap,
               L"HKLM\\SYSTEM\\CurrentControlSet\\Control\\Network\\%s\\%s\\Connection",
               NET_CLASS_GUID_STR, g);
    buf[cap - 1] = 0;
}

/* オーディオエンドポイント ID は "{0.0.0.00000000}.{guid}" の形。
 * レジストリ上は末尾の {guid} が Render / Capture 配下のキー名になる。
 * flow は 0 = 出力 (Render)、1 = 入力 (Capture)。 */
void dnm_regpath_audio(const WCHAR *endpointId, int flow, WCHAR *buf, size_t cap)
{
    const WCHAR *last = wcsrchr(endpointId, L'.');
    const WCHAR *key = (last && last[1]) ? last + 1 : endpointId;
    _snwprintf(buf, cap,
               L"HKLM\\SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\MMDevices\\"
               L"Audio\\%s\\%s",
               flow == 1 ? L"Capture" : L"Render", key);
    buf[cap - 1] = 0;
}

/* ------------------------------------------------------------------ */
/* ファイル名づくり                                                    */
/* ------------------------------------------------------------------ */
/* Instance ID には \ & { } が入る。ファイル名に使えない文字を潰す。
 * \ は # に置き換える (Windows がデバイスインターフェイス名で使う流儀)。 */
static void sanitize(const WCHAR *src, WCHAR *dst, size_t cap)
{
    size_t i = 0;
    for (; src[i] && i + 1 < cap; i++) {
        WCHAR c = src[i];
        if (c == L'\\' || c == L'/')                      dst[i] = L'#';
        else if (c == L':' || c == L'*' || c == L'?' ||
                 c == L'"' || c == L'<' || c == L'>' ||
                 c == L'|')                               dst[i] = L'_';
        else                                              dst[i] = c;
    }
    dst[i] = 0;
}

static void timestamp_name(WCHAR *buf, size_t cap)
{
    SYSTEMTIME st;
    GetLocalTime(&st);
    _snwprintf(buf, cap, L"%04d%02d%02d-%02d%02d%02d",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    buf[cap - 1] = 0;
}

/* 途中のフォルダも含めて作る。既にあれば何もしない。 */
static BOOL ensure_dir(const WCHAR *dir)
{
    WCHAR tmp[MAX_PATH];
    size_t i;

    if (!dir || !dir[0]) return FALSE;
    dnm_strcpy(tmp, MAX_PATH, dir);

    for (i = 0; tmp[i]; i++) {
        if (tmp[i] == L'\\' && i > 2) {
            tmp[i] = 0;
            CreateDirectoryW(tmp, NULL);
            tmp[i] = L'\\';
        }
    }
    if (CreateDirectoryW(tmp, NULL)) return TRUE;
    return GetLastError() == ERROR_ALREADY_EXISTS;
}

/* ------------------------------------------------------------------ */
/* 本体                                                                */
/* ------------------------------------------------------------------ */
BOOL dnm_backup_reg_key(const WCHAR *regPath, const WCHAR *tag, const WCHAR *label,
                        WCHAR *outPath, size_t outCap, WCHAR *errOut, size_t errCap)
{
    Config cfg;
    WCHAR ts[32], safe[240], file[MAX_PATH], cmd[MAX_PATH * 3];
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    DWORD exitCode = (DWORD)-1;

    if (outPath && outCap) outPath[0] = 0;
    if (errOut && errCap)  errOut[0] = 0;

    dnm_config_load(&cfg);
    if (!cfg.backupEnabled) {
        if (errOut) dnm_strcpy(errOut, errCap, L"バックアップは設定で無効になっています。");
        return FALSE;
    }

    if (!ensure_dir(cfg.backupDir)) {
        if (errOut)
            _snwprintf(errOut, errCap,
                       L"バックアップ先フォルダを作成できません: %s", cfg.backupDir);
        return FALSE;
    }

    timestamp_name(ts, 32);
    sanitize(label, safe, 240);
    _snwprintf(file, MAX_PATH, L"%s\\%s_%s_%s.reg", cfg.backupDir, ts, tag, safe);
    file[MAX_PATH - 1] = 0;

    /* reg.exe export "<key>" "<file>" /y
     * キーにもパスにも空白が入りうるので、両方とも引用符で囲む。 */
    _snwprintf(cmd, MAX_PATH * 3, L"reg.exe export \"%s\" \"%s\" /y", regPath, file);
    cmd[MAX_PATH * 3 - 1] = 0;

    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;   /* コンソールを一瞬出さない */
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, CREATE_NO_WINDOW,
                        NULL, NULL, &si, &pi)) {
        if (errOut) {
            WCHAR m[200];
            dnm_format_error(GetLastError(), m, 200);
            _snwprintf(errOut, errCap, L"reg.exe を起動できません: %s", m);
        }
        return FALSE;
    }

    WaitForSingleObject(pi.hProcess, 30000);
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    /* 終了コードだけでは信用しない。実際にファイルができたかを見る
     * (設計書 21 章と同じ考え方)。 */
    if (exitCode != 0 || GetFileAttributesW(file) == INVALID_FILE_ATTRIBUTES) {
        if (errOut)
            _snwprintf(errOut, errCap,
                       L"reg export に失敗しました (終了コード %lu)\nキー: %s",
                       (unsigned long)exitCode, regPath);
        return FALSE;
    }

    if (outPath) dnm_strcpy(outPath, outCap, file);
    return TRUE;
}
