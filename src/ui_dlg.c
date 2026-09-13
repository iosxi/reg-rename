/*
 * ui_dlg.c - 各ダイアログ (設計書 3.2 / 5.3 / 7.2 / 9.3)
 *
 * リソースには文字列を一切置かず、表示文字列はすべてここから流し込む。
 */
#include "dnm.h"
#include "resource.h"
#include <mmdeviceapi.h>   /* DEVICE_STATE_* */
#include <shlobj.h>        /* SHBrowseForFolderW (設定のフォルダ選択) */

extern const WCHAR *dnm_multisz_next(const WCHAR *cur);

/* ------------------------------------------------------------------ */
/* 共通の小道具                                                        */
/* ------------------------------------------------------------------ */
static void set_text(HWND dlg, int id, const WCHAR *s)
{
    SetDlgItemTextW(dlg, id, s);
}

static void show_ctl(HWND dlg, int id, BOOL show)
{
    HWND h = GetDlgItem(dlg, id);
    if (h) ShowWindow(h, show ? SW_SHOW : SW_HIDE);
}

static void get_text(HWND dlg, int id, WCHAR *buf, int cap)
{
    buf[0] = 0;
    GetDlgItemTextW(dlg, id, buf, cap);
}

static void trim(WCHAR *s)
{
    size_t len = wcslen(s), i = 0;
    while (len > 0 && (s[len - 1] == L' ' || s[len - 1] == L'\t')) s[--len] = 0;
    while (s[i] == L' ' || s[i] == L'\t') i++;
    if (i) memmove(s, s + i, (wcslen(s + i) + 1) * sizeof(WCHAR));
}

static const WCHAR *present_text(BOOL present)
{
    return present ? L"接続中" : L"未接続";
}

/* オーディオ欄のラベル。
 *
 * 「サウンド」画面に出る名前は Windows が
 *   <DeviceDesc> (<インターフェイス名>)
 * と組み立てた読み取り専用の値で、書けるのは前半だけ (実測)。
 * 何を編集しているのかが分かるよう、組み立て結果も一緒に見せる。 */
static const WCHAR *audio_label(const DeviceInfo *d, int idx, BOOL render,
                                WCHAR *buf, size_t cap)
{
    const AudioEndpointInfo *ep = &d->audio[idx];

    if (ep->interfaceName[0])
        _snwprintf(buf, cap,
                   L"オーディオ%s名 (編集できるのはこの欄。表示は「%s (%s)」になります)",
                   render ? L"出力エンドポイント" : L"入力エンドポイント",
                   ep->deviceDesc[0] ? ep->deviceDesc : L"?",
                   ep->interfaceName);
    else
        _snwprintf(buf, cap, L"オーディオ%s名 (PKEY_Device_DeviceDesc)",
                   render ? L"出力エンドポイント" : L"入力エンドポイント");
    buf[cap - 1] = 0;
    return buf;
}

/* 結果コードに応じたアイコンで結果を見せる。
 * 書き出したバックアップのパスも必ず添える (本人の指定)。 */
static void report(HWND parent, const WCHAR *title, const OpResult *res)
{
    UINT icon = MB_ICONINFORMATION;
    WCHAR body[4096];

    if (res->code == OPR_APPLIED_NOT_KEPT) icon = MB_ICONWARNING;
    else if (res->code == OPR_FAILED)      icon = MB_ICONERROR;
    else if (res->code == OPR_SKIPPED)     icon = MB_ICONWARNING;

    if (res->backupInfo[0])
        _snwprintf(body, 4096,
                   L"%s\n\n--- 変更前のレジストリを書き出しました ---\n%s",
                   res->message, res->backupInfo);
    else
        dnm_strcpy(body, 4096, res->message);
    body[4095] = 0;

    MessageBoxW(parent, body, title, MB_OK | icon);
}

/* ------------------------------------------------------------------ */
/* リネームの適用 (rename ダイアログと cleanup ダイアログで共用)        */
/*                                                                     */
/* 種別ごとに適切な Windows 管理機構へ振り分ける (設計書 26 章)。        */
/* ------------------------------------------------------------------ */
typedef struct {
    BOOL  renamePnp;     WCHAR pnpName[DNM_MAX_NAME];
    BOOL  renameNet;     WCHAR netName[DNM_MAX_NAME];
    BOOL  renameAudioR;  WCHAR audioRName[DNM_MAX_NAME]; int audioRIndex;
    BOOL  renameAudioC;  WCHAR audioCName[DNM_MAX_NAME]; int audioCIndex;
} RenamePlan;

/* 最も悪い結果コードを全体の結果にする */
static OpResultCode worse(OpResultCode a, OpResultCode b)
{
    if (a == OPR_FAILED || b == OPR_FAILED) return OPR_FAILED;
    if (a == OPR_APPLIED_NOT_KEPT || b == OPR_APPLIED_NOT_KEPT)
        return OPR_APPLIED_NOT_KEPT;
    if (a == OPR_SKIPPED || b == OPR_SKIPPED) return OPR_SKIPPED;
    return OPR_OK;
}

static void append_line(WCHAR *buf, size_t cap, const WCHAR *line)
{
    size_t len = wcslen(buf);
    if (len + 2 >= cap) return;
    if (len) { buf[len++] = L'\n'; buf[len] = 0; }
    dnm_strcpy(buf + wcslen(buf), cap - wcslen(buf), line);
}

/* ------------------------------------------------------------------ */
/* バックアップの取り回し                                              */
/*                                                                     */
/* バックアップは変更の「直前」でなければ意味がないが、書いたあとで      */
/* 変更が行われなかったのなら、その .reg は残す理由がない。             */
/* (指摘: 競合の確認画面でキャンセルしたのに .reg ができていた)          */
/*                                                                     */
/* そこで書き出しと記録を分ける。                                       */
/*   backup_begin   ... 書き出す。まだ結果には載せない                  */
/*   backup_commit  ... 変更が行われたので残し、パスを結果に載せる      */
/*   backup_discard ... 何も変わらなかったのでファイルごと消す          */
/* ------------------------------------------------------------------ */
typedef struct {
    BOOL  taken;             /* 書き出せたか */
    WCHAR path[MAX_PATH];
    WCHAR err[300];
} BackupSlot;

/* バックアップに失敗しても操作自体は止めない。止めると、バックアップ先を
 * 設定していないだけで何もできなくなる。代わりに画面に必ず出す。 */
static void backup_begin(BackupSlot *b, const WCHAR *regPath,
                         const WCHAR *tag, const WCHAR *label)
{
    ZeroMemory(b, sizeof(*b));
    b->taken = dnm_backup_reg_key(regPath, tag, label,
                                  b->path, MAX_PATH, b->err, 300);
}

static void backup_commit(OpResult *out, const BackupSlot *b, const WCHAR *tag)
{
    WCHAR line[MAX_PATH + 80];

    if (b->taken)
        _snwprintf(line, MAX_PATH + 80, L"  [%s] %s", tag, b->path);
    else
        _snwprintf(line, MAX_PATH + 80, L"  [%s] 書き出せませんでした: %s", tag, b->err);
    line[MAX_PATH + 79] = 0;

    append_line(out->backupInfo, 1024, line);
}

static void backup_discard(BackupSlot *b)
{
    if (b->taken) DeleteFileW(b->path);
    b->taken = FALSE;
}

/* 変更が起きたと言えるか。失敗と見送りは「何も変わっていない」。 */
static BOOL changed_anything(OpResultCode c)
{
    return c == OPR_OK || c == OPR_APPLIED_NOT_KEPT;
}

/* ------------------------------------------------------------------ */
/* 接続名の競合 → 確認画面                                             */
/*                                                                     */
/* 以前はエラーを出して終わりだったが、「旧アダプターを削除してから      */
/* 再実行してください」と言われても手が止まるだけなので、その場で        */
/* 削除して続けるかどうかを選ばせる。                                   */
/* 削除の直前には dnm_remove_device が自分でバックアップを書き出す。     */
/* ------------------------------------------------------------------ */
/* lParam は「タイトル \n OK ボタン名 \n 本文」の形。
 * リソースに文字列を置かない方針なので、3 つまとめて渡す。 */
