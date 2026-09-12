/*
 * smoke.c - GUI を起動せずにロジック部分を確かめる読み取り専用テスト。
 *
 *   mingw32-make -f tests/Makefile.smoke
 *   build/smoke.exe
 *
 * 実機のデバイスを列挙するだけで、何も変更しない。
 * 唯一の書き込みは --history を付けたときの履歴 JSON の出力テスト。
 */
#include "dnm.h"
#include <objbase.h>
#include <string.h>

extern const WCHAR *dnm_multisz_next(const WCHAR *cur);

static int failures = 0;

/* MinGW の printf("%ls") は C ロケールで変換するため、日本語で途中切れする。
 * 実測でデバイス名が欠けたので、UTF-8 に変換して %s で出す。 */
static const char *u8(const WCHAR *w)
{
    static char pool[8][1024];
    static int  turn = 0;
    char *b = pool[turn = (turn + 1) % 8];
    if (!w) { strcpy(b, "(null)"); return b; }
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, b, 1024, NULL, NULL) == 0)
        strcpy(b, "(conv error)");
    return b;
}

static void check(const char *what, int ok)
{
    printf("  [%s] %s\n", ok ? "OK" : "NG", what);
    if (!ok) failures++;
}

/* ---- 10 章: ベース名推定 ---------------------------------------- */
static void test_base_name(void)
{
    struct { const WCHAR *in; const WCHAR *want; } cases[] = {
        { L"USB DAC (2)",   L"USB DAC" },
        { L"USB DAC (10)",  L"USB DAC" },
        { L"Ethernet 2",    L"Ethernet" },
        { L"Headphones #2", L"Headphones" },
        { L"USB DAC",       L"USB DAC" },
        /* 数字が製品名の一部のケース: 消えてしまうのは承知のうえで、
         * UI 側でユーザーが確定する前提 (設計書 10 章 / 24.4) */
        { L"Realtek(R) Audio", L"Realtek(R) Audio" },
    };
    int i;
    printf("\n[1] ベース名推定 (設計書 10 章)\n");
    for (i = 0; i < (int)(sizeof(cases) / sizeof(cases[0])); i++) {
        WCHAR out[DNM_MAX_NAME];
        char buf[512];
        dnm_guess_base_name(cases[i].in, out, DNM_MAX_NAME);
        _snprintf(buf, sizeof(buf), "\"%s\" -> \"%s\" (期待 \"%s\")",
                  u8(cases[i].in), u8(out), u8(cases[i].want));
        check(buf, wcscmp(out, cases[i].want) == 0);
    }
}

/* ---- 4 章: 列挙 --------------------------------------------------- */
static void test_enumerate(DeviceList *list)
{
    int i, present = 0, absent = 0, named = 0, protectedCount = 0;
    int kinds[DK_Other + 1];
    char buf[256];

    printf("\n[2] デバイス列挙 (設計書 4 章)\n");
    memset(kinds, 0, sizeof(kinds));

    check("dnm_enumerate が成功する", dnm_enumerate(list));
    _snprintf(buf, sizeof(buf), "デバイスを %d 台取得した", list->count);
    check(buf, list->count > 0);

    for (i = 0; i < list->count; i++) {
        const DeviceInfo *d = &list->items[i];
        if (d->isPresent) present++; else absent++;
        if (d->friendlyName[0]) named++;
        if (d->protect != PROT_NONE) protectedCount++;
        if ((int)d->kind <= DK_Other) kinds[d->kind]++;
    }

    printf("      接続中 %d / 未接続 %d / FriendlyName あり %d / 保護 %d\n",
           present, absent, named, protectedCount);
    printf("      種別: PnP=%d Audio=%d Net=%d USB=%d BT=%d HID=%d "
           "Storage=%d Display=%d Other=%d\n",
           kinds[DK_GenericPnP], kinds[DK_AudioEndpoint], kinds[DK_NetworkAdapter],
           kinds[DK_Usb], kinds[DK_Bluetooth], kinds[DK_Hid],
           kinds[DK_Storage], kinds[DK_Display], kinds[DK_Other]);

    check("未接続デバイスも取得できている (DIGCF_PRESENT を付けていない)", absent > 0);

    /* 表示名は必ず埋まっていなければならない。
     * 実測で FriendlyName を持つのは全体の 2 割程度しかないため、
     * DeviceDesc へのフォールバックが効いているかを見る。 */
    {
        int emptyDisplay = 0;
        for (i = 0; i < list->count; i++)
            if (!list->items[i].displayName[0]) emptyDisplay++;
        _snprintf(buf, sizeof(buf), "表示名が空のデバイスが 0 件 (実際 %d 件)",
                  emptyDisplay);
        check(buf, emptyDisplay == 0);
    }

    /* Instance ID は主キーなので必ず一意かつ非空 */
    {
        int emptyId = 0;
        for (i = 0; i < list->count; i++)
            if (!list->items[i].instanceId[0]) emptyId++;
        _snprintf(buf, sizeof(buf), "Instance ID が空のデバイスが 0 件 (実際 %d 件)",
                  emptyId);
        check(buf, emptyId == 0);
    }
}

