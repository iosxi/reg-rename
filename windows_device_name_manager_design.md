# Windows Device Name Manager 設計書

## 1. 概要

Windows 11 上で、PnP デバイスに付与された表示名を GUI から整理・変更するためのデスクトップアプリケーションを開発する。

主な対象は、同一ハードウェアを別の接続位置・インスタンスとして再認識した結果、Windows 上で次のような連番付き名称になったケースである。

```text
USB DAC
USB DAC (2)
USB DAC (3)
```

```text
Ethernet
Ethernet 2
Ethernet 3
```

想定する基本操作は以下。

1. Windows に登録されているデバイスを一覧表示する。
2. 「USB DAC (2)」など対象インスタンスを選択する。
3. もう使用しない旧インスタンス「USB DAC」を必要に応じて削除する。
4. 対象インスタンスの表示名を「USB DAC」など任意の文字列へ変更する。
5. 再列挙後も意図した名前が維持されるか確認する。

対象は USB オーディオに限定せず、ネットワークアダプター、USB 機器、Bluetooth 機器、HID、ストレージ等の PnP デバイスまで拡張可能な設計とする。

---

## 2. 重要な設計方針

### 2.1 「製品名」と「デバイスインスタンス」を別物として扱う

Windows の PnP では、同一製品でも接続位置やインスタンスが異なると別のデバイスインスタンスとして存在する場合がある。

したがって GUI では単純な名称だけでデバイスを識別してはならない。

各行を最低でも以下の情報で管理する。

- Friendly Name
- Device Instance ID
- Device Class
- Class GUID
- Hardware ID
- Manufacturer
- Location
- Container ID
- Present / Disconnected 状態
- 親デバイス / 子デバイス関係

`Device Instance ID` を内部的な主キーとして扱う。

### 2.2 「削除」と「ドライバーパッケージ削除」を分離する

ユーザーが求めている通常操作は、古いデバイスインスタンスを削除することであり、ドライバーそのものを Driver Store から削除することではない。

したがって通常の「削除」は以下を意味する。

> デバイスインスタンスを PnP ツリーからアンインストールする。

ドライバー パッケージ削除は別の高度な操作として扱い、初期版では原則提供しない。

Windows には `PnPUtil /remove-device` があり、デバイスインスタンス単位の削除に使用できる。また SetupAPI の `SetupDiCallClassInstaller(DIF_REMOVE)` / `DiUninstallDevice` でもデバイスを削除できる。citeturn774332search10turn774332search6turn774332search7

### 2.3 表示名変更はデバイス種別ごとに実装を分ける

「Windows 上の名前」は全デバイスで一種類ではない。

特に次を分離する。

- 一般 PnP デバイスの `DEVPKEY_Device_FriendlyName`
- オーディオエンドポイントの `PKEY_Device_FriendlyName`
- ネットワークアダプターのインターフェース名 / Interface Alias
- 必要に応じたデバイス固有の名称

一般 PnP デバイスでは `DEVPKEY_Device_FriendlyName` がデバイスインスタンスの Friendly Name に相当し、Windows API の `SetupDiSetDeviceProperty` で設定できる。citeturn774332search1turn774332search3

一方、オーディオエンドポイントには Core Audio の `PKEY_Device_FriendlyName` が存在するため、P​​nP devnode 名だけを書き換えても、Windows の「サウンド出力」画面に表示される名前が期待通り変わらない場合がある。citeturn344564search0turn344564search4

ネットワークアダプターは Windows の NetAdapter API / PowerShell に独自の名前管理機構があり、`Set-NetAdapter -Name` に相当する処理を利用する設計を検討する。citeturn774332search0

---

## 3. 想定 GUI

### 3.1 メイン画面

