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
typedef struct VideoRender{
	SDL_Window* window;
	SDL_Renderer* render;
	SDL_Texture* texture;
	int format;
	int x;
	int y;
	int width;
	int height;
}VideoRender;

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
    pthread_cond_t ring_queue_cond_produce;
    pthread_cond_t ring_queue_cond_consume;

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
    pthread_cond_t queue_cond;

    int abort;
}PacketQueue;


typedef struct AVPlayer {
    VideoRender *video_render;
    AudioRender *audio_render;

    AVFormatContext *context;
    AVCodecContext* vcodec_context;
    AVCodecContext* acodec_context;

    /* video info */
    int video_width;
    int video_height;

    /* audio info */
    int audio_samplerate;
    int audio_channels;
    int audio_format;
    int64_t  audio_channel_layout;
    SDL_Thread* main_thread;
    SDL_Thread* video_refresh;
    SDL_Thread* audio_decoder_thread;

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

AVPlayer player;
SDL_Window* window;
SDL_Renderer* sdlRenderer;
SDL_Texture* sdlTexture;
static SDL_AudioDeviceID audio_render;

static int frame_queue_init(SueFrameRingQueue* frame_queue) {
    frame_queue->pos_read = 0;
	frame_queue->pos_write = 0;
    frame_queue->last_operation = 0;
    frame_queue->abort = 0;
    pthread_mutex_init(&(frame_queue->ring_queue_lock), NULL);
    pthread_cond_init(&(frame_queue->ring_queue_cond_produce), NULL);
    pthread_cond_init(&(frame_queue->ring_queue_cond_consume), NULL);
    for (int i = 0; i < NUM_FRAMES_RING_BUFFER; i++) {
        if (!(frame_queue->sue_frames[i].frame = av_frame_alloc()))
            return AVERROR(ENOMEM);
    }

    return 0;
}

static int frame_queue_destroy(SueFrameRingQueue* frame_queue) {
    for (int i = 0; i < NUM_FRAMES_RING_BUFFER; i++) {
        av_frame_free(frame_queue->sue_frames[i].frame);
    }
    pthread_mutex_destroy(&(frame_queue->ring_queue_lock), NULL);
    pthread_cond_destroy(&(frame_queue->ring_queue_cond_produce), NULL);
    pthread_cond_destroy(&(frame_queue->ring_queue_cond_consume), NULL);
}

static int frame_queue_put(SueFrameRingQueue* frame_queue, AVFrame* frame) {
    pthread_mutex_lock(&(frame_queue->ring_queue_lock));
    int write_space = 0;
    if (frame_queue->abort) {
        goto exit;
    }
    do {
        if (frame_queue->pos_read == frame_queue->pos_write && frame_queue->last_operation == 0) {
            write_space = NUM_FRAMES_RING_BUFFER;
        } else {
            write_space = (frame_queue->pos_read - frame_queue->pos_write + NUM_FRAMES_RING_BUFFER) % (NUM_FRAMES_RING_BUFFER);
        }
        //av_log(NULL, AV_LOG_ERROR, "====write, pos_read:%d, pos_write:%d, last_operation:%d, write_space:%d\n",
        //         frame_queue->pos_read, frame_queue->pos_write, frame_queue->last_operation, write_space);
        if (write_space < 1) {
            pthread_cond_wait(&(frame_queue->ring_queue_cond_consume), &(frame_queue->ring_queue_lock));
        }
    }while (write_space < 1);
    if (write_space < 1) {
        /* only match when queue is aborting */
        goto exit;
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
    pthread_cond_signal(&(frame_queue->ring_queue_cond_produce));
exit:
    pthread_mutex_unlock(&(frame_queue->ring_queue_lock));
    return 0;
}


static int frame_queue_get(SueFrameRingQueue* frame_queue, AVFrame* frame) {
    int ret = -1;
    pthread_mutex_lock(&(frame_queue->ring_queue_lock));
    int read_space;
    do {
        if (frame_queue->pos_read == frame_queue->pos_write && frame_queue->last_operation == 1) {
            read_space = NUM_FRAMES_RING_BUFFER;
        } else {
            read_space = (frame_queue->pos_write - frame_queue->pos_read + NUM_FRAMES_RING_BUFFER) % (NUM_FRAMES_RING_BUFFER);
        }
        //av_log(NULL, AV_LOG_ERROR, "====read, pos_write:%d, pos_read:%d, read_space:%d\n", frame_queue->pos_write, frame_queue->pos_read, read_space);
        if (read_space < 1 && !frame_queue->abort) {
			pthread_cond_wait(&(frame_queue->ring_queue_cond_produce), &(frame_queue->ring_queue_lock));
        }
        if (read_space < 1 && frame_queue->abort) {
            goto exit;
        }
    } while (read_space < 1);
    av_frame_unref(frame);
    av_frame_move_ref(frame, frame_queue->sue_frames[frame_queue->pos_read].frame);
    av_frame_unref(frame_queue->sue_frames[frame_queue->pos_read].frame);

    int temp_pos_read = frame_queue->pos_read;
    frame_queue->pos_read++;
    frame_queue->pos_read %= NUM_FRAMES_RING_BUFFER;
    frame_queue->last_operation = 0;
    //av_log(NULL, AV_LOG_ERROR, "====read frame(pts:%lld) success, read_pos:%d, next_read_pos:%d\n", frame->pts, temp_pos_read, frame_queue->pos_read);
    pthread_cond_signal(&(frame_queue->ring_queue_cond_consume));
    ret = 0;
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
    pthread_cond_init(&(queue->queue_cond), NULL);
}

static int packet_queue_put(PacketQueue *queue, AVPacket *pkt) {
    pthread_mutex_lock(&(queue->queue_lock));
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
	pthread_cond_signal(&(queue->queue_cond));
    pthread_mutex_unlock(&(queue->queue_lock));

    return 0;
}

static int packet_queue_get(PacketQueue *queue, AVPacket* pkt) {
    int ret = -1;
    pthread_mutex_lock(&(queue->queue_lock));
    for (;;) {
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
            if (!queue->abort) {
                pthread_cond_wait(&(queue->queue_cond), &(queue->queue_lock));
            } else {
                ret = -1;
                break;
            }
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
    pthread_cond_destroy(&(queue->queue_cond));
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

static void fill_pcm_data(void *opaque, Uint8 *buffer, int len) {
    int data_length = 0;
    int length_read = 0;
    if (player.aframe_playing) {
        SDL_memset(buffer, 0, len);
        while (len > 0) {
            int ret;
            if (player.pos_abuffer_read >= player.pos_abuffer_tail) {
                ret = frame_queue_get(&player.audio_frames_queue, player.aframe_playing);
                if (ret == 0) {
                    player.pos_abuffer_read = 0;
                    player.pos_abuffer_tail = 2 * player.aframe_playing->nb_samples * av_get_channel_layout_nb_channels(player.aframe_playing->channel_layout);;
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
        }
    }
}

static int create_video_render(int width, int height, int pixel_format)
{
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO | SDL_INIT_TIMER);
    
    window = SDL_CreateWindow("test", 0, 0, width, height, SDL_WINDOW_RESIZABLE);
    sdlRenderer = SDL_CreateRenderer(window, -1, 0);
    sdlTexture = SDL_CreateTexture(sdlRenderer, pixel_format, SDL_TEXTUREACCESS_STREAMING, width, height);

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


static int open_video_decoder(AVFormatContext* context) {

    int ret = -1;

    AVCodec* pCodec = NULL;
    pCodec = avcodec_find_decoder(context->streams[AVMEDIA_TYPE_VIDEO]->codecpar->codec_id);
    if (NULL != pCodec) {
        player.vcodec_context = avcodec_alloc_context3(pCodec);
        if (NULL == player.vcodec_context) {
            av_log(context, AV_LOG_ERROR, "alloc condec context failed\n");
            ret = -1;
            goto fail;
        }
        avcodec_parameters_to_context(player.vcodec_context, context->streams[AVMEDIA_TYPE_VIDEO]->codecpar);
    }
    ret = avcodec_open2(player.vcodec_context, NULL, NULL);
    if (0 == ret) {
        av_log(player.context, AV_LOG_DEBUG, "%s, open decoder ==%s== success\n", __func__, pCodec->name);
    }
fail:
    return ret;
}


static int open_audio_decoder(AVFormatContext* context) {
	int ret = -1;

    AVCodec* pCodec = NULL;
    pCodec = avcodec_find_decoder(context->streams[AVMEDIA_TYPE_AUDIO]->codecpar->codec_id);
    if (NULL != pCodec) {
        player.acodec_context = avcodec_alloc_context3(pCodec);
        if (NULL == player.acodec_context) {
            av_log(context, AV_LOG_ERROR, "alloc condec context failed\n");
            ret = -1;
            goto fail;
        }
        player.audio_samplerate = context->streams[AVMEDIA_TYPE_AUDIO]->codecpar->sample_rate;
        player.audio_channels = context->streams[AVMEDIA_TYPE_AUDIO]->codecpar->channels;
        player.audio_format = context->streams[AVMEDIA_TYPE_AUDIO]->codecpar->format;
        player.audio_channel_layout = context->streams[AVMEDIA_TYPE_AUDIO]->codecpar->channel_layout;
        avcodec_parameters_to_context(player.acodec_context, context->streams[AVMEDIA_TYPE_AUDIO]->codecpar);
    }
    ret = avcodec_open2(player.acodec_context, NULL, NULL);
    if (0 == ret) {
        av_log(player.context, AV_LOG_ERROR, "%s, open decoder ==%s== success\n", __func__, pCodec->name);
        av_log(player.context , AV_LOG_ERROR, "audio_info (channels:%d, samplerate:%d, format:%d, channel_layout:0x%x)\n", 
             player.audio_channels, player.audio_samplerate, player.audio_format, player.audio_channel_layout);
    }
fail:
	return ret;
}

static int audio_decoder_threadloop() {
    AVPacket pkt;
    int ret;
    AVFrame* audio_frame = av_frame_alloc();
    AVFrame* filter_audio_frame = av_frame_alloc();
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
                    dump_audio(filter_audio_frame->data[0], size_audio_sample);
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
static int streams_open(AVFormatContext **context, const char*name) {
    int ret;
    player.is_aplay_end = 0;
    if (!player.aframe_playing) {
        player.aframe_playing = av_frame_alloc();
    }
    player.pos_abuffer_read = 0;
    player.pos_abuffer_tail = 0;
    ret = avformat_open_input(context, name, NULL, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s, open %s failed!!!!\n", __func__, name);
        return ret;
    }

    ret = avformat_find_stream_info(*context, NULL);
    if (ret < 0) {
        avformat_close_input(*context);
        av_log(context, AV_LOG_ERROR, "%s, find stream info failed\n", __func__);
        return ret;
    }

    player.video_width = (*context)->streams[AVMEDIA_TYPE_VIDEO]->codecpar->width;
    player.video_height = (*context)->streams[AVMEDIA_TYPE_VIDEO]->codecpar->height;

    ret = open_video_decoder(*context);
    if (ret < 0) {
        av_log(context, AV_LOG_ERROR, "%s, open video decoder failed\n", __func__);
        avformat_close_input(*context);
        return ret;
    }

	ret = open_audio_decoder(*context);
    if (ret < 0) {
        av_log(context, AV_LOG_ERROR, "%s, open audio decoder failed\n", __func__);
    }
    packet_queue_init(&player.audio_pkts_queue);
    init_audio_filter();
    frame_queue_init(&player.audio_frames_queue);
    player.audio_decoder_thread	= SDL_CreateThread(audio_decoder_threadloop, "audio_decoder_threadloop", player.context);
    return ret;
}

static int streams_close() {
    /* release audio pks queue */
    player.audio_pkts_queue.abort = 1;
    av_log(NULL, AV_LOG_ERROR, "set audio queue abort flag\n");
    pthread_mutex_lock(&(player.audio_pkts_queue.queue_lock));
    pthread_cond_signal(&(player.audio_pkts_queue.queue_cond));
    pthread_mutex_unlock(&(player.audio_pkts_queue.queue_lock));

    SDL_WaitThread(player.audio_decoder_thread, NULL);
    release_audio_filter();
    packet_queue_destroy(&player.audio_pkts_queue);

    /* release audio frames queue */
    player.audio_frames_queue.abort = 1;
    pthread_mutex_lock(&(player.audio_frames_queue.ring_queue_lock));
    pthread_cond_signal(&(player.audio_frames_queue.ring_queue_cond_produce));
    pthread_cond_signal(&(player.audio_frames_queue.ring_queue_cond_consume));
    pthread_mutex_unlock(&(player.audio_frames_queue.ring_queue_lock));

    while (!player.is_aplay_end){
        usleep(10 * 1000);
    }
    SDL_CloseAudio();
    if (player.vcodec_context != NULL) {
        avcodec_close(player.vcodec_context);
        avcodec_free_context(&player.vcodec_context);
        player.vcodec_context = NULL;
    }
    if (player.context != NULL) {
        avformat_close_input(&player.context);
    }
    av_frame_free(&player.aframe_playing);
    return 0;
}

static int render_video_frame(AVFrame* frame) {
    SDL_Event event;
	if (frame) {
		SDL_PollEvent(&event);
	    SDL_UpdateYUVTexture(sdlTexture, NULL, frame->data[0], frame->linesize[0],
	                              frame->data[1], frame->linesize[1],
	                              frame->data[2], frame->linesize[2]);
	    SDL_Rect sdlRect;
	    sdlRect.x = 0;  
	    sdlRect.y = 0;  
	    sdlRect.w = player.video_width;  
	    sdlRect.h = player.video_height;
	    SDL_RenderClear(sdlRenderer);
	    SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
	    SDL_RenderPresent(sdlRenderer);
    }
}



static void main_threadloop(AVFormatContext* context) {
    int ret = -1;
    AVFrame *video_frame = NULL;
    AVPacket pkt;

    video_frame = av_frame_alloc();
    while (video_frame != NULL) {
        int read_ret = av_read_frame(player.context, &pkt);
        if (read_ret < 0) {
            av_log(player.context, AV_LOG_ERROR, "read packet failed, ret:%d\n", ret);
            break;
        }
    
        if (pkt.stream_index == AVMEDIA_TYPE_VIDEO) {
            int ret_send_pkt = avcodec_send_packet(player.vcodec_context, &pkt);
            int ret_decoder = avcodec_receive_frame(player.vcodec_context, video_frame);
            if (ret_decoder >= 0) {
                render_video_frame(video_frame);
                av_frame_unref(video_frame);
            }
        } 
        if (pkt.stream_index == AVMEDIA_TYPE_AUDIO) {
            packet_queue_put(&player.audio_pkts_queue, &pkt);
        }
    }
    av_frame_free(&video_frame);
    av_log(NULL, AV_LOG_ERROR, "main thread exit success\n");
    return;
}



static int event_loop() {
    while (1) {
        usleep(100 * 1000);
    }
}

int main(int argc, char* argv[])
{
    int ret = 0;
    if (argc < 2) {
       av_log(NULL, AV_LOG_ERROR, "You should specify file to open");
       return -1;
    }
    
    const char* file_name = argv[1];
    av_log(NULL, AV_LOG_ERROR, "====source:%s\n", file_name);

    av_register_all();

    ret = streams_open(&player.context, file_name);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "====open %s failed, goto fail\n", file_name);
		return ret;
    }
    av_dump_format(player.context, 0, file_name, 0);

    create_video_render(player.video_width, player.video_height, SDL_PIXELFORMAT_IYUV);

    create_audio_render(player.audio_channels, player.audio_samplerate, player.audio_format);
    //create main thread
    player.main_thread = SDL_CreateThread(main_threadloop, "main_threadloop", player.context);
    SDL_WaitThread(player.main_thread, NULL);
    streams_close();
    return 0;
}