static INT_PTR CALLBACK conflict_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        const WCHAR *p = (const WCHAR *)lp;
        WCHAR caption[128], okLabel[64];
        const WCHAR *nl1, *nl2;
        size_t n;

        nl1 = wcschr(p, L'\n');
        nl2 = nl1 ? wcschr(nl1 + 1, L'\n') : NULL;
        if (!nl1 || !nl2) {   /* 形が違えば本文だけとして扱う */
            SetWindowTextW(dlg, L"確認");
            set_text(dlg, IDC_CF_TEXT, p);
            set_text(dlg, IDOK, L"実行する");
        } else {
            n = (size_t)(nl1 - p);
            if (n > 127) n = 127;
            wmemcpy(caption, p, n); caption[n] = 0;

            n = (size_t)(nl2 - (nl1 + 1));
            if (n > 63) n = 63;
            wmemcpy(okLabel, nl1 + 1, n); okLabel[n] = 0;

            SetWindowTextW(dlg, caption);
            set_text(dlg, IDC_CF_TEXT, nl2 + 1);
            set_text(dlg, IDOK, okLabel);
        }
        set_text(dlg, IDCANCEL, L"キャンセル");
        /* 既定はキャンセル側。どちらの用途も取り消せない操作を伴う。 */
        SetFocus(GetDlgItem(dlg, IDCANCEL));
        return FALSE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)     { EndDialog(dlg, IDOK);     return TRUE; }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

/* IDD_CONFLICT は「長い説明 + 2 ボタン」の汎用の形なので使い回す */
static BOOL confirm_box(HWND parent, const WCHAR *caption, const WCHAR *text,
                        const WCHAR *okLabel)
{
    WCHAR packed[2400];
    /* conflict_proc は lParam を本文として受け取る。ボタン名とタイトルは
     * 先頭 2 行に載せて渡す (リソースに文字列を置かない方針のため)。 */
    _snwprintf(packed, 2400, L"%s\n%s\n%s", caption, okLabel, text);
    packed[2399] = 0;
    return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_CONFLICT), parent,
                           conflict_proc, (LPARAM)packed) == IDOK;
}

/* ------------------------------------------------------------------ */
/* オーディオの連番を解消する                                          */
/* ------------------------------------------------------------------ */
void dnm_dlg_fix_audio_serial(HWND parent, const DeviceInfo *d)
{
    WCHAR text[1800];
    OpResult res;

    if (d->audioCount == 0) {
        MessageBoxW(parent,
                    L"このデバイスにはオーディオエンドポイントがありません。",
                    L"オーディオの連番を解消", MB_OK | MB_ICONINFORMATION);
        return;
    }
    if (!dnm_audio_has_serial(d)) {
        WCHAR m[600];
        _snwprintf(m, 600,
                   L"このデバイスの名前に連番は付いていません。\n\n現在の名前: %s",
                   d->audio[0].friendlyName);
        m[599] = 0;
        MessageBoxW(parent, m, L"オーディオの連番を解消", MB_OK | MB_ICONINFORMATION);
        return;
    }

    _snwprintf(text, 1800,
        L"「%s」の名前に連番が付いています。\r\n"
        L"\r\n"
        L"  現在の名前 : %s\r\n"
        L"  Instance ID: %s\r\n"
        L"\r\n"
        L"連番はレジストリに保存されておらず、Windows が同じ名前の\r\n"
        L"エンドポイントを見つけたときに実行時に付けています。\r\n"
        L"そのため名前を書き換えても消えません。\r\n"
        L"\r\n"
        L"「実行する」を押すと、この順で行います。\r\n"
        L"  1. オーディオサービス (AudioEndpointBuilder) を再起動\r\n"
        L"     → 残っている古いエンドポイント登録が片付きます\r\n"
        L"  2. このデバイスを削除して、すぐに再検出させる\r\n"
        L"     → エンドポイントが作り直され、連番が付かなくなります\r\n"
        L"  3. 取り直して、連番が実際に取れたか確認\r\n"
        L"\r\n"
        L"削除の直前にレジストリを .reg へ書き出します。\r\n"
        L"実行中は数秒間、音が途切れます。デバイスは自動で戻ります。\r\n"
        L"\r\n"
        L"先に「旧インスタンスを整理して名前を変更」で同じ製品の古い\r\n"
        L"インスタンスを消しておくと、確実に連番が取れます。",
        d->displayName, d->audio[0].friendlyName, d->instanceId);
    text[1799] = 0;

    if (!confirm_box(parent, L"オーディオの連番を解消", text, L"実行する"))
        return;

    dnm_fix_audio_serial(d, &res);
    report(parent, L"オーディオの連番を解消", &res);
}

static BOOL conflict_confirm(HWND parent, const WCHAR *newName,
                             const ConnNameOwner *own, const DeviceInfo *old,
                             BOOL oldFound)
{
    WCHAR text[2000];

    _snwprintf(text, 2000,
        L"接続名「%s」は、すでに別のネットワークアダプターが使っています。\r\n"
        L"\r\n"
        L"  アダプター : %s\r\n"
        L"  Instance ID: %s\r\n"
        L"  接続の GUID: %s\r\n"
        L"  状態       : %s\r\n"
        L"\r\n"
        L"「削除する」を押すと、上のアダプターを削除してから、\r\n"
        L"もう一度この名前への変更を試します。\r\n"
        L"\r\n"
        L"実行前に、次の 2 つのレジストリキーを書き出します。\r\n"
        L"  1. 変更対象の接続キー (Connection)\r\n"
        L"  2. 削除するアダプターのキー (Enum)\r\n"
        L"書き出したファイルのパスは、完了後の画面に表示します。\r\n"
        L"「キャンセル」を押した場合は、レジストリも変更しませんし、\r\n"
        L".reg ファイルも作りません。\r\n"
        L"\r\n"
        L"なお、一覧に「%s」という名前の PnP デバイス (SWD\\RADIO\\...) が\r\n"
        L"出ている場合、それは上のアダプターの無線ノードであって、\r\n"
        L"名前の持ち主そのものではありません。そちらを消しても名前は空きません。",
        newName,
        own->adapterDesc[0] ? own->adapterDesc : L"(不明)",
        own->instanceId[0] ? own->instanceId : L"(不明)",
        own->guid,
        !oldFound ? L"デバイスとして見つかりません (削除できません)"
                  : (old->isPresent ? L"接続中" : L"未接続"),
        newName);
    text[1999] = 0;

    return confirm_box(parent, L"接続名が使われています", text, L"削除する");
}

/* 競合していた旧アダプターを削除する。
 * 名前が空いて、変更をやり直してよければ TRUE。 */
static BOOL remove_conflicting_adapter(const DeviceInfo *d, const WCHAR *newName,
                                       const ConnNameOwner *own, OpResult *out)
{
    DeviceInfo old;
    OpResult rm;

    if (!dnm_refetch(own->instanceId, &old)) {
        out->code = worse(out->code, OPR_FAILED);
        append_line(out->message, 1024,
                    L"→ 競合相手をデバイスとして取得できず、削除できませんでした。");
        return FALSE;
    }

    /* 削除の直前のバックアップは dnm_remove_device が自分で書き出す。
     * 削除できなかった場合は、あちらで .reg も消える。 */
    dnm_remove_device(&old, &rm);
    dnm_history_log_remove(&old, &rm);
    free(old.hardwareIds);

    if (rm.backupInfo[0]) append_line(out->backupInfo, 1024, rm.backupInfo);

    append_line(out->message, 1024, L"");
    append_line(out->message, 1024, L"[競合していたアダプターの削除]");
    append_line(out->message, 1024, rm.message);

    if (rm.code != OPR_OK) {
        out->code = worse(out->code, rm.code);
        append_line(out->message, 1024,
                    L"→ 削除できなかったので、名前の変更はやり直していません。");
        return FALSE;
    }

    /* PnP の状態が落ち着くのを待って再列挙 */
    dnm_rescan_devices();
    Sleep(800);

    /* 名前が本当に空いたかを確かめる。アダプターを消しても Windows が
     * 接続の登録を残すことがあり、そのまま進めても失敗するだけ。 */
    {
        ConnNameOwner again;
        dnm_find_conn_name_owner(newName, &d->netConnGuid, &again);
        if (again.found) {
            out->code = worse(out->code, OPR_FAILED);
            append_line(out->message, 1024, L"");
            append_line(out->message, 1024,
                L"[再確認] アダプターを削除しても、接続名はまだ使われたままです。");
            append_line(out->message, 1024,
                L"Windows が接続の登録を残しているため、再起動後に再実行してください。");
            return FALSE;
        }
    }
    return TRUE;
}

