#include "system.h"

#ifndef THIS_IS_NOT_XBMC
  #if (defined HAVE_CONFIG_H) && (!defined WIN32)
    #include "config.h"
  #endif

  #include "utils/log.h"
#endif

#include "LinuxC1Codec.h"

#ifdef CLASSNAME
#undef CLASSNAME
#endif
#define CLASSNAME "CLinuxC1Codec"

static vformat_t codecid_to_vformat(enum AVCodecID id)
{
  vformat_t format;
  switch (id)
  {
    case AV_CODEC_ID_H264:
      format = VFORMAT_H264;
      break;
    case AV_CODEC_ID_HEVC:
      format = VFORMAT_HEVC;
      break;

    default:
      format = VFORMAT_UNSUPPORT;
      break;
  }

  CLog::Log(LOGDEBUG, "codecid_to_vformat, id(%d) -> vformat(%d)", (int)id, format);
  return format;
}

static vdec_type_t codec_tag_to_vdec_type(unsigned int codec_tag)
{
  vdec_type_t dec_type;
  switch (codec_tag)
  {
    case CODEC_TAG_AVC1:
    case CODEC_TAG_avc1:
    case CODEC_TAG_H264:
    case CODEC_TAG_h264:
    case AV_CODEC_ID_H264:
      dec_type = VIDEO_DEC_FORMAT_H264;
      break;
    case AV_CODEC_ID_HEVC:
      dec_type = VIDEO_DEC_FORMAT_HEVC;
      break;

    default:
      dec_type = VIDEO_DEC_FORMAT_UNKNOW;
      break;
  }

  CLog::Log(LOGDEBUG, "codec_tag_to_vdec_type, codec_tag(%d) -> vdec_type(%d)", codec_tag, dec_type);
  return dec_type;
}

void codec_init_para(aml_generic_param *p_in, codec_para_t *p_out)
{
  memzero(*p_out);

  p_out->has_video          = 1;
  p_out->noblock            = p_in->noblock;
  p_out->video_pid          = p_in->video_pid;
  p_out->video_type         = p_in->video_type;
  p_out->stream_type        = p_in->stream_type;
  p_out->am_sysinfo.format  = p_in->format;
  p_out->am_sysinfo.width   = p_in->width;
  p_out->am_sysinfo.height  = p_in->height;
  p_out->am_sysinfo.rate    = p_in->rate;
  p_out->am_sysinfo.extra   = p_in->extra;
  p_out->am_sysinfo.status  = p_in->status;
  p_out->am_sysinfo.ratio   = p_in->ratio;
  p_out->am_sysinfo.ratio64 = p_in->ratio64;
  p_out->am_sysinfo.param   = p_in->param;
}

void am_packet_release(am_packet_t *pkt)
{
  if (pkt->buf != NULL)
    free(pkt->buf), pkt->buf= NULL;
  if (pkt->hdr != NULL)
  {
    if (pkt->hdr->data != NULL)
      free(pkt->hdr->data), pkt->hdr->data = NULL;
    free(pkt->hdr), pkt->hdr = NULL;
  }

  pkt->codec = NULL;
}

int check_in_pts(am_private_t *para, am_packet_t *pkt)
{
    int last_duration = 0;
    static int last_v_duration = 0;
    int64_t pts = 0;

    last_duration = last_v_duration;

    if (para->stream_type == AM_STREAM_ES) {
        if ((int64_t)AV_NOPTS_VALUE != pkt->avpts) {
            pts = pkt->avpts;

            if (codec_checkin_pts(pkt->codec, pts) != 0) {
                CLog::Log(LOGDEBUG, "ERROR check in pts error!");
                return PLAYER_PTS_ERROR;
            }

        } else if ((int64_t)AV_NOPTS_VALUE != pkt->avdts) {
            pts = pkt->avdts * last_duration;

            if (codec_checkin_pts(pkt->codec, pts) != 0) {
                CLog::Log(LOGDEBUG, "ERROR check in dts error!");
                return PLAYER_PTS_ERROR;
            }

            last_v_duration = pkt->avduration ? pkt->avduration : 1;
        } else {
            if (!para->check_first_pts) {
                if (codec_checkin_pts(pkt->codec, 0) != 0) {
                    CLog::Log(LOGDEBUG, "ERROR check in 0 to video pts error!");
                    return PLAYER_PTS_ERROR;
                }
            }
        }
        if (!para->check_first_pts) {
            para->check_first_pts = 1;
        }
    }
    if (pts > 0)
      pkt->lastpts = pts;

    return PLAYER_SUCCESS;
}

