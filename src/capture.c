#include "awesomewm_screenlock_capture.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>

static int fail(int code, const char *message)
{
    char error[AV_ERROR_MAX_STRING_SIZE];

    av_strerror(code, error, sizeof(error));
    fprintf(stderr, "awesomewm-screenlock: %s: %s\n", message, error);
    return code;
}

int awesomewm_screenlock_capture(
    const char *display,
    const char *resolution,
    struct screenlock_capture *capture
)
{
    AVFormatContext *input = NULL;
    AVCodecContext *decoder = NULL;
    AVPacket *packet = NULL;
    AVFrame *decoded = NULL;
    AVFrame *filtered = NULL;
    AVFrame *rgb = NULL;
    AVFilterGraph *filter_graph = NULL;
    AVFilterContext *filter_source = NULL;
    AVFilterContext *filter_sink = NULL;
    AVFilterInOut *filter_inputs = NULL;
    AVFilterInOut *filter_outputs = NULL;
    struct SwsContext *scaler = NULL;
    AVDictionary *options = NULL;
    const AVInputFormat *input_format;
    const AVCodec *decoder_codec;
    int video_stream = -1;
    int result = AVERROR(EINVAL);
    int frame_received = 0;

    /*
     * Takes a screenshot, blurs it through downscaling, then pixelates it on
     * the way back up. This is the native equivalent of the filter from the
     * former bin/screenlock.sh:
     *
     *   noise=alls=10,scale=iw*.05:-1,scale=iw*20:-1:flags=neighbor
     *
     * The result is intentionally privacy-oriented: it leaves the desktop
     * recognizable while making the locked content difficult to inspect.
     * The lock boundary is implemented by this helper, with its focused
     * surface inspired by i3lock: https://github.com/i3/i3lock
     */
    const char *filter_description =
        "noise=alls=10,scale=iw*.05:-1,scale=iw*20:-1:flags=neighbor";

    if (capture == NULL)
        return AVERROR(EINVAL);
    memset(capture, 0, sizeof(*capture));

    avdevice_register_all();
    av_log_set_level(AV_LOG_ERROR);
    input_format = av_find_input_format("x11grab");
    if (input_format == NULL)
        return fail(AVERROR_DECODER_NOT_FOUND, "x11grab input is unavailable");

    /*
     * This is a one-frame capture, not a media session. Keep x11grab from
     * waiting for a large probe window and avoid spending time collecting
     * cursor data that the lock surface will never display.
     */
    av_dict_set(&options, "video_size", resolution, 0);
    av_dict_set(&options, "framerate", "1", 0);
    av_dict_set(&options, "draw_mouse", "0", 0);
    av_dict_set(&options, "probesize", "32", 0);
    av_dict_set(&options, "analyzeduration", "0", 0);
    result = avformat_open_input(&input, display, input_format, &options);
    av_dict_free(&options);
    if (result < 0)
        goto cleanup;
    /* x11grab creates its video stream during avformat_open_input(). */
    if (input->nb_streams == 0) {
        result = AVERROR_STREAM_NOT_FOUND;
        goto cleanup;
    }

    for (unsigned int index = 0; index < input->nb_streams; index++) {
        if (input->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream = (int)index;
            break;
        }
    }
    if (video_stream < 0) {
        result = AVERROR_STREAM_NOT_FOUND;
        goto cleanup;
    }

    decoder_codec = avcodec_find_decoder(input->streams[video_stream]->codecpar->codec_id);
    if (decoder_codec == NULL) {
        result = AVERROR_DECODER_NOT_FOUND;
        goto cleanup;
    }
    decoder = avcodec_alloc_context3(decoder_codec);
    if (decoder == NULL) {
        result = AVERROR(ENOMEM);
        goto cleanup;
    }
    result = avcodec_parameters_to_context(
        decoder, input->streams[video_stream]->codecpar
    );
    if (result < 0)
        goto cleanup;
    result = avcodec_open2(decoder, decoder_codec, NULL);
    if (result < 0)
        goto cleanup;

    packet = av_packet_alloc();
    decoded = av_frame_alloc();
    filtered = av_frame_alloc();
    rgb = av_frame_alloc();
    if (packet == NULL || decoded == NULL || filtered == NULL || rgb == NULL) {
        result = AVERROR(ENOMEM);
        goto cleanup;
    }

    filter_graph = avfilter_graph_alloc();
    if (filter_graph == NULL) {
        result = AVERROR(ENOMEM);
        goto cleanup;
    }
    {
        char arguments[256];

        snprintf(
            arguments, sizeof(arguments),
            "video_size=%dx%d:pix_fmt=%d:time_base=1/1:pixel_aspect=1/1",
            decoder->width, decoder->height, decoder->pix_fmt
        );
        result = avfilter_graph_create_filter(
            &filter_source, avfilter_get_by_name("buffer"), "in",
            arguments, NULL, filter_graph
        );
        if (result < 0)
            goto cleanup;
    }
    result = avfilter_graph_create_filter(
        &filter_sink, avfilter_get_by_name("buffersink"), "out",
        NULL, NULL, filter_graph
    );
    if (result < 0)
        goto cleanup;

    filter_inputs = avfilter_inout_alloc();
    filter_outputs = avfilter_inout_alloc();
    if (filter_inputs == NULL || filter_outputs == NULL) {
        result = AVERROR(ENOMEM);
        goto cleanup;
    }
    filter_outputs->name = av_strdup("in");
    filter_outputs->filter_ctx = filter_source;
    filter_outputs->pad_idx = 0;
    filter_inputs->name = av_strdup("out");
    filter_inputs->filter_ctx = filter_sink;
    filter_inputs->pad_idx = 0;
    result = avfilter_graph_parse_ptr(
        filter_graph, filter_description, &filter_inputs, &filter_outputs, NULL
    );
    if (result < 0)
        goto cleanup;
    result = avfilter_graph_config(filter_graph, NULL);
    if (result < 0)
        goto cleanup;

    /*
     * Decode only until the first filtered frame is available. The filter
     * graph performs the privacy transformation before the image crosses
     * into the lock renderer, so the unfiltered desktop is never displayed.
     */
    while (!frame_received && (result = av_read_frame(input, packet)) >= 0) {
        if (packet->stream_index == video_stream) {
            result = avcodec_send_packet(decoder, packet);
            if (result >= 0)
                result = avcodec_receive_frame(decoder, decoded);
            if (result >= 0) {
                result = av_buffersrc_add_frame_flags(
                    filter_source, decoded, AV_BUFFERSRC_FLAG_KEEP_REF
                );
                if (result >= 0)
                    result = av_buffersink_get_frame(filter_sink, filtered);
                if (result >= 0) {
                    rgb->format = AV_PIX_FMT_RGB24;
                    rgb->width = filtered->width;
                    rgb->height = filtered->height;
                    result = av_frame_get_buffer(rgb, 1);
                }
                if (result >= 0) {
                    scaler = sws_getContext(
                        filtered->width, filtered->height, filtered->format,
                        filtered->width, filtered->height, AV_PIX_FMT_RGB24,
                        SWS_POINT, NULL, NULL, NULL
                    );
                    if (scaler == NULL)
                        result = AVERROR(EINVAL);
                }
                if (result >= 0) {
                    sws_scale(
                        scaler,
                        (const uint8_t *const *)filtered->data,
                        filtered->linesize,
                        0,
                        filtered->height,
                        rgb->data,
                        rgb->linesize
                    );
                    capture->width = rgb->width;
                    capture->height = rgb->height;
                    capture->stride = rgb->linesize[0];
                        /* Transfer a compact, private copy out of FFmpeg. */
                        capture->pixels = malloc(
                        (size_t)capture->stride * (size_t)capture->height
                    );
                    if (capture->pixels == NULL)
                        result = AVERROR(ENOMEM);
                    else {
                        for (int row = 0; row < capture->height; row++)
                            memcpy(
                                capture->pixels + row * capture->stride,
                                rgb->data[0] + row * rgb->linesize[0],
                                (size_t)capture->stride
                            );
                        frame_received = 1;
                    }
                }
            }
        }
        av_packet_unref(packet);
        if (result == AVERROR(EAGAIN))
            result = 0;
    }
    if (!frame_received && result >= 0)
        result = AVERROR(EIO);

cleanup:
    if (result < 0) {
        awesomewm_screenlock_capture_free(capture);
        fail(result, "screen capture failed");
    }
    sws_freeContext(scaler);
    avfilter_inout_free(&filter_inputs);
    avfilter_inout_free(&filter_outputs);
    avfilter_graph_free(&filter_graph);
    av_frame_free(&rgb);
    av_frame_free(&filtered);
    av_frame_free(&decoded);
    av_packet_free(&packet);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    return result;
}

void awesomewm_screenlock_capture_free(struct screenlock_capture *capture)
{
    if (capture == NULL)
        return;
    free(capture->pixels);
    memset(capture, 0, sizeof(*capture));
}
