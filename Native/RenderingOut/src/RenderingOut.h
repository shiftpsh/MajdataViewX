#pragma once

/*
 * Portable RenderingOut: a graphics-API-free H.264/AAC mp4 writer.
 *
 * Unlike the Windows D3D11 build (which receives a native texture pointer),
 * this build receives CPU-side RGBA pixels produced by AsyncGPUReadback, so
 * the same source works under Metal, Vulkan and OpenGL.
 *
 * video_encoder_create / video_encoder_mux_audio / video_encoder_free keep
 * the exact signatures of the Windows DLL so ScreenRecorder.cs can share its
 * declarations; frame submission uses video_encoder_submit_rgba instead of
 * video_encoder_submit_frame.
 */

#include <stdint.h>

#if defined(_WIN32)
#define RENDERINGOUT_EXPORT __declspec(dllexport)
#define RENDERINGOUT_API __stdcall
#else
#define RENDERINGOUT_EXPORT __attribute__((visibility("default")))
#define RENDERINGOUT_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VideoEncoder VideoEncoder;

enum RenderingOutLogLevel
{
	RENDERINGOUT_LOG_INFO = 0,
	RENDERINGOUT_LOG_ERROR = 1
};

typedef void (RENDERINGOUT_API *RenderingOutLogCallback)(
	int level,
	const char* message
);

/*
 * Routes plugin log lines to the host (Unity's Debug.Log). May be invoked
 * from the encoder worker thread. Pass NULL to log to stderr.
 */
RENDERINGOUT_EXPORT void RENDERINGOUT_API
renderingout_set_log_callback(RenderingOutLogCallback callback);

/*
 * quality: 0 (Low) .. 3 (Ultra), see ENCODER_QUALITY_PRESETS.md.
 * width and height must be even. Opens the encoder and the output file
 * immediately; returns NULL on failure.
 */
RENDERINGOUT_EXPORT VideoEncoder* RENDERINGOUT_API
video_encoder_create(
	int quality,
	int width,
	int height,
	int fps,
	const char* filename
);

/*
 * pixels:        width x height RGBA8 pixels (the encoder's dimensions).
 * stride:        bytes between the starts of consecutive rows (>= width*4).
 * flip_vertical: nonzero if row 0 of pixels is the bottom of the image.
 *
 * The pixels are copied before returning, so the caller may release them
 * immediately. Encoding happens on a worker thread; this call only blocks
 * when the internal queue is full. Frames are timestamped in call order.
 * Returns 0 on success or a negative AVERROR value, including any error the
 * worker hit while encoding an earlier frame.
 */
RENDERINGOUT_EXPORT int RENDERINGOUT_API
video_encoder_submit_rgba(
	VideoEncoder* ve,
	const void* pixels,
	int stride,
	int flip_vertical
);

/*
 * Registers a PCM buffer to be muxed into the output when video_encoder_free
 * is called. The PCM data is resampled and re-encoded to AAC.
 *
 * pcm_data:         interleaved normalized 32-bit float PCM samples.
 * pcm_length_bytes: must contain complete interleaved sample frames.
 */
RENDERINGOUT_EXPORT int RENDERINGOUT_API
video_encoder_mux_audio(
	VideoEncoder* ve,
	const void* pcm_data,
	int pcm_length_bytes,
	int sample_rate,
	int channels
);

/*
 * Drains queued frames, writes audio and the trailer, and releases ve.
 */
RENDERINGOUT_EXPORT void RENDERINGOUT_API
video_encoder_free(VideoEncoder* ve);

#ifdef __cplusplus
}
#endif
