#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/opt.h>
#include <SDL2/SDL.h>


//compile cmd: sudo gcc player.c -lavutil -lavformat -lavcodec -lz -lavutil -lpthread -lm -lswscale -lavfilter -lswresample -lSDL2
//ffplay -f rawvideo -video_size 1280x720 0239.yuv

static int file_index = 0;
/* Minimum SDL audio buffer size, in samples. */
#define SDL_AUDIO_MIN_BUFFER_SIZE 512
/* Calculate actual buffer size keeping in mind not cause too frequent audio callbacks */
#define SDL_AUDIO_MAX_CALLBACKS_PER_SEC 30

#define NUM_FRAMES_RING_BUFFER   16
typedef struct VideoSurface{
    SDL_Window* window;
    SDL_Renderer* render;
    SDL_Texture* texture;
    int format;
    int x;
    int y;
    int width;
    int height;
}VideoSurface;

typedef struct AudioRender {

}AudioRender;

typedef struct SueFrame {
    AVFrame* frame;
}SueFrame;

typedef struct SueFrameRingQueue {
    SueFrame  sue_frames[NUM_FRAMES_RING_BUFFER];
    int pos_read;
    int pos_write;
    pthread_mutex_t ring_queue_lock;

    int last_operation;/* 0:read, 1:write */
    int abort;
}SueFrameRingQueue;

typedef struct SueAVPacket {
    AVPacket pkt;
    int serial;
    int64_t ts_enter_queue; //timestamp enter queue
    struct SueAVPacket* next;
}SueAVPacket;

typedef struct PacketQueue {
    SueAVPacket* first_pkt;
    SueAVPacket* last_pkt;
    int num_packets;
    int serial;
    int64_t duration;
    pthread_mutex_t queue_lock;

    int abort;
}PacketQueue;


typedef struct SueClock {
    int64_t timestamp_audio_real;
    int64_t timestamp_audio_stream;

    int64_t timestamp_video_real;
    int64_t timestamp_video_stream;

    int master_type;
    int timestamp_master_real;
    int timestamp_master_stream;
}SueClock;


typedef struct AVPlayer {
    VideoSurface video_surface;
    AudioRender *audio_render;

    AVFormatContext *context;
    pthread_mutex_t context_lock;
    AVCodecContext* vcodec_context;
    AVCodecContext* acodec_context;

    /* video info */
    int video_width;
    int video_height;

    int index_video_stream;
    int index_audio_stream;
    /* audio info */
    int audio_samplerate;
    int audio_channels;
    int audio_format;
    int64_t  audio_channel_layout;

    SDL_Thread* main_thread;
    SDL_Thread* video_refresh;
    SDL_Thread* audio_decoder_thread;
    SDL_Thread* video_decoder_thread;
    SDL_Thread* event_thread;

    AVFilterContext *src_audio_filter;
    AVFilterContext *sink_audio_filter;
    AVFilterGraph *audio_graph;

    PacketQueue video_pkts_queue;
    PacketQueue audio_pkts_queue;

    SueFrameRingQueue video_frames_queue;
    SueFrameRingQueue audio_frames_queue;

    int is_aplay_end;
    int pos_abuffer_read;
    int pos_abuffer_tail;
    AVFrame *aframe_playing;

    SueClock clock;

    int flag_exit;

    int index_src_track;
    int index_dst_track;
    int flag_select_track;
}AVPlayer;

static const struct TextureFormatEntry {
    enum AVPixelFormat format;
    int texture_fmt;
} sdl_texture_format_map[] = {
    { AV_PIX_FMT_RGB8,           SDL_PIXELFORMAT_RGB332 },
    { AV_PIX_FMT_RGB444,         SDL_PIXELFORMAT_RGB444 },
    { AV_PIX_FMT_RGB555,         SDL_PIXELFORMAT_RGB555 },
    { AV_PIX_FMT_BGR555,         SDL_PIXELFORMAT_BGR555 },
    { AV_PIX_FMT_RGB565,         SDL_PIXELFORMAT_RGB565 },
    { AV_PIX_FMT_BGR565,         SDL_PIXELFORMAT_BGR565 },
    { AV_PIX_FMT_RGB24,          SDL_PIXELFORMAT_RGB24 },
    { AV_PIX_FMT_BGR24,          SDL_PIXELFORMAT_BGR24 },
    { AV_PIX_FMT_0RGB32,         SDL_PIXELFORMAT_RGB888 },
    { AV_PIX_FMT_0BGR32,         SDL_PIXELFORMAT_BGR888 },
    { AV_PIX_FMT_NE(RGB0, 0BGR), SDL_PIXELFORMAT_RGBX8888 },
    { AV_PIX_FMT_NE(BGR0, 0RGB), SDL_PIXELFORMAT_BGRX8888 },
    { AV_PIX_FMT_RGB32,          SDL_PIXELFORMAT_ARGB8888 },
    { AV_PIX_FMT_RGB32_1,        SDL_PIXELFORMAT_RGBA8888 },
    { AV_PIX_FMT_BGR32,          SDL_PIXELFORMAT_ABGR8888 },
    { AV_PIX_FMT_BGR32_1,        SDL_PIXELFORMAT_BGRA8888 },
    { AV_PIX_FMT_YUV420P,        SDL_PIXELFORMAT_IYUV },
    { AV_PIX_FMT_YUYV422,        SDL_PIXELFORMAT_YUY2 },
    { AV_PIX_FMT_UYVY422,        SDL_PIXELFORMAT_UYVY },
    { AV_PIX_FMT_NONE,           SDL_PIXELFORMAT_UNKNOWN },
};

#define MAX_PACKETS_NUM    1024U
static AVPlayer player;
static SDL_AudioDeviceID audio_render;

static int streams_close();