static int write_header(am_private_t *para, am_packet_t *pkt)
{
    int write_bytes = 0, len = 0;

    if (pkt->hdr && pkt->hdr->size > 0) {
        if ((NULL == pkt->codec) || (NULL == pkt->hdr->data)) {
            CLog::Log(LOGDEBUG, "[write_header]codec null!");
            return PLAYER_EMPTY_P;
        }
        //some wvc1 es data not need to add header
        if (para->video_format == VFORMAT_VC1 && para->video_codec_type == VIDEO_DEC_FORMAT_WVC1) {
            if ((pkt->data) && (pkt->data_size >= 4)
              && (pkt->data[0] == 0) && (pkt->data[1] == 0)
              && (pkt->data[2] == 1) && (pkt->data[3] == 0xd || pkt->data[3] == 0xf)) {
                return PLAYER_SUCCESS;
            }
        }
        while (1) {
            write_bytes = codec_write(pkt->codec, pkt->hdr->data + len, pkt->hdr->size - len);
            if (write_bytes < 0 || write_bytes > (pkt->hdr->size - len)) {
                if (-errno != AVERROR(EAGAIN)) {
                    CLog::Log(LOGDEBUG, "ERROR:write header failed!");
                    return PLAYER_WR_FAILED;
                } else {
                    continue;
                }
            } else {
                len += write_bytes;
                if (len == pkt->hdr->size) {
                    break;
                }
            }
        }
    }
    return PLAYER_SUCCESS;
}

int write_av_packet(am_private_t *para, am_packet_t *pkt)
{
  //CLog::Log(LOGDEBUG, "write_av_packet, pkt->isvalid(%d), pkt->data(%p), pkt->data_size(%d)",
  //  pkt->isvalid, pkt->data, pkt->data_size);

    int write_bytes = 0, len = 0, ret;
    unsigned char *buf;
    int size;

    // do we need to check in pts or write the header ?
    if (pkt->newflag) {
        if (pkt->isvalid) {
            ret = check_in_pts(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                CLog::Log(LOGDEBUG, "check in pts failed");
                return PLAYER_WR_FAILED;
            }
        }
        if (write_header(para, pkt) == PLAYER_WR_FAILED) {
            CLog::Log(LOGDEBUG, "[%s]write header failed!", __FUNCTION__);
            return PLAYER_WR_FAILED;
        }
        pkt->newflag = 0;
    }

    buf = pkt->data;
    size = pkt->data_size ;
    if (size == 0 && pkt->isvalid) {
        pkt->isvalid = 0;
        pkt->data_size = 0;
    }

    while (size > 0 && pkt->isvalid) {
        write_bytes = codec_write(pkt->codec, buf, size);
        if (write_bytes < 0 || write_bytes > size) {
            CLog::Log(LOGDEBUG, "write codec data failed, write_bytes(%d), errno(%d), size(%d)", write_bytes, errno, size);
            if (-errno != AVERROR(EAGAIN)) {
                CLog::Log(LOGDEBUG, "write codec data failed!");
                return PLAYER_WR_FAILED;
            } else {
                // adjust for any data we already wrote into codec.
                // we sleep a bit then exit as we will get called again
                // with the same pkt because pkt->isvalid has not been cleared.
                pkt->data += len;
                pkt->data_size -= len;
                usleep(RW_WAIT_TIME);
                CLog::Log(LOGDEBUG, "usleep(RW_WAIT_TIME), len(%d)", len);
                return PLAYER_SUCCESS;
            }
        } else {
            // keep track of what we write into codec from this pkt
            // in case we get hit with EAGAIN.
            len += write_bytes;
            if (len == pkt->data_size) {
                pkt->isvalid = 0;
                pkt->data_size = 0;
                break;
            } else if (len < pkt->data_size) {
                buf += write_bytes;
                size -= write_bytes;
            } else {
                // writing more that we should is a failure.
                return PLAYER_WR_FAILED;
            }
        }
    }

    return PLAYER_SUCCESS;
}

