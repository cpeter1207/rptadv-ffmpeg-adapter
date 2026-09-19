/** @file bridge_faults.c
 * @brief Exercise real bridge code with deterministic FFmpeg failure returns.
 *
 * Only this test translation unit substitutes dependency calls. The release
 * bridge is compiled separately, without these substitutions. Successful calls
 * delegate to the actual dynamically linked FFmpeg implementation.
 */
#include <errno.h>
#include <stdint.h>
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
#include <libavutil/opt.h>
#include <libavutil/samplefmt.h>

static void check(int condition, int line)
{
    if (!condition) {
        fprintf(stderr, "bridge check failed at line %d\n", line);
        abort();
    }
}

#define CHECK(expression) check((expression), __LINE__)

enum fault_point {
    NONE, CALLOC, FRAME_ALLOC, FRAME_BUFFER, FILTER_LOOKUP, GRAPH_ALLOC,
    FILTER_CREATE, INOUT_ALLOC, STRING_DUP, GRAPH_PARSE, GRAPH_CONFIG,
    FIFO_ALLOC, FRAME_WRITABLE, SOURCE_ADD, FIFO_WRITE, FIFO_READ, OPT_SET
};
static enum fault_point selected_fault;
static unsigned int failure_call, observed_calls;
static int sink_mode, sink_emitted;

static int fail(enum fault_point point)
{
    return point == selected_fault && ++observed_calls == failure_call;
}

static void select_fault(enum fault_point point, unsigned int call)
{
    selected_fault = point;
    failure_call = call;
    observed_calls = 0;
}

/* Model the sink's documented error returns and malformed external frames. */
static int scripted_sink(AVFilterContext *sink, AVFrame *frame)
{
    if (sink_mode == 0)
        return av_buffersink_get_frame(sink, frame);
    if (sink_mode == 1)
        return AVERROR_EOF;
    if (sink_mode == 2)
        return AVERROR(EIO);
    if (sink_emitted++)
        return AVERROR(EAGAIN);
    av_frame_unref(frame);
    frame->format = sink_mode == 8 ? AV_SAMPLE_FMT_FLTP : AV_SAMPLE_FMT_FLT;
    av_channel_layout_default(&frame->ch_layout, 1);
    frame->nb_samples = 8;
    CHECK(av_frame_get_buffer(frame, 0) == 0);
    memset(frame->data[0], 0, 8 * sizeof(float));
    if (sink_mode == 3)
        frame->format = AV_SAMPLE_FMT_S16;
    if (sink_mode == 4)
        frame->ch_layout.nb_channels = 2;
    if (sink_mode == 5)
        frame->nb_samples = -1;
    if (sink_mode == 6)
        frame->nb_samples = 65;
    return 0;
}

#define calloc(...) (fail(CALLOC) ? NULL : calloc(__VA_ARGS__))
#define av_frame_alloc(...) (fail(FRAME_ALLOC) ? NULL : av_frame_alloc(__VA_ARGS__))
#define av_frame_get_buffer(...) (fail(FRAME_BUFFER) ? AVERROR(ENOMEM) : av_frame_get_buffer(__VA_ARGS__))
#define avfilter_get_by_name(...) (fail(FILTER_LOOKUP) ? NULL : avfilter_get_by_name(__VA_ARGS__))
#define avfilter_graph_alloc(...) (fail(GRAPH_ALLOC) ? NULL : avfilter_graph_alloc(__VA_ARGS__))
#define avfilter_graph_create_filter(...) (fail(FILTER_CREATE) ? AVERROR(EIO) : avfilter_graph_create_filter(__VA_ARGS__))
#define av_opt_set_bin(...) (fail(OPT_SET) ? AVERROR(EIO) : av_opt_set_bin(__VA_ARGS__))
#define avfilter_inout_alloc(...) (fail(INOUT_ALLOC) ? NULL : avfilter_inout_alloc(__VA_ARGS__))
#define av_strdup(...) (fail(STRING_DUP) ? NULL : av_strdup(__VA_ARGS__))
#define avfilter_graph_parse_ptr(...) (fail(GRAPH_PARSE) ? AVERROR(EIO) : avfilter_graph_parse_ptr(__VA_ARGS__))
#define avfilter_graph_config(...) (fail(GRAPH_CONFIG) ? AVERROR(EIO) : avfilter_graph_config(__VA_ARGS__))
#define av_audio_fifo_alloc(...) (fail(FIFO_ALLOC) ? NULL : av_audio_fifo_alloc(__VA_ARGS__))
#define av_frame_make_writable(...) (fail(FRAME_WRITABLE) ? AVERROR(ENOMEM) : av_frame_make_writable(__VA_ARGS__))
#define av_buffersrc_add_frame_flags(...) (fail(SOURCE_ADD) ? AVERROR(EIO) : av_buffersrc_add_frame_flags(__VA_ARGS__))
#define av_audio_fifo_write(...) (fail(FIFO_WRITE) ? AVERROR(EIO) : av_audio_fifo_write(__VA_ARGS__))
#define av_audio_fifo_read(...) (fail(FIFO_READ) ? AVERROR(EIO) : av_audio_fifo_read(__VA_ARGS__))
#define av_buffersink_get_frame(...) scripted_sink(__VA_ARGS__)
#include "../src/ffmpeg_bridge.c"
#undef calloc
#undef av_frame_alloc
#undef av_frame_get_buffer
#undef avfilter_get_by_name
#undef avfilter_graph_alloc
#undef avfilter_graph_create_filter
#undef av_opt_set_bin
#undef avfilter_inout_alloc
#undef av_strdup
#undef avfilter_graph_parse_ptr
#undef avfilter_graph_config
#undef av_audio_fifo_alloc
#undef av_frame_make_writable
#undef av_buffersrc_add_frame_flags
#undef av_audio_fifo_write
#undef av_audio_fifo_read
#undef av_buffersink_get_frame

