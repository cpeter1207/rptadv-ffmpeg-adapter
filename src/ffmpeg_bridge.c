/**
 * @file ffmpeg_bridge.c
 * @brief Private ownership of all FFmpeg graph objects.
 *
 * Keeping FFmpeg's changing headers and opaque object layouts in this small C
 * bridge lets the Rust-facing public descriptor remain ABI-stable.
 */

#include "ffmpeg_bridge.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/audio_fifo.h>
#include <libavutil/channel_layout.h>
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>

/** Number of source frames reserved for exact-block graph processing. */
#define RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT 8U

/** @brief Selects the output-draining contract used by one graph. */
enum rptadv_ffmpeg_bridge_processing_contract {
	/** @brief No processing contract has been selected yet. */
	RPTADV_FFMPEG_BRIDGE_PROCESSING_UNSET,
	/** @brief The original caller-managed streaming contract is active. */
	RPTADV_FFMPEG_BRIDGE_PROCESSING_STREAMING,
	/** @brief The prepared exact-block contract is active. */
	RPTADV_FFMPEG_BRIDGE_PROCESSING_BLOCK,
};

/** @brief FFmpeg objects owned by one public graph handle. */
struct rptadv_ffmpeg_bridge_graph {
	/** @brief Entire configured FFmpeg graph. */
	AVFilterGraph *filter_graph;
	/** @brief Implicit mono F32 graph source. */
	AVFilterContext *source;
	/** @brief Implicit mono F32 graph sink. */
	AVFilterContext *sink;
	/** @brief Reused caller-input frame sized for the declared maximum. */
	AVFrame *input_frame;
	/** @brief Preallocated input frames retained safely by an exact-block graph. */
	AVFrame *block_input_frames[RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT];
	/** @brief Reused graph-output frame. */
	AVFrame *output_frame;
	/** @brief Buffered graph output used to return one complete exact block. */
	AVAudioFifo *block_output_fifo;
	/** @brief Largest permitted input block. */
	uint32_t maximum_frame_count;
	/** @brief Next preallocated exact-block input frame to inspect. */
	uint32_t block_input_frame_index;
	/** @brief Selected output-draining contract for this graph lifetime. */
	enum rptadv_ffmpeg_bridge_processing_contract processing_contract;
	/** @brief Nonzero after the exact-block graph has emitted real output. */
	int block_output_started;
};

void rptadv_ffmpeg_bridge_destroy(struct rptadv_ffmpeg_bridge_graph *graph)
{
	if (graph == NULL) {
		return;
	}
	av_frame_free(&graph->input_frame);
	for (size_t index = 0; index < RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT; ++index)
		av_frame_free(&graph->block_input_frames[index]);
	av_frame_free(&graph->output_frame);
	av_audio_fifo_free(graph->block_output_fifo);
	avfilter_graph_free(&graph->filter_graph);
	free(graph);
}

/**
 * @brief Allocate one normalized mono F32 source frame with fixed capacity.
 *
 * @param slot Destination for the newly allocated frame.
 * @param sample_rate_hz Immutable source sample rate.
 * @param maximum_frame_count Fixed capacity in mono PCM frames.
 * @return Zero on success or an FFmpeg error code.
 */
static int configure_pcm_frame(AVFrame **slot, uint32_t sample_rate_hz,
			       uint32_t maximum_frame_count)
{
	AVFrame *frame;
	int result;

	frame = av_frame_alloc();
	if (frame == NULL) {
		return AVERROR(ENOMEM);
	}
	frame->format = AV_SAMPLE_FMT_FLT;
	frame->sample_rate = (int)sample_rate_hz;
	av_channel_layout_default(&frame->ch_layout, 1);
	frame->nb_samples = (int)maximum_frame_count;
	result = av_frame_get_buffer(frame, 0);
	if (result < 0) {
		av_frame_free(&frame);
		return result;
	}
	*slot = frame;
	return 0;
}

/**
 * @brief Configure the source/output frames used by the streaming API.
 *
 * @param graph Allocated graph state with a declared maximum frame count.
 * @param sample_rate_hz Immutable source sample rate.
 * @return Zero on success or an FFmpeg error code.
 */
static int configure_streaming_buffers(struct rptadv_ffmpeg_bridge_graph *graph,
				       uint32_t sample_rate_hz)
{
	int result = configure_pcm_frame(&graph->input_frame, sample_rate_hz,
					 graph->maximum_frame_count);

	if (result < 0) {
		return result;
	}
	graph->output_frame = av_frame_alloc();
	if (graph->output_frame == NULL) {
		return AVERROR(ENOMEM);
	}
	return 0;
}

