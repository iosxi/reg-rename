# Windows Device Name Manager

`windows_device_name_manager_design.md` を C + Win32 API で実装したもの。
`USB DAC (2)` や `Ethernet 2` のような連番付きデバイス名を整理する GUI ツール。

```
build/DeviceNameManager.exe          配布用 (起動時に UAC 昇格)
build/DeviceNameManager-uitest.exe   画面確認用 (asInvoker。変更・削除は失敗する)
build/smoke.exe                      非 GUI の読み取り専用テスト
```

```
src/         C ソースと Win32 リソース
Makefile     MinGW-w64 (gcc) 用
Makefile.msvc + build.bat   MSVC 用
tests/       非 GUI テスト
```

---

## ビルド

MSVC と MinGW-w64 のどちらでもビルドできる。**既定は MSVC**。

### MSVC (推奨)

```bat
build.bat            :: 配布用
build.bat uitest     :: 画面確認用 (UAC なし)
build.bat test       :: 非 GUI テスト
build.bat clean
```

`build.bat` が `vswhere` で Visual Studio を探して `vcvars64.bat` を
読み込むので、素のコマンドプロンプトからそのまま叩ける。

検証環境: Visual Studio Build Tools 2022 (17.14, cl 19.44) +
Windows SDK 10.0.26100 / Windows 11 26200。**警告ゼロでビルドできる。**

### MinGW-w64

```sh
mingw32-make            # 配布用
mingw32-make uitest     # 画面確認用 (UAC なし)
mingw32-make debug      # デバッグ情報つき + コンソール
mingw32-make clean
mingw32-make -f tests/Makefile.smoke

build/smoke.exe             # 読み取りのみ
build/smoke.exe --history   # 履歴 JSON の書き込みも試す
```

検証環境: MinGW-W64 x86_64-ucrt-posix-seh (gcc 16.1.0)。

MinGW では下の警告が出るが、**exit code は 0 でビルドは成功している**。

```
ld.exe: .rsrc merge failure: multiple non-default manifests
```

MinGW が常にリンクする `default-manifest.o` と本アプリの `app.manifest`
が衝突するもの。生成物には本アプリのマニフェストが正しく入ることを
実測で確認済み (`FindResource(RT_MANIFEST, 1)` で取り出して内容を照合)。
gcc の spec が `%{!shared:%:if-exists(default-manifest.o%s)}` で無条件に
リンクするため、`-B` での差し替えや `LANGUAGE` 指定では回避できなかった
(いずれも実測)。**MSVC ではこの警告は出ない。**

### なぜ MSVC を既定にしたか

Windows SDK があると、MinGW 向けに書いていた回避策が減る。

| 項目 | MinGW | MSVC + SDK 10.0.26100 |
|---|---|---|
| `SetupDiSetDevicePropertyW` | ヘッダーに宣言が無く自前宣言 | setupapi.h に宣言あり |
| `DiUninstallDevice` | `libnewdev.a` に無い | `newdev.lib` にある |
| netcon の GUID | 実体がどのライブラリにも無く自前定義 | `uuid.lib` にある |
| マニフェスト | `.rsrc merge failure` が出る (対処済み) | 警告なし |
| `NcIsValidConnectionName` | `libnetshell.a` にある | **`netshell.lib` が SDK に無い** |
| Core Audio の GUID | ヘッダーが `DEFINE_GUID` で定義 | **宣言のみ。`uuid.lib` にも無い** |

下 2 行は MSVC が不利になる点だったので、次のように解いた。

- `DiUninstallDevice` と `NcIsValidConnectionName` は **実行時に DLL から
  取る** (`devops.c`)。インポートライブラリの有無に依存しなくなり、
  両ツールチェーンで同じ経路を通る。おまけに設計書 5.2 の第一候補である
  `DiUninstallDevice` を MinGW でも使えるようになった。
  実測で Windows 11 26200 の `newdev.dll` / `netshell.dll` に両方存在。
- Core Audio の GUID は `guids.c` で `#ifndef __MINGW32__` のときだけ
  定義する。値は MinGW のヘッダーに書かれているものを写した。
- マニフェストの `.rsrc merge failure` は、MinGW-w64 の spec が実行ファイルに
  必ず `default-manifest.o` (asInvoker) を足すのが原因で、放っておくと exe に
  マニフェストが 2 つ入る。spec を切るオプションが無いので、`Makefile` は
  `-B` で空の `default-manifest.o` を先に見つけさせて差し替えている。
  MSVC は `/MANIFEST:NO` で同じことをしている。

