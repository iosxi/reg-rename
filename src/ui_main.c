/*
 * ui_main.c - メインウィンドウ (設計書 3.1)
 *
 * 一覧は DeviceList をそのまま持ち、表示フィルターを通した結果を
 * ListView に流し込む。ListView の行と DeviceList の添字の対応は
 * g_rowToIndex で持つ。操作対象は必ず Instance ID で特定する。
 */
#include "dnm.h"
#include "resource.h"
#include <dbt.h>
#include <objbase.h>   /* CoInitializeEx */

HINSTANCE g_hInst = NULL;

static HWND g_hMain     = NULL;
static HWND g_hList     = NULL;
static HWND g_hStatus   = NULL;
static HFONT g_hFont    = NULL;

static DeviceList  g_devices;
static EnumOptions g_opt;
static int        *g_rowToIndex = NULL;
static int         g_rowCount   = 0;

#define TIMER_DEVCHANGE 1

static const WCHAR *kKindItems[] = {
    L"すべての種別", L"PnP", L"Audio", L"Net", L"USB",
    L"Bluetooth", L"HID", L"Storage", L"Display", L"Other"
};
/* コンボの添字 → DeviceKind (0 番目は「すべて」= -1) */
static const int kKindValues[] = {
    -1, DK_GenericPnP, DK_AudioEndpoint, DK_NetworkAdapter, DK_Usb,
    DK_Bluetooth, DK_Hid, DK_Storage, DK_Display, DK_Other
};

/* ------------------------------------------------------------------ */
/* ステータスバー                                                      */
/* ------------------------------------------------------------------ */
void dnm_status(const WCHAR *fmt, ...)
{
    WCHAR buf[512];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf(buf, 512, fmt, ap);
    va_end(ap);
    buf[511] = 0;
    if (g_hStatus) SendMessageW(g_hStatus, SB_SETTEXTW, 0, (LPARAM)buf);
}

/* ------------------------------------------------------------------ */
/* 一覧の再構築                                                        */
/* ------------------------------------------------------------------ */
static void read_filter_state(void)
{
    int sel;
    g_opt.showPresent = (IsDlgButtonChecked(g_hMain, IDC_CHK_PRESENT) == BST_CHECKED);
    g_opt.showAbsent  = (IsDlgButtonChecked(g_hMain, IDC_CHK_ABSENT)  == BST_CHECKED);
    g_opt.showSystem  = (IsDlgButtonChecked(g_hMain, IDC_CHK_SYSTEM)  == BST_CHECKED);

    sel = (int)SendDlgItemMessageW(g_hMain, IDC_CMB_KIND, CB_GETCURSEL, 0, 0);
    if (sel < 0 || sel >= (int)(sizeof(kKindValues) / sizeof(kKindValues[0]))) sel = 0;
    g_opt.kindFilter = kKindValues[sel];

    GetDlgItemTextW(g_hMain, IDC_EDT_SEARCH, g_opt.search, 128);
}

