//
// Created by xu fulong on 2022/7/11.
//

#include <jni.h>

#ifdef __cplusplus
extern "C" {
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#ifdef __cplusplus
}
#endif

#include "ffmpeg_jni_define.h"

#define ALOGE(Format, ...) LOGE("Transcode", Format, ##__VA_ARGS__)

static AVFormatContext *ifmt_ctx;
static AVFormatContext *ofmt_ctx;
typedef struct FilteringContext {
    AVFilterContext *buffersink_ctx;
    AVFilterContext *buffersrc_ctx;
    AVFilterGraph *filter_graph;
    AVPacket *enc_pkt;
    AVFrame *filter_frame;
} FilteringContext;
static FilteringContext *filter_ctx;

typedef struct StreamContext {
    AVCodecContext *dec_ctx;
    AVCodecContext *enc_ctx;
    AVFrame *dec_frame;
} StreamContext;
static StreamContext *stream_ctx;

static int open_input_file(const char *filename)
{
    int ret;
    ifmt_ctx = nullptr;

    if ((ret = avformat_open_input(&ifmt_ctx, filename, nullptr, nullptr)) < 0) {
        ALOGE("Cannot open input file\n");
        return ret;
    }

    if ((ret = avformat_find_stream_info(ifmt_ctx, nullptr)) < 0) {
        ALOGE("Cannot find stream information\n");
        return ret;
    }

    stream_ctx = static_cast<StreamContext *>(av_mallocz_array(ifmt_ctx->nb_streams,
                                                               sizeof(*stream_ctx)));
    if (!stream_ctx)
        return AVERROR(ENOMEM);

    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        AVStream *stream = ifmt_ctx->streams[i];
        const AVCodec *dec = avcodec_find_decoder(stream->codecpar->codec_id);
        AVCodecContext *codec_ctx;
        if (!dec) {
            ALOGE("Failed to find decoder for stream #%u\n", i);
            return AVERROR_DECODER_NOT_FOUND;
        }
        codec_ctx = avcodec_alloc_context3(dec);
        if (!codec_ctx) {
            ALOGE("Failed to allocate the decoder context for stream #%u\n", i);
            return AVERROR(ENOMEM);
        }
        ret = avcodec_parameters_to_context(codec_ctx, stream->codecpar);
        if (ret < 0) {
            ALOGE("Failed to copy decoder parameters to input decoder context "
                                       "for stream #%u\n", i);
            return ret;
        }
        /* encode video & audio and remux subtitles etc. */
        if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
            || codec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            if (codec_ctx->codec_type == AVMEDIA_TYPE_VIDEO)
                codec_ctx->framerate = av_guess_frame_rate(ifmt_ctx, stream, nullptr);
            /* Open decoder */
            ret = avcodec_open2(codec_ctx, dec, nullptr);
            if (ret < 0) {
                ALOGE("Failed to open decoder for stream #%u\n", i);
                return ret;
            }
        }
        stream_ctx[i].dec_ctx = codec_ctx;
        stream_ctx[i].dec_frame = av_frame_alloc();
    }

    av_dump_format(ifmt_ctx, 0, filename, 0);
    return 0;
}

