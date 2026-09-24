# 视频导出质量等级对应配置

`ScreenRecorder.QualityLevel` / `QualityLevel`（native）取值 0–3，各编码器映射到自身最合适的恒定质量模式。除 `h264_mf` 外均与分辨率/帧率无关；`h264_mf` 只能走码率，按 `w·h·fps` 相对 1080p60 缩放。

## 配置总表

| 编码器 | 码控模式 | Low (0) | Medium (1) | High (2) | Ultra (3) |
|---|---|---|---|---|---|
| libx264   | CRF   | 28 | 23 | 18 | 14 |
| h264_nvenc | CQ    | 30 | 24 | 18 | 14 |
| h264_qsv  | ICQ   | 32 | 25 | 18 | 14 |
| h264_amf  | QVBR  | 30 | 24 | 18 | 14 |
| h264_mf   | 码率 (Mbps @1080p60) | 4 | 8 | 16 | 32 |
| h264_videotoolbox (macOS) | 码率 (Mbps @1080p60) | 4 | 8 | 16 | 32 |

> CRF / CQ / ICQ / QVBR：数值越小 -> 质量越高、文件越大。
> h264_mf 码率：数值越大 -> 质量越高。

## 各编码器 FFmpeg 选项

以下选项在 `avcodec_open2` 之前通过 `av_opt_set` / `av_opt_set_int` 写入 `ctx->priv_data`。

### libx264
```
preset = medium
tune   = zerolatency
crf    = {28, 23, 18, 14}
```

### h264_nvenc
```
delay  = 0
rc     = vbr
preset = p7
cq     = {30, 24, 18, 14}
```

### h264_qsv
```
rate_control    = icq
global_quality  = {32, 25, 18, 14}
```
> 若驱动/FFmpeg 版本不支持 `global_quality`，可回退试 `icq_quality`。

### h264_amf
```
rc                 = qvbr
qvbr_quality_level = {30, 24, 18, 14}
```
> 旧版驱动若无 `qvbr`，回退 `rc = vbr_peak`。

### h264_mf
```
ctx->bit_rate = base[quality] * (width * height * fps) / (1920 * 1080 * 60)
base = {4_000_000, 8_000_000, 16_000_000, 32_000_000}
```
> Media Foundation 经 FFmpeg 暴露的质量控制极少，只能用码率。direct D3D11 模式额外设 `hw_encoding = 1`。

### h264_videotoolbox（macOS，`Native/RenderingOut`）
```
ctx->bit_rate = 与 h264_mf 相同的码率阶梯（按 w·h·fps 相对 1080p60 缩放）
profile       = high
```
> **不设** `AV_CODEC_FLAG_LOW_DELAY`：VideoToolbox 会因此启用低延迟码控并主动丢帧（4 Mbps 1080p60 测试中 240 帧只剩 27 帧）。
> 恒定质量（`-q:v`）仅 Apple Silicon 支持，为兼容 Intel Mac 统一使用码率。

## 备注

- **libx264 保留 `tune=zerolatency`、h264_nvenc 保留 `delay=0`**：录制是同步单帧提交，`send_frame_and_write_packets` 对 `EAGAIN` 只重试一次即返回错误；启用 lookahead / B 帧重排会导致 `avcodec_send_frame` 持续 `EAGAIN` 被误判为失败。仅把码率控制从 ABR 换成恒定质量，不破坏录制流程。
- **编码器回落顺序**（`initialize_encoder` 中）：`h264_nvenc(direct)` -> `h264_mf(direct)` -> `h264_nvenc` -> `h264_mf` -> `h264_amf` -> `h264_qsv` -> `libx264`。某编码器选项无效会自动跳到下一个。
- **macOS**（`Native/RenderingOut`，经 AsyncGPUReadback 提交 BGRA）：`h264_videotoolbox` -> `libx264`。可用环境变量 `RENDERINGOUT_ENCODER=libx264` 强制指定。
- `max_b_frames = 0`、`AV_CODEC_FLAG_LOW_DELAY` 保留不变；颜色矩阵按高度 ≥720 用 BT.709，否则 SMPTE170M。
- 1080p60 音游默认推荐 **High**（x264 CRF 18 / NVENC CQ 18 / QSV ICQ 18 / AMF QVBR 18 / MF 16 Mbps），肉眼几乎无压缩痕迹且体积合理。