結果として**ソースは両対応のまま**で、MSVC では警告ゼロになる。

---

## 構成

```
src/
  dnm.h          共通の型と宣言 (設計書 17 章のデータモデル)
  guids.c        GUID / DEVPROPKEY / PROPERTYKEY の実体 (INITGUID はここだけ)
  util.c         文字列・リスト・権限・ベース名推定 (設計書 10 章)
  devlist.c      列挙と Audio/Net の紐づけ (設計書 4, 7, 8 章)
  devops.c       リネーム・削除・再列挙・検証 (設計書 5, 6, 7, 8, 13, 21 章)
  matching.c     同一ハードウェア候補のスコアリング (設計書 20 章)
  safety.c       削除禁止デバイスの判定 (設計書 11.2)
  history.c      操作履歴 JSON (設計書 12 章)
  ui_main.c      メインウィンドウ (設計書 3.1)
  ui_dlg.c       各ダイアログ (設計書 3.2, 5.3, 7.2, 9.3)
  app.rc         ダイアログテンプレート (文字列は持たない)
  app.manifest   requireAdministrator + comctl32 v6 + PerMonitorV2 DPI
  app-uitest.manifest  同上だが asInvoker (画面確認用)
tests/
  smoke.c        実機を読み取るだけのテスト
```

`app.rc` / `build.bat` / `Makefile.msvc` は **ASCII のみ**にしてある。
`rc.exe`・`cmd.exe`・`nmake.exe` はいずれも OEM コードページ (日本語環境では
932) でファイルを読むため、UTF-8 の日本語を置くと構文が壊れる。実際に
`cmd` では `'...' is not recognized`、`nmake` では `U1035 syntax error` になった。
画面に出る日本語はすべて C 側から `SetDlgItemTextW` で流し込んでいる。
GNU make にはこの問題が無いので、MinGW 用 `Makefile` は日本語コメントのまま。

設計書 26 章の「GUI は統一し、内部は種別ごとに振り分ける」構造:

| 種別 | 変更先 | 使う API |
|---|---|---|
| 一般 PnP | Friendly Name | `SetupDiSetDevicePropertyW(DEVPKEY_Device_FriendlyName)`<br>失敗時 `SetupDiSetDeviceRegistryPropertyW(SPDRP_FRIENDLYNAME)` |
| オーディオ | エンドポイント名 | `IMMDevice::OpenPropertyStore(STGM_READWRITE)` → **`PKEY_Device_DeviceDesc`** → `Commit` |
| ネットワーク | 接続名 (Interface Alias) | 1. `NciSetConnectionName` (nci.dll から実行時取得)<br>2. 駄目なら `INetConnection::Rename`<br>どちらも駄目なら両方のエラーコードを表示 |
| 削除 | デバイスインスタンス | `DiUninstallDevice` (newdev.dll から実行時取得)<br>取れなければ `SetupDiCallClassInstaller(DIF_REMOVE)` |
| 再列挙 | — | `CM_Reenumerate_DevNode` |

### 変更前のレジストリを書き出す

名前の変更・デバイスの削除を行う**直前**に、対象のレジストリキーを `.reg` へ
書き出す。書き出したファイルのフルパスは、実行後の結果画面に必ず出す。

| 操作 | 書き出すキー |
|---|---|
| PnP デバイス名 | `HKLM\SYSTEM\CurrentControlSet\Enum\<Instance ID>` |
| ネットワーク接続名 | `HKLM\SYSTEM\CurrentControlSet\Control\Network\{4D36E972-…}\{NetCfgInstanceId}\Connection` |
| オーディオ endpoint 名 | `HKLM\SOFTWARE\…\MMDevices\Audio\{Render\|Capture}\{endpoint}` |
| デバイスの削除 | `HKLM\SYSTEM\CurrentControlSet\Enum\<Instance ID>` |

エクスポートは `reg.exe export` に任せている。`.reg` のテキスト形式は
`REG_MULTI_SZ` の `hex(7)`、`REG_EXPAND_SZ` の `hex(2)`、既定値の `@=` 表記、
UTF-16LE + BOM といった細かい決まりがあり、自前で組み立てて取り違えると
「バックアップはあるのに戻せない」という最悪の壊れ方をする。`reg.exe` は
どの Windows にも入っていて、出力はダブルクリックで戻せる。
終了コードだけでは信用せず、ファイルが実際にできたかも確認している。

