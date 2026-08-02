#include "awesomewm_screenlock_capture.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>

struct capture_context {
    AVFormatContext *input;
    AVCodecContext *decoder;
    AVPacket *packet;
    AVFrame *decoded;
    AVFrame *filtered;
    AVFrame *rgb;
    AVFilterGraph *filter_graph;
    AVFilterContext *filter_source;
    AVFilterContext *filter_sink;
    AVFilterInOut *filter_inputs;
    AVFilterInOut *filter_outputs;
    struct SwsContext *scaler;
    int video_stream;
};

/*
 * Report an FFmpeg error with the operation that failed. Keeping conversion
 * here gives all failure paths the same diagnostic format without changing
 * the error code returned to the caller.
 */
static int capture_report_error(int code, const char *operation)
{
    char error[AV_ERROR_MAX_STRING_SIZE];

    av_strerror(code, error, sizeof(error));
    fprintf(stderr, "awesomewm-screenlock: %s: %s\n", operation, error);
    return code;
}

/*
 * Release every temporary object owned by a capture operation.
 *
 * Contract: every field may be NULL, and the context is left empty after the
 * call. Use this after both successful and failed operations. The release
 * order follows FFmpeg ownership: the scaler and filter graph are released
 * before the frames and codec/input contexts they reference.
 */
static void capture_context_cleanup(struct capture_context *context)
{
    sws_freeContext(context->scaler);
    avfilter_inout_free(&context->filter_inputs);
    avfilter_inout_free(&context->filter_outputs);
    avfilter_graph_free(&context->filter_graph);
    av_frame_free(&context->rgb);
    av_frame_free(&context->filtered);
    av_frame_free(&context->decoded);
    av_packet_free(&context->packet);
    avcodec_free_context(&context->decoder);
    avformat_close_input(&context->input);
    memset(context, 0, sizeof(*context));
    context->video_stream = -1;
}

/*
 * Free operation state and report a failed capture.
 *
 * Contract: `capture` must be a valid output object, while `context` owns all
 * FFmpeg resources acquired for the current operation. This helper is used
 * at every failure boundary so callers cannot forget a resource release.
 */
static int capture_failure(
    struct capture_context *context,
    struct screenlock_capture *capture,
    int result,
    const char *operation
)
{
    awesomewm_screenlock_capture_free(capture);
    capture_context_cleanup(context);
    return capture_report_error(result, operation);
}

/*
 * Open one x11grab input for a single filtered frame.
 *
 * Contract: `display` and `resolution` identify the X11 source, and `context`
 * receives the opened input. The caller owns cleanup after this function
 * returns, including when the function reports an error.
 */
static int capture_open_input(
    struct capture_context *context,
    const char *display,
    const char *resolution
)
{
    AVDictionary *options = NULL;
    const AVInputFormat *input_format = av_find_input_format("x11grab");
    int result;

    if (input_format == NULL)
        return AVERROR_DECODER_NOT_FOUND;

    av_dict_set(&options, "video_size", resolution, 0);
    av_dict_set(&options, "framerate", "1", 0);
    av_dict_set(&options, "draw_mouse", "0", 0);
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "0", 0);
    result = avformat_open_input(
        &context->input, display, input_format, &options
    );
    av_dict_free(&options);
    return result;
}

/*
 * Select the video stream produced by x11grab.
 *
 * Contract: the input must be open. The returned index is stored in the
 * context and is used for every subsequent packet read.
 */
static int capture_find_video_stream(struct capture_context *context)
{
    for (unsigned int index = 0; index < context->input->nb_streams; index++) {
        if (context->input->streams[index]->codecpar->codec_type
            == AVMEDIA_TYPE_VIDEO) {
            context->video_stream = (int)index;
            return 0;
        }
    }
    return AVERROR_STREAM_NOT_FOUND;
}

