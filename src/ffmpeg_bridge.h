/**
 * @file ffmpeg_bridge.h
 * @brief Private C bridge that contains all direct FFmpeg ABI use.
 */

#ifndef RPTADV_FFMPEG_BRIDGE_H
#define RPTADV_FFMPEG_BRIDGE_H

#include <stdint.h>

/** @brief Opaque FFmpeg graph state private to the bridge. */
struct rptadv_ffmpeg_bridge_graph;

/** @brief Successful private bridge result. */
#define RPTADV_FFMPEG_BRIDGE_OK 0
/** @brief Invalid private bridge argument. */
#define RPTADV_FFMPEG_BRIDGE_INVALID_ARGUMENT -1
/** @brief FFmpeg failed a private bridge operation. */
#define RPTADV_FFMPEG_BRIDGE_FFMPEG_ERROR -2

/**
 * @brief Create one FFmpeg graph with implicit mono F32 source and sink.
 *
 * @param sample_rate_hz Immutable graph sample rate.
 * @param maximum_frame_count Largest accepted input frame count.
 * @param filter_description FFmpeg graph between the implicit endpoints.
 * @param out_graph Destination for a newly owned bridge graph.
 * @return One @c RPTADV_FFMPEG_BRIDGE_* result.
 */
int rptadv_ffmpeg_bridge_create(uint32_t sample_rate_hz,
				uint32_t maximum_frame_count,
				const char *filter_description,
				struct rptadv_ffmpeg_bridge_graph **out_graph);

/**
 * @brief Process or drain a bounded F32 graph block.
 *
 * @param graph Graph returned by @ref rptadv_ffmpeg_bridge_create.
 * @param input F32 input or null when @p input_frames is zero.
 * @param input_frames Number of input frames.
 * @param output F32 output buffer.
 * @param output_capacity Available output frames.
 * @param out_input_used Destination for consumed input frames.
 * @param out_output_generated Destination for produced output frames.
 * @return One @c RPTADV_FFMPEG_BRIDGE_* result.
 */
int rptadv_ffmpeg_bridge_process(struct rptadv_ffmpeg_bridge_graph *graph,
				 const float *input, uint32_t input_frames,
				 float *output, uint32_t output_capacity,
				 uint32_t *out_input_used,
				 uint32_t *out_output_generated);

/**
 * @brief Process one exact F32 block through a prepared graph.
 *
 * @param graph Graph returned by @ref rptadv_ffmpeg_bridge_create.
 * @param input Normalized F32 input PCM.
 * @param frame_count Input and output frame count.
 * @param output Destination for exactly @p frame_count normalized F32 samples.
 * @return One @c RPTADV_FFMPEG_BRIDGE_* result.
 *
 * The graph reserves its complete source-frame pool and output FIFO during
 * creation. This callback path never allocates, locks, performs I/O, or emits
 * bridge log messages. A graph cannot mix this operation with
 * @ref rptadv_ffmpeg_bridge_process because the two contracts drain output
 * differently.
 */
int rptadv_ffmpeg_bridge_process_block(struct rptadv_ffmpeg_bridge_graph *graph,
				       const float *input, uint32_t frame_count,
				       float *output);

/**
 * @brief Release a bridge graph.
 *
 * @param graph Graph returned by @ref rptadv_ffmpeg_bridge_create, or null.
 */
void rptadv_ffmpeg_bridge_destroy(struct rptadv_ffmpeg_bridge_graph *graph);

#endif
