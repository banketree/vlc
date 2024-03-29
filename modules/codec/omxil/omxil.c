/*****************************************************************************
 * omxil.c: Video decoder module making use of OpenMAX IL components.
 *****************************************************************************
 * Copyright (C) 2010 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Gildas Bazin <gbazin@videolan.org>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation; either version 2.1 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston MA 02110-1301, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#ifdef HAVE_CONFIG_H
# include "config.h"
#endif

#include <limits.h>

#include <vlc_common.h>
#include <vlc_plugin.h>
#include <vlc_codec.h>
#include <vlc_block_helper.h>
#include <vlc_cpu.h>
#include "../h264_nal.h"
#include "../hevc_nal.h"

#include "omxil.h"
#include "omxil_core.h"
#include "OMX_Broadcom.h"

#if defined(USE_IOMX)
#include <dlfcn.h>
#include <jni.h>
#include "android_opaque.h"
#endif

#ifndef NDEBUG
# define OMX_DBG(...) msg_Dbg( p_dec, __VA_ARGS__ )
#else
# define OMX_DBG(...)
#endif

#define SENTINEL_FLAG 0x10000

/* Defined in the broadcom version of OMX_Index.h */
#define OMX_IndexConfigRequestCallback 0x7f000063
#define OMX_IndexParamBrcmPixelAspectRatio 0x7f00004d
#define OMX_IndexParamBrcmVideoDecodeErrorConcealment 0x7f000080

/* Defined in the broadcom version of OMX_Core.h */
#define OMX_EventParamOrConfigChanged 0x7F000001

#if defined(USE_IOMX)
/* JNI functions to get/set an Android Surface object. */
#define THREAD_NAME "omxil"
extern int jni_attach_thread(JNIEnv **env, const char *thread_name);
extern void jni_detach_thread();
extern jobject jni_LockAndGetAndroidJavaSurface();
extern void jni_UnlockAndroidSurface();
extern void jni_SetAndroidSurfaceSize(int width, int height, int visible_width, int visible_height, int sar_num, int sar_den);
extern bool jni_IsVideoPlayerActivityCreated();
#endif

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  OpenDecoder( vlc_object_t * );
static int  OpenEncoder( vlc_object_t * );
static int  OpenGeneric( vlc_object_t *, bool b_encode );
static void CloseGeneric( vlc_object_t * );

static picture_t *DecodeVideo( decoder_t *, block_t ** );
static block_t *DecodeAudio ( decoder_t *, block_t ** );
static block_t *EncodeVideo( encoder_t *, picture_t * );

static OMX_ERRORTYPE OmxEventHandler( OMX_HANDLETYPE, OMX_PTR, OMX_EVENTTYPE,
                                      OMX_U32, OMX_U32, OMX_PTR );
static OMX_ERRORTYPE OmxEmptyBufferDone( OMX_HANDLETYPE, OMX_PTR,
                                         OMX_BUFFERHEADERTYPE * );
static OMX_ERRORTYPE OmxFillBufferDone( OMX_HANDLETYPE, OMX_PTR,
                                        OMX_BUFFERHEADERTYPE * );

#if defined(USE_IOMX)
static void *DequeueThread( void *data );
static void DisplayCallback( picture_sys_t* p_picsys );
static void UnlockCallback( picture_sys_t* p_picsys );
static void HwBuffer_Init( decoder_t *p_dec, OmxPort *p_port );
static void HwBuffer_Destroy( decoder_t *p_dec, OmxPort *p_port );
static int  HwBuffer_AllocateBuffers( decoder_t *p_dec, OmxPort *p_port );
static int  HwBuffer_FreeBuffers( decoder_t *p_dec, OmxPort *p_port );
static int  HwBuffer_Start( decoder_t *p_dec, OmxPort *p_port );
static int  HwBuffer_Stop( decoder_t *p_dec, OmxPort *p_port );
static int  HwBuffer_Join( decoder_t *p_dec, OmxPort *p_port );
static int  HwBuffer_GetPic( decoder_t *p_dec, OmxPort *p_port,
                             picture_t **pp_pic );
static void HwBuffer_SetCrop( decoder_t *p_dec, OmxPort *p_port,
                              OMX_CONFIG_RECTTYPE *p_rect );
static void HwBuffer_ChangeState( decoder_t *p_dec, OmxPort *p_port,
                                  int i_index, int i_state );

#define HWBUFFER_LOCK() vlc_mutex_lock( get_android_opaque_mutex() )
#define HWBUFFER_UNLOCK() vlc_mutex_unlock( get_android_opaque_mutex() )
#define HWBUFFER_WAIT(p_port) vlc_cond_wait( &(p_port)->p_hwbuf->wait, \
                                              get_android_opaque_mutex() )
#define HWBUFFER_BROADCAST(p_port) vlc_cond_broadcast( &(p_port)->p_hwbuf->wait )

#else
static inline int HwBuffer_dummy( )
{
    return 0;
}
#define HwBuffer_Init(p_dec, p_port) do { } while (0)
#define HwBuffer_Destroy(p_dec, p_port) do { } while (0)
#define HwBuffer_AllocateBuffers(p_dec, p_port) HwBuffer_dummy()
#define HwBuffer_FreeBuffers(p_dec, p_port) HwBuffer_dummy()
#define HwBuffer_Start(p_dec, p_port) HwBuffer_dummy()
#define HwBuffer_Stop(p_dec, p_port) HwBuffer_dummy()
#define HwBuffer_Join(p_dec, p_port) HwBuffer_dummy()
#define HwBuffer_GetPic(p_dec, p_port, pp_pic) HwBuffer_dummy()
#define HwBuffer_SetCrop(p_dec, p_port, p_rect) do { } while (0)

#define HWBUFFER_LOCK() do { } while (0)
#define HWBUFFER_UNLOCK() do { } while (0)
#define HWBUFFER_WAIT(p_port) do { } while (0)
#define HWBUFFER_BROADCAST(p_port) do { } while (0)
#endif

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
#define DIRECTRENDERING_TEXT N_("OMX direct rendering")
#define DIRECTRENDERING_LONGTEXT N_(\
        "Enable OMX direct rendering.")

#define CFG_PREFIX "omxil-"
vlc_module_begin ()
    set_description( N_("Audio/Video decoder (using OpenMAX IL)") )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_VCODEC )
    set_section( N_("Decoding") , NULL )
#if defined(USE_IOMX)
    /* For IOMX, don't enable it automatically via priorities,
     * enable it only via the --codec iomx command line parameter when
     * wanted. */
    set_capability( "decoder", 0 )
    add_bool(CFG_PREFIX "dr", true,
             DIRECTRENDERING_TEXT, DIRECTRENDERING_LONGTEXT, true)
#else
    set_capability( "decoder", 80 )
#endif
    set_callbacks( OpenDecoder, CloseGeneric )

    add_submodule ()
    set_section( N_("Encoding") , NULL )
    set_description( N_("Video encoder (using OpenMAX IL)") )
    set_capability( "encoder", 0 )
    set_callbacks( OpenEncoder, CloseGeneric )
vlc_module_end ()

/*****************************************************************************
 * ImplementationSpecificWorkarounds: place-holder for implementation
 * specific workarounds
 *****************************************************************************/
static OMX_ERRORTYPE ImplementationSpecificWorkarounds(decoder_t *p_dec,
    OmxPort *p_port, es_format_t *p_fmt)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_PARAM_PORTDEFINITIONTYPE *def = &p_port->definition;
    size_t i_profile = 0xFFFF, i_level = 0xFFFF;

    /* Try to find out the profile of the video */
    if(p_fmt->i_cat == VIDEO_ES && def->eDir == OMX_DirInput &&
            p_fmt->i_codec == VLC_CODEC_H264)
        h264_get_profile_level(&p_dec->fmt_in, &i_profile, &i_level, &p_sys->i_nal_size_length);

    if(!strcmp(p_sys->psz_component, "OMX.TI.Video.Decoder"))
    {
        if(p_fmt->i_cat == VIDEO_ES && def->eDir == OMX_DirInput &&
           p_fmt->i_codec == VLC_CODEC_H264 &&
           (i_profile != 66 || i_level > 30))
        {
            msg_Dbg(p_dec, "h264 profile/level not supported (0x%x, 0x%x)",
                    i_profile, i_level);
            return OMX_ErrorNotImplemented;
        }

        if(p_fmt->i_cat == VIDEO_ES && def->eDir == OMX_DirOutput &&
           p_fmt->i_codec == VLC_CODEC_I420)
        {
            /* I420 xvideo is slow on OMAP */
            def->format.video.eColorFormat = OMX_COLOR_FormatCbYCrY;
            GetVlcChromaFormat( def->format.video.eColorFormat,
                                &p_fmt->i_codec, 0 );
            GetVlcChromaSizes( p_fmt->i_codec,
                               def->format.video.nFrameWidth,
                               def->format.video.nFrameHeight,
                               &p_port->i_frame_size, &p_port->i_frame_stride,
                               &p_port->i_frame_stride_chroma_div );
            def->format.video.nStride = p_port->i_frame_stride;
            def->nBufferSize = p_port->i_frame_size;
        }
    }
    else if(!strcmp(p_sys->psz_component, "OMX.st.video_encoder"))
    {
        if(p_fmt->i_cat == VIDEO_ES)
        {
            /* Bellagio's encoder doesn't encode the framerate in Q16 */
            def->format.video.xFramerate >>= 16;
        }
    }
#if 0 /* FIXME: doesn't apply for HP Touchpad */
    else if (!strncmp(p_sys->psz_component, "OMX.qcom.video.decoder.",
                      strlen("OMX.qcom.video.decoder")))
    {
        /* qdsp6 refuses buffer size larger than 450K on input port */
        if (def->nBufferSize > 450 * 1024)
        {
            def->nBufferSize = 450 * 1024;
            p_port->i_frame_size = def->nBufferSize;
        }
    }
#endif
#ifdef RPI_OMX
    else if (!strcmp(p_sys->psz_component, "OMX.broadcom.video_decode"))
    {
        /* Clear these fields before setting parameters, to allow the codec
         * fill in what it wants (instead of rejecting whatever happened to
         * be there. */
        def->format.video.nStride = def->format.video.nSliceHeight = 0;
    }
#endif

    return OMX_ErrorNone;
}

/*****************************************************************************
 * SetPortDefinition: set definition of the omx port based on the vlc format
 *****************************************************************************/