/* ネットワーク接続名の変更。
 *
 * 競合の確認は「バックアップを書く前」に行う。順番を逆にすると、
 * 確認画面でキャンセルしただけで .reg が残ってしまう (指摘を受けた点)。
 * レジストリを変えないなら .reg も作らない。 */
static void rename_net_with_conflict_check(HWND parent, const DeviceInfo *d,
                                           const WCHAR *newName, OpResult *out)
{
    ConnNameOwner own;
    BOOL removeOld = FALSE;
    BackupSlot bs;
    WCHAR regPath[600];
    OpResult r;

    append_line(out->message, 1024, L"");
    append_line(out->message, 1024, L"[ネットワーク接続名]");

    /* --- 1. まだ何も書かずに、名前が空いているかだけ調べる ------------ */
    dnm_find_conn_name_owner(newName, &d->netConnGuid, &own);

    if (own.found) {
        DeviceInfo old;
        BOOL oldFound = dnm_refetch(own.instanceId, &old);

        if (own.instanceId[0] == 0) {
            out->code = worse(out->code, OPR_FAILED);
            append_line(out->message, 1024,
                L"この接続名はすでに使われていますが、相手のアダプターを"
                L"特定できませんでした。");
            if (oldFound) free(old.hardwareIds);
            return;
        }

        removeOld = conflict_confirm(parent, newName, &own, &old, oldFound);
        if (oldFound) free(old.hardwareIds);

        if (!removeOld) {
            /* ここで抜ける。バックアップはまだ 1 つも書いていない。 */
            out->code = worse(out->code, OPR_SKIPPED);
            append_line(out->message, 1024,
                L"キャンセルしました。何も変更していません "
                L"(レジストリの書き出しも行っていません)。");
            return;
        }
    }

    /* --- 2. ここから実際に変える。変更対象の接続キーを書き出す -------- */
    dnm_regpath_netconn(&d->netConnGuid, regPath, 600);
    backup_begin(&bs, regPath, L"net", d->instanceId);

    /* --- 3. 競合していたなら、先に旧アダプターを削除 ------------------ */
    if (removeOld && !remove_conflicting_adapter(d, newName, &own, out)) {
        /* 接続キーは結局変えていないので、その .reg は残さない */
        backup_discard(&bs);
        return;
    }

    /* --- 4. 名前を変える ---------------------------------------------- */
    dnm_rename_net_alias(d, newName, &r);
    dnm_history_log_rename(d, L"net_alias", d->netAlias, newName, &r);
    out->code = worse(out->code, r.code);
    append_line(out->message, 1024, r.message);

    if (changed_anything(r.code)) backup_commit(out, &bs, L"net");
    else                          backup_discard(&bs);
}

/* 変更を 1 件行い、変わったときだけバックアップを残す共通処理 */
static void rename_step(OpResult *out, const WCHAR *regPath, const WCHAR *tag,
                        const WCHAR *label, const WCHAR *heading,
                        const OpResult *r, BackupSlot *bs)
{
    out->code = worse(out->code, r->code);
    append_line(out->message, 1024, heading);
    append_line(out->message, 1024, r->message);

    if (changed_anything(r->code)) backup_commit(out, bs, tag);
    else                           backup_discard(bs);
    (void)regPath; (void)label;
}

static void apply_rename_plan(HWND parent, const DeviceInfo *d,
                              const RenamePlan *plan, OpResult *out)
{
    OpResult r;
    WCHAR regPath[600];
    BackupSlot bs;

    ZeroMemory(out, sizeof(*out));
    out->code = OPR_OK;
    out->message[0] = 0;

    /* 項目ごとに「直前に書き出す → 実行する → 変わっていなければ消す」。
     * まとめて先に書き出すと、失敗した項目のぶんまで .reg が残る。 */
    if (plan->renamePnp) {
        dnm_regpath_pnp(d->instanceId, regPath, 600);
        backup_begin(&bs, regPath, L"pnp", d->instanceId);
        dnm_rename_pnp(d, plan->pnpName, &r);
        dnm_history_log_rename(d, L"pnp", d->friendlyName, plan->pnpName, &r);
        rename_step(out, regPath, L"pnp", d->instanceId,
                    L"[PnP デバイス名]", &r, &bs);
    }
    if (plan->renameNet) {
        rename_net_with_conflict_check(parent, d, plan->netName, out);
    }
    if (plan->renameAudioR && plan->audioRIndex >= 0) {
        const WCHAR *ep = d->audio[plan->audioRIndex].endpointId;
        dnm_regpath_audio(ep, 0, regPath, 600);
        backup_begin(&bs, regPath, L"audio_render", ep);
        dnm_rename_audio_endpoint(ep, plan->audioRName, &r);
        dnm_history_log_rename(d, L"audio_endpoint_render",
                               d->audio[plan->audioRIndex].friendlyName,
                               plan->audioRName, &r);
        append_line(out->message, 1024, L"");
        rename_step(out, regPath, L"audio_render", ep,
                    L"[オーディオ出力エンドポイント]", &r, &bs);
    }
    if (plan->renameAudioC && plan->audioCIndex >= 0) {
        const WCHAR *ep = d->audio[plan->audioCIndex].endpointId;
        dnm_regpath_audio(ep, 1, regPath, 600);
        backup_begin(&bs, regPath, L"audio_capture", ep);
        dnm_rename_audio_endpoint(ep, plan->audioCName, &r);
        dnm_history_log_rename(d, L"audio_endpoint_capture",
                               d->audio[plan->audioCIndex].friendlyName,
                               plan->audioCName, &r);
        append_line(out->message, 1024, L"");
        rename_step(out, regPath, L"audio_capture", ep,
                    L"[オーディオ入力エンドポイント]", &r, &bs);
    }

    if (out->message[0] == 0) {
        out->code = OPR_SKIPPED;
        dnm_strcpy(out->message, 1024, L"変更する項目がありませんでした。");
    }
}

/* ------------------------------------------------------------------ */
/* 名前を変更ダイアログ                                                */
/* ------------------------------------------------------------------ */
typedef struct {
    DeviceInfo *dev;
    int  renderIndex;   /* audio[] の添字 (-1 = なし) */
    int  captureIndex;
    BOOL isNet;
    BOOL applied;
} RenameCtx;

/*
 * そのコントロール自身に WS_VISIBLE が立っているか。
 *
 * IsWindowVisible() は親をたどるので、WM_INITDIALOG の時点では
 * ダイアログ自身がまだ非表示であり、どの子も FALSE になってしまう。
 * 表示前にレイアウトを決めたいので、自分のスタイルビットだけを見る。
 */
static BOOL ctl_is_shown(HWND dlg, int id)
{
    HWND h = GetDlgItem(dlg, id);
    if (!h) return FALSE;
    return (GetWindowLongPtrW(h, GWL_STYLE) & WS_VISIBLE) ? TRUE : FALSE;
}

/* 子コントロールの矩形を、ダイアログのクライアント座標で得る */
static void child_rect(HWND dlg, int id, RECT *rc)
{
    HWND h = GetDlgItem(dlg, id);
    SetRect(rc, 0, 0, 0, 0);
    if (!h) return;
    GetWindowRect(h, rc);
    MapWindowPoints(NULL, dlg, (POINT *)rc, 2);
}