static int h264_add_header(unsigned char *buf, int size, am_packet_t *pkt)
{
    if (size > HDR_BUF_SIZE)
    {
        free(pkt->hdr->data);
        pkt->hdr->data = (char *)malloc(size);
        if (!pkt->hdr->data)
            return PLAYER_NOMEM;
    }

    memcpy(pkt->hdr->data, buf, size);
    pkt->hdr->size = size;
    return PLAYER_SUCCESS;
}

static int h264_write_header(am_private_t *para, am_packet_t *pkt)
{
    // CLog::Log(LOGDEBUG, "h264_write_header");
    int ret = h264_add_header(para->extradata, para->extrasize, pkt);
    if (ret == PLAYER_SUCCESS) {
        //if (ctx->vcodec) {
        if (1) {
            pkt->codec = &para->vcodec;
        } else {
            //CLog::Log(LOGDEBUG, "[pre_header_feeding]invalid video codec!");
            return PLAYER_EMPTY_P;
        }

        pkt->newflag = 1;
        ret = write_av_packet(para, pkt);
    }
    return ret;
}

static int hevc_add_header(unsigned char *buf, int size,  am_packet_t *pkt)
{
    if (size > HDR_BUF_SIZE)
    {
        free(pkt->hdr->data);
        pkt->hdr->data = (char *)malloc(size);
        if (!pkt->hdr->data)
            return PLAYER_NOMEM;
    }

    memcpy(pkt->hdr->data, buf, size);
    pkt->hdr->size = size;
    return PLAYER_SUCCESS;
}

static int hevc_write_header(am_private_t *para, am_packet_t *pkt)
{
    int ret = -1;

    if (para->extradata) {
      ret = hevc_add_header(para->extradata, para->extrasize, pkt);
    }
    if (ret == PLAYER_SUCCESS) {
      pkt->codec = &para->vcodec;
      pkt->newflag = 1;
      ret = write_av_packet(para, pkt);
    }
    return ret;
}

