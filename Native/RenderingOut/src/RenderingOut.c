#include "RenderingOut.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/log.h>
#include <libavutil/mem.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

/*
 * Frames waiting for the worker. Each slot is one width*height*4 BGRA copy,
 * so this bounds memory (~33 MB at 1080p) while still letting encoding
 * overlap Unity's rendering of the next frames.
 */
#define FRAME_QUEUE_CAPACITY 4

#define QUALITY_LEVELS 4

/* ENCODER_QUALITY_PRESETS.md: libx264 CRF per level. */
static const int x264_crf[QUALITY_LEVELS] = { 28, 23, 18, 14 };

/*
 * ENCODER_QUALITY_PRESETS.md: bitrate ladder at 1080p60, scaled by pixel
 * rate. Shared by h264_mf (Windows) and h264_videotoolbox.
 */
static const int64_t bitrate_ladder_1080p60[QUALITY_LEVELS] = {
	4000000, 8000000, 16000000, 32000000
};

typedef struct EncoderCandidate
{
	const char* name;
	enum AVPixelFormat input_format;
} EncoderCandidate;

static const EncoderCandidate encoder_candidates[] = {
#if defined(__APPLE__)
	{ "h264_videotoolbox", AV_PIX_FMT_NV12 },
#endif
	{ "libx264", AV_PIX_FMT_NV12 },
};

struct VideoEncoder
{
	AVFormatContext* format;
	AVCodecContext* ctx;
	AVStream* stream;
	AVCodecContext* audio_ctx;
	AVStream* audio_stream;
	AVPacket* packet;
	struct SwsContext* sws;
	AVFrame* yuv_frame;
	char* filename;
	uint8_t* pending_pcm_data;
	int64_t pending_pcm_size;
	int pending_pcm_sample_rate;
	int pending_pcm_channels;

	int quality;
	int width;
	int height;
	int fps;
	int header_written;
	int audio_enabled;
	int64_t next_pts;
	int64_t encoded_frames;
	int64_t video_packets;
	int64_t audio_next_pts;

	pthread_t worker;
	int worker_started;
	pthread_mutex_t lock;
	pthread_cond_t changed;
	AVFrame* queue[FRAME_QUEUE_CAPACITY];
	int queue_head;
	int queue_count;
	AVFrame* free_frames[FRAME_QUEUE_CAPACITY + 1];
	int free_count;
	int stopping;
	int fatal_error;
};

/* ------------------------------------------------------------------------ */
/* Logging                                                                  */
/* ------------------------------------------------------------------------ */

static RenderingOutLogCallback log_callback;
static volatile int probe_logging_suppressed;

static void log_message(int level, const char* fmt, va_list args)
{
	char message[2048];

	vsnprintf(message, sizeof(message), fmt, args);
	if (log_callback)
		log_callback(level, message);
	else
		fprintf(stderr, "%s\n", message);
}

static void renderingout_logf(const char* fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_message(RENDERINGOUT_LOG_INFO, fmt, args);
	va_end(args);
}

static void renderingout_log_errorf(const char* fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	log_message(RENDERINGOUT_LOG_ERROR, fmt, args);
	va_end(args);
}

static void ffmpeg_log_callback(void* avcl, int level, const char* fmt, va_list args)
{
	char line[1024];
	int print_prefix = 1;
	size_t length;

	if (level > AV_LOG_ERROR || probe_logging_suppressed)
		return;

	av_log_format_line(avcl, level, fmt, args, line, sizeof(line), &print_prefix);
	length = strlen(line);
	while (length > 0 && (line[length - 1] == '\n' || line[length - 1] == '\r'))
		line[--length] = '\0';
	if (length > 0)
		renderingout_log_errorf("RenderingOut: FFmpeg: %s", line);
}

RENDERINGOUT_EXPORT void RENDERINGOUT_API
renderingout_set_log_callback(RenderingOutLogCallback callback)
{
	log_callback = callback;
	av_log_set_callback(ffmpeg_log_callback);
}