static int open_output_file(const char *filename)
{
    AVStream *out_stream;
    AVStream *in_stream;
    AVCodecContext *dec_ctx, *enc_ctx;
    const AVCodec *encoder;
    int ret;

    ofmt_ctx = nullptr;
    avformat_alloc_output_context2(&ofmt_ctx, nullptr, nullptr, filename);
    if (!ofmt_ctx) {
        ALOGE("Could not create output context\n");
        return AVERROR_UNKNOWN;
    }

    for (int i = 0; i < ifmt_ctx->nb_streams; i++) {
        out_stream = avformat_new_stream(ofmt_ctx, nullptr);
        if (!out_stream) {
            ALOGE("Failed allocating output stream\n");
            return AVERROR_UNKNOWN;
        }

        in_stream = ifmt_ctx->streams[i];
        dec_ctx = stream_ctx[i].dec_ctx;

        if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO
            || dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
            /* in this example, we choose transcoding to same codec */
            encoder = avcodec_find_encoder(dec_ctx->codec_id);
            if (!encoder) {
                ALOGE("Necessary encoder not found\n");
                return AVERROR_INVALIDDATA;
            }
            enc_ctx = avcodec_alloc_context3(encoder);
            if (!enc_ctx) {
                ALOGE("Failed to allocate the encoder context\n");
                return AVERROR(ENOMEM);
            }

            /* we transcode to same properties (picture size, sample rate etc.).
             * These properties can be changed for output streams using filters */
            if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
                enc_ctx->height = dec_ctx->height;
                enc_ctx->width = dec_ctx->width;
                enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                /* take first format from list of supported formats */
                if (encoder->pix_fmts)
                    enc_ctx->pix_fmt = encoder->pix_fmts[0];
                else
                    enc_ctx->pix_fmt = dec_ctx->pix_fmt;
                /* video time_base can be set to whatever is handy and supported by encoder */
//                enc_ctx->time_base = av_inv_q(dec_ctx->framerate);
                enc_ctx->time_base = dec_ctx->time_base;
            } else {
                enc_ctx->sample_rate = dec_ctx->sample_rate;
                enc_ctx->channel_layout = dec_ctx->channel_layout;
                enc_ctx->channels = av_get_channel_layout_nb_channels(enc_ctx->channel_layout);
                /* take first format from list of supported formats */
                enc_ctx->sample_fmt = encoder->sample_fmts[0];
                enc_ctx->time_base = (AVRational){1, enc_ctx->sample_rate};
            }

            if (ofmt_ctx->oformat->flags & AVFMT_GLOBALHEADER)
                enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

            /* Third parameter can be used to pass settings to encoder */
            ret = avcodec_open2(enc_ctx, encoder, nullptr);
            if (ret < 0) {
                ALOGE("Cannot open video encoder for stream #%u\n", i);
                return ret;
            }
            ret = avcodec_parameters_from_context(out_stream->codecpar, enc_ctx);
            if (ret < 0) {
                ALOGE("Failed to copy encoder parameters to output stream #%u\n", i);
                return ret;
            }

            out_stream->time_base = enc_ctx->time_base;
            stream_ctx[i].enc_ctx = enc_ctx;
        } else if (dec_ctx->codec_type == AVMEDIA_TYPE_UNKNOWN) {
            ALOGE("Elementary stream #%d is of unknown type, cannot proceed\n", i);
            return AVERROR_INVALIDDATA;
        } else {
            ret = avcodec_parameters_copy(out_stream->codecpar, in_stream->codecpar);
            if (ret < 0) {
                ALOGE("Copying parameters for stream #%u failed\n", i);
                return ret;
            }
            out_stream->time_base = in_stream->time_base;
        }

    }
    av_dump_format(ofmt_ctx, 0, filename, 1);

    if (!(ofmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open(&ofmt_ctx->pb, filename, AVIO_FLAG_WRITE);
        if (ret < 0) {
            ALOGE("Could not open output file '%s'", filename);
            return ret;
        }
    }

    /* init muxer, write output file header */
    ret = avformat_write_header(ofmt_ctx, nullptr);
    if (ret < 0) {
        ALOGE("Error occurred when opening output file\n");
        return ret;
    }

    return 0;
}