バックアップ先は**設定画面**（メイン画面の「設定...」ボタン）で指定する。
設定ファイルは**実行ファイルと同じフォルダ**の `DeviceNameManager.ini`。

```ini
[Backup]
Enabled=1
Dir=C:\path\to\backup
```

未設定なら `<exe のあるフォルダ>\RegBackup` を使う。書き出しに失敗しても
操作自体は止めない。止めると、バックアップ先を設定していないだけで何も
できなくなるため。代わりに失敗の理由を結果画面に必ず出す。

### オーディオの「エンドポイント名」は組み立て結果で、直接は書けない

「サウンド」画面に出る名前 (`PKEY_Device_FriendlyName`) は、Windows が

```
<PKEY_Device_DeviceDesc> (<PKEY_DeviceInterface_FriendlyName>)
```

と組み立てた**読み取り専用**の値。実測 (Windows 11 / 2026-09-13、
**管理者でも非管理者でも同じ結果**):

| プロパティ | `SetValue` |
|---|---|
| `PKEY_Device_FriendlyName` | `0x80070005` |
| `PKEY_DeviceInterface_FriendlyName` | `0x80070005` |
| `PKEY_Device_DeviceDesc` | `0`（非管理者でも通る） |

`MMDevices` のレジストリキーは Administrators に `SetValue` を許可しており、
ACL の問題ではない。`FriendlyName` へ書こうとしたことが原因で、昇格しても
直らない。そのため書き込み先は `PKEY_Device_DeviceDesc` にしてある。

括弧の中はデバイス側の名前なので、そこを変えたいときは PnP 名の変更が要る。
`2- ` のような連番が付いている場合は、同じ製品の古いインスタンスが元の名前を
押さえているので、「旧インスタンスを整理して名前を変更」で片付ける。

### オーディオの連番 ("2- " 等) を解消する

同じ製品を挿し直すと、Windows は名前の衝突を避けるため括弧の中に連番を付ける。

```
SPDIF インターフェイス (3- FX-D03J)
                       ^^ これ
```

**この連番はレジストリのどこにも保存されていない。** 実測で、`MMDevices` /
`Enum\SWD\MMDEVAPI` / `Enum\USB` 配下をバイナリ値まで UTF-16 として復号して
全走査したが `4- FX` という文字列は存在しなかった。MMDevAPI が実行時に
付けているので、**名前を書き換えても消えない**。

実測で効いたのは次の順序だけで、アプリの「オーディオの連番を解消」が
これを行う (右クリックメニュー、および旧インスタンス整理の完了後に提案)。

1. 旧インスタンスを削除する — **これだけでは連番は残る**
2. `AudioEndpointBuilder` を再起動する — `MMDevices` に残っていた死んだ
   エンドポイント登録が刈り取られる。**この時点でもまだ連番は残る**
3. 対象デバイスを削除して再検出させる — エンドポイントが作り直され、
   衝突相手が居ないので番号が付かない

効かなかったもの (実測):

| 試したこと | 結果 |
|---|---|
| `pnputil /restart-device` | 既存のエンドポイントを使い回すため連番はそのまま |
| SWD エンドポイントの PnP 名を直接書き換える | 一度は変わるが、サービス再起動で上書きし直される |
| `MMDevices` の古い登録を消す / 書き換える | 所有者が SYSTEM で Administrators に削除権が無く、昇格しても拒否される |

3 はデバイスの削除を伴うので自動では実行しない。確認画面を出し、削除の
直前に `.reg` を書き出す。

### ネットワーク接続名だけ経路が 2 つある理由

設計書は `INetConnection::Rename` を想定していたが、**Windows 11 26200 では
昇格しても `0x800702E4` (ERROR_ELEVATION_REQUIRED) を返して通らない**
(`TokenIsElevated=1` / 整合性レベル 高 を確認したうえでの実測)。
権限の問題ではなく、この API がそのビルドで機能していない。

代わりに `netsh interface set interface` が内部で呼んでいる
`nci.dll` の `NciSetConnectionName` を第一候補にした。実測で
非管理者は `5` (ACCESS_DENIED)、管理者は `0` を返し、`Get-NetAdapter` の
`InterfaceAlias` まで追従する。