/* ------------------------------------------------------------------------ */
/* Packet / frame plumbing                                                  */
/* ------------------------------------------------------------------------ */

static int write_available_packets(
	VideoEncoder* ve,
	AVCodecContext* ctx,
	AVStream* stream)
{
	int ret;

	if (!ve->header_written || !ctx || !stream)
		return AVERROR(EINVAL);

	for (;;)
	{
		ret = avcodec_receive_packet(ctx, ve->packet);
		if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
			return 0;
		if (ret < 0)
			return ret;

		av_packet_rescale_ts(ve->packet, ctx->time_base, stream->time_base);
		ve->packet->stream_index = stream->index;
		if (stream == ve->stream)
			++ve->video_packets;

		ret = av_interleaved_write_frame(ve->format, ve->packet);
		av_packet_unref(ve->packet);
		if (ret < 0)
			return ret;
	}
}

/*
 * frame == NULL flushes. Per the FFmpeg contract, once all pending output
 * has been read a resend cannot fail with EAGAIN, so one retry suffices.
 */
static int send_and_write(
	VideoEncoder* ve,
	AVCodecContext* ctx,
	AVStream* stream,
	const AVFrame* frame)
{
	int ret;

	ret = avcodec_send_frame(ctx, frame);
	if (ret == AVERROR(EAGAIN))
	{
		ret = write_available_packets(ve, ctx, stream);
		if (ret < 0)
			return ret;
		ret = avcodec_send_frame(ctx, frame);
	}
	if (ret < 0 && !(frame == NULL && ret == AVERROR_EOF))
		return ret;

	return write_available_packets(ve, ctx, stream);
}

/* ------------------------------------------------------------------------ */
/* Encoder setup                                                            */
/* ------------------------------------------------------------------------ */

static int64_t scaled_ladder_bitrate(const VideoEncoder* ve)
{
	return av_rescale(
		bitrate_ladder_1080p60[ve->quality],
		(int64_t)ve->width * ve->height * ve->fps,
		(int64_t)1920 * 1080 * 60);
}

static void set_codec_options(
	const VideoEncoder* ve,
	const EncoderCandidate* candidate,
	AVCodecContext* context,
	AVDictionary** options)
{
	if (strcmp(candidate->name, "libx264") == 0)
	{
		av_dict_set(options, "preset", "medium", 0);
		av_dict_set(options, "tune", "zerolatency", 0);
		av_dict_set_int(options, "crf", x264_crf[ve->quality], 0);
	}
	else if (strcmp(candidate->name, "h264_videotoolbox") == 0)
	{
		context->bit_rate = scaled_ladder_bitrate(ve);
		av_dict_set(options, "profile", "high", 0);
	}
}