static void move_child(HWND dlg, int id, int x, int y, int cx, int cy)
{
    HWND h = GetDlgItem(dlg, id);
    if (h) MoveWindow(h, x, y, cx, cy, TRUE);
}

/* ヒント文が実際に必要とする高さを測る */
static int measure_text_height(HWND ctl, int width)
{
    HDC dc = GetDC(ctl);
    HFONT font = (HFONT)SendMessageW(ctl, WM_GETFONT, 0, 0);
    HGDIOBJ old = font ? SelectObject(dc, font) : NULL;
    WCHAR text[1024];
    RECT rc;
    int h;

    GetWindowTextW(ctl, text, 1024);
    SetRect(&rc, 0, 0, width, 0);
    DrawTextW(dc, text, -1, &rc, DT_CALCRECT | DT_WORDBREAK | DT_LEFT);
    h = rc.bottom - rc.top;

    if (old) SelectObject(dc, old);
    ReleaseDC(ctl, dc);
    return h;
}

/*
 * 表示する欄の数はデバイス種別で変わる。隠した欄の分だけ空白が残ると
 * 間延びして見えるので、ヒント文とボタンを詰め直し、ダイアログ自体の
 * 高さも中身に合わせて縮める。
 */
static void rename_reflow(HWND dlg)
{
    const int ids[3] = { IDC_RN_EDIT_PNP, IDC_RN_EDIT_A, IDC_RN_EDIT_B };
    RECT rc, hint, ok, cancel, client, frame;
    int i, bottom = 0, hintH, y, newClientH, delta;

    /* 1. 表示する入力欄のうち、いちばん下の底辺を探す */
    for (i = 0; i < 3; i++) {
        if (!ctl_is_shown(dlg, ids[i])) continue;
        child_rect(dlg, ids[i], &rc);
        if (rc.bottom > bottom) bottom = rc.bottom;
    }
    if (bottom == 0) return;

    /* 2. ヒント文を入力欄の直下へ、必要な高さで置く */
    child_rect(dlg, IDC_RN_HINT, &hint);
    hintH = measure_text_height(GetDlgItem(dlg, IDC_RN_HINT),
                                hint.right - hint.left);
    y = bottom + 16;
    move_child(dlg, IDC_RN_HINT, hint.left, y, hint.right - hint.left, hintH);

    /* 3. ボタンをヒントの下へ */
    child_rect(dlg, IDOK, &ok);
    child_rect(dlg, IDCANCEL, &cancel);
    y += hintH + 16;
    move_child(dlg, IDOK, ok.left, y, ok.right - ok.left, ok.bottom - ok.top);
    move_child(dlg, IDCANCEL, cancel.left, y,
               cancel.right - cancel.left, cancel.bottom - cancel.top);

    /* 4. ダイアログの高さを中身に合わせる */
    GetClientRect(dlg, &client);
    newClientH = y + (ok.bottom - ok.top) + 12;
    delta = newClientH - client.bottom;
    if (delta == 0) return;

    GetWindowRect(dlg, &frame);
    SetWindowPos(dlg, NULL, 0, 0,
                 frame.right - frame.left,
                 (frame.bottom - frame.top) + delta,
                 SWP_NOMOVE | SWP_NOZORDER);

    /* 高さが変わったぶん、画面中央へ置き直す */
    {
        HWND owner = GetWindow(dlg, GW_OWNER);
        RECT ref;
        if (!owner || !GetWindowRect(owner, &ref))
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &ref, 0);
        GetWindowRect(dlg, &frame);
        SetWindowPos(dlg, NULL,
                     ref.left + ((ref.right - ref.left) -
                                 (frame.right - frame.left)) / 2,
                     ref.top + ((ref.bottom - ref.top) -
                                (frame.bottom - frame.top)) / 2,
                     0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
}

static void rename_init(HWND dlg, RenameCtx *ctx)
{
    DeviceInfo *d = ctx->dev;
    WCHAR buf[600], labA[300], labB[300];
    int i;

    SetWindowTextW(dlg, L"名前を変更");

    _snwprintf(buf, 600, L"対象:  %s", d->displayName);
    set_text(dlg, IDC_RN_TARGET, buf);
    _snwprintf(buf, 600, L"種類:  %s      状態:  %s",
               dnm_kind_name(d->kind), present_text(d->isPresent));
    set_text(dlg, IDC_RN_KIND, buf);
    /* Instance ID は長いので独立した行に置く。SS_ENDELLIPSIS が効くので
     * 折り返して下の行に食い込むことはない。全文は「詳細」で見られる。 */
    _snwprintf(buf, 600, L"Instance ID:  %s", d->instanceId);
    set_text(dlg, IDC_RN_INSTID, buf);

    set_text(dlg, IDC_RN_LBL_PNP, L"PnP デバイス名 (DEVPKEY_Device_FriendlyName)");
    set_text(dlg, IDC_RN_EDIT_PNP,
             d->friendlyName[0] ? d->friendlyName : d->displayName);
    set_text(dlg, IDC_RN_BASE, L"連番を除去");
    set_text(dlg, IDOK, L"適用");
    set_text(dlg, IDCANCEL, L"キャンセル");

    ctx->renderIndex = ctx->captureIndex = -1;
    for (i = 0; i < d->audioCount; i++) {
        if (d->audio[i].flow == 0 && ctx->renderIndex  < 0) ctx->renderIndex  = i;
        if (d->audio[i].flow == 1 && ctx->captureIndex < 0) ctx->captureIndex = i;
    }
    ctx->isNet = (d->kind == DK_NetworkAdapter && d->hasNetConn);

    if (d->audioCount > 0) {
        /* 設計書 7.2 のレイアウト */
        if (ctx->renderIndex >= 0) {
            set_text(dlg, IDC_RN_LBL_A, audio_label(d, ctx->renderIndex, TRUE, labA, 300));
            set_text(dlg, IDC_RN_EDIT_A, d->audio[ctx->renderIndex].deviceDesc);
        } else {
            show_ctl(dlg, IDC_RN_LBL_A, FALSE);
            show_ctl(dlg, IDC_RN_EDIT_A, FALSE);
        }
        if (ctx->captureIndex >= 0) {
            set_text(dlg, IDC_RN_LBL_B, audio_label(d, ctx->captureIndex, FALSE, labB, 300));
            set_text(dlg, IDC_RN_EDIT_B, d->audio[ctx->captureIndex].deviceDesc);
        } else {
            show_ctl(dlg, IDC_RN_LBL_B, FALSE);
            show_ctl(dlg, IDC_RN_EDIT_B, FALSE);
        }
        {   /* 括弧の中がどこから来ているかを具体的に書く。
             * ここを読まないと「(2- FX-D03J) の 2- を消したい」のに
             * エンドポイント名をいくら書き換えても変わらない。 */
            const AudioEndpointInfo *ep =
                &d->audio[ctx->renderIndex >= 0 ? ctx->renderIndex : ctx->captureIndex];
            WCHAR hint[700];
            _snwprintf(hint, 700,
                L"「サウンド」画面に出る名前は、Windows が次のように組み立てた\n"
                L"読み取り専用の値です。直接は書き換えられません。\n"
                L"    %s (%s)\n"
                L"     ↑ ここは上の欄で変更   ↑ ここはデバイス側の名前\n\n"
                L"括弧の中は、この PnP デバイスの名前です。「2- 」のような連番が\n"
                L"付いているときは、同じ製品の古いインスタンスが元の名前を\n"
                L"押さえています。「旧インスタンスを整理して名前を変更...」で\n"
                L"古いほうを片付けると取れます。",
                ep->deviceDesc[0] ? ep->deviceDesc : L"(名前)",
                ep->interfaceName[0] ? ep->interfaceName : L"(デバイス名)");
            hint[699] = 0;
            set_text(dlg, IDC_RN_HINT, hint);
        }
    } else if (ctx->isNet) {
        set_text(dlg, IDC_RN_LBL_A, L"ネットワーク接続名 (Interface Alias)");
        set_text(dlg, IDC_RN_EDIT_A, d->netAlias);
        show_ctl(dlg, IDC_RN_LBL_B, FALSE);
        show_ctl(dlg, IDC_RN_EDIT_B, FALSE);
        /* この欄は説明であって、エラー表示ではない。
         * 以前は「既存の接続と同じ名前にするには先に旧アダプターを削除して
         * ください」とだけ書いてあり、いま何か問題が起きているという
         * 診断文に見えると指摘を受けた。注意書きだと分かる書き方にする。
         * 実際の重複は適用時に検出してエラーとして出す。 */
        set_text(dlg, IDC_RN_HINT,
                 L"PnP デバイス名 (アダプターの製品名) と、ネットワーク接続一覧に\n"
                 L"出る接続名は別物です。「Ethernet 2」のような名前は後者です。\n\n"
                 L"[注意書き] 以下は一般的な説明で、いま問題が起きているという\n"
                 L"意味ではありません。接続名は重複できないため、他の接続が使って\n"
                 L"いる名前を指定すると、適用したときにエラーになります。その場合は\n"
                 L"先に旧アダプターを削除してください。");
    } else {
        show_ctl(dlg, IDC_RN_LBL_A, FALSE);
        show_ctl(dlg, IDC_RN_EDIT_A, FALSE);
        show_ctl(dlg, IDC_RN_LBL_B, FALSE);
        show_ctl(dlg, IDC_RN_EDIT_B, FALSE);
        set_text(dlg, IDC_RN_HINT,
                 L"変更対象は、この 1 つのデバイスインスタンスだけです。\n"
                 L"同じ製品の別インスタンスには影響しません。\n\n"
                 L"適用後に再取得して、名前が実際に保たれたか確認します。");
    }

    /* 隠した欄のぶんの空白を詰め、中身に合わせてダイアログを縮める */
    rename_reflow(dlg);
}

static INT_PTR CALLBACK rename_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    RenameCtx *ctx = (RenameCtx *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG:
        ctx = (RenameCtx *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)ctx);
        rename_init(dlg, ctx);
        SetFocus(GetDlgItem(dlg, IDC_RN_EDIT_PNP));
        return FALSE;

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_RN_BASE: {
            /* 設計書 10 章: 推定して入れるだけ。確定はユーザー */
            WCHAR cur[DNM_MAX_NAME], base[DNM_MAX_NAME];
            int ids[3] = { IDC_RN_EDIT_PNP, IDC_RN_EDIT_A, IDC_RN_EDIT_B };
            int i;
            for (i = 0; i < 3; i++) {
                if (!ctl_is_shown(dlg, ids[i])) continue;
                get_text(dlg, ids[i], cur, DNM_MAX_NAME);
                dnm_guess_base_name(cur, base, DNM_MAX_NAME);
                set_text(dlg, ids[i], base);
            }
            return TRUE;
        }

        case IDOK: {
            DeviceInfo *d = ctx->dev;
            RenamePlan plan;
            OpResult res;
            WCHAR buf[DNM_MAX_NAME];

            ZeroMemory(&plan, sizeof(plan));
            plan.audioRIndex = ctx->renderIndex;
            plan.audioCIndex = ctx->captureIndex;

            get_text(dlg, IDC_RN_EDIT_PNP, buf, DNM_MAX_NAME);
            trim(buf);
            if (buf[0] && wcscmp(buf, d->friendlyName) != 0) {
                plan.renamePnp = TRUE;
                dnm_strcpy(plan.pnpName, DNM_MAX_NAME, buf);
            }

            if (d->audioCount > 0) {
                if (ctx->renderIndex >= 0) {
                    get_text(dlg, IDC_RN_EDIT_A, buf, DNM_MAX_NAME);
                    trim(buf);
                    if (buf[0] &&
                        wcscmp(buf, d->audio[ctx->renderIndex].deviceDesc) != 0) {
                        plan.renameAudioR = TRUE;
                        dnm_strcpy(plan.audioRName, DNM_MAX_NAME, buf);
                    }
                }
                if (ctx->captureIndex >= 0) {
                    get_text(dlg, IDC_RN_EDIT_B, buf, DNM_MAX_NAME);
                    trim(buf);
                    if (buf[0] &&
                        wcscmp(buf, d->audio[ctx->captureIndex].deviceDesc) != 0) {
                        plan.renameAudioC = TRUE;
                        dnm_strcpy(plan.audioCName, DNM_MAX_NAME, buf);
                    }
                }
            } else if (ctx->isNet) {
                get_text(dlg, IDC_RN_EDIT_A, buf, DNM_MAX_NAME);
                trim(buf);
                if (buf[0] && wcscmp(buf, d->netAlias) != 0) {
                    plan.renameNet = TRUE;
                    dnm_strcpy(plan.netName, DNM_MAX_NAME, buf);
                }
            }

            if (!plan.renamePnp && !plan.renameNet &&
                !plan.renameAudioR && !plan.renameAudioC) {
                MessageBoxW(dlg, L"変更された項目がありません。", L"名前を変更",
                            MB_OK | MB_ICONINFORMATION);
                return TRUE;
            }

            apply_rename_plan(dlg, d, &plan, &res);
            report(dlg, L"名前を変更", &res);
            ctx->applied = TRUE;
            EndDialog(dlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

BOOL dnm_dlg_rename(HWND parent, DeviceInfo *d)
{
    RenameCtx ctx;
    ZeroMemory(&ctx, sizeof(ctx));
    ctx.dev = d;
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_RENAME), parent,
                    rename_proc, (LPARAM)&ctx);
    return ctx.applied;
}

/* ------------------------------------------------------------------ */
/* 削除確認ダイアログ (設計書 5.3)                                     */
/* ------------------------------------------------------------------ */
static INT_PTR CALLBACK remove_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    const DeviceInfo *d = (const DeviceInfo *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG: {
        WCHAR text[2000];
        d = (const DeviceInfo *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)d);
        SetWindowTextW(dlg, L"デバイスを削除");
        set_text(dlg, IDOK, L"削除");
        set_text(dlg, IDCANCEL, L"キャンセル");

        _snwprintf(text, 2000,
            L"デバイスを削除しますか？\r\n"
            L"\r\n"
            L"名前:\r\n"
            L"    %s\r\n"
            L"\r\n"
            L"Instance ID:\r\n"
            L"    %s\r\n"
            L"\r\n"
            L"状態:\r\n"
            L"    %s\r\n"
            L"\r\n"
            L"この操作はデバイスインスタンスを Windows から削除します。\r\n"
            L"ドライバー本体は削除しません。\r\n"
            L"%s",
            d->displayName, d->instanceId, present_text(d->isPresent),
            d->isPresent
                ? L"\r\n--------------------------------------------------\r\n"
                  L"このデバイスは現在接続中です。\r\n"
                  L"削除すると一時的に利用できなくなる可能性があります。\r\n"
                  L"物理的に接続されたままなら、再スキャンで再検出されます。"
                : L"");
        text[1999] = 0;
        set_text(dlg, IDC_RM_TEXT, text);

        /* 接続中なら既定ボタンをキャンセル側に寄せたままにする */
        SetFocus(GetDlgItem(dlg, IDCANCEL));
        return FALSE;
    }

    case WM_COMMAND:
        if (LOWORD(wp) == IDOK) {
            /* 接続中デバイスは二重確認 (設計書 11.3) */
            if (d->isPresent) {
                if (MessageBoxW(dlg,
                        L"接続中のデバイスを削除しようとしています。\n\n"
                        L"本当に実行しますか？",
                        L"最終確認", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES)
                    return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        if (LOWORD(wp) == IDCANCEL) { EndDialog(dlg, IDCANCEL); return TRUE; }
        break;
    }
    return FALSE;
}

BOOL dnm_dlg_remove_confirm(HWND parent, const DeviceInfo *d)
{
    if (d->protect != PROT_NONE) {
        WCHAR msg[512];
        _snwprintf(msg, 512,
                   L"このデバイスは削除操作を禁止しています。\n\n名前: %s\n理由: %s",
                   d->displayName, dnm_protect_reason_text(d->protect));
        msg[511] = 0;
        MessageBoxW(parent, msg, L"デバイスを削除", MB_OK | MB_ICONWARNING);
        return FALSE;
    }
    return DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_REMOVE), parent,
                           remove_proc, (LPARAM)d) == IDOK;
}

