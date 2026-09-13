/* resource.h - コントロール ID */
#ifndef DNM_RESOURCE_H
#define DNM_RESOURCE_H

#define IDI_APPICON             100

/* メインウィンドウ上の子コントロール */
#define IDC_LIST                1000
#define IDC_BTN_REFRESH         1001
#define IDC_CHK_PRESENT         1002
#define IDC_CHK_ABSENT          1003
#define IDC_CHK_SYSTEM          1004
#define IDC_CMB_KIND            1005
#define IDC_EDT_SEARCH          1006
#define IDC_BTN_RENAME          1007
#define IDC_BTN_REMOVE          1008
#define IDC_BTN_CLEANUP         1009
#define IDC_BTN_DETAILS         1010
#define IDC_BTN_RESCAN          1011
#define IDC_STATUSBAR           1012
#define IDC_LBL_FILTER          1013
#define IDC_LBL_SEARCH          1014
#define IDC_BTN_SETTINGS        1015

/* コンテキストメニュー (設計書 9.1) */
#define IDM_CTX_RENAME          1100
#define IDM_CTX_CLEANUP         1101
#define IDM_CTX_REMOVE          1102
#define IDM_CTX_DETAILS         1103
#define IDM_CTX_COPYID          1104
#define IDM_CTX_PRIVINFO        1105
#define IDM_CTX_SETTINGS        1106
#define IDM_CTX_AUDIOFIX        1107

/* 接続名の競合を伝える確認ダイアログ */
#define IDD_CONFLICT            205
#define IDC_CF_TEXT             2500

/* 設定ダイアログ (バックアップ先の指定) */
#define IDD_SETTINGS            204
#define IDC_ST_LBL_DIR          2400
#define IDC_ST_EDIT_DIR         2401
#define IDC_ST_BROWSE           2402
#define IDC_ST_CHK_ENABLE       2403
#define IDC_ST_HINT             2404
#define IDC_ST_LBL_INI          2405

/* 名前を変更ダイアログ (設計書 3.2 / 7.2) */
#define IDD_RENAME              200
#define IDC_RN_TARGET           2001
#define IDC_RN_KIND             2002
#define IDC_RN_LBL_PNP          2003
#define IDC_RN_EDIT_PNP         2004
#define IDC_RN_BASE             2005
#define IDC_RN_LBL_A            2006
#define IDC_RN_EDIT_A           2007
#define IDC_RN_LBL_B            2008
#define IDC_RN_EDIT_B           2009
#define IDC_RN_HINT             2010
#define IDC_RN_INSTID           2011

/* 削除確認ダイアログ (設計書 5.3) */
#define IDD_REMOVE              201
#define IDC_RM_TEXT             2101

/* 詳細ダイアログ */
#define IDD_DETAILS             202
#define IDC_DT_TEXT             2201

/* 旧インスタンス整理ダイアログ (設計書 9.3) */
#define IDD_CLEANUP             203
#define IDC_CU_TARGET           2301
#define IDC_CU_LBL1             2302
#define IDC_CU_LIST             2303
#define IDC_CU_LBL2             2304
#define IDC_CU_NEWNAME          2305
#define IDC_CU_BASE             2306
#define IDC_CU_PLAN             2307

#endif /* DNM_RESOURCE_H */