static OMX_ERRORTYPE SetPortDefinition(decoder_t *p_dec, OmxPort *p_port,
                                       es_format_t *p_fmt)
{
    OMX_PARAM_PORTDEFINITIONTYPE *def = &p_port->definition;
    OMX_ERRORTYPE omx_error;

    omx_error = OMX_GetParameter(p_port->omx_handle,
                                 OMX_IndexParamPortDefinition, def);
    CHECK_ERROR(omx_error, "OMX_GetParameter failed (%x : %s)",
                omx_error, ErrorToString(omx_error));

    switch(p_fmt->i_cat)
    {
    case VIDEO_ES:
        def->format.video.nFrameWidth = p_fmt->video.i_width;
        def->format.video.nFrameHeight = p_fmt->video.i_height;
        if(def->format.video.eCompressionFormat == OMX_VIDEO_CodingUnused)
            def->format.video.nStride = def->format.video.nFrameWidth;
        if( p_fmt->video.i_frame_rate > 0 &&
            p_fmt->video.i_frame_rate_base > 0 )
            def->format.video.xFramerate = (p_fmt->video.i_frame_rate << 16) /
                p_fmt->video.i_frame_rate_base;

        if(def->eDir == OMX_DirInput || p_dec->p_sys->b_enc)
        {
            if (def->eDir == OMX_DirInput && p_dec->p_sys->b_enc)
                def->nBufferSize = def->format.video.nFrameWidth *
                  def->format.video.nFrameHeight * 2;
            p_port->i_frame_size = def->nBufferSize;

            if(!GetOmxVideoFormat(p_fmt->i_codec,
                                  &def->format.video.eCompressionFormat, 0) )
            {
                if(!GetOmxChromaFormat(p_fmt->i_codec,
                                       &def->format.video.eColorFormat, 0) )
                {
                    omx_error = OMX_ErrorNotImplemented;
                    CHECK_ERROR(omx_error, "codec %4.4s doesn't match any OMX format",
                                (char *)&p_fmt->i_codec );
                }
                GetVlcChromaSizes( p_fmt->i_codec,
                                   def->format.video.nFrameWidth,
                                   def->format.video.nFrameHeight,
                                   &p_port->i_frame_size, &p_port->i_frame_stride,
                                   &p_port->i_frame_stride_chroma_div );
                def->format.video.nStride = p_port->i_frame_stride;
                def->nBufferSize = p_port->i_frame_size;
            }
        }
        else
        {
            if( p_port->p_hwbuf )
            {
                p_fmt->i_codec = VLC_CODEC_ANDROID_OPAQUE;
                break;
            }

            if( !GetVlcChromaFormat( def->format.video.eColorFormat,
                                     &p_fmt->i_codec, 0 ) )
            {
                omx_error = OMX_ErrorNotImplemented;
                CHECK_ERROR(omx_error, "OMX color format %i not supported",
                            (int)def->format.video.eColorFormat );
            }
            GetVlcChromaSizes( p_fmt->i_codec,
                               def->format.video.nFrameWidth,
                               def->format.video.nFrameHeight,
                               &p_port->i_frame_size, &p_port->i_frame_stride,
                               &p_port->i_frame_stride_chroma_div );
            def->format.video.nStride = p_port->i_frame_stride;
            if (p_port->i_frame_size > def->nBufferSize)
                def->nBufferSize = p_port->i_frame_size;
        }
        break;

    case AUDIO_ES:
        p_port->i_frame_size = def->nBufferSize;
        if(def->eDir == OMX_DirInput)
        {
            if(!GetOmxAudioFormat(p_fmt->i_codec,
                                  &def->format.audio.eEncoding, 0) )
            {
                omx_error = OMX_ErrorNotImplemented;
                CHECK_ERROR(omx_error, "codec %4.4s doesn't match any OMX format",
                            (char *)&p_fmt->i_codec );
            }
        }
        else
        {
            if( !OmxToVlcAudioFormat(def->format.audio.eEncoding,
                                   &p_fmt->i_codec, 0 ) )
            {
                omx_error = OMX_ErrorNotImplemented;
                CHECK_ERROR(omx_error, "OMX audio encoding %i not supported",
                            (int)def->format.audio.eEncoding );
            }
        }
        break;

    default: return OMX_ErrorNotImplemented;
    }

    omx_error = ImplementationSpecificWorkarounds(p_dec, p_port, p_fmt);
    CHECK_ERROR(omx_error, "ImplementationSpecificWorkarounds failed (%x : %s)",
                omx_error, ErrorToString(omx_error));

    omx_error = OMX_SetParameter(p_port->omx_handle,
                                 OMX_IndexParamPortDefinition, def);
    CHECK_ERROR(omx_error, "OMX_SetParameter failed (%x : %s)",
                omx_error, ErrorToString(omx_error));

    omx_error = OMX_GetParameter(p_port->omx_handle,
                                 OMX_IndexParamPortDefinition, def);
    CHECK_ERROR(omx_error, "OMX_GetParameter failed (%x : %s)",
                omx_error, ErrorToString(omx_error));

    if(p_port->i_frame_size > def->nBufferSize)
        def->nBufferSize = p_port->i_frame_size;
    p_port->i_frame_size = def->nBufferSize;

    /* Deal with audio params */
    if(p_fmt->i_cat == AUDIO_ES)
    {
        omx_error = SetAudioParameters(p_port->omx_handle,
                                       &p_port->format_param, def->nPortIndex,
                                       def->format.audio.eEncoding,
                                       p_fmt->i_codec,
                                       p_fmt->audio.i_channels,
                                       p_fmt->audio.i_rate,
                                       p_fmt->i_bitrate,
                                       p_fmt->audio.i_bitspersample,
                                       p_fmt->audio.i_blockalign);
        if (def->eDir == OMX_DirInput) {
            CHECK_ERROR(omx_error, "SetAudioParameters failed (%x : %s)",
                        omx_error, ErrorToString(omx_error));
        } else if (omx_error != OMX_ErrorNone) {
            msg_Warn(p_dec, "SetAudioParameters failed (%x : %s) on output port",
                     omx_error, ErrorToString(omx_error));
            omx_error = OMX_ErrorNone;
        }
    }
    if (!strcmp(p_dec->p_sys->psz_component, "OMX.TI.DUCATI1.VIDEO.DECODER") &&
                def->eDir == OMX_DirOutput && !p_port->p_hwbuf)
    {
        /* When setting the output buffer size above, the decoder actually
         * sets the buffer size to a lower value than what was chosen. If
         * we try to allocate buffers of this size, it fails. Thus, forcibly
         * use a larger buffer size. */
        def->nBufferSize *= 2;
    }

 error:
    return omx_error;
}


/*****************************************************************************
 * UpdatePixelAspect: Update vlc pixel aspect based on the aspect reported on
 * the omx port - NOTE: Broadcom specific
 *****************************************************************************/
static OMX_ERRORTYPE UpdatePixelAspect(decoder_t *p_dec)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_CONFIG_POINTTYPE pixel_aspect;
    OMX_INIT_STRUCTURE(pixel_aspect);
    OMX_ERRORTYPE omx_err;

    if (strncmp(p_sys->psz_component, "OMX.broadcom.", 13))
        return OMX_ErrorNotImplemented;

    pixel_aspect.nPortIndex = p_sys->out.i_port_index;
    omx_err = OMX_GetParameter(p_sys->omx_handle,
            OMX_IndexParamBrcmPixelAspectRatio, &pixel_aspect);
    if (omx_err != OMX_ErrorNone) {
        msg_Warn(p_dec, "Failed to retrieve aspect ratio");
    } else {
        p_dec->fmt_out.video.i_sar_num = pixel_aspect.nX;
        p_dec->fmt_out.video.i_sar_den = pixel_aspect.nY;
    }

    return omx_err;
}

/*****************************************************************************
 * AllocateBuffers: Allocate Omx buffers
 *****************************************************************************/
static OMX_ERRORTYPE AllocateBuffers(decoder_t *p_dec, OmxPort *p_port)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_ERRORTYPE omx_error = OMX_ErrorUndefined;
    OMX_PARAM_PORTDEFINITIONTYPE *def = &p_port->definition;
    unsigned int i = 0;

    OMX_DBG( "AllocateBuffers(%d)", def->eDir );

    p_port->i_buffers = p_port->definition.nBufferCountActual;

    p_port->pp_buffers = calloc(p_port->i_buffers, sizeof(OMX_BUFFERHEADERTYPE*));
    if( !p_port->pp_buffers )
    {
        p_port->i_buffers = 0;
        return OMX_ErrorInsufficientResources;
    }

    for(i = 0; i < p_port->i_buffers; i++)
    {
#if 0
#define ALIGN(x,BLOCKLIGN) (((x) + BLOCKLIGN - 1) & ~(BLOCKLIGN - 1))
        char *p_buf = malloc(p_port->definition.nBufferSize +
                             p_port->definition.nBufferAlignment);
        p_port->pp_buffers[i] = (void *)ALIGN((uintptr_t)p_buf, p_port->definition.nBufferAlignment);
#endif

        if( p_port->p_hwbuf )
        {
            omx_error =
                OMX_UseBuffer( p_sys->omx_handle, &p_port->pp_buffers[i],
                               p_port->i_port_index, 0,
                               p_port->definition.nBufferSize,
                               p_port->p_hwbuf->pp_handles[i] );
            OMX_DBG( "OMX_UseBuffer(%d) %p, %p", def->eDir,
                     p_port->pp_buffers[i], p_port->p_hwbuf->pp_handles[i] );
        }
        else if( p_port->b_direct )
        {
            omx_error =
                OMX_UseBuffer( p_sys->omx_handle, &p_port->pp_buffers[i],
                               p_port->i_port_index, 0,
                               p_port->definition.nBufferSize, (void*)1);
            OMX_DBG( "OMX_UseBuffer(%d) %p, %p", def->eDir,
                     p_port->pp_buffers[i], p_port->pp_buffers[i] ?
                     p_port->pp_buffers[i]->pBuffer : NULL );
        }
        else
        {
            omx_error =
                OMX_AllocateBuffer( p_sys->omx_handle, &p_port->pp_buffers[i],
                                    p_port->i_port_index, 0,
                                    p_port->definition.nBufferSize);
            OMX_DBG( "OMX_AllocateBuffer(%d) %p, %p", def->eDir,
                     p_port->pp_buffers[i], p_port->pp_buffers[i] ? 
                     p_port->pp_buffers[i]->pBuffer : NULL );
        }

        if(omx_error != OMX_ErrorNone)
        {
            p_port->i_buffers = i;
            break;
        }
        if( !p_port->p_hwbuf )
            OMX_FIFO_PUT(&p_port->fifo, p_port->pp_buffers[i]);
    }

    CHECK_ERROR(omx_error, "AllocateBuffers failed (%x, %i)",
                omx_error, (int)p_port->i_port_index );


    OMX_DBG( "AllocateBuffers(%d)::done", def->eDir );
error:
    return omx_error;
}

/*****************************************************************************
 * FreeBuffers: Free Omx buffers
 *****************************************************************************/
static OMX_ERRORTYPE FreeBuffers(decoder_t *p_dec, OmxPort *p_port)
{
    OMX_PARAM_PORTDEFINITIONTYPE *def = &p_port->definition;
    OMX_ERRORTYPE omx_error = OMX_ErrorNone;
    OMX_BUFFERHEADERTYPE *p_buffer;
    unsigned int i, i_wait_buffers;

    /* Normally, all buffers are in the port fifo, or given to the codec that
     * will return them when disabling the port or changing state, therefore we
     * normally wait for all buffers. For IOMX direct rendering (HwBuffer),
     * only a few buffers are given to the codec at a time, thus we can only
     * wait for that many buffers. And after that, we can still free all OMX
     * buffers since we either got some of them returned via OMX_FIFO_GET, or
     * never passed them to the codec at all. */
    if( p_port->p_hwbuf )
        i_wait_buffers = p_port->p_hwbuf->i_owned;
    else
        i_wait_buffers = p_port->i_buffers;

    OMX_DBG( "FreeBuffers(%d), waiting for %u buffers", def->eDir,
             i_wait_buffers);

    for(i = 0; i < i_wait_buffers; i++)
    {
        OMX_FIFO_GET(&p_port->fifo, p_buffer);
        if (p_buffer->nFlags & SENTINEL_FLAG) {
            free(p_buffer);
            i--;
            continue;
        }
    }

    for(i = 0; i < p_port->i_buffers; i++)
    {
        p_buffer = p_port->pp_buffers[i];
        if( p_buffer )
        {
            if (p_buffer->pAppPrivate != NULL)
                decoder_DeletePicture( p_dec, p_buffer->pAppPrivate );

            omx_error = OMX_FreeBuffer( p_port->omx_handle,
                                        p_port->i_port_index, p_buffer );
            OMX_DBG( "OMX_FreeBuffer(%d) %p, %p", def->eDir,
                     p_buffer, p_buffer->pBuffer );

            if(omx_error != OMX_ErrorNone) break;
        }
    }

    if( omx_error != OMX_ErrorNone )
       msg_Err( p_dec, "OMX_FreeBuffer failed (%x, %i, %i)",
                omx_error, (int)p_port->i_port_index, i );

    p_port->i_buffers = 0;
    free( p_port->pp_buffers );
    p_port->pp_buffers = NULL;

    OMX_DBG( "FreeBuffers(%d)::done", def->eDir );

    return omx_error;
}

/*****************************************************************************
 * GetPortDefinition: set vlc format based on the definition of the omx port
 *****************************************************************************/