static struct rptadv_ffmpeg_bridge_graph *new_graph(void)
{
    struct rptadv_ffmpeg_bridge_graph *graph = NULL;
    select_fault(NONE, 0);
    sink_mode = 0;
    sink_emitted = 0;
    CHECK(rptadv_ffmpeg_bridge_create(48000, 8, "anull", &graph) == 0);
    CHECK(graph != NULL);
    return graph;
}

static void creation_failures(void)
{
    struct rptadv_ffmpeg_bridge_graph *graph = NULL;
    const struct { enum fault_point point; unsigned int calls; } faults[] = {
        { CALLOC, 1 }, { FRAME_ALLOC, 10 }, { FRAME_BUFFER, 9 },
        { FILTER_LOOKUP, 2 }, { GRAPH_ALLOC, 1 }, { FILTER_CREATE, 2 },
        { INOUT_ALLOC, 2 }, { STRING_DUP, 2 }, { GRAPH_PARSE, 1 },
        { GRAPH_CONFIG, 1 }, { FIFO_ALLOC, 1 }, { OPT_SET, 1 }
    };
    for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); ++i) {
        for (unsigned int call = 1; call <= faults[i].calls; ++call) {
            select_fault(faults[i].point, call);
            CHECK(rptadv_ffmpeg_bridge_create(48000, 8, "anull", &graph) == -2);
            CHECK(graph == NULL);
        }
    }
    select_fault(NONE, 0);
    CHECK(rptadv_ffmpeg_bridge_create(48000, 8, "anull", NULL) == -1);
    CHECK(rptadv_ffmpeg_bridge_create(0, 8, "anull", &graph) == -1);
    CHECK(rptadv_ffmpeg_bridge_create(48000, 0, "anull", &graph) == -1);
    CHECK(rptadv_ffmpeg_bridge_create(48000, UINT32_MAX, "anull", &graph) == -1);
    CHECK(rptadv_ffmpeg_bridge_create(48000, 8, NULL, &graph) == -1);
    CHECK(rptadv_ffmpeg_bridge_create(48000, 8, "", &graph) == -1);
    rptadv_ffmpeg_bridge_destroy(NULL);
    struct rptadv_ffmpeg_bridge_graph oversized = { .maximum_frame_count = INT_MAX };
    CHECK(configure_block_buffers(&oversized, 48000) == AVERROR(EINVAL));
}