static int frame_queue_init(SueFrameRingQueue* frame_queue) {
    frame_queue->pos_read = 0;
    frame_queue->pos_write = 0;
    frame_queue->last_operation = 0;
    frame_queue->abort = 0;
    pthread_mutex_init(&(frame_queue->ring_queue_lock), NULL);
    for (int i = 0; i < NUM_FRAMES_RING_BUFFER; i++) {
        if (!(frame_queue->sue_frames[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    }

    return 0;
}

static int frame_queue_destroy(SueFrameRingQueue* frame_queue) {
    for (int i = 0; i < NUM_FRAMES_RING_BUFFER; i++) {
        av_frame_unref(frame_queue->sue_frames[i].frame);
        av_frame_free(frame_queue->sue_frames[i].frame);
    }
    pthread_mutex_destroy(&(frame_queue->ring_queue_lock), NULL);
}

static int frame_queue_put(SueFrameRingQueue* frame_queue, AVFrame* frame) {
    int ret = -1;

    int write_space = 0;
    while (!frame_queue->abort) {
        pthread_mutex_lock(&(frame_queue->ring_queue_lock));
        if (frame_queue->pos_read == frame_queue->pos_write && frame_queue->last_operation == 0) {
            write_space = NUM_FRAMES_RING_BUFFER;
        } else {
            write_space = (frame_queue->pos_read - frame_queue->pos_write + NUM_FRAMES_RING_BUFFER) % (NUM_FRAMES_RING_BUFFER);
        }
        //av_log(NULL, AV_LOG_ERROR, "====write, pos_read:%d, pos_write:%d, last_operation:%d, write_space:%d\n",
        //         frame_queue->pos_read, frame_queue->pos_write, frame_queue->last_operation, write_space);
        if (write_space < 1) {
            pthread_mutex_unlock(&(frame_queue->ring_queue_lock));
            usleep(5);
            continue;
        }
        av_frame_unref(frame_queue->sue_frames[frame_queue->pos_write].frame);
        av_frame_move_ref(frame_queue->sue_frames[frame_queue->pos_write].frame, frame);

        if (frame_queue->pos_read == -1) {
            frame_queue->pos_read = 0;
        }

        int tmp_pos_write = frame_queue->pos_write;
        frame_queue->pos_write++;
        frame_queue->pos_write %= NUM_FRAMES_RING_BUFFER;
        frame_queue->last_operation = 1;
       //av_log(NULL, AV_LOG_ERROR, "====write frame(pts:%lld) success, write_pos:%d, next_write_pos:%d\n",
       //       frame_queue->sue_frames[tmp_pos_write].frame->pts, tmp_pos_write, frame_queue->pos_write);
        ret = 0;
        break;
    }

exit:
    pthread_mutex_unlock(&(frame_queue->ring_queue_lock));
    return ret;
}


static int frame_queue_get(SueFrameRingQueue* frame_queue, AVFrame* frame) {
    int ret = -1;
    int read_space;
    while (!frame_queue->abort) {
        ret = -1;
        pthread_mutex_lock(&(frame_queue->ring_queue_lock));
        if (frame_queue->pos_read == frame_queue->pos_write && frame_queue->last_operation == 1) {
            read_space = NUM_FRAMES_RING_BUFFER;
        } else {
            read_space = (frame_queue->pos_write - frame_queue->pos_read + NUM_FRAMES_RING_BUFFER) % (NUM_FRAMES_RING_BUFFER);
        }
        //av_log(NULL, AV_LOG_ERROR, "====read, pos_write:%d, pos_read:%d, read_space:%d, abort:%d\n", frame_queue->pos_write, frame_queue->pos_read, read_space, frame_queue->abort);
        if (read_space < 1) {
            pthread_mutex_unlock(&(frame_queue->ring_queue_lock));
            usleep(5 * 1000);
            continue;
        }
        av_frame_unref(frame);
        av_frame_move_ref(frame, frame_queue->sue_frames[frame_queue->pos_read].frame);

        int temp_pos_read = frame_queue->pos_read;
        frame_queue->pos_read++;
        frame_queue->pos_read %= NUM_FRAMES_RING_BUFFER;
        frame_queue->last_operation = 0;
        //av_log(NULL, AV_LOG_ERROR, "====read frame(pts:%lld) success, read_pos:%d, next_read_pos:%d\n", frame->pts, temp_pos_read, frame_queue->pos_read);
        ret = 0;
        break;
    }
exit:
    pthread_mutex_unlock(&(frame_queue->ring_queue_lock));
    //av_log(NULL, AV_LOG_ERROR, "%s exit, ret:%d\n", __func__, ret);
    return ret;
}
static int packet_queue_init(PacketQueue *queue) {
    queue->first_pkt = NULL;
    queue->last_pkt = NULL;
    queue->num_packets = 0;
    queue->serial = 0;
    queue->duration = 0;
    queue->abort = 0;

    pthread_mutex_init(&(queue->queue_lock), NULL);
}

static int packet_queue_put(PacketQueue *queue, AVPacket *pkt) {
    int ret = -1;
    while (!queue->abort) {
        pthread_mutex_lock(&(queue->queue_lock));
        if (queue->num_packets > MAX_PACKETS_NUM) {
            pthread_mutex_unlock(&(queue->queue_lock));
            usleep(5 * 1000);
            continue;
        }
        SueAVPacket *sue_pkt = av_malloc(sizeof(SueAVPacket));
        if (sue_pkt == NULL) {
            pthread_mutex_unlock(&(queue->queue_lock));
            av_log(NULL, AV_LOG_ERROR, "no memory, queue pkt(stream:%d, pts:%lld) failed\n", pkt->stream_index, pkt->pts);
            return -1;
        }
        sue_pkt->pkt = *pkt;
        sue_pkt->serial = queue->serial;
        sue_pkt->next = NULL;
        if (queue->last_pkt) {
           queue->last_pkt->next = sue_pkt;
        }
        queue->last_pkt = sue_pkt;
        queue->num_packets++;
        if (queue->first_pkt == NULL) {
            queue->first_pkt = queue->last_pkt;
        }
        //av_log(NULL, AV_LOG_ERROR, "queue pkt(stream:%d, pts:%lld) success, pkt_num:%d\n", pkt->stream_index, pkt->pts, queue->num_packets);
        ret = 0;
        break;
    }
    pthread_mutex_unlock(&(queue->queue_lock));

    return ret;
}

static int packet_queue_get(PacketQueue *queue, AVPacket* pkt) {
    int ret = -1;
    while (!queue->abort) {
        pthread_mutex_lock(&(queue->queue_lock));
        if(queue->first_pkt) {
            SueAVPacket *sue_pkt = queue->first_pkt;
            *pkt = sue_pkt->pkt;
            queue->first_pkt = sue_pkt->next;
            queue->num_packets--;
            if (queue->first_pkt == NULL) {
                queue->last_pkt = NULL;
            }
            //av_log(NULL, AV_LOG_ERROR, "dequeue pkt(stream:%d, pts:%lld) success, pkt_num:%d\n", pkt->stream_index, pkt->pts, queue->num_packets);
            av_free(sue_pkt);
            ret = 0;
            break;
        } else {
            pthread_mutex_unlock(&(queue->queue_lock));
            usleep(5 * 1000);
            continue;
        }
    }
    pthread_mutex_unlock(&(queue->queue_lock));
    return ret;
}

static void packet_queue_flush(PacketQueue *queue)
{
    SueAVPacket *pkt, *pkt1;
    pthread_mutex_lock(&(queue->queue_lock));
    for (pkt = queue->first_pkt; pkt; pkt = pkt1) {
        pkt1 = pkt->next;
        av_packet_unref(&pkt->pkt);
        av_freep(&pkt);
    }
    queue->last_pkt = NULL;
    queue->first_pkt = NULL;
    queue->num_packets = 0;
    pthread_mutex_unlock(&(queue->queue_lock));
}

static int packet_queue_destroy(PacketQueue *queue) {
    packet_queue_flush(queue);
    pthread_mutex_destroy(&(queue->queue_lock));
}

int dump_frame(AVFrame* frame)
{
    char file_path[256] = {0};
    FILE* pFile;
    sprintf(file_path, "./%04d.yuv", file_index++);

    pFile=fopen(file_path, "wb");
    if(pFile==NULL)
        return;
  
    int width = frame->width;
    int height = frame->height;
    
    fwrite(frame->data[0], 1, width * height, pFile);
    fwrite(frame->data[1], 1, width * height / 4, pFile);
    fwrite(frame->data[2], 1, width * height / 4, pFile);

 
    fclose(pFile);
}


int dump_audio_sdl(Uint8* data, int length)
{
    FILE* pFile;
    const char* file_path = "./audio_sdl.pcm";

    pFile = fopen(file_path, "ab+");
    if(pFile==NULL)
        return;

    fwrite(data, 1, length, pFile);

    fclose(pFile);
}

int dump_audio(Uint8* data, int length)
{
    FILE* pFile;
    const char* file_path = "./audio.pcm";

    pFile = fopen(file_path, "ab+");
    if(pFile==NULL)
        return;
    
    fwrite(data, 1, length, pFile);

 
    fclose(pFile);
}

static release_audio_filter() {
    avfilter_graph_free(&player.audio_graph);
}


static int init_audio_filter() {
    char args[512] = {0};
    int ret = 0;
    const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
    const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");

    static const enum AVSampleFormat out_sample_fmts[2] = {AV_SAMPLE_FMT_S16, -1};
    static const out_channels[2] = {2, -1};
    static const int64_t out_channel_layouts[2] = {AV_CH_LAYOUT_STEREO, -1};
    static const int out_sample_rates[2] = {44100, -1};

    player.audio_graph = avfilter_graph_alloc();
    if (!player.audio_graph) {
        ret = AVERROR(ENOMEM);
        goto end;
    }

    snprintf(args, sizeof(args),
              "sample_rate=%d:sample_fmt=%s:channels=%d:channel_layout=0x%"PRIx64,
               player.audio_samplerate, av_get_sample_fmt_name(player.audio_format),
               player.audio_channels, player.audio_channel_layout);
    av_log(NULL, AV_LOG_ERROR, "audio filter src:%s\n", args);

    ret = avfilter_graph_create_filter(&player.src_audio_filter, abuffersrc, "in", args, NULL, player.audio_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer source\n");
        goto end;
    }

    /* buffer audio sink: to terminate the filter chain. */
    ret = avfilter_graph_create_filter(&player.sink_audio_filter, abuffersink, "out", NULL, NULL, player.audio_graph);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "Cannot create audio buffer sink\n");
        goto end;
    }
    if ((ret = av_opt_set_int_list(player.sink_audio_filter, "channel_layouts", out_channel_layouts, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int_list(player.sink_audio_filter, "channel_counts", out_channels, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int_list(player.sink_audio_filter, "sample_rates", out_sample_rates, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = av_opt_set_int_list(player.sink_audio_filter, "sample_fmts", out_sample_fmts, -1, AV_OPT_SEARCH_CHILDREN)) < 0)
        goto end;
    if ((ret = avfilter_link(player.src_audio_filter, 0, player.sink_audio_filter, 0)) < 0)
        goto end;

    if ((ret = avfilter_graph_config(player.audio_graph, NULL)) < 0)
        goto end;

end:
    if (ret < 0)
        avfilter_graph_free(&player.audio_graph);
    return ret;
}

static int render_video_frame(AVFrame* frame) {
    SDL_Event event;
    if (frame) {
        SDL_PollEvent(&event);
        SDL_UpdateYUVTexture(player.video_surface.texture, NULL, frame->data[0], frame->linesize[0],
                              frame->data[1], frame->linesize[1],
                              frame->data[2], frame->linesize[2]);
        SDL_Rect sdlRect;
        sdlRect.x = 0;
        sdlRect.y = 0;
        sdlRect.w = player.video_surface.width;
        sdlRect.h = player.video_surface.height;
        SDL_RenderClear(player.video_surface.render);
        SDL_RenderCopy(player.video_surface.render, player.video_surface.texture, NULL, &sdlRect);
        SDL_RenderPresent(player.video_surface.render);
    }
}
static int update_video_render(int width, int height, int pixel_format) {
    int access, w, h, format;
    SDL_Texture *texture = player.video_surface.texture;
    if (SDL_QueryTexture(texture, &format, &access, &w, &h) < 0 || width != w || height != h || pixel_format != format) {
        void *pixels;
        int pitch;
        if (texture)
            SDL_DestroyTexture(texture);
        if (!(player.video_surface.texture = SDL_CreateTexture(player.video_surface.render, pixel_format, SDL_TEXTUREACCESS_STREAMING, width, height)))
            return -1;
        if (SDL_LockTexture(player.video_surface.texture, NULL, &pixels, &pitch) < 0)
            return -1;
        memset(pixels, 0, pitch * height);
        SDL_UnlockTexture(player.video_surface.texture);
    }
    av_log(NULL, AV_LOG_VERBOSE, "Created %dx%d texture with %s.\n", width, height, SDL_GetPixelFormatName(pixel_format));
}

static void video_refresh() {
    int ret;
    AVFrame* frame_refesh = av_frame_alloc();
    if (!frame_refesh) {
        return;
    }
    for (;;) {
        ret = frame_queue_get(&player.video_frames_queue, frame_refesh);
        if (ret == 0) {
            int64_t ts_stream = frame_refesh->pts;
            if (ts_stream == AV_NOPTS_VALUE)
                ts_stream = frame_refesh->pkt_dts;
            int64_t timestamp_video_real = av_rescale_q(ts_stream,
                                                        player.context->streams[player.index_video_stream]->time_base,AV_TIME_BASE_Q);
            int64_t av_diff = timestamp_video_real - player.clock.timestamp_audio_real;
            player.clock.timestamp_video_real = timestamp_video_real;
			player.clock.timestamp_video_stream = frame_refesh->pkt_dts;
            //av_log(NULL, AV_LOG_ERROR,"av diff:%lldms\n", av_diff / 1000);
            if (av_diff > 0) {
                usleep(av_diff);
            }
            update_video_render(frame_refesh->width, frame_refesh->height, SDL_PIXELFORMAT_IYUV);
            render_video_frame(frame_refesh);
            av_frame_unref(frame_refesh);
        } else {
            break;
        }
    }

    av_frame_free(&frame_refesh);
    av_log(NULL, AV_LOG_ERROR, "%s exit\n", __func__);
}

static void update_audio_clock(int64_t pts) {
    player.clock.timestamp_audio_stream = pts;
    int64_t timestamp_real = av_rescale_q(pts, player.context->streams[1]->time_base, AV_TIME_BASE_Q);
    player.clock.timestamp_audio_real= timestamp_real - 150000;
}

static int64_t get_current_position() {
    return player.clock.timestamp_audio_real - player.context->start_time;
}

static void avcodec_to_string(char *buf, int buf_size, AVCodecContext *enc, int encode)
{
    const char *codec_type;
    const char *codec_name;
    const char *profile = NULL;
    int64_t bitrate;
    int new_line = 0;
    AVRational display_aspect_ratio;
    const char *separator = enc->dump_separator ? (const char *)enc->dump_separator : ", ";

    if (!buf || buf_size <= 0)
        return;
    codec_type = av_get_media_type_string(enc->codec_type);
    codec_name = avcodec_get_name(enc->codec_id);
    profile = avcodec_profile_name(enc->codec_id, enc->profile);
    if (enc->codec_type == AVMEDIA_TYPE_VIDEO)
        snprintf(buf, buf_size, "%s_%s(%dx%d)", codec_type ? codec_type : "unknown",
                 codec_name, enc->width, enc->height);
    else {
        snprintf(buf, buf_size, "%s_%s", codec_type ? codec_type : "unknown",
                 codec_name);
    }
    return;
}

static void show_tracks_info() {
    char tracks_info[2048] = {0};
    for (int i = 0; i < player.context->nb_streams; i++) {
        char buf[256] = {0};
        AVStream *st = player.context->streams[i];
        AVCodecContext *avctx = avcodec_alloc_context3(NULL);
        if (!avctx)
            return;
        int ret = avcodec_parameters_to_context(avctx, st->codecpar);
        if (ret < 0) {
            avcodec_free_context(&avctx);
            return;
        }
        // Fields which are missing from AVCodecParameters need to be taken from the AVCodecContext
        avctx->properties = st->codec->properties;
        avctx->codec      = st->codec->codec;
        avctx->qmin       = st->codec->qmin;
        avctx->qmax       = st->codec->qmax;
        avctx->coded_width  = st->codec->coded_width;
        avctx->coded_height = st->codec->coded_height;
        avcodec_to_string(buf, sizeof(buf), avctx, 1);
        sprintf(tracks_info + strlen(tracks_info), "#Track_%d:%s    ", i, buf);
        //av_log(NULL, AV_LOG_ERROR, "tracks info:%s\n", tracks_info);
        avcodec_free_context(&avctx);
    }
    SDL_SetWindowTitle(player.video_surface.window, tracks_info);
}
static void update_play_info() {
    char title[256] = {0};
    sprintf(title, "%s, play(%d:%d)", player.context->filename, 
                (get_current_position() + 500000) / 1000000,
                (player.context->duration + 500000) / 1000000);
    SDL_SetWindowTitle(player.video_surface.window, title);
}

static void fill_pcm_data(void *opaque, Uint8 *buffer, int len) {
    int data_length = 0;
    int length_read = 0;
    //update_played_time();
    if (player.aframe_playing) {
        SDL_memset(buffer, 0, len);
        while (len > 0) {
            int ret;
            if (player.pos_abuffer_read >= player.pos_abuffer_tail) {
                ret = frame_queue_get(&player.audio_frames_queue, player.aframe_playing);
                if (ret == 0) {
                    player.pos_abuffer_read = 0;
                    player.pos_abuffer_tail = 2 * player.aframe_playing->nb_samples * av_get_channel_layout_nb_channels(player.aframe_playing->channel_layout);
                } else {
                    player.is_aplay_end = 1;
                    SDL_memset(buffer, 0, len);
                    return;
                }
            }
            data_length = player.pos_abuffer_tail - player.pos_abuffer_read;
            length_read = data_length > len ? len : data_length;
            SDL_MixAudio(buffer, player.aframe_playing->data[0] + player.pos_abuffer_read, length_read, SDL_MIX_MAXVOLUME);
            buffer += length_read;
            player.pos_abuffer_read += length_read;
            len -= length_read;
            update_audio_clock(player.aframe_playing->pkt_pts);
        }
    }
}


static int create_video_render(int width, int height, int pixel_format)
{
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER);
    player.video_surface.window = SDL_CreateWindow("", 0, 0, width, height, SDL_WINDOW_RESIZABLE);
    player.video_surface.render = SDL_CreateRenderer(player.video_surface.window, -1, 0);
    player.video_surface.texture = SDL_CreateTexture(player.video_surface.render, pixel_format, SDL_TEXTUREACCESS_STREAMING, width, height);

}

static int create_audio_render(int channels, int samplerate, int format) {
    SDL_AudioSpec request_params, response_params;
    request_params.channels = 2;
    request_params.freq = 44100;
    if (request_params.freq <= 0 || request_params.channels <= 0) {
        av_log(NULL, AV_LOG_ERROR, "Invalid sample rate or channel count!\n");
        return -1;
    }
    request_params.format = AUDIO_S16SYS;
    request_params.silence = 0;
    request_params.samples = FFMAX(SDL_AUDIO_MIN_BUFFER_SIZE, 2 << av_log2(request_params.freq / SDL_AUDIO_MAX_CALLBACKS_PER_SEC));
    request_params.callback = fill_pcm_data;
    request_params.userdata = NULL;

    if (SDL_OpenAudio(&request_params, NULL)) {
        av_log(NULL, AV_LOG_ERROR, "open audio failed\n");
        return -1;
    }
    SDL_PauseAudio(0);
    return 0;
}


static int open_video_decoder(AVFormatContext* context, AVStream *st) {

    int ret = -1;

    AVCodec* pCodec = NULL;
    pCodec = avcodec_find_decoder(st->codecpar->codec_id);
    if (NULL != pCodec) {
        player.vcodec_context = avcodec_alloc_context3(pCodec);
        if (NULL == player.vcodec_context) {
            av_log(context, AV_LOG_ERROR, "alloc condec context failed\n");
            ret = -1;
            goto fail;
        }
        avcodec_parameters_to_context(player.vcodec_context, st->codecpar);
    }
    ret = avcodec_open2(player.vcodec_context, NULL, NULL);
    if (0 == ret) {
        av_log(player.context, AV_LOG_DEBUG, "%s, open decoder ==%s== success\n", __func__, pCodec->name);
    }
fail:
    return ret;
}


static int open_audio_decoder(AVFormatContext* context, AVStream *st) {
    int ret = -1;

    AVCodec* pCodec = NULL;
    pCodec = avcodec_find_decoder(st->codecpar->codec_id);
    if (NULL != pCodec) {
        player.acodec_context = avcodec_alloc_context3(pCodec);
        if (NULL == player.acodec_context) {
            av_log(context, AV_LOG_ERROR, "alloc condec context failed\n");
            ret = -1;
            goto fail;
        }
        player.audio_samplerate = st->codecpar->sample_rate;
        player.audio_channels = st->codecpar->channels;
        player.audio_format = st->codecpar->format;
        player.audio_channel_layout = st->codecpar->channel_layout;
        avcodec_parameters_to_context(player.acodec_context, st->codecpar);
    }
    ret = avcodec_open2(player.acodec_context, NULL, NULL);
    if (0 == ret) {
        av_log(player.context, AV_LOG_DEBUG, "%s, open decoder [%s] success\n", __func__, pCodec->name);
        av_log(player.context , AV_LOG_DEBUG, "audio params (channels:%d, samplerate:%d, format:%d, channel_layout:0x%x)\n", 
             player.audio_channels, player.audio_samplerate, player.audio_format, player.audio_channel_layout);
    }
fail:
    return ret;
}

static int video_decoder_threadloop() {
    AVPacket pkt;
    int ret;
    AVFrame* video_frame = av_frame_alloc();
    if (!video_frame) {
        return -1;
    }
    for (;;) {
        ret = packet_queue_get(&player.video_pkts_queue, &pkt);
        if (-1 == ret) {
            av_packet_unref(&pkt);
            av_frame_unref(video_frame);
            break;
        }
        //av_log(NULL, AV_LOG_ERROR, "%s, pts:%lld, dts:%lld\n", __func__, pkt.pts, pkt.dts);
        int ret_send_pkt = avcodec_send_packet(player.vcodec_context, &pkt);
        int ret_decoder = avcodec_receive_frame(player.vcodec_context, video_frame);
        if (ret_decoder >= 0) {
            //av_log(NULL, AV_LOG_ERROR, "%s, pts:%lld, dts:%lld\n", __func__, video_frame->pts, video_frame->pkt_dts);
            frame_queue_put(&player.video_frames_queue, video_frame);
            av_frame_unref(video_frame);
        }
        av_packet_unref(&pkt);
    }

    av_frame_free(&video_frame);
    av_log(NULL, AV_LOG_ERROR, "%s exit\n", __func__);
}


static int audio_decoder_threadloop() {
    AVPacket pkt;
    int ret;
    AVFrame* audio_frame = av_frame_alloc();
    AVFrame* filter_audio_frame = av_frame_alloc();
    if (!audio_frame || !filter_audio_frame) {
        return -1;
    }
    for(;;) {
        ret = packet_queue_get(&player.audio_pkts_queue, &pkt);
        if (-1 == ret) {
            av_packet_unref(&pkt);
            av_frame_unref(filter_audio_frame);
            av_frame_unref(audio_frame);
            break;
        }
        avcodec_send_packet(player.acodec_context, &pkt);
        int ret_decoder = avcodec_receive_frame(player.acodec_context, audio_frame);
        if (ret_decoder >= 0 ) {
            if ((ret = av_buffersrc_add_frame_flags(player.src_audio_filter, audio_frame, AV_BUFFERSRC_FLAG_KEEP_REF)) < 0) {
                av_log(NULL, AV_LOG_ERROR, "send frame to src audio filter failed\n");
                goto go_on;
            }
            while (1) {
                ret = av_buffersink_get_frame(player.sink_audio_filter, filter_audio_frame);
                if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
                    //av_log(NULL, AV_LOG_ERROR, "get filterd audio frame failed, go on\n");
                    goto go_on;
                }
                if (ret == 0) {
                    int size_audio_sample = 2 * filter_audio_frame->nb_samples * av_get_channel_layout_nb_channels(filter_audio_frame->channel_layout);
                    //av_log(NULL, AV_LOG_ERROR, "get audio samples %d bytes from audio filter\n", size_audio_sample);
                    //dump_audio(filter_audio_frame->data[0], size_audio_sample);
                    frame_queue_put(&player.audio_frames_queue, filter_audio_frame);
                    av_frame_unref(filter_audio_frame);
                    av_frame_unref(audio_frame);
                } else {
                    av_log(NULL, AV_LOG_ERROR, "get filterd audio frame failed\n");
                    break;
                }
            }
        }
go_on:
        av_packet_unref(&pkt);
        av_frame_unref(filter_audio_frame);
        av_frame_unref(audio_frame);
    }
    av_frame_free(&audio_frame);
    av_frame_free(&filter_audio_frame);
    av_log(NULL, AV_LOG_ERROR, "%s exit\n", __func__);
}

static int get_stream_type(int stream_index) {
    return player.context->streams[stream_index]->codecpar->codec_type;
}

static int reset_video_decoder(AVStream* st) {
    avcodec_close(player.vcodec_context);
    avcodec_free_context(&player.vcodec_context);
    player.vcodec_context = NULL;
    open_video_decoder(player.context, st);
}

static int reset_audio_decoder(AVStream* st) {
    avcodec_close(player.acodec_context);
    avcodec_free_context(&player.acodec_context);
    player.acodec_context = NULL;
    open_audio_decoder(player.context, st);
}
static int select_tracks(int stream_selected) {
    int index_stream_selected = stream_selected;
    if (index_stream_selected < 0 || index_stream_selected > player.context->nb_streams - 1) {
        av_log(NULL, AV_LOG_ERROR, "stream index %d not found\n", index_stream_selected);
        return -1;
    }
    if (index_stream_selected == player.index_video_stream || index_stream_selected == player.index_audio_stream) {
        return 0;
    }

    player.index_dst_track = stream_selected;

    AVStream *st_dst = player.context->streams[stream_selected];
    st_dst->discard = AVDISCARD_DEFAULT;
    player.index_dst_track = stream_selected;
	av_log(NULL, AV_LOG_ERROR, "stream %d will be selected\n", player.index_dst_track);
    player.flag_select_track = 1;
}


static void get_defalut_tracks(AVFormatContext* context) {
    int i = 0;
    int is_vstream_find = 0;
    int is_astream_find = 0;

    player.index_video_stream = AVMEDIA_TYPE_VIDEO;
    player.index_audio_stream = AVMEDIA_TYPE_AUDIO;
    for (int i = 0; i < context->nb_streams; i++) {
        enum AVMediaType stream_type = context->streams[i]->codecpar->codec_type;
        av_log(NULL, AV_LOG_ERROR, "%s: stream:%d type:%d\n", __func__, i, stream_type);
        if (stream_type == AVMEDIA_TYPE_VIDEO) {
            if (!is_vstream_find) {
                is_vstream_find = 1;
                player.index_video_stream = i;
            }else {
                context->streams[i]->discard = AVDISCARD_ALL;
            }
        }
        else if (stream_type == AVMEDIA_TYPE_AUDIO) {
            if (!is_astream_find) {
                is_astream_find = 1;
                player.index_audio_stream = i;
            } else {
                context->streams[i]->discard = AVDISCARD_ALL;
            }
        }else {
            context->streams[i]->discard = AVDISCARD_ALL;
        }
    }
    av_log(NULL, AV_LOG_ERROR, "%s: video_stream:%d, audio_stream:%d\n", __func__,
             player.index_video_stream, player.index_audio_stream);
}

static int streams_open(const char*name) {
    int ret;
    AVFormatContext *context = NULL;
    player.flag_exit = 0;
    player.flag_select_track = 0;
    pthread_mutex_init(&(player.context_lock), NULL);
    player.is_aplay_end = 0;
    if (!player.aframe_playing) {
        player.aframe_playing = av_frame_alloc();
    }
    player.pos_abuffer_read = 0;
    player.pos_abuffer_tail = 0;

    player.clock.timestamp_audio_stream = -1;
    player.clock.timestamp_audio_real = -1;
    player.clock.timestamp_video_stream = -1;
    pthread_mutex_lock(&(player.context_lock));
    ret = avformat_open_input(&context, name, NULL, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s, open %s failed!!!!\n", __func__, name);
        goto fail;
    }

    ret = avformat_find_stream_info(context, NULL);
    if (ret < 0) {
        avformat_close_input(&context);
        av_log(context, AV_LOG_ERROR, "%s, find stream info failed\n", __func__);
        goto fail;
    }
    player.context = context;
    av_log(NULL, AV_LOG_ERROR, "start time:%lld\n", (context)->start_time);
    av_dump_format(player.context, 0, player.context->filename, 0);
    get_defalut_tracks(context);

    AVStream* st_video = (context)->streams[player.index_video_stream];
    AVStream* st_audio = (context)->streams[player.index_audio_stream];
    player.video_width = st_video->codecpar->width;
    player.video_height = st_video->codecpar->height;

    player.video_surface.width = player.video_width;
    player.video_surface.height = player.video_height;

    ret = open_video_decoder(context, st_video);
    if (ret < 0) {
        av_log(context, AV_LOG_ERROR, "%s, open video decoder failed\n", __func__);
        avformat_close_input(&context);
        goto fail;
    }
    packet_queue_init(&player.video_pkts_queue);
    frame_queue_init(&player.video_frames_queue);

    player.video_decoder_thread = SDL_CreateThread(video_decoder_threadloop, "video_decoder_threadloop", context);
    player.video_refresh = SDL_CreateThread(video_refresh, "video_refresh", context);

    ret = open_audio_decoder(context, st_audio);
    if (ret < 0) {
        av_log(context, AV_LOG_ERROR, "%s, open audio decoder failed\n", __func__);
		goto fail;
    }
    packet_queue_init(&player.audio_pkts_queue);
    init_audio_filter();
    frame_queue_init(&player.audio_frames_queue);
    player.audio_decoder_thread = SDL_CreateThread(audio_decoder_threadloop, "audio_decoder_threadloop", context);
fail:
    pthread_mutex_unlock(&(player.context_lock));
    return ret;
}

static void signal_decoder_thread_exit() {
     /* release audio pks queue */
    player.audio_pkts_queue.abort = 1;
    av_log(NULL, AV_LOG_ERROR, "set audio queue abort flag\n");

    /* release video pks queue */
    player.video_pkts_queue.abort = 1;
    av_log(NULL, AV_LOG_ERROR, "set video queue abort flag\n");
}

static void signal_render_thread_exit() {
    /* release audio frames queue */
    player.audio_frames_queue.abort = 1;
    av_log(NULL, AV_LOG_ERROR, "set audio frames queue abort flag\n");

    /* release video frames queue */
    player.video_frames_queue.abort = 1;
    av_log(NULL, AV_LOG_ERROR, "set video frames queue abort flag\n");
}


static int streams_close() {
    player.flag_exit = 1;
    signal_render_thread_exit();
    SDL_WaitThread(player.video_refresh, NULL);
    SDL_CloseAudio();

    signal_decoder_thread_exit();
    SDL_WaitThread(player.audio_decoder_thread, NULL);
    release_audio_filter();
    packet_queue_destroy(&player.audio_pkts_queue);

    SDL_WaitThread(player.video_decoder_thread, NULL);
    packet_queue_destroy(&player.video_pkts_queue);


    frame_queue_destroy(&player.audio_frames_queue);
    frame_queue_destroy(&player.video_frames_queue);

    if (player.vcodec_context != NULL) {
        avcodec_close(player.vcodec_context);
        avcodec_free_context(&player.vcodec_context);
        player.vcodec_context = NULL;
    }
    if (player.context != NULL) {
        pthread_mutex_lock(&(player.context_lock));
        avformat_close_input(&player.context);
        pthread_mutex_unlock(&(player.context_lock));
    }
    av_frame_free(&player.aframe_playing);
    return 0;
}



static void main_threadloop(AVFormatContext* context) {
    AVPacket pkt;
    int read_ret;
    while(!player.flag_exit) {
            pthread_mutex_lock(&(player.context_lock));
            if (player.context) {
                read_ret = av_read_frame(player.context, &pkt);
            }
            pthread_mutex_unlock(&(player.context_lock));
            if (read_ret < 0) {
                av_log(player.context, AV_LOG_ERROR, "read packet failed, ret:%d\n", read_ret);
                return;
            }
            if (get_stream_type(pkt.stream_index) == AVMEDIA_TYPE_VIDEO) {
                av_log(NULL, AV_LOG_ERROR, "video pkt(index:%d, pts:%lld, play_index:%d)\n", pkt.stream_index, pkt.pts, player.index_video_stream);
            }
            if (pkt.stream_index == player.index_video_stream) {
                packet_queue_put(&player.video_pkts_queue, &pkt);
            }
            if (pkt.stream_index == player.index_audio_stream) {
                packet_queue_put(&player.audio_pkts_queue, &pkt);
            }
            if (player.flag_select_track) {
                packet_queue_flush(&player.video_pkts_queue);
                packet_queue_flush(&player.audio_pkts_queue);
                avformat_seek_file(player.context, -1, 0, get_current_position() +  player.context->start_time, 
                                        INT64_MAX, AVSEEK_FLAG_BACKWARD);
                if (get_stream_type(player.index_dst_track) == AVMEDIA_TYPE_VIDEO) {
                    AVStream *st_dst = player.context->streams[player.index_dst_track];
                    player.index_src_track = player.index_video_stream;
                    player.index_video_stream = player.index_dst_track;
                    reset_video_decoder(st_dst);
                    //reset_video_render(st_dst->codecpar->width, st_dst->codecpar->height, SDL_PIXELFORMAT_IYUV);
                    player.flag_select_track = 0;
                }
            }
    }
    av_log(NULL, AV_LOG_ERROR, "main thread exit success\n");
    return;
}


static void process_key_event(const SDL_Event * event) {
    int index_stream_selected = 0;
    switch (event->key.keysym.sym) {
        case SDLK_t:
            SDL_Log("key T is down\n");
            update_play_info();
            break;
        case SDLK_s:
            SDL_Log("key S is down\n");
            show_tracks_info();
            break;
        case SDLK_0:
            index_stream_selected = 0;
            goto select_track;
        case SDLK_1:
            index_stream_selected = 1;
            goto select_track;
        case SDLK_2:
            index_stream_selected = 2;
            goto select_track;
        case SDLK_3:
            index_stream_selected = 3;
            goto select_track;
        case SDLK_4:
            index_stream_selected = 4;
            goto select_track;
        case SDLK_5:
            index_stream_selected = 5;
            goto select_track;
        case SDLK_6:
            index_stream_selected = 6;
            goto select_track;
        case SDLK_7:
            index_stream_selected = 7;
            goto select_track;
        case SDLK_8:
            index_stream_selected = 8;
select_track:
           select_tracks(index_stream_selected);
    };
}
static void process_window_event(const SDL_Event * event) {
    switch (event->window.event) {
        case SDL_WINDOWEVENT_SHOWN:
            SDL_Log("Window %d shown", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_HIDDEN:
            SDL_Log("Window %d hidden", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_EXPOSED:
            SDL_Log("Window %d exposed", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_MOVED:
            SDL_Log("Window %d moved to %d,%d",
                    event->window.windowID, event->window.data1,
                    event->window.data2);
            break;
        case SDL_WINDOWEVENT_RESIZED:
            SDL_Log("Window %d resized to %dx%d",
                    event->window.windowID, event->window.data1,
                    event->window.data2);
            player.video_surface.width  = event->window.data1;
            player.video_surface.height  = event->window.data2;
            break;
        case SDL_WINDOWEVENT_SIZE_CHANGED:
            SDL_Log("Window %d size changed to %dx%d",
                    event->window.windowID, event->window.data1,
                    event->window.data2);
            break;
        case SDL_WINDOWEVENT_MINIMIZED:
            SDL_Log("Window %d minimized", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_MAXIMIZED:
            SDL_Log("Window %d maximized", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_RESTORED:
            SDL_Log("Window %d restored", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_ENTER:
            SDL_Log("Mouse entered window %d",
                    event->window.windowID);
            break;
        case SDL_WINDOWEVENT_LEAVE:
            SDL_Log("Mouse left window %d", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_FOCUS_GAINED:
            SDL_Log("Window %d gained keyboard focus",
                    event->window.windowID);
            break;
        case SDL_WINDOWEVENT_FOCUS_LOST:
            SDL_Log("Window %d lost keyboard focus",
                    event->window.windowID);
            break;
        case SDL_WINDOWEVENT_CLOSE:
            SDL_Log("Window %d closed", event->window.windowID);
            streams_close();
            exit(0);
            break;
#if SDL_VERSION_ATLEAST(2, 0, 5)
        case SDL_WINDOWEVENT_TAKE_FOCUS:
            SDL_Log("Window %d is offered a focus", event->window.windowID);
            break;
        case SDL_WINDOWEVENT_HIT_TEST:
            SDL_Log("Window %d has a special hit test", event->window.windowID);
            break;
#endif
        default:
            SDL_Log("Window %d got unknown event %d",
                    event->window.windowID, event->window.event);
            break;
        }
}

static void eventloop() {
   SDL_Event event;
   av_log(NULL, AV_LOG_ERROR, "event_loop enter\n");
    while (SDL_WaitEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                av_log(NULL, AV_LOG_ERROR, "SDL Quit event");
                break;
            case SDL_KEYDOWN:
               process_key_event(&event);
               av_log(NULL, AV_LOG_ERROR, "SDL Key event");
               break;
            case SDL_WINDOWEVENT:
                av_log(NULL, AV_LOG_ERROR, "SDL Window event");
                process_window_event(&event);
                break;
            default:
                break;
        };
    }
    av_log(NULL, AV_LOG_ERROR, "event_loop exit\n");
}

int main(int argc, char* argv[])
{
    int ret = 0;
    if (argc < 2) {
       av_log(NULL, AV_LOG_ERROR, "no input file\n");
       return -1;
    }

    //av_log_set_level(AV_LOG_DEBUG);
    const char* file_name = argv[1];
    av_log(NULL, AV_LOG_DEBUG, "opening source:%s\n", file_name);

    av_register_all();

    ret = streams_open(file_name);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "open %s failed, goto fail\n", file_name);
        return ret;
    }

    create_video_render(player.video_width, player.video_height, SDL_PIXELFORMAT_IYUV);

    create_audio_render(player.audio_channels, player.audio_samplerate, player.audio_format);

    player.event_thread = SDL_CreateThread(eventloop, "event_loop", NULL);
    //create main thread
    player.main_thread = SDL_CreateThread(main_threadloop, "main_threadloop", player.context);
    SDL_WaitThread(player.main_thread, NULL);
    while (1) {
        usleep(10 * 100); //never exit
    }
    return 0;
}