static OMX_ERRORTYPE GetPortDefinition(decoder_t *p_dec, OmxPort *p_port,
                                       es_format_t *p_fmt)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_PARAM_PORTDEFINITIONTYPE *def = &p_port->definition;
    OMX_ERRORTYPE omx_error;
    OMX_CONFIG_RECTTYPE crop_rect;

    omx_error = OMX_GetParameter(p_port->omx_handle,
                                 OMX_IndexParamPortDefinition, def);
    CHECK_ERROR(omx_error, "OMX_GetParameter failed (%x : %s)",
                omx_error, ErrorToString(omx_error));

    switch(p_fmt->i_cat)
    {
    case VIDEO_ES:
        p_fmt->video.i_width = def->format.video.nFrameWidth;
        p_fmt->video.i_visible_width = def->format.video.nFrameWidth;
        p_fmt->video.i_height = def->format.video.nFrameHeight;
        p_fmt->video.i_visible_height = def->format.video.nFrameHeight;
        p_fmt->video.i_frame_rate = p_dec->fmt_in.video.i_frame_rate;
        p_fmt->video.i_frame_rate_base = p_dec->fmt_in.video.i_frame_rate_base;

        OMX_INIT_STRUCTURE(crop_rect);
        crop_rect.nPortIndex = def->nPortIndex;
        omx_error = OMX_GetConfig(p_port->omx_handle, OMX_IndexConfigCommonOutputCrop, &crop_rect);
        if (omx_error == OMX_ErrorNone)
        {
            if (!def->format.video.nSliceHeight)
                def->format.video.nSliceHeight = def->format.video.nFrameHeight;
            if (!def->format.video.nStride)
                def->format.video.nStride = def->format.video.nFrameWidth;
            p_fmt->video.i_width = crop_rect.nWidth;
            p_fmt->video.i_visible_width = crop_rect.nWidth;
            p_fmt->video.i_height = crop_rect.nHeight;
            p_fmt->video.i_visible_height = crop_rect.nHeight;
            if (def->format.video.eColorFormat == OMX_TI_COLOR_FormatYUV420PackedSemiPlanar)
                def->format.video.nSliceHeight -= crop_rect.nTop/2;

            if( p_port->p_hwbuf )
                HwBuffer_SetCrop( p_dec, p_port, &crop_rect );
        }
        else
        {
            /* Don't pass the error back to the caller, this isn't mandatory */
            omx_error = OMX_ErrorNone;
        }

        if( p_port->p_hwbuf )
        {
            UpdatePixelAspect(p_dec);
            break;
        }
        /* Hack: Nexus One (stock firmware with binary OMX driver blob)
         * claims to output 420Planar even though it in in practice is
         * NV21. */
        if(def->format.video.eColorFormat == OMX_COLOR_FormatYUV420Planar &&
           !strncmp(p_sys->psz_component, "OMX.qcom.video.decoder",
                    strlen("OMX.qcom.video.decoder")))
            def->format.video.eColorFormat = OMX_QCOM_COLOR_FormatYVU420SemiPlanar;

        if (IgnoreOmxDecoderPadding(p_sys->psz_component)) {
            def->format.video.nSliceHeight = 0;
            def->format.video.nStride = p_fmt->video.i_width;
        }

        if(!GetVlcVideoFormat( def->format.video.eCompressionFormat,
                               &p_fmt->i_codec, 0 ) )
        {
            if( !GetVlcChromaFormat( def->format.video.eColorFormat,
                                     &p_fmt->i_codec, 0 ) )
            {
                omx_error = OMX_ErrorNotImplemented;
                CHECK_ERROR(omx_error, "OMX color format %i not supported",
                            (int)def->format.video.eColorFormat );
            }
            GetVlcChromaSizes( p_fmt->i_codec,
                               def->format.video.nFrameWidth,
                               def->format.video.nFrameHeight,
                               &p_port->i_frame_size, &p_port->i_frame_stride,
                               &p_port->i_frame_stride_chroma_div );
        }
        if(p_port->i_frame_size > def->nBufferSize)
            def->nBufferSize = p_port->i_frame_size;
        p_port->i_frame_size = def->nBufferSize;
#if 0
        if((int)p_port->i_frame_stride > def->format.video.nStride)
            def->format.video.nStride = p_port->i_frame_stride;
#endif
        p_port->i_frame_stride = def->format.video.nStride;
        UpdatePixelAspect(p_dec);
        break;

    case AUDIO_ES:
        if( !OmxToVlcAudioFormat( def->format.audio.eEncoding,
                                &p_fmt->i_codec, 0 ) )
        {
            omx_error = OMX_ErrorNotImplemented;
            CHECK_ERROR(omx_error, "OMX audio format %i not supported",
                        (int)def->format.audio.eEncoding );
        }

        omx_error = GetAudioParameters(p_port->omx_handle,
                                       &p_port->format_param, def->nPortIndex,
                                       def->format.audio.eEncoding,
                                       &p_fmt->audio.i_channels,
                                       &p_fmt->audio.i_rate,
                                       &p_fmt->i_bitrate,
                                       &p_fmt->audio.i_bitspersample,
                                       &p_fmt->audio.i_blockalign);
        CHECK_ERROR(omx_error, "GetAudioParameters failed (%x : %s)",
                    omx_error, ErrorToString(omx_error));

        if(p_fmt->audio.i_channels < 9)
        {
            static const int pi_channels_maps[9] =
            {
                0, AOUT_CHAN_CENTER, AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT,
                AOUT_CHAN_CENTER | AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT,
                AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_REARLEFT
                | AOUT_CHAN_REARRIGHT,
                AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
                | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT,
                AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
                | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT | AOUT_CHAN_LFE,
                AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER
                | AOUT_CHAN_REARLEFT | AOUT_CHAN_REARRIGHT | AOUT_CHAN_MIDDLELEFT
                | AOUT_CHAN_MIDDLERIGHT,
                AOUT_CHAN_LEFT | AOUT_CHAN_RIGHT | AOUT_CHAN_CENTER | AOUT_CHAN_REARLEFT
                | AOUT_CHAN_REARRIGHT | AOUT_CHAN_MIDDLELEFT | AOUT_CHAN_MIDDLERIGHT
                | AOUT_CHAN_LFE
            };
            p_fmt->audio.i_physical_channels =
                p_fmt->audio.i_original_channels =
                    pi_channels_maps[p_fmt->audio.i_channels];
        }

        date_Init( &p_dec->p_sys->end_date, p_fmt->audio.i_rate, 1 );

        break;

    default: return OMX_ErrorNotImplemented;
    }

 error:
    return omx_error;
}

/*****************************************************************************
 * DeinitialiseComponent: Deinitialise and unload an OMX component
 *****************************************************************************/
static OMX_ERRORTYPE DeinitialiseComponent(decoder_t *p_dec,
                                           OMX_HANDLETYPE omx_handle)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_BUFFERHEADERTYPE *p_buffer;
    OMX_ERRORTYPE omx_error;
    OMX_STATETYPE state;
    unsigned int i;

    if(!omx_handle) return OMX_ErrorNone;

    omx_error = OMX_GetState(omx_handle, &state);
    CHECK_ERROR(omx_error, "OMX_GetState failed (%x)", omx_error );

    if( p_sys->out.p_hwbuf && HwBuffer_Stop( p_dec, &p_sys->out ) != 0 )
        msg_Warn( p_dec, "HwBuffer_Stop failed" );

    if(state == OMX_StateExecuting)
    {
        omx_error = OMX_SendCommand( omx_handle, OMX_CommandStateSet,
                                     OMX_StateIdle, 0 );
        CHECK_ERROR(omx_error, "OMX_CommandStateSet Idle failed (%x)", omx_error );
        while (1) {
            OMX_U32 cmd, state;
            omx_error = WaitForSpecificOmxEvent(&p_sys->event_queue, OMX_EventCmdComplete, &cmd, &state, 0);
            CHECK_ERROR(omx_error, "Wait for Idle failed (%x)", omx_error );
            // The event queue can contain other OMX_EventCmdComplete items,
            // such as for OMX_CommandFlush
            if (cmd == OMX_CommandStateSet && state == OMX_StateIdle)
                break;
        }
    }

    omx_error = OMX_GetState(omx_handle, &state);
    CHECK_ERROR(omx_error, "OMX_GetState failed (%x)", omx_error );

    if(state == OMX_StateIdle)
    {
        omx_error = OMX_SendCommand( omx_handle, OMX_CommandStateSet,
                                     OMX_StateLoaded, 0 );
        CHECK_ERROR(omx_error, "OMX_CommandStateSet Loaded failed (%x)", omx_error );

        for(i = 0; i < p_sys->ports; i++)
        {
            OmxPort *p_port = &p_sys->p_ports[i];

            omx_error = FreeBuffers( p_dec, p_port );
            CHECK_ERROR(omx_error, "FreeBuffers failed (%x, %i)",
                        omx_error, (int)p_port->i_port_index );
            if( p_port->p_hwbuf )
            {
                HwBuffer_FreeBuffers( p_dec, p_port );
                HwBuffer_Join( p_dec, p_port );
            }
        }

        omx_error = WaitForSpecificOmxEvent(&p_sys->event_queue, OMX_EventCmdComplete, 0, 0, 0);
        CHECK_ERROR(omx_error, "Wait for Loaded failed (%x)", omx_error );
    }

 error:
    for(i = 0; i < p_sys->ports; i++)
    {
        OmxPort *p_port = &p_sys->p_ports[i];
        free(p_port->pp_buffers);
        p_port->pp_buffers = 0;

        while (1) {
            OMX_FIFO_PEEK(&p_port->fifo, p_buffer);
            if (!p_buffer) break;

            OMX_FIFO_GET(&p_port->fifo, p_buffer);
            if (p_buffer->nFlags & SENTINEL_FLAG) {
                free(p_buffer);
                continue;
            }
            msg_Warn( p_dec, "Stray buffer left in fifo, %p", p_buffer );
        }
        HwBuffer_Destroy( p_dec, p_port );
    }
    omx_error = pf_free_handle( omx_handle );
    return omx_error;
}

/*****************************************************************************
 * InitialiseComponent: Load and initialise an OMX component
 *****************************************************************************/
static OMX_ERRORTYPE InitialiseComponent(decoder_t *p_dec,
    OMX_STRING psz_component, OMX_HANDLETYPE *p_handle)
{
    static OMX_CALLBACKTYPE callbacks =
        { OmxEventHandler, OmxEmptyBufferDone, OmxFillBufferDone };
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_HANDLETYPE omx_handle;
    OMX_ERRORTYPE omx_error;
    unsigned int i;
    OMX_U8 psz_role[OMX_MAX_STRINGNAME_SIZE];
    OMX_PARAM_COMPONENTROLETYPE role;
    OMX_PARAM_PORTDEFINITIONTYPE definition;
    OMX_PORT_PARAM_TYPE param;

    /* Load component */
    omx_error = pf_get_handle( &omx_handle, psz_component, p_dec, &callbacks );
    if(omx_error != OMX_ErrorNone)
    {
        msg_Warn( p_dec, "OMX_GetHandle(%s) failed (%x: %s)", psz_component,
                  omx_error, ErrorToString(omx_error) );
        return omx_error;
    }
    strncpy(p_sys->psz_component, psz_component, OMX_MAX_STRINGNAME_SIZE-1);

    omx_error = OMX_ComponentRoleEnum(omx_handle, psz_role, 0);
    if(omx_error == OMX_ErrorNone)
        msg_Dbg(p_dec, "loaded component %s of role %s", psz_component, psz_role);
    else
        msg_Dbg(p_dec, "loaded component %s", psz_component);
    PrintOmx(p_dec, omx_handle, OMX_ALL);

    /* Set component role */
    OMX_INIT_STRUCTURE(role);
    strcpy((char*)role.cRole,
           GetOmxRole(p_sys->b_enc ? p_dec->fmt_out.i_codec : p_dec->fmt_in.i_codec,
                      p_dec->fmt_in.i_cat, p_sys->b_enc));

    omx_error = OMX_SetParameter(omx_handle, OMX_IndexParamStandardComponentRole,
                                 &role);
    omx_error = OMX_GetParameter(omx_handle, OMX_IndexParamStandardComponentRole,
                                 &role);
    if(omx_error == OMX_ErrorNone)
        msg_Dbg(p_dec, "component standard role set to %s", role.cRole);

    /* Find the input / output ports */
    OMX_INIT_STRUCTURE(param);
    OMX_INIT_STRUCTURE(definition);
    omx_error = OMX_GetParameter(omx_handle, p_dec->fmt_in.i_cat == VIDEO_ES ?
                                 OMX_IndexParamVideoInit : OMX_IndexParamAudioInit, &param);
    if(omx_error != OMX_ErrorNone) {
#ifdef __ANDROID__
        param.nPorts = 2;
        param.nStartPortNumber = 0;
#else
        param.nPorts = 0;
#endif
    }

    for(i = 0; i < param.nPorts; i++)
    {
        OmxPort *p_port;

        /* Get port definition */
        definition.nPortIndex = param.nStartPortNumber + i;
        omx_error = OMX_GetParameter(omx_handle, OMX_IndexParamPortDefinition,
                                     &definition);
        if(omx_error != OMX_ErrorNone) continue;

        if(definition.eDir == OMX_DirInput) p_port = &p_sys->in;
        else  p_port = &p_sys->out;

        p_port->b_valid = true;
        p_port->i_port_index = definition.nPortIndex;
        p_port->definition = definition;
        p_port->omx_handle = omx_handle;
        HwBuffer_Init( p_dec, p_port );
    }

    if(!p_sys->in.b_valid || !p_sys->out.b_valid)
    {
        omx_error = OMX_ErrorInvalidComponent;
        CHECK_ERROR(omx_error, "couldn't find an input and output port");
    }

    if( !p_sys->out.p_hwbuf && !strncmp(p_sys->psz_component, "OMX.SEC.", 8) &&
       p_dec->fmt_in.i_cat == VIDEO_ES )
    {
        OMX_INDEXTYPE index;
        omx_error = OMX_GetExtensionIndex(omx_handle, (OMX_STRING) "OMX.SEC.index.ThumbnailMode", &index);
        if(omx_error == OMX_ErrorNone)
        {
            OMX_BOOL enable = OMX_TRUE;
            omx_error = OMX_SetConfig(omx_handle, index, &enable);
            CHECK_ERROR(omx_error, "Unable to set ThumbnailMode");
        } else {
            OMX_BOOL enable = OMX_TRUE;
            /* Needed on Samsung Galaxy S II */
            omx_error = OMX_SetConfig(omx_handle, OMX_IndexVendorSetYUV420pMode, &enable);
            if (omx_error == OMX_ErrorNone)
                msg_Dbg(p_dec, "Set OMX_IndexVendorSetYUV420pMode successfully");
            else
                msg_Dbg(p_dec, "Unable to set OMX_IndexVendorSetYUV420pMode: %x", omx_error);
        }
    }

    if(!strncmp(p_sys->psz_component, "OMX.broadcom.", 13))
    {
        OMX_CONFIG_REQUESTCALLBACKTYPE notifications;
        OMX_INIT_STRUCTURE(notifications);

        notifications.nPortIndex = p_sys->out.i_port_index;
        notifications.nIndex = OMX_IndexParamBrcmPixelAspectRatio;
        notifications.bEnable = OMX_TRUE;

        omx_error = OMX_SetParameter(omx_handle,
                OMX_IndexConfigRequestCallback, &notifications);
        if (omx_error == OMX_ErrorNone) {
            msg_Dbg(p_dec, "Enabled aspect ratio notifications");
            p_sys->b_aspect_ratio_handled = true;
        } else
            msg_Dbg(p_dec, "Could not enable aspect ratio notifications");
    }

    /* Set port definitions */
    for(i = 0; i < p_sys->ports; i++)
    {
        omx_error = SetPortDefinition(p_dec, &p_sys->p_ports[i],
                                      p_sys->p_ports[i].p_fmt);
        if(omx_error != OMX_ErrorNone) goto error;
    }

    if(!strncmp(p_sys->psz_component, "OMX.broadcom.", 13) &&
        p_sys->in.p_fmt->i_codec == VLC_CODEC_H264)
    {
        OMX_PARAM_BRCMVIDEODECODEERRORCONCEALMENTTYPE concanParam;
        OMX_INIT_STRUCTURE(concanParam);
        concanParam.bStartWithValidFrame = OMX_FALSE;

        omx_error = OMX_SetParameter(omx_handle,
                OMX_IndexParamBrcmVideoDecodeErrorConcealment, &concanParam);
        if (omx_error == OMX_ErrorNone)
            msg_Dbg(p_dec, "StartWithValidFrame disabled.");
        else
            msg_Dbg(p_dec, "Could not disable StartWithValidFrame.");
    }

    /* Allocate our array for the omx buffers and enable ports */
    for(i = 0; i < p_sys->ports; i++)
    {
        OmxPort *p_port = &p_sys->p_ports[i];

        /* Enable port */
        if(!p_port->definition.bEnabled)
        {
            omx_error = OMX_SendCommand( omx_handle, OMX_CommandPortEnable,
                                         p_port->i_port_index, NULL);
            CHECK_ERROR(omx_error, "OMX_CommandPortEnable on %i failed (%x)",
                        (int)p_port->i_port_index, omx_error );
            omx_error = WaitForSpecificOmxEvent(&p_sys->event_queue, OMX_EventCmdComplete, 0, 0, 0);
            CHECK_ERROR(omx_error, "Wait for PortEnable on %i failed (%x)",
                        (int)p_port->i_port_index, omx_error );
        }
    }

    *p_handle = omx_handle;
    return OMX_ErrorNone;

 error:
    DeinitialiseComponent(p_dec, omx_handle);
    *p_handle = 0;
    return omx_error;
}