static int try_open_encoder(
	VideoEncoder* ve,
	const EncoderCandidate* candidate,
	char* attempt,
	size_t attempt_size)
{
	const AVCodec* codec;
	AVCodecContext* context = NULL;
	AVDictionary* options = NULL;
	int ret;

	codec = avcodec_find_encoder_by_name(candidate->name);
	if (!codec)
	{
		snprintf(attempt, attempt_size, "%s=unavailable", candidate->name);
		return AVERROR_ENCODER_NOT_FOUND;
	}

	context = avcodec_alloc_context3(codec);
	if (!context)
		return AVERROR(ENOMEM);

	context->width = ve->width;
	context->height = ve->height;
	context->framerate = (AVRational){ ve->fps, 1 };
	context->time_base = (AVRational){ 1, ve->fps };
	context->gop_size = ve->fps;
	context->max_b_frames = 0;
	/*
	 * VideoToolbox maps LOW_DELAY to its low-latency (conferencing) rate
	 * control, which drops frames to hold the bitrate: 27 of 240 frames
	 * survived a 4 Mbps 1080p60 test.
	 */
	if (strcmp(candidate->name, "h264_videotoolbox") != 0)
		context->flags |= AV_CODEC_FLAG_LOW_DELAY;
	context->pix_fmt = candidate->input_format;
	context->color_range = AVCOL_RANGE_MPEG;

	if (ve->height >= 720)
	{
		context->colorspace = AVCOL_SPC_BT709;
		context->color_primaries = AVCOL_PRI_BT709;
		context->color_trc = AVCOL_TRC_BT709;
	}
	else
	{
		context->colorspace = AVCOL_SPC_SMPTE170M;
		context->color_primaries = AVCOL_PRI_SMPTE170M;
		context->color_trc = AVCOL_TRC_SMPTE170M;
	}

	if (ve->format->oformat->flags & AVFMT_GLOBALHEADER)
		context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	set_codec_options(ve, candidate, context, &options);
	ret = avcodec_open2(context, codec, &options);
	av_dict_free(&options);
	if (ret < 0)
	{
		snprintf(attempt, attempt_size, "%s=open failed (%s)",
			candidate->name, av_err2str(ret));
		avcodec_free_context(&context);
		return ret;
	}

	ve->ctx = context;
	snprintf(attempt, attempt_size, "%s=selected", candidate->name);
	return 0;
}

static int initialize_encoder(VideoEncoder* ve)
{
	const char* forced = getenv("RENDERINGOUT_ENCODER");
	char summary[1024] = { 0 };
	char attempt[256];
	int last_error = AVERROR_ENCODER_NOT_FOUND;
	size_t i;

	probe_logging_suppressed = 1;

	for (i = 0; i < sizeof(encoder_candidates) / sizeof(encoder_candidates[0]); ++i)
	{
		const EncoderCandidate* candidate = &encoder_candidates[i];
		size_t length;

		if (forced && *forced && strcmp(forced, candidate->name) != 0)
			continue;

		last_error = try_open_encoder(ve, candidate, attempt, sizeof(attempt));
		length = strlen(summary);
		snprintf(summary + length, sizeof(summary) - length, "%s%s",
			length > 0 ? "; " : "", attempt);

		if (last_error >= 0)
		{
			probe_logging_suppressed = 0;
			renderingout_logf(
				"RenderingOut: encoder selection [%s]; input=%s, %dx%d @ %d fps, quality %d",
				summary,
				av_get_pix_fmt_name(candidate->input_format),
				ve->width, ve->height, ve->fps, ve->quality);
			return 0;
		}
	}

	probe_logging_suppressed = 0;
	renderingout_log_errorf(
		"RenderingOut: encoder selection failed [%s]; final error: %s",
		summary, av_err2str(last_error));
	return last_error;
}

static int initialize_converter(VideoEncoder* ve)
{
	int ret;

	ve->sws = sws_getContext(
		ve->width, ve->height, AV_PIX_FMT_BGRA,
		ve->width, ve->height, ve->ctx->pix_fmt,
		SWS_BILINEAR | SWS_ACCURATE_RND | SWS_FULL_CHR_H_INP,
		NULL, NULL, NULL);
	if (!ve->sws)
		return AVERROR(ENOMEM);

	/*
	 * Unity renders full-range RGB; encode to limited range with the same
	 * matrix the stream is tagged with (swscale defaults to BT.601).
	 */
	ret = sws_setColorspaceDetails(
		ve->sws,
		sws_getCoefficients(SWS_CS_DEFAULT), 1,
		sws_getCoefficients(ve->height >= 720 ? SWS_CS_ITU709 : SWS_CS_SMPTE170M), 0,
		0, 1 << 16, 1 << 16);
	if (ret < 0)
		return AVERROR(ENOTSUP);

	ve->yuv_frame = av_frame_alloc();
	if (!ve->yuv_frame)
		return AVERROR(ENOMEM);
	ve->yuv_frame->format = ve->ctx->pix_fmt;
	ve->yuv_frame->width = ve->width;
	ve->yuv_frame->height = ve->height;
	ve->yuv_frame->color_range = ve->ctx->color_range;
	ve->yuv_frame->colorspace = ve->ctx->colorspace;
	ve->yuv_frame->color_primaries = ve->ctx->color_primaries;
	ve->yuv_frame->color_trc = ve->ctx->color_trc;
	return av_frame_get_buffer(ve->yuv_frame, 0);
}