/**
 * @brief Reserve every buffer needed by the exact-block callback contract.
 *
 * @param graph Allocated graph state with a declared maximum frame count.
 * @param sample_rate_hz Immutable source sample rate.
 * @return Zero on success or an FFmpeg error code.
 */
static int configure_block_buffers(struct rptadv_ffmpeg_bridge_graph *graph,
				   uint32_t sample_rate_hz)
{
	uint32_t capacity;

	if (graph->maximum_frame_count > INT_MAX / RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT)
		return AVERROR(EINVAL);
	capacity = graph->maximum_frame_count * RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT;
	graph->block_output_fifo = av_audio_fifo_alloc(AV_SAMPLE_FMT_FLT, 1, (int)capacity);
	if (graph->block_output_fifo == NULL)
		return AVERROR(ENOMEM);
	for (size_t index = 0; index < RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT; ++index) {
		int result = configure_pcm_frame(&graph->block_input_frames[index], sample_rate_hz,
						 graph->maximum_frame_count);

		if (result < 0)
			return result;
	}
	return 0;
}

/**
 * @brief Connect the implicit source and sink through a caller graph string.
 *
 * @param graph Allocated graph state.
 * @param sample_rate_hz Immutable source sample rate.
 * @param filter_description Caller graph string between implicit endpoints.
 * @return Zero on success or an FFmpeg error code.
 */
static int configure_filter_graph(struct rptadv_ffmpeg_bridge_graph *graph,
					  uint32_t sample_rate_hz,
					  const char *filter_description)
{
	AVFilterInOut *inputs = NULL;
	AVFilterInOut *outputs = NULL;
	const AVFilter *source_filter;
	const AVFilter *sink_filter;
	char source_args[128];
	int result;

	source_filter = avfilter_get_by_name("abuffer");
	sink_filter = avfilter_get_by_name("abuffersink");
	if (source_filter == NULL || sink_filter == NULL) {
		return AVERROR_FILTER_NOT_FOUND;
	}
	if (snprintf(source_args, sizeof(source_args),
		     "time_base=1/%u:sample_rate=%u:sample_fmt=flt:channel_layout=mono",
		     sample_rate_hz, sample_rate_hz) >= (int)sizeof(source_args)) {
		return AVERROR(EINVAL);
	}
	graph->filter_graph = avfilter_graph_alloc();
	if (graph->filter_graph == NULL) {
		return AVERROR(ENOMEM);
	}
	result = avfilter_graph_create_filter(&graph->source, source_filter, "in",
					     source_args, NULL, graph->filter_graph);
	if (result < 0) {
		return result;
	}
	result = avfilter_graph_create_filter(&graph->sink, sink_filter, "out", NULL,
					     NULL, graph->filter_graph);
	if (result < 0) {
		return result;
	}
	outputs = avfilter_inout_alloc();
	inputs = avfilter_inout_alloc();
	if (outputs == NULL || inputs == NULL) {
		result = AVERROR(ENOMEM);
		goto done;
	}
	outputs->name = av_strdup("in");
	outputs->filter_ctx = graph->source;
	outputs->pad_idx = 0;
	outputs->next = NULL;
	inputs->name = av_strdup("out");
	inputs->filter_ctx = graph->sink;
	inputs->pad_idx = 0;
	inputs->next = NULL;
	if (outputs->name == NULL || inputs->name == NULL) {
		result = AVERROR(ENOMEM);
		goto done;
	}
	result = avfilter_graph_parse_ptr(graph->filter_graph, filter_description,
					  &inputs, &outputs, NULL);
	if (result < 0) {
		goto done;
	}
	result = avfilter_graph_config(graph->filter_graph, NULL);

done:
	avfilter_inout_free(&inputs);
	avfilter_inout_free(&outputs);
	return result;
}

int rptadv_ffmpeg_bridge_create(uint32_t sample_rate_hz,
				uint32_t maximum_frame_count,
				const char *filter_description,
				struct rptadv_ffmpeg_bridge_graph **out_graph)
{
	struct rptadv_ffmpeg_bridge_graph *graph;
	int result;