/*****************************************************************************
 * OpenDecoder: Create the decoder instance
 *****************************************************************************/
static int OpenDecoder( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    int status;

#ifdef __ANDROID__
    if( p_dec->fmt_in.i_cat == AUDIO_ES )
        return VLC_EGENERIC;
#endif

    if( 0 || !GetOmxRole(p_dec->fmt_in.i_codec, p_dec->fmt_in.i_cat, false) )
        return VLC_EGENERIC;

    status = OpenGeneric( p_this, false );
    if(status != VLC_SUCCESS) return status;

    p_dec->pf_decode_video = DecodeVideo;
    p_dec->pf_decode_audio = DecodeAudio;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * OpenEncoder: Create the encoder instance
 *****************************************************************************/
static int OpenEncoder( vlc_object_t *p_this )
{
    encoder_t *p_enc = (encoder_t*)p_this;
    int status;

    if( !GetOmxRole(p_enc->fmt_out.i_codec, p_enc->fmt_in.i_cat, true) )
        return VLC_EGENERIC;

    status = OpenGeneric( p_this, true );
    if(status != VLC_SUCCESS) return status;

    p_enc->pf_encode_video = EncodeVideo;

    return VLC_SUCCESS;
}

/*****************************************************************************
 * OpenGeneric: Create the generic decoder/encoder instance
 *****************************************************************************/
static int OpenGeneric( vlc_object_t *p_this, bool b_encode )
{
    decoder_t *p_dec = (decoder_t*)p_this;
    decoder_sys_t *p_sys;
    OMX_ERRORTYPE omx_error;
    OMX_BUFFERHEADERTYPE *p_header;
    unsigned int i;

    if (InitOmxCore(p_this) != VLC_SUCCESS) {
        return VLC_EGENERIC;
    }

    /* Allocate the memory needed to store the decoder's structure */
    if( ( p_dec->p_sys = p_sys = calloc( 1, sizeof(*p_sys)) ) == NULL )
    {
        DeinitOmxCore();
        return VLC_ENOMEM;
    }

    /* Initialise the thread properties */
    if(!b_encode)
    {
        p_dec->fmt_out.i_cat = p_dec->fmt_in.i_cat;
        p_dec->fmt_out.video = p_dec->fmt_in.video;
        p_dec->fmt_out.audio = p_dec->fmt_in.audio;
        p_dec->fmt_out.i_codec = 0;

        /* set default aspect of 1, if parser did not set it */
        if (p_dec->fmt_out.video.i_sar_num == 0)
            p_dec->fmt_out.video.i_sar_num = 1;
        if (p_dec->fmt_out.video.i_sar_den == 0)
            p_dec->fmt_out.video.i_sar_den = 1;
    }
    p_sys->b_enc = b_encode;
    InitOmxEventQueue(&p_sys->event_queue);
    OMX_FIFO_INIT (&p_sys->in.fifo, pOutputPortPrivate );
    p_sys->in.b_direct = false;
    p_sys->in.b_flushed = true;
    p_sys->in.p_fmt = &p_dec->fmt_in;
    OMX_FIFO_INIT (&p_sys->out.fifo, pInputPortPrivate );
#if defined(USE_IOMX)
    p_sys->out.b_direct = jni_IsVideoPlayerActivityCreated() && var_InheritBool(p_dec, CFG_PREFIX "dr");
#else
    p_sys->out.b_direct = false;
#endif
    p_sys->out.b_flushed = true;
    p_sys->out.p_fmt = &p_dec->fmt_out;
    p_sys->ports = 2;
    p_sys->p_ports = &p_sys->in;
    p_sys->b_use_pts = 1;

    msg_Dbg(p_dec, "fmt in:%4.4s, out: %4.4s", (char *)&p_dec->fmt_in.i_codec,
            (char *)&p_dec->fmt_out.i_codec);

    /* Enumerate components and build a list of the one we want to try */
    p_sys->components =
        CreateComponentsList(p_this,
             GetOmxRole(p_sys->b_enc ? p_dec->fmt_out.i_codec :
                        p_dec->fmt_in.i_codec, p_dec->fmt_in.i_cat,
                        p_sys->b_enc), p_sys->ppsz_components);
    if( !p_sys->components )
    {
        msg_Warn( p_this, "couldn't find an omx component for codec %4.4s",
                  (char *)&p_dec->fmt_in.i_codec );
        CloseGeneric(p_this);
        return VLC_EGENERIC;
    }

    /* Try to load and initialise a component */
    omx_error = OMX_ErrorUndefined;
    for(i = 0; i < p_sys->components; i++)
    {
#ifdef __ANDROID__
        /* ignore OpenCore software codecs */
        if (!strncmp(p_sys->ppsz_components[i], "OMX.PV.", 7))
            continue;
        /* The same sw codecs, renamed in ICS (perhaps also in honeycomb) */
        if (!strncmp(p_sys->ppsz_components[i], "OMX.google.", 11))
            continue;
        /* This one has been seen on HTC One V - it behaves like it works,
         * but FillBufferDone returns buffers filled with 0 bytes. The One V
         * has got a working OMX.qcom.video.decoder.avc instead though. */
        if (!strncmp(p_sys->ppsz_components[i], "OMX.ARICENT.", 12))
            continue;
        /* Codecs with DRM, that don't output plain YUV data but only
         * support direct rendering where the output can't be intercepted. */
        if (strstr(p_sys->ppsz_components[i], ".secure"))
            continue;
        /* Use VC1 decoder for WMV3 for now */
        if (!strcmp(p_sys->ppsz_components[i], "OMX.SEC.WMV.Decoder"))
            continue;
        /* This decoder does work, but has an insane latency (leading to errors
         * about "main audio output playback way too late" and dropped frames).
         * At least Samsung Galaxy S III (where this decoder is present) has
         * got another one, OMX.SEC.mp3.dec, that works well and has a
         * sensible latency. (Also, even if that one isn't found, in general,
         * using SW codecs is usually more than fast enough for MP3.) */
        if (!strcmp(p_sys->ppsz_components[i], "OMX.SEC.MP3.Decoder"))
            continue;
        /* This codec should be able to handle both VC1 and WMV3, but
         * for VC1 it doesn't output any buffers at all (in the way we use
         * it) and for WMV3 it outputs plain black buffers. Thus ignore
         * it until we can make it work properly. */
        if (!strcmp(p_sys->ppsz_components[i], "OMX.Nvidia.vc1.decode"))
            continue;
#endif
        omx_error = InitialiseComponent(p_dec, p_sys->ppsz_components[i],
                                        &p_sys->omx_handle);
        if(omx_error == OMX_ErrorNone) break;
    }
    CHECK_ERROR(omx_error, "no component could be initialised" );

    /* Move component to Idle then Executing state */
    OMX_SendCommand( p_sys->omx_handle, OMX_CommandStateSet, OMX_StateIdle, 0 );
    CHECK_ERROR(omx_error, "OMX_CommandStateSet Idle failed (%x)", omx_error );

    /* Allocate omx buffers */
    for(i = 0; i < p_sys->ports; i++)
    {
        OmxPort *p_port = &p_sys->p_ports[i];
        if( p_port->p_hwbuf )
        {
            if( HwBuffer_AllocateBuffers( p_dec, p_port ) != 0 )
            {
                omx_error = OMX_ErrorInsufficientResources;
                goto error;
            }
        }
        omx_error = AllocateBuffers( p_dec, p_port );
        CHECK_ERROR(omx_error, "AllocateBuffers failed (%x, %i)",
                    omx_error, (int)p_port->i_port_index );
    }

    omx_error = WaitForSpecificOmxEvent(&p_sys->event_queue, OMX_EventCmdComplete, 0, 0, 0);
    CHECK_ERROR(omx_error, "Wait for Idle failed (%x)", omx_error );

    omx_error = OMX_SendCommand( p_sys->omx_handle, OMX_CommandStateSet,
                                 OMX_StateExecuting, 0);
    CHECK_ERROR(omx_error, "OMX_CommandStateSet Executing failed (%x)", omx_error );
    omx_error = WaitForSpecificOmxEvent(&p_sys->event_queue, OMX_EventCmdComplete, 0, 0, 0);
    CHECK_ERROR(omx_error, "Wait for Executing failed (%x)", omx_error );

    if( p_sys->out.p_hwbuf && HwBuffer_Start( p_dec, &p_sys->out ) != 0 )
    {
        omx_error = OMX_ErrorUndefined;
        goto error;
    }

    /* Send codec configuration data */
    if( p_dec->fmt_in.i_extra )
    {
        OMX_FIFO_GET(&p_sys->in.fifo, p_header);
        p_header->nFilledLen = p_dec->fmt_in.i_extra;

        /* Convert H.264 NAL format to annex b */
        if( p_sys->i_nal_size_length && !p_sys->in.b_direct )
        {
            p_header->nFilledLen = 0;
            convert_sps_pps( p_dec, p_dec->fmt_in.p_extra, p_dec->fmt_in.i_extra,
                             p_header->pBuffer, p_header->nAllocLen,
                             (uint32_t*) &p_header->nFilledLen, NULL );
        }
        else if( p_dec->fmt_in.i_codec == VLC_CODEC_HEVC && !p_sys->in.b_direct )
        {
            p_header->nFilledLen = 0;
            convert_hevc_nal_units( p_dec, p_dec->fmt_in.p_extra,
                                    p_dec->fmt_in.i_extra,
                                    p_header->pBuffer, p_header->nAllocLen,
                                    (uint32_t*) &p_header->nFilledLen,
                                    &p_sys->i_nal_size_length );
        }
        else if(p_sys->in.b_direct)
        {
            p_header->pOutputPortPrivate = p_header->pBuffer;
            p_header->pBuffer = p_dec->fmt_in.p_extra;
        }
        else if (p_dec->fmt_in.i_codec == VLC_CODEC_WMV3 &&
                 p_dec->fmt_in.i_extra >= 4 &&
                 p_header->nAllocLen >= 36)
        {
            int profile;
            // According to OMX IL 1.2.0 spec (4.3.33.2), the codec config
            // data for VC-1 Main/Simple (aka WMV3) is according to table 265
            // in the VC-1 spec. Most of the fields are just set with placeholders
            // (like framerate, hrd_buffer/rate).
            static const uint8_t wmv3seq[] = {
                0xff, 0xff, 0xff, 0xc5, // numframes=ffffff, marker byte
                0x04, 0x00, 0x00, 0x00, // marker byte
                0x00, 0x00, 0x00, 0x00, // struct C, almost equal to p_extra
                0x00, 0x00, 0x00, 0x00, // struct A, vert size
                0x00, 0x00, 0x00, 0x00, // struct A, horiz size
                0x0c, 0x00, 0x00, 0x00, // marker byte
                0xff, 0xff, 0x00, 0x80, // struct B, level=4, cbr=0, hrd_buffer=ffff
                0xff, 0xff, 0x00, 0x00, // struct B, hrd_rate=ffff
                0xff, 0xff, 0xff, 0xff, // struct B, framerate=ffffffff
            };
            p_header->nFilledLen = sizeof(wmv3seq);
            memcpy(p_header->pBuffer, wmv3seq, p_header->nFilledLen);
            // Struct C - almost equal to the extradata
            memcpy(&p_header->pBuffer[8], p_dec->fmt_in.p_extra, 4);
            // Expand profile from the highest 2 bits to the highest 4 bits
            profile = p_header->pBuffer[8] >> 6;
            p_header->pBuffer[8] = (p_header->pBuffer[8] & 0x0f) | (profile << 4);
            // Fill in the height/width for struct A
            SetDWLE(&p_header->pBuffer[12], p_dec->fmt_in.video.i_height);
            SetDWLE(&p_header->pBuffer[16], p_dec->fmt_in.video.i_width);
        }
        else
        {
            if(p_header->nFilledLen > p_header->nAllocLen)
            {
                msg_Dbg(p_dec, "buffer too small (%i,%i)", (int)p_header->nFilledLen,
                        (int)p_header->nAllocLen);
                p_header->nFilledLen = p_header->nAllocLen;
            }
            memcpy(p_header->pBuffer, p_dec->fmt_in.p_extra, p_header->nFilledLen);
        }

        p_header->nOffset = 0;
        p_header->nFlags = OMX_BUFFERFLAG_CODECCONFIG | OMX_BUFFERFLAG_ENDOFFRAME;
        msg_Dbg(p_dec, "sending codec config data %p, %p, %i", p_header,
                p_header->pBuffer, (int)p_header->nFilledLen);
        OMX_EmptyThisBuffer(p_sys->omx_handle, p_header);
    }

    /* Get back output port definition */
    omx_error = GetPortDefinition(p_dec, &p_sys->out, p_sys->out.p_fmt);
    if(omx_error != OMX_ErrorNone) goto error;

    PrintOmx(p_dec, p_sys->omx_handle, p_dec->p_sys->in.i_port_index);
    PrintOmx(p_dec, p_sys->omx_handle, p_dec->p_sys->out.i_port_index);

    if(p_sys->b_error) goto error;

    p_dec->b_need_packetized = true;

    if (!p_sys->b_use_pts)
        msg_Dbg( p_dec, "using dts timestamp mode for %s", p_sys->psz_component);

    return VLC_SUCCESS;

 error:
    CloseGeneric(p_this);
    return VLC_EGENERIC;
}

/*****************************************************************************
 * PortReconfigure
 *****************************************************************************/
static OMX_ERRORTYPE PortReconfigure(decoder_t *p_dec, OmxPort *p_port)
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_PARAM_PORTDEFINITIONTYPE definition;
    OMX_ERRORTYPE omx_error;

    OMX_DBG( "PortReconfigure(%d)", p_port->definition.eDir );

    /* Sanity checking */
    OMX_INIT_STRUCTURE(definition);
    definition.nPortIndex = p_port->i_port_index;
    omx_error = OMX_GetParameter(p_dec->p_sys->omx_handle, OMX_IndexParamPortDefinition,
                                 &definition);
    if(omx_error != OMX_ErrorNone || (p_dec->fmt_in.i_cat == VIDEO_ES &&
       (!definition.format.video.nFrameWidth ||
       !definition.format.video.nFrameHeight)) )
        return OMX_ErrorUndefined;

    if( p_port->p_hwbuf && HwBuffer_Stop( p_dec, p_port ) != 0 )
        msg_Warn( p_dec, "HwBuffer_Stop failed" );

    omx_error = OMX_SendCommand( p_sys->omx_handle, OMX_CommandPortDisable,
                                 p_port->i_port_index, NULL);
    CHECK_ERROR(omx_error, "OMX_CommandPortDisable on %i failed (%x)",
                (int)p_port->i_port_index, omx_error );

    omx_error = FreeBuffers( p_dec, p_port );
    CHECK_ERROR(omx_error, "FreeBuffers failed (%x, %i)",
                omx_error, (int)p_port->i_port_index );

    if( p_port->p_hwbuf )
    {
        HwBuffer_FreeBuffers( p_dec, p_port );
        HwBuffer_Join( p_dec, p_port );
    }

    omx_error = WaitForSpecificOmxEvent(&p_sys->event_queue, OMX_EventCmdComplete, 0, 0, 0);
    CHECK_ERROR(omx_error, "Wait for PortDisable failed (%x)", omx_error );

    /* Get the new port definition */
    omx_error = GetPortDefinition(p_dec, &p_sys->out, p_sys->out.p_fmt);
    if(omx_error != OMX_ErrorNone) goto error;

    if( p_port->p_hwbuf )
    {
        if( HwBuffer_AllocateBuffers( p_dec, p_port ) != 0 )
        {
            omx_error = OMX_ErrorInsufficientResources;
            goto error;
        }
    }
    else if( p_dec->fmt_in.i_cat != AUDIO_ES )
    {
        /* Don't explicitly set the new parameters that we got with
         * OMX_GetParameter above when using audio codecs.
         * That struct hasn't been changed since, so there should be
         * no need to set it here, unless some codec expects the
         * SetParameter call as a trigger event for some part of
         * the reconfiguration.
         * This fixes using audio decoders on Samsung Galaxy S II,
         *
         * Only skipping this for audio codecs, to minimize the
         * change for current working configurations for video.
         */
        omx_error = OMX_SetParameter(p_dec->p_sys->omx_handle, OMX_IndexParamPortDefinition,
                                     &definition);
        CHECK_ERROR(omx_error, "OMX_SetParameter failed (%x : %s)",
                    omx_error, ErrorToString(omx_error));
    }

    omx_error = OMX_SendCommand( p_sys->omx_handle, OMX_CommandPortEnable,
                                 p_port->i_port_index, NULL);
    CHECK_ERROR(omx_error, "OMX_CommandPortEnable on %i failed (%x)",
                (int)p_port->i_port_index, omx_error );

    omx_error = AllocateBuffers( p_dec, p_port );
    CHECK_ERROR(omx_error, "OMX_AllocateBuffers failed (%x, %i)",
                omx_error, (int)p_port->i_port_index );

    omx_error = WaitForSpecificOmxEvent(&p_sys->event_queue, OMX_EventCmdComplete, 0, 0, 0);
    CHECK_ERROR(omx_error, "Wait for PortEnable failed (%x)", omx_error );

    if( p_port->p_hwbuf && HwBuffer_Start( p_dec, p_port ) != 0 )
    {
        omx_error = OMX_ErrorUndefined;
        goto error;
    }

    PrintOmx(p_dec, p_sys->omx_handle, p_dec->p_sys->in.i_port_index);
    PrintOmx(p_dec, p_sys->omx_handle, p_dec->p_sys->out.i_port_index);

    OMX_DBG( "PortReconfigure(%d)::done", p_port->definition.eDir );
 error:
    return omx_error;
}