```text
┌──────────────────────────────────────────────────────────────────────────────┐
│ Windows Device Name Manager                                      [管理者] │
├──────────────────────────────────────────────────────────────────────────────┤
│ [更新] [非接続を表示] [種別▼] [検索.............................]          │
├──────┬──────────────────────┬────────────────┬────────────┬───────────────┤
│状態  │表示名                │デバイス種別    │接続状態    │Instance ID    │
├──────┼──────────────────────┼────────────────┼────────────┼───────────────┤
│ ●    │USB DAC               │Audio           │接続中      │USB\VID_...    │
│ ●    │USB DAC (2)           │Audio           │接続中      │USB\VID_...    │
│ ○    │USB DAC               │Audio           │未接続      │USB\VID_...    │
│ ●    │Ethernet 2            │Net             │接続中      │PCI\VEN_...    │
│ ○    │Ethernet              │Net             │未接続      │PCI\VEN_...    │
└──────┴──────────────────────┴────────────────┴────────────┴───────────────┘

[名前を変更] [デバイスを削除] [詳細] [再スキャン]
```

### 3.2 「名前を変更」ダイアログ

```text
対象:
    USB DAC (2)

種類:
    Audio Endpoint

新しい名前:
    [ USB DAC                                  ]

適用対象:
    (●) 現在のデバイスインスタンスのみ
    ( ) このデバイスの関連エンドポイントすべて

[適用] [キャンセル]
```

一般デバイスでは「現行インスタンスのみ」を基本とする。

オーディオでは、P​​nPデバイス名とオーディオエンドポイント名が別物であることを説明し、どちらを変更するか選択できるようにする。

---

## 4. デバイス一覧取得

### 4.1 基本 API

Windows ネイティブ API の SetupAPI / CfgMgr32 を基本とする。

主な API:

- `SetupDiGetClassDevs`
- `SetupDiEnumDeviceInfo`
- `SetupDiGetDeviceProperty`
- `SetupDiGetDeviceRegistryProperty`
- `CM_Get_DevNode_Property`
- `CM_Get_Device_ID`
- `SetupDiGetDeviceInstanceId`

Friendly Name については `DEVPKEY_Device_FriendlyName` または `SPDRP_FRIENDLYNAME` を取得する。Microsoft は `DEVPKEY_Device_FriendlyName` をデバイスインスタンスの friendly name として定義している。citeturn774332search1turn774332search5

### 4.2 一覧に含めるデバイス

初期設定では以下をすべて取得する。

- Present devices
- Non-present / disconnected devices
- Hidden devices

ただしシステム上重要なデバイスが大量に表示されるため、初期表示は以下を推奨する。

```text
表示:
[✓] 接続中
[ ] 未接続
[ ] システムデバイス
```

「未接続」を有効にすると、今回の用途で重要な古い USB インスタンスを発見できる。

### 4.3 デバイス種別

Class GUID や Setup Class を基に以下のカテゴリへ分類する。

- Audio
- Net
- USB
- Bluetooth
- HIDClass
- DiskDrive
- Display
- Camera
- Ports
- Storage
- System
- Other

カテゴリは UI 上の分類であり、実際のデバイス操作は Instance ID を基準に行う。

---

## 5. 旧デバイス削除機能

### 5.1 基本操作

ユーザーが

```text
USB DAC
USB DAC (2)
```

を見て「USB DAC」は旧ポートAの不要インスタンス、「USB DAC (2)」は現在利用中のポートB、と判断した場合、旧インスタンスを選択して「デバイスを削除」を実行する。

### 5.2 削除方式

第一候補:

```text
DiUninstallDevice()
```

または SetupAPI の DIF_REMOVE。

代替方式:

```text
pnputil /remove-device "<Instance ID>"
```

Windows 10 2004 以降の PnPUtil には `/remove-device` が存在する。Windows 11 では利用可能である。citeturn774332search10

`SetupDiRemoveDevice` はデバイスをシステムから削除し、関連するデバイスのハードウェア／ソフトウェア レジストリキー等も削除する。citeturn774332search8

### 5.3 削除確認

危険度の高い操作なので確認ダイアログを表示する。

```text
デバイスを削除しますか？

名前:
    USB DAC

Instance ID:
    USB\VID_1234&PID_5678\...

状態:
    未接続

この操作はデバイスインスタンスを Windows から削除します。
ドライバー本体は削除しません。

[削除] [キャンセル]
```

接続中デバイスの場合はさらに警告する。

```text
このデバイスは現在接続中です。
削除すると一時的に利用できなくなる可能性があります。
```