int pre_header_feeding(am_private_t *para, am_packet_t *pkt)
{
    int ret;
    if (para->stream_type == AM_STREAM_ES) {
        if (pkt->hdr == NULL) {
            pkt->hdr = (hdr_buf_t*)malloc(sizeof(hdr_buf_t));
            pkt->hdr->data = (char *)malloc(HDR_BUF_SIZE);
            if (!pkt->hdr->data) {
                //CLog::Log(LOGDEBUG, "[pre_header_feeding] NOMEM!");
                return PLAYER_NOMEM;
            }
        }

        if (VFORMAT_H264 == para->video_format || VFORMAT_H264_4K2K == para->video_format) {
            ret = h264_write_header(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        } else if (VFORMAT_HEVC == para->video_format) {
            ret = hevc_write_header(para, pkt);
            if (ret != PLAYER_SUCCESS) {
                return ret;
            }
        }

        if (pkt->hdr) {
            if (pkt->hdr->data) {
                free(pkt->hdr->data);
                pkt->hdr->data = NULL;
            }
            free(pkt->hdr);
            pkt->hdr = NULL;
        }
    }
    return PLAYER_SUCCESS;
}

CLinuxC1Codec::CLinuxC1Codec() {
  am_private = new am_private_t;
  memzero(am_private);
}

CLinuxC1Codec::~CLinuxC1Codec() {
  delete am_private;
  am_private = NULL;
}

bool CLinuxC1Codec::OpenDecoder(CDVDStreamInfo &hints) {
  CLog::Log(LOGDEBUG, "CLinuxC1Codec::OpenDecoder");
  m_speed = DVD_PLAYSPEED_NORMAL;
  m_1st_pts = 0;
  m_cur_pts = 0;
/*
  m_cur_pictcnt = 0;
  m_old_pictcnt = 0;
  m_dst_rect.SetRect(0, 0, 0, 0);
  m_zoom = -1;
  m_contrast = -1;
  m_brightness = -1;
  m_vbufsize = 500000 * 2;
  m_start_dts = 0;
  m_start_pts = 0;
 */
  m_hints = hints;

  ShowMainVideo(false);

  memzero(am_private->am_pkt);
  am_private->stream_type      = AM_STREAM_ES;
  am_private->video_width      = hints.width;
  am_private->video_height     = hints.height;
  am_private->video_codec_id   = hints.codec;
  am_private->video_codec_tag  = hints.codec_tag;
  am_private->video_pid        = hints.pid;

  // handle video ratio
/*
  AVRational video_ratio       = av_d2q(1, SHRT_MAX);
  am_private->video_ratio      = ((int32_t)video_ratio.num << 16) | video_ratio.den;
  am_private->video_ratio64    = ((int64_t)video_ratio.num << 32) | video_ratio.den;
*/
  // handle video rate
  if (hints.rfpsrate > 0 && hints.rfpsscale != 0)
  {
    // check ffmpeg r_frame_rate 1st
    am_private->video_rate = 0.5 + (float)UNIT_FREQ * hints.rfpsscale / hints.rfpsrate;
  }
  else if (hints.fpsrate > 0 && hints.fpsscale != 0)
  {
    // then ffmpeg avg_frame_rate next
    am_private->video_rate = 0.5 + (float)UNIT_FREQ * hints.fpsscale / hints.fpsrate;
  }

  // check for 1920x1080, interlaced, 25 fps
  // incorrectly reported as 50 fps (yes, video_rate == 1920)
  if (hints.width == 1920 && am_private->video_rate == 1920)
  {
    CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder video_rate exception");
    am_private->video_rate = 0.5 + (float)UNIT_FREQ * 1001 / 25000;
  }

  // check for SD h264 content incorrectly reported as 60 fsp
  // mp4/avi containers :(
  if (hints.codec == AV_CODEC_ID_H264 && hints.width <= 720 && am_private->video_rate == 1602)
  {
    CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder video_rate exception");
    am_private->video_rate = 0.5 + (float)UNIT_FREQ * 1001 / 24000;
  }

  // check for SD h264 content incorrectly reported as some form of 30 fsp
  // mp4/avi containers :(
  if (hints.codec == AV_CODEC_ID_H264 && hints.width <= 720)
  {
    if (am_private->video_rate >= 3200 && am_private->video_rate <= 3210)
    {
      CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder video_rate exception");
      am_private->video_rate = 0.5 + (float)UNIT_FREQ * 1001 / 24000;
    }
  }

  // handle orientation
  am_private->video_rotation_degree = 0;
  if (hints.orientation == 90)
    am_private->video_rotation_degree = 1;
  else if (hints.orientation == 180)
    am_private->video_rotation_degree = 2;
  else if (hints.orientation == 270)
    am_private->video_rotation_degree = 3;
  // handle extradata
  am_private->video_format      = codecid_to_vformat(hints.codec);
  if (am_private->video_format == VFORMAT_H264) {
      if (hints.width > 1920 || hints.height > 1088) {
        am_private->video_format = VFORMAT_H264_4K2K;
      }
  }
  switch (am_private->video_format)
  {
    default:
      am_private->extrasize       = hints.extrasize;
      am_private->extradata       = (uint8_t*)malloc(hints.extrasize);
      memcpy(am_private->extradata, hints.extradata, hints.extrasize);
      break;
    case VFORMAT_REAL:
    case VFORMAT_MPEG12:
      break;
  }

  if (am_private->stream_type == AM_STREAM_ES && am_private->video_codec_tag != 0)
    am_private->video_codec_type = codec_tag_to_vdec_type(am_private->video_codec_tag);
  if (am_private->video_codec_type == VIDEO_DEC_FORMAT_UNKNOW)
    am_private->video_codec_type = codec_tag_to_vdec_type(am_private->video_codec_id);

  am_private->flv_flag = 0;

  CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder "
    "hints.width(%d), hints.height(%d), hints.codec(%d), hints.codec_tag(%d), hints.pid(%d)",
    hints.width, hints.height, hints.codec, hints.codec_tag, hints.pid);
  CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder hints.fpsrate(%d), hints.fpsscale(%d), hints.rfpsrate(%d), hints.rfpsscale(%d), video_rate(%d)",
    hints.fpsrate, hints.fpsscale, hints.rfpsrate, hints.rfpsscale, am_private->video_rate);
  CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder hints.orientation(%d), hints.forced_aspect(%d), hints.extrasize(%d)",
    hints.orientation, hints.forced_aspect, hints.extrasize);

  // default video codec params
  am_private->gcodec.noblock     = 0;
  am_private->gcodec.video_pid   = am_private->video_pid;
  am_private->gcodec.video_type  = am_private->video_format;
  am_private->gcodec.stream_type = STREAM_TYPE_ES_VIDEO;
  am_private->gcodec.format      = am_private->video_codec_type;
  am_private->gcodec.width       = am_private->video_width;
  am_private->gcodec.height      = am_private->video_height;
  am_private->gcodec.rate        = am_private->video_rate;
  am_private->gcodec.ratio       = am_private->video_ratio;
  am_private->gcodec.ratio64     = am_private->video_ratio64;
  am_private->gcodec.param       = NULL;

  switch(am_private->video_format)
  {
    case VFORMAT_H264:
    case VFORMAT_H264MVC:
      am_private->gcodec.format = VIDEO_DEC_FORMAT_H264;
      am_private->gcodec.param  = (void*)EXTERNAL_PTS;
      // h264 in an avi file
      if (m_hints.ptsinvalid)
        am_private->gcodec.param = (void*)(EXTERNAL_PTS | SYNC_OUTSIDE);
      break;
    case VFORMAT_H264_4K2K:
      am_private->gcodec.format = VIDEO_DEC_FORMAT_H264_4K2K;
      am_private->gcodec.param  = (void*)EXTERNAL_PTS;
      // h264 in an avi file
      if (m_hints.ptsinvalid)
        am_private->gcodec.param = (void*)(EXTERNAL_PTS | SYNC_OUTSIDE);
      break;
    case VFORMAT_HEVC:
      am_private->gcodec.format = VIDEO_DEC_FORMAT_HEVC;
      am_private->gcodec.param  = (void*)EXTERNAL_PTS;
      if (m_hints.ptsinvalid)
        am_private->gcodec.param = (void*)(EXTERNAL_PTS | SYNC_OUTSIDE);
      break;
    default:
      break;
  }
  am_private->gcodec.param = (void *)((unsigned int)am_private->gcodec.param | (am_private->video_rotation_degree << 16));

  // translate from generic to firmware version dependent
  codec_init_para(&am_private->gcodec, &am_private->vcodec);

  int ret = codec_init(&am_private->vcodec);
  if (ret != CODEC_ERROR_NONE)
  {
    CLog::Log(LOGDEBUG, "CAMLCodec::OpenDecoder codec init failed, ret=0x%x", -ret);
    return false;
  }

  // make sure we are not stuck in pause (amcodec bug)
  codec_resume(&am_private->vcodec);
  codec_set_cntl_mode(&am_private->vcodec, TRICKMODE_NONE);

  codec_set_cntl_avthresh(&am_private->vcodec, AV_SYNC_THRESH);
  codec_set_cntl_syncthresh(&am_private->vcodec, 0);
  // disable tsync, we are playing video disconnected from audio.
  SysfsUtils::SetInt("/sys/class/tsync/enable", 0);

  am_private->am_pkt.codec = &am_private->vcodec;
  pre_header_feeding(am_private, &am_private->am_pkt);