/*****************************************************************************
 * DecodeVideoOutput
 *****************************************************************************/
static int DecodeVideoOutput( decoder_t *p_dec, OmxPort *p_port, picture_t **pp_pic )
{
    VLC_UNUSED( p_dec );
    OMX_BUFFERHEADERTYPE *p_header;
    picture_t *p_pic = NULL, *p_next_pic;
    OMX_ERRORTYPE omx_error;

    while(!p_pic)
    {
        OMX_FIFO_PEEK(&p_port->fifo, p_header);
        if(!p_header) break; /* No frame available */

        if(p_port->b_update_def)
        {
            omx_error = GetPortDefinition(p_dec, p_port, p_port->p_fmt);
            p_port->b_update_def = 0;
            CHECK_ERROR(omx_error, "GetPortDefinition failed");
        }

        if( p_port->p_hwbuf )
        {
            if( HwBuffer_GetPic( p_dec, p_port, &p_pic ) != 0 )
                goto error;
            else
                continue;
        }

        if(p_header->nFilledLen)
        {
            p_pic = p_header->pAppPrivate;
            if(!p_pic)
            {
                /* We're not in direct rendering mode.
                 * Get a new picture and copy the content */
                p_pic = decoder_NewPicture( p_dec );

                if (p_pic)
                    CopyOmxPicture(p_port->definition.format.video.eColorFormat,
                                   p_pic, p_port->definition.format.video.nSliceHeight,
                                   p_port->i_frame_stride,
                                   p_header->pBuffer + p_header->nOffset,
                                   p_port->i_frame_stride_chroma_div, NULL);
            }

            if (p_pic)
                p_pic->date = FromOmxTicks(p_header->nTimeStamp);
            p_header->nFilledLen = 0;
            p_header->pAppPrivate = 0;
        }

        /* Get a new picture */
        if(p_port->b_direct && !p_header->pAppPrivate)
        {
            p_next_pic = decoder_NewPicture( p_dec );
            if(!p_next_pic) break;

            OMX_FIFO_GET(&p_port->fifo, p_header);
            p_header->pAppPrivate = p_next_pic;
            p_header->pInputPortPrivate = p_header->pBuffer;
            p_header->pBuffer = p_next_pic->p[0].p_pixels;
        }
        else
        {
            OMX_FIFO_GET(&p_port->fifo, p_header);
        }
        OMX_DBG( "FillThisBuffer %p, %p", p_header, p_header->pBuffer );
        OMX_FillThisBuffer(p_port->omx_handle, p_header);
    }

    *pp_pic = p_pic;
    return 0;
error:
    return -1;
}

static int DecodeVideoInput( decoder_t *p_dec, OmxPort *p_port, block_t **pp_block,
                             unsigned int i_input_used, bool *p_reconfig )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_BUFFERHEADERTYPE *p_header;
    struct H264ConvertState convert_state = { 0, 0 };
    block_t *p_block = *pp_block;

    /* Send the input buffer to the component */
    OMX_FIFO_GET_TIMEOUT(&p_port->fifo, p_header, 10000);

    if (p_header && p_header->nFlags & SENTINEL_FLAG) {
        free(p_header);
        *p_reconfig = true;
        return 0;
    }
    *p_reconfig = false;

    if(p_header)
    {
        bool decode_more = false;
        p_header->nFilledLen = p_block->i_buffer - i_input_used;
        p_header->nOffset = 0;
        p_header->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
        if (p_sys->b_use_pts && p_block->i_pts)
            p_header->nTimeStamp = ToOmxTicks(p_block->i_pts);
        else
            p_header->nTimeStamp = ToOmxTicks(p_block->i_dts);

        /* In direct mode we pass the input pointer as is.
         * Otherwise we memcopy the data */
        if(p_port->b_direct)
        {
            p_header->pOutputPortPrivate = p_header->pBuffer;
            p_header->pBuffer = p_block->p_buffer;
            p_header->pAppPrivate = p_block;
            i_input_used = p_header->nFilledLen;
        }
        else
        {
            if(p_header->nFilledLen > p_header->nAllocLen)
            {
                p_header->nFilledLen = p_header->nAllocLen;
            }
            memcpy(p_header->pBuffer, p_block->p_buffer + i_input_used, p_header->nFilledLen);
            i_input_used += p_header->nFilledLen;
            if (i_input_used == p_block->i_buffer)
            {
                block_Release(p_block);
            }
            else
            {
                decode_more = true;
                p_header->nFlags &= ~OMX_BUFFERFLAG_ENDOFFRAME;
            }
        }

        /* Convert H.264 NAL format to annex b. Doesn't do anything if
         * i_nal_size_length == 0, which is the case for codecs other
         * than H.264 */
        convert_h264_to_annexb( p_header->pBuffer, p_header->nFilledLen,
                                p_sys->i_nal_size_length, &convert_state );
        OMX_DBG( "EmptyThisBuffer %p, %p, %i, %"PRId64, p_header, p_header->pBuffer,
                 (int)p_header->nFilledLen, FromOmxTicks(p_header->nTimeStamp) );
        OMX_EmptyThisBuffer(p_port->omx_handle, p_header);
        p_port->b_flushed = false;
        if (decode_more)
            return DecodeVideoInput( p_dec, p_port, pp_block, i_input_used,
                                     p_reconfig );
        else
            *pp_block = NULL; /* Avoid being fed the same packet again */
    }

    return 0;
}

/*****************************************************************************
 * DecodeVideo: Called to decode one frame
 *****************************************************************************/