### 5.4 「ドライバーも削除」は初期版では付けない

ユーザーの目的は名称整理であるため、Driver Store まで削除する機能は別画面に隔離する。

---

## 6. 一般 PnP デバイスの名前変更

### 6.1 基本方式

`DEVPKEY_Device_FriendlyName` を対象の Device Instance に対して設定する。

Microsoft の仕様上、このプロパティは読み書き可能で、`SetupDiGetDeviceProperty` / `SetupDiSetDeviceProperty` により取得・設定できる。citeturn774332search1

想定処理:

```text
1. Instance ID から SP_DEVINFO_DATA を特定
2. 現在の Friendly Name を保存
3. DEVPKEY_Device_FriendlyName を設定
4. 必要ならデバイス再列挙 / UI 更新
5. 実際の Windows 表示を再取得して検証
```

### 6.2 レジストリ直接編集を第一選択にしない

デバイスのレジストリキーを書き換える方法ではなく、原則 SetupAPI のプロパティ設定 API を使用する。

理由:

- Windows の PnP 管理モデルに沿う
- レジストリ構造への依存を減らせる
- Instance ID 単位で対象を限定しやすい
- 32bit/64bit や将来の Windows 更新に強い

レジストリ直接編集は診断・互換性用のフォールバックとしてのみ検討する。

---

## 7. オーディオデバイスの名前変更

### 7.1 一般 PnP 名だけでは不十分

USB DAC では、例えば次のように複数レイヤーが存在する。

```text
USB PnP Device
    ↓
Audio Adapter
    ↓
Render Endpoint
    ↓
「USB DAC (2)」として表示
```

Windows Core Audio のオーディオエンドポイントには `PKEY_Device_FriendlyName` があり、これはエンドポイント自身の Friendly Name を表す。citeturn344564search0turn344564search4

したがって GUI では「Audio Endpoint」を別カテゴリとして検出し、一般 PnP Friendly Name とオーディオ表示名を混同しない。

### 7.2 オーディオ画面

```text
対象デバイス
    USB DAC (2)

PnP Device Name
    [ USB DAC (2) ]

Audio Output Endpoint
    [ USB DAC (2) ]

Audio Input Endpoint
    [ USB DAC (2) ]

[名前を適用]
```

### 7.3 変更方式

まず Core Audio の endpoint property store を取得する。

```text
IMMDeviceEnumerator
    ↓
IMMDevice
    ↓
IMMDevice::OpenPropertyStore
    ↓
PKEY_Device_FriendlyName
```

ただし Microsoft の最新ドキュメントでは、オーディオ endpoint 固有プロパティの一部はシステムが管理し、クライアントは読み取り専用とされているため、対象プロパティの書き込み可否を実機で検証する必要がある。citeturn344564search3

また、Microsoft のオーディオドライバー資料では、ユーザーによるエンドポイント名変更という概念自体は存在し、過去の Windows では `mmsys.cpl` から変更できたことが説明されている。citeturn344564search6

したがってオーディオについては、

1. documented API で変更可能か確認
2. 不可なら互換性のある Windows API / property mechanism を採用
3. 最終的に Windows 11 の実機テストで表示維持を確認

という実装方針にする。

---

## 8. ネットワークアダプター

ネットワークアダプターは一般 PnP Friendly Name と、Windows ネットワークで利用する Interface Alias が別に存在する。

例:

```text
PnP Friendly Name:
    Intel(R) Ethernet Controller

Interface Alias:
    Ethernet 2
```

今回ユーザーが問題としている「Ethernet 2」のような名称は、後者である可能性が高い。

そのため NetAdapter 系については専用処理を用意する。

### 8.1 変更方式

PowerShell / NetAdapter:

```powershell
Set-NetAdapter -Name "Ethernet 2" -NewName "Ethernet"
```

※実際のアプリでは PowerShell コマンドの単純な文字列実行より、可能なら CIM / WMI / NetAdapter API 相当の管理方式を使用する。

Microsoft の `Set-NetAdapter` はネットワークアダプターの `Name` を指定して基本プロパティを変更できる。citeturn774332search0

