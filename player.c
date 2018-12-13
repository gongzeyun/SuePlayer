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

int main(int argc, char* argv[])
{
    AVFormatContext *pFormatContext = NULL;
    
    if (argc < 2) {
       av_log(pFormatContext, AV_LOG_ERROR, "You should specify file path to open");
       return -1;
    }

	SDL_Event event;
    
#if 1	
    const char* file_name = argv[1];
    av_log(pFormatContext, AV_LOG_ERROR, "open file ====%s====\n", file_name);

    av_register_all();

    int open_ret = avformat_open_input(&pFormatContext, file_name, NULL, NULL);
    if (open_ret < 0) {
        av_log(pFormatContext, AV_LOG_ERROR, "open file %s failed!!!!\n");
        goto fail;
    }

    av_log(pFormatContext, AV_LOG_ERROR, "open file ====%s==== success\n", pFormatContext->filename);

    avformat_find_stream_info(pFormatContext, NULL);
   
    av_dump_format(pFormatContext, 0, file_name, 0);
    av_log(pFormatContext, AV_LOG_ERROR, "duration:%lld, video_width:%d, video_height:%d\n",
		pFormatContext->duration, pFormatContext->streams[0]->codecpar->width, pFormatContext->streams[0]->codecpar->height);

    //open decoder
    AVCodec* pCodec = NULL;
    AVCodecContext* pCodecContext = NULL;
    AVFrame *frame = av_frame_alloc();
    pCodec = avcodec_find_decoder(pFormatContext->streams[AVMEDIA_TYPE_VIDEO]->codecpar->codec_id);
    if (NULL != pCodec) {
        av_log(pFormatContext, AV_LOG_ERROR, "codec name:%s\n", pCodec->name);
        pCodecContext = avcodec_alloc_context3(pCodec);
        if (NULL == pCodecContext) {
            av_log(pFormatContext, AV_LOG_ERROR, "alloc condec context failed\n");
            goto fail;
        }
        avcodec_parameters_to_context(pCodecContext, pFormatContext->streams[AVMEDIA_TYPE_VIDEO]->codecpar);
    }
    int video_width = pFormatContext->streams[AVMEDIA_TYPE_VIDEO]->codecpar->width;
    int video_height = pFormatContext->streams[AVMEDIA_TYPE_VIDEO]->codecpar->height;
    create_sdl_window(NULL, video_width, video_height, SDL_PIXELFORMAT_IYUV);
    int ret_open_decoder = avcodec_open2(pCodecContext, NULL, NULL);
    if (0 == ret_open_decoder) {
        av_log(pFormatContext, AV_LOG_ERROR, "open decoder %s success\n", pCodec->name);
#if 1
        AVPacket pkt;
        while (frame != NULL) {
            int read_ret = av_read_frame(pFormatContext, &pkt);
            if (read_ret < 0) {
                av_log(pFormatContext, AV_LOG_ERROR, "read packet failed\n");
                break;
            }
            
            if (pkt.stream_index == AVMEDIA_TYPE_VIDEO) {
                int ret_send_pkt = avcodec_send_packet(pCodecContext, &pkt);
                //av_log(pFormatContext, AV_LOG_ERROR, "send pkt ret %d\n", ret_send_pkt);
                int ret_decoder = avcodec_receive_frame(pCodecContext, frame);
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
    avcodec_close(pCodecContext);
    avcodec_free_context(&pCodecContext);
    av_frame_free(frame);
    avformat_close_input(&pFormatContext);
#endif
    return 0;
}

