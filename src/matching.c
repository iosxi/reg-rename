/*
 * matching.c - 同一物理デバイス候補の抽出 (設計書 20 章)
 *
 * スコアは「同一である証明」ではない。候補の並べ替えに使うだけで、
 * 最終的な削除判断は必ずユーザーに委ねる (設計書 9.2)。
 *
 *   Container ID 一致  +40
 *   Hardware ID 一致   +30
 *   VID/PID 一致       +20
 *   Class GUID 一致    +10
 *   Manufacturer 一致   +5
 *   名前の類似          +5
 *
 *   80 以上 : 強候補
 *   60-79   : 候補
 *   59 以下 : 自動候補にしない
 */
#include "dnm.h"

#define SCORE_MIN_CANDIDATE 60

extern const WCHAR *dnm_multisz_next(const WCHAR *cur);

/* MULTI_SZ 同士に共通要素があるか */
static BOOL multisz_intersects(const WCHAR *a, const WCHAR *b)
{
    const WCHAR *p, *q;
    if (!a || !b) return FALSE;
    for (p = a; p && *p; p = dnm_multisz_next(p))
        for (q = b; q && *q; q = dnm_multisz_next(q))
            if (_wcsicmp(p, q) == 0)
                return TRUE;
    return FALSE;
}

/* ベース名が一致するか (設計書 10 章の推定を流用) */
static BOOL base_name_similar(const WCHAR *a, const WCHAR *b)
{
    WCHAR ba[DNM_MAX_NAME], bb[DNM_MAX_NAME];
    if (!a[0] || !b[0]) return FALSE;
    dnm_guess_base_name(a, ba, DNM_MAX_NAME);
    dnm_guess_base_name(b, bb, DNM_MAX_NAME);
    return _wcsicmp(ba, bb) == 0;
}

static void append_reason(WCHAR *buf, size_t cap, const WCHAR *text)
{
    size_t len = wcslen(buf);
    if (len && len + 2 < cap) { buf[len++] = L','; buf[len++] = L' '; buf[len] = 0; }
    dnm_strcpy(buf + wcslen(buf), cap - wcslen(buf), text);
}

static int score_pair(const DeviceInfo *t, const DeviceInfo *c, WCHAR *reasons,
                      size_t cap, BOOL *hwidMatch)
{
    int s = 0;
    reasons[0] = 0;
    *hwidMatch = FALSE;

    if (t->hasContainerId && c->hasContainerId &&
        IsEqualGUID(&t->containerId, &c->containerId)) {
        s += 40;
        append_reason(reasons, cap, L"Container ID 一致");
    }
    if (multisz_intersects(t->hardwareIds, c->hardwareIds)) {
        s += 30;
        *hwidMatch = TRUE;
        append_reason(reasons, cap, L"Hardware ID 一致");
    }
    if (t->hasVidPid && c->hasVidPid && t->vid == c->vid && t->pid == c->pid) {
        s += 20;
        append_reason(reasons, cap, L"VID/PID 一致");
    }
    if (t->hasClassGuid && c->hasClassGuid &&
        IsEqualGUID(&t->classGuid, &c->classGuid)) {
        s += 10;
        append_reason(reasons, cap, L"Class GUID 一致");
    }
    if (t->manufacturer[0] && _wcsicmp(t->manufacturer, c->manufacturer) == 0) {
        s += 5;
        append_reason(reasons, cap, L"Manufacturer 一致");
    }
    if (base_name_similar(t->displayName, c->displayName)) {
        s += 5;
        append_reason(reasons, cap, L"名前が類似");
    }
    return s;
}

int dnm_find_candidates(const DeviceList *list, int targetIndex,
                        MatchCandidate *out, int maxOut)
{
    const DeviceInfo *t;
    int i, n = 0;

    if (targetIndex < 0 || targetIndex >= list->count) return 0;
    t = &list->items[targetIndex];

    for (i = 0; i < list->count && n < maxOut; i++) {
        const DeviceInfo *c = &list->items[i];
        WCHAR reasons[256];
        BOOL hwid = FALSE;
        int s;

        if (i == targetIndex) continue;
        if (c->protect != PROT_NONE) continue;   /* 保護デバイスは候補にしない */

        s = score_pair(t, c, reasons, sizeof(reasons) / sizeof(WCHAR), &hwid);
        if (s < SCORE_MIN_CANDIDATE) continue;

        out[n].index = i;
        out[n].score = s;
        out[n].hardwareIdMatch = hwid;
        dnm_strcpy(out[n].reasons, 256, reasons);
        n++;
    }

    /* スコア降順 + 未接続を優先 (整理対象は未接続の旧インスタンスが多い) */
    {
        int j, k;
        for (j = 1; j < n; j++) {
            MatchCandidate tmp = out[j];
            const DeviceInfo *dt = &list->items[tmp.index];
            int tkey = tmp.score + (dt->isPresent ? 0 : 5);
            for (k = j - 1; k >= 0; k--) {
                const DeviceInfo *dk = &list->items[out[k].index];
                int kkey = out[k].score + (dk->isPresent ? 0 : 5);
                if (kkey >= tkey) break;
                out[k + 1] = out[k];
            }
            out[k + 1] = tmp;
        }
    }
    return n;
}