### 8.2 名前競合

ネットワーク名は同一スコープ内で重複できないため、

```text
Ethernet
Ethernet 2
```

が存在する状態で `Ethernet 2` → `Ethernet` を直接実行すると失敗する可能性がある。

したがって GUI の「旧デバイス削除→リネーム」を一連のトランザクションとして扱う。

```text
旧 Ethernet を削除
        ↓
Windows に状態反映
        ↓
Ethernet 2 → Ethernet
        ↓
再取得
        ↓
成功確認
```

---

## 9. 「旧デバイス削除してから名前変更」を一括実行する機能

今回の用途で最も重要な機能。

### 9.1 UI

対象行を右クリック:

```text
デバイス名を変更
旧インスタンスを整理して名前を変更
デバイスを削除
詳細
```

「旧インスタンスを整理して名前を変更」を選択すると、同一ハードウェア候補を自動検索する。

### 9.2 同一デバイス候補の判定

優先順位:

1. Container ID
2. Hardware ID
3. Compatible ID
4. Manufacturer
5. Device Class
6. PnP 親子関係
7. 接続場所 / Location
8. VID/PID
9. Name similarity

名前だけで自動削除してはいけない。

例えば

```text
USB DAC
USB DAC (2)
```

の2台が本当に同じ物理機器かどうかは名前だけでは判定できない。

したがって候補一覧を表示し、ユーザーに明示的に選択させる。

### 9.3 推奨 UI

```text
「USB DAC (2)」を「USB DAC」に変更します

現在の対象:
  ● USB DAC (2)
    Instance: USB\VID_1234&PID_5678\ABC...
    Location: Port_#0002.Hub_#0001

同一ハードウェア候補:
  ☑ USB DAC
    Instance: USB\VID_1234&PID_5678\XYZ...
    状態: 未接続
    Location: Port_#0001.Hub_#0001

  ☐ USB DAC (3)
    状態: 未接続

実行内容:
  1. ☑ の旧デバイスを削除
  2. 現在のデバイス名を「USB DAC」に変更
  3. デバイスを再列挙して確認

[実行] [キャンセル]
```

---

## 10. 名前の連番除去支援

手作業を減らすため、現在名から以下を検出する。

```text
USB DAC (2)
USB DAC (3)
Ethernet 2
Ethernet 3
Headphones #2
```

ただし「数字を削除すれば元の名前」というルールを自動適用してはいけない。

### 10.1 推奨処理

```text
現在名:
    USB DAC (2)

推定ベース名:
    USB DAC

[ USB DAC ]
```

ユーザーが編集して確定する。

「自動的に連番だけ除去」はオプション機能とする。

---

## 11. 操作の安全性

### 11.1 管理者権限

アプリ起動時に管理者権限を要求する方式を採用する。

推奨:

```xml
requestedExecutionLevel level="requireAdministrator"
```

ただし一覧表示だけなら非管理者でも可能な範囲があるため、将来的には

- 通常起動: 閲覧
- 管理者モード: 変更 / 削除

の分離も可能。

初期版では変更時に UAC 昇格する方式が望ましい。

### 11.2 削除対象の強い保護

以下は初期版で削除禁止にする。

- システムディスク関連
- Boot / System Critical
- 現在使用中の唯一のネットワークアダプター
- キーボード / マウスの唯一の入力デバイス
- USB root hub
- PCI root complex
- ACPI root devices
- Storage controller

「削除できない」ではなく「UI から削除操作を禁止」する。

### 11.3 現在接続中のデバイス

接続中デバイスの削除は可能であっても初期設定では二重確認する。

---

## 12. Undo / バックアップ

変更前の情報をローカル JSON に保存する。

例:

```json
{
  "timestamp": "2026-09-12T21:00:00+09:00",
  "instanceId": "USB\\VID_1234&PID_5678\\ABC",
  "classGuid": "{00000000-0000-0000-0000-000000000000}",
  "oldFriendlyName": "USB DAC (2)",
  "newFriendlyName": "USB DAC",
  "operation": "rename"
}
```

削除の場合も以下を記録する。