static void refill_list(void)
{
    int i, row = 0, shown = 0;

    read_filter_state();

    SendMessageW(g_hList, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(g_hList);

    free(g_rowToIndex);
    g_rowToIndex = (int *)calloc((size_t)(g_devices.count > 0 ? g_devices.count : 1),
                                 sizeof(int));
    g_rowCount = 0;

    for (i = 0; i < g_devices.count; i++) {
        const DeviceInfo *d = &g_devices.items[i];
        LVITEMW it;
        WCHAR mark[8];

        if (!dnm_device_matches(d, &g_opt)) continue;

        /* ● = 接続中、○ = 未接続 (設計書 3.1 のモック) */
        dnm_strcpy(mark, 8, d->isPresent ? L"●" : L"○");

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = row;
        it.pszText = mark;
        it.lParam = i;
        ListView_InsertItem(g_hList, &it);

        ListView_SetItemText(g_hList, row, 1, (LPWSTR)d->displayName);
        ListView_SetItemText(g_hList, row, 2, (LPWSTR)dnm_kind_name(d->kind));
        ListView_SetItemText(g_hList, row, 3,
                             (LPWSTR)(d->isPresent ? L"接続中" : L"未接続"));
        ListView_SetItemText(g_hList, row, 4,
                             (LPWSTR)(d->netAlias[0] ? d->netAlias
                                    : (d->audioCount > 0 ? d->audio[0].friendlyName
                                                         : L"")));
        ListView_SetItemText(g_hList, row, 5, (LPWSTR)d->instanceId);

        g_rowToIndex[g_rowCount++] = i;
        row++;
        shown++;
    }

    SendMessageW(g_hList, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(g_hList, NULL, TRUE);

    {
        WCHAR priv[160];
        dnm_elevation_status_text(priv, 160);
        dnm_status(L"%d 台中 %d 台を表示  |  %s", g_devices.count, shown, priv);
    }
}

static void reload_devices(void)
{
    dnm_status(L"デバイスを列挙しています...");
    dnm_enumerate(&g_devices);
    refill_list();
}

/* 選択行 → DeviceList の添字 */
static int selected_index(void)
{
    int row = ListView_GetNextItem(g_hList, -1, LVNI_SELECTED);
    if (row < 0 || row >= g_rowCount) return -1;
    return g_rowToIndex[row];
}

/* 選択を Instance ID で復元する (再列挙で順番が変わるため) */
static void select_by_instance_id(const WCHAR *instanceId)
{
    int i;
    for (i = 0; i < g_rowCount; i++) {
        const DeviceInfo *d = &g_devices.items[g_rowToIndex[i]];
        if (_wcsicmp(d->instanceId, instanceId) == 0) {
            ListView_SetItemState(g_hList, i, LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
            ListView_EnsureVisible(g_hList, i, FALSE);
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/* コマンド                                                            */
/* ------------------------------------------------------------------ */
static void cmd_rename(void)
{
    int idx = selected_index();
    WCHAR keep[MAX_DEVICE_ID_LEN];
    if (idx < 0) { dnm_status(L"デバイスを選択してください。"); return; }

    dnm_strcpy(keep, MAX_DEVICE_ID_LEN, g_devices.items[idx].instanceId);
    if (dnm_dlg_rename(g_hMain, &g_devices.items[idx])) {
        reload_devices();
        select_by_instance_id(keep);
    }
}

static void cmd_remove(void)
{
    int idx = selected_index();
    WCHAR keep[MAX_DEVICE_ID_LEN];
    if (idx < 0) { dnm_status(L"デバイスを選択してください。"); return; }

    dnm_strcpy(keep, MAX_DEVICE_ID_LEN, g_devices.items[idx].instanceId);
    if (dnm_dlg_remove_confirm(g_hMain, &g_devices.items[idx])) {
        OpResult res;
        WCHAR body[4096];
        dnm_remove_device(&g_devices.items[idx], &res);
        dnm_history_log_remove(&g_devices.items[idx], &res);
        /* 書き出したバックアップのパスも添える (本人の指定) */
        if (res.backupInfo[0])
            _snwprintf(body, 4096,
                       L"%s\n\n--- 削除前のレジストリを書き出しました ---\n%s",
                       res.message, res.backupInfo);
        else
            dnm_strcpy(body, 4096, res.message);
        body[4095] = 0;
        MessageBoxW(g_hMain, body, L"デバイスを削除",
                    MB_OK | (res.code == OPR_OK ? MB_ICONINFORMATION : MB_ICONWARNING));
        reload_devices();
        select_by_instance_id(keep);
    }
}

static void cmd_cleanup(void)
{
    int idx = selected_index();
    WCHAR keep[MAX_DEVICE_ID_LEN];
    if (idx < 0) { dnm_status(L"デバイスを選択してください。"); return; }

    dnm_strcpy(keep, MAX_DEVICE_ID_LEN, g_devices.items[idx].instanceId);
    if (dnm_dlg_cleanup(g_hMain, &g_devices, idx)) {
        reload_devices();
        select_by_instance_id(keep);
    }
}

static void cmd_details(void)
{
    int idx = selected_index();
    if (idx < 0) { dnm_status(L"デバイスを選択してください。"); return; }
    dnm_dlg_details(g_hMain, &g_devices.items[idx]);
}

static void cmd_copy_id(void)
{
    int idx = selected_index();
    const WCHAR *id;
    size_t bytes;
    HGLOBAL mem;

    if (idx < 0) return;
    id = g_devices.items[idx].instanceId;
    bytes = (wcslen(id) + 1) * sizeof(WCHAR);

    mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!mem) return;
    memcpy(GlobalLock(mem), id, bytes);
    GlobalUnlock(mem);

    if (OpenClipboard(g_hMain)) {
        EmptyClipboard();
        SetClipboardData(CF_UNICODETEXT, mem);
        CloseClipboard();
        dnm_status(L"Instance ID をコピーしました。");
    } else {
        GlobalFree(mem);
    }
}

/* 権限の診断。
 * 「管理者権限で実行中」と出ているのが正しいかを、利用者自身が
 * 確かめられるようにする。表示の根拠を全部並べる。 */
static void cmd_privilege_info(void)
{
    WCHAR report[600];
    WCHAR body[900];
    dnm_privilege_report(report, 600);
    _snwprintf(body, 900,
               L"%s\n\n"
               L"マニフェストが requireAdministrator なら、ダブルクリックで\n"
               L"起動しても Windows が昇格させます。UAC の確認画面が出るか\n"
               L"どうかは ConsentPromptBehaviorAdmin で決まります。",
               report);
    body[899] = 0;
    MessageBoxW(g_hMain, body, L"権限の診断", MB_OK | MB_ICONINFORMATION);
}

/* オーディオの連番を解消する。実行後は状態が変わるので一覧を取り直す。 */
static void cmd_audio_fix(void)
{
    int idx = selected_index();
    WCHAR keep[MAX_DEVICE_ID_LEN];
    if (idx < 0) { dnm_status(L"デバイスを選択してください。"); return; }

    dnm_strcpy(keep, MAX_DEVICE_ID_LEN, g_devices.items[idx].instanceId);
    dnm_dlg_fix_audio_serial(g_hMain, &g_devices.items[idx]);
    reload_devices();
    select_by_instance_id(keep);
}

static void cmd_rescan(void)
{
    dnm_status(L"デバイスを再スキャンしています...");
    dnm_rescan_devices();
    Sleep(300);
    reload_devices();
}

/* ------------------------------------------------------------------ */
/* コンテキストメニュー (設計書 9.1)                                   */
/* ------------------------------------------------------------------ */
static void show_context_menu(int x, int y)
{
    HMENU menu;
    int idx = selected_index();

    /* 選択が無くても権限診断だけは出せるようにする。
     * 「管理者権限で実行中」の表示が正しいかを確かめたいとき、
     * デバイスを選ばせる必然性が無い。 */
    if (idx < 0) {
        menu = CreatePopupMenu();
        AppendMenuW(menu, MF_STRING, IDM_CTX_PRIVINFO, L"権限の診断(&P)...");
        AppendMenuW(menu, MF_STRING, IDM_CTX_NAMEHELP, L"名前のしくみ(&H)...");
        AppendMenuW(menu, MF_STRING, IDM_CTX_SETTINGS, L"設定(&S)...");
        TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                       x, y, 0, g_hMain, NULL);
        DestroyMenu(menu);
        return;
    }

    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_CTX_RENAME,  L"デバイス名を変更(&R)...");
    AppendMenuW(menu, MF_STRING, IDM_CTX_CLEANUP, L"旧インスタンスを整理して名前を変更(&C)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING |
                (g_devices.items[idx].protect != PROT_NONE ? MF_GRAYED : 0),
                IDM_CTX_REMOVE, L"デバイスを削除(&D)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (g_devices.items[idx].audioCount > 0) {
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_STRING |
                    (dnm_audio_has_serial(&g_devices.items[idx]) ? 0 : MF_GRAYED),
                    IDM_CTX_AUDIOFIX,
                    L"オーディオの連番を解消(&N)...");
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_CTX_DETAILS, L"詳細(&I)...");
    AppendMenuW(menu, MF_STRING, IDM_CTX_COPYID,  L"Instance ID をコピー(&Y)");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_CTX_PRIVINFO, L"権限の診断(&P)...");
    AppendMenuW(menu, MF_STRING, IDM_CTX_NAMEHELP, L"名前のしくみ(&H)...");
    AppendMenuW(menu, MF_STRING, IDM_CTX_SETTINGS, L"設定(&S)...");

    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                   x, y, 0, g_hMain, NULL);
    DestroyMenu(menu);
}