/* ------------------------------------------------------------------ */
/* 詳細ダイアログ                                                      */
/* ------------------------------------------------------------------ */
static void build_details(const DeviceInfo *d, WCHAR *out, size_t cap)
{
    WCHAR guid[64];
    size_t n = 0;
    int i;

#define APPENDF(...) do { \
        n = wcslen(out); \
        if (n < cap) _snwprintf(out + n, cap - n, __VA_ARGS__); \
        out[cap - 1] = 0; \
    } while (0)

    out[0] = 0;
    APPENDF(L"表示名          : %s\r\n", d->displayName);
    APPENDF(L"Friendly Name   : %s\r\n",
            d->friendlyName[0] ? d->friendlyName : L"(未設定)");
    APPENDF(L"Device Desc     : %s\r\n", d->deviceDesc);
    APPENDF(L"Instance ID     : %s\r\n", d->instanceId);
    APPENDF(L"Class           : %s\r\n", d->className);

    if (d->hasClassGuid) dnm_guid_to_string(&d->classGuid, guid, 64);
    else                 dnm_strcpy(guid, 64, L"(なし)");
    APPENDF(L"Class GUID      : %s\r\n", guid);

    if (d->hasContainerId) dnm_guid_to_string(&d->containerId, guid, 64);
    else                   dnm_strcpy(guid, 64, L"(なし)");
    APPENDF(L"Container ID    : %s\r\n", guid);

    APPENDF(L"Manufacturer    : %s\r\n", d->manufacturer[0] ? d->manufacturer : L"(なし)");
    APPENDF(L"Location        : %s\r\n", d->location[0] ? d->location : L"(なし)");
    APPENDF(L"種別 (UI 分類)  : %s\r\n", dnm_kind_name(d->kind));
    APPENDF(L"接続状態        : %s\r\n", present_text(d->isPresent));
    APPENDF(L"デバマネ非表示  : %s\r\n", d->isHidden ? L"はい" : L"いいえ");

    if (d->hasVidPid)
        APPENDF(L"VID / PID       : VID_%04X / PID_%04X\r\n", d->vid, d->pid);
    else
        APPENDF(L"VID / PID       : (なし)\r\n");

    APPENDF(L"削除の可否      : %s\r\n",
            d->protect == PROT_NONE ? L"削除可"
                                    : dnm_protect_reason_text(d->protect));

    APPENDF(L"\r\nHardware IDs:\r\n");
    {
        const WCHAR *p;
        BOOL any = FALSE;
        for (p = d->hardwareIds; p && *p; p = dnm_multisz_next(p)) {
            APPENDF(L"    %s\r\n", p);
            any = TRUE;
        }
        if (!any) APPENDF(L"    (なし)\r\n");
    }

    if (d->hasNetConn) {
        dnm_guid_to_string(&d->netConnGuid, guid, 64);
        APPENDF(L"\r\nネットワーク接続:\r\n");
        APPENDF(L"    接続名 (Interface Alias) : %s\r\n",
                d->netAlias[0] ? d->netAlias : L"(取得できず)");
        APPENDF(L"    NetCfgInstanceId         : %s\r\n", guid);
    }

    if (d->audioCount > 0) {
        APPENDF(L"\r\nオーディオエンドポイント (%d):\r\n", d->audioCount);
        for (i = 0; i < d->audioCount; i++) {
            const AudioEndpointInfo *ep = &d->audio[i];
            const WCHAR *st = L"不明";
            switch (ep->state) {
            case DEVICE_STATE_ACTIVE:     st = L"有効";       break;
            case DEVICE_STATE_DISABLED:   st = L"無効";       break;
            case DEVICE_STATE_NOTPRESENT: st = L"未接続";     break;
            case DEVICE_STATE_UNPLUGGED:  st = L"未接続(端子)"; break;
            }
            APPENDF(L"    [%s] %s\r\n", ep->flow == 0 ? L"出力" : L"入力",
                    ep->friendlyName);
            APPENDF(L"        状態 : %s\r\n", st);
            APPENDF(L"        ID   : %s\r\n", ep->endpointId);
        }
    }