static void streaming_failures(void)
{
    struct rptadv_ffmpeg_bridge_graph *graph = new_graph();
    const float input[8] = { 0 };
    float output[8];
    uint32_t used = 0, generated = 0;
    CHECK(rptadv_ffmpeg_bridge_process(NULL, input, 8, output, 8, &used, &generated) == -1);
    CHECK(rptadv_ffmpeg_bridge_process(graph, input, 8, NULL, 8, &used, &generated) == -1);
    CHECK(rptadv_ffmpeg_bridge_process(graph, input, 8, output, 8, NULL, &generated) == -1);
    CHECK(rptadv_ffmpeg_bridge_process(graph, input, 8, output, 8, &used, NULL) == -1);
    CHECK(rptadv_ffmpeg_bridge_process(graph, input, 8, output, 7, &used, &generated) == -1);
    CHECK(rptadv_ffmpeg_bridge_process(graph, input, 9, output, 8, &used, &generated) == -1);
    CHECK(rptadv_ffmpeg_bridge_process(graph, NULL, 8, output, 8, &used, &generated) == -1);
    graph->processing_contract = RPTADV_FFMPEG_BRIDGE_PROCESSING_BLOCK;
    CHECK(rptadv_ffmpeg_bridge_process(graph, input, 8, output, 8, &used, &generated) == -1);
    graph->processing_contract = RPTADV_FFMPEG_BRIDGE_PROCESSING_UNSET;
    for (int mode = 1; mode <= 8; ++mode) {
        sink_mode = mode;
        sink_emitted = 0;
        int expected = mode == 1 || mode >= 7 ? 0 : -2;
        CHECK(rptadv_ffmpeg_bridge_process(graph, NULL, 0, output, 8, &used, &generated) == expected);
    }
    sink_mode = 0;
    select_fault(FRAME_WRITABLE, 1);
    CHECK(rptadv_ffmpeg_bridge_process(graph, input, 8, output, 8, &used, &generated) == -2);
    select_fault(SOURCE_ADD, 1);
    CHECK(rptadv_ffmpeg_bridge_process(graph, input, 8, output, 8, &used, &generated) == -2);
    select_fault(NONE, 0);
    CHECK(rptadv_ffmpeg_bridge_process(graph, input, 8, output, 8, &used, &generated) == 0);
    CHECK(used == 8 && generated == 8);
    rptadv_ffmpeg_bridge_destroy(graph);
}

static void block_failures(void)
{
    struct rptadv_ffmpeg_bridge_graph *graph = new_graph();
    const float input[8] = { 0 };
    float output[8];
    CHECK(rptadv_ffmpeg_bridge_process_block(NULL, input, 8, output) == -1);
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, NULL, 8, output) == -1);
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 8, NULL) == -1);
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 0, output) == -1);
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 9, output) == -1);
    graph->processing_contract = RPTADV_FFMPEG_BRIDGE_PROCESSING_STREAMING;
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 8, output) == -1);
    graph->processing_contract = RPTADV_FFMPEG_BRIDGE_PROCESSING_UNSET;
    struct rptadv_ffmpeg_bridge_graph empty = { 0 };
    CHECK(next_block_input_frame(&empty) == NULL);
    AVFrame *held[RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT];
    for (size_t i = 0; i < RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT; ++i) {
        held[i] = av_frame_clone(graph->block_input_frames[i]);
        CHECK(held[i] != NULL);
    }
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 8, output) == -2);
    for (size_t i = 0; i < RPTADV_FFMPEG_BLOCK_INPUT_FRAME_COUNT; ++i)
        av_frame_free(&held[i]);
    select_fault(SOURCE_ADD, 1);
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 8, output) == -2);
    select_fault(NONE, 0);
    rptadv_ffmpeg_bridge_destroy(graph);
    for (int mode = 1; mode <= 8; ++mode) {
        graph = new_graph();
        sink_mode = mode;
        sink_emitted = 0;
        int expected = mode == 1 || mode >= 7 ? 0 : -2;
        CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 8, output) == expected);
        rptadv_ffmpeg_bridge_destroy(graph);
    }
    graph = new_graph();
    select_fault(FIFO_WRITE, 1);
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 8, output) == -2);
    rptadv_ffmpeg_bridge_destroy(graph);
    for (int started = 0; started <= 1; ++started) {
        graph = new_graph();
        graph->block_output_started = started;
        select_fault(FIFO_READ, 1);
        CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 8, output) == -2);
        rptadv_ffmpeg_bridge_destroy(graph);
    }
    graph = new_graph();
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 8, output) == 0);
    CHECK(rptadv_ffmpeg_bridge_process_block(graph, input, 8, output) == 0);
    CHECK(read_block_output(graph, output, 8) == 0);
    rptadv_ffmpeg_bridge_destroy(graph);
}

int main(void)
{
    creation_failures();
    streaming_failures();
    block_failures();
    return 0;
}