static picture_t *DecodeVideo( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    picture_t *p_pic = NULL;
    OMX_ERRORTYPE omx_error;
    unsigned int i;
    block_t *p_block;

    if( !pp_block || !*pp_block )
        return NULL;

    p_block = *pp_block;

    /* Check for errors from codec */
    if(p_sys->b_error)
    {
        msg_Dbg(p_dec, "error during decoding");
        block_Release( p_block );
        return 0;
    }

    if( p_block->i_flags & (BLOCK_FLAG_DISCONTINUITY|BLOCK_FLAG_CORRUPTED) )
    {
        block_Release( p_block );
        if(!p_sys->in.b_flushed)
        {
            msg_Dbg(p_dec, "flushing");
            OMX_SendCommand( p_sys->omx_handle, OMX_CommandFlush,
                             p_sys->in.definition.nPortIndex, 0 );
        }
        p_sys->in.b_flushed = true;
        return NULL;
    }

    /* Use the aspect ratio provided by the input (ie read from packetizer).
     * In case the we get aspect ratio info from the decoder (as in the
     * broadcom OMX implementation on RPi), don't let the packetizer values
     * override what the decoder says, if anything - otherwise always update
     * even if it already is set (since it can change within a stream). */
    if((p_dec->fmt_in.video.i_sar_num != 0 && p_dec->fmt_in.video.i_sar_den != 0) &&
       (p_dec->fmt_out.video.i_sar_num == 0 || p_dec->fmt_out.video.i_sar_den == 0 ||
             !p_sys->b_aspect_ratio_handled))
    {
        p_dec->fmt_out.video.i_sar_num = p_dec->fmt_in.video.i_sar_num;
        p_dec->fmt_out.video.i_sar_den = p_dec->fmt_in.video.i_sar_den;
    }

    /* Take care of decoded frames first */
    if( DecodeVideoOutput( p_dec, &p_sys->out, &p_pic ) != 0 )
        goto error;

    /* Loop as long as we haven't either got an input buffer (and cleared
     * *pp_block) or got an output picture */
    int max_polling_attempts = 100;
    int attempts = 0;
    while( *pp_block && !p_pic ) {
        bool b_reconfig = false;

        if( DecodeVideoInput( p_dec, &p_sys->in, pp_block, 0, &b_reconfig ) != 0 )
            goto error;

        /* If we don't have a p_pic from the first try. Try again */
        if( !b_reconfig && !p_pic &&
            DecodeVideoOutput( p_dec, &p_sys->out, &p_pic ) != 0 )
            goto error;

        /* Handle the PortSettingsChanged events */
        for(i = 0; i < p_sys->ports; i++)
        {
            OmxPort *p_port = &p_sys->p_ports[i];
            if(p_port->b_reconfigure)
            {
                omx_error = PortReconfigure(p_dec, p_port);
                p_port->b_reconfigure = 0;
                CHECK_ERROR(omx_error, "PortReconfigure failed");
            }
            if(p_port->b_update_def)
            {
                omx_error = GetPortDefinition(p_dec, p_port, p_port->p_fmt);
                p_port->b_update_def = 0;
                CHECK_ERROR(omx_error, "GetPortDefinition failed");
            }
        }

        attempts++;
        /* With opaque DR the output buffers are released by the
           vout therefore we implement a timeout for polling in
           order to avoid being indefinitely stalled in this loop, if
           playback is paused. */
        if( p_sys->out.p_hwbuf && attempts == max_polling_attempts ) {
#ifdef USE_IOMX
            picture_t *invalid_picture = decoder_NewPicture(p_dec);
            if (invalid_picture) {
                invalid_picture->date = VLC_TS_INVALID;
                picture_sys_t *p_picsys = invalid_picture->p_sys;
                p_picsys->pf_display_callback = NULL;
                p_picsys->pf_unlock_callback = NULL;
                p_picsys->p_dec = NULL;
                p_picsys->i_index = -1;
                p_picsys->b_valid = false;
            } else {
                /* If we cannot return a picture we must free the
                   block since the decoder will proceed with the
                   next block. */
                block_Release(p_block);
                *pp_block = NULL;
            }
            return invalid_picture;
#endif
        }
    }

    return p_pic;
error:
    p_sys->b_error = true;
    return NULL;
}

/*****************************************************************************
 * DecodeAudio: Called to decode one frame
 *****************************************************************************/
block_t *DecodeAudio ( decoder_t *p_dec, block_t **pp_block )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    block_t *p_buffer = NULL;
    OMX_BUFFERHEADERTYPE *p_header;
    OMX_ERRORTYPE omx_error;
    block_t *p_block;
    unsigned int i;

    if( !pp_block || !*pp_block ) return NULL;

    p_block = *pp_block;

    /* Check for errors from codec */
    if(p_sys->b_error)
    {
        msg_Dbg(p_dec, "error during decoding");
        block_Release( p_block );
        return 0;
    }

    if( p_block->i_flags & (BLOCK_FLAG_DISCONTINUITY|BLOCK_FLAG_CORRUPTED) )
    {
        block_Release( p_block );
        date_Set( &p_sys->end_date, 0 );
        if(!p_sys->in.b_flushed)
        {
            msg_Dbg(p_dec, "flushing");
            OMX_SendCommand( p_sys->omx_handle, OMX_CommandFlush,
                             p_sys->in.definition.nPortIndex, 0 );
        }
        p_sys->in.b_flushed = true;
        return NULL;
    }

    if( !date_Get( &p_sys->end_date ) )
    {
        if( !p_block->i_pts )
        {
            /* We've just started the stream, wait for the first PTS. */
            block_Release( p_block );
            return NULL;
        }
        date_Set( &p_sys->end_date, p_block->i_pts );
    }

    /* Take care of decoded frames first */
    while(!p_buffer)
    {
        unsigned int i_samples = 0;

        OMX_FIFO_PEEK(&p_sys->out.fifo, p_header);
        if(!p_header) break; /* No frame available */

        if (p_sys->out.p_fmt->audio.i_channels)
            i_samples = p_header->nFilledLen / p_sys->out.p_fmt->audio.i_channels / 2;
        if(i_samples)
        {
            p_buffer = decoder_NewAudioBuffer( p_dec, i_samples );
            if( !p_buffer ) break; /* No audio buffer available */

            memcpy( p_buffer->p_buffer, p_header->pBuffer, p_buffer->i_buffer );
            p_header->nFilledLen = 0;

            int64_t timestamp = FromOmxTicks(p_header->nTimeStamp);
            if( timestamp != 0 &&
                timestamp != date_Get( &p_sys->end_date ) )
                date_Set( &p_sys->end_date, timestamp );

            p_buffer->i_pts = date_Get( &p_sys->end_date );
            p_buffer->i_length = date_Increment( &p_sys->end_date, i_samples ) -
                p_buffer->i_pts;
        }

        OMX_DBG( "FillThisBuffer %p, %p", p_header, p_header->pBuffer );
        OMX_FIFO_GET(&p_sys->out.fifo, p_header);
        OMX_FillThisBuffer(p_sys->omx_handle, p_header);
    }


    /* Send the input buffer to the component */
    OMX_FIFO_GET_TIMEOUT(&p_sys->in.fifo, p_header, 200000);

    if (p_header && p_header->nFlags & SENTINEL_FLAG) {
        free(p_header);
        goto reconfig;
    }

    if(p_header)
    {
        p_header->nFilledLen = p_block->i_buffer;
        p_header->nOffset = 0;
        p_header->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
        p_header->nTimeStamp = ToOmxTicks(p_block->i_dts);

        /* In direct mode we pass the input pointer as is.
         * Otherwise we memcopy the data */
        if(p_sys->in.b_direct)
        {
            p_header->pOutputPortPrivate = p_header->pBuffer;
            p_header->pBuffer = p_block->p_buffer;
            p_header->pAppPrivate = p_block;
        }
        else
        {
            if(p_header->nFilledLen > p_header->nAllocLen)
            {
                msg_Dbg(p_dec, "buffer too small (%i,%i)",
                        (int)p_header->nFilledLen, (int)p_header->nAllocLen);
                p_header->nFilledLen = p_header->nAllocLen;
            }
            memcpy(p_header->pBuffer, p_block->p_buffer, p_header->nFilledLen );
            block_Release(p_block);
        }

        OMX_DBG( "EmptyThisBuffer %p, %p, %i", p_header, p_header->pBuffer,
                 (int)p_header->nFilledLen );
        OMX_EmptyThisBuffer(p_sys->omx_handle, p_header);
        p_sys->in.b_flushed = false;
        *pp_block = NULL; /* Avoid being fed the same packet again */
    }

reconfig:
    /* Handle the PortSettingsChanged events */
    for(i = 0; i < p_sys->ports; i++)
    {
        OmxPort *p_port = &p_sys->p_ports[i];
        if(!p_port->b_reconfigure) continue;
        p_port->b_reconfigure = 0;
        omx_error = PortReconfigure(p_dec, p_port);
        CHECK_ERROR(omx_error, "PortReconfigure failed");
    }

    return p_buffer;
error:
    p_sys->b_error = true;
    return NULL;
}

/*****************************************************************************
 * EncodeVideo: Called to encode one frame
 *****************************************************************************/
static block_t *EncodeVideo( encoder_t *p_enc, picture_t *p_pic )
{
    decoder_t *p_dec = ( decoder_t *)p_enc;
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_ERRORTYPE omx_error;
    unsigned int i;

    OMX_BUFFERHEADERTYPE *p_header;
    block_t *p_block = 0;

    if( !p_pic ) return NULL;

    /* Check for errors from codec */
    if(p_sys->b_error)
    {
        msg_Dbg(p_dec, "error during encoding");
        return NULL;
    }

    /* Send the input buffer to the component */
    OMX_FIFO_GET(&p_sys->in.fifo, p_header);
    if(p_header)
    {
        /* In direct mode we pass the input pointer as is.
         * Otherwise we memcopy the data */
        if(p_sys->in.b_direct)
        {
            p_header->pOutputPortPrivate = p_header->pBuffer;
            p_header->pBuffer = p_pic->p[0].p_pixels;
        }
        else
        {
            CopyVlcPicture(p_dec, p_header, p_pic);
        }

        p_header->nFilledLen = p_sys->in.i_frame_size;
        p_header->nOffset = 0;
        p_header->nFlags = OMX_BUFFERFLAG_ENDOFFRAME;
        p_header->nTimeStamp = ToOmxTicks(p_pic->date);
        OMX_DBG( "EmptyThisBuffer %p, %p, %i", p_header, p_header->pBuffer,
                 (int)p_header->nFilledLen );
        OMX_EmptyThisBuffer(p_sys->omx_handle, p_header);
        p_sys->in.b_flushed = false;
    }

    /* Handle the PortSettingsChanged events */
    for(i = 0; i < p_sys->ports; i++)
    {
        OmxPort *p_port = &p_sys->p_ports[i];
        if(!p_port->b_reconfigure) continue;
        p_port->b_reconfigure = 0;
        omx_error = PortReconfigure(p_dec, p_port);
        CHECK_ERROR(omx_error, "PortReconfigure failed");
    }

    /* Wait for the decoded frame */
    while(!p_block)
    {
        OMX_FIFO_GET(&p_sys->out.fifo, p_header);

        if(p_header->nFilledLen)
        {
            if(p_header->nFlags & OMX_BUFFERFLAG_CODECCONFIG)
            {
                /* TODO: need to store codec config */
                msg_Dbg(p_dec, "received codec config %i", (int)p_header->nFilledLen);
            }

            p_block = p_header->pAppPrivate;
            if(!p_block)
            {
                /* We're not in direct rendering mode.
                 * Get a new block and copy the content */
                p_block = block_Alloc( p_header->nFilledLen );
                memcpy(p_block->p_buffer, p_header->pBuffer, p_header->nFilledLen );
            }

            p_block->i_buffer = p_header->nFilledLen;
            p_block->i_pts = p_block->i_dts = FromOmxTicks(p_header->nTimeStamp);
            p_header->nFilledLen = 0;
            p_header->pAppPrivate = 0;
        }

        OMX_DBG( "FillThisBuffer %p, %p", p_header, p_header->pBuffer );
        OMX_FillThisBuffer(p_sys->omx_handle, p_header);
    }

    msg_Dbg(p_dec, "done");
    return p_block;
error:
    p_sys->b_error = true;
    return NULL;
}

/*****************************************************************************
 * CloseGeneric: omxil decoder destruction
 *****************************************************************************/
static void CloseGeneric( vlc_object_t *p_this )
{
    decoder_t *p_dec = (decoder_t *)p_this;
    decoder_sys_t *p_sys = p_dec->p_sys;

    if(p_sys->omx_handle) DeinitialiseComponent(p_dec, p_sys->omx_handle);

    DeinitOmxCore();

    DeinitOmxEventQueue(&p_sys->event_queue);

    OMX_FIFO_DESTROY( &p_sys->in.fifo );
    OMX_FIFO_DESTROY( &p_sys->out.fifo );

    free( p_sys );
}

/*****************************************************************************
 * OmxEventHandler:
 *****************************************************************************/
