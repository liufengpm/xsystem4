/* Copyright (C) 2019 Nunuhara Cabbage <nunuhara@haniwa.technology>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <stdatomic.h>
#include <assert.h>
#include <limits.h>
#include <stdint.h>

#include <SDL.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>

#include "system4.h"
#include "system4/archive.h"

#include "asset_manager.h"
#include "audio.h"
#include "mixer.h"
#include "xsystem4.h"

#define clamp(min_value, max_value, value) min(max_value, max(min_value, value))
#define muldiv(x, y, denom) ((int64_t)(x) * (int64_t)(y) / (int64_t)(denom))

#define STS_MIXER_IMPLEMENTATION
#include "sts_mixer.h"

#define CHUNK_SIZE 1024
#define AVIO_BUFFER_SIZE (64 * 1024)
#define CHANNEL_MAGIC 0x43484e4cU
#define CHANNEL_DESTROY_GRACE_CALLBACKS 512
#define FF_AUDIO_DECODE_MAX_FRAMES 65536

struct fade {
	atomic_bool fading;
	bool stop;
	uint_least32_t start_pos;
	uint_least32_t frames;
	uint_least32_t elapsed;
	float start_volume;
	float end_volume;
};

struct decoded_audio_info {
	int channels;
	int samplerate;
	uint_least32_t frames;
};

struct channel;

struct ff_audio {
	AVFormatContext *format_ctx;
	AVCodecContext *codec_ctx;
	AVStream *stream;
	AVPacket *packet;
	AVFrame *frame;
	SwrContext *swr;
	AVIOContext *avio;
	uint8_t *avio_buffer;
	float *decoded;
	int decoded_capacity;
	int decoded_frames;
	int decoded_pos;
	int out_channels;
	int out_sample_rate;
	uint_least32_t total_frames;
	bool eof;
	bool draining;
};

struct channel {
	uint32_t magic;
	bool closing;
	bool warned_invalid_mixer;
	bool warned_invalid_voice;
	enum asset_type type;
	int default_mixer_no;

	struct archive_data *dfile;
	int no;
	int mixer_no;

	struct ff_audio *file;
	struct ff_audio *closing_file;
	struct decoded_audio_info info;
	int64_t offset;

	atomic_int voice;
	sts_mixer_stream_t stream;
	float data[CHUNK_SIZE * 2];

	atomic_uint_least32_t frame;

	atomic_uint volume;
	atomic_bool swapped;
	uint_least32_t loop_start;
	uint_least32_t loop_end;
	atomic_uint loop_count;
	struct fade fade;

	struct channel *destroy_next;
	uint64_t destroy_after_epoch;
};

struct mixer {
	sts_mixer_t mixer;
	sts_mixer_stream_t stream;
	int voice;
	atomic_bool muted;
	float data[CHUNK_SIZE * 2];
	char *name;

	struct mixer *parent;
	struct mixer **children;
	int nr_children;
};

static struct mixer *master = NULL;
static struct mixer *mixers = NULL;
static int nr_mixers = 0;
static struct channel *pending_destroy = NULL;
static uint64_t audio_callback_epoch = 0;

static SDL_AudioDeviceID audio_device = 0;

static void ffaudio_close(struct ff_audio *audio);

static void mixer_release_state(void)
{
	if (!mixers)
		return;

	for (int i = 0; i < nr_mixers; i++) {
		free(mixers[i].name);
		free(mixers[i].children);
	}
	free(mixers);
	mixers = NULL;
	master = NULL;
	nr_mixers = 0;
}

static void channel_free_immediate(struct channel *ch)
{
	if (!ch)
		return;

	struct ff_audio *file = ch->closing_file ? ch->closing_file : ch->file;
	ch->closing_file = NULL;
	ch->file = NULL;
	ffaudio_close(file);
	if (ch->dfile)
		archive_free_data(ch->dfile);
	ch->magic = 0;
	free(ch);
}

static int channel_default_mixer_no(enum asset_type type)
{
	if (nr_mixers <= 0)
		return 0;
	if (type == ASSET_SOUND && nr_mixers > 1)
		return 1;
	return 0;
}

static int sanitize_mixer_index(int mixer_no, int fallback)
{
	if (nr_mixers <= 0)
		return -1;
	fallback = clamp(0, nr_mixers - 1, fallback);
	if (mixer_no < 0 || mixer_no >= nr_mixers)
		return fallback;
	return mixer_no;
}

static void channel_normalize_loop_points(struct channel *ch)
{
	uint_least32_t total_frames = ch->info.frames;
	if (!total_frames) {
		ch->loop_start = 0;
		ch->loop_end = 0;
		return;
	}
	if (!ch->loop_end || ch->loop_end > total_frames)
		ch->loop_end = total_frames;
	if (ch->loop_start > total_frames)
		ch->loop_start = 0;
	if (ch->loop_start >= ch->loop_end) {
		ch->loop_start = 0;
		ch->loop_end = total_frames;
	}
}

static uint_least32_t channel_clamp_loop_pos(struct channel *ch, int pos)
{
	if (pos <= 0 || !ch->info.frames)
		return 0;
	if ((uint_least64_t)pos >= ch->info.frames)
		return ch->info.frames;
	return (uint_least32_t)pos;
}

static unsigned int channel_clamp_loop_count(int count)
{
	return count <= 0 ? 0u : (unsigned int)count;
}

static int channel_clamp_volume_percent(int volume)
{
	return clamp(0, 100, volume);
}

static uint_least32_t channel_ms_to_seek_frame(struct channel *ch, int pos_ms)
{
	if (pos_ms <= 0 || ch->info.samplerate <= 0)
		return 0;
	uint_least64_t frame = ((uint_least64_t)pos_ms * (uint_least64_t)ch->info.samplerate) / 1000u;
	if (ch->info.frames && frame > ch->info.frames)
		frame = ch->info.frames;
	if (frame > UINT_LEAST32_MAX)
		frame = UINT_LEAST32_MAX;
	return (uint_least32_t)frame;
}

static uint_least32_t channel_ms_to_duration_frames(struct channel *ch, int duration_ms)
{
	if (duration_ms <= 0 || ch->info.samplerate <= 0)
		return 0;
	uint_least64_t frames = ((uint_least64_t)duration_ms * (uint_least64_t)ch->info.samplerate) / 1000u;
	if (frames > UINT_LEAST32_MAX)
		frames = UINT_LEAST32_MAX;
	return (uint_least32_t)frames;
}

static bool channel_validate(struct channel *ch, const char *op, bool require_file)
{
	if (!ch || ch->magic != CHANNEL_MAGIC) {
		WARNING("%s: invalid channel state", op);
		return false;
	}
	if (ch->closing)
		return false;
	if (require_file && !ch->file)
		return false;
	return true;
}

static int channel_resolve_mixer_no_locked(struct channel *ch, const char *op)
{
	if (!channel_validate(ch, op, false) || !mixers || nr_mixers <= 0)
		return -1;

	int fallback = sanitize_mixer_index(ch->default_mixer_no,
			channel_default_mixer_no(ch->type));
	int mixer_no = ch->mixer_no;
	if (mixer_no >= 0 && mixer_no < nr_mixers)
		return mixer_no;

	if (ch->no >= 0) {
		if (ch->type == ASSET_SOUND) {
			struct wai *wai = wai_get(ch->no);
			fallback = sanitize_mixer_index(wai ? wai->channel : fallback, fallback);
		} else if (ch->type == ASSET_BGM) {
			struct bgi *bgi = bgi_get(ch->no);
			fallback = sanitize_mixer_index(bgi ? bgi->channel : fallback, fallback);
		}
	}

	if (!ch->warned_invalid_mixer) {
		WARNING("%s: repairing invalid mixer index %d for audio no=%d", op, mixer_no, ch->no);
		ch->warned_invalid_mixer = true;
	}
	ch->mixer_no = fallback;
	return fallback;
}

static int channel_voice_index_locked(struct channel *ch, const char *op)
{
	int voice = atomic_load_explicit(&ch->voice, memory_order_relaxed);
	if (voice >= 0 && voice < STS_MIXER_VOICES)
		return voice;

	if (!ch->warned_invalid_voice) {
		WARNING("%s: invalid voice index %d for audio no=%d", op, voice, ch->no);
		ch->warned_invalid_voice = true;
	}
	return -1;
}

static bool channel_update_voice_gain_locked(struct channel *ch, float gain, const char *op)
{
	int mixer_no = channel_resolve_mixer_no_locked(ch, op);
	int voice = channel_voice_index_locked(ch, op);
	if (mixer_no < 0 || voice < 0)
		return false;

	mixers[mixer_no].mixer.voices[voice].gain = gain;
	ch->warned_invalid_mixer = false;
	ch->warned_invalid_voice = false;
	return true;
}

static void channel_queue_destroy_locked(struct channel *ch)
{
	ch->destroy_after_epoch = audio_callback_epoch + CHANNEL_DESTROY_GRACE_CALLBACKS;
	ch->destroy_next = pending_destroy;
	pending_destroy = ch;
}

static void channel_collect_pending_locked(bool force)
{
	struct channel **link = &pending_destroy;
	while (*link) {
		struct channel *ch = *link;
		if (!force && ch->destroy_after_epoch > audio_callback_epoch) {
			link = &ch->destroy_next;
			continue;
		}

		*link = ch->destroy_next;
		ch->destroy_next = NULL;
		channel_free_immediate(ch);
	}
}

static void ffaudio_close(struct ff_audio *audio)
{
	if (!audio)
		return;
	if (audio->codec_ctx)
		avcodec_free_context(&audio->codec_ctx);
	if (audio->format_ctx)
		avformat_close_input(&audio->format_ctx);
	if (audio->frame)
		av_frame_free(&audio->frame);
	if (audio->packet)
		av_packet_free(&audio->packet);
	if (audio->swr)
		swr_free(&audio->swr);
	if (audio->avio)
		avio_context_free(&audio->avio);
	if (audio->decoded)
		free(audio->decoded);
	free(audio);
}

static bool ffaudio_prepare_decoded_buffer(struct ff_audio *audio)
{
	uint_least64_t reserve_frames = audio->codec_ctx->frame_size > 0
		? (uint_least64_t)audio->codec_ctx->frame_size * 4u
		: (uint_least64_t)audio->out_sample_rate;
	if (reserve_frames < CHUNK_SIZE)
		reserve_frames = CHUNK_SIZE;
	if (reserve_frames > FF_AUDIO_DECODE_MAX_FRAMES)
		reserve_frames = FF_AUDIO_DECODE_MAX_FRAMES;
	audio->decoded_capacity = (int)(reserve_frames * audio->out_channels);
	audio->decoded = xcalloc(audio->decoded_capacity, sizeof(float));
	return !!audio->decoded;
}

static bool ffaudio_init_decoder(struct ff_audio *audio)
{
	int stream_index = av_find_best_stream(audio->format_ctx, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
	if (stream_index < 0)
		return false;

	audio->stream = audio->format_ctx->streams[stream_index];
	const AVCodec *codec = avcodec_find_decoder(audio->stream->codecpar->codec_id);
	if (!codec)
		return false;

	audio->codec_ctx = avcodec_alloc_context3(codec);
	if (!audio->codec_ctx)
		return false;
	if (avcodec_parameters_to_context(audio->codec_ctx, audio->stream->codecpar) < 0)
		return false;
	if (avcodec_open2(audio->codec_ctx, codec, NULL) < 0)
		return false;

	audio->packet = av_packet_alloc();
	audio->frame = av_frame_alloc();
	if (!audio->packet || !audio->frame)
		return false;

	audio->out_channels = audio->codec_ctx->ch_layout.nb_channels == 1 ? 1 : 2;
	audio->out_sample_rate = audio->codec_ctx->sample_rate > 0 ? audio->codec_ctx->sample_rate : 44100;

	AVChannelLayout dst_layout;
	av_channel_layout_default(&dst_layout, audio->out_channels);
	if (swr_alloc_set_opts2(&audio->swr,
			&dst_layout,
			AV_SAMPLE_FMT_FLT,
			audio->out_sample_rate,
			&audio->codec_ctx->ch_layout,
			audio->codec_ctx->sample_fmt,
			audio->codec_ctx->sample_rate,
			0,
			NULL) < 0) {
		av_channel_layout_uninit(&dst_layout);
		return false;
	}
	av_channel_layout_uninit(&dst_layout);
	if (swr_init(audio->swr) < 0)
		return false;

	int64_t total_frames = 0;
	if (audio->stream->duration != AV_NOPTS_VALUE) {
		total_frames = av_rescale_q(audio->stream->duration,
				audio->stream->time_base,
				(AVRational){ 1, audio->out_sample_rate });
	} else if (audio->format_ctx->duration != AV_NOPTS_VALUE) {
		total_frames = av_rescale_q(audio->format_ctx->duration,
				AV_TIME_BASE_Q,
				(AVRational){ 1, audio->out_sample_rate });
	}
	if (total_frames <= 0)
		total_frames = INT_MAX / 4;
	audio->total_frames = (uint_least32_t)min((int64_t)UINT_MAX, total_frames);
	if (!ffaudio_prepare_decoded_buffer(audio))
		return false;
	return true;
}

static int ffaudio_avio_read(void *opaque, uint8_t *buf, int buf_size)
{
	struct channel *ch = opaque;
	int64_t remaining = (int64_t)ch->dfile->size - ch->offset;
	if (remaining <= 0)
		return AVERROR_EOF;
	int read_size = (int)min((int64_t)buf_size, remaining);
	memcpy(buf, ch->dfile->data + ch->offset, read_size);
	ch->offset += read_size;
	return read_size;
}

static int64_t ffaudio_avio_seek(void *opaque, int64_t offset, int whence)
{
	struct channel *ch = opaque;
	if (whence == AVSEEK_SIZE)
		return (int64_t)ch->dfile->size;

	switch (whence) {
	case SEEK_SET:
		ch->offset = offset;
		break;
	case SEEK_CUR:
		ch->offset += offset;
		break;
	case SEEK_END:
		ch->offset = (int64_t)ch->dfile->size + offset;
		break;
	default:
		return -1;
	}

	ch->offset = clamp(0, (int64_t)ch->dfile->size, ch->offset);
	return ch->offset;
}

static struct ff_audio *ffaudio_open_archive_data(struct channel *ch)
{
	struct ff_audio *audio = xcalloc(1, sizeof(struct ff_audio));
	audio->format_ctx = avformat_alloc_context();
	if (!audio->format_ctx) {
		ffaudio_close(audio);
		return NULL;
	}

	audio->avio_buffer = av_malloc(AVIO_BUFFER_SIZE);
	if (!audio->avio_buffer) {
		ffaudio_close(audio);
		return NULL;
	}

	audio->avio = avio_alloc_context(audio->avio_buffer,
			AVIO_BUFFER_SIZE,
			0,
			ch,
			ffaudio_avio_read,
			NULL,
			ffaudio_avio_seek);
	if (!audio->avio) {
		ffaudio_close(audio);
		return NULL;
	}

	audio->format_ctx->pb = audio->avio;
	audio->format_ctx->flags |= AVFMT_FLAG_CUSTOM_IO;
	if (avformat_open_input(&audio->format_ctx, NULL, NULL, NULL) < 0) {
		ffaudio_close(audio);
		return NULL;
	}
	{
		AVDictionary *probe_opts = NULL;
		av_dict_set_int(&probe_opts, "threads", 1, 0);
		int _ret = avformat_find_stream_info(audio->format_ctx, &probe_opts);
		av_dict_free(&probe_opts);
		if (_ret < 0) {
			ffaudio_close(audio);
			return NULL;
		}
	}
	if (!ffaudio_init_decoder(audio)) {
		ffaudio_close(audio);
		return NULL;
	}
	return audio;
}

static struct ff_audio *ffaudio_open_file(const char *path)
{
	struct ff_audio *audio = xcalloc(1, sizeof(struct ff_audio));
	if (avformat_open_input(&audio->format_ctx, path, NULL, NULL) < 0) {
		ffaudio_close(audio);
		return NULL;
	}
	{
		AVDictionary *probe_opts = NULL;
		av_dict_set_int(&probe_opts, "threads", 1, 0);
		int _ret = avformat_find_stream_info(audio->format_ctx, &probe_opts);
		av_dict_free(&probe_opts);
		if (_ret < 0) {
			ffaudio_close(audio);
			return NULL;
		}
	}
	if (!ffaudio_init_decoder(audio)) {
		ffaudio_close(audio);
		return NULL;
	}
	return audio;
}

static bool ffaudio_seek(struct ff_audio *audio, uint_least32_t frame)
{
	int64_t timestamp = av_rescale_q(frame,
			(AVRational){ 1, audio->out_sample_rate },
			audio->stream->time_base);
	if (av_seek_frame(audio->format_ctx, audio->stream->index, timestamp, AVSEEK_FLAG_BACKWARD) < 0)
		return false;
	avcodec_flush_buffers(audio->codec_ctx);
	audio->decoded_frames = 0;
	audio->decoded_pos = 0;
	audio->eof = false;
	audio->draining = false;
	return true;
}

static bool ffaudio_decode_next(struct ff_audio *audio)
{
	for (;;) {
		int ret = avcodec_receive_frame(audio->codec_ctx, audio->frame);
		if (ret == 0) {
			int out_max = (int)av_rescale_rnd(
					swr_get_delay(audio->swr, audio->codec_ctx->sample_rate) + audio->frame->nb_samples,
					audio->out_sample_rate,
					audio->codec_ctx->sample_rate,
					AV_ROUND_UP);
			if (out_max <= 0 || out_max > FF_AUDIO_DECODE_MAX_FRAMES) {
				WARNING("decoded audio frame too large: %d samples", out_max);
				return false;
			}
			int needed = out_max * audio->out_channels;
			if (needed > audio->decoded_capacity) {
				WARNING("decoded audio frame exceeded fixed buffer: need=%d capacity=%d",
						needed, audio->decoded_capacity);
				return false;
			}
			uint8_t *out_planes[] = { (uint8_t *)audio->decoded };
			ret = swr_convert(audio->swr,
					out_planes,
					out_max,
					(const uint8_t **)audio->frame->extended_data,
					audio->frame->nb_samples);
			if (ret < 0) {
				WARNING("swr_convert failed: %d", ret);
				return false;
			}
			audio->decoded_frames = ret;
			audio->decoded_pos = 0;
			return ret > 0;
		}

		if (ret == AVERROR(EAGAIN)) {
			if (audio->eof) {
				if (audio->draining)
					return false;
				if (avcodec_send_packet(audio->codec_ctx, NULL) < 0)
					return false;
				audio->draining = true;
				continue;
			}

			ret = av_read_frame(audio->format_ctx, audio->packet);
			if (ret < 0) {
				audio->eof = true;
				continue;
			}
			if (audio->packet->stream_index != audio->stream->index) {
				av_packet_unref(audio->packet);
				continue;
			}
			ret = avcodec_send_packet(audio->codec_ctx, audio->packet);
			av_packet_unref(audio->packet);
			if (ret == 0 || ret == AVERROR(EAGAIN))
				continue;
			WARNING("avcodec_send_packet failed: %d", ret);
			return false;
		}

		if (ret == AVERROR_EOF)
			return false;

		WARNING("avcodec_receive_frame failed: %d", ret);
		return false;
	}
}

static uint_least32_t ffaudio_read_frames(struct ff_audio *audio, float *out, uint_least32_t frame_count)
{
	uint_least32_t total = 0;
	while (total < frame_count) {
		if (audio->decoded_pos < audio->decoded_frames) {
			int available = audio->decoded_frames - audio->decoded_pos;
			int take = (int)min((uint_least32_t)available, frame_count - total);
			memcpy(out + total * audio->out_channels,
				audio->decoded + audio->decoded_pos * audio->out_channels,
				(size_t)take * audio->out_channels * sizeof(float));
			audio->decoded_pos += take;
			total += take;
			continue;
		}

		if (!ffaudio_decode_next(audio))
			break;
	}
	return total;
}

static void audio_callback(possibly_unused void *data, Uint8 *stream, int len)
{
	audio_callback_epoch++;
	if (!master) {
		memset(stream, 0, len);
		channel_collect_pending_locked(false);
		return;
	}
	sts_mixer_mix_audio(&master->mixer, stream, len / (sizeof(float) * 2));
	if (master->muted)
		memset(stream, 0, len);
	channel_collect_pending_locked(false);
}

static bool cb_seek(struct channel *ch, uint_least32_t pos)
{
	if (ch->info.frames && pos > ch->info.frames)
		pos = ch->info.frames;
	if (!ffaudio_seek(ch->file, pos)) {
		WARNING("ffaudio seek failed");
		return false;
	}
	ch->frame = pos;
	return true;
}

static bool cb_loop(struct channel *ch)
{
	if (!cb_seek(ch, ch->loop_start) || ch->loop_count == 1)
		return false;
	if (ch->loop_count > 1)
		ch->loop_count--;
	return true;
}

static int cb_read_frames(struct channel *ch, float *out, uint_least32_t frame_count, uint_least32_t *num_read)
{
	*num_read = 0;

	if (ch->frame >= ch->loop_end) {
		if (!cb_loop(ch))
			return STS_STREAM_COMPLETE;
	}

	uint_least32_t frames_to_loop_end = ch->loop_end - ch->frame;
	if (frame_count >= frames_to_loop_end) {
		*num_read = ffaudio_read_frames(ch->file, out, frames_to_loop_end);
		ch->frame += *num_read;
		out += *num_read * ch->info.channels;
		frame_count -= *num_read;
		if (!cb_loop(ch))
			return STS_STREAM_COMPLETE;
	}

	uint_least32_t n = ffaudio_read_frames(ch->file, out, frame_count);
	*num_read += n;
	ch->frame += n;
	frame_count -= n;

	if (frame_count > 0) {
		if (!cb_loop(ch))
			return STS_STREAM_COMPLETE;
		uint_least32_t looped = ffaudio_read_frames(ch->file, out + n * ch->info.channels, frame_count);
		*num_read += looped;
		ch->frame += looped;
	}

	return STS_STREAM_CONTINUE;
}

static float cb_calc_fade(struct fade *fade)
{
	if (fade->elapsed >= fade->frames)
		return fade->stop ? 0.0f : fade->end_volume;

	float progress = (float)fade->elapsed / (float)fade->frames;
	float delta_v = fade->end_volume - fade->start_volume;
	float gain = fade->start_volume + delta_v * progress;
	if (gain < 0.0f)
		return 0.0f;
	if (gain > 1.0f)
		return 1.0f;
	return gain;
}

static int refill_stream(sts_mixer_sample_t *sample, void *data)
{
	struct channel *ch = data;
	if (!channel_validate(ch, "refill_stream", true))
		return STS_STREAM_COMPLETE;
	uint_least32_t frames_read;
	memset(ch->data, 0, sizeof(float) * sample->length);

	int r = cb_read_frames(ch, ch->data, CHUNK_SIZE, &frames_read);

	if (ch->info.channels == 1) {
		for (int i = CHUNK_SIZE - 1; i >= 0; i--) {
			ch->data[i * 2 + 1] = ch->data[i];
			ch->data[i * 2] = ch->data[i];
		}
	} else if (ch->swapped) {
		for (int i = 0; i < CHUNK_SIZE; i++) {
			float tmp = ch->data[i * 2];
			ch->data[i * 2] = ch->data[i * 2 + 1];
			ch->data[i * 2 + 1] = tmp;
		}
	}

	if (ch->fade.fading) {
		float gain = cb_calc_fade(&ch->fade);
		if (!channel_update_voice_gain_locked(ch, gain, "refill_stream")) {
			atomic_store_explicit(&ch->voice, -1, memory_order_relaxed);
			return STS_STREAM_COMPLETE;
		}
		ch->volume = gain * 100.0f;

		ch->fade.elapsed += frames_read;
		if (ch->fade.elapsed >= ch->fade.frames) {
			ch->fade.fading = false;
			ch->volume = ch->fade.end_volume * 100.0f;
			if (ch->fade.stop) {
				cb_seek(ch, 0);
				r = STS_STREAM_COMPLETE;
			}
		}
	} else {
		float gain = ch->volume / 100.0f;
		if (!channel_update_voice_gain_locked(ch, gain, "refill_stream")) {
			atomic_store_explicit(&ch->voice, -1, memory_order_relaxed);
			return STS_STREAM_COMPLETE;
		}
	}

	if (r == STS_STREAM_COMPLETE)
		atomic_store_explicit(&ch->voice, -1, memory_order_relaxed);

	return r;
}

static int refill_mixer(sts_mixer_sample_t *sample, void *data)
{
	struct mixer *mixer = data;
	sts_mixer_mix_audio(&mixer->mixer, &mixer->data, CHUNK_SIZE);
	if (mixer->muted)
		memset(mixer->data, 0, sizeof(float) * sample->length);
	return STS_STREAM_CONTINUE;
}

int channel_play(struct channel *ch)
{
	if (!audio_device)
		return 0;

	SDL_LockAudioDevice(audio_device);
	if (!channel_validate(ch, "channel_play", true)) {
		SDL_UnlockAudioDevice(audio_device);
		return 0;
	}
	if (atomic_load_explicit(&ch->voice, memory_order_relaxed) >= 0) {
		SDL_UnlockAudioDevice(audio_device);
		return 1;
	}
	int mixer_no = channel_resolve_mixer_no_locked(ch, "channel_play");
	if (mixer_no < 0) {
		SDL_UnlockAudioDevice(audio_device);
		return 0;
	}
	memset(ch->data, 0, sizeof(ch->data));
	int voice = sts_mixer_play_stream(&mixers[mixer_no].mixer, &ch->stream, 1.0f);
	if (voice < 0) {
		WARNING("channel_play: no free mixer voice for audio no=%d", ch->no);
		SDL_UnlockAudioDevice(audio_device);
		return 0;
	}
	atomic_store_explicit(&ch->voice, voice, memory_order_relaxed);
	ch->warned_invalid_voice = false;
	SDL_UnlockAudioDevice(audio_device);
	return 1;
}

int channel_stop(struct channel *ch)
{
	if (!audio_device)
		return 0;

	SDL_LockAudioDevice(audio_device);
	if (!channel_validate(ch, "channel_stop", false)) {
		SDL_UnlockAudioDevice(audio_device);
		return 0;
	}
	int voice = channel_voice_index_locked(ch, "channel_stop");
	if (voice < 0) {
		atomic_store_explicit(&ch->voice, -1, memory_order_relaxed);
		SDL_UnlockAudioDevice(audio_device);
		return 1;
	}
	if (ch->file)
		cb_seek(ch, 0);
	int mixer_no = channel_resolve_mixer_no_locked(ch, "channel_stop");
	if (mixer_no >= 0)
		sts_mixer_stop_voice(&mixers[mixer_no].mixer, voice);
	atomic_store_explicit(&ch->voice, -1, memory_order_relaxed);
	SDL_UnlockAudioDevice(audio_device);
	return 1;
}

int channel_is_playing(struct channel *ch)
{
	return channel_validate(ch, "channel_is_playing", false)
		&& atomic_load_explicit(&ch->voice, memory_order_relaxed) >= 0;
}

int channel_set_loop_count(struct channel *ch, int count)
{
	if (!audio_device)
		return 0;
	SDL_LockAudioDevice(audio_device);
	if (!channel_validate(ch, "channel_set_loop_count", false)) {
		SDL_UnlockAudioDevice(audio_device);
		return 0;
	}
	ch->loop_count = channel_clamp_loop_count(count);
	SDL_UnlockAudioDevice(audio_device);
	return 1;
}

int channel_get_loop_count(struct channel *ch)
{
	return ch->loop_count;
}

int channel_set_loop_start_pos(struct channel *ch, int pos)
{
	if (!audio_device)
		return 0;
	SDL_LockAudioDevice(audio_device);
	if (!channel_validate(ch, "channel_set_loop_start_pos", false)) {
		SDL_UnlockAudioDevice(audio_device);
		return 0;
	}
	ch->loop_start = channel_clamp_loop_pos(ch, pos);
	channel_normalize_loop_points(ch);
	SDL_UnlockAudioDevice(audio_device);
	return 1;
}

int channel_set_loop_end_pos(struct channel *ch, int pos)
{
	if (!audio_device)
		return 0;
	SDL_LockAudioDevice(audio_device);
	if (!channel_validate(ch, "channel_set_loop_end_pos", false)) {
		SDL_UnlockAudioDevice(audio_device);
		return 0;
	}
	ch->loop_end = channel_clamp_loop_pos(ch, pos);
	channel_normalize_loop_points(ch);
	SDL_UnlockAudioDevice(audio_device);
	return 1;
}

int channel_fade(struct channel *ch, int time, int volume, bool stop)
{
	if (!audio_device)
		return 0;
	if (time <= 0 && stop)
		return channel_stop(ch);

	SDL_LockAudioDevice(audio_device);
	if (!channel_validate(ch, "channel_fade", false)) {
		SDL_UnlockAudioDevice(audio_device);
		return 0;
	}
	if (time <= 0) {
		ch->fade.fading = false;
		ch->volume = channel_clamp_volume_percent(volume);
	} else {
		ch->fade.fading = true;
		ch->fade.stop = stop;
		ch->fade.start_pos = ch->frame;
		ch->fade.start_volume = (float)ch->volume / 100.0f;
		ch->fade.frames = channel_ms_to_duration_frames(ch, time);
		ch->fade.elapsed = 0;
		ch->fade.end_volume = (float)channel_clamp_volume_percent(volume) / 100.0f;
		if (!ch->fade.frames) {
			ch->fade.fading = false;
			ch->volume = channel_clamp_volume_percent(volume);
		}
	}
	SDL_UnlockAudioDevice(audio_device);
	return 1;
}

int channel_stop_fade(struct channel *ch)
{
	SDL_LockAudioDevice(audio_device);
	ch->fade.elapsed = ch->fade.frames;
	SDL_UnlockAudioDevice(audio_device);
	return 1;
}

int channel_is_fading(struct channel *ch)
{
	return ch->fade.fading;
}

int channel_pause(possibly_unused struct channel *ch)
{
	WARNING("channel_pause not implemented");
	return 0;
}

int channel_restart(possibly_unused struct channel *ch)
{
	WARNING("channel_restart not implemented");
	return 0;
}

int channel_is_paused(possibly_unused struct channel *ch)
{
	return 0;
}

int channel_get_pos(struct channel *ch)
{
	return muldiv(ch->frame, 1000, ch->info.samplerate);
}

int channel_get_length(struct channel *ch)
{
	return muldiv(ch->info.frames, 1000, ch->info.samplerate);
}

int channel_get_sample_pos(struct channel *ch)
{
	return ch->frame;
}

int channel_get_sample_length(struct channel *ch)
{
	return ch->info.frames;
}

int channel_seek(struct channel *ch, int pos)
{
	if (!audio_device)
		return 0;

	SDL_LockAudioDevice(audio_device);
	if (!channel_validate(ch, "channel_seek", true)) {
		SDL_UnlockAudioDevice(audio_device);
		return 0;
	}
	int r = cb_seek(ch, channel_ms_to_seek_frame(ch, pos));
	SDL_UnlockAudioDevice(audio_device);
	return r;
}

int channel_reverse_LR(struct channel *ch)
{
	ch->swapped = !ch->swapped;
	return 1;
}

int channel_get_volume(struct channel *ch)
{
	return ch->volume;
}

int channel_get_time_length(struct channel *ch)
{
	return muldiv(ch->info.frames, 1000, ch->info.samplerate);
}

int channel_get_data_no(struct channel *ch)
{
	return ch->dfile ? ch->dfile->no : -1;
}

static bool init_channel(struct channel *ch)
{
	if (!ch->file) {
		WARNING("Failed to open audio stream with FFmpeg");
		return false;
	}

	ch->magic = CHANNEL_MAGIC;
	ch->closing = false;
	ch->warned_invalid_mixer = false;
	ch->warned_invalid_voice = false;
	ch->type = ASSET_BGM;
	ch->default_mixer_no = 0;

	ch->info.channels = ch->file->out_channels;
	ch->info.samplerate = ch->file->out_sample_rate;
	ch->info.frames = ch->file->total_frames;

	ch->stream.userdata = ch;
	ch->stream.callback = refill_stream;
	ch->stream.sample.frequency = ch->info.samplerate;
	ch->stream.sample.audio_format = STS_MIXER_SAMPLE_FORMAT_FLOAT;
	ch->stream.sample.length = CHUNK_SIZE * 2;
	ch->stream.sample.data = ch->data;
	ch->voice = -1;

	ch->volume = 100;
	ch->loop_start = 0;
	ch->loop_end = ch->info.frames;
	ch->loop_count = 1;
	ch->mixer_no = 0;
	ch->no = -1;
	return true;
}

struct channel *channel_open_archive_data(struct archive_data *dfile)
{
	struct channel *ch = xcalloc(1, sizeof(struct channel));
	ch->dfile = dfile;
	ch->offset = 0;
	ch->file = ffaudio_open_archive_data(ch);

	if (!init_channel(ch)) {
		archive_free_data(dfile);
		ffaudio_close(ch->file);
		free(ch);
		return NULL;
	}
	return ch;
}

struct channel *channel_open_file(const char *path)
{
	struct channel *ch = xcalloc(1, sizeof(struct channel));
	ch->file = ffaudio_open_file(path);

	if (!init_channel(ch)) {
		ffaudio_close(ch->file);
		free(ch);
		return NULL;
	}
	return ch;
}

struct channel *channel_open(enum asset_type type, int no)
{
	struct archive_data *dfile = asset_get(type, no);
	if (!dfile) {
		WARNING("Failed to load %s %d", type == ASSET_SOUND ? "WAV" : "BGM", no);
		return NULL;
	}

	struct channel *ch = channel_open_archive_data(dfile);
	if (!ch) {
		WARNING("Failed to open %s %d", type == ASSET_SOUND ? "WAV" : "BGM", no);
		return NULL;
	}

	if (type == ASSET_SOUND) {
		ch->type = ASSET_SOUND;
		ch->default_mixer_no = channel_default_mixer_no(type);
		struct wai *wai = wai_get(no);
		ch->volume = 100;
		ch->loop_start = 0;
		ch->loop_end = ch->info.frames;
		ch->loop_count = 1;
		ch->mixer_no = sanitize_mixer_index(wai ? wai->channel : ch->default_mixer_no,
				ch->default_mixer_no);
	} else {
		ch->type = ASSET_BGM;
		ch->default_mixer_no = channel_default_mixer_no(type);
		struct bgi *bgi = bgi_get(no);
		if (bgi) {
			ch->volume = clamp(0, 100, bgi->volume);
			ch->loop_start = clamp(0, ch->info.frames, bgi->loop_start);
			ch->loop_end = clamp(0, ch->info.frames, bgi->loop_end);
			ch->loop_count = max(0, bgi->loop_count);
			ch->mixer_no = sanitize_mixer_index(bgi->channel, ch->default_mixer_no);
		} else {
			ch->loop_count = 0;
			ch->mixer_no = ch->default_mixer_no;
		}
	}
	ch->no = no;
	channel_normalize_loop_points(ch);

	return ch;
}

void channel_close(struct channel *ch)
{
	if (!ch || ch->magic != CHANNEL_MAGIC)
		return;
	if (ch->closing)
		return;

	if (!audio_device) {
		ch->closing = true;
		channel_free_immediate(ch);
		return;
	}

	/* On OHOS, rapid prepare/unprepare/play cycles can still leave a just-stopped
	 * stream reachable for a later mixer tick. Tombstone the channel under the
	 * audio lock, then defer actual resource destruction until subsequent audio
	 * callbacks have completed. */
	SDL_LockAudioDevice(audio_device);
	if (ch->closing) {
		SDL_UnlockAudioDevice(audio_device);
		return;
	}
	int voice = atomic_load_explicit(&ch->voice, memory_order_relaxed);
	int mixer_no = channel_resolve_mixer_no_locked(ch, "channel_close");
	ch->closing = true;
	if (voice >= 0 && voice < STS_MIXER_VOICES && mixer_no >= 0) {
		sts_mixer_stop_voice(&mixers[mixer_no].mixer, voice);
	}
	atomic_store_explicit(&ch->voice, -1, memory_order_relaxed);
	ch->fade.fading = false;
	ch->closing_file = ch->file;
	ch->file = NULL;
	channel_queue_destroy_locked(ch);
	SDL_UnlockAudioDevice(audio_device);
}

