/**
 * @file descriptor_smoke.c
 * @brief Verify that a C consumer can run an ABI-v1 FFmpeg graph.
 */

#include <assert.h>
#include <math.h>
#include <string.h>

#include "rptadv_ffmpeg_adapter/rptadv_ffmpeg_adapter.h"

_Static_assert(RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_MIN_SIZE <
	       RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_PROCESS_BLOCK_MIN_SIZE,
	       "process_block must remain appended after the original ABI-v1 prefix");

/** @brief Return whether two single-precision values are sufficiently equal. */
static int approximately_equal(float left, float right)
{
	return fabsf(left - right) < 0.0001F;
}

/** @brief Verify exact-block playout preserves samples buffered by FFmpeg. */
static void test_process_block_fifo(const struct rptadv_ffmpeg_adapter_descriptor *descriptor)
{
	const struct rptadv_ffmpeg_graph_config config = {
		.struct_size = sizeof(config),
		.abi_version = RPTADV_FFMPEG_ADAPTER_ABI_VERSION,
		.sample_rate_hz = 48000,
		.maximum_frame_count = 8,
		/* The filter emits only complete 16-sample frames. */
		.filter_description = "asetnsamples=n=16:p=0",
	};
	const float first[8] = { 0.10F, 0.20F, 0.30F, 0.40F,
				   0.50F, 0.60F, 0.70F, 0.80F };
	const float second[8] = { -0.10F, -0.20F, -0.30F, -0.40F,
				    -0.50F, -0.60F, -0.70F, -0.80F };
	const float third[8] = { 0.15F, 0.25F, 0.35F, 0.45F,
				   0.55F, 0.65F, 0.75F, 0.85F };
	float output[8] = { 0.0F };
	struct rptadv_ffmpeg_graph *graph = NULL;

	assert(descriptor->create(&config, &graph) == RPTADV_FFMPEG_ADAPTER_OK);
	assert(descriptor->process_block(graph, first, 8, output) ==
	       RPTADV_FFMPEG_ADAPTER_OK);
	for (unsigned int index = 0; index < 8; ++index)
		assert(approximately_equal(output[index], 0.0F));
	assert(descriptor->process_block(graph, second, 8, output) ==
	       RPTADV_FFMPEG_ADAPTER_OK);
	for (unsigned int index = 0; index < 8; ++index)
		assert(approximately_equal(output[index], first[index]));
	assert(descriptor->process_block(graph, third, 8, output) ==
	       RPTADV_FFMPEG_ADAPTER_OK);
	for (unsigned int index = 0; index < 8; ++index)
		assert(approximately_equal(output[index], second[index]));
	descriptor->destroy(graph);
}

/** @brief Exercise descriptor discovery, graph creation, processing, and release. */
int main(void)
{
	const struct rptadv_ffmpeg_adapter_descriptor *descriptor;
	const struct rptadv_ffmpeg_graph_config config = {
		.struct_size = sizeof(config),
		.abi_version = RPTADV_FFMPEG_ADAPTER_ABI_VERSION,
		.sample_rate_hz = 48000,
		.maximum_frame_count = 8,
		.filter_description = "volume=0.5",
	};
	struct rptadv_ffmpeg_graph *graph = NULL;
	struct rptadv_ffmpeg_graph *block_graph = NULL;
	const float input[8] = { -1.0F, -0.5F, -0.25F, 0.0F,
				 0.25F, 0.5F, 0.75F, 1.0F };
	float output[8] = { 0.0F };
	uint32_t input_used = 0;
	uint32_t output_generated = 0;

	descriptor = rptadv_ffmpeg_adapter_descriptor();
	assert(descriptor != NULL);
	assert(descriptor->abi_version == RPTADV_FFMPEG_ADAPTER_ABI_VERSION);
	assert(descriptor->struct_size >=
	       RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_MIN_SIZE);
	assert(descriptor->struct_size >=
	       RPTADV_FFMPEG_ADAPTER_DESCRIPTOR_V1_PROCESS_BLOCK_MIN_SIZE);
	assert(strcmp(descriptor->capability_name,
		      RPTADV_FFMPEG_ADAPTER_CAPABILITY) == 0);
	assert(descriptor->create != NULL);
	assert(descriptor->process != NULL);
	assert(descriptor->destroy != NULL);
	assert(descriptor->process_block != NULL);
	assert(descriptor->create(&config, &graph) == RPTADV_FFMPEG_ADAPTER_OK);
	assert(graph != NULL);
	assert(descriptor->process(graph, input, 8, output, 8, &input_used,
			   &output_generated) == RPTADV_FFMPEG_ADAPTER_OK);
	assert(input_used == 8);
	assert(output_generated == 8);
	assert(approximately_equal(output[0], -0.5F));
	assert(approximately_equal(output[7], 0.5F));
	/* A graph keeps one output-draining contract for its lifetime. */
	assert(descriptor->process_block(graph, input, 8, output) ==
	       RPTADV_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	descriptor->destroy(graph);

	assert(descriptor->create(&config, &block_graph) == RPTADV_FFMPEG_ADAPTER_OK);
	assert(block_graph != NULL);
	for (unsigned int iteration = 0; iteration < 32; ++iteration) {
		memset(output, 0, sizeof(output));
		assert(descriptor->process_block(block_graph, input, 8, output) ==
		       RPTADV_FFMPEG_ADAPTER_OK);
		assert(approximately_equal(output[0], -0.5F));
		assert(approximately_equal(output[7], 0.5F));
	}
	assert(descriptor->process_block(block_graph, input, 0, output) ==
	       RPTADV_FFMPEG_ADAPTER_INVALID_ARGUMENT);
	descriptor->destroy(block_graph);
	test_process_block_fifo(descriptor);
	return 0;
}