/*
 * Create the decoder and privacy filter graph for the selected stream.
 *
 * Contract: `context->video_stream` must identify a video stream. The graph
 * deliberately applies noise, downscaling, and nearest-neighbor upscaling
 * before the frame is copied to the lock renderer.
 */
static int capture_prepare_filter(struct capture_context *context)
{
    const AVCodec *decoder_codec;
    const char *filter_description =
        "noise=alls=10,scale=iw*.05:-1,scale=iw*20:-1:flags=neighbor";
    char arguments[256];
    int result;

    decoder_codec = avcodec_find_decoder(
        context->input->streams[context->video_stream]->codecpar->codec_id
    );
    if (decoder_codec == NULL)
        return AVERROR_DECODER_NOT_FOUND;

    context->decoder = avcodec_alloc_context3(decoder_codec);
    if (context->decoder == NULL)
        return AVERROR(ENOMEM);
    result = avcodec_parameters_to_context(
        context->decoder,
        context->input->streams[context->video_stream]->codecpar
    );
    if (result < 0)
        return result;
    result = avcodec_open2(context->decoder, decoder_codec, NULL);
    if (result < 0)
        return result;

    context->packet = av_packet_alloc();
    context->decoded = av_frame_alloc();
    context->filtered = av_frame_alloc();
    context->rgb = av_frame_alloc();
    if (context->packet == NULL || context->decoded == NULL
        || context->filtered == NULL || context->rgb == NULL)
        return AVERROR(ENOMEM);

    context->filter_graph = avfilter_graph_alloc();
    if (context->filter_graph == NULL)
        return AVERROR(ENOMEM);
    snprintf(
        arguments, sizeof(arguments),
        "video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=1/1",
        context->decoder->width, context->decoder->height,
        context->decoder->pix_fmt
    );
    result = avfilter_graph_create_filter(
        &context->filter_source, avfilter_get_by_name("buffer"), "in",
        arguments, NULL, context->filter_graph
    );
    if (result < 0)
        return result;
    result = avfilter_graph_create_filter(
        &context->filter_sink, avfilter_get_by_name("buffersink"), "out",
        NULL, NULL, context->filter_graph
    );
    if (result < 0)
        return result;

    context->filter_inputs = avfilter_inout_alloc();
    context->filter_outputs = avfilter_inout_alloc();
    if (context->filter_inputs == NULL || context->filter_outputs == NULL)
        return AVERROR(ENOMEM);
    context->filter_outputs->name = av_strdup("in");
    context->filter_outputs->filter_ctx = context->filter_source;
    context->filter_outputs->pad_idx = 0;
    context->filter_inputs->name = av_strdup("out");
    context->filter_inputs->filter_ctx = context->filter_sink;
    context->filter_inputs->pad_idx = 0;
    if (context->filter_inputs->name == NULL
        || context->filter_outputs->name == NULL)
        return AVERROR(ENOMEM);
    result = avfilter_graph_parse_ptr(
        context->filter_graph, filter_description,
        &context->filter_inputs, &context->filter_outputs, NULL
    );
    if (result < 0)
        return result;
    return avfilter_graph_config(context->filter_graph, NULL);
}

/*
 * Copy one filtered frame into the public RGB capture buffer.
 *
 * Contract: `context->filtered` must contain a frame. Ownership of the
 * returned pixels moves to `capture`, which the caller releases with
 * awesomewm_screenlock_capture_free().
 */