ただしこれは 26200 での測定で、**他のビルドで `INetConnection::Rename` が
通らないと決まったわけではない**。そのため片方に賭けず、1 が駄目なら 2 も
試し、どちらも駄目なら両方のエラーコードと権限の状態をそのまま表示する。

レジストリ (`Control\Network\{class}\{guid}\Connection\Name`) の直接書き換えは
採らなかった。実測で値は変わるが `InterfaceAlias` が追従せず、名前が
二重管理になるため。

---

## 実装前に実測したこと

設計書の前提が実機で成り立つかを、コードを書く前に測った。結果として
設計書のままでは動かない箇所が 4 つ見つかっている。

### 1. FriendlyName を持つデバイスは 2 割しかない

全 636 台のうち `DEVPKEY_Device_FriendlyName` を持つのは **137 台**だけ。
残りは `DEVPKEY_Device_DeviceDesc` で表示するしかない。
表示名は FriendlyName → DeviceDesc → Instance ID の順にフォールバックする。

### 2. オーディオエンドポイントに `PKEY_Device_InstanceId` は無い

設計書が想定する経路 (エンドポイントから Instance ID を引く) は、
`GetValue` が `S_OK` を返すのに **値が `VT_EMPTY`** で使えなかった。

代わりに 2 経路を測って比較した:

| 紐づけ方式 | 解決できた数 |
|---|---|
| `{b3f8fa53-0004-438e-9003-51a46e139bfc},2` プロパティ (値は `{N}.<Instance ID>`) | **62 / 63** |
| `SWD\MMDEVAPI\<endpoint id>` devnode の `DEVPKEY_Device_Parent` | 8 / 63 |

両方成立した 8 件は値が完全に一致した。前者を主、後者を補助にしている。

### 3. ツールチェーンごとに足りないものが違う

上の「なぜ MSVC を既定にしたか」の表のとおり。MinGW と MSVC で
欠けているものが逆になる箇所があるため、
実行時解決と `#ifdef __MINGW32__` で両対応にしている。

### 3-2. UI 側で踏んだ Win32 の癖

画面を実機で動かして分かったもの。どれも静かに壊れるので記録しておく。

| 症状 | 原因 | 対処 |
|---|---|---|
| Instance ID の `&` が消える (`VID_262A&PID_9023` → `VID_262APID_9023`) | スタティックコントロールが `&` をニーモニック指定として食う | `SS_NOPREFIX` |
| 長い Instance ID が折り返して下の行に食い込む | `LTEXT` は既定で折り返す | 独立した行 + `SS_ENDELLIPSIS` |
| 欄を隠しても空白が残って間延びする | テンプレートの座標は固定 | `rename_reflow()` で詰め直し、ダイアログ自体も縮める |
| その reflow が効かない | `IsWindowVisible()` は親をたどるため、`WM_INITDIALOG` の時点ではどの子も FALSE になる | `GWL_STYLE & WS_VISIBLE` で自分のビットだけ見る |
| `ListView_SetItemText` が ANSI 版に解決される | `UNICODE` 未定義。MinGW は `-municode` が定義していたので気づきにくい | 両 Makefile で `-DUNICODE` を明示し、`dnm.h` に `#error` を置いた |

### 4. 複合デバイスがスコア 80 点に届いてしまう

設計書 20 章のスコアで実データを流すと、**キーボード兼マウスのような
複合デバイスが 80〜90 点**になる。Container ID / VID/PID / Class GUID /
Manufacturer / 名前がすべて揃うため。これらは「同じ製品の別インスタンス」
ではなく「同じ機器の別機能」なので、消してはいけない。

両者を分けられるのは **Hardware ID の一致**だけだった (複合デバイスは
`MI_00` / `MI_01` のように異なる)。そのため整理ダイアログが最初から
チェックを入れる条件を

> スコア 80 以上 **かつ** 未接続 **かつ** Hardware ID 一致

に絞っている。この条件で実機の既定チェック対象は 1074 件 → 946 件に減った。
チェックされていない候補も一覧には出るので、ユーザーが自分で選べる。

---

## テスト結果 (実機)

`build/smoke.exe` は実機のデバイスを読むだけで、何も変更しない。