static OMX_ERRORTYPE OmxEventHandler( OMX_HANDLETYPE omx_handle,
    OMX_PTR app_data, OMX_EVENTTYPE event, OMX_U32 data_1,
    OMX_U32 data_2, OMX_PTR event_data )
{
    decoder_t *p_dec = (decoder_t *)app_data;
    decoder_sys_t *p_sys = p_dec->p_sys;
    unsigned int i;
    (void)omx_handle;

    PrintOmxEvent((vlc_object_t *) p_dec, event, data_1, data_2, event_data);
    switch (event)
    {
    case OMX_EventError:
        //p_sys->b_error = true;
        break;

    case OMX_EventPortSettingsChanged:
        if( data_2 == 0 || data_2 == OMX_IndexParamPortDefinition ||
            data_2 == OMX_IndexParamAudioPcm )
        {
            OMX_BUFFERHEADERTYPE *sentinel;
            for(i = 0; i < p_sys->ports; i++)
                if(p_sys->p_ports[i].definition.eDir == OMX_DirOutput)
                    p_sys->p_ports[i].b_reconfigure = true;
            sentinel = calloc(1, sizeof(*sentinel));
            if (sentinel) {
                sentinel->nFlags = SENTINEL_FLAG;
                OMX_FIFO_PUT(&p_sys->in.fifo, sentinel);
            }
        }
        else if( data_2 == OMX_IndexConfigCommonOutputCrop )
        {
            for(i = 0; i < p_sys->ports; i++)
                if(p_sys->p_ports[i].definition.nPortIndex == data_1)
                    p_sys->p_ports[i].b_update_def = true;
        }
        else
        {
            msg_Dbg( p_dec, "Unhandled setting change %x", (unsigned int)data_2 );
        }
        break;
    case OMX_EventParamOrConfigChanged:
        UpdatePixelAspect(p_dec);
        break;

    default:
        break;
    }

    PostOmxEvent(&p_sys->event_queue, event, data_1, data_2, event_data);
    return OMX_ErrorNone;
}

static OMX_ERRORTYPE OmxEmptyBufferDone( OMX_HANDLETYPE omx_handle,
    OMX_PTR app_data, OMX_BUFFERHEADERTYPE *omx_header )
{
    decoder_t *p_dec = (decoder_t *)app_data;
    decoder_sys_t *p_sys = p_dec->p_sys;
    (void)omx_handle;

    OMX_DBG( "OmxEmptyBufferDone %p, %p", omx_header, omx_header->pBuffer );

    if(omx_header->pAppPrivate || omx_header->pOutputPortPrivate)
    {
        block_t *p_block = (block_t *)omx_header->pAppPrivate;
        omx_header->pBuffer = omx_header->pOutputPortPrivate;
        if(p_block) block_Release(p_block);
        omx_header->pAppPrivate = 0;
    }
    OMX_FIFO_PUT(&p_sys->in.fifo, omx_header);

    return OMX_ErrorNone;
}

static OMX_ERRORTYPE OmxFillBufferDone( OMX_HANDLETYPE omx_handle,
    OMX_PTR app_data, OMX_BUFFERHEADERTYPE *omx_header )
{
    decoder_t *p_dec = (decoder_t *)app_data;
    decoder_sys_t *p_sys = p_dec->p_sys;
    (void)omx_handle;

    OMX_DBG( "OmxFillBufferDone %p, %p, %i, %"PRId64, omx_header, omx_header->pBuffer,
             (int)omx_header->nFilledLen, FromOmxTicks(omx_header->nTimeStamp) );

    if(omx_header->pInputPortPrivate)
    {
        omx_header->pBuffer = omx_header->pInputPortPrivate;
    }
    OMX_FIFO_PUT(&p_sys->out.fifo, omx_header);

    return OMX_ErrorNone;
}

#if defined(USE_IOMX)

/* Life cycle of buffers when using IOMX direct rendering (HwBuffer):
 *
 * <- android display
 * DequeueThread owned++
 * -> OMX_FillThisBuffer
 * ...
 * <- FillBufferDone OMX_FIFO_PUT
 * ...
 * DecodeVideoOutput OMX_FIFO_GET
 * -> vlc core
 * ...
 * DisplayBuffer
 * -> android display owned--
 */

/*****************************************************************************
 * HwBuffer_ChangeState
 *****************************************************************************/
static void HwBuffer_ChangeState( decoder_t *p_dec, OmxPort *p_port,
                                  int i_index, int i_state )
{
    VLC_UNUSED( p_dec );
    p_port->p_hwbuf->i_states[i_index] = i_state;
    if( i_state == BUF_STATE_OWNED )
        p_port->p_hwbuf->i_owned++;
    else
        p_port->p_hwbuf->i_owned--;

    OMX_DBG( "buffer[%d]: state -> %d, owned buffers: %u",
             i_index, i_state, p_port->p_hwbuf->i_owned );
}

/*****************************************************************************
 * HwBuffer_Init
 *****************************************************************************/
static void HwBuffer_Init( decoder_t *p_dec, OmxPort *p_port )
{
    VLC_UNUSED( p_dec );
    void *surf;
    JNIEnv *p_env;
    OMX_ERRORTYPE omx_error;

    if( !p_port->b_direct || p_port->definition.eDir != OMX_DirOutput ||
        p_port->p_fmt->i_cat != VIDEO_ES )
        return;

    msg_Dbg( p_dec, "HwBuffer_Init");

    if( !(pf_enable_graphic_buffers && pf_get_graphic_buffer_usage &&
          pf_get_hal_format &&
          ((OMX_COMPONENTTYPE*)p_port->omx_handle)->UseBuffer) )
    {
        msg_Warn( p_dec, "direct output port enabled but can't find "
                          "extra symbols, switch back to non direct" );
        goto error;
    }

    p_port->p_hwbuf = calloc(1, sizeof(HwBuffer));
    if( !p_port->p_hwbuf )
    {
        goto error;
    }
    vlc_cond_init (&p_port->p_hwbuf->wait);
    p_port->p_hwbuf->p_library = LoadNativeWindowAPI( &p_port->p_hwbuf->native_window );
    if( !p_port->p_hwbuf->p_library )
    {
        msg_Warn( p_dec, "LoadNativeWindowAPI failed" );
        goto error;
    }
    if( LoadNativeWindowPrivAPI( &p_port->p_hwbuf->anwpriv ) != 0 )
    {
        msg_Warn( p_dec, "LoadNativeWindowPrivAPI failed" );
        goto error;
    }

    surf = jni_LockAndGetAndroidJavaSurface();
    if( !surf ) {
        jni_UnlockAndroidSurface();
        msg_Warn( p_dec, "jni_LockAndGetAndroidJavaSurface failed" );
        goto error;
    }

    jni_attach_thread( &p_env, THREAD_NAME );
    p_port->p_hwbuf->window = p_port->p_hwbuf->native_window.winFromSurface( p_env, surf );
    jni_detach_thread();

    jni_UnlockAndroidSurface();
    if( !p_port->p_hwbuf->window ) {
        msg_Warn( p_dec, "winFromSurface failed" );
        goto error;
    }
    if( p_port->p_hwbuf->anwpriv.connect( p_port->p_hwbuf->window ) != 0 ) {
        msg_Warn( p_dec, "connect failed" );
        p_port->p_hwbuf->native_window.winRelease( p_port->p_hwbuf->window );
        p_port->p_hwbuf->window = NULL;
        goto error;
    }

    omx_error = pf_enable_graphic_buffers( p_port->omx_handle,
                                           p_port->i_port_index, OMX_TRUE );
    CHECK_ERROR( omx_error, "can't enable graphic buffers" );

    /* PortDefinition may change after pf_enable_graphic_buffers call */
    omx_error = OMX_GetParameter( p_port->omx_handle,
                                  OMX_IndexParamPortDefinition,
                                  &p_port->definition );
    CHECK_ERROR( omx_error, "OMX_GetParameter failed (GraphicBuffers) (%x : %s)",
                 omx_error, ErrorToString(omx_error) );


    msg_Dbg( p_dec, "direct output port enabled" );
    return;
error:
    /* if HwBuffer_Init fails, we can fall back to non direct buffers */
    HwBuffer_Destroy( p_dec, p_port );
}

/*****************************************************************************
 * HwBuffer_Destroy
 *****************************************************************************/
static void HwBuffer_Destroy( decoder_t *p_dec, OmxPort *p_port )
{
    if( p_port->p_hwbuf )
    {
        if( p_port->p_hwbuf->p_library )
        {
            if( p_port->p_hwbuf->window )
            {
                HwBuffer_Stop( p_dec, p_port );
                HwBuffer_FreeBuffers( p_dec, p_port );
                HwBuffer_Join( p_dec, p_port );
                p_port->p_hwbuf->anwpriv.disconnect( p_port->p_hwbuf->window );
                pf_enable_graphic_buffers( p_port->omx_handle,
                                           p_port->i_port_index, OMX_FALSE );
                p_port->p_hwbuf->native_window.winRelease( p_port->p_hwbuf->window );
            }
            dlclose( p_port->p_hwbuf->p_library );
        }

        vlc_cond_destroy( &p_port->p_hwbuf->wait );
        free( p_port->p_hwbuf );
        p_port->p_hwbuf = NULL;
    }
    p_port->b_direct = false;
}

/*****************************************************************************
 * HwBuffer_AllocateBuffers
 *****************************************************************************/
static int HwBuffer_AllocateBuffers( decoder_t *p_dec, OmxPort *p_port )
{
    decoder_sys_t *p_sys = p_dec->p_sys;
    OMX_PARAM_PORTDEFINITIONTYPE *def = &p_port->definition;
    unsigned int min_undequeued = 0;
    unsigned int i = 0;
    int colorFormat = def->format.video.eColorFormat;
    OMX_ERRORTYPE omx_error;
    OMX_U32 i_hw_usage;

    if( !p_port->p_hwbuf )
        return 0;

    omx_error = pf_get_hal_format( p_sys->psz_component, &colorFormat );
    if( omx_error != OMX_ErrorNone )
    {
        msg_Warn( p_dec, "pf_get_hal_format failed (Not fatal)" );
    }

    omx_error = pf_get_graphic_buffer_usage( p_port->omx_handle,
                                             p_port->i_port_index,
                                             &i_hw_usage );
    if( omx_error != OMX_ErrorNone )
    {
        msg_Warn( p_dec, "pf_get_graphic_buffer_usage failed (Not fatal)" );
        i_hw_usage = 0;
    }

    if( p_port->p_fmt->video.orientation != ORIENT_NORMAL )
    {
        int i_angle;

        switch( p_port->p_fmt->video.orientation )
        {
            case ORIENT_ROTATED_90:
                i_angle = 90;
                break;
            case ORIENT_ROTATED_180:
                i_angle = 180;
                break;
            case ORIENT_ROTATED_270:
                i_angle = 270;
                break;
            default:
                i_angle = 0;
        }
        p_port->p_hwbuf->anwpriv.setOrientation( p_port->p_hwbuf->window,
                                                 i_angle );
        video_format_ApplyRotation( &p_port->p_hwbuf->fmt_out,
                                    &p_port->p_fmt->video );
    } else
        p_port->p_hwbuf->fmt_out = p_port->p_fmt->video;

    if( p_port->p_hwbuf->anwpriv.setup( p_port->p_hwbuf->window,
                                        def->format.video.nFrameWidth,
                                        def->format.video.nFrameHeight,
                                        colorFormat,
                                        (int) i_hw_usage ) != 0 )
    {
        msg_Err( p_dec, "can't setup OMXHWBuffer" );
        goto error;
    }

    if( p_port->p_hwbuf->anwpriv.getMinUndequeued( p_port->p_hwbuf->window,
                                                   &min_undequeued ) != 0 )
    {
        msg_Err( p_dec, "can't get min_undequeued" );
        goto error;
    }

    if( def->nBufferCountActual < def->nBufferCountMin + min_undequeued )
    {
        unsigned int new_frames_num = def->nBufferCountMin + min_undequeued;

        OMX_DBG( "AllocateBuffers: video out wants more frames: %lu vs %u",
                 p_port->definition.nBufferCountActual, new_frames_num );

        p_port->definition.nBufferCountActual = new_frames_num;
        omx_error = OMX_SetParameter( p_dec->p_sys->omx_handle,
                                      OMX_IndexParamPortDefinition,
                                      &p_port->definition );
        CHECK_ERROR( omx_error, "OMX_SetParameter failed (%x : %s)",
                     omx_error, ErrorToString(omx_error) );
    }

    if( p_port->p_hwbuf->anwpriv.setBufferCount( p_port->p_hwbuf->window,
                                                 def->nBufferCountActual ) != 0 )
    {
        msg_Err( p_dec, "can't set buffer_count" );
        goto error;
    }

    jni_SetAndroidSurfaceSize( p_port->p_hwbuf->fmt_out.i_width,
                               p_port->p_hwbuf->fmt_out.i_height,
                               p_port->p_hwbuf->fmt_out.i_visible_width,
                               p_port->p_hwbuf->fmt_out.i_visible_height,
                               p_port->p_hwbuf->fmt_out.i_sar_num,
                               p_port->p_hwbuf->fmt_out.i_sar_den );

    p_port->p_hwbuf->i_buffers = p_port->definition.nBufferCountActual;
    p_port->p_hwbuf->i_max_owned = p_port->p_hwbuf->i_buffers - min_undequeued;

    p_port->p_hwbuf->pp_handles = calloc( p_port->p_hwbuf->i_buffers,
                                          sizeof(void *) );
    if( !p_port->p_hwbuf->pp_handles )
        goto error;

    p_port->p_hwbuf->i_states = calloc( p_port->p_hwbuf->i_buffers, sizeof(int) );
    if( !p_port->p_hwbuf->i_states )
        goto error;

    p_port->p_hwbuf->inflight_picture = calloc( p_port->p_hwbuf->i_buffers,
                                                sizeof(picture_t*) );
    if( !p_port->p_hwbuf->inflight_picture )
        goto error;

    for(i = 0; i < p_port->p_hwbuf->i_buffers; i++)
    {
        void *p_handle = NULL;

        if( p_port->p_hwbuf->anwpriv.dequeue( p_port->p_hwbuf->window,
                                              &p_handle ) != 0 )
        {
            msg_Err( p_dec, "OMXHWBuffer_dequeue Fail" );
            goto error;
        }
        p_port->p_hwbuf->pp_handles[i] = p_handle;
    }
    for(i = 0; i < p_port->p_hwbuf->i_max_owned; i++)
        HwBuffer_ChangeState( p_dec, p_port, i, BUF_STATE_OWNED );
    for(; i < p_port->p_hwbuf->i_buffers; i++)
    {
        OMX_DBG( "canceling buffer(%d)", i );
        p_port->p_hwbuf->anwpriv.cancel( p_port->p_hwbuf->window,
                                         p_port->p_hwbuf->pp_handles[i] );
    }

    return 0;

error:

    msg_Err( p_dec, "HwBuffer_AllocateBuffers(%d) failed", def->eDir );
    return -1;
}