/* ------------------------------------------------------------------ */
/* 文字の実測                                                          */
/*                                                                     */
/* フォントは SPI_GETNONCLIENTMETRICS から取るので、DPI や「テキストの   */
/* サイズ」の設定で大きくなる。にもかかわらず座標と幅をピクセルで       */
/* 決め打ちしていたため、フォントが大きい環境ではラベルが途切れ、       */
/* ボタンの文字の下が切れ、隣のコントロールと重なっていた。            */
/* 幅も高さも、表示する文字を実測してから決める。                      */
/* ------------------------------------------------------------------ */
static int g_textH   = 16;  /* 1 行の文字の高さ */
static int g_ctrlH   = 26;  /* ボタン・入力欄の高さ */
static int g_toolbarH = 32; /* 上段の占める高さ */
static int g_buttonsH = 38; /* 下段の占める高さ */

/* 文字列の描画幅 (ピクセル)。g_hFont で測る。 */
static int text_width(const WCHAR *s)
{
    HDC dc = GetDC(g_hMain);
    HFONT old = NULL;
    SIZE sz;
    int w = 0;

    if (!dc) return (int)wcslen(s) * 12;
    if (g_hFont) old = (HFONT)SelectObject(dc, g_hFont);
    if (GetTextExtentPoint32W(dc, s, (int)wcslen(s), &sz)) w = sz.cx;
    if (old) SelectObject(dc, old);
    ReleaseDC(g_hMain, dc);
    return w;
}