//  Create();

/*
  g_renderManager.RegisterRenderUpdateCallBack((const void*)this, RenderUpdateCallBack);
  g_renderManager.RegisterRenderFeaturesCallBack((const void*)this, RenderFeaturesCallBack);

  m_display_rect = CRect(0, 0, CDisplaySettings::Get().GetCurrentResolutionInfo().iWidth, CDisplaySettings::Get().GetCurrentResolutionInfo().iHeight);

  std::string strScaler;
  SysfsUtils::GetString("/sys/class/ppmgr/ppscaler", strScaler);
  if (strScaler.find("enabled") == std::string::npos)     // Scaler not enabled, use screen size
    m_display_rect = CRect(0, 0, CDisplaySettings::Get().GetCurrentResolutionInfo().iScreenWidth, CDisplaySettings::Get().GetCurrentResolutionInfo().iScreenHeight);
*/
/*
  // if display is set to 1080xxx, then disable deinterlacer for HD content
  // else bandwidth usage is too heavy and it will slow down video decoder.
  char display_mode[256] = {0};
  SysfsUtils::GetString("/sys/class/display/mode", display_mode, 255);
  if (strstr(display_mode,"1080"))
    SysfsUtils::SetInt("/sys/module/di/parameters/bypass_all", 1);
  else
    SysfsUtils::SetInt("/sys/module/di/parameters/bypass_all", 0);
*/

  SetSpeed(m_speed);

  return true;
}

