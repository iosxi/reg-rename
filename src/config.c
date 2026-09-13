/*
 * config.c - 設定ファイルの読み書き
 *
 * 設定ファイルは実行ファイルと同じフォルダに置く (本人の指定)。
 *   <exe のあるフォルダ>\DeviceNameManager.ini
 *
 * 形式は INI。GetPrivateProfileStringW / WritePrivateProfileStringW を
 * そのまま使えるので、パーサーを自前で持たない。
 * ただしこれらの API は「パスにファイル名だけを書くと Windows フォルダを
 * 見に行く」ため、必ず絶対パスを渡すこと。
 */
#include "dnm.h"

#define CFG_SECTION_BACKUP  L"Backup"

/* 実行ファイルのあるフォルダ。末尾に \ は付けない。 */
void dnm_exe_dir(WCHAR *buf, size_t cap)
{
    WCHAR path[MAX_PATH];
    WCHAR *slash;
    DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);

    if (n == 0 || n >= MAX_PATH) { dnm_strcpy(buf, cap, L"."); return; }

    slash = wcsrchr(path, L'\\');
    if (slash) *slash = 0;
    dnm_strcpy(buf, cap, path);
}

void dnm_config_path(WCHAR *buf, size_t cap)
{
    WCHAR dir[MAX_PATH];
    dnm_exe_dir(dir, MAX_PATH);
    _snwprintf(buf, cap, L"%s\\DeviceNameManager.ini", dir);
    buf[cap - 1] = 0;
}

/* 既定のバックアップ先。設定が空のときにこれを使う。 */
void dnm_default_backup_dir(WCHAR *buf, size_t cap)
{
    WCHAR dir[MAX_PATH];
    dnm_exe_dir(dir, MAX_PATH);
    _snwprintf(buf, cap, L"%s\\RegBackup", dir);
    buf[cap - 1] = 0;
}

void dnm_config_load(Config *c)
{
    WCHAR ini[MAX_PATH];
    ZeroMemory(c, sizeof(*c));
    dnm_config_path(ini, MAX_PATH);

    c->backupEnabled =
        GetPrivateProfileIntW(CFG_SECTION_BACKUP, L"Enabled", 1, ini) ? TRUE : FALSE;

    GetPrivateProfileStringW(CFG_SECTION_BACKUP, L"Dir", L"",
                             c->backupDir, MAX_PATH, ini);
    if (c->backupDir[0] == 0)
        dnm_default_backup_dir(c->backupDir, MAX_PATH);
}

BOOL dnm_config_save(const Config *c)
{
    WCHAR ini[MAX_PATH];
    BOOL ok = TRUE;
    dnm_config_path(ini, MAX_PATH);

    if (!WritePrivateProfileStringW(CFG_SECTION_BACKUP, L"Enabled",
                                    c->backupEnabled ? L"1" : L"0", ini))
        ok = FALSE;
    if (!WritePrivateProfileStringW(CFG_SECTION_BACKUP, L"Dir", c->backupDir, ini))
        ok = FALSE;

    /* 書き込みはキャッシュされることがあるので明示的に流す */
    WritePrivateProfileStringW(NULL, NULL, NULL, ini);
    return ok;
}