/* コントロールに入っている文字から必要幅を出す。
 * pad は種類ごとの余白 (枠、チェックボックスの四角、コンボの矢印)。 */
static int ctrl_width(int id, int pad)
{
    WCHAR buf[256];
    buf[0] = 0;
    GetDlgItemTextW(g_hMain, id, buf, 256);
    return text_width(buf) + pad;
}

static void measure_metrics(void)
{
    HDC dc = GetDC(g_hMain);
    TEXTMETRICW tm;
    HFONT old = NULL;

    if (dc) {
        if (g_hFont) old = (HFONT)SelectObject(dc, g_hFont);
        if (GetTextMetricsW(dc, &tm))
            g_textH = tm.tmHeight;
        if (old) SelectObject(dc, old);
        ReleaseDC(g_hMain, dc);
    }

    /* 文字の高さ + 上下の余白。これを下回るとボタンの文字の下が切れる。 */
    g_ctrlH    = g_textH + 12;
    if (g_ctrlH < 24) g_ctrlH = 24;
    g_toolbarH = g_ctrlH + 12;
    g_buttonsH = g_ctrlH + 14;
}

/* ------------------------------------------------------------------ */
/* ウィンドウ生成                                                      */
/* ------------------------------------------------------------------ */
static HWND mk(const WCHAR *cls, const WCHAR *text, DWORD style, int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style,
                             0, 0, 10, 10, g_hMain, (HMENU)(INT_PTR)id,
                             g_hInst, NULL);
    if (h && g_hFont) SendMessageW(h, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    return h;
}