#define SJIS_MASTER "\x83\x7d\x83\x58\x83\x5e\x81\x5b"
#define SJIS_VOICE  "\x89\xb9\x90\xba"

void mixer_init(void)
{
	if (audio_device)  /* idempotency guard: SDL device already open, skip double-open */
		return;
	NOTICE("xsystem4 audio backend: FFmpeg");

	if (!config.mixer_nr_channels) {
		nr_mixers = 3;
		mixers = xcalloc(nr_mixers, sizeof(struct mixer));
		mixers[0].name = strdup("Music");
		mixers[1].name = strdup("Sound");
		mixers[2].name = strdup("Master");
		master = &mixers[2];
	} else {
		int need_master = 1;
		if (!strcmp(config.mixer_channels[0], "Master") || !strcmp(config.mixer_channels[0], SJIS_MASTER))
			need_master = 0;
		nr_mixers = config.mixer_nr_channels + need_master;
		mixers = xcalloc(nr_mixers, sizeof(struct mixer));
		for (unsigned i = 0; i < config.mixer_nr_channels; i++) {
			mixers[i].name = strdup(config.mixer_channels[i]);
		}
		if (need_master) {
			mixers[nr_mixers - 1].name = strdup("Master");
			master = &mixers[nr_mixers - 1];
		} else {
			master = &mixers[0];
		}
	}

	struct mixer *parent = master;
	for (int i = 0; i < nr_mixers; i++) {
		if (&mixers[i] == master)
			continue;
		mixers[i].parent = parent;
		parent->children = xrealloc_array(parent->children, parent->nr_children, parent->nr_children + 1, sizeof(struct mixer *));
		parent->children[parent->nr_children++] = &mixers[i];
		if (!strcmp(mixers[i].name, "Voice") || !strcmp(mixers[i].name, SJIS_VOICE))
			parent = &mixers[i];
	}

	for (int i = 0; i < nr_mixers; i++) {
		sts_mixer_init(&mixers[i].mixer, 44100, STS_MIXER_SAMPLE_FORMAT_FLOAT);
		int volume = i < (int)config.mixer_nr_channels ? config.mixer_volumes[i] : config.default_volume;
		mixers[i].mixer.gain = clamp(0.0f, 1.0f, (float)volume / 100.0f);
	}

	for (int i = 0; i < nr_mixers; i++) {
		if (&mixers[i] == master)
			continue;
		mixers[i].stream.userdata = &mixers[i];
		mixers[i].stream.callback = refill_mixer;
		mixers[i].stream.sample.frequency = 44100;
		mixers[i].stream.sample.audio_format = STS_MIXER_SAMPLE_FORMAT_FLOAT;
		mixers[i].stream.sample.length = CHUNK_SIZE * 2;
		mixers[i].stream.sample.data = mixers[i].data;
		mixers[i].voice = sts_mixer_play_stream(&mixers[i].parent->mixer, &mixers[i].stream, 1.0f);
	}

	if (config.bgi_path)
		bgi_read(config.bgi_path);
	if (config.wai_path)
		wai_load(config.wai_path);

	SDL_AudioSpec have;
	SDL_AudioSpec want = {
		.format = AUDIO_F32,
		.freq = 44100,
		.channels = 2,
		.samples = CHUNK_SIZE,
		.callback = audio_callback,
	};
	audio_device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	SDL_PauseAudioDevice(audio_device, 0);
}