- Instance ID
- Friendly Name
- Hardware IDs
- Class GUID
- Manufacturer
- Location
- Container ID
- 削除日時

「復元」は単純なレジストリ復元ではなく、PnP 再列挙を促す設計とする。

---

## 13. 更新・再列挙

名前変更や削除後は UI を即時に書き換えるだけでは不十分。

```text
操作実行
  ↓
Windows API 成否確認
  ↓
PnP 再列挙
  ↓
デバイス一覧再取得
  ↓
実際の Friendly Name 再取得
  ↓
画面更新
```

PnPUtil には `/scan-devices` があり、Windows 10 2004 以降でデバイス変更の再スキャンに利用できる。citeturn774332search10

ただし、常に PnPUtil を外部プロセスとして呼ぶ必要はなく、SetupAPI / CfgMgr32 のイベント通知等も検討する。

---

## 14. デバイスイベント監視

常駐型の更新を行う場合は Windows のデバイス変更通知を監視する。

候補:

- `RegisterDeviceNotification`
- `WM_DEVICECHANGE`
- SetupAPI notification
- CfgMgr32 notification

これにより、USB の抜き差し時に一覧を自動更新する。

---

## 15. 技術スタック案

### 推奨

```text
Language: C#
Runtime: .NET 8 以上
UI: WPF
Platform: Windows 11 x64
```

理由:

- Windows API との相性が良い
- P/Invoke の利用が容易
- 管理者権限/UAC と組み合わせやすい
- GUI の構築コストが低い
- 将来的な PowerShell / CIM 連携が容易

### ネイティブ実装

より低レベルの PnP 操作を重視するなら C++ / Win32 も候補。

ただし初期版では C# + P/Invoke で十分。

---

## 16. モジュール構成

```text
DeviceNameManager
│
├─ UI
│   ├─ MainWindow
│   ├─ DeviceDetailsWindow
│   ├─ RenameDialog
│   └─ RemoveConfirmDialog
│
├─ DeviceModel
│   ├─ DeviceInfo
│   ├─ DeviceInstance
│   ├─ AudioEndpointInfo
│   └─ NetworkAdapterInfo
│
├─ DeviceDiscovery
│   ├─ SetupApiDeviceProvider
│   ├─ CfgMgrDeviceProvider
│   ├─ AudioDeviceProvider
│   └─ NetworkAdapterProvider
│
├─ DeviceOperations
│   ├─ GenericPnPDeviceOperator
│   ├─ AudioEndpointOperator
│   ├─ NetworkAdapterOperator
│   └─ DeviceRemovalOperator
│
├─ DeviceMatching
│   ├─ HardwareIdMatcher
│   ├─ ContainerIdMatcher
│   └─ SimilarNameMatcher
│
├─ Safety
│   ├─ ProtectedDeviceRules
│   └─ OperationValidator
│
└─ History
    ├─ OperationLog
    └─ BackupStore
```

---

## 17. データモデル

```csharp
public sealed class DeviceInfo
{
    public string InstanceId { get; init; } = "";
    public string FriendlyName { get; init; } = "";
    public string ClassName { get; init; } = "";
    public Guid? ClassGuid { get; init; }
    public string[] HardwareIds { get; init; } = [];
    public string? Manufacturer { get; init; }
    public string? Location { get; init; }
    public Guid? ContainerId { get; init; }
    public bool IsPresent { get; init; }
    public bool IsHidden { get; init; }
    public bool IsProtected { get; init; }
    public DeviceKind Kind { get; init; }
}
```

```csharp
public enum DeviceKind
{
    GenericPnP,
    AudioEndpoint,
    NetworkAdapter,
    Usb,
    Bluetooth,
    Hid,
    Storage,
    Display,
    Other
}
```

重要なのは `FriendlyName` を主キーにしないことである。

---

## 18. 操作 API 抽象化

```csharp
public interface IDeviceOperator
{
    bool CanHandle(DeviceInfo device);

    Task RenameAsync(
        DeviceInfo device,
        string newName,
        CancellationToken cancellationToken);

    Task RemoveAsync(
        DeviceInfo device,
        CancellationToken cancellationToken);
}
```