static void create_children(void)
{
    LVCOLUMNW col;
    int i;

    mk(L"BUTTON", L"更新", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_REFRESH);
    mk(L"BUTTON", L"接続中", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_PRESENT);
    mk(L"BUTTON", L"未接続", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_ABSENT);
    mk(L"BUTTON", L"システムデバイス", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_CHK_SYSTEM);
    mk(L"STATIC", L"種別:", SS_CENTERIMAGE, IDC_LBL_FILTER);
    mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_CMB_KIND);
    mk(L"STATIC", L"検索:", SS_CENTERIMAGE, IDC_LBL_SEARCH);
    mk(L"EDIT", L"", WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_EDT_SEARCH);

    mk(L"BUTTON", L"名前を変更...", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_RENAME);
    mk(L"BUTTON", L"旧インスタンスを整理して名前を変更...",
       BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_CLEANUP);
    mk(L"BUTTON", L"デバイスを削除...", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_REMOVE);
    mk(L"BUTTON", L"詳細...", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_DETAILS);
    mk(L"BUTTON", L"再スキャン", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_RESCAN);
    mk(L"BUTTON", L"名前のしくみ...", BS_PUSHBUTTON | WS_TABSTOP,
       IDC_BTN_NAMEHELP);
    mk(L"BUTTON", L"設定...", BS_PUSHBUTTON | WS_TABSTOP, IDC_BTN_SETTINGS);

    for (i = 0; i < (int)(sizeof(kKindItems) / sizeof(kKindItems[0])); i++)
        SendDlgItemMessageW(g_hMain, IDC_CMB_KIND, CB_ADDSTRING, 0,
                            (LPARAM)kKindItems[i]);
    SendDlgItemMessageW(g_hMain, IDC_CMB_KIND, CB_SETCURSEL, 0, 0);

    /* 初期表示は接続中のみ (設計書 4.2) */
    CheckDlgButton(g_hMain, IDC_CHK_PRESENT, BST_CHECKED);
    CheckDlgButton(g_hMain, IDC_CHK_ABSENT,  BST_UNCHECKED);
    CheckDlgButton(g_hMain, IDC_CHK_SYSTEM,  BST_UNCHECKED);

    g_hList = CreateWindowExW(0, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_BORDER | WS_TABSTOP |
                              LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              0, 0, 10, 10, g_hMain, (HMENU)(INT_PTR)IDC_LIST,
                              g_hInst, NULL);
    if (g_hFont) SendMessageW(g_hList, WM_SETFONT, (WPARAM)g_hFont, TRUE);
    ListView_SetExtendedListViewStyle(g_hList,
        LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES | LVS_EX_DOUBLEBUFFER);

    /* 列幅も決め打ちだと見出しが「状...」「接続状...」と欠ける。
     * 見出しの実測幅を下限にして、そこに中身のぶんの余裕を足す。 */
    {
        static const WCHAR *kCols[] = {
            L"状態", L"表示名", L"種別", L"接続状態",
            L"別名 (接続名 / エンドポイント)", L"Instance ID"
        };
        static const int kMin[] = { 44, 260, 80, 70, 220, 380 };
        int i;

        ZeroMemory(&col, sizeof(col));
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        for (i = 0; i < (int)(sizeof(kCols) / sizeof(kCols[0])); i++) {
            /* ソートの矢印と余白のぶん 24px 足す */
            int need = text_width(kCols[i]) + 24;
            col.pszText  = (LPWSTR)kCols[i];
            col.cx       = need > kMin[i] ? need : kMin[i];
            col.iSubItem = i;
            ListView_InsertColumn(g_hList, i, &col);
        }
    }

    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                0, 0, 0, 0, g_hMain,
                                (HMENU)(INT_PTR)IDC_STATUSBAR, g_hInst, NULL);
    if (g_hFont) SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

/* コントロール同士の間隔 */
#define GAP 8

/* 下段のボタンを全部並べるのに要る幅。ウィンドウの下限に使う。
 * 上段は検索欄が伸縮するので、下段のほうが常に厳しい。 */
