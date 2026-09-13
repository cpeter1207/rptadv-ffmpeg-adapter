/**
 * @file rptadv_ffmpeg_adapter.h
 * @brief Stable C ABI for the rpt_advanced FFmpeg graph adapter.
 *
 * FFmpeg headers and FFmpeg-owned objects stay private to this shared object.
 * ABI-v1 callers exchange mono normalized F32 PCM on a fixed-rate graph.
 */

#ifndef RPTADV_FFMPEG_ADAPTER_H
#define RPTADV_FFMPEG_ADAPTER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief ABI implemented by this adapter descriptor. */
#define RPTADV_FFMPEG_ADAPTER_ABI_VERSION 1U

/** @brief Stable capability string exported by the ABI-v1 descriptor. */
#define RPTADV_FFMPEG_ADAPTER_CAPABILITY "rptadv.ffmpeg"

/** @brief Opaque persistent FFmpeg graph state. */
struct rptadv_ffmpeg_graph;

/** @brief Result returned by an adapter operation. */
enum rptadv_ffmpeg_adapter_result {
	/** Operation completed. */
	RPTADV_FFMPEG_ADAPTER_OK = 0,
	/** A required pointer, rate, frame count, or graph description was invalid. */
	RPTADV_FFMPEG_ADAPTER_INVALID_ARGUMENT = -1,
	/** FFmpeg rejected or failed the requested graph operation. */
	RPTADV_FFMPEG_ADAPTER_FFMPEG_ERROR = -2,
	/** The request needs functionality not present in ABI v1. */
	RPTADV_FFMPEG_ADAPTER_UNSUPPORTED = -3,
};

/**
 * @brief Fixed properties used to create one persistent mono FFmpeg graph.
 *
 * ABI v1 intentionally accepts only sample-preserving graphs. A graph may
 * buffer internally, but it must not change the stream rate or channel count.
 * @c filter_description is the FFmpeg description between implicit @c abuffer
 * and @c abuffersink endpoints, for example @c "anull" or @c "volume=0.5".
 */
struct rptadv_ffmpeg_graph_config {
	/** Size of this config, enabling compatible future extension. */
	uint32_t struct_size;
	/** Required ABI version, currently @ref RPTADV_FFMPEG_ADAPTER_ABI_VERSION. */
	uint32_t abi_version;
	/** Immutable PCM sample rate for this graph's lifetime. */
	uint32_t sample_rate_hz;
	/** Largest process input block in mono PCM frames. */
	uint32_t maximum_frame_count;
	/** NUL-terminated FFmpeg graph description between the implicit endpoints. */
	const char *filter_description;
};

/** @brief Minimum readable size of an ABI-v1 graph configuration. */
#define RPTADV_FFMPEG_GRAPH_CONFIG_V1_MIN_SIZE \
	(offsetof(struct rptadv_ffmpeg_graph_config, filter_description) + \
	 sizeof(((struct rptadv_ffmpeg_graph_config *)0)->filter_description))

/**
 * @brief Versioned function table exported by the adapter shared object.
 *
 * A caller verifies the capability string, ABI version, minimum readable
 * prefix, and every required function pointer before using the descriptor.
 * Graph creation and destruction are control-plane operations. @ref process
 * has bounded caller-owned F32 buffers, but its real-time suitability is
 * intentionally not promised by this migration scaffold.  The optional
 * append-only @ref process_block entry provides the prepared exact-block
 * real-time contract introduced after the original ABI-v1 prefix.
 */