static int capture_copy_rgb(
    struct capture_context *context,
    struct screenlock_capture *capture
)
{
    int result;

    context->rgb->format = AV_PIX_FMT_RGB24;
    context->rgb->width = context->filtered->width;
    context->rgb->height = context->filtered->height;
    result = av_frame_get_buffer(context->rgb, 1);
    if (result < 0)
        return result;
    context->scaler = sws_getContext(
        context->filtered->width, context->filtered->height,
        context->filtered->format, context->filtered->width,
        context->filtered->height, AV_PIX_FMT_RGB24, SWS_POINT,
        NULL, NULL, NULL
    );
    if (context->scaler == NULL)
        return AVERROR(EINVAL);
    sws_scale(
        context->scaler, (const uint8_t *const *)context->filtered->data,
        context->filtered->linesize, 0, context->filtered->height,
        context->rgb->data, context->rgb->linesize
    );

    capture->width = context->rgb->width;
    capture->height = context->rgb->height;
    capture->stride = context->rgb->linesize[0];
    capture->pixels = malloc(
        (size_t)capture->stride * (size_t)capture->height
    );
    if (capture->pixels == NULL)
        return AVERROR(ENOMEM);
    for (int row = 0; row < capture->height; row++)
        memcpy(
            capture->pixels + row * capture->stride,
            context->rgb->data[0] + row * context->rgb->linesize[0],
            (size_t)capture->stride
        );
    return 0;
}

/*
 * Read packets until one frame has passed through the privacy filter.
 *
 * Contract: the decoder and filter graph must be prepared. The function
 * returns zero with a populated capture, or an FFmpeg error with no promise
 * about partial output (the caller always releases it through capture_failure
 * or awesomewm_screenlock_capture_free()).
 */
static int capture_read_frame(
    struct capture_context *context,
    struct screenlock_capture *capture
)
{
    int result;
    int frame_received = 0;

    while ((result = av_read_frame(context->input, context->packet)) >= 0) {
        if (context->packet->stream_index == context->video_stream) {
            result = avcodec_send_packet(
                context->decoder, context->packet
            );
            if (result >= 0)
                result = avcodec_receive_frame(
                    context->decoder, context->decoded
                );
            if (result >= 0)
                result = av_buffersrc_add_frame_flags(
                    context->filter_source, context->decoded,
                    AV_BUFFERSRC_FLAG_KEEP_REF
                );
            if (result >= 0)
                result = av_buffersink_get_frame(
                    context->filter_sink, context->filtered
                );
            if (result >= 0)
                result = capture_copy_rgb(context, capture);
            if (result == 0)
                frame_received = 1;
        }
        av_packet_unref(context->packet);
        if (frame_received)
            return 0;
        if (result < 0 && result != AVERROR(EAGAIN))
            return result;
    }
    return result < 0 ? result : AVERROR(EIO);
}

int awesomewm_screenlock_capture(
    const char *display,
    const char *resolution,
    struct screenlock_capture *capture
)
{
    struct capture_context context = { .video_stream = -1 };
    int result;

    if (capture == NULL)
        return AVERROR(EINVAL);
    memset(capture, 0, sizeof(*capture));

    avdevice_register_all();
    av_log_set_level(AV_LOG_ERROR);
    result = capture_open_input(&context, display, resolution);
    if (result < 0)
        return capture_failure(
            &context, capture, result, "x11grab input is unavailable"
        );
    if (context.input->nb_streams == 0)
        return capture_failure(
            &context, capture, AVERROR_STREAM_NOT_FOUND,
            "capture stream is unavailable"
        );
    result = capture_find_video_stream(&context);
    if (result < 0)
        return capture_failure(
            &context, capture, result, "video stream is unavailable"
        );
    result = capture_prepare_filter(&context);
    if (result < 0)
        return capture_failure(
            &context, capture, result, "privacy filter setup failed"
        );
    result = capture_read_frame(&context, capture);
    if (result < 0)
        return capture_failure(
            &context, capture, result, "screen capture failed"
        );
    capture_context_cleanup(&context);
    return 0;
}

/*
 * Release the public capture buffer and reset its metadata.
 *
 * Contract: NULL is accepted. The function is the only release operation
 * callers need after awesomewm_screenlock_capture() returns a frame.
 */
void awesomewm_screenlock_capture_free(struct screenlock_capture *capture)
{
    if (capture == NULL)
        return;
    free(capture->pixels);
    memset(capture, 0, sizeof(*capture));
}