static int min_client_width(void)
{
    static const int kBtns[] = {
        IDC_BTN_RENAME, IDC_BTN_CLEANUP, IDC_BTN_REMOVE,
        IDC_BTN_DETAILS, IDC_BTN_RESCAN, IDC_BTN_NAMEHELP,
        IDC_BTN_SETTINGS
    };
    int i, total = 8 + 8;
    if (!g_hMain || !GetDlgItem(g_hMain, IDC_BTN_RENAME)) return 0;
    for (i = 0; i < (int)(sizeof(kBtns) / sizeof(kBtns[0])); i++)
        total += ctrl_width(kBtns[i], 30) + GAP;
    return total;
}

static void layout(void)
{
    RECT rc;
    int w, h, statusH = 0, x, y;
    HDWP dwp;

    GetClientRect(g_hMain, &rc);
    w = rc.right;
    h = rc.bottom;

    if (g_hStatus) {
        RECT sr;
        SendMessageW(g_hStatus, WM_SIZE, 0, 0);
        GetWindowRect(g_hStatus, &sr);
        statusH = sr.bottom - sr.top;
    }

    dwp = BeginDeferWindowPos(16);
#define MOVE(id, x_, y_, cx, cy) \
    dwp = DeferWindowPos(dwp, GetDlgItem(g_hMain, id), NULL, x_, y_, cx, cy, \
                         SWP_NOZORDER | SWP_NOACTIVATE)

    /* 上段: 更新 / 表示フィルター / 種別 / 検索。
     * 幅はすべて実測から出し、左から詰めて置く。座標は決め打ちしない。 */
    {
        /* チェックボックスの四角と文字の間隔。SM_CXMENUCHECK は DPI に
         * 追従するが、下限を置かないと小さすぎる環境がある。 */
        int boxW = GetSystemMetrics(SM_CXMENUCHECK);
        int cmbW, i, itemW = 0;
        if (boxW < 16) boxW = 16;

        y = (g_toolbarH - g_ctrlH) / 2;
        x = 8;

        MOVE(IDC_BTN_REFRESH, x, y, ctrl_width(IDC_BTN_REFRESH, 28), g_ctrlH);
        x += ctrl_width(IDC_BTN_REFRESH, 28) + GAP;

        MOVE(IDC_CHK_PRESENT, x, y, ctrl_width(IDC_CHK_PRESENT, boxW + 10), g_ctrlH);
        x += ctrl_width(IDC_CHK_PRESENT, boxW + 10) + GAP;

        MOVE(IDC_CHK_ABSENT, x, y, ctrl_width(IDC_CHK_ABSENT, boxW + 10), g_ctrlH);
        x += ctrl_width(IDC_CHK_ABSENT, boxW + 10) + GAP;

        MOVE(IDC_CHK_SYSTEM, x, y, ctrl_width(IDC_CHK_SYSTEM, boxW + 10), g_ctrlH);
        x += ctrl_width(IDC_CHK_SYSTEM, boxW + 10) + GAP * 2;

        MOVE(IDC_LBL_FILTER, x, y, ctrl_width(IDC_LBL_FILTER, 6), g_ctrlH);
        x += ctrl_width(IDC_LBL_FILTER, 6) + 4;

        /* コンボは一番長い項目が収まる幅にする */
        for (i = 0; i < (int)(sizeof(kKindItems) / sizeof(kKindItems[0])); i++) {
            int t = text_width(kKindItems[i]);
            if (t > itemW) itemW = t;
        }
        cmbW = itemW + GetSystemMetrics(SM_CXVSCROLL) + 16;
        /* 高さはドロップダウンを開いたときの一覧の高さも兼ねる */
        MOVE(IDC_CMB_KIND, x, y, cmbW, g_ctrlH + 200);
        x += cmbW + GAP * 2;

        MOVE(IDC_LBL_SEARCH, x, y, ctrl_width(IDC_LBL_SEARCH, 6), g_ctrlH);
        x += ctrl_width(IDC_LBL_SEARCH, 6) + 4;

        MOVE(IDC_EDT_SEARCH, x, y, (w - x - 8 > 120 ? w - x - 8 : 120), g_ctrlH);
    }

    /* 中段: 一覧 */
    MOVE(IDC_LIST, 8, g_toolbarH + 4,
         (w - 16 > 100 ? w - 16 : 100),
         (h - statusH - g_toolbarH - g_buttonsH - 8 > 60
              ? h - statusH - g_toolbarH - g_buttonsH - 8 : 60));

    /* 下段: 操作ボタン。ここも文字幅から出す。
     * 「旧インスタンスを整理して名前を変更...」は特に長く、
     * 決め打ちの 230px では先頭と末尾が欠けていた。 */
    {
        static const int kBtns[] = {
            IDC_BTN_RENAME, IDC_BTN_CLEANUP, IDC_BTN_REMOVE,
            IDC_BTN_DETAILS, IDC_BTN_RESCAN, IDC_BTN_NAMEHELP,
            IDC_BTN_SETTINGS
        };
        int i;
        y = h - statusH - g_buttonsH + (g_buttonsH - g_ctrlH) / 2;
        x = 8;
        for (i = 0; i < (int)(sizeof(kBtns) / sizeof(kBtns[0])); i++) {
            int bw = ctrl_width(kBtns[i], 30);
            MOVE(kBtns[i], x, y, bw, g_ctrlH);
            x += bw + GAP;
        }
    }
#undef MOVE
    EndDeferWindowPos(dwp);
}

