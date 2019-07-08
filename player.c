#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <SDL2/SDL.h>


//compile cmd: sudo gcc player.c -lavutil -lavformat -lavcodec -lz -lavutil -lpthread -lm -lswscale -lavfilter -lswresample -lSDL2
//ffplay -f rawvideo -video_size 1280x720 0239.yuv

static int file_index = 0;

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


typedef struct AVPlayer {
	VideoRender *video_render;
	AudioRender *audio_render;

        AVFormatContext *context;
        AVCodecContext* vcodec_context;
	/* video info */
	int video_width;
	int video_height;

	/* audio info */
	int audio_samplerate;
	int audio_channels;
	int audio_format;
	
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



static int create_sdl_window(AVPlayer* player, int width, int height, int pixel_format)
{
    SDL_Init(SDL_INIT_AUDIO | SDL_INIT_VIDEO);
    
    window = SDL_CreateWindow("test", 0, 0, width, height, SDL_WINDOW_RESIZABLE);
    sdlRenderer = SDL_CreateRenderer(window, -1, 0);
    sdlTexture = SDL_CreateTexture(sdlRenderer, pixel_format, SDL_TEXTUREACCESS_STREAMING, width, height);

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


static int streams_open(AVFormatContext **context, const char*name) {
    int ret;
    ret = avformat_open_input(context, name, NULL, NULL);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "%s, open %s failed!!!!\n", __func__, name);
        return ret;
    }

    return avformat_find_stream_info(*context, NULL);
}


int main(int argc, char* argv[])
{
    int ret = 0;
    if (argc < 2) {
       av_log(NULL, AV_LOG_ERROR, "You should specify file to open");
       return -1;
    }

    SDL_Event event;
    
#if 1	
    const char* file_name = argv[1];
    av_log(NULL, AV_LOG_ERROR, "====source:%s\n", file_name);

    av_register_all();


    ret = streams_open(&player.context, file_name);
    if (ret < 0) {
        av_log(NULL, AV_LOG_ERROR, "====open %s failed, goto fail\n", file_name);
        goto fail;
    }
    av_dump_format(player.context, 0, file_name, 0);


    int video_width = player.context->streams[AVMEDIA_TYPE_VIDEO]->codecpar->width;
    int video_height = player.context->streams[AVMEDIA_TYPE_VIDEO]->codecpar->height;
    create_sdl_window(NULL, video_width, video_height, SDL_PIXELFORMAT_IYUV);

    AVFrame *frame = av_frame_alloc();
    
    if (0 == open_video_decoder(player.context)) {
#if 1
        AVPacket pkt;
        while (frame != NULL) {
            int read_ret = av_read_frame(player.context, &pkt);
            if (read_ret < 0) {
                av_log(player.context, AV_LOG_ERROR, "read packet failed\n");
                break;
            }
            
            if (pkt.stream_index == AVMEDIA_TYPE_VIDEO) {
                int ret_send_pkt = avcodec_send_packet(player.vcodec_context, &pkt);
                int ret_decoder = avcodec_receive_frame(player.vcodec_context, frame);
                if (ret_decoder >= 0) {
                    //dump_frame(frame);
                    SDL_PollEvent(&event);
                    SDL_UpdateYUVTexture(sdlTexture, NULL, frame->data[0], frame->linesize[0],
                                                  frame->data[1], frame->linesize[1],
                                                  frame->data[2], frame->linesize[2]);
                    SDL_Rect sdlRect;
                    sdlRect.x = 0;  
                    sdlRect.y = 0;  
                    sdlRect.w = 1920;  
                    sdlRect.h = 1080;
                    SDL_RenderClear(sdlRenderer);
                    SDL_RenderCopy(sdlRenderer, sdlTexture, NULL, &sdlRect);
                    SDL_RenderPresent(sdlRenderer);
                    av_frame_unref(frame);
                    usleep(40 * 1000);
                }
            } 
            av_packet_unref(&pkt);
        }
    }
#endif
fail:
    avcodec_close(player.vcodec_context);
    avcodec_free_context(&player.vcodec_context);
    av_frame_free(frame);
    avformat_close_input(&player.context);
#endif
    return 0;
}