#undef APPENDF
}

static INT_PTR CALLBACK details_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        const DeviceInfo *d = (const DeviceInfo *)lp;
        WCHAR *text = (WCHAR *)calloc(16384, sizeof(WCHAR));
        SetWindowTextW(dlg, L"デバイスの詳細");
        set_text(dlg, IDOK, L"閉じる");
        if (text) {
            build_details(d, text, 16384);
            set_text(dlg, IDC_DT_TEXT, text);
            free(text);
        }
        return TRUE;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK || LOWORD(wp) == IDCANCEL) {
            EndDialog(dlg, IDOK);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void dnm_dlg_details(HWND parent, const DeviceInfo *d)
{
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_DETAILS), parent,
                    details_proc, (LPARAM)d);
}

/* ------------------------------------------------------------------ */
/* 旧インスタンス整理 + リネーム (設計書 9 章)                          */
/* ------------------------------------------------------------------ */
#define MAX_CANDIDATES 64

typedef struct {
    DeviceList     *list;
    int             targetIndex;
    MatchCandidate  cand[MAX_CANDIDATES];
    int             candCount;
    BOOL            executed;
} CleanupCtx;

static void cleanup_fill_list(HWND dlg, CleanupCtx *ctx)
{
    HWND lv = GetDlgItem(dlg, IDC_CU_LIST);
    LVCOLUMNW col;
    int i;

    ListView_SetExtendedListViewStyle(
        lv, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);

    ZeroMemory(&col, sizeof(col));
    col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;

    /* 「別名」列は必須。同じ製品が 3 つあると表示名は 3 つとも同じで、
     * Instance ID は長すぎて見比べられない。接続名やエンドポイント名なら
     * 個体ごとに違う (「SPDIF インターフェイス (2- FX-D03J)」等) ので、
     * 表示名のすぐ右に置いて見分けられるようにする。 */
    col.pszText = (LPWSTR)L"表示名";      col.cx = 170; col.iSubItem = 0;
    ListView_InsertColumn(lv, 0, &col);
    /* 別名はここで見分けるための列なので、切れないように広く取る */
    col.pszText = (LPWSTR)L"別名 (接続名 / エンドポイント)";
                                          col.cx = 320; col.iSubItem = 1;
    ListView_InsertColumn(lv, 1, &col);
    col.pszText = (LPWSTR)L"状態";        col.cx =  60; col.iSubItem = 2;
    ListView_InsertColumn(lv, 2, &col);
    col.pszText = (LPWSTR)L"スコア";      col.cx =  50; col.iSubItem = 3;
    ListView_InsertColumn(lv, 3, &col);
    col.pszText = (LPWSTR)L"一致した根拠"; col.cx = 180; col.iSubItem = 4;
    ListView_InsertColumn(lv, 4, &col);
    col.pszText = (LPWSTR)L"Instance ID"; col.cx = 200; col.iSubItem = 5;
    ListView_InsertColumn(lv, 5, &col);

    for (i = 0; i < ctx->candCount; i++) {
        const DeviceInfo *d = &ctx->list->items[ctx->cand[i].index];
        LVITEMW it;
        WCHAR num[16];

        ZeroMemory(&it, sizeof(it));
        it.mask = LVIF_TEXT | LVIF_PARAM;
        it.iItem = i;
        it.pszText = (LPWSTR)d->displayName;
        it.lParam = i;
        ListView_InsertItem(lv, &it);

        ListView_SetItemText(lv, i, 1, (LPWSTR)dnm_alias_text(d));
        ListView_SetItemText(lv, i, 2, (LPWSTR)present_text(d->isPresent));
        _snwprintf(num, 16, L"%d", ctx->cand[i].score);
        ListView_SetItemText(lv, i, 3, num);
        ListView_SetItemText(lv, i, 4, (LPWSTR)ctx->cand[i].reasons);
        ListView_SetItemText(lv, i, 5, (LPWSTR)d->instanceId);

        /* 既定チェックは「強候補 かつ 未接続 かつ Hardware ID 一致」のみ。
         *
         * 実機で測ると、複合デバイス (キーボード兼マウスなど) は
         * Container ID / VID/PID / Class / Manufacturer / 名前が揃うため
         * Hardware ID が違っていても 80 点に届いてしまう。それらは
         * 「同じ製品の別インスタンス」ではなく「同じ機器の別機能」なので、
         * 消してはいけない。Hardware ID 一致を必須にすると両者を分けられる。
         * (設計書 9.2「名前だけで自動削除してはいけない」) */
        if (ctx->cand[i].score >= 80 && !d->isPresent && ctx->cand[i].hardwareIdMatch)
            ListView_SetCheckState(lv, i, TRUE);
    }
}