	if (out_graph == NULL || sample_rate_hz == 0 || maximum_frame_count == 0 ||
	    maximum_frame_count > INT_MAX || filter_description == NULL ||
	    filter_description[0] == '\0') {
		return RPTADV_FFMPEG_BRIDGE_INVALID_ARGUMENT;
	}
	*out_graph = NULL;
	graph = calloc(1, sizeof(*graph));
	if (graph == NULL) {
		return RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR;
	}
	graph->maximum_frame_count = maximum_frame_count;
	result = configure_streaming_buffers(graph, sample_rate_hz);
	if (result >= 0) {
		result = configure_filter_graph(graph, sample_rate_hz, filter_description);
	}
	if (result >= 0) {
		result = configure_block_buffers(graph, sample_rate_hz);
	}
	if (result < 0) {
		rptadv_ffmpeg_bridge_destroy(graph);
		return RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR;
	}
	*out_graph = graph;
	return RPTADV_FFMPEG_BRIDGE_OK;
}

/**
 * @brief Check that one graph output frame is normalized mono F32 PCM.
 *
 * @param frame FFmpeg output frame to validate.
 * @return Nonzero when @p frame has the graph's declared PCM format.
 */
static int output_frame_is_normalized_mono_f32(const AVFrame *frame)
{
	return frame->format == AV_SAMPLE_FMT_FLT && frame->ch_layout.nb_channels == 1 &&
	       frame->nb_samples >= 0;
}

/**
 * @brief Copy the available graph output into a caller-owned F32 buffer.
 *
 * @param graph Initialized bridge graph.
 * @param output Destination F32 buffer.
 * @param output_capacity Destination capacity in frames.
 * @param out_output_generated Running result count updated on success.
 * @return Zero on success or an FFmpeg error code.
 */
static int drain_output(struct rptadv_ffmpeg_bridge_graph *graph, float *output,
			uint32_t output_capacity, uint32_t *out_output_generated)
{
	for (;;) {
		int result = av_buffersink_get_frame(graph->sink, graph->output_frame);
		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
			return 0;
		}
		if (result < 0) {
			return result;
		}
		if (!output_frame_is_normalized_mono_f32(graph->output_frame) ||
		    (uint32_t)graph->output_frame->nb_samples >
			    output_capacity - *out_output_generated) {
			av_frame_unref(graph->output_frame);
			return AVERROR(EINVAL);
		}
		memcpy(output + *out_output_generated, graph->output_frame->data[0],
		       (size_t)graph->output_frame->nb_samples * sizeof(*output));
		*out_output_generated += (uint32_t)graph->output_frame->nb_samples;
		av_frame_unref(graph->output_frame);
	}
}

/**
 * @brief Find an exact-block source frame which FFmpeg no longer retains.
 *
 * @param graph Initialized bridge graph.
 * @return A writable preallocated source frame, or null when the pool is busy.
 */
static AVFrame *next_block_input_frame(struct rptadv_ffmpeg_bridge_graph *graph)
{
	for (uint32_t offset = 0; offset < RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT; ++offset) {
		uint32_t index =
			(graph->block_input_frame_index + offset) % RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT;
		AVFrame *frame = graph->block_input_frames[index];

		if (frame != NULL && av_frame_is_writable(frame)) {
			graph->block_input_frame_index =
				(index + 1) % RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT;
			return frame;
		}
	}
	return NULL;
}

/**
 * @brief Drain every available output frame into the fixed exact-block FIFO.
 *
 * @param graph Initialized bridge graph.
 * @return Zero on success or an FFmpeg error code.
 */
static int drain_block_output(struct rptadv_ffmpeg_bridge_graph *graph)
{
	for (;;) {
		int result = av_buffersink_get_frame(graph->sink, graph->output_frame);

		if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
			return 0;
		if (result < 0)
			return result;
		if (!output_frame_is_normalized_mono_f32(graph->output_frame) ||
		    graph->output_frame->nb_samples > av_audio_fifo_space(graph->block_output_fifo)) {
			av_frame_unref(graph->output_frame);
			return AVERROR(EINVAL);
		}
		{
			void *planes[1] = { graph->output_frame->data[0] };
			int written = av_audio_fifo_write(graph->block_output_fifo, planes,
							 graph->output_frame->nb_samples);

			if (written != graph->output_frame->nb_samples) {
				av_frame_unref(graph->output_frame);
				return AVERROR(EIO);
			}
		}
		av_frame_unref(graph->output_frame);
	}
}