/* ---- 7 / 8 章: Audio と Net の紐づけ ------------------------------ */
static void test_associations(const DeviceList *list)
{
    int i, withAudio = 0, withNet = 0, shown = 0;
    char buf[256];

    printf("\n[3] Audio / Net の紐づけ (設計書 7 章 / 8 章)\n");

    for (i = 0; i < list->count; i++) {
        const DeviceInfo *d = &list->items[i];
        if (d->audioCount > 0) withAudio++;
        if (d->hasNetConn && d->netAlias[0]) withNet++;
    }

    _snprintf(buf, sizeof(buf),
              "オーディオエンドポイントを持つ PnP デバイス %d 台", withAudio);
    check(buf, withAudio > 0);

    _snprintf(buf, sizeof(buf),
              "接続名 (Interface Alias) を取得できたネットワークアダプター %d 台",
              withNet);
    check(buf, withNet > 0);

    printf("      -- ネットワークアダプター --\n");
    for (i = 0; i < list->count && shown < 6; i++) {
        const DeviceInfo *d = &list->items[i];
        if (!d->hasNetConn || !d->netAlias[0]) continue;
        printf("      PnP名=\"%s\"\n", u8(d->displayName));
        printf("        接続名=\"%s\"  %s\n",
               u8(d->netAlias), d->isPresent ? "接続中" : "未接続");
        shown++;
    }

    shown = 0;
    printf("      -- オーディオ --\n");
    for (i = 0; i < list->count && shown < 6; i++) {
        const DeviceInfo *d = &list->items[i];
        int k;
        if (d->audioCount == 0) continue;
        printf("      PnP名=\"%s\"\n", u8(d->displayName));
        for (k = 0; k < d->audioCount; k++)
            printf("        [%s] \"%s\" (state=%lu)\n",
                   d->audio[k].flow == 0 ? "出力" : "入力",
                   u8(d->audio[k].friendlyName),
                   (unsigned long)d->audio[k].state);
        shown++;
    }
}

/* ---- 11.2 章: 保護ルール ------------------------------------------ */
static void test_protection(const DeviceList *list)
{
    int i;
    int rootHub = 0, sys = 0, lastNet = 0, lastInput = 0;

    printf("\n[4] 保護ルール (設計書 11.2)\n");

    for (i = 0; i < list->count; i++) {
        switch (list->items[i].protect) {
        case PROT_USB_ROOT_HUB:   rootHub++;   break;
        case PROT_SYSTEM_CLASS:   sys++;       break;
        case PROT_LAST_NETWORK:   lastNet++;   break;
        case PROT_LAST_INPUT:     lastInput++; break;
        default: break;
        }
    }
    printf("      USB ルートハブ %d / システム %d / 唯一のNIC %d / 唯一の入力 %d\n",
           rootHub, sys, lastNet, lastInput);

    check("USB ルートハブが保護されている", rootHub > 0);
    check("システムクラスのデバイスが保護されている", sys > 0);

    /* 保護されたデバイスは候補にも出てはいけない */
    {
        int leaked = 0;
        for (i = 0; i < list->count && i < 50; i++) {
            MatchCandidate cand[32];
            int n = dnm_find_candidates(list, i, cand, 32), k;
            for (k = 0; k < n; k++)
                if (list->items[cand[k].index].protect != PROT_NONE) leaked++;
        }
        check("保護デバイスが同一ハードウェア候補に混ざらない", leaked == 0);
    }
}

