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
#define TOOLBAR_H  32
#define BUTTONS_H  38

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

    dnm_status(L"%d 台中 %d 台を表示  |  %s",
               g_devices.count, shown,
               dnm_is_elevated() ? L"管理者権限で実行中"
                                 : L"管理者権限ではありません (変更は失敗します)");
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
        dnm_remove_device(&g_devices.items[idx], &res);
        dnm_history_log_remove(&g_devices.items[idx], &res);
        MessageBoxW(g_hMain, res.message, L"デバイスを削除",
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
    if (idx < 0) return;

    menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, IDM_CTX_RENAME,  L"デバイス名を変更(&R)...");
    AppendMenuW(menu, MF_STRING, IDM_CTX_CLEANUP, L"旧インスタンスを整理して名前を変更(&C)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING |
                (g_devices.items[idx].protect != PROT_NONE ? MF_GRAYED : 0),
                IDM_CTX_REMOVE, L"デバイスを削除(&D)...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, IDM_CTX_DETAILS, L"詳細(&I)...");
    AppendMenuW(menu, MF_STRING, IDM_CTX_COPYID,  L"Instance ID をコピー(&Y)");

    TrackPopupMenu(menu, TPM_LEFTALIGN | TPM_TOPALIGN | TPM_RIGHTBUTTON,
                   x, y, 0, g_hMain, NULL);
    DestroyMenu(menu);
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

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
    col.pszText = (LPWSTR)L"状態";       col.cx =  44; col.iSubItem = 0;
    ListView_InsertColumn(g_hList, 0, &col);
    col.pszText = (LPWSTR)L"表示名";     col.cx = 260; col.iSubItem = 1;
    ListView_InsertColumn(g_hList, 1, &col);
    col.pszText = (LPWSTR)L"種別";       col.cx =  80; col.iSubItem = 2;
    ListView_InsertColumn(g_hList, 2, &col);
    col.pszText = (LPWSTR)L"接続状態";   col.cx =  70; col.iSubItem = 3;
    ListView_InsertColumn(g_hList, 3, &col);
    col.pszText = (LPWSTR)L"別名 (接続名 / エンドポイント)";
                                         col.cx = 220; col.iSubItem = 4;
    ListView_InsertColumn(g_hList, 4, &col);
    col.pszText = (LPWSTR)L"Instance ID"; col.cx = 380; col.iSubItem = 5;
    ListView_InsertColumn(g_hList, 5, &col);

    g_hStatus = CreateWindowExW(0, STATUSCLASSNAMEW, L"",
                                WS_CHILD | WS_VISIBLE | SBARS_SIZEGRIP,
                                0, 0, 0, 0, g_hMain,
                                (HMENU)(INT_PTR)IDC_STATUSBAR, g_hInst, NULL);
    if (g_hFont) SendMessageW(g_hStatus, WM_SETFONT, (WPARAM)g_hFont, TRUE);
}

static void layout(void)
{
    RECT rc;
    int w, h, statusH = 0, y;
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

    /* 上段: 更新 / 表示フィルター / 種別 / 検索 */
    y = 6;
    MOVE(IDC_BTN_REFRESH,   8, y,  64, 22);
    MOVE(IDC_CHK_PRESENT,  80, y,  70, 22);
    MOVE(IDC_CHK_ABSENT,  154, y,  70, 22);
    MOVE(IDC_CHK_SYSTEM,  228, y, 130, 22);
    MOVE(IDC_LBL_FILTER,  366, y,  36, 22);
    MOVE(IDC_CMB_KIND,    402, y, 130, 200);
    MOVE(IDC_LBL_SEARCH,  544, y,  36, 22);
    MOVE(IDC_EDT_SEARCH,  580, y, (w - 588 > 120 ? w - 588 : 120), 22);

    /* 中段: 一覧 */
    MOVE(IDC_LIST, 8, TOOLBAR_H + 4,
         (w - 16 > 100 ? w - 16 : 100),
         (h - statusH - TOOLBAR_H - BUTTONS_H - 8 > 60
              ? h - statusH - TOOLBAR_H - BUTTONS_H - 8 : 60));

    /* 下段: 操作ボタン */
    y = h - statusH - BUTTONS_H + 6;
    MOVE(IDC_BTN_RENAME,    8, y, 100, 26);
    MOVE(IDC_BTN_CLEANUP, 114, y, 230, 26);
    MOVE(IDC_BTN_REMOVE,  350, y, 120, 26);
    MOVE(IDC_BTN_DETAILS, 476, y,  80, 26);
    MOVE(IDC_BTN_RESCAN,  562, y,  90, 26);
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
        create_children();
        return 0;

    case WM_SIZE:
        layout();
        return 0;

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        mmi->ptMinTrackSize.x = 800;
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
     * 状態はタイトルバーとステータスバーに常時出す。 */
    if (!dnm_is_elevated())
        SetWindowTextW(hwnd,
            L"Windows Device Name Manager  -  管理者権限なし (変更・削除は失敗します)");

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
