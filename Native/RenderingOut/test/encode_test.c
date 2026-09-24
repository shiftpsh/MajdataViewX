/*
 * Drives the plugin exactly as ScreenRecorder.cs does: create, submit BGRA
 * frames, register PCM, free. Verification of the output is done by
 * test/verify.sh with ffprobe.
 *
 * usage: encode_test <out.mp4> <quality 0-3> <width> <height> <fps> <frames>
 *                    [flip 0|1] [audio_seconds] [noise 0|1]
 *
 * Frames: top half pure red, bottom half pure blue, with a white bar that
 * moves one column per frame (so the encoder sees motion). With flip=1 the
 * rows are handed over bottom-up, so a correct output looks identical.
 * noise=1 fills the middle third with per-frame gray noise (128 +- 24) so the
 * encoder's rate control, not the content, bounds the output size.
 */

#include "RenderingOut.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void RENDERINGOUT_API log_line(int level, const char* message)
{
	fprintf(stderr, "[%s] %s\n", level ? "error" : "info", message);
}

int main(int argc, char** argv)
{
	const char* path;
	int quality, width, height, fps, frames, flip = 0, noise = 0;
	double audio_seconds = -1;
	int stride, f, x, y, ret;
	unsigned char* pixels;
	VideoEncoder* ve;

	if (argc < 7)
	{
		fprintf(stderr, "usage: %s out.mp4 quality width height fps frames [flip] [audio_seconds] [noise]\n", argv[0]);
		return 2;
	}
	path = argv[1];
	quality = atoi(argv[2]);
	width = atoi(argv[3]);
	height = atoi(argv[4]);
	fps = atoi(argv[5]);
	frames = atoi(argv[6]);
	if (argc > 7)
		flip = atoi(argv[7]);
	if (argc > 8)
		audio_seconds = atof(argv[8]);
	if (argc > 9)
		noise = atoi(argv[9]);

	renderingout_set_log_callback(log_line);

	ve = video_encoder_create(quality, width, height, fps, path);
	if (!ve)
	{
		fprintf(stderr, "video_encoder_create failed\n");
		return 1;
	}

	/* Padded stride, like a GPU readback row pitch may be. */
	stride = width * 4 + 64;
	pixels = malloc((size_t)stride * height);

	for (f = 0; f < frames; ++f)
	{
		for (y = 0; y < height; ++y)
		{
			/* Logical row (0 = top); stored bottom-up when flipping. */
			int stored = flip ? height - 1 - y : y;
			unsigned char* row = pixels + (size_t)stored * stride;

			for (x = 0; x < width; ++x)
			{
				unsigned char* p = row + x * 4;
				int bar = abs(x - (f * 4) % width) < 8;

				if (noise && y >= height / 3 && y < height * 2 / 3)
				{
					p[0] = p[1] = p[2] = (unsigned char)(128 + rand() % 49 - 24);
					p[3] = 255;
					continue;
				}
				p[0] = bar ? 255 : (y < height / 2 ? 0 : 255);   /* B */
				p[1] = bar ? 255 : 0;                            /* G */
				p[2] = bar ? 255 : (y < height / 2 ? 255 : 0);   /* R */
				p[3] = 255;
			}
		}

		ret = video_encoder_submit_bgra(ve, pixels, stride, flip);
		if (ret < 0)
		{
			fprintf(stderr, "submit failed at frame %d: %d\n", f, ret);
			video_encoder_free(ve);
			return 1;
		}
	}

	if (audio_seconds > 0)
	{
		const int rate = 44100, channels = 2;
		int samples = (int)(audio_seconds * rate);
		float* pcm = malloc(sizeof(float) * samples * channels);
		int i;

		for (i = 0; i < samples; ++i)
		{
			float v = 0.25f * sinf(2.0f * 3.14159265f * 440.0f * i / rate);
			pcm[i * 2] = v;
			pcm[i * 2 + 1] = v;
		}
		ret = video_encoder_mux_audio(ve, pcm, samples * channels * (int)sizeof(float), rate, channels);
		free(pcm);
		if (ret < 0)
		{
			fprintf(stderr, "mux_audio failed: %d\n", ret);
			video_encoder_free(ve);
			return 1;
		}
	}

	video_encoder_free(ve);
	free(pixels);
	return 0;
}