/* ------------------------------------------------------------------ */
/* ウィンドウプロシージャ                                              */
/* ------------------------------------------------------------------ */
static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_CREATE:
        g_hMain = hwnd;
        measure_metrics();   /* 幅と高さの基準。create_children より先 */
        create_children();
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    /* 実測した幅の合計より狭くできないようにする。
     * これが無いと、フォントの大きい環境でウィンドウを縮めたときに
     * 下段のボタンが右端からはみ出して見えなくなる。 */
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        int need = min_client_width();
        RECT r;
        if (need > 0) {
            r.left = 0; r.top = 0; r.right = need; r.bottom = 300;
            AdjustWindowRect(&r, (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE), FALSE);
            mmi->ptMinTrackSize.x = r.right - r.left;
        } else {
            mmi->ptMinTrackSize.x = 800;
        }
        mmi->ptMinTrackSize.y = 420;
        return 0;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_BTN_REFRESH: reload_devices(); return 0;
        case IDC_BTN_RENAME:
        case IDM_CTX_RENAME:  cmd_rename();  return 0;
        case IDC_BTN_REMOVE:
        case IDM_CTX_REMOVE:  cmd_remove();  return 0;
        case IDC_BTN_CLEANUP:
        case IDM_CTX_CLEANUP: cmd_cleanup(); return 0;
        case IDC_BTN_DETAILS:
        case IDM_CTX_DETAILS: cmd_details(); return 0;
        case IDM_CTX_COPYID:  cmd_copy_id(); return 0;
        case IDM_CTX_PRIVINFO: cmd_privilege_info(); return 0;
        case IDM_CTX_AUDIOFIX: cmd_audio_fix(); return 0;
        case IDM_CTX_NAMEHELP:
        case IDC_BTN_NAMEHELP: dnm_dlg_name_mechanics(g_hMain); return 0;
        case IDM_CTX_SETTINGS:
        case IDC_BTN_SETTINGS: dnm_dlg_settings(g_hMain); return 0;
        case IDC_BTN_RESCAN:  cmd_rescan();  return 0;

        case IDC_CHK_PRESENT:
        case IDC_CHK_ABSENT:
        case IDC_CHK_SYSTEM:
            refill_list();
            return 0;

        case IDC_CMB_KIND:
            if (HIWORD(wp) == CBN_SELCHANGE) refill_list();
            return 0;

        case IDC_EDT_SEARCH:
            if (HIWORD(wp) == EN_CHANGE) refill_list();
            return 0;
        }
        return 0;

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == IDC_LIST) {
            if (nh->code == NM_DBLCLK)   { cmd_details(); return 0; }
            if (nh->code == NM_RCLICK) {
                POINT pt;
                GetCursorPos(&pt);
                show_context_menu(pt.x, pt.y);
                return 0;
            }
        }
        break;
    }

    /* 設計書 14 章: USB の抜き差しで一覧を自動更新する。
     * 抜き差し中は通知が連続するのでタイマーで間引く。 */
    case WM_DEVICECHANGE:
        if (wp == DBT_DEVNODES_CHANGED)
            SetTimer(hwnd, TIMER_DEVCHANGE, 1200, NULL);
        return TRUE;

    case WM_TIMER:
        if (wp == TIMER_DEVCHANGE) {
            KillTimer(hwnd, TIMER_DEVCHANGE);
            {
                int idx = selected_index();
                WCHAR keep[MAX_DEVICE_ID_LEN];
                keep[0] = 0;
                if (idx >= 0) dnm_strcpy(keep, MAX_DEVICE_ID_LEN,
                                         g_devices.items[idx].instanceId);
                reload_devices();
                if (keep[0]) select_by_instance_id(keep);
            }
        }
        return 0;

    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ------------------------------------------------------------------ */