void mixer_shutdown(void)
{
	if (!audio_device && !mixers && !pending_destroy)
		return;

	if (audio_device) {
		SDL_PauseAudioDevice(audio_device, 1);
		SDL_LockAudioDevice(audio_device);
	}

	if (mixers) {
		for (int i = 0; i < nr_mixers; i++) {
			sts_mixer_shutdown(&mixers[i].mixer);
			mixers[i].voice = -1;
		}
	}

	channel_collect_pending_locked(true);

	if (audio_device) {
		SDL_UnlockAudioDevice(audio_device);
		SDL_CloseAudioDevice(audio_device);
		audio_device = 0;
	}

	mixer_release_state();
}

int mixer_get_numof(void)
{
	return config.mixer_nr_channels;
}

const char *mixer_get_name(int n)
{
	if (n < 0 || n >= config.mixer_nr_channels)
		return NULL;
	return mixers[n].name;
}

int mixer_set_name(int n, const char *name)
{
	if (n < 0 || n >= config.mixer_nr_channels)
		return 0;
	free(mixers[n].name);
	mixers[n].name = strdup(name);
	return 1;
}

int mixer_get_volume(int n, int *volume)
{
	if (n < 0 || n >= config.mixer_nr_channels)
		return 0;
	SDL_LockAudioDevice(audio_device);
	*volume = clamp(0, 100, (int)(mixers[n].mixer.gain * 100));
	SDL_UnlockAudioDevice(audio_device);
	return 1;
}