```
[1] ベース名推定            6 項目すべて OK
[2] デバイス列挙            636 台 (接続中 365 / 未接続 271)
[3] Audio / Net の紐づけ    エンドポイント保持 15 台 / 接続名取得 3 台
[4] 保護ルール              USB ルートハブ 1 / システム 96
[5] 同一ハードウェア候補    候補あり 350 台、自己参照なし
[6] 表示フィルター          既定 256 台 → 未接続込み 524 台 → 全部 636 台
[7] 再取得による検証        20 件すべて一致
[8] 履歴 JSON               JSON として妥当、設計書 12 章のキーをすべて出力
失敗: 0 件
```

GUI は `DeviceNameManager-uitest.exe` を起動して UI ツリーで確認した
(画面撮影はせず、要素をテキストで読んでいる):

- 一覧・フィルター・検索・6 列のヘッダーが設計書 3.1 どおり表示される
- 検索で絞り込みが効く
- 「詳細」ダイアログが開く
- 「名前を変更」ダイアログが種別で切り替わり、欄数に応じて高さが変わる
  - 一般デバイス → PnP 名の 1 欄のみ
  - オーディオ → PnP 名「FX-D03J」+ 出力エンドポイント名
    「SPDIF インターフェイス (3- FX-D03J)」の 2 欄
    (入力エンドポイントが無いデバイスなので入力欄は非表示)
  - Instance ID が `USB\VID_262A&PID_9023&MI_01\8&63A6124&0&0001` のように
    `&` を含んだまま、1 行に収まって表示される

**未検証**: 名前変更と削除の実行そのもの。管理者権限が要り、実機の
デバイス構成を実際に変更するため、意図的に走らせていない。
設計書 23 章の受入テストは実機で手動実行が必要。

---

## 使い方

1. `build/DeviceNameManager.exe` を起動する (UAC が出る)。
2. 「未接続」にチェックを入れると、古いインスタンスが見えるようになる。
3. **残したいほう**のデバイスを選んで「旧インスタンスを整理して名前を変更」。
4. 同一ハードウェア候補が出るので、消してよいものだけチェックする。
5. 新しい名前を入れて「実行」。

削除 → 再列挙 → リネーム → 検証 の順で実行される (設計書 24.3)。

**選ぶのは「残したいほう」**。選んだデバイスは削除されず、名前だけが変わる。
消えるのは候補一覧でチェックしたものだけ。ふつうは接続中のものを選ぶ。

同じ製品が複数あると表示名は全部同じになるので、候補一覧には**別名**
(ネットワークなら接続名、オーディオならエンドポイント名) を出している。
`SPDIF インターフェイス (FX-D03J)` / `(2- FX-D03J)` / `(3- FX-D03J)` のように
連番が付くため、これが個体を見分ける手がかりになる。Instance ID は長すぎて
見比べに向かない。

候補の既定チェックは「スコア 80 以上 かつ 未接続 かつ Hardware ID 一致」に
限っている。複合デバイス (キーボード兼マウス等) は Container ID や VID/PID が
揃うため名前だけでは 80 点近くまで届いてしまい、それらは「同じ製品の別
インスタンス」ではなく「同じ機器の別機能」なので消してはいけない
(設計書 9.2)。既定でチェックが付かない候補は、別名と状態を見て判断する。

### 変更が反映されたかの判定

API が成功しただけでは成功扱いにしない (設計書 21 章)。
実行後に必ず取り直して、期待した名前になっているかを確認し、
戻ってしまった場合は警告を出す。

```
Windows API では変更に成功しましたが、
再取得すると表示名が「...」になっています。

このデバイスのドライバーまたは Windows のデバイス管理機構が
名前を再生成している可能性があります。
```

### 履歴

`%LOCALAPPDATA%\DeviceNameManager\history.json` に JSON 配列で追記される。
削除したデバイスの Hardware ID や Container ID も残すので、
復元が必要になったときの手掛かりになる。

---

## 初期版で入れていないもの

設計書 22 章「初期版では避ける」に従い、以下は実装していない。

- ドライバーパッケージの削除 (Driver Store)
- INF の書き換え
- レジストリの一括削除
- Undo の実行 (履歴の記録のみ)

また設計書 11.2 に従い、以下は UI から削除操作を禁止している
(「削除できない」ではなく「操作させない」)。

- System / Computer / Processor クラス
- ルート列挙子配下 (`HTREE\`, `ACPI_HAL\`, `ACPI\PNP0A` 等)
- USB ルートハブ
- ストレージコントローラー (HDC / SCSIAdapter)
- 接続中のボリュームとディスクドライブ
- 唯一のキーボード / マウス
- 唯一の接続中ネットワークアダプター