struct rptadv_ffmpeg_adapter_descriptor {
	/** Size of this descriptor, enabling compatible future extension. */
	uint32_t struct_size;
	/** ABI implemented by every function in this table. */
	uint32_t abi_version;
	/** Stable adapter capability name. */
	const char *capability_name;
	/**
	 * @brief Create one fixed-rate mono FFmpeg graph.
	 *
	 * @param config Complete graph properties and filter description.
	 * @param out_graph Destination for the newly owned graph on success.
	 * @return One @ref rptadv_ffmpeg_adapter_result value.
	 *
	 * The caller serializes later operations on the returned graph.
	 */
	enum rptadv_ffmpeg_adapter_result (*create)(
		const struct rptadv_ffmpeg_graph_config *config,
		struct rptadv_ffmpeg_graph **out_graph);
	/**
	 * @brief Process one bounded F32 PCM portion through a graph.
	 *
	 * @param graph Graph obtained from @ref create.
	 * @param input Input mono normalized F32 PCM, required for nonzero frames.
	 * @param input_frames Input PCM frames, no larger than the configured maximum.
	 * @param output Caller-owned mono normalized F32 output buffer.
	 * @param output_capacity Available output frames, at least the configured maximum.
	 * @param out_input_used Required destination for consumed input frames.
	 * @param out_output_generated Required destination for generated output frames.
	 * @return One @ref rptadv_ffmpeg_adapter_result value.
	 *
	 * Input and output cannot overlap. A zero-input call drains already available
	 * graph output without declaring end-of-stream. ABI v1 supports the
	 * sample-preserving graph subset needed to migrate the existing audio graph.
	 */
	enum rptadv_ffmpeg_adapter_result (*process)(
		struct rptadv_ffmpeg_graph *graph, const float *input,
		uint32_t input_frames, float *output, uint32_t output_capacity,
		uint32_t *out_input_used, uint32_t *out_output_generated);
	/**
	 * @brief Destroy a graph after all callers have stopped using it.
	 *
	 * @param graph Graph obtained from @ref create, or null.
	 */
	void (*destroy)(struct rptadv_ffmpeg_graph *graph);
	/**
	 * @brief Process one complete prepared block without allocating or waiting.
	 *
	 * @param graph Graph obtained from @ref create.
	 * @param input Normalized mono F32 input PCM.
	 * @param frame_count Input and output frame count, from one through the
	 * configured maximum.
	 * @param output Caller-owned normalized mono F32 output PCM.
	 * @return One @ref rptadv_ffmpeg_adapter_result value.
	 *
	 * This optional, append-only ABI-v1 extension returns exactly @p frame_count
	 * output samples on success.  The adapter owns a fixed source-frame pool and
	 * delayed-output FIFO allocated during @ref create, so this function performs
	 * no allocation, lock, I/O, or adapter logging.  A graph may use either this
	 * exact-block entry or the streaming @ref process entry for its lifetime, but
	 * not both.  Callers must verify
	 * @ref RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_PROCESS_BLOCK_MIN_SIZE and this
	 * function pointer before use because older ABI-v1 providers do not expose
	 * this appended entry.
	 */
	enum rptadv_ffmpeg_adapter_result (*process_block)(
		struct rptadv_ffmpeg_graph *graph, const float *input,
		uint32_t frame_count, float *output);
};

/** @brief Minimum readable size of an ABI-v1 adapter descriptor. */
#define RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_MIN_SIZE \
	(offsetof(struct rptadv_ffmpeg_adapter_descriptor, destroy) + \
	 sizeof(((struct rptadv_ffmpeg_adapter_descriptor *)0)->destroy))

/**
 * @brief Minimum descriptor size required for the appended exact-block entry.
 *
 * This intentionally differs from the original ABI-v1 descriptor prefix:
 * consumers using only the original ABI-v1 prefix remain compatible with providers predating
 * @ref rptadv_ffmpeg_adapter_descriptor::process_block.
 */
#define RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_PROCESS_BLOCK_MIN_SIZE \
	(offsetof(struct rptadv_ffmpeg_adapter_descriptor, process_block) + \
	 sizeof(((struct rptadv_ffmpeg_adapter_descriptor *)0)->process_block))

#if defined(__cplusplus)
static_assert(sizeof(enum rptadv_ffmpeg_adapter_result) == sizeof(int),
	      "adapter result enum must use the C int ABI");
static_assert(RPTADV_FFMPEG_ADAPTER_OK == 0,
	      "adapter result values are part of ABI v1");
static_assert(RPTADV_FFMPEG_ADAPTER_INVALID_ARGUMENT == -1,
	      "adapter result values are part of ABI v1");
static_assert(RPTADV_FFMPEG_ADAPTER_FFMPEG_ERROR == -2,
	      "adapter result values are part of ABI v1");
static_assert(RPTADV_FFMPEG_ADAPTER_UNSUPPORTED == -3,
	      "adapter result values are part of ABI v1");
#elif defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
_Static_assert(sizeof(enum rptadv_ffmpeg_adapter_result) == sizeof(int),
	       "adapter result enum must use the C int ABI");
_Static_assert(RPTADV_FFMPEG_ADAPTER_OK == 0,
	       "adapter result values are part of ABI v1");
_Static_assert(RPTADV_FFMPEG_ADAPTER_INVALID_ARGUMENT == -1,
	       "adapter result values are part of ABI v1");
_Static_assert(RPTADV_FFMPEG_ADAPTER_FFMPEG_ERROR == -2,
	       "adapter result values are part of ABI v1");
_Static_assert(RPTADV_FFMPEG_ADAPTER_UNSUPPORTED == -3,
	       "adapter result values are part of ABI v1");
#endif

/**
 * @brief Return the immutable ABI-v1 FFmpeg graph adapter descriptor.
 *
 * @return A process-lifetime descriptor; it must not be freed or modified.
 */
const struct rptadv_ffmpeg_adapter_descriptor *
rptadv_ffmpeg_adapter_descriptor(void);

#ifdef __cplusplus
}
#endif

#endif
