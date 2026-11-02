# RenderDoc MCP Server

RenderDoc UI拡張機能として動作するMCPサーバー。AIアシスタントがRenderDocのキャプチャデータにアクセスし、グラフィックスデバッグを支援する。

## アーキテクチャ

```
Claude/AI Client (stdio)
        │
        ▼
MCP Server Process (Python + FastMCP 2.0)
        │ File-based IPC (%TEMP%/renderdoc_mcp/)
        ▼
RenderDoc Process (Extension)
```

RenderDoc内蔵のPythonにはsocketモジュールがないため、ファイルベースのIPCで通信を行う。

## セットアップ

### 1. RenderDoc拡張機能のインストール

```bash
python scripts/install_extension.py
```

拡張機能は `%APPDATA%\qrenderdoc\extensions\renderdoc_mcp_bridge` にインストールされる。

### 2. RenderDocで拡張機能を有効化

1. RenderDocを起動
2. Tools > Manage Extensions
3. "RenderDoc MCP Bridge" を有効化

### 3. MCPサーバーのインストール

```bash
uv tool install
uv tool update-shell  # PATHに追加
```

シェルを再起動すると `renderdoc-mcp` コマンドが使えるようになる。

> **Note**: `--editable` を付けると、ソースコードの変更が即座に反映される（開発時に便利）。
> 安定版としてインストールする場合は `uv tool install .` を使用。

### 4. MCPクライアントの設定

#### Claude Desktop

`claude_desktop_config.json` に追加:

```json
{
  "mcpServers": {
    "renderdoc": {
      "command": "renderdoc-mcp"
    }
  }
}
```

#### Claude Code

`.mcp.json` に追加:

```json
{
  "mcpServers": {
    "renderdoc": {
      "command": "renderdoc-mcp"
    }
  }
}
```

## 使い方

1. RenderDocを起動し、キャプチャファイル (.rdc) を開く
2. MCPクライアント (Claude等) から RenderDoc のデータにアクセス

## MCPツール一覧

| ツール | 説明 |
|--------|------|
| `get_capture_status` | キャプチャの読み込み状態を確認 |
| `get_draw_calls` | ドローコール一覧を階層構造で取得 |
| `get_frame_summary` | フレーム統計（draw/texture/buffer 数等）を取得 |
| `get_draw_call_details` | 特定のドローコールの詳細情報を取得 |
| `get_action_timings` | GPU タイミング計測値を取得 |
| `find_draws_by_shader` / `find_draws_by_texture` / `find_draws_by_resource` | シェーダー名・テクスチャ名・リソースIDでドローコールを逆引き |
| `get_shader_info` | シェーダーのソースコード・定数バッファの値を取得 |
| `get_shader_source` / `apply_shader_replacement` / `remove_shader_replacement` | シェーダーの編集可能ソース取得・置き換え・復元（再リプレイ付き） |
| `get_buffer_contents` | バッファの内容を取得 (Base64) |
| `get_texture_info` | テクスチャのメタデータを取得 |
| `get_texture_data` | テクスチャのピクセルデータを取得 (Base64) |
| `get_pipeline_state` | パイプライン状態を取得 |
| `list_captures` / `open_capture` | .rdc ファイル一覧・キャプチャを開く |
| `list_meshes` / `export_mesh` / `export_material` / `export_frame` | ドローコールのメッシュ(.glb)・マテリアル(.png+.json)をエクスポート（`manifest.json` で共有管理） |
| `verify_transforms` | エクスポート変換の妥当性を draw 単位で交差検証（M\*obj vs inv(VP)\*clip） |
| `build_scene` | エクスポート済みメッシュ/マテリアルを Blender で `scene.blend` に組み立て |
| `preview_scene` | 組み立てた .blend をヘッドレス Blender (Eevee) でプレビュー PNG レンダリング |
| `save_frame_image` | 指定イベントの出力ターゲットを PNG 保存（ground truth 取得用） |

## 使用例

### ドローコール一覧の取得

```
get_draw_calls(include_children=true)
```

### シェーダー情報の取得

```
get_shader_info(event_id=123, stage="pixel")
```

### パイプライン状態の取得

```
get_pipeline_state(event_id=123)
```

### テクスチャデータの取得

```
# 2Dテクスチャのmip 0を取得
get_texture_data(resource_id="ResourceId::123")

# 特定のmipレベルを取得
get_texture_data(resource_id="ResourceId::123", mip=2)

# キューブマップの特定の面を取得 (0=X+, 1=X-, 2=Y+, 3=Y-, 4=Z+, 5=Z-)
get_texture_data(resource_id="ResourceId::456", slice=3)

# 3Dテクスチャの特定の深度スライスを取得
get_texture_data(resource_id="ResourceId::789", depth_slice=5)
```

### バッファデータの部分取得

```
# バッファ全体を取得
get_buffer_contents(resource_id="ResourceId::123")

# オフセット256から512バイト取得
get_buffer_contents(resource_id="ResourceId::123", offset=256, length=512)
```

### フレームの 3D シーン再構築（Blender）

```
# 1. 対象 draw を選択（例: 不透明パスのみ）
list_meshes(marker_filter="DrawOpaque", min_indices=100)

# 2. メッシュ + マテリアルを一括エクスポート（~/Documents/rd_frame_export）
export_frame(marker_filter="DrawOpaque", min_indices=100, fresh=True)

# 3. 変換行列を交差検証（MISMATCH が出たらその draw を個別調査）
verify_transforms(marker_filter="DrawOpaque", min_indices=100)

# 4. Blender で scene.blend を組み立て（Blender 3.6+ 必須）
build_scene()

# 5. ground truth とプレビューをレンダリングして目視比較
save_frame_image(event_id=<最終合成 pass の eid>)
preview_scene()                      # 全体俯瞰
preview_scene(camera={"location": [x,y,z], "forward": [x,y,z],
                      "vfov_deg": 52.3, "width": 610, "height": 1356})
```

座標系メモ: Unity 系キャプチャはエクスポート時に z 反転で glTF 化され、
Blender 組み立て時に RotX(90°) が頂点へ焼き込まれる。ノード変換は共役
（`R @ m @ R⁻¹`）で適用されるため、Unity (x,y,z) は Blender (x,z,y) に対応する。

## 要件

- Python 3.10+
- [uv](https://docs.astral.sh/uv/)
- RenderDoc 1.20+

> **Note**: 動作確認はWindows + DirectX 11環境でのみ行っています。
> Linux/macOS + Vulkan/OpenGL環境でも動作する可能性がありますが、未検証です。

## ライセンス

MIT