/*****************************************************************************
 * HwBuffer_FreeBuffers
 *****************************************************************************/
static int HwBuffer_FreeBuffers( decoder_t *p_dec, OmxPort *p_port )
{
    msg_Dbg( p_dec, "HwBuffer_FreeBuffers");

    HWBUFFER_LOCK();

    p_port->p_hwbuf->b_run = false;

    if( p_port->p_hwbuf->pp_handles )
    {
        for(unsigned int i = 0; i < p_port->p_hwbuf->i_buffers; i++)
        {
            void *p_handle = p_port->p_hwbuf->pp_handles[i];

            if( p_handle && p_port->p_hwbuf->i_states[i] == BUF_STATE_OWNED )
            {
                p_port->p_hwbuf->anwpriv.cancel( p_port->p_hwbuf->window, p_handle );
                HwBuffer_ChangeState( p_dec, p_port, i, BUF_STATE_NOT_OWNED );
            }
        }
    }
    HWBUFFER_BROADCAST( p_port );

    HWBUFFER_UNLOCK();

    p_port->p_hwbuf->i_buffers = 0;

    free( p_port->p_hwbuf->pp_handles );
    p_port->p_hwbuf->pp_handles = NULL;

    free( p_port->p_hwbuf->i_states );
    p_port->p_hwbuf->i_states = NULL;

    free( p_port->p_hwbuf->inflight_picture );
    p_port->p_hwbuf->inflight_picture = NULL;

    return 0;
}

/*****************************************************************************
 * HwBuffer_Start
 *****************************************************************************/
static int HwBuffer_Start( decoder_t *p_dec, OmxPort *p_port )
{
    OMX_BUFFERHEADERTYPE *p_header;

    msg_Dbg( p_dec, "HwBuffer_Start" );
    HWBUFFER_LOCK();

    /* fill all owned buffers dequeued by HwBuffer_AllocatesBuffers */
    for(unsigned int i = 0; i < p_port->p_hwbuf->i_buffers; i++)
    {
        p_header = p_port->pp_buffers[i];

        if( p_header && p_port->p_hwbuf->i_states[i] == BUF_STATE_OWNED )
        {
            if( p_port->p_hwbuf->anwpriv.lock( p_port->p_hwbuf->window,
                                               p_header->pBuffer ) != 0 )
            {
                msg_Err( p_dec, "lock failed" );
                HWBUFFER_UNLOCK();
                return -1;
            }
            OMX_DBG( "FillThisBuffer %p, %p", p_header, p_header->pBuffer );
            OMX_FillThisBuffer( p_port->omx_handle, p_header );
        }
    }

    p_port->p_hwbuf->b_run = true;
    if( vlc_clone( &p_port->p_hwbuf->dequeue_thread,
                   DequeueThread, p_dec, VLC_THREAD_PRIORITY_LOW ) )
    {
        p_port->p_hwbuf->b_run = false;
        HWBUFFER_UNLOCK();
        return -1;
    }

    HWBUFFER_UNLOCK();

    return 0;
}

/*****************************************************************************
 * HwBuffer_Stop: stop DequeueThread and invalidate all pictures that are sent
 * to vlc core. The thread can be stuck in dequeue, so don't
 * join it now since it can be unblocked later by HwBuffer_FreeBuffers.
 *****************************************************************************/
static int HwBuffer_Stop( decoder_t *p_dec, OmxPort *p_port )
{
    VLC_UNUSED( p_dec );

    msg_Dbg( p_dec, "HwBuffer_Stop" );
    HWBUFFER_LOCK();

    p_port->p_hwbuf->b_run = false;

    /* invalidate and release all inflight pictures */
    if( p_port->p_hwbuf->inflight_picture ) {
        for( unsigned int i = 0; i < p_port->i_buffers; ++i ) {
            picture_t *p_pic = p_port->p_hwbuf->inflight_picture[i];
            if( p_pic ) {
                picture_sys_t *p_picsys = p_pic->p_sys;
                if( p_picsys ) {
                    void *p_handle = p_port->pp_buffers[p_picsys->i_index]->pBuffer;
                    if( p_handle )
                    {
                        p_port->p_hwbuf->anwpriv.cancel( p_port->p_hwbuf->window, p_handle );
                        HwBuffer_ChangeState( p_dec, p_port, p_picsys->i_index,
                                              BUF_STATE_NOT_OWNED );
                    }
                    p_picsys->b_valid = false;
                }
                p_port->p_hwbuf->inflight_picture[i] = NULL;
            }
        }
    }

    HWBUFFER_BROADCAST( p_port );

    HWBUFFER_UNLOCK();

    return 0;
}

/*****************************************************************************
 * HwBuffer_Join: join DequeueThread previously stopped by HwBuffer_Stop.
 *****************************************************************************/
static int HwBuffer_Join( decoder_t *p_dec, OmxPort *p_port )
{
    VLC_UNUSED( p_dec );

    if( p_port->p_hwbuf->dequeue_thread )
    {
        vlc_join( p_port->p_hwbuf->dequeue_thread, NULL );
        p_port->p_hwbuf->dequeue_thread = NULL;
    }
    return 0;
}

/*****************************************************************************
 * HwBuffer_GetPic
 *****************************************************************************/
static int HwBuffer_GetPic( decoder_t *p_dec, OmxPort *p_port,
                            picture_t **pp_pic)
{
    int i_index = -1;
    picture_t *p_pic;
    picture_sys_t *p_picsys;
    OMX_BUFFERHEADERTYPE *p_header;

    OMX_FIFO_PEEK(&p_port->fifo, p_header);

    if( !p_header )
        return 0;

    for(unsigned int i = 0; i < p_port->i_buffers; i++)
    {
        if( p_port->pp_buffers[i] == p_header )
        {
            i_index = i;
            break;
        }
    }
    if( i_index == -1 )
    {
        msg_Err( p_dec, "output buffer not found" );
        return -1;
    }

    p_pic = decoder_NewPicture( p_dec );
    if(!p_pic)
    {
        msg_Err( p_dec, "decoder_NewPicture failed" );
        return -1;
    }
    p_pic->date = FromOmxTicks( p_header->nTimeStamp );

    p_picsys = p_pic->p_sys;
    p_picsys->pf_display_callback = DisplayCallback;
    p_picsys->pf_unlock_callback = UnlockCallback;
    p_picsys->p_dec = p_dec;
    p_picsys->i_index = i_index;
    p_picsys->b_valid = true;

    HWBUFFER_LOCK();
    p_port->p_hwbuf->inflight_picture[i_index] = p_pic;
    HWBUFFER_UNLOCK();

    *pp_pic = p_pic;
    OMX_FIFO_GET( &p_port->fifo, p_header );
    return 0;
}

/*****************************************************************************
 * HwBuffer_SetCrop
 *****************************************************************************/
static void HwBuffer_SetCrop( decoder_t *p_dec, OmxPort *p_port,
                              OMX_CONFIG_RECTTYPE *p_rect )
{
    VLC_UNUSED( p_dec );

    p_port->p_hwbuf->anwpriv.setCrop( p_port->p_hwbuf->window,
                                      p_rect->nLeft, p_rect->nTop,
                                      p_rect->nWidth, p_rect->nHeight );
}

/*****************************************************************************
 * DequeueThread
 *****************************************************************************/
static void *DequeueThread( void *data )
{
    decoder_t *p_dec = data;
    decoder_sys_t *p_sys = p_dec->p_sys;;
    OmxPort *p_port = &p_sys->out;
    unsigned int i;
    int i_index = -1;
    int err;
    void *p_handle = NULL;
    OMX_BUFFERHEADERTYPE *p_header;

    msg_Dbg( p_dec, "DequeueThread running");
    HWBUFFER_LOCK();
    while( p_port->p_hwbuf->b_run )
    {
        while( p_port->p_hwbuf->b_run &&
               p_port->p_hwbuf->i_owned >= p_port->p_hwbuf->i_max_owned )
            HWBUFFER_WAIT( p_port );

        if( !p_port->p_hwbuf->b_run ) continue;

        HWBUFFER_UNLOCK();


        /* The thread can be stuck here. It shouldn't happen since we make sure
         * we call the dequeue function if there is at least one buffer
         * available. */
        err = p_port->p_hwbuf->anwpriv.dequeue( p_port->p_hwbuf->window, &p_handle );
        if( err == 0 )
            err = p_port->p_hwbuf->anwpriv.lock( p_port->p_hwbuf->window, p_handle );

        HWBUFFER_LOCK();

        if( err != 0 ) {
            if( err != -EBUSY )
                p_port->p_hwbuf->b_run = false;
            continue;
        }

        if( !p_port->p_hwbuf->b_run )
        {
            p_port->p_hwbuf->anwpriv.cancel( p_port->p_hwbuf->window, p_handle );
            continue;
        }

        for(i = 0; i < p_port->i_buffers; i++)
        {
            if( p_port->pp_buffers[i]->pBuffer == p_handle )
            {
                i_index = i;
                p_header = p_port->pp_buffers[i_index];
                break;
            }
        }
        if( i_index == -1 )
        {
            msg_Err( p_dec, "p_port->p_hwbuf->anwpriv.dequeue returned unknown handle" );
            continue;
        }

        HwBuffer_ChangeState( p_dec, p_port, i_index, BUF_STATE_OWNED );

        OMX_DBG( "FillThisBuffer %p, %p", p_header, p_header->pBuffer );
        OMX_FillThisBuffer( p_sys->omx_handle, p_header );

        HWBUFFER_BROADCAST( p_port );
    }
    HWBUFFER_UNLOCK();

    msg_Dbg( p_dec, "DequeueThread stopped");
    return NULL;
}

/*****************************************************************************
 * vout callbacks
 *****************************************************************************/
static void DisplayBuffer( picture_sys_t* p_picsys, bool b_render )
{
    decoder_t *p_dec = p_picsys->p_dec;
    decoder_sys_t *p_sys = p_dec->p_sys;
    OmxPort *p_port = &p_sys->out;
    void *p_handle;

    if( !p_picsys->b_valid ) return;

    HWBUFFER_LOCK();

    /* Picture might have been invalidated while waiting on the mutex. */
    if (!p_picsys->b_valid) {
        HWBUFFER_UNLOCK();
        return;
    }

    p_handle = p_port->pp_buffers[p_picsys->i_index]->pBuffer;

    OMX_DBG( "DisplayBuffer: %s %p",
             b_render ? "render" : "cancel", p_handle );

    if( !p_handle )
    {
        msg_Err( p_dec, "DisplayBuffer: buffer handle invalid" );
        goto end;
    }

    if( b_render )
        p_port->p_hwbuf->anwpriv.queue( p_port->p_hwbuf->window, p_handle );
    else
        p_port->p_hwbuf->anwpriv.cancel( p_port->p_hwbuf->window, p_handle );

    HwBuffer_ChangeState( p_dec, p_port, p_picsys->i_index, BUF_STATE_NOT_OWNED );
    HWBUFFER_BROADCAST( p_port );

    p_port->p_hwbuf->inflight_picture[p_picsys->i_index] = NULL;

end:
    p_picsys->b_valid = false;
    p_picsys->i_index = -1;

    HWBUFFER_UNLOCK();
}

static void UnlockCallback( picture_sys_t* p_picsys )
{
    DisplayBuffer( p_picsys, false );
}

static void DisplayCallback( picture_sys_t* p_picsys )
{
    DisplayBuffer( p_picsys, true );
}

#endif // USE_IOMX