/**
 * @brief Deliver exactly one requested block from the fixed output FIFO.
 *
 * @param graph Initialized bridge graph.
 * @param output Destination for @p frame_count samples.
 * @param frame_count Requested output frames.
 * @return Zero on success or an FFmpeg error code.
 */
static int read_block_output(struct rptadv_ffmpeg_bridge_graph *graph, float *output,
			     uint32_t frame_count)
{
	int available = av_audio_fifo_size(graph->block_output_fifo);
	uint32_t copied = available < (int)frame_count ? (uint32_t)available : frame_count;
	uint32_t fill = frame_count - copied;
	void *planes[1];
	int read;

	if (!graph->block_output_started) {
		memset(output, 0, (size_t)fill * sizeof(*output));
		if (copied == 0)
			return 0;
		planes[0] = output + fill;
		read = av_audio_fifo_read(graph->block_output_fifo, planes, (int)copied);
		if (read != (int)copied)
			return AVERROR(EIO);
		graph->block_output_started = 1;
		return 0;
	}
	if (copied != 0) {
		planes[0] = output;
		read = av_audio_fifo_read(graph->block_output_fifo, planes, (int)copied);
		if (read != (int)copied)
			return AVERROR(EIO);
	}
	memset(output + copied, 0, (size_t)fill * sizeof(*output));
	return 0;
}

int rptadv_ffmpeg_bridge_process(struct rptadv_ffmpeg_bridge_graph *graph,
				 const float *input, uint32_t input_frames,
				 float *output, uint32_t output_capacity,
				 uint32_t *out_input_used,
				 uint32_t *out_output_generated)
{
	if (graph == NULL || output == NULL || out_input_used == NULL ||
	    out_output_generated == NULL || output_capacity < graph->maximum_frame_count ||
	    input_frames > graph->maximum_frame_count ||
	    (input_frames != 0 && input == NULL) ||
	    graph->processing_contract == RPTADV_FFMPEG_BRIDGE_PROCESSING_BLOCK) {
		return RPTADV_FFMPEG_BRIDGE_INVALID_ARGUMENT;
	}
	graph->processing_contract = RPTADV_FFMPEG_BRIDGE_PROCESSING_STREAMING;
	*out_input_used = 0;
	*out_output_generated = 0;
	if (input_frames != 0) {
		int result;

		result = av_frame_make_writable(graph->input_frame);
		if (result < 0) {
			return RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR;
		}
		graph->input_frame->nb_samples = (int)input_frames;
		graph->input_frame->pts = AV_NOPTS_VALUE;
		memcpy(graph->input_frame->data[0], input,
		       (size_t)input_frames * sizeof(*input));
		result = av_buffersrc_add_frame_flags(graph->source, graph->input_frame,
						     AV_BUFFERSRC_FLAG_KEEP_REF);
		if (result < 0) {
			return RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR;
		}
		*out_input_used = input_frames;
	}
	if (drain_output(graph, output, output_capacity, out_output_generated) < 0) {
		*out_input_used = 0;
		*out_output_generated = 0;
		return RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR;
	}
	return RPTADV_FFMPEG_BRIDGE_OK;
}

int rptadv_ffmpeg_bridge_process_block(struct rptadv_ffmpeg_bridge_graph *graph,
				       const float *input, uint32_t frame_count,
				       float *output)
{
	AVFrame *frame;
	int result;

	if (graph == NULL || input == NULL || output == NULL || frame_count == 0 ||
	    frame_count > graph->maximum_frame_count ||
	    graph->processing_contract == RPTADV_FFMPEG_BRIDGE_PROCESSING_STREAMING) {
		return RPTADV_FFMPEG_BRIDGE_INVALID_ARGUMENT;
	}
	graph->processing_contract = RPTADV_FFMPEG_BRIDGE_PROCESSING_BLOCK;
	frame = next_block_input_frame(graph);
	if (frame == NULL)
		return RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR;
	frame->nb_samples = (int)frame_count;
	frame->pts = AV_NOPTS_VALUE;
	memcpy(frame->data[0], input, (size_t)frame_count * sizeof(*input));
	result = av_buffersrc_add_frame_flags(graph->source, frame,
					 AV_BUFFERSRC_FLAG_KEEP_REF | AV_BUFFERSRC_FLAG_PUSH);
	if (result < 0)
		return RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR;
	result = drain_block_output(graph);
	if (result < 0)
		return RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR;
	result = read_block_output(graph, output, frame_count);
	return result < 0 ? RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR : RPTADV_FFMPEG_BRIDGE_OK;
}