static int initialize_audio_stream(VideoEncoder* ve)
{
	const AVCodec* codec;
	AVCodecContext* ctx = NULL;
	AVStream* stream;
	int ret;

	codec = avcodec_find_encoder_by_name("aac");
	if (!codec)
	{
		renderingout_log_errorf("RenderingOut: AAC codec not found");
		return AVERROR_ENCODER_NOT_FOUND;
	}

	stream = avformat_new_stream(ve->format, codec);
	if (!stream)
		return AVERROR(ENOMEM);

	ctx = avcodec_alloc_context3(codec);
	if (!ctx)
		return AVERROR(ENOMEM);

	ctx->bit_rate = 128000;
	ctx->sample_rate = 44100;
	ctx->ch_layout = (AVChannelLayout)AV_CHANNEL_LAYOUT_STEREO;
	ctx->time_base = (AVRational){ 1, ctx->sample_rate };
	ctx->sample_fmt = AV_SAMPLE_FMT_FLTP;

	if (ve->format->oformat->flags & AVFMT_GLOBALHEADER)
		ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

	ret = avcodec_open2(ctx, codec, NULL);
	if (ret < 0)
		goto fail;

	ret = avcodec_parameters_from_context(stream->codecpar, ctx);
	if (ret < 0)
		goto fail;
	stream->time_base = ctx->time_base;

	ve->audio_ctx = ctx;
	ve->audio_stream = stream;
	ve->audio_enabled = 1;
	return 0;

fail:
	renderingout_log_errorf(
		"RenderingOut: audio stream initialization failed (%s)",
		av_err2str(ret));
	avcodec_free_context(&ctx);
	return ret;
}