static int init_filter(FilteringContext* fctx, AVCodecContext *dec_ctx,
                       AVCodecContext *enc_ctx, const char *filter_spec)
{
    char args[512];
    int ret;
    const AVFilter *buffersrc;
    const AVFilter *buffersink;
    AVFilterContext *buffersrc_ctx = nullptr;
    AVFilterContext *buffersink_ctx = nullptr;
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    AVFilterGraph *filter_graph = avfilter_graph_alloc();

    if (!outputs || !inputs || !filter_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if (dec_ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
        buffersrc = avfilter_get_by_name("buffer");
        buffersink = avfilter_get_by_name("buffersink");
        if (!buffersrc || !buffersink) {
            ALOGE("filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        snprintf(args, sizeof(args),
                 "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
                 dec_ctx->width, dec_ctx->height, dec_ctx->pix_fmt,
                 dec_ctx->time_base.num, dec_ctx->time_base.den,
                 dec_ctx->sample_aspect_ratio.num,
                 dec_ctx->sample_aspect_ratio.den);

        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                           args, nullptr, filter_graph);
        if (ret < 0) {
            ALOGE("Cannot create buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                           nullptr, nullptr, filter_graph);
        if (ret < 0) {
            ALOGE("Cannot create buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "pix_fmts",
                             (uint8_t*)&enc_ctx->pix_fmt, sizeof(enc_ctx->pix_fmt),
                             AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            ALOGE( "Cannot set output pixel format\n");
            goto end;
        }
    } else if (dec_ctx->codec_type == AVMEDIA_TYPE_AUDIO) {
        buffersrc = avfilter_get_by_name("abuffer");
        buffersink = avfilter_get_by_name("abuffersink");
        if (!buffersrc || !buffersink) {
            ALOGE( "filtering source or sink element not found\n");
            ret = AVERROR_UNKNOWN;
            goto end;
        }

        if (!dec_ctx->channel_layout)
            dec_ctx->channel_layout =
                    av_get_default_channel_layout(dec_ctx->channels);
        snprintf(args, sizeof(args),
                 "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
                 dec_ctx->time_base.num, dec_ctx->time_base.den, dec_ctx->sample_rate,
                 av_get_sample_fmt_name(dec_ctx->sample_fmt),
                 dec_ctx->channel_layout);
        ret = avfilter_graph_create_filter(&buffersrc_ctx, buffersrc, "in",
                                           args, nullptr, filter_graph);
        if (ret < 0) {
            ALOGE("Cannot create audio buffer source\n");
            goto end;
        }

        ret = avfilter_graph_create_filter(&buffersink_ctx, buffersink, "out",
                                           nullptr, nullptr, filter_graph);
        if (ret < 0) {
            ALOGE("Cannot create audio buffer sink\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_fmts",
                             (uint8_t*)&enc_ctx->sample_fmt, sizeof(enc_ctx->sample_fmt),
                             AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            ALOGE("Cannot set output sample format\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "channel_layouts",
                             (uint8_t*)&enc_ctx->channel_layout,
                             sizeof(enc_ctx->channel_layout), AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            ALOGE("Cannot set output channel layout\n");
            goto end;
        }

        ret = av_opt_set_bin(buffersink_ctx, "sample_rates",
                             (uint8_t*)&enc_ctx->sample_rate, sizeof(enc_ctx->sample_rate),
                             AV_OPT_SEARCH_CHILDREN);
        if (ret < 0) {
            ALOGE("Cannot set output sample rate\n");
            goto end;
        }
    } else {
        ret = AVERROR_UNKNOWN;
        goto end;
    }

    /* Endpoints for the filter graph. */
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = buffersrc_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = nullptr;

    inputs->name       = av_strdup("out");
    inputs->filter_ctx = buffersink_ctx;
    inputs->pad_idx    = 0;
    inputs->next       = nullptr;

    if (!outputs->name || !inputs->name) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    if ((ret = avfilter_graph_parse_ptr(filter_graph, filter_spec,
                                        &inputs, &outputs, nullptr)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(filter_graph, nullptr)) < 0)
        goto end;

    /* Fill FilteringContext */
    fctx->filter_graph = filter_graph;
    fctx->buffersrc_ctx = buffersrc_ctx;
    fctx->buffersink_ctx = buffersink_ctx;

end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);

    return ret;
}

static int init_filters()
{
    const char *filter_spec;
    unsigned int i;
    int ret;
    filter_ctx = static_cast<FilteringContext *>(av_malloc_array(ifmt_ctx->nb_streams,
                                                                 sizeof(*filter_ctx)));
    if (!filter_ctx)
        return AVERROR(ENOMEM);

    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        filter_ctx[i].buffersrc_ctx  = nullptr;
        filter_ctx[i].buffersink_ctx = nullptr;
        filter_ctx[i].filter_graph   = nullptr;
        if (!(ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO
              || ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO))
            continue;

        if (ifmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
            filter_spec = "null";
        else
            filter_spec = "anull";
        ret = init_filter(&filter_ctx[i], stream_ctx[i].dec_ctx,
                          stream_ctx[i].enc_ctx, filter_spec);
        if (ret)
            return ret;

        filter_ctx[i].enc_pkt = av_packet_alloc();
        filter_ctx[i].filter_frame = av_frame_alloc();
    }
    return 0;
}

static int encode_write_frame(int stream_index, int flush) {
    int ret;
    StreamContext *stream = &stream_ctx[stream_index];
    AVPacket *enc_pkt = filter_ctx[stream_index].enc_pkt;
    av_packet_unref(enc_pkt);
    AVFrame *filter_frame = flush ? nullptr : filter_ctx[stream_index].filter_frame;

    ret = avcodec_send_frame(stream->enc_ctx, filter_frame);

    if (ret < 0)
        return ret;

    while (ret >= 0) {
        ret = avcodec_receive_packet(stream->enc_ctx, enc_pkt);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            return 0;
        }

        enc_pkt->stream_index = stream_index;
        av_packet_rescale_ts(enc_pkt, stream->enc_ctx->time_base,
                             ofmt_ctx->streams[stream_index]->time_base);

        /* mux encoded frame */
        ret = av_interleaved_write_frame(ofmt_ctx, enc_pkt);
    }
    return ret;
}

static int filter_encode_write_frame(AVFrame *frame, int stream_index)
{
    int ret;
    FilteringContext *filter_contex = &filter_ctx[stream_index];

    /* push the decoded frame into the filtergraph */
    ret = av_buffersrc_add_frame_flags(filter_contex->buffersrc_ctx, frame, 0);
    if (ret < 0) {
        ALOGE("Error while feeding the filtergraph\n");
        return ret;
    }

    while (1) {
        ret = av_buffersink_get_frame(filter_contex->buffersink_ctx, filter_contex->filter_frame);
        if (ret < 0) {
            if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                ret = 0;
            break;
        }

        filter_contex->filter_frame->pict_type = AV_PICTURE_TYPE_NONE;
        ret = encode_write_frame(stream_index, 0);
        av_frame_unref(filter_contex->filter_frame);
        if (ret < 0)
            break;
    }

    return ret;
}

static int flush_encoder(int stream_index)
{
    if (!(stream_ctx[stream_index].enc_ctx->codec->capabilities &
          AV_CODEC_CAP_DELAY))
        return 0;

    return encode_write_frame(stream_index, 1);
}

int transcode(const char *input_file, const char *output_file)
{
    int ret;
    AVPacket *packet = nullptr;
    int stream_index;
    int i;

    if ((ret = open_input_file(input_file)) < 0)
        goto end;
    if ((ret = open_output_file(output_file)) < 0)
        goto end;
    if ((ret = init_filters()) < 0)
        goto end;
    if (!(packet = av_packet_alloc()))
        goto end;

    /* read all packets */
    while (1) {
        if ((ret = av_read_frame(ifmt_ctx, packet)) < 0)
            break;
        stream_index = packet->stream_index;

        if (filter_ctx && filter_ctx[stream_index].filter_graph) {
            StreamContext *stream = &stream_ctx[stream_index];
            av_packet_rescale_ts(packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 stream_ctx[stream_index].dec_ctx->time_base);
            ret = avcodec_send_packet(stream->dec_ctx, packet);
            if (ret < 0) {
                ALOGE("Decoding failed\n");
                break;
            }

            while (ret >= 0) {
                ret = avcodec_receive_frame(stream->dec_ctx, stream->dec_frame);
                if (ret == AVERROR_EOF || ret == AVERROR(EAGAIN))
                    break;
                else if (ret < 0)
                    goto end;

                stream->dec_frame->pts = stream->dec_frame->best_effort_timestamp;
                ret = filter_encode_write_frame(stream->dec_frame, stream_index);
                if (ret < 0)
                    goto end;
            }
        } else {
            /* remux this frame without re-encoding */
            av_packet_rescale_ts(packet,
                                 ifmt_ctx->streams[stream_index]->time_base,
                                 ofmt_ctx->streams[stream_index]->time_base);

            ret = av_interleaved_write_frame(ofmt_ctx, packet);
            if (ret < 0)
                goto end;
        }
        av_packet_unref(packet);
    }

    /* flush filters and encoders */
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        if (!filter_ctx || !filter_ctx[i].filter_graph)
            continue;
        ret = filter_encode_write_frame(nullptr, i);
        if (ret < 0) {
            ALOGE("Flushing filter failed\n");
            goto end;
        }

        ret = flush_encoder(i);
        if (ret < 0) {
            ALOGE("Flushing encoder failed\n");
            goto end;
        }
    }

    av_write_trailer(ofmt_ctx);
end:
    av_packet_unref(packet);
    for (i = 0; i < ifmt_ctx->nb_streams; i++) {
        avcodec_free_context(&stream_ctx[i].dec_ctx);
        if (ofmt_ctx && ofmt_ctx->nb_streams > i && ofmt_ctx->streams[i] && stream_ctx[i].enc_ctx)
            avcodec_free_context(&stream_ctx[i].enc_ctx);
        if (filter_ctx && filter_ctx[i].filter_graph) {
            avfilter_graph_free(&filter_ctx[i].filter_graph);
            av_packet_free(&filter_ctx[i].enc_pkt);
            av_frame_free(&filter_ctx[i].filter_frame);
        }
        av_frame_free(&stream_ctx[i].dec_frame);
    }
    if (filter_ctx)
        av_free(filter_ctx);
    av_free(stream_ctx);
    avformat_close_input(&ifmt_ctx);
    if (ofmt_ctx && !(ofmt_ctx->oformat->flags & AVFMT_NOFILE))
        avio_closep(&ofmt_ctx->pb);
    avformat_free_context(ofmt_ctx);

    if (ret < 0)
        ALOGE("Error occurred: %s\n", av_err2str(ret));

    return ret;
}

#ifdef __cplusplus
extern "C" {
#endif
VIDEO_PLAYER_FUNC(int, executeTranscode, jstring inputFile, jstring outputFile) {
    const char *input_file = env->GetStringUTFChars(inputFile, JNI_FALSE);
    const char *output_file = env->GetStringUTFChars(outputFile, JNI_FALSE);
    int ret = transcode(input_file, output_file);
    env->ReleaseStringUTFChars(inputFile, input_file);
    env->ReleaseStringUTFChars(outputFile, output_file);
    return ret;
}
#ifdef __cplusplus
}
#endif
