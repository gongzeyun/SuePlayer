#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>


//compile cmd: sudo gcc player.c -lavutil -lavformat -lavcodec -lz -lavutil -lpthread -lm -lswscale -lavfilter -lswresample

static int file_index = 0;

int dump_frame(AVFrame* frame)
{
    char file_path[256] = {0};
    FILE* pFile;
    sprintf(file_path, "/home/gongzeyun/work/SuePlayer/%04d.yuv", file_index++);

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

int main(int argc, char* argv[])
{
    AVFormatContext *pFormatContext;
    if (argc < 2) {
       av_log(pFormatContext, AV_LOG_ERROR, "You should specify file path to open");
       return -1;
    }
    
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
    }
   
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
                int ret_decoder = avcodec_receive_frame(pCodecContext, frame);
                if (ret_decoder >= 0) {
                    av_log(pFormatContext, AV_LOG_ERROR, "decoder frame success, line_size_y:%d, line_size_u:%d, line_size_v:%d, width:%d, height:%d\n", 
			   frame->linesize[0], frame->linesize[1], frame->linesize[2], frame->width, frame->height);
                    dump_frame(frame);
                    av_frame_unref(frame);
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
    return 0;
}