void CLinuxC1Codec::ShowMainVideo(const bool show)
{
  SysfsUtils::SetInt("/sys/class/video/disable_video", show ? 0 : 1);
}

void CLinuxC1Codec::SetSpeed(int speed)
{
  CLog::Log(LOGDEBUG, "CAMLCodec::SetSpeed, speed(%d)", speed);

  m_speed = speed;

  switch(speed)
  {
    case DVD_PLAYSPEED_PAUSE:
      codec_pause(&am_private->vcodec);
      codec_set_cntl_mode(&am_private->vcodec, TRICKMODE_NONE);
      break;
    case DVD_PLAYSPEED_NORMAL:
      codec_resume(&am_private->vcodec);
      codec_set_cntl_mode(&am_private->vcodec, TRICKMODE_NONE);
      break;
    default:
      codec_resume(&am_private->vcodec);
      if ((am_private->video_format == VFORMAT_H264) || (am_private->video_format == VFORMAT_H264_4K2K))
        codec_set_cntl_mode(&am_private->vcodec, TRICKMODE_FFFB);
      else
        codec_set_cntl_mode(&am_private->vcodec, TRICKMODE_I);
      break;
  }
}

bool CLinuxC1Codec::GetPicture(DVDVideoPicture *pDvdVideoPicture)
{
  pDvdVideoPicture->iFlags = DVP_FLAG_ALLOCATED;
  pDvdVideoPicture->format = RENDER_FMT_BYPASS;
  pDvdVideoPicture->iDuration = (double)(am_private->video_rate * DVD_TIME_BASE) / UNIT_FREQ;

  pDvdVideoPicture->dts = DVD_NOPTS_VALUE;
  if (m_speed == DVD_PLAYSPEED_NORMAL)
  {
    pDvdVideoPicture->pts = GetPlayerPtsSeconds() * (double)DVD_TIME_BASE;
    // video pts cannot be late or dvdplayer goes nuts,
    // so run it one frame ahead
    pDvdVideoPicture->pts += 1 * pDvdVideoPicture->iDuration;
  }
  else
  {
    // We are FF/RW; Do not use the Player clock or it just doesn't work
    if (m_cur_pts == 0)
      pDvdVideoPicture->pts = (double)m_1st_pts / PTS_FREQ * DVD_TIME_BASE;
    else
      pDvdVideoPicture->pts = (double)m_cur_pts / PTS_FREQ * DVD_TIME_BASE;
  }

  return true;
}

void CLinuxC1Codec::CloseDecoder() {
  CLog::Log(LOGDEBUG, "CLinuxC1Codec::CloseDecoder");
/*
  g_renderManager.RegisterRenderUpdateCallBack((const void*)NULL, NULL);
  g_renderManager.RegisterRenderFeaturesCallBack((const void*)NULL, NULL);
*/
  // never leave vcodec ff/rw or paused.
  if (m_speed != DVD_PLAYSPEED_NORMAL)
  {
    codec_resume(&am_private->vcodec);
    codec_set_cntl_mode(&am_private->vcodec, TRICKMODE_NONE);
  }
  codec_close(&am_private->vcodec);

  am_packet_release(&am_private->am_pkt);
  free(am_private->extradata);
  am_private->extradata = NULL;
  SysfsUtils::SetInt("/sys/class/tsync/enable", 1);

  ShowMainVideo(false);
  usleep(500 * 1000);
}

double CLinuxC1Codec::GetPlayerPtsSeconds()
{
  double clock_pts = 0.0;
/*
  CDVDClock *playerclock = CDVDClock::GetMasterClock();
  if (playerclock)
    clock_pts = playerclock->GetClock() / DVD_TIME_BASE;
*/
  return clock_pts;
}

int CLinuxC1Codec::Decode(uint8_t *pData, size_t size, double dts, double pts) {

  return VC_PICTURE | VC_BUFFER;

}

void CLinuxC1Codec::Reset() {
  CLog::Log(LOGDEBUG, "CLinuxC1Codec::Reset");
}