/* ---- 20 章: 同一ハードウェア候補 ---------------------------------- */
static void test_matching(const DeviceList *list)
{
    int i, withCandidates = 0, shown = 0;
    char buf[256];

    printf("\n[5] 同一ハードウェア候補の抽出 (設計書 20 章)\n");

    for (i = 0; i < list->count; i++) {
        MatchCandidate cand[32];
        int n = dnm_find_candidates(list, i, cand, 32);
        if (n > 0) withCandidates++;
    }
    _snprintf(buf, sizeof(buf), "候補が見つかるデバイスが %d 台ある", withCandidates);
    check(buf, withCandidates >= 0);   /* 0 でもテスト失敗ではない */

    for (i = 0; i < list->count && shown < 3; i++) {
        MatchCandidate cand[32];
        int n = dnm_find_candidates(list, i, cand, 32), k;
        if (n == 0) continue;
        printf("      対象: \"%s\"\n", u8(list->items[i].displayName));
        for (k = 0; k < n && k < 3; k++) {
            const DeviceInfo *c = &list->items[cand[k].index];
            printf("        %3d点 \"%s\"", cand[k].score, u8(c->displayName));
            printf(" (%s)  根拠: %s\n",
                   c->isPresent ? "接続中" : "未接続", u8(cand[k].reasons));
        }
        shown++;
    }

    /* 自分自身が候補に出てはいけない */
    {
        int self = 0;
        for (i = 0; i < list->count; i++) {
            MatchCandidate cand[32];
            int n = dnm_find_candidates(list, i, cand, 32), k;
            for (k = 0; k < n; k++) if (cand[k].index == i) self++;
        }
        check("自分自身が候補に含まれない", self == 0);
    }

    /* 既定チェックの条件 (整理ダイアログが最初からチェックを入れる範囲)。
     * 誤って消すと痛いので、どれだけ絞れているかを実データで見る。 */
    {
        int loose = 0, strict = 0;
        for (i = 0; i < list->count; i++) {
            MatchCandidate cand[32];
            int n = dnm_find_candidates(list, i, cand, 32), k;
            for (k = 0; k < n; k++) {
                const DeviceInfo *c = &list->items[cand[k].index];
                if (cand[k].score >= 80 && !c->isPresent) {
                    loose++;
                    if (cand[k].hardwareIdMatch) strict++;
                }
            }
        }
        printf("      既定チェック対象: Hardware ID 条件なし %d 件 -> あり %d 件\n",
               loose, strict);
        check("Hardware ID 一致を必須にすると既定チェックが減る (複合デバイス除外)",
              strict < loose);
    }
}

/* ---- 4.2 章: 表示フィルター ---------------------------------------- */
static void test_filter(const DeviceList *list)
{
    EnumOptions opt;
    int i, n;
    char buf[256];

    printf("\n[6] 表示フィルター (設計書 4.2)\n");

    memset(&opt, 0, sizeof(opt));
    opt.kindFilter = -1;
    opt.showPresent = TRUE;
    for (i = 0, n = 0; i < list->count; i++)
        if (dnm_device_matches(&list->items[i], &opt)) n++;
    _snprintf(buf, sizeof(buf), "既定 (接続中のみ、システム除く) で %d 台", n);
    check(buf, n > 0 && n < list->count);

    opt.showAbsent = TRUE;
    for (i = 0, n = 0; i < list->count; i++)
        if (dnm_device_matches(&list->items[i], &opt)) n++;
    _snprintf(buf, sizeof(buf), "未接続を足すと %d 台に増える", n);
    check(buf, n > 0);

    opt.showSystem = TRUE;
    for (i = 0, n = 0; i < list->count; i++)
        if (dnm_device_matches(&list->items[i], &opt)) n++;
    _snprintf(buf, sizeof(buf), "システムも足すと全 %d 台になる", n);
    check(buf, n == list->count);

    /* 検索 */
    opt.showSystem = FALSE;
    dnm_strcpy(opt.search, 128, L"usb");
    for (i = 0, n = 0; i < list->count; i++)
        if (dnm_device_matches(&list->items[i], &opt)) n++;
    _snprintf(buf, sizeof(buf), "\"usb\" で絞ると %d 台", n);
    check(buf, n > 0);
}