static void cleanup_update_plan(HWND dlg, CleanupCtx *ctx)
{
    HWND lv = GetDlgItem(dlg, IDC_CU_LIST);
    WCHAR name[DNM_MAX_NAME], plan[1024];
    int i, checked = 0;

    get_text(dlg, IDC_CU_NEWNAME, name, DNM_MAX_NAME);
    trim(name);

    for (i = 0; i < ctx->candCount; i++)
        if (ListView_GetCheckState(lv, i)) checked++;

    _snwprintf(plan, 1024,
        L"実行内容 (この順番で実行します):\r\n"
        L"  1. チェックした旧デバイス %d 件を削除 (直前に .reg を書き出します)\r\n"
        L"  2. PnP 状態の反映を待って再列挙\r\n"
        L"  3. 残す対象を取り直す\r\n"
        L"  4. 残す対象の名前を「%s」に変更\r\n"
        L"  5. もう一度取り直して、名前が保たれたか検証\r\n"
        L"※ この画面を開く前に選ぶのは「残したいほう」です。"
        L"ふつうは接続中のものを選びます。",
        checked, name[0] ? name : L"(未入力)");
    plan[1023] = 0;
    set_text(dlg, IDC_CU_PLAN, plan);
}

/* 実行本体 (設計書 24.3 の順序を守る) */
static void cleanup_execute(HWND dlg, CleanupCtx *ctx)
{
    HWND lv = GetDlgItem(dlg, IDC_CU_LIST);
    WCHAR newName[DNM_MAX_NAME];
    WCHAR summary[2048];
    WCHAR keepId[MAX_DEVICE_ID_LEN];
    DeviceInfo target;
    int i, removed = 0;
    OpResultCode overall = OPR_OK;

    get_text(dlg, IDC_CU_NEWNAME, newName, DNM_MAX_NAME);
    trim(newName);
    if (!newName[0]) {
        MessageBoxW(dlg, L"新しい名前を入力してください。", L"旧インスタンスを整理",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    summary[0] = 0;
    dnm_strcpy(keepId, MAX_DEVICE_ID_LEN,
               ctx->list->items[ctx->targetIndex].instanceId);

    /* --- 1. 旧デバイスの削除 ----------------------------------------- */
    for (i = 0; i < ctx->candCount; i++) {
        const DeviceInfo *d;
        OpResult r;
        WCHAR line[600];

        if (!ListView_GetCheckState(lv, i)) continue;
        d = &ctx->list->items[ctx->cand[i].index];

        dnm_remove_device(d, &r);
        dnm_history_log_remove(d, &r);

        if (r.code == OPR_OK) removed++;
        else overall = worse(overall, r.code);

        _snwprintf(line, 600, L"削除 [%s] %s\n    %s",
                   r.code == OPR_OK ? L"OK" : L"NG", d->displayName, r.message);
        line[599] = 0;
        append_line(summary, 2048, line);
    }

    /* --- 2. 再列挙 ---------------------------------------------------- */
    if (removed > 0) {
        dnm_rescan_devices();
        Sleep(500);   /* PnP 側の反映を少し待つ */
    }

    /* --- 3. 対象を取り直す -------------------------------------------- */
    {
        const DeviceInfo *orig = &ctx->list->items[ctx->targetIndex];
        WCHAR instanceId[MAX_DEVICE_ID_LEN];
        dnm_strcpy(instanceId, MAX_DEVICE_ID_LEN, orig->instanceId);

        if (!dnm_refetch(instanceId, &target)) {
            append_line(summary, 2048,
                        L"\n対象デバイスを取り直せませんでした。名前変更は行いません。");
            MessageBoxW(dlg, summary, L"旧インスタンスを整理", MB_OK | MB_ICONERROR);
            return;
        }
        /* refetch は audio/net の紐づけを持たないので元の情報から引き継ぐ */
        memcpy(target.audio, orig->audio, sizeof(target.audio));
        target.audioCount   = orig->audioCount;
        target.hasNetConn   = orig->hasNetConn;
        target.netConnGuid  = orig->netConnGuid;
        dnm_strcpy(target.netAlias, DNM_MAX_NAME, orig->netAlias);
    }

    /* --- 4. リネーム --------------------------------------------------- */
    {
        RenamePlan plan;
        OpResult r;

        ZeroMemory(&plan, sizeof(plan));
        plan.audioRIndex = plan.audioCIndex = -1;

        plan.renamePnp = TRUE;
        dnm_strcpy(plan.pnpName, DNM_MAX_NAME, newName);

        if (target.kind == DK_NetworkAdapter && target.hasNetConn) {
            plan.renameNet = TRUE;
            dnm_strcpy(plan.netName, DNM_MAX_NAME, newName);
        }
        for (i = 0; i < target.audioCount; i++) {
            if (target.audio[i].flow == 0 && plan.audioRIndex < 0) {
                plan.audioRIndex = i;
                plan.renameAudioR = TRUE;
                dnm_strcpy(plan.audioRName, DNM_MAX_NAME, newName);
            }
            if (target.audio[i].flow == 1 && plan.audioCIndex < 0) {
                plan.audioCIndex = i;
                plan.renameAudioC = TRUE;
                dnm_strcpy(plan.audioCName, DNM_MAX_NAME, newName);
            }
        }

        apply_rename_plan(dlg, &target, &plan, &r);
        overall = worse(overall, r.code);
        append_line(summary, 2048, L"");
        append_line(summary, 2048, r.message);
    }

    free(target.hardwareIds);

    {
        UINT icon = (overall == OPR_OK)     ? MB_ICONINFORMATION
                  : (overall == OPR_FAILED) ? MB_ICONERROR
                                            : MB_ICONWARNING;
        MessageBoxW(dlg, summary, L"旧インスタンスを整理して名前を変更", MB_OK | icon);
    }

    /* 旧インスタンスを消しても、オーディオの連番はそれだけでは取れない
     * (実測)。取り直して連番が残っていたら、その場で続けられるようにする。
     * 勝手には実行しない。デバイスの削除と再検出を伴うため。 */
    {
        DeviceList after;
        DeviceInfo *now;
        dnm_list_init(&after);
        dnm_enumerate(&after);
        now = dnm_list_find(&after, keepId);
        if (now && now->audioCount > 0 && dnm_audio_has_serial(now))
            dnm_dlg_fix_audio_serial(dlg, now);
        dnm_list_free(&after);
    }

    ctx->executed = TRUE;
    EndDialog(dlg, IDOK);
}

static INT_PTR CALLBACK cleanup_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    CleanupCtx *ctx = (CleanupCtx *)GetWindowLongPtrW(dlg, DWLP_USER);

    switch (msg) {
    case WM_INITDIALOG: {
        const DeviceInfo *t;
        WCHAR buf[700], base[DNM_MAX_NAME];

        ctx = (CleanupCtx *)lp;
        SetWindowLongPtrW(dlg, DWLP_USER, (LONG_PTR)ctx);
        t = &ctx->list->items[ctx->targetIndex];

        SetWindowTextW(dlg, L"旧インスタンスを整理して名前を変更");
        set_text(dlg, IDOK, L"実行");
        set_text(dlg, IDCANCEL, L"キャンセル");
        set_text(dlg, IDC_CU_BASE, L"連番を除去した名前");

        /* 表示名だけだと、同じ製品が複数あるときにどれを指しているのか
         * 分からない (指摘)。別名と接続状態も並べる。 */
        _snwprintf(buf, 700,
                   L"残す対象 (このデバイスは削除しません。名前だけ変更します)\r\n"
                   L"    表示名  : %s\r\n"
                   L"    別名    : %s\r\n"
                   L"    状態    : %s\r\n"
                   L"    Instance: %s",
                   t->displayName,
                   dnm_alias_text(t)[0] ? dnm_alias_text(t) : L"(なし)",
                   present_text(t->isPresent),
                   t->instanceId);
        buf[699] = 0;
        set_text(dlg, IDC_CU_TARGET, buf);

        set_text(dlg, IDC_CU_LBL1,
                 L"削除する候補 (チェックしたものだけを削除します。"
                 L"上の「残す対象」は消えません)");
        set_text(dlg, IDC_CU_LBL2, L"残す対象に付ける新しい名前:");

        cleanup_fill_list(dlg, ctx);

        dnm_guess_base_name(t->displayName, base, DNM_MAX_NAME);
        set_text(dlg, IDC_CU_NEWNAME, base);
        cleanup_update_plan(dlg, ctx);

        if (ctx->candCount == 0)
            set_text(dlg, IDC_CU_LBL1,
                     L"同一ハードウェア候補は見つかりませんでした。名前の変更だけを行えます。");
        return TRUE;
    }

    case WM_NOTIFY: {
        LPNMHDR nh = (LPNMHDR)lp;
        if (nh->idFrom == IDC_CU_LIST && nh->code == LVN_ITEMCHANGED) {
            LPNMLISTVIEW nlv = (LPNMLISTVIEW)lp;
            if (nlv->uChanged & LVIF_STATE)
                cleanup_update_plan(dlg, ctx);
        }
        break;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_CU_BASE: {
            const DeviceInfo *t = &ctx->list->items[ctx->targetIndex];
            WCHAR base[DNM_MAX_NAME];
            dnm_guess_base_name(t->displayName, base, DNM_MAX_NAME);
            set_text(dlg, IDC_CU_NEWNAME, base);
            cleanup_update_plan(dlg, ctx);
            return TRUE;
        }
        case IDC_CU_NEWNAME:
            if (HIWORD(wp) == EN_CHANGE) cleanup_update_plan(dlg, ctx);
            break;
        case IDOK:
            cleanup_execute(dlg, ctx);
            return TRUE;
        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

/* ------------------------------------------------------------------ */
/* 設定 (バックアップ先の指定)                                         */
/*                                                                     */
/* 設定ファイルは実行ファイルと同じフォルダに置く (本人の指定)。        */
/* 置き場所を画面に出しておく。Program Files 配下に置いた場合は書けない  */
/* ことがあり、そのときは保存時に分かるようにする。                     */
/* ------------------------------------------------------------------ */
static int CALLBACK browse_init(HWND dlg, UINT msg, LPARAM lp, LPARAM data)
{
    /* 現在の設定値を初期選択にする */
    if (msg == BFFM_INITIALIZED && data)
        SendMessageW(dlg, BFFM_SETSELECTIONW, TRUE, data);
    return 0;
}

static void settings_browse(HWND dlg)
{
    BROWSEINFOW bi;
    LPITEMIDLIST idl;
    WCHAR cur[MAX_PATH], picked[MAX_PATH];

    get_text(dlg, IDC_ST_EDIT_DIR, cur, MAX_PATH);

    ZeroMemory(&bi, sizeof(bi));
    bi.hwndOwner = dlg;
    bi.lpszTitle = L"バックアップ先のフォルダを選んでください";
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    bi.lpfn      = browse_init;
    bi.lParam    = (LPARAM)cur;

    idl = SHBrowseForFolderW(&bi);
    if (!idl) return;
    if (SHGetPathFromIDListW(idl, picked))
        set_text(dlg, IDC_ST_EDIT_DIR, picked);
    CoTaskMemFree(idl);
}

static INT_PTR CALLBACK settings_proc(HWND dlg, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_INITDIALOG: {
        Config c;
        WCHAR ini[MAX_PATH], buf[MAX_PATH + 120], def[MAX_PATH];

        dnm_config_load(&c);
        dnm_config_path(ini, MAX_PATH);
        dnm_default_backup_dir(def, MAX_PATH);

        SetWindowTextW(dlg, L"設定");
        set_text(dlg, IDC_ST_CHK_ENABLE,
                 L"名前の変更・デバイスの削除の直前に、対象のレジストリキーを書き出す");
        CheckDlgButton(dlg, IDC_ST_CHK_ENABLE,
                       c.backupEnabled ? BST_CHECKED : BST_UNCHECKED);

        set_text(dlg, IDC_ST_LBL_DIR, L"バックアップ先フォルダ:");
        set_text(dlg, IDC_ST_EDIT_DIR, c.backupDir);
        set_text(dlg, IDC_ST_BROWSE, L"参照...");

        _snwprintf(buf, MAX_PATH + 120, L"設定ファイル: %s", ini);
        buf[MAX_PATH + 119] = 0;
        set_text(dlg, IDC_ST_LBL_INI, buf);

        _snwprintf(buf, MAX_PATH + 120,
                   L"空欄にすると既定値 (%s) を使います。\n"
                   L"フォルダが無ければ書き出すときに作ります。\n"
                   L"書き出したファイルのパスは、実行後の画面に表示します。", def);
        buf[MAX_PATH + 119] = 0;
        set_text(dlg, IDC_ST_HINT, buf);

        set_text(dlg, IDOK, L"保存");
        set_text(dlg, IDCANCEL, L"キャンセル");
        return TRUE;
    }

    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case IDC_ST_BROWSE:
            settings_browse(dlg);
            return TRUE;

        case IDOK: {
            Config c;
            ZeroMemory(&c, sizeof(c));
            c.backupEnabled =
                (IsDlgButtonChecked(dlg, IDC_ST_CHK_ENABLE) == BST_CHECKED);
            get_text(dlg, IDC_ST_EDIT_DIR, c.backupDir, MAX_PATH);
            trim(c.backupDir);
            if (c.backupDir[0] == 0)
                dnm_default_backup_dir(c.backupDir, MAX_PATH);

            if (!dnm_config_save(&c)) {
                WCHAR ini[MAX_PATH], m[MAX_PATH + 200];
                dnm_config_path(ini, MAX_PATH);
                _snwprintf(m, MAX_PATH + 200,
                           L"設定ファイルに書き込めませんでした。\n\n%s\n\n"
                           L"実行ファイルを書き込めない場所 (Program Files など) に\n"
                           L"置いている場合、別のフォルダへ移すと保存できます。", ini);
                m[MAX_PATH + 199] = 0;
                MessageBoxW(dlg, m, L"設定", MB_OK | MB_ICONWARNING);
                return TRUE;
            }
            EndDialog(dlg, IDOK);
            return TRUE;
        }

        case IDCANCEL:
            EndDialog(dlg, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

void dnm_dlg_settings(HWND parent)
{
    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_SETTINGS), parent,
                    settings_proc, 0);
}

BOOL dnm_dlg_cleanup(HWND parent, DeviceList *list, int targetIndex)
{
    CleanupCtx ctx;

    ZeroMemory(&ctx, sizeof(ctx));
    ctx.list = list;
    ctx.targetIndex = targetIndex;
    ctx.candCount = dnm_find_candidates(list, targetIndex, ctx.cand, MAX_CANDIDATES);

    DialogBoxParamW(g_hInst, MAKEINTRESOURCEW(IDD_CLEANUP), parent,
                    cleanup_proc, (LPARAM)&ctx);
    return ctx.executed;
}