実装:

```text
GenericPnPDeviceOperator
AudioEndpointOperator
NetworkAdapterOperator
```

これにより、新しい種類のデバイスを後から追加できる。

---

## 19. 操作フロー

### 19.1 一般 PnP

```text
User
 ↓
Device List
 ↓
Select instance
 ↓
Rename
 ↓
SetupDiSetDeviceProperty
 ↓
Re-enumerate
 ↓
Verify
```

### 19.2 USB DAC

```text
Enumerate PnP
        ↓
Detect audio endpoints
        ↓
Associate endpoint ↔ PnP device
        ↓
Show both IDs / names
        ↓
Optional old-instance removal
        ↓
Rename appropriate audio/device property
        ↓
Re-enumerate Core Audio + PnP
        ↓
Verify Windows UI-visible name
```

### 19.3 Network

```text
Enumerate PnP
        ↓
Enumerate NetAdapter
        ↓
Associate Interface Alias ↔ PnP Instance ID
        ↓
Remove obsolete adapter if selected
        ↓
Rename interface alias
        ↓
Refresh NetAdapter/PnP
        ↓
Verify
```

---

## 20. 同一物理デバイス判定

自動削除を行う場合でも、候補抽出と削除決定を分ける。

### スコア例

```text
Container ID 一致       +40
Hardware ID 一致         +30
VID/PID 一致             +20
Class GUID 一致          +10
Manufacturer 一致         +5
Name similarity            +5

80以上: 強候補
60-79: 候補
59以下: 自動候補にしない
```

ただし、スコアは「同一である証明」ではない。

最終的な削除はユーザー操作とする。

---

## 21. 名前変更後の検証

名前変更 API が成功したことだけでは成功扱いにしない。

```text
API return = success
        ↓
再列挙
        ↓
新しい Friendly Name を取得
        ↓
期待値と比較
        ↓
一致 → 成功
不一致 → 警告
```

警告例:

```text
Windows API では変更に成功しましたが、
再列挙後に表示名が元に戻りました。

このデバイスのドライバーまたは Windows のデバイス管理機構が
名前を再生成している可能性があります。
```

これは特にオーディオやメーカー独自ドライバーを考慮して必要。

---

## 22. 初期版の対応範囲

### Must

- Windows 11 x64
- PnP デバイス一覧
- 接続中 / 未接続デバイス表示
- Friendly Name 表示
- Instance ID 表示
- Device Class 表示
- 検索
- 詳細表示
- 一般 PnP Friendly Name 変更
- デバイスインスタンス削除
- 操作確認ダイアログ
- 操作履歴
- 再列挙 / 再取得

### Should

- USB デバイス識別
- VID / PID 表示
- Container ID 表示
- Location 表示
- 同一ハードウェア候補検索
- 連番付き名称のベース名推定
- ネットワークアダプター名変更

### Could

- オーディオエンドポイント名変更
- Bluetooth デバイス名変更
- 一括整理
- 自動連番除去
- Undo
- エクスポート / インポート

### 初期版では避ける

- ドライバー パッケージ削除
- INF 書き換え
- システムデバイスの無条件操作
- レジストリの一括削除
- Device Store 全消去

---

## 23. 受け入れテスト

### Test 1: USB DAC

初期状態:

```text
USB DAC
USB DAC (2)
```

現在ポートBに `USB DAC (2)` が接続されている。

操作:

1. `USB DAC` を未接続デバイスとして表示
2. `USB DAC` を削除
3. `USB DAC (2)` を選択
4. 新名称 `USB DAC` を入力
5. 適用
6. デバイス一覧を再取得

期待:

```text
USB DAC
```

のみが残り、ポートBの実デバイスとして利用できる。

### Test 2: USB 再抜挿

ポートBから抜き、同じポートBへ再接続。

期待:

```text
USB DAC
```

の名前が維持される。

### Test 3: 別ポート

ポートCに接続。

期待:

- 新しい PnP Instance が発生する可能性がある
- 連番が再発する可能性がある
- アプリは別インスタンスであることを正しく表示する

### Test 4: Network

