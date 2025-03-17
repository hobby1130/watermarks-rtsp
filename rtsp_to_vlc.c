/**
 * 从RTSP摄像头拉取视频流并推送给VLC播放器，并添加文字水印
 * 编译命令: gcc -o rtsp_to_vlc rtsp_to_vlc.c -lavformat -lavcodec -lavutil -lswscale -lavfilter
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>

// 修改为RTP协议
#define OUTPUT_UDP "rtp://10.68.128.67:12340"
// 水印文字
#define WATERMARK_TEXT "测试水印 - %{localtime}"

// 初始化滤镜
static int init_filters(AVFormatContext *fmt_ctx, int video_stream_idx, 
                        AVFilterContext **buffersrc_ctx, AVFilterContext **buffersink_ctx,
                        AVFilterGraph **filter_graph, const char *watermark_text) {
    char args[512];
    int ret = 0;
    AVFilterGraph *graph;
    AVFilterContext *src_ctx = NULL, *sink_ctx = NULL;
    AVStream *stream = fmt_ctx->streams[video_stream_idx];
    const AVCodecParameters *codecpar = stream->codecpar;
    
    // 创建滤镜图
    graph = avfilter_graph_alloc();
    if (!graph) {
        fprintf(stderr, "无法创建滤镜图\n");
        return AVERROR(ENOMEM);
    }
    
    // 创建源滤镜 (buffer)
    snprintf(args, sizeof(args),
             "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
             codecpar->width, codecpar->height, codecpar->format,
             stream->time_base.num, stream->time_base.den,
             codecpar->sample_aspect_ratio.num, codecpar->sample_aspect_ratio.den > 0 ? 
             codecpar->sample_aspect_ratio.den : 1);
    
    ret = avfilter_graph_create_filter(&src_ctx, avfilter_get_by_name("buffer"),
                                      "in", args, NULL, graph);
    if (ret < 0) {
        fprintf(stderr, "无法创建缓冲源滤镜\n");
        goto end;
    }
    
    // 创建接收器滤镜 (buffersink)
    ret = avfilter_graph_create_filter(&sink_ctx, avfilter_get_by_name("buffersink"),
                                      "out", NULL, NULL, graph);
    if (ret < 0) {
        fprintf(stderr, "无法创建缓冲接收器滤镜\n");
        goto end;
    }
    
    // 创建文字水印滤镜链
    // 使用drawtext滤镜添加水印
    // 本来x=10,水印在左上角，现在设置(w-text_w-10)，移到右上角
    char filter_descr[1024];
    snprintf(filter_descr, sizeof(filter_descr),
             "drawtext=text='%s':fontcolor=white:fontsize=20:x=(w-text_w-10):y=10:box=1:boxcolor=black@0.5",
             watermark_text);
    
    // 连接滤镜
    AVFilterInOut *outputs = avfilter_inout_alloc();
    AVFilterInOut *inputs  = avfilter_inout_alloc();
    if (!outputs || !inputs) {
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    outputs->name       = av_strdup("in");
    outputs->filter_ctx = src_ctx;
    outputs->pad_idx    = 0;
    outputs->next       = NULL;
    
    inputs->name        = av_strdup("out");
    inputs->filter_ctx  = sink_ctx;
    inputs->pad_idx     = 0;
    inputs->next        = NULL;
    
    ret = avfilter_graph_parse_ptr(graph, filter_descr, &inputs, &outputs, NULL);
    if (ret < 0) {
        fprintf(stderr, "无法解析滤镜图描述\n");
        goto end;
    }
    
    ret = avfilter_graph_config(graph, NULL);
    if (ret < 0) {
        fprintf(stderr, "无法配置滤镜图\n");
        goto end;
    }
    
    *buffersrc_ctx = src_ctx;
    *buffersink_ctx = sink_ctx;
    *filter_graph = graph;
    
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    
    return 0;
    
end:
    avfilter_inout_free(&inputs);
    avfilter_inout_free(&outputs);
    avfilter_graph_free(&filter_graph);
    return ret;
}

int main(int argc, char *argv[]) {
    AVFormatContext *input_format_ctx = NULL;
    AVFormatContext *output_format_ctx = NULL;
    AVPacket pkt;
    AVFrame *frame = NULL, *filt_frame = NULL;
    int ret, i;
    int video_stream_idx = -1;
    char *input_rtsp_url = NULL;
    int64_t start_time = 0;
    
    // 滤镜相关变量
    AVFilterContext *buffersrc_ctx = NULL;
    AVFilterContext *buffersink_ctx = NULL;
    AVFilterGraph *filter_graph = NULL;
    AVCodecContext *dec_ctx = NULL;
    
    // 检查命令行参数
    if (argc != 2) {
        fprintf(stderr, "用法: %s <rtsp_url>\n", argv[0]);
        fprintf(stderr, "例如: %s rtsp://admin:password@10.68.132.55:554/stream\n", argv[0]);
        return 1;
    }
    
    input_rtsp_url = argv[1];
    
    // 注册所有编解码器和格式
    #if LIBAVFORMAT_VERSION_INT < AV_VERSION_INT(58, 9, 100)
        av_register_all();
    #endif
    avformat_network_init();
    
    printf("正在连接到RTSP流: %s\n", input_rtsp_url);
    
    // 设置RTSP传输选项
    AVDictionary *opts = NULL;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0); // 使用TCP传输
    av_dict_set(&opts, "stimeout", "5000000", 0);   // 设置超时时间（微秒）
    
    // 打开输入RTSP流
    ret = avformat_open_input(&input_format_ctx, input_rtsp_url, NULL, &opts);
    if (ret < 0) {
        char errbuf[100];
        av_strerror(ret, errbuf, sizeof(errbuf));
        fprintf(stderr, "无法打开输入流: %s\n", errbuf);
        return 1;
    }
    
    // 获取流信息
    ret = avformat_find_stream_info(input_format_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "无法获取流信息\n");
        goto end;
    }
    
    // 查找视频流
    for (i = 0; i < input_format_ctx->nb_streams; i++) {
        if (input_format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }
    
    if (video_stream_idx == -1) {
        fprintf(stderr, "未找到视频流\n");
        goto end;
    }
    
    printf("找到视频流，索引: %d\n", video_stream_idx);
    
    // 获取解码器
    const AVCodec *dec = avcodec_find_decoder(input_format_ctx->streams[video_stream_idx]->codecpar->codec_id);
    if (!dec) {
        fprintf(stderr, "无法找到解码器\n");
        goto end;
    }
    
    // 分配解码器上下文
    dec_ctx = avcodec_alloc_context3(dec);
    if (!dec_ctx) {
        fprintf(stderr, "无法分配解码器上下文\n");
        goto end;
    }
    
    // 从输入流复制参数到解码器上下文
    ret = avcodec_parameters_to_context(dec_ctx, input_format_ctx->streams[video_stream_idx]->codecpar);
    if (ret < 0) {
        fprintf(stderr, "无法复制解码器参数\n");
        goto end;
    }
    
    // 打开解码器
    ret = avcodec_open2(dec_ctx, dec, NULL);
    if (ret < 0) {
        fprintf(stderr, "无法打开解码器\n");
        goto end;
    }
    
    // 创建输出格式上下文 - 修改为RTP格式
    avformat_alloc_output_context2(&output_format_ctx, NULL, "rtp", OUTPUT_UDP);
    if (!output_format_ctx) {
        fprintf(stderr, "无法创建输出上下文\n");
        goto end;
    }
    
    // 从输入流复制视频流到输出流
    AVStream *out_stream = avformat_new_stream(output_format_ctx, NULL);
    if (!out_stream) {
        fprintf(stderr, "无法创建输出流\n");
        goto end;
    }
    
    ret = avcodec_parameters_copy(out_stream->codecpar, input_format_ctx->streams[video_stream_idx]->codecpar);
    if (ret < 0) {
        fprintf(stderr, "无法复制编解码器参数\n");
        goto end;
    }
    
    // 设置输出流的时间基准
    out_stream->time_base = input_format_ctx->streams[video_stream_idx]->time_base;
    
    // 打印输出流信息
    printf("输出流: %s\n", OUTPUT_UDP);
    
    // 打开输出URL
    ret = avio_open(&output_format_ctx->pb, OUTPUT_UDP, AVIO_FLAG_WRITE);
    if (ret < 0) {
        fprintf(stderr, "无法打开输出URL\n");
        goto end;
    }
    
    // 写入流头
    ret = avformat_write_header(output_format_ctx, NULL);
    if (ret < 0) {
        fprintf(stderr, "写入头信息失败\n");
        goto end;
    }
    
    // 初始化滤镜
    ret = init_filters(input_format_ctx, video_stream_idx, &buffersrc_ctx, &buffersink_ctx, &filter_graph, WATERMARK_TEXT);
    if (ret < 0) {
        fprintf(stderr, "无法初始化滤镜\n");
        goto end;
    }
    
    // 分配帧
    frame = av_frame_alloc();
    filt_frame = av_frame_alloc();
    if (!frame || !filt_frame) {
        fprintf(stderr, "无法分配帧\n");
        ret = AVERROR(ENOMEM);
        goto end;
    }
    
    printf("开始转发带水印的视频流到VLC，请使用VLC打开 %s\n", OUTPUT_UDP);
    printf("VLC命令: vlc %s\n", OUTPUT_UDP);
    
    // 获取开始时间
    start_time = av_gettime();
    
    // 读取数据包并处理
    while (1) {
        ret = av_read_frame(input_format_ctx, &pkt);
        if (ret < 0)
            break;
            
        // 只处理视频流
        if (pkt.stream_index == video_stream_idx) {
            // 解码视频帧
            ret = avcodec_send_packet(dec_ctx, &pkt);
            if (ret < 0) {
                fprintf(stderr, "解码错误\n");
                break;
            }
            
            while (ret >= 0) {
                ret = avcodec_receive_frame(dec_ctx, frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    break;
                } else if (ret < 0) {
                    fprintf(stderr, "解码错误\n");
                    goto end;
                }
                
                // 将帧推送到滤镜
                ret = av_buffersrc_add_frame_flags(buffersrc_ctx, frame, AV_BUFFERSRC_FLAG_KEEP_REF);
                if (ret < 0) {
                    fprintf(stderr, "添加帧到滤镜错误\n");
                    break;
                }
                
                // 从滤镜获取处理后的帧
                while (1) {
                    ret = av_buffersink_get_frame(buffersink_ctx, filt_frame);
                    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF)
                        break;
                    if (ret < 0)
                        goto end;
                        
                    // 编码处理后的帧
                    AVPacket enc_pkt;
                    av_init_packet(&enc_pkt);
                    enc_pkt.data = NULL;
                    enc_pkt.size = 0;
                    
                    // 关键修改：不再复制原始包的数据，而是重新编码滤镜处理后的帧
                    static AVCodecContext *enc_ctx = NULL;
                    if (!enc_ctx) {
                        // 查找编码器
                        const AVCodec *encoder = avcodec_find_encoder_by_name("libx264");
                        if (!encoder) {
                            // 如果libx264不可用，尝试使用默认的H264编码器
                            encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
                        }
                        if (!encoder) {
                            fprintf(stderr, "无法找到编码器\n");
                            av_frame_unref(filt_frame);
                            break;
                        }
                        
                        // 创建编码器上下文
                        enc_ctx = avcodec_alloc_context3(encoder);
                        if (!enc_ctx) {
                            fprintf(stderr, "无法分配编码器上下文\n");
                            av_frame_unref(filt_frame);
                            break;
                        }
                        
                        // 复制解码器参数到编码器
                        enc_ctx->height = dec_ctx->height;
                        enc_ctx->width = dec_ctx->width;
                        enc_ctx->sample_aspect_ratio = dec_ctx->sample_aspect_ratio;
                        enc_ctx->pix_fmt = dec_ctx->pix_fmt;
                        enc_ctx->time_base = dec_ctx->time_base;
                        
                        // 设置编码参数
                        enc_ctx->bit_rate = 2000000;  // 2Mbps
                        enc_ctx->gop_size = 50;       // 设置较大的GOP大小，降低CPU使用率
                        enc_ctx->max_b_frames = 0;    // 禁用B帧，降低编码复杂度

                        // 设置低复杂度预设
                        AVDictionary *opts = NULL;
                        av_dict_set(&opts, "preset", "ultrafast", 0);  // 使用最快的预设
                        av_dict_set(&opts, "tune", "zerolatency", 0);  // 优化低延迟
                        av_dict_set(&opts, "threads", "auto", 0);      // 自动线程数
                        av_dict_set(&opts, "profile", "baseline", 0);  // 使用基本配置文件，兼容性更好
                        
                        // 设置x264特定参数，解决关键帧间隔问题
                        char x264_params[256];
                        snprintf(x264_params, sizeof(x264_params), 
                                "keyint=%d:min-keyint=%d:no-scenecut=1:force-cfr=1", 
                                enc_ctx->gop_size, enc_ctx->gop_size);
                        av_dict_set(&opts, "x264-params", x264_params, 0);
                        
                        // 打开编码器
                        ret = avcodec_open2(enc_ctx, encoder, &opts);
                        av_dict_free(&opts);
                        if (ret < 0) {
                            fprintf(stderr, "无法打开编码器\n");
                            avcodec_free_context(&enc_ctx);
                            av_frame_unref(filt_frame);
                            break;
                        }
                    }
                    
                    // 使用固定的时间基准来计算时间戳
                    static int64_t frame_count = 0;
                    
                    // 设置帧的时间戳
                    filt_frame->pts = frame_count;
                    
                    // 编码滤镜处理后的帧
                    ret = avcodec_send_frame(enc_ctx, filt_frame);
                    if (ret < 0) {
                        fprintf(stderr, "编码错误\n");
                        av_frame_unref(filt_frame);
                        break;
                    }
                    
                    while (ret >= 0) {
                        ret = avcodec_receive_packet(enc_ctx, &enc_pkt);
                        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                            break;
                        } else if (ret < 0) {
                            fprintf(stderr, "编码错误\n");
                            av_frame_unref(filt_frame);
                            break;
                        }
                        
                        // 设置时间戳
                        int64_t timestamp_increment = 3600; // 根据您的视频帧率调整这个值
                        enc_pkt.pts = frame_count * timestamp_increment;
                        enc_pkt.dts = enc_pkt.pts;
                        
                        // 确保流索引正确
                        enc_pkt.stream_index = 0;
                        enc_pkt.duration = timestamp_increment;
                        enc_pkt.pos = -1;
                        
                        // 打印调试信息
                        if (frame_count % 100 == 0) {
                            printf("处理帧 #%ld: pts=%ld dts=%ld 关键帧=%s\n", 
                                   frame_count, 
                                   enc_pkt.pts, 
                                   enc_pkt.dts, 
                                   (enc_pkt.flags & AV_PKT_FLAG_KEY) ? "是" : "否");
                        }
                        
                        // 写入数据包到输出
                        ret = av_interleaved_write_frame(output_format_ctx, &enc_pkt);
                        if (ret < 0) {
                            fprintf(stderr, "写入数据包错误: %d\n", ret);
                            av_packet_unref(&enc_pkt);
                            break;
                        }
                        
                        // 释放资源
                        av_packet_unref(&enc_pkt);
                    }
                    
                    frame_count++;
                    av_frame_unref(filt_frame);
                }
                
                av_frame_unref(frame);
            }
        }
        
        av_packet_unref(&pkt);
    }
    
    // 写入流尾
    av_write_trailer(output_format_ctx);
    
end:
    // 清理资源
    if (dec_ctx) {
        avcodec_free_context(&dec_ctx);
    }
    
    if (filter_graph) {
        avfilter_graph_free(&filter_graph);
    }
    
    av_frame_free(&frame);
    av_frame_free(&filt_frame);
    
    if (input_format_ctx) {
        avformat_close_input(&input_format_ctx);
    }
    
    if (output_format_ctx) {
        if (output_format_ctx->pb) {
            avio_closep(&output_format_ctx->pb);
        }
        avformat_free_context(output_format_ctx);
    }
    
    avformat_network_deinit();
    
    return 0;
}