/* エントリポイント                                                    */
/* ------------------------------------------------------------------ */
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE hPrev, LPWSTR cmdLine, int nShow)
{
    WNDCLASSEXW wc;
    INITCOMMONCONTROLSEX icc;
    NONCLIENTMETRICSW ncm;
    MSG msg;
    HWND hwnd;

    (void)hPrev; (void)cmdLine;
    g_hInst = hInst;

    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES | ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    /* システムの UI フォント (日本語環境なら Yu Gothic UI / Meiryo UI) */
    ZeroMemory(&ncm, sizeof(ncm));
    ncm.cbSize = sizeof(ncm);
    if (SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0))
        g_hFont = CreateFontIndirectW(&ncm.lfMessageFont);

    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"DnmMainWindow";
    wc.hIcon         = LoadIconW(NULL, IDI_APPLICATION);
    wc.hIconSm       = LoadIconW(NULL, IDI_APPLICATION);
    if (!RegisterClassExW(&wc)) return 1;

    dnm_list_init(&g_devices);

    hwnd = CreateWindowExW(0, wc.lpszClassName,
                           L"Windows Device Name Manager",
                           WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT, CW_USEDEFAULT, 1160, 640,
                           NULL, NULL, hInst, NULL);
    if (!hwnd) return 1;

    /* 権限の警告はモーダルにしない。
     * 配布版は app.manifest で requireAdministrator にしてあるので、
     * 通常運用ではこの警告が出ること自体が無い。出るのは UI 確認ビルドか、
     * 昇格をキャンセルされた場合だけで、そこで操作を止める必要はない。
     * 状態はタイトルバーとステータスバーに常時出す。
     *
     * 昇格している側も明示する。UAC が無音昇格の設定だと、ダブルクリック
     * しただけでも昇格するため、黙っていると「昇格していないはずなのに
     * 管理者と出る」と見える。 */
    {
        ElevationInfo ei;
        WCHAR title[256];
        dnm_get_elevation(&ei);
        _snwprintf(title, 256,
                   ei.elevated
                     ? L"Windows Device Name Manager  -  管理者権限あり (整合性レベル: %s)"
                     : L"Windows Device Name Manager  -  管理者権限なし"
                       L" (整合性レベル: %s / 変更・削除は失敗します)",
                   dnm_integrity_text(ei.integrityRid));
        title[255] = 0;
        SetWindowTextW(hwnd, title);
    }

    ShowWindow(hwnd, nShow);
    UpdateWindow(hwnd);
    reload_devices();

    while (GetMessageW(&msg, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    free(g_rowToIndex);
    dnm_list_free(&g_devices);
    if (g_hFont) DeleteObject(g_hFont);
    CoUninitialize();
    return (int)msg.wParam;
}