int mixer_set_volume(int n, int volume)
{
	if (n < 0 || n >= config.mixer_nr_channels)
		return 0;
	SDL_LockAudioDevice(audio_device);
	mixers[n].mixer.gain = clamp(0.0f, 1.0f, (float)volume / 100.0f);
	SDL_UnlockAudioDevice(audio_device);
	return 1;
}

int mixer_get_mute(int n, int *mute)
{
	if (n < 0 || n >= config.mixer_nr_channels)
		return 0;
	*mute = mixers[n].muted;
	return 1;
}

int mixer_set_mute(int n, int mute)
{
	if (n < 0 || n >= config.mixer_nr_channels)
		return 0;
	mixers[n].muted = !!mute;
	return 1;
}

int mixer_stream_play(sts_mixer_stream_t *stream, int volume)
{
	if (!audio_device || !master)
		return -1;
	SDL_LockAudioDevice(audio_device);
	float gain = clamp(0.0f, 1.0f, (float)volume / 100.0f);
	int voice = sts_mixer_play_stream(&master->mixer, stream, gain);
	SDL_UnlockAudioDevice(audio_device);
	return voice;
}

bool mixer_stream_set_volume(int voice, int volume)
{
	if (!audio_device || !master || voice < 0 || voice >= STS_MIXER_VOICES)
		return false;
	SDL_LockAudioDevice(audio_device);
	master->mixer.voices[voice].gain = clamp(0.0f, 1.0f, (float)volume / 100.0f);
	SDL_UnlockAudioDevice(audio_device);
	return true;
}

void mixer_stream_stop(int voice)
{
	if (!audio_device || !master)
		return;
	SDL_LockAudioDevice(audio_device);
	sts_mixer_stop_voice(&master->mixer, voice);
	SDL_UnlockAudioDevice(audio_device);
}

void mixer_lock_audio(void)
{
	if (audio_device)
		SDL_LockAudioDevice(audio_device);
}

void mixer_unlock_audio(void)
{
	if (audio_device)
		SDL_UnlockAudioDevice(audio_device);
}