```text
Ethernet
Ethernet 2
```

旧 `Ethernet` を削除して `Ethernet 2` を `Ethernet` に変更。

期待:

Windows のネットワーク接続一覧で `Ethernet` と表示される。

### Test 5: 名前変更失敗

ドライバーが Friendly Name を再生成するデバイスで変更。

期待:

「API 成功」と「実表示維持」を別々に判定し、警告する。

---

## 24. 重要な実装上の注意

### 24.1 「USB DAC (2)」を単純にレジストリ検索して置換しない

検索対象は文字列ではなく、Device Instance ID で一意に特定する。

### 24.2 Device Instance ID と Hardware ID を混同しない

例:

```text
Hardware ID:
USB\VID_1234&PID_5678

Instance ID:
USB\VID_1234&PID_5678\7&12345678&0&2
```

Hardware ID は製品を識別するために使い、操作対象は Instance ID とする。

### 24.3 名前変更と削除を同時に行う場合でも順序を固定する

原則:

```text
旧インスタンスを削除
        ↓
PnP 状態反映
        ↓
対象を再取得
        ↓
名前変更
        ↓
再取得
        ↓
検証
```

これにより、旧名称との競合を極力避ける。

### 24.4 「(2)」が Windows 自動生成番号とは限らない

デバイスによってはメーカー側が本当に `(2)` を名前として提供している場合もある。

したがって UI に「連番」と断定して表示せず、単に Friendly Name として扱う。

---

## 25. 開発優先順位

### Phase 1

```text
SetupAPI 列挙
↓
一覧 UI
↓
詳細 UI
↓
一般 PnP Friendly Name 変更
↓
PnP デバイス削除
```

### Phase 2

```text
USB 詳細表示
↓
未接続デバイス整理
↓
同一 Hardware ID 候補抽出
↓
一括操作
```

### Phase 3

```text
Network Adapter 対応
↓
Ethernet 2 → Ethernet
```

### Phase 4

```text
Audio Endpoint 対応
↓
USB DAC (2) → USB DAC
```

### Phase 5

```text
履歴
Undo
自動整理
常駐監視
```

---

## 26. 実装上の最重要ポイント

このアプリは「デバイスの名前を書き換えるだけのレジストリエディター」として実装しない。

中核となる概念は以下である。

```text
                  ┌────────────────────┐
                  │ Windows PnP Device │
                  └─────────┬──────────┘
                            │
                      Instance ID
                            │
           ┌────────────────┼────────────────┐
           │                │                │
      Generic PnP       Audio           Network
           │                │                │
 FriendlyName       Audio Endpoint     Interface Alias
           │                │                │
 SetupAPI          Core Audio APIs      NetAdapter
```

つまり、GUI は統一するが、内部処理はデバイス種別ごとに適切な Windows 管理機構へ振り分ける。

この設計にしておけば、USB DAC の `USB DAC (2)`、ネットワークの `Ethernet 2`、その他 PnP デバイスの同様の名称問題を、同じ GUI から扱いつつ、Windows の各サブシステム固有の制約にも対応できる。

---

## 27. 参考 Microsoft ドキュメント

- `DEVPKEY_Device_FriendlyName` — デバイスインスタンスの Friendly Name と取得・設定 API
- SetupAPI `SetupDiGetDeviceRegistryProperty` / `SPDRP_FRIENDLYNAME`
- SetupAPI `SetupDiSetDeviceRegistryProperty`
- PnPUtil `/remove-device`, `/enum-devices`, `/scan-devices`
- Core Audio Device Properties / `PKEY_Device_FriendlyName`
- NetAdapter `Set-NetAdapter`

主要仕様確認先:

- https://learn.microsoft.com/en-us/windows-hardware/drivers/install/devpkey-device-friendlyname
- https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdigetdeviceregistrypropertya
- https://learn.microsoft.com/en-us/windows/win32/api/setupapi/nf-setupapi-setupdisetdeviceregistrypropertya
- https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax
- https://learn.microsoft.com/en-us/windows/win32/coreaudio/device-properties
- https://learn.microsoft.com/en-us/powershell/module/netadapter/set-netadapter