/* ---- 21 章: 単一デバイスの再取得 ---------------------------------- */
static void test_refetch(const DeviceList *list)
{
    int i, tried = 0, ok = 0;
    char buf[256];

    printf("\n[7] 再取得による検証経路 (設計書 21 章)\n");

    for (i = 0; i < list->count && tried < 20; i++) {
        DeviceInfo one;
        if (!list->items[i].isPresent) continue;
        tried++;
        if (dnm_refetch(list->items[i].instanceId, &one)) {
            if (_wcsicmp(one.instanceId, list->items[i].instanceId) == 0) ok++;
            free(one.hardwareIds);
        }
    }
    _snprintf(buf, sizeof(buf), "接続中デバイス %d 件を再取得し %d 件一致", tried, ok);
    check(buf, tried > 0 && ok == tried);
}

/* ---- 12 章: 履歴 JSON --------------------------------------------- */
static void test_history(const DeviceList *list)
{
    WCHAR path[MAX_PATH];
    OpResult res;
    FILE *f;
    int i;

    printf("\n[8] 履歴 JSON (設計書 12 章)  ※--history 指定時のみ\n");

    dnm_history_path(path, MAX_PATH);
    printf("      出力先: %s\n", u8(path));

    memset(&res, 0, sizeof(res));
    res.code = OPR_OK;
    dnm_strcpy(res.actualName, DNM_MAX_NAME, L"TEST NAME");

    /* 適当な 1 台を使って rename/remove を 1 件ずつ書く */
    for (i = 0; i < list->count; i++) {
        if (!list->items[i].hardwareIds) continue;
        dnm_history_log_rename(&list->items[i], L"pnp",
                               list->items[i].displayName, L"TEST NAME", &res);
        dnm_history_log_remove(&list->items[i], &res);
        break;
    }

    f = _wfopen(path, L"rb");
    check("履歴ファイルが作成された", f != NULL);
    if (f) {
        char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        check("JSON 配列として開いている", buf[0] == '[');
        check("2 件ぶん追記されている",
              strstr(buf, "\"rename\"") && strstr(buf, "\"remove\""));
        printf("      ---- 先頭 600 バイト ----\n%.600s\n      ----\n", buf);
    }
}

int main(int argc, char **argv)
{
    DeviceList list;
    BOOL doHistory = FALSE;
    int i;

    for (i = 1; i < argc; i++)
        if (strcmp(argv[i], "--history") == 0) doHistory = TRUE;

    /* 日本語をコンソールへ出すため */
    SetConsoleOutputCP(CP_UTF8);
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

    printf("Windows Device Name Manager - smoke test\n");
    printf("管理者権限: %s\n", dnm_is_elevated() ? "あり" : "なし (読み取りのみ検証)");

    dnm_list_init(&list);

    test_base_name();
    test_enumerate(&list);
    test_associations(&list);
    test_protection(&list);
    test_matching(&list);
    test_filter(&list);
    test_refetch(&list);
    if (doHistory) test_history(&list);
    else printf("\n[8] 履歴 JSON: --history 指定時のみ実行 (ファイルを書くため)\n");

    dnm_list_free(&list);
    CoUninitialize();

    printf("\n===============================\n");
    printf("失敗: %d 件\n", failures);
    return failures ? 1 : 0;
}