static int initialize_output(VideoEncoder* ve)
{
	AVDictionary* options = NULL;
	int ret;

	ve->stream = avformat_new_stream(ve->format, NULL);
	if (!ve->stream)
		return AVERROR(ENOMEM);

	ve->stream->time_base = ve->ctx->time_base;
	ve->stream->avg_frame_rate = ve->ctx->framerate;
	ret = avcodec_parameters_from_context(ve->stream->codecpar, ve->ctx);
	if (ret < 0)
		return ret;

	/*
	 * Audio is set up before the header is written so the stream appears in
	 * the file header. Failure only disables the audio track.
	 */
	if (initialize_audio_stream(ve) < 0)
	{
		renderingout_log_errorf("RenderingOut: falling back to video-only output");
		ve->audio_enabled = 0;
	}

	if (!(ve->format->oformat->flags & AVFMT_NOFILE))
	{
		ret = avio_open(&ve->format->pb, ve->filename, AVIO_FLAG_WRITE);
		if (ret < 0)
		{
			renderingout_log_errorf(
				"RenderingOut: avio_open failed for '%s' (%s)",
				ve->filename, av_err2str(ret));
			return ret;
		}
	}

	if (strcmp(ve->format->oformat->name, "mp4") == 0 ||
		strcmp(ve->format->oformat->name, "mov") == 0)
		av_dict_set(&options, "movflags", "+faststart", 0);

	ret = avformat_write_header(ve->format, &options);
	av_dict_free(&options);
	if (ret < 0)
	{
		renderingout_log_errorf(
			"RenderingOut: avformat_write_header failed (%s)",
			av_err2str(ret));
		return ret;
	}

	ve->header_written = 1;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Audio                                                                    */
/* ------------------------------------------------------------------------ */

/*
 * Converts the registered PCM to the encoder format, pads silence up to the
 * video duration, and encodes it in frame_size chunks. Buffering through a
 * FIFO keeps every frame but the last at exactly frame_size, which the AAC
 * encoder requires.
 */
static int write_audio(VideoEncoder* ve, int64_t target_samples)
{
	AVCodecContext* ctx = ve->audio_ctx;
	AVAudioFifo* fifo = NULL;
	SwrContext* resampler = NULL;
	AVChannelLayout src_layout = { 0 };
	AVFrame* frame = NULL;
	uint8_t** converted = NULL;
	int converted_capacity = 0;
	int frame_size = ctx->frame_size > 0 ? ctx->frame_size : 1024;
	int ret;

	fifo = av_audio_fifo_alloc(ctx->sample_fmt, ctx->ch_layout.nb_channels, frame_size);
	frame = av_frame_alloc();
	if (!fifo || !frame)
	{
		ret = AVERROR(ENOMEM);
		goto done;
	}

	if (ve->pending_pcm_data)
	{
		int channels = ve->pending_pcm_channels;
		int bytes_per_sample = channels * (int)sizeof(float);
		int64_t total = ve->pending_pcm_size / bytes_per_sample;
		int64_t offset = 0;

		av_channel_layout_default(&src_layout, channels);
		ret = swr_alloc_set_opts2(
			&resampler,
			&ctx->ch_layout, ctx->sample_fmt, ctx->sample_rate,
			&src_layout, AV_SAMPLE_FMT_FLT, ve->pending_pcm_sample_rate,
			0, NULL);
		if (ret < 0)
			goto done;
		ret = swr_init(resampler);
		if (ret < 0)
			goto done;

		for (;;)
		{
			const uint8_t* src = NULL;
			int in_samples = 0;
			int out_capacity;

			if (offset < total)
			{
				in_samples = (int)FFMIN(total - offset, (int64_t)frame_size * 16);
				src = ve->pending_pcm_data + offset * bytes_per_sample;
			}

			out_capacity = swr_get_out_samples(resampler, in_samples);
			if (out_capacity <= 0 && offset >= total)
				break;
			if (out_capacity > converted_capacity)
			{
				if (converted)
					av_freep(&converted[0]);
				av_freep(&converted);
				ret = av_samples_alloc_array_and_samples(
					&converted, NULL,
					ctx->ch_layout.nb_channels, out_capacity,
					ctx->sample_fmt, 0);
				if (ret < 0)
					goto done;
				converted_capacity = out_capacity;
			}

			ret = swr_convert(resampler, converted, out_capacity, &src, in_samples);
			if (ret < 0)
				goto done;
			if (ret > 0 && av_audio_fifo_write(fifo, (void**)converted, ret) < ret)
			{
				ret = AVERROR(ENOMEM);
				goto done;
			}

			if (offset >= total && ret == 0)
				break;
			offset += in_samples;
		}
	}

	if (av_audio_fifo_size(fifo) < target_samples)
	{
		int64_t missing = target_samples - av_audio_fifo_size(fifo);

		while (missing > 0)
		{
			int chunk = (int)FFMIN(missing, (int64_t)frame_size);

			av_frame_unref(frame);
			frame->format = ctx->sample_fmt;
			frame->sample_rate = ctx->sample_rate;
			frame->nb_samples = chunk;
			ret = av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
			if (ret < 0)
				goto done;
			ret = av_frame_get_buffer(frame, 0);
			if (ret < 0)
				goto done;
			av_samples_set_silence(frame->data, 0, chunk,
				ctx->ch_layout.nb_channels, ctx->sample_fmt);
			if (av_audio_fifo_write(fifo, (void**)frame->data, chunk) < chunk)
			{
				ret = AVERROR(ENOMEM);
				goto done;
			}
			missing -= chunk;
		}
	}

	while (av_audio_fifo_size(fifo) > 0)
	{
		int chunk = FFMIN(av_audio_fifo_size(fifo), frame_size);

		av_frame_unref(frame);
		frame->format = ctx->sample_fmt;
		frame->sample_rate = ctx->sample_rate;
		frame->nb_samples = chunk;
		ret = av_channel_layout_copy(&frame->ch_layout, &ctx->ch_layout);
		if (ret < 0)
			goto done;
		ret = av_frame_get_buffer(frame, 0);
		if (ret < 0)
			goto done;
		if (av_audio_fifo_read(fifo, (void**)frame->data, chunk) < chunk)
		{
			ret = AVERROR(EIO);
			goto done;
		}

		frame->pts = ve->audio_next_pts;
		ve->audio_next_pts += chunk;
		ret = send_and_write(ve, ctx, ve->audio_stream, frame);
		if (ret < 0)
			goto done;
	}

	ret = send_and_write(ve, ctx, ve->audio_stream, NULL);

done:
	if (ret < 0)
		renderingout_log_errorf(
			"RenderingOut: audio encoding failed (%s)", av_err2str(ret));
	if (converted)
		av_freep(&converted[0]);
	av_freep(&converted);
	av_frame_free(&frame);
	av_audio_fifo_free(fifo);
	swr_free(&resampler);
	av_channel_layout_uninit(&src_layout);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* Worker                                                                   */
/* ------------------------------------------------------------------------ */

static int encode_bgra_frame(VideoEncoder* ve, const AVFrame* bgra)
{
	int ret;

	/* The encoder may still reference the previous buffer. */
	ret = av_frame_make_writable(ve->yuv_frame);
	if (ret < 0)
		return ret;

	sws_scale(
		ve->sws,
		(const uint8_t* const*)bgra->data, bgra->linesize,
		0, ve->height,
		ve->yuv_frame->data, ve->yuv_frame->linesize);
	ve->yuv_frame->pts = bgra->pts;

	ret = send_and_write(ve, ve->ctx, ve->stream, ve->yuv_frame);
	if (ret >= 0)
		++ve->encoded_frames;
	return ret;
}

static void* encoder_worker_main(void* parameter)
{
	VideoEncoder* ve = (VideoEncoder*)parameter;

	pthread_mutex_lock(&ve->lock);
	for (;;)
	{
		AVFrame* frame;
		int failed;
		int ret = 0;

		while (ve->queue_count == 0 && !ve->stopping)
			pthread_cond_wait(&ve->changed, &ve->lock);
		if (ve->queue_count == 0)
			break;

		frame = ve->queue[ve->queue_head];
		ve->queue_head = (ve->queue_head + 1) % FRAME_QUEUE_CAPACITY;
		--ve->queue_count;
		failed = ve->fatal_error < 0;
		pthread_mutex_unlock(&ve->lock);

		/* After a failure keep draining so the producer never deadlocks. */
		if (!failed)
			ret = encode_bgra_frame(ve, frame);

		pthread_mutex_lock(&ve->lock);
		if (ret < 0 && ve->fatal_error >= 0)
		{
			ve->fatal_error = ret;
			renderingout_log_errorf(
				"RenderingOut: encoding frame %lld failed (%s)",
				(long long)frame->pts, av_err2str(ret));
		}
		if (ve->free_count < FRAME_QUEUE_CAPACITY + 1)
			ve->free_frames[ve->free_count++] = frame;
		else
			av_frame_free(&frame);
		pthread_cond_broadcast(&ve->changed);
	}
	pthread_mutex_unlock(&ve->lock);
	return NULL;
}

static void stop_worker(VideoEncoder* ve)
{
	if (!ve->worker_started)
		return;

	pthread_mutex_lock(&ve->lock);
	ve->stopping = 1;
	pthread_cond_broadcast(&ve->changed);
	pthread_mutex_unlock(&ve->lock);

	pthread_join(ve->worker, NULL);
	ve->worker_started = 0;
}

/* ------------------------------------------------------------------------ */
/* Exports                                                                  */
/* ------------------------------------------------------------------------ */

static void release_encoder(VideoEncoder* ve)
{
	int i;

	for (i = 0; i < ve->queue_count; ++i)
		av_frame_free(&ve->queue[(ve->queue_head + i) % FRAME_QUEUE_CAPACITY]);
	for (i = 0; i < ve->free_count; ++i)
		av_frame_free(&ve->free_frames[i]);

	if (ve->format && ve->format->pb &&
		!(ve->format->oformat->flags & AVFMT_NOFILE))
		avio_closep(&ve->format->pb);

	sws_freeContext(ve->sws);
	av_frame_free(&ve->yuv_frame);
	avcodec_free_context(&ve->ctx);
	avcodec_free_context(&ve->audio_ctx);
	av_packet_free(&ve->packet);
	avformat_free_context(ve->format);
	av_freep(&ve->pending_pcm_data);
	av_freep(&ve->filename);
	pthread_cond_destroy(&ve->changed);
	pthread_mutex_destroy(&ve->lock);
	free(ve);
}

RENDERINGOUT_EXPORT VideoEncoder* RENDERINGOUT_API
video_encoder_create(
	int quality,
	int width,
	int height,
	int fps,
	const char* filename)
{
	VideoEncoder* ve;
	int ret;

	if (quality < 0 || quality >= QUALITY_LEVELS ||
		width <= 0 || height <= 0 || fps <= 0 ||
		!filename || (width & 1) || (height & 1))
		return NULL;

	ve = (VideoEncoder*)calloc(1, sizeof(*ve));
	if (!ve)
		return NULL;
	pthread_mutex_init(&ve->lock, NULL);
	pthread_cond_init(&ve->changed, NULL);

	ve->quality = quality;
	ve->width = width;
	ve->height = height;
	ve->fps = fps;
	ve->filename = av_strdup(filename);
	if (!ve->filename)
		goto fail;

	renderingout_logf(
		"RenderingOut: video_encoder_create('%s', %dx%d @ %d fps, quality: %d)",
		filename, width, height, fps, quality);

	ret = avformat_alloc_output_context2(&ve->format, NULL, NULL, filename);
	if (ret < 0 || !ve->format)
		goto fail;

	ve->packet = av_packet_alloc();
	if (!ve->packet)
		goto fail;

	if (initialize_encoder(ve) < 0)
		goto fail;

	ret = initialize_converter(ve);
	if (ret < 0)
	{
		renderingout_log_errorf(
			"RenderingOut: color converter setup failed (%s)", av_err2str(ret));
		goto fail;
	}

	if (initialize_output(ve) < 0)
		goto fail;

	if (pthread_create(&ve->worker, NULL, encoder_worker_main, ve) != 0)
		goto fail;
	ve->worker_started = 1;
	return ve;

fail:
	release_encoder(ve);
	return NULL;
}

RENDERINGOUT_EXPORT int RENDERINGOUT_API
video_encoder_submit_bgra(
	VideoEncoder* ve,
	const void* pixels,
	int stride,
	int flip_vertical)
{
	const int row_bytes = ve ? ve->width * 4 : 0;
	AVFrame* frame = NULL;
	int ret;
	int y;

	if (!ve || !pixels || stride < row_bytes)
		return AVERROR(EINVAL);

	pthread_mutex_lock(&ve->lock);
	while (ve->queue_count == FRAME_QUEUE_CAPACITY && ve->fatal_error >= 0)
		pthread_cond_wait(&ve->changed, &ve->lock);
	ret = ve->fatal_error;
	if (ret >= 0 && ve->free_count > 0)
		frame = ve->free_frames[--ve->free_count];
	pthread_mutex_unlock(&ve->lock);
	if (ret < 0)
		return ret;

	if (!frame)
	{
		frame = av_frame_alloc();
		if (!frame)
			return AVERROR(ENOMEM);
		frame->format = AV_PIX_FMT_BGRA;
		frame->width = ve->width;
		frame->height = ve->height;
		ret = av_frame_get_buffer(frame, 0);
		if (ret < 0)
		{
			av_frame_free(&frame);
			return ret;
		}
	}

	for (y = 0; y < ve->height; ++y)
	{
		int src_row = flip_vertical ? ve->height - 1 - y : y;

		memcpy(
			frame->data[0] + (size_t)y * frame->linesize[0],
			(const uint8_t*)pixels + (size_t)src_row * stride,
			row_bytes);
	}
	frame->pts = ve->next_pts++;

	pthread_mutex_lock(&ve->lock);
	ve->queue[(ve->queue_head + ve->queue_count) % FRAME_QUEUE_CAPACITY] = frame;
	++ve->queue_count;
	pthread_cond_broadcast(&ve->changed);
	pthread_mutex_unlock(&ve->lock);
	return 0;
}

RENDERINGOUT_EXPORT int RENDERINGOUT_API
video_encoder_mux_audio(
	VideoEncoder* ve,
	const void* pcm_data,
	int pcm_length_bytes,
	int sample_rate,
	int channels)
{
	uint8_t* copy;

	if (!ve || !pcm_data || pcm_length_bytes <= 0 ||
		sample_rate <= 0 || channels <= 0 ||
		channels > AV_NUM_DATA_POINTERS ||
		pcm_length_bytes % (channels * (int)sizeof(float)) != 0)
		return AVERROR(EINVAL);
	if (!ve->audio_enabled)
		return AVERROR(EINVAL);

	copy = av_malloc(pcm_length_bytes);
	if (!copy)
		return AVERROR(ENOMEM);
	memcpy(copy, pcm_data, pcm_length_bytes);

	renderingout_logf(
		"RenderingOut: registered PCM audio data (%d bytes, %d Hz, %d channels, duration: %.2f sec)",
		pcm_length_bytes, sample_rate, channels,
		(double)pcm_length_bytes / (channels * sizeof(float) * sample_rate));

	/* The encode happens in video_encoder_free, so later calls overwrite. */
	av_freep(&ve->pending_pcm_data);
	ve->pending_pcm_data = copy;
	ve->pending_pcm_size = pcm_length_bytes;
	ve->pending_pcm_sample_rate = sample_rate;
	ve->pending_pcm_channels = channels;
	return 0;
}

RENDERINGOUT_EXPORT void RENDERINGOUT_API
video_encoder_free(VideoEncoder* ve)
{
	int ret;

	if (!ve)
		return;

	stop_worker(ve);

	if (ve->header_written)
	{
		ret = ve->fatal_error;
		if (ret >= 0)
			ret = send_and_write(ve, ve->ctx, ve->stream, NULL);
		if (ret < 0)
			renderingout_log_errorf(
				"RenderingOut: video flush failed (%s)", av_err2str(ret));

		if (ve->audio_enabled)
			write_audio(ve, av_rescale_q(
				ve->encoded_frames,
				ve->ctx->time_base,
				ve->audio_ctx->time_base));

		ret = av_write_trailer(ve->format);
		if (ret < 0)
			renderingout_log_errorf(
				"RenderingOut: av_write_trailer failed (%s)", av_err2str(ret));

		if (ve->video_packets != ve->encoded_frames)
			renderingout_log_errorf(
				"RenderingOut: encoder dropped %lld of %lld frames",
				(long long)(ve->encoded_frames - ve->video_packets),
				(long long)ve->encoded_frames);

		renderingout_logf(
			"RenderingOut: finished '%s'. Total video frames: %lld",
			ve->filename, (long long)ve->encoded_frames);
	}

	release_encoder(ve);
}
