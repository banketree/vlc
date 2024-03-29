/*****************************************************************************
 * ts.c: Transport Stream input module for VLC.
 *****************************************************************************
 * Copyright (C) 2004-2005 VLC authors and VideoLAN
 * $Id$
 *
 * Authors: Laurent Aimar <fenrir@via.ecp.fr>
 *          Jean-Paul Saman <jpsaman #_at_# m2x.nl>
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

#include <vlc_common.h>
#include <vlc_plugin.h>

#include <assert.h>
#include <time.h>

#include <vlc_access.h>    /* DVB-specific things */
#include <vlc_demux.h>
#include <vlc_meta.h>
#include <vlc_epg.h>
#include <vlc_charset.h>   /* FromCharset, for EIT */
#include <vlc_bits.h>

#include "../mux/mpeg/csa.h"

/* Include dvbpsi headers */
# include <dvbpsi/dvbpsi.h>
# include <dvbpsi/demux.h>
# include <dvbpsi/descriptor.h>
# include <dvbpsi/pat.h>
# include <dvbpsi/pmt.h>
# include <dvbpsi/sdt.h>
# include <dvbpsi/dr.h>
# include <dvbpsi/psi.h>

/* EIT support */
# include <dvbpsi/eit.h>

/* TDT support */
# include <dvbpsi/tot.h>

#include "../mux/mpeg/dvbpsi_compat.h"

#include "../codec/opus_header.h"

#include "opus.h"

#undef TS_DEBUG
VLC_FORMAT(1, 2) static void ts_debug(const char *format, ...)
{
#ifdef TS_DEBUG
    va_list ap;
    va_start(ap, format);
    vfprintf(stderr, format, ap);
    va_end(ap);
#else
    (void)format;
#endif
}

#ifdef HAVE_ARIBB24
 #include <aribb24/aribb24.h>
 #include <aribb24/decoder.h>
#endif

typedef enum arib_modes_e
{
    ARIBMODE_AUTO = -1,
    ARIBMODE_DISABLED = 0,
    ARIBMODE_ENABLED = 1
} arib_modes_e;

/*****************************************************************************
 * Module descriptor
 *****************************************************************************/
static int  Open  ( vlc_object_t * );
static void Close ( vlc_object_t * );

/* TODO
 * - Rename "extra pmt" to "user pmt"
 * - Update extra pmt description
 *      pmt_pid[:pmt_number][=pid_description[,pid_description]]
 *      where pid_description could take 3 forms:
 *          1. pid:pcr (to force the pcr pid)
 *          2. pid:stream_type
 *          3. pid:type=fourcc where type=(video|audio|spu)
 */
#define PMT_TEXT N_("Extra PMT")
#define PMT_LONGTEXT N_( \
  "Allows a user to specify an extra pmt (pmt_pid=pid:stream_type[,...])." )

#define PID_TEXT N_("Set id of ES to PID")
#define PID_LONGTEXT N_("Set the internal ID of each elementary stream" \
                       " handled by VLC to the same value as the PID in" \
                       " the TS stream, instead of 1, 2, 3, etc. Useful to" \
                       " do \'#duplicate{..., select=\"es=<pid>\"}\'.")

#define CSA_TEXT N_("CSA Key")
#define CSA_LONGTEXT N_("CSA encryption key. This must be a " \
  "16 char string (8 hexadecimal bytes).")

#define CSA2_TEXT N_("Second CSA Key")
#define CSA2_LONGTEXT N_("The even CSA encryption key. This must be a " \
  "16 char string (8 hexadecimal bytes).")


#define CPKT_TEXT N_("Packet size in bytes to decrypt")
#define CPKT_LONGTEXT N_("Specify the size of the TS packet to decrypt. " \
    "The decryption routines subtract the TS-header from the value before " \
    "decrypting. " )

#define SPLIT_ES_TEXT N_("Separate sub-streams")
#define SPLIT_ES_LONGTEXT N_( \
    "Separate teletex/dvbs pages into independent ES. " \
    "It can be useful to turn off this option when using stream output." )

#define SEEK_PERCENT_TEXT N_("Seek based on percent not time")
#define SEEK_PERCENT_LONGTEXT N_( \
    "Seek and position based on a percent byte position, not a PCR generated " \
    "time position. If seeking doesn't work property, turn on this option." )

#define PCR_TEXT N_("Trust in-stream PCR")
#define PCR_LONGTEXT N_("Use the stream PCR as a reference.")

static const int const arib_mode_list[] =
  { ARIBMODE_AUTO, ARIBMODE_ENABLED, ARIBMODE_DISABLED };
static const char *const arib_mode_list_text[] =
  { N_("Auto"), N_("Enabled"), N_("Disabled") };

#define SUPPORT_ARIB_TEXT N_("ARIB STD-B24 mode")
#define SUPPORT_ARIB_LONGTEXT N_( \
    "Forces ARIB STD-B24 mode for decoding characters." \
    "This feature affects EPG information and subtitles." )

vlc_module_begin ()
    set_description( N_("MPEG Transport Stream demuxer") )
    set_shortname ( "MPEG-TS" )
    set_category( CAT_INPUT )
    set_subcategory( SUBCAT_INPUT_DEMUX )

    add_string( "ts-extra-pmt", NULL, PMT_TEXT, PMT_LONGTEXT, true )
    add_bool( "ts-trust-pcr", true, PCR_TEXT, PCR_LONGTEXT, true )
        change_safe()
    add_bool( "ts-es-id-pid", true, PID_TEXT, PID_LONGTEXT, true )
        change_safe()
    add_obsolete_string( "ts-out" ) /* since 2.2.0 */
    add_obsolete_integer( "ts-out-mtu" ) /* since 2.2.0 */
    add_string( "ts-csa-ck", NULL, CSA_TEXT, CSA_LONGTEXT, true )
        change_safe()
    add_string( "ts-csa2-ck", NULL, CSA2_TEXT, CSA2_LONGTEXT, true )
        change_safe()
    add_integer( "ts-csa-pkt", 188, CPKT_TEXT, CPKT_LONGTEXT, true )
        change_safe()

    add_bool( "ts-split-es", true, SPLIT_ES_TEXT, SPLIT_ES_LONGTEXT, false )
    add_bool( "ts-seek-percent", false, SEEK_PERCENT_TEXT, SEEK_PERCENT_LONGTEXT, true )

    add_integer( "ts-arib", ARIBMODE_AUTO, SUPPORT_ARIB_TEXT, SUPPORT_ARIB_LONGTEXT, false )
        change_integer_list( arib_mode_list, arib_mode_list_text )

    add_obsolete_bool( "ts-silent" );

    set_capability( "demux", 10 )
    set_callbacks( Open, Close )
    add_shortcut( "ts" )
vlc_module_end ()

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static const char *const ppsz_teletext_type[] = {
 "",
 N_("Teletext"),
 N_("Teletext subtitles"),
 N_("Teletext: additional information"),
 N_("Teletext: program schedule"),
 N_("Teletext subtitles: hearing impaired")
};

typedef struct
{
    uint8_t                 i_objectTypeIndication;
    uint8_t                 i_streamType;

    int                     i_extra;
    uint8_t                 *p_extra;

} decoder_config_descriptor_t;

typedef struct
{
    bool                    b_ok;
    uint16_t                i_es_id;

    char                    *psz_url;

    decoder_config_descriptor_t    dec_descr;

} es_mpeg4_descriptor_t;

#define ES_DESCRIPTOR_COUNT 255
typedef struct
{
    /* IOD */
    char                    *psz_url;

    es_mpeg4_descriptor_t   es_descr[ES_DESCRIPTOR_COUNT];

} iod_descriptor_t;

typedef struct
{
    dvbpsi_handle   handle;
    int             i_version;
    int             i_number;
    int             i_pid_pcr;
    int             i_pid_pmt;
    mtime_t         i_pcr_value;
    /* IOD stuff (mpeg4) */
    iod_descriptor_t *iod;

} ts_prg_psi_t;

typedef struct
{
    /* for special PAT/SDT case */
    dvbpsi_handle   handle; /* PAT/SDT/EIT */
    int             i_pat_version;
    int             i_sdt_version;

    /* For PMT */
    int             i_prg;
    ts_prg_psi_t    **prg;

} ts_psi_t;

typedef enum
{
    TS_ES_DATA_PES,
    TS_ES_DATA_TABLE_SECTION
} ts_es_data_type_t;

typedef enum
{
    TS_PMT_REGISTRATION_NONE = 0,
    TS_PMT_REGISTRATION_HDMV
} ts_pmt_registration_type_t;

typedef struct
{
    es_format_t  fmt;
    es_out_id_t *id;
    ts_es_data_type_t data_type;
    int         i_data_size;
    int         i_data_gathered;
    block_t     *p_data;
    block_t     **pp_last;

    es_mpeg4_descriptor_t *p_mpeg4desc;

} ts_es_t;

typedef struct
{
    int         i_pid;

    bool        b_seen;
    bool        b_valid;
    int         i_cc;   /* countinuity counter */
    bool        b_scrambled;

    /* PSI owner (ie PMT -> PAT, ES -> PMT */
    ts_psi_t   *p_owner;
    int         i_owner_number;

    /* */
    ts_psi_t    *psi;
    ts_es_t     *es;

    /* Some private streams encapsulate several ES (eg. DVB subtitles)*/
    ts_es_t     **extra_es;
    int         i_extra_es;

} ts_pid_t;

struct demux_sys_t
{
    vlc_mutex_t     csa_lock;

    /* TS packet size (188, 192, 204) */
    int         i_packet_size;

    /* Additional TS packet header size (BluRay TS packets have 4-byte header before sync byte) */
    int         i_packet_header_size;

    /* how many TS packet we read at once */
    int         i_ts_read;

    /* to determine length and time */
    int         i_pid_ref_pcr;
    mtime_t     i_first_pcr;
    mtime_t     i_current_pcr;
    mtime_t     i_last_pcr;
    bool        b_force_seek_per_percent;
    int         i_pcrs_num;
    mtime_t     *p_pcrs;
    int64_t     *p_pos;

    struct
    {
        arib_modes_e e_mode;
#ifdef HAVE_ARIBB24
        arib_instance_t *p_instance;
#endif
    } arib;

    /* All pid */
    ts_pid_t    pid[8192];

    /* All PMT */
    bool        b_user_pmt;
    int         i_pmt;
    ts_pid_t    **pmt;
    int         i_pmt_es;

    /* */
    bool        b_es_id_pid;
    csa_t       *csa;
    int         i_csa_pkt_size;
    bool        b_split_es;

    bool        b_trust_pcr;

    /* */
    bool        b_access_control;

    /* */
    bool        b_dvb_meta;
    int64_t     i_tdt_delta;
    int64_t     i_dvb_start;
    int64_t     i_dvb_length;
    bool        b_broken_charset; /* True if broken encoding is used in EPG/SDT */

    /* */
    int         i_current_program;
    vlc_list_t  programs_list;

    /* */
    bool        b_start_record;
};

static int Demux    ( demux_t *p_demux );
static int Control( demux_t *p_demux, int i_query, va_list args );

static void PIDInit ( ts_pid_t *pid, bool b_psi, ts_psi_t *p_owner );
static void PIDClean( demux_t *, ts_pid_t *pid );
static void PIDFillFormat( es_format_t *fmt, int i_stream_type );

static void PATCallBack( void*, dvbpsi_pat_t * );
static void PMTCallBack( void *data, dvbpsi_pmt_t *p_pmt );
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
static void PSINewTableCallBack( dvbpsi_t *handle, uint8_t  i_table_id,
                                 uint16_t i_extension, demux_t * );
#else
static void PSINewTableCallBack( demux_t *, dvbpsi_handle,
                                 uint8_t  i_table_id, uint16_t i_extension );
#endif

static int ChangeKeyCallback( vlc_object_t *, char const *, vlc_value_t, vlc_value_t, void * );

static inline int PIDGet( block_t *p )
{
    return ( (p->p_buffer[1]&0x1f)<<8 )|p->p_buffer[2];
}

static bool GatherData( demux_t *p_demux, ts_pid_t *pid, block_t *p_bk );

static block_t* ReadTSPacket( demux_t *p_demux );
static int Seek( demux_t *p_demux, double f_percent );
static void GetFirstPCR( demux_t *p_demux );
static void GetLastPCR( demux_t *p_demux );
static void CheckPCR( demux_t *p_demux );
static void PCRHandle( demux_t *p_demux, ts_pid_t *, block_t * );

static void              IODFree( iod_descriptor_t * );

#define TS_USER_PMT_NUMBER (0)
static int UserPmt( demux_t *p_demux, const char * );

static int  SetPIDFilter( demux_t *, int i_pid, bool b_selected );
static void SetPrgFilter( demux_t *, int i_prg, bool b_selected );

#define TS_PACKET_SIZE_188 188
#define TS_PACKET_SIZE_192 192
#define TS_PACKET_SIZE_204 204
#define TS_PACKET_SIZE_MAX 204

static int DetectPacketSize( demux_t *p_demux, int *pi_header_size )
{
    const uint8_t *p_peek;
    if( stream_Peek( p_demux->s,
                     &p_peek, TS_PACKET_SIZE_MAX ) < TS_PACKET_SIZE_MAX )
        return -1;

    *pi_header_size = 0;

    if( memcmp( p_peek, "TFrc", 4 ) == 0 )
    {
#if 0
        /* I used the TF5000PVR 2004 Firmware .doc header documentation,
         * http://www.i-topfield.com/data/product/firmware/Structure%20of%20Recorded%20File%20in%20TF5000PVR%20(Feb%2021%202004).doc
         * but after the filename the offsets seem to be incorrect.  - DJ */
        int i_duration, i_name;
        char *psz_name = xmalloc(25);
        char *psz_event_name;
        char *psz_event_text = xmalloc(130);
        char *psz_ext_text = xmalloc(1025);

        // 2 bytes version Uimsbf (4,5)
        // 2 bytes reserved (6,7)
        // 2 bytes duration in minutes Uimsbf (8,9(
        i_duration = (int) (p_peek[8] << 8) | p_peek[9];
        msg_Dbg( p_demux, "Topfield recording length: +/- %d minutes", i_duration);
        // 2 bytes service number in channel list (10, 11)
        // 2 bytes service type Bslbf 0=TV 1=Radio Bslb (12, 13)
        // 4 bytes of reserved + tuner info (14,15,16,17)
        // 2 bytes of Service ID  Bslbf (18,19)
        // 2 bytes of PMT PID  Uimsbf (20,21)
        // 2 bytes of PCR PID  Uimsbf (22,23)
        // 2 bytes of Video PID  Uimsbf (24,25)
        // 2 bytes of Audio PID  Uimsbf (26,27)
        // 24 bytes filename Bslbf
        memcpy( psz_name, &p_peek[28], 24 );
        psz_name[24] = '\0';
        msg_Dbg( p_demux, "recordingname=%s", psz_name );
        // 1 byte of sat index Uimsbf  (52)
        // 3 bytes (1 bit of polarity Bslbf +23 bits reserved)
        // 4 bytes of freq. Uimsbf (56,57,58,59)
        // 2 bytes of symbol rate Uimsbf (60,61)
        // 2 bytes of TS stream ID Uimsbf (62,63)
        // 4 bytes reserved
        // 2 bytes reserved
        // 2 bytes duration Uimsbf (70,71)
        //i_duration = (int) (p_peek[70] << 8) | p_peek[71];
        //msg_Dbg( p_demux, "Topfield 2nd duration field: +/- %d minutes", i_duration);
        // 4 bytes EventID Uimsbf (72-75)
        // 8 bytes of Start and End time info (76-83)
        // 1 byte reserved (84)
        // 1 byte event name length Uimsbf (89)
        i_name = (int)(p_peek[89]&~0x81);
        msg_Dbg( p_demux, "event name length = %d", i_name);
        psz_event_name = xmalloc( i_name+1 );
        // 1 byte parental rating (90)
        // 129 bytes of event text
        memcpy( psz_event_name, &p_peek[91], i_name );
        psz_event_name[i_name] = '\0';
        memcpy( psz_event_text, &p_peek[91+i_name], 129-i_name );
        psz_event_text[129-i_name] = '\0';
        msg_Dbg( p_demux, "event name=%s", psz_event_name );
        msg_Dbg( p_demux, "event text=%s", psz_event_text );
        // 12 bytes reserved (220)
        // 6 bytes reserved
        // 2 bytes Event Text Length Uimsbf
        // 4 bytes EventID Uimsbf
        // FIXME We just have 613 bytes. not enough for this entire text
        // 1024 bytes Extended Event Text Bslbf
        memcpy( psz_ext_text, p_peek+372, 1024 );
        psz_ext_text[1024] = '\0';
        msg_Dbg( p_demux, "extended event text=%s", psz_ext_text );
        // 52 bytes reserved Bslbf
#endif
        msg_Dbg( p_demux, "this is a topfield file" );
        return TS_PACKET_SIZE_188;
    }

    for( int i_sync = 0; i_sync < TS_PACKET_SIZE_MAX; i_sync++ )
    {
        if( p_peek[i_sync] != 0x47 )
            continue;

        /* Check next 3 sync bytes */
        int i_peek = TS_PACKET_SIZE_MAX * 3 + i_sync + 1;
        if( ( stream_Peek( p_demux->s, &p_peek, i_peek ) ) < i_peek )
        {
            msg_Err( p_demux, "cannot peek" );
            return -1;
        }
        if( p_peek[i_sync + 1 * TS_PACKET_SIZE_188] == 0x47 &&
            p_peek[i_sync + 2 * TS_PACKET_SIZE_188] == 0x47 &&
            p_peek[i_sync + 3 * TS_PACKET_SIZE_188] == 0x47 )
        {
            return TS_PACKET_SIZE_188;
        }
        else if( p_peek[i_sync + 1 * TS_PACKET_SIZE_192] == 0x47 &&
                 p_peek[i_sync + 2 * TS_PACKET_SIZE_192] == 0x47 &&
                 p_peek[i_sync + 3 * TS_PACKET_SIZE_192] == 0x47 )
        {
            if( i_sync == 4 )
            {
                *pi_header_size = 4; /* BluRay TS packets have 4-byte header */
            }
            return TS_PACKET_SIZE_192;
        }
        else if( p_peek[i_sync + 1 * TS_PACKET_SIZE_204] == 0x47 &&
                 p_peek[i_sync + 2 * TS_PACKET_SIZE_204] == 0x47 &&
                 p_peek[i_sync + 3 * TS_PACKET_SIZE_204] == 0x47 )
        {
            return TS_PACKET_SIZE_204;
        }
    }

    if( p_demux->b_force )
    {
        msg_Warn( p_demux, "this does not look like a TS stream, continuing" );
        return TS_PACKET_SIZE_188;
    }
    msg_Dbg( p_demux, "TS module discarded (lost sync)" );
    return -1;
}

#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
static void vlc_dvbpsi_reset( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    ts_pid_t *pat = &p_sys->pid[0];
    ts_pid_t *sdt = &p_sys->pid[0x11];
    ts_pid_t *eit = &p_sys->pid[0x12];
    ts_pid_t *tdt = &p_sys->pid[0x14];

    if( pat->psi->handle )
    {
        if( dvbpsi_decoder_present( pat->psi->handle ) )
            dvbpsi_pat_detach( pat->psi->handle );
        dvbpsi_delete( pat->psi->handle );
        pat->psi->handle = NULL;
    }

    if( sdt->psi->handle )
    {
        if( dvbpsi_decoder_present( sdt->psi->handle ) )
            dvbpsi_DetachDemux( sdt->psi->handle );
        dvbpsi_delete( sdt->psi->handle );
        sdt->psi->handle = NULL;
    }

    if( eit->psi->handle )
    {
        if( dvbpsi_decoder_present( eit->psi->handle ) )
            dvbpsi_DetachDemux( eit->psi->handle );
        dvbpsi_delete( eit->psi->handle );
        eit->psi->handle = NULL;
    }

    if( tdt->psi->handle )
    {
        if( dvbpsi_decoder_present( tdt->psi->handle ) )
            dvbpsi_DetachDemux( tdt->psi->handle );
        dvbpsi_delete( tdt->psi->handle );
        tdt->psi->handle = NULL;
    }
}
#endif

/*****************************************************************************
 * Open
 *****************************************************************************/
static int Open( vlc_object_t *p_this )
{
    demux_t     *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys;

    int          i_packet_size, i_packet_header_size = 0;

    ts_pid_t    *pat;

    /* Search first sync byte */
    i_packet_size = DetectPacketSize( p_demux, &i_packet_header_size );
    if( i_packet_size < 0 )
        return VLC_EGENERIC;

    p_demux->p_sys = p_sys = malloc( sizeof( demux_sys_t ) );
    if( !p_sys )
        return VLC_ENOMEM;
    memset( p_sys, 0, sizeof( demux_sys_t ) );
    vlc_mutex_init( &p_sys->csa_lock );

    p_demux->pf_demux = Demux;
    p_demux->pf_control = Control;

    /* Init p_sys field */
    p_sys->b_dvb_meta = true;
    p_sys->b_access_control = true;
    p_sys->i_current_program = 0;
    p_sys->programs_list.i_count = 0;
    p_sys->programs_list.p_values = NULL;
    p_sys->i_tdt_delta = 0;
    p_sys->i_dvb_start = 0;
    p_sys->i_dvb_length = 0;

    p_sys->b_broken_charset = false;

    for( int i = 0; i < 8192; i++ )
    {
        ts_pid_t *pid = &p_sys->pid[i];
        pid->i_pid      = i;
        pid->b_seen     = false;
        pid->b_valid    = false;
    }
    /* PID 8191 is padding */
    p_sys->pid[8191].b_seen = true;
    p_sys->i_packet_size = i_packet_size;
    p_sys->i_packet_header_size = i_packet_header_size;
    p_sys->i_ts_read = 50;
    p_sys->csa = NULL;
    p_sys->b_start_record = false;

#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
# define VLC_DVBPSI_DEMUX_TABLE_INIT(table,obj) \
    do { \
        (table)->psi->handle = dvbpsi_new( &dvbpsi_messages, DVBPSI_MSG_DEBUG ); \
        if( ! (table)->psi->handle ) \
        { \
            vlc_mutex_destroy( &p_sys->csa_lock ); \
            free( p_sys ); \
            return VLC_ENOMEM; \
        } \
        (table)->psi->handle->p_sys = (void *) VLC_OBJECT(obj); \
        if( !dvbpsi_AttachDemux( (table)->psi->handle, (dvbpsi_demux_new_cb_t)PSINewTableCallBack, (obj) ) ) \
        { \
            vlc_dvbpsi_reset( obj ); \
            vlc_mutex_destroy( &p_sys->csa_lock ); \
            free( p_sys ); \
            return VLC_EGENERIC; \
        } \
    } while (0);
#endif

    /* Init PAT handler */
    pat = &p_sys->pid[0];
    PIDInit( pat, true, NULL );
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
    pat->psi->handle = dvbpsi_new( &dvbpsi_messages, DVBPSI_MSG_DEBUG );
    if( !pat->psi->handle )
    {
        vlc_mutex_destroy( &p_sys->csa_lock );
        free( p_sys );
        return VLC_ENOMEM;
    }
    pat->psi->handle->p_sys = (void *) p_demux;
    if( !dvbpsi_pat_attach( pat->psi->handle, PATCallBack, p_demux ) )
    {
        vlc_dvbpsi_reset( p_demux );
        vlc_mutex_destroy( &p_sys->csa_lock );
        free( p_sys );
        return VLC_EGENERIC;
    }
#else
    pat->psi->handle = dvbpsi_AttachPAT( PATCallBack, p_demux );
#endif
    if( p_sys->b_dvb_meta )
    {
        ts_pid_t *sdt = &p_sys->pid[0x11];
        ts_pid_t *eit = &p_sys->pid[0x12];

        PIDInit( sdt, true, NULL );
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
        VLC_DVBPSI_DEMUX_TABLE_INIT( sdt, p_demux )
#else
        sdt->psi->handle =
            dvbpsi_AttachDemux( (dvbpsi_demux_new_cb_t)PSINewTableCallBack,
                                p_demux );
#endif
        PIDInit( eit, true, NULL );
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
        VLC_DVBPSI_DEMUX_TABLE_INIT( eit, p_demux )
#else
        eit->psi->handle =
            dvbpsi_AttachDemux( (dvbpsi_demux_new_cb_t)PSINewTableCallBack,
                                p_demux );
#endif
        ts_pid_t *tdt = &p_sys->pid[0x14];
        PIDInit( tdt, true, NULL );
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
        VLC_DVBPSI_DEMUX_TABLE_INIT( tdt, p_demux )
#else
        tdt->psi->handle =
            dvbpsi_AttachDemux( (dvbpsi_demux_new_cb_t)PSINewTableCallBack,
                                p_demux );
#endif

        if( p_sys->b_access_control )
        {
            if( SetPIDFilter( p_demux, 0x11, true ) ||
                SetPIDFilter( p_demux, 0x14, true ) ||
                SetPIDFilter( p_demux, 0x12, true ) )
                p_sys->b_access_control = false;
        }
    }

#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
# undef VLC_DVBPSI_DEMUX_TABLE_INIT
#endif

    /* Init PMT array */
    TAB_INIT( p_sys->i_pmt, p_sys->pmt );
    p_sys->i_pmt_es = 0;

    /* Read config */
    p_sys->b_es_id_pid = var_CreateGetBool( p_demux, "ts-es-id-pid" );

    p_sys->b_trust_pcr = var_CreateGetBool( p_demux, "ts-trust-pcr" );

    /* We handle description of an extra PMT */
    char* psz_string = var_CreateGetString( p_demux, "ts-extra-pmt" );
    p_sys->b_user_pmt = false;
    if( psz_string && *psz_string )
        UserPmt( p_demux, psz_string );
    free( psz_string );

    psz_string = var_CreateGetStringCommand( p_demux, "ts-csa-ck" );
    if( psz_string && *psz_string )
    {
        int i_res;
        char* psz_csa2;

        p_sys->csa = csa_New();

        psz_csa2 = var_CreateGetStringCommand( p_demux, "ts-csa2-ck" );
        i_res = csa_SetCW( (vlc_object_t*)p_demux, p_sys->csa, psz_string, true );
        if( i_res == VLC_SUCCESS && psz_csa2 && *psz_csa2 )
        {
            if( csa_SetCW( (vlc_object_t*)p_demux, p_sys->csa, psz_csa2, false ) != VLC_SUCCESS )
            {
                csa_SetCW( (vlc_object_t*)p_demux, p_sys->csa, psz_string, false );
            }
        }
        else if ( i_res == VLC_SUCCESS )
        {
            csa_SetCW( (vlc_object_t*)p_demux, p_sys->csa, psz_string, false );
        }
        else
        {
            csa_Delete( p_sys->csa );
            p_sys->csa = NULL;
        }

        if( p_sys->csa )
        {
            var_AddCallback( p_demux, "ts-csa-ck", ChangeKeyCallback, (void *)1 );
            var_AddCallback( p_demux, "ts-csa2-ck", ChangeKeyCallback, NULL );

            int i_pkt = var_CreateGetInteger( p_demux, "ts-csa-pkt" );
            if( i_pkt < 4 || i_pkt > 188 )
            {
                msg_Err( p_demux, "wrong packet size %d specified.", i_pkt );
                msg_Warn( p_demux, "using default packet size of 188 bytes" );
                p_sys->i_csa_pkt_size = 188;
            }
            else
                p_sys->i_csa_pkt_size = i_pkt;
            msg_Dbg( p_demux, "decrypting %d bytes of packet", p_sys->i_csa_pkt_size );
        }
        free( psz_csa2 );
    }
    free( psz_string );

    p_sys->b_split_es = var_InheritBool( p_demux, "ts-split-es" );

    p_sys->i_pid_ref_pcr = -1;
    p_sys->i_first_pcr = -1;
    p_sys->i_current_pcr = -1;
    p_sys->i_last_pcr = -1;
    p_sys->b_force_seek_per_percent = var_InheritBool( p_demux, "ts-seek-percent" );
    p_sys->i_pcrs_num = 10;
    p_sys->p_pcrs = (mtime_t *)calloc( p_sys->i_pcrs_num, sizeof( mtime_t ) );
    p_sys->p_pos = (int64_t *)calloc( p_sys->i_pcrs_num, sizeof( int64_t ) );

    p_sys->arib.e_mode = var_InheritInteger( p_demux, "ts-arib" );

    if( !p_sys->p_pcrs || !p_sys->p_pos )
    {
        Close( p_this );
        return VLC_ENOMEM;
    }

    bool can_seek = false;
    stream_Control( p_demux->s, STREAM_CAN_FASTSEEK, &can_seek );
    if( can_seek  )
    {
        GetFirstPCR( p_demux );
        CheckPCR( p_demux );
        GetLastPCR( p_demux );
    }
    if( p_sys->i_first_pcr < 0 || p_sys->i_last_pcr < 0 )
    {
        msg_Dbg( p_demux, "Force Seek Per Percent: PCR's not found,");
        p_sys->b_force_seek_per_percent = true;
    }

    while( p_sys->i_pmt_es <= 0 && vlc_object_alive( p_demux ) )
    {
        if( Demux( p_demux ) != 1 )
            break;
    }
    return VLC_SUCCESS;
}

/*****************************************************************************
 * Close
 *****************************************************************************/
static void Close( vlc_object_t *p_this )
{
    demux_t     *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys = p_demux->p_sys;

    msg_Dbg( p_demux, "pid list:" );
    for( int i = 0; i < 8192; i++ )
    {
        ts_pid_t *pid = &p_sys->pid[i];

        if( pid->b_valid && pid->psi )
        {
            switch( pid->i_pid )
            {
            case 0: /* PAT */
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
                if( dvbpsi_decoder_present( pid->psi->handle ) )
                    dvbpsi_pat_detach( pid->psi->handle );
                dvbpsi_delete( pid->psi->handle );
                pid->psi->handle = NULL;
#else
                dvbpsi_DetachPAT( pid->psi->handle );
#endif
                free( pid->psi );
                break;
            case 1: /* CAT */
                free( pid->psi );
                break;
            default:
                if( p_sys->b_dvb_meta && ( pid->i_pid == 0x11 || pid->i_pid == 0x12 || pid->i_pid == 0x14 ) )
                {
                    /* SDT or EIT or TDT */
                    dvbpsi_DetachDemux( pid->psi->handle );
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
                    dvbpsi_delete( pid->psi->handle );
                    pid->psi->handle = NULL;
#endif
                    free( pid->psi );
                }
                else
                {
                    PIDClean( p_demux, pid );
                }
                break;
            }
        }
        else if( pid->b_valid && pid->es )
        {
            PIDClean( p_demux, pid );
        }

        if( pid->b_seen )
        {
            msg_Dbg( p_demux, "  - pid[%d] seen", pid->i_pid );
        }

        /* too much */
        if( pid->i_pid > 0 )
            SetPIDFilter( p_demux, pid->i_pid, false );
    }

    vlc_mutex_lock( &p_sys->csa_lock );
    if( p_sys->csa )
    {
        var_DelCallback( p_demux, "ts-csa-ck", ChangeKeyCallback, NULL );
        var_DelCallback( p_demux, "ts-csa2-ck", ChangeKeyCallback, NULL );
        csa_Delete( p_sys->csa );
    }
    vlc_mutex_unlock( &p_sys->csa_lock );

    TAB_CLEAN( p_sys->i_pmt, p_sys->pmt );

    free( p_sys->programs_list.p_values );

    free( p_sys->p_pcrs );
    free( p_sys->p_pos );

#ifdef HAVE_ARIBB24
    if ( p_sys->arib.p_instance )
        arib_instance_destroy( p_sys->arib.p_instance );
#endif

    vlc_mutex_destroy( &p_sys->csa_lock );
    free( p_sys );
}

/*****************************************************************************
 * ChangeKeyCallback: called when changing the odd encryption key on the fly.
 *****************************************************************************/
static int ChangeKeyCallback( vlc_object_t *p_this, char const *psz_cmd,
                           vlc_value_t oldval, vlc_value_t newval,
                           void *p_data )
{
    VLC_UNUSED(psz_cmd); VLC_UNUSED(oldval);
    demux_t     *p_demux = (demux_t*)p_this;
    demux_sys_t *p_sys = p_demux->p_sys;
    int         i_tmp = (intptr_t)p_data;

    vlc_mutex_lock( &p_sys->csa_lock );
    if ( i_tmp )
        i_tmp = csa_SetCW( p_this, p_sys->csa, newval.psz_string, true );
    else
        i_tmp = csa_SetCW( p_this, p_sys->csa, newval.psz_string, false );

    vlc_mutex_unlock( &p_sys->csa_lock );
    return i_tmp;
}

/*****************************************************************************
 * Demux:
 *****************************************************************************/
static int Demux( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    bool b_wait_es = p_sys->i_pmt_es <= 0;

    /* We read at most 100 TS packet or until a frame is completed */
    for( int i_pkt = 0; i_pkt < p_sys->i_ts_read; i_pkt++ )
    {
        bool         b_frame = false;
        block_t     *p_pkt;
        if( !(p_pkt = ReadTSPacket( p_demux )) )
        {
            return 0;
        }

        if( p_sys->b_start_record )
        {
            /* Enable recording once synchronized */
            stream_Control( p_demux->s, STREAM_SET_RECORD_STATE, true, "ts" );
            p_sys->b_start_record = false;
        }

        /* Parse the TS packet */
        ts_pid_t *p_pid = &p_sys->pid[PIDGet( p_pkt )];

        if( p_pid->b_valid )
        {
            if( p_pid->psi )
            {
                if( p_pid->i_pid == 0 || ( p_sys->b_dvb_meta && ( p_pid->i_pid == 0x11 || p_pid->i_pid == 0x12 || p_pid->i_pid == 0x14 ) ) )
                {
                    dvbpsi_PushPacket( p_pid->psi->handle, p_pkt->p_buffer );
                }
                else
                {
                    for( int i_prg = 0; i_prg < p_pid->psi->i_prg; i_prg++ )
                    {
                        dvbpsi_PushPacket( p_pid->psi->prg[i_prg]->handle,
                                           p_pkt->p_buffer );
                    }
                }
                block_Release( p_pkt );
            }
            else
            {
                b_frame = GatherData( p_demux, p_pid, p_pkt );
            }
        }
        else
        {
            if( !p_pid->b_seen )
            {
                msg_Dbg( p_demux, "pid[%d] unknown", p_pid->i_pid );
            }
            /* We have to handle PCR if present */
            PCRHandle( p_demux, p_pid, p_pkt );
            block_Release( p_pkt );
        }
        p_pid->b_seen = true;

        if( b_frame || ( b_wait_es && p_sys->i_pmt_es > 0 ) )
            break;
    }

    demux_UpdateTitleFromStream( p_demux );
    return 1;
}

/*****************************************************************************
 * Control:
 *****************************************************************************/
static int DVBEventInformation( demux_t *p_demux, int64_t *pi_time, int64_t *pi_length )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    if( pi_length )
        *pi_length = 0;
    if( pi_time )
        *pi_time = 0;

    if( p_sys->i_dvb_length > 0 )
    {
        const int64_t t = mdate() + p_sys->i_tdt_delta;

        if( p_sys->i_dvb_start <= t && t < p_sys->i_dvb_start + p_sys->i_dvb_length )
        {
            if( pi_length )
                *pi_length = p_sys->i_dvb_length;
            if( pi_time )
                *pi_time   = t - p_sys->i_dvb_start;
            return VLC_SUCCESS;
        }
    }
    return VLC_EGENERIC;
}

static int Control( demux_t *p_demux, int i_query, va_list args )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    double f, *pf;
    bool b_bool, *pb_bool;
    int64_t i64;
    int64_t *pi64;
    int i_int;

    switch( i_query )
    {
    case DEMUX_GET_POSITION:
        pf = (double*) va_arg( args, double* );

        if( p_sys->b_force_seek_per_percent ||
            (p_sys->b_dvb_meta && p_sys->b_access_control) ||
            p_sys->i_current_pcr - p_sys->i_first_pcr < 0 ||
            p_sys->i_last_pcr - p_sys->i_first_pcr <= 0 )
        {
            int64_t i_time, i_length;
            if( !DVBEventInformation( p_demux, &i_time, &i_length ) && i_length > 0 )
                *pf = (double)i_time/(double)i_length;
            else if( (i64 = stream_Size( p_demux->s) ) > 0 )
            {
                int64_t offset = stream_Tell( p_demux->s );

                *pf = (double)offset / (double)i64;
            }
            else
                *pf = 0.0;
        }
        else
        {
            *pf = (double)(p_sys->i_current_pcr - p_sys->i_first_pcr) / (double)(p_sys->i_last_pcr - p_sys->i_first_pcr);
        }
        return VLC_SUCCESS;

    case DEMUX_SET_POSITION:
        f = (double) va_arg( args, double );

        if( p_sys->b_force_seek_per_percent ||
            (p_sys->b_dvb_meta && p_sys->b_access_control) ||
            p_sys->i_last_pcr - p_sys->i_first_pcr <= 0 )
        {
            i64 = stream_Size( p_demux->s );
            if( stream_Seek( p_demux->s, (int64_t)(i64 * f) ) )
                return VLC_EGENERIC;
        }
        else
        {
            if( Seek( p_demux, f ) )
            {
                p_sys->b_force_seek_per_percent = true;
                return VLC_EGENERIC;
            }
        }
        return VLC_SUCCESS;

    case DEMUX_GET_TIME:
        pi64 = (int64_t*)va_arg( args, int64_t * );
        if( (p_sys->b_dvb_meta && p_sys->b_access_control) ||
            p_sys->b_force_seek_per_percent ||
            p_sys->i_current_pcr - p_sys->i_first_pcr < 0 )
        {
            if( DVBEventInformation( p_demux, pi64, NULL ) )
            {
                *pi64 = 0;
            }
        }
        else
        {
            *pi64 = (p_sys->i_current_pcr - p_sys->i_first_pcr) * 100 / 9;
        }
        return VLC_SUCCESS;

    case DEMUX_GET_LENGTH:
        pi64 = (int64_t*)va_arg( args, int64_t * );
        if( (p_sys->b_dvb_meta && p_sys->b_access_control) ||
            p_sys->b_force_seek_per_percent ||
            p_sys->i_last_pcr - p_sys->i_first_pcr <= 0 )
        {
            if( DVBEventInformation( p_demux, NULL, pi64 ) )
            {
                *pi64 = 0;
            }
        }
        else
        {
            *pi64 = (p_sys->i_last_pcr - p_sys->i_first_pcr) * 100 / 9;
        }
        return VLC_SUCCESS;

    case DEMUX_SET_GROUP:
    {
        vlc_list_t *p_list;

        i_int = (int)va_arg( args, int );
        p_list = (vlc_list_t *)va_arg( args, vlc_list_t * );
        msg_Dbg( p_demux, "DEMUX_SET_GROUP %d %p", i_int, p_list );

        if( i_int == 0 && p_sys->i_current_program > 0 )
            i_int = p_sys->i_current_program;

        if( p_sys->i_current_program > 0 )
        {
            if( p_sys->i_current_program != i_int )
                SetPrgFilter( p_demux, p_sys->i_current_program, false );
        }
        else if( p_sys->i_current_program < 0 )
        {
            for( int i = 0; i < p_sys->programs_list.i_count; i++ )
                SetPrgFilter( p_demux, p_sys->programs_list.p_values[i].i_int, false );
        }

        if( i_int > 0 )
        {
            p_sys->i_current_program = i_int;
            SetPrgFilter( p_demux, p_sys->i_current_program, true );
        }
        else if( i_int < 0 )
        {
            p_sys->i_current_program = -1;
            p_sys->programs_list.i_count = 0;
            if( p_list )
            {
                vlc_list_t *p_dst = &p_sys->programs_list;
                free( p_dst->p_values );

                p_dst->p_values = calloc( p_list->i_count,
                                          sizeof(*p_dst->p_values) );
                if( p_dst->p_values )
                {
                    p_dst->i_count = p_list->i_count;
                    for( int i = 0; i < p_list->i_count; i++ )
                    {
                        p_dst->p_values[i] = p_list->p_values[i];
                        SetPrgFilter( p_demux, p_dst->p_values[i].i_int, true );
                    }
                }
            }
        }
        return VLC_SUCCESS;
    }

    case DEMUX_GET_TITLE_INFO:
    {
        struct input_title_t ***v = va_arg( args, struct input_title_t*** );
        int *c = va_arg( args, int * );

        *va_arg( args, int* ) = 0; /* Title offset */
        *va_arg( args, int* ) = 0; /* Chapter offset */
        return stream_Control( p_demux->s, STREAM_GET_TITLE_INFO, v, c );
    }

    case DEMUX_SET_TITLE:
        return stream_vaControl( p_demux->s, STREAM_SET_TITLE, args );

    case DEMUX_SET_SEEKPOINT:
        return stream_vaControl( p_demux->s, STREAM_SET_SEEKPOINT, args );

    case DEMUX_GET_META:
        return stream_vaControl( p_demux->s, STREAM_GET_META, args );

    case DEMUX_CAN_RECORD:
        pb_bool = (bool*)va_arg( args, bool * );
        *pb_bool = true;
        return VLC_SUCCESS;

    case DEMUX_SET_RECORD_STATE:
        b_bool = (bool)va_arg( args, int );

        if( !b_bool )
            stream_Control( p_demux->s, STREAM_SET_RECORD_STATE, false );
        p_sys->b_start_record = b_bool;
        return VLC_SUCCESS;

    case DEMUX_GET_SIGNAL:
        return stream_vaControl( p_demux->s, STREAM_GET_SIGNAL, args );

    default:
        return VLC_EGENERIC;
    }
}

/*****************************************************************************
 *
 *****************************************************************************/
static int UserPmt( demux_t *p_demux, const char *psz_fmt )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    char *psz_dup = strdup( psz_fmt );
    char *psz = psz_dup;
    int  i_pid;
    int  i_number;
    ts_prg_psi_t *prg = NULL;

    if( !psz_dup )
        return VLC_ENOMEM;

    /* Parse PID */
    i_pid = strtol( psz, &psz, 0 );
    if( i_pid < 2 || i_pid >= 8192 )
        goto error;

    /* Parse optional program number */
    i_number = 0;
    if( *psz == ':' )
        i_number = strtol( &psz[1], &psz, 0 );

    /* */
    ts_pid_t *pmt = &p_sys->pid[i_pid];

    msg_Dbg( p_demux, "user pmt specified (pid=%d,number=%d)", i_pid, i_number );
    PIDInit( pmt, true, NULL );

    /* Dummy PMT */
    prg = calloc( 1, sizeof( ts_prg_psi_t ) );
    if( !prg )
        goto error;

    prg->i_pid_pcr  = -1;
    prg->i_pid_pmt  = -1;
    prg->i_version  = -1;
    prg->i_number   = i_number != 0 ? i_number : TS_USER_PMT_NUMBER;
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
    prg->handle = dvbpsi_new( &dvbpsi_messages, DVBPSI_MSG_DEBUG );
    if( !prg->handle )
        goto error;
    prg->handle->p_sys = (void *) VLC_OBJECT(p_demux);
    if( !dvbpsi_pmt_attach( prg->handle,
                            ((i_number != TS_USER_PMT_NUMBER ? i_number : 1)),
                            PMTCallBack, p_demux ) )
    {
        dvbpsi_delete( prg->handle );
        prg->handle = NULL;
        goto error;
    }
#else
    prg->handle     = dvbpsi_AttachPMT(
        ((i_number != TS_USER_PMT_NUMBER) ? i_number : 1),
        PMTCallBack, p_demux );
#endif
    TAB_APPEND( pmt->psi->i_prg, pmt->psi->prg, prg );

    psz = strchr( psz, '=' );
    if( psz )
        psz++;
    while( psz && *psz )
    {
        char *psz_next = strchr( psz, ',' );
        int i_pid;

        if( psz_next )
            *psz_next++ = '\0';

        i_pid = strtol( psz, &psz, 0 );
        if( *psz != ':' || i_pid < 2 || i_pid >= 8192 )
            goto next;

        char *psz_opt = &psz[1];
        if( !strcmp( psz_opt, "pcr" ) )
        {
            prg->i_pid_pcr = i_pid;
        }
        else if( !p_sys->pid[i_pid].b_valid )
        {
            ts_pid_t *pid = &p_sys->pid[i_pid];

            char *psz_arg = strchr( psz_opt, '=' );
            if( psz_arg )
                *psz_arg++ = '\0';

            PIDInit( pid, false, pmt->psi);
            if( prg->i_pid_pcr <= 0 )
                prg->i_pid_pcr = i_pid;

            if( psz_arg && strlen( psz_arg ) == 4 )
            {
                const vlc_fourcc_t i_codec = VLC_FOURCC( psz_arg[0], psz_arg[1],
                                                         psz_arg[2], psz_arg[3] );
                int i_cat = UNKNOWN_ES;
                es_format_t *fmt = &pid->es->fmt;

                if( !strcmp( psz_opt, "video" ) )
                    i_cat = VIDEO_ES;
                else if( !strcmp( psz_opt, "audio" ) )
                    i_cat = AUDIO_ES;
                else if( !strcmp( psz_opt, "spu" ) )
                    i_cat = SPU_ES;

                es_format_Init( fmt, i_cat, i_codec );
                fmt->b_packetized = false;
            }
            else
            {
                const int i_stream_type = strtol( psz_opt, NULL, 0 );
                PIDFillFormat( &pid->es->fmt, i_stream_type );
            }
            pid->es->fmt.i_group = i_number;
            if( p_sys->b_es_id_pid )
                pid->es->fmt.i_id = i_pid;

            if( pid->es->fmt.i_cat != UNKNOWN_ES )
            {
                msg_Dbg( p_demux, "  * es pid=%d fcc=%4.4s", i_pid,
                         (char*)&pid->es->fmt.i_codec );
                pid->es->id = es_out_Add( p_demux->out,
                                          &pid->es->fmt );
                p_sys->i_pmt_es++;
            }
        }

    next:
        psz = psz_next;
    }

    p_sys->b_user_pmt = true;
    TAB_APPEND( p_sys->i_pmt, p_sys->pmt, pmt );
    free( psz_dup );
    return VLC_SUCCESS;

error:
    free( prg );
    free( psz_dup );
    return VLC_EGENERIC;
}

static int SetPIDFilter( demux_t *p_demux, int i_pid, bool b_selected )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if( !p_sys->b_access_control )
        return VLC_EGENERIC;

    return stream_Control( p_demux->s, STREAM_SET_PRIVATE_ID_STATE,
                           i_pid, b_selected );
}

static void SetPrgFilter( demux_t *p_demux, int i_prg_id, bool b_selected )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    ts_prg_psi_t *p_prg = NULL;
    int i_pmt_pid = -1;

    /* Search pmt to be unselected */
    for( int i = 0; i < p_sys->i_pmt; i++ )
    {
        ts_pid_t *pmt = p_sys->pmt[i];

        for( int i_prg = 0; i_prg < pmt->psi->i_prg; i_prg++ )
        {
            if( pmt->psi->prg[i_prg]->i_number == i_prg_id )
            {
                i_pmt_pid = p_sys->pmt[i]->i_pid;
                p_prg = p_sys->pmt[i]->psi->prg[i_prg];
                break;
            }
        }
        if( i_pmt_pid > 0 )
            break;
    }
    if( i_pmt_pid <= 0 )
        return;
    assert( p_prg );

    SetPIDFilter( p_demux, i_pmt_pid, b_selected );
    if( p_prg->i_pid_pcr > 0 )
        SetPIDFilter( p_demux, p_prg->i_pid_pcr, b_selected );

    /* All ES */
    for( int i = 2; i < 8192; i++ )
    {
        ts_pid_t *pid = &p_sys->pid[i];

        if( !pid->b_valid || pid->psi )
            continue;

        for( int i_prg = 0; i_prg < pid->p_owner->i_prg; i_prg++ )
        {
            if( pid->p_owner->prg[i_prg]->i_pid_pmt == i_pmt_pid && pid->es->id )
            {
                /* We only remove/select es that aren't defined by extra pmt */
                SetPIDFilter( p_demux, i, b_selected );
                break;
            }
        }
    }
}

static void PIDInit( ts_pid_t *pid, bool b_psi, ts_psi_t *p_owner )
{
    bool b_old_valid = pid->b_valid;

    pid->b_valid    = true;
    pid->i_cc       = 0xff;
    pid->b_scrambled = false;
    pid->p_owner    = p_owner;
    pid->i_owner_number = 0;

    TAB_INIT( pid->i_extra_es, pid->extra_es );

    if( b_psi )
    {
        pid->es  = NULL;

        if( !b_old_valid )
        {
            pid->psi = xmalloc( sizeof( ts_psi_t ) );
            pid->psi->handle = NULL;
            TAB_INIT( pid->psi->i_prg, pid->psi->prg );
        }
        assert( pid->psi );

        pid->psi->i_pat_version  = -1;
        pid->psi->i_sdt_version  = -1;
        if( p_owner )
        {
            ts_prg_psi_t *prg = malloc( sizeof( ts_prg_psi_t ) );
            if( !prg )
                return;
            /* PMT */
            prg->i_version  = -1;
            prg->i_number   = -1;
            prg->i_pid_pcr  = -1;
            prg->i_pid_pmt  = -1;
            prg->i_pcr_value= -1;
            prg->iod        = NULL;
            prg->handle     = NULL;

            TAB_APPEND( pid->psi->i_prg, pid->psi->prg, prg );
        }
    }
    else
    {
        pid->psi = NULL;
        pid->es  = calloc( 1, sizeof( ts_es_t ) );
        if( !pid->es )
            return;

        es_format_Init( &pid->es->fmt, UNKNOWN_ES, 0 );
        pid->es->data_type = TS_ES_DATA_PES;
        pid->es->pp_last = &pid->es->p_data;
    }
}

static void PIDClean( demux_t *p_demux, ts_pid_t *pid )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    es_out_t *out = p_demux->out;

    if( pid->psi )
    {
        if( pid->psi->handle )
        {
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
            if( dvbpsi_decoder_present( pid->psi->handle ) )
                dvbpsi_pmt_detach( pid->psi->handle );
            dvbpsi_delete( pid->psi->handle );
            pid->psi->handle = NULL;
#else
            dvbpsi_DetachPMT( pid->psi->handle );
#endif
        }
        for( int i = 0; i < pid->psi->i_prg; i++ )
        {
            if( pid->psi->prg[i]->iod )
                IODFree( pid->psi->prg[i]->iod );
            if( pid->psi->prg[i]->handle )
            {
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
                if( dvbpsi_decoder_present( pid->psi->prg[i]->handle ) )
                    dvbpsi_pmt_detach( pid->psi->prg[i]->handle );
                dvbpsi_delete( pid->psi->prg[i]->handle );
#else
                dvbpsi_DetachPMT( pid->psi->prg[i]->handle );
#endif
            }
            free( pid->psi->prg[i] );
        }
        free( pid->psi->prg );
        free( pid->psi );
    }
    else
    {
        if( pid->es->id )
        {
            es_out_Del( out, pid->es->id );
            p_sys->i_pmt_es--;
        }

        if( pid->es->p_data )
            block_ChainRelease( pid->es->p_data );

        es_format_Clean( &pid->es->fmt );

        free( pid->es );

        for( int i = 0; i < pid->i_extra_es; i++ )
        {
            if( pid->extra_es[i]->id )
            {
                es_out_Del( out, pid->extra_es[i]->id );
                p_sys->i_pmt_es--;
            }

            if( pid->extra_es[i]->p_data )
                block_ChainRelease( pid->extra_es[i]->p_data );

            es_format_Clean( &pid->extra_es[i]->fmt );

            free( pid->extra_es[i] );
        }
        if( pid->i_extra_es )
            free( pid->extra_es );
    }

    pid->b_valid = false;
}

static int16_t read_opus_flag(uint8_t **buf, size_t *len)
{
    if (*len < 2)
        return -1;

    int16_t ret = ((*buf)[0] << 8) | (*buf)[1];

    *len -= 2;
    *buf += 2;

    if (ret & (3<<13))
        ret = -1;

    return ret;
}

static block_t *Opus_Parse(demux_t *demux, block_t *block)
{
    block_t *out = NULL;
    block_t **last = NULL;

    uint8_t *buf = block->p_buffer;
    size_t len = block->i_buffer;

    while (len > 3 && ((buf[0] << 3) | (buf[1] >> 5)) == 0x3ff) {
        int16_t start_trim = 0, end_trim = 0;
        int start_trim_flag        = (buf[1] >> 4) & 1;
        int end_trim_flag          = (buf[1] >> 3) & 1;
        int control_extension_flag = (buf[1] >> 2) & 1;

        len -= 2;
        buf += 2;

        unsigned au_size = 0;
        while (len--) {
            int c = *buf++;
            au_size += c;
            if (c != 0xff)
                break;
        }

        if (start_trim_flag) {
            start_trim = read_opus_flag(&buf, &len);
            if (start_trim < 0) {
                msg_Err(demux, "Invalid start trimming flag");
            }
        }
        if (end_trim_flag) {
            end_trim = read_opus_flag(&buf, &len);
            if (end_trim < 0) {
                msg_Err(demux, "Invalid end trimming flag");
            }
        }
        if (control_extension_flag && len) {
            unsigned l = *buf++; len--;
            if (l > len) {
                msg_Err(demux, "Invalid control extension length %d > %zu", l, len);
                break;
            }
            buf += l;
            len -= l;
        }

        if (!au_size || au_size > len) {
            msg_Err(demux, "Invalid Opus AU size %d (PES %zu)", au_size, len);
            break;
        }

        block_t *au = block_Alloc(au_size);
        if (!au)
            break;
        memcpy(au->p_buffer, buf, au_size);
        block_CopyProperties(au, block);
        au->p_next = NULL;

        if (!out)
            out = au;
        else
            *last = au;
        last = &au->p_next;

        au->i_nb_samples = opus_frame_duration(buf, au_size);
        if (end_trim && end_trim <= au->i_nb_samples)
            au->i_length = end_trim; /* Blatant abuse of the i_length field. */
        else
            au->i_length = 0;

        if (start_trim && start_trim < (au->i_nb_samples - au->i_length)) {
            au->i_nb_samples -= start_trim;
            if (au->i_nb_samples == 0)
                au->i_flags |= BLOCK_FLAG_PREROLL;
        }

        buf += au_size;
        len -= au_size;
    }

    block_Release(block);
    return out;
}

/****************************************************************************
 * gathering stuff
 ****************************************************************************/
static void ParsePES( demux_t *p_demux, ts_pid_t *pid, block_t *p_pes )
{
    demux_sys_t *p_sys = p_demux->p_sys;
    uint8_t header[34];
    unsigned i_pes_size = 0;
    unsigned i_skip = 0;
    mtime_t i_dts = -1;
    mtime_t i_pts = -1;
    mtime_t i_length = 0;

    /* FIXME find real max size */
    /* const int i_max = */ block_ChainExtract( p_pes, header, 34 );

    if( pid->b_scrambled || header[0] != 0 || header[1] != 0 || header[2] != 1 )
    {
        if ( !pid->b_scrambled )
            msg_Warn( p_demux, "invalid header [0x%02x:%02x:%02x:%02x] (pid: %d)",
                        header[0], header[1],header[2],header[3], pid->i_pid );
        block_ChainRelease( p_pes );
        return;
    }

    /* TODO check size */
    switch( header[3] )
    {
    case 0xBC:  /* Program stream map */
    case 0xBE:  /* Padding */
    case 0xBF:  /* Private stream 2 */
    case 0xF0:  /* ECM */
    case 0xF1:  /* EMM */
    case 0xFF:  /* Program stream directory */
    case 0xF2:  /* DSMCC stream */
    case 0xF8:  /* ITU-T H.222.1 type E stream */
        i_skip = 6;
        break;
    default:
        if( ( header[6]&0xC0 ) == 0x80 )
        {
            /* mpeg2 PES */
            i_skip = header[8] + 9;

            if( header[7]&0x80 )    /* has pts */
            {
                i_pts = ((mtime_t)(header[ 9]&0x0e ) << 29)|
                         (mtime_t)(header[10] << 22)|
                        ((mtime_t)(header[11]&0xfe) << 14)|
                         (mtime_t)(header[12] << 7)|
                         (mtime_t)(header[13] >> 1);

                if( header[7]&0x40 )    /* has dts */
                {
                     i_dts = ((mtime_t)(header[14]&0x0e ) << 29)|
                             (mtime_t)(header[15] << 22)|
                            ((mtime_t)(header[16]&0xfe) << 14)|
                             (mtime_t)(header[17] << 7)|
                             (mtime_t)(header[18] >> 1);
                }
            }
        }
        else
        {
            i_skip = 6;
            while( i_skip < 23 && header[i_skip] == 0xff )
            {
                i_skip++;
            }
            if( i_skip == 23 )
            {
                msg_Err( p_demux, "too much MPEG-1 stuffing" );
                block_ChainRelease( p_pes );
                return;
            }
            if( ( header[i_skip] & 0xC0 ) == 0x40 )
            {
                i_skip += 2;
            }

            if(  header[i_skip]&0x20 )
            {
                 i_pts = ((mtime_t)(header[i_skip]&0x0e ) << 29)|
                          (mtime_t)(header[i_skip+1] << 22)|
                         ((mtime_t)(header[i_skip+2]&0xfe) << 14)|
                          (mtime_t)(header[i_skip+3] << 7)|
                          (mtime_t)(header[i_skip+4] >> 1);

                if( header[i_skip]&0x10 )    /* has dts */
                {
                     i_dts = ((mtime_t)(header[i_skip+5]&0x0e ) << 29)|
                              (mtime_t)(header[i_skip+6] << 22)|
                             ((mtime_t)(header[i_skip+7]&0xfe) << 14)|
                              (mtime_t)(header[i_skip+8] << 7)|
                              (mtime_t)(header[i_skip+9] >> 1);
                     i_skip += 10;
                }
                else
                {
                    i_skip += 5;
                }
            }
            else
            {
                i_skip += 1;
            }
        }
        break;
    }

    if( pid->es->fmt.i_codec == VLC_FOURCC( 'a', '5', '2', 'b' ) ||
        pid->es->fmt.i_codec == VLC_FOURCC( 'd', 't', 's', 'b' ) )
    {
        i_skip += 4;
    }
    else if( pid->es->fmt.i_codec == VLC_FOURCC( 'l', 'p', 'c', 'b' ) ||
             pid->es->fmt.i_codec == VLC_FOURCC( 's', 'p', 'u', 'b' ) ||
             pid->es->fmt.i_codec == VLC_FOURCC( 's', 'd', 'd', 'b' ) )
    {
        i_skip += 1;
    }
    else if( pid->es->fmt.i_codec == VLC_CODEC_SUBT &&
             pid->es->p_mpeg4desc )
    {
        decoder_config_descriptor_t *dcd = &pid->es->p_mpeg4desc->dec_descr;

        if( dcd->i_extra > 2 &&
            dcd->p_extra[0] == 0x10 &&
            ( dcd->p_extra[1]&0x10 ) )
        {
            /* display length */
            if( p_pes->i_buffer + 2 <= i_skip )
                i_length = GetWBE( &p_pes->p_buffer[i_skip] );

            i_skip += 2;
        }
        if( p_pes->i_buffer + 2 <= i_skip )
            i_pes_size = GetWBE( &p_pes->p_buffer[i_skip] );
        /* */
        i_skip += 2;
    }

    /* skip header */
    while( p_pes && i_skip > 0 )
    {
        if( p_pes->i_buffer <= i_skip )
        {
            block_t *p_next = p_pes->p_next;

            i_skip -= p_pes->i_buffer;
            block_Release( p_pes );
            p_pes = p_next;
        }
        else
        {
            p_pes->i_buffer -= i_skip;
            p_pes->p_buffer += i_skip;
            break;
        }
    }

    /* ISO/IEC 13818-1 2.7.5: if no pts and no dts, then dts == pts */
    if( i_pts >= 0 && i_dts < 0 )
        i_dts = i_pts;

    if( p_pes )
    {
        block_t *p_block;

        if( i_dts >= 0 )
            p_pes->i_dts = VLC_TS_0 + i_dts * 100 / 9;

        if( i_pts >= 0 )
            p_pes->i_pts = VLC_TS_0 + i_pts * 100 / 9;

        p_pes->i_length = i_length * 100 / 9;

        p_block = block_ChainGather( p_pes );
        if( pid->es->fmt.i_codec == VLC_CODEC_SUBT )
        {
            if( i_pes_size > 0 && p_block->i_buffer > i_pes_size )
            {
                p_block->i_buffer = i_pes_size;
            }
            /* Append a \0 */
            p_block = block_Realloc( p_block, 0, p_block->i_buffer + 1 );
            if( !p_block )
                return;
            p_block->p_buffer[p_block->i_buffer -1] = '\0';
        }
        else if( pid->es->fmt.i_codec == VLC_CODEC_TELETEXT )
        {
            if( p_block->i_pts <= VLC_TS_INVALID )
            {
                /* Teletext may have missing PTS (ETSI EN 300 472 Annexe A)
                 * In this case use the last PCR + 40ms */
                for( int i = 0; pid->p_owner && i < pid->p_owner->i_prg; i++ )
                {
                    if( pid->i_owner_number == pid->p_owner->prg[i]->i_number )
                    {
                        mtime_t i_pcr = pid->p_owner->prg[i]->i_pcr_value;
                        if( i_pcr > VLC_TS_INVALID )
                            p_block->i_pts = VLC_TS_0 + i_pcr * 100 / 9 + 40000;
                        break;
                    }
                }
            }
        }
        else if( pid->es->fmt.i_codec == VLC_CODEC_ARIB_A ||
                 pid->es->fmt.i_codec == VLC_CODEC_ARIB_C )
        {
            if( p_block->i_pts <= VLC_TS_INVALID )
            {
                if( i_pes_size > 0 && p_block->i_buffer > i_pes_size )
                {
                    p_block->i_buffer = i_pes_size;
                }
                /* Append a \0 */
                p_block = block_Realloc( p_block, 0, p_block->i_buffer + 1 );
                if( !p_block )
                    return;
                p_block->p_buffer[p_block->i_buffer -1] = '\0';
            }
        }
        else if( pid->es->fmt.i_codec == VLC_CODEC_OPUS)
        {
            p_block = Opus_Parse(p_demux, p_block);
        }

        while (p_block) {
            block_t *p_next = p_block->p_next;
            p_block->p_next = NULL;
            for( int i = 0; i < pid->i_extra_es; i++ )
            {
                es_out_Send( p_demux->out, pid->extra_es[i]->id,
                        block_Duplicate( p_block ) );
            }

            if (!p_sys->b_trust_pcr)
                es_out_Control( p_demux->out, ES_OUT_SET_GROUP_PCR,
                        pid->i_owner_number, p_block->i_pts);

            es_out_Send( p_demux->out, pid->es->id, p_block );

            p_block = p_next;
        }
    }
    else
    {
        msg_Warn( p_demux, "empty pes" );
    }
}

static void ParseTableSection( demux_t *p_demux, ts_pid_t *pid, block_t *p_data )
{
    block_t *p_content = block_ChainGather( p_data );
    mtime_t i_date = -1;
    for( int i = 0; pid->p_owner && i < pid->p_owner->i_prg; i++ )
    {
        if( pid->i_owner_number == pid->p_owner->prg[i]->i_number )
        {
            i_date = pid->p_owner->prg[i]->i_pcr_value;
            if( i_date >= 0 )
                break;
        }
    }
    if( i_date >= 0 )
    {
        if( pid->es->fmt.i_codec == VLC_CODEC_SCTE_27 )
        {
            /* We need to extract the truncated pts stored inside the payload */
            if( p_content->i_buffer > 9 && p_content->p_buffer[0] == 0xc6 )
            {
                int i_index = 0;
                size_t i_offset = 4;
                if( p_content->p_buffer[3] & 0x40 )
                {
                    i_index = ((p_content->p_buffer[7] & 0x0f) << 8) |
                              p_content->p_buffer[8];
                    i_offset = 9;
                }
                if( i_index == 0 && p_content->i_buffer > i_offset + 8 )
                {
                    bool is_immediate = p_content->p_buffer[i_offset + 3] & 0x40;
                    if( !is_immediate )
                    {
                        mtime_t i_display_in = GetDWBE( &p_content->p_buffer[i_offset + 4] );
                        if( i_display_in < i_date )
                            i_date = i_display_in + (1ll << 32);
                        else
                            i_date = i_display_in;
                    }

                }
            }
        }
        p_content->i_dts =
        p_content->i_pts = VLC_TS_0 + i_date * 100 / 9;
    }
    es_out_Send( p_demux->out, pid->es->id, p_content );
}
static void ParseData( demux_t *p_demux, ts_pid_t *pid )
{
    block_t *p_data = pid->es->p_data;

    /* remove the pes from pid */
    pid->es->p_data = NULL;
    pid->es->i_data_size = 0;
    pid->es->i_data_gathered = 0;
    pid->es->pp_last = &pid->es->p_data;

    if( pid->es->data_type == TS_ES_DATA_PES )
    {
        ParsePES( p_demux, pid, p_data );
    }
    else if( pid->es->data_type == TS_ES_DATA_TABLE_SECTION )
    {
        ParseTableSection( p_demux, pid, p_data );
    }
    else
    {
        block_ChainRelease( p_data );
    }
}

static block_t* ReadTSPacket( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    block_t     *p_pkt;

    /* Get a new TS packet */
    if( !( p_pkt = stream_Block( p_demux->s, p_sys->i_packet_size ) ) )
    {
        msg_Dbg( p_demux, "eof ?" );
        return NULL;
    }

    /* Skip header (BluRay streams).
     * re-sync logic would do this (by adjusting packet start), but this would result in losing first and last ts packets.
     * First packet is usually PAT, and losing it means losing whole first GOP. This is fatal with still-image based menus.
     */
    p_pkt->p_buffer += p_sys->i_packet_header_size;
    p_pkt->i_buffer -= p_sys->i_packet_header_size;

    /* Check sync byte and re-sync if needed */
    if( p_pkt->p_buffer[0] != 0x47 )
    {
        msg_Warn( p_demux, "lost synchro" );
        block_Release( p_pkt );
        while( vlc_object_alive (p_demux) )
        {
            const uint8_t *p_peek;
            int i_peek, i_skip = 0;

            i_peek = stream_Peek( p_demux->s, &p_peek,
                    p_sys->i_packet_size * 10 );
            if( i_peek < p_sys->i_packet_size + 1 )
            {
                msg_Dbg( p_demux, "eof ?" );
                return NULL;
            }

            while( i_skip < i_peek - p_sys->i_packet_size )
            {
                if( p_peek[i_skip + p_sys->i_packet_header_size] == 0x47 &&
                        p_peek[i_skip + p_sys->i_packet_header_size + p_sys->i_packet_size] == 0x47 )
                {
                    break;
                }
                i_skip++;
            }
            msg_Dbg( p_demux, "skipping %d bytes of garbage", i_skip );
            stream_Read( p_demux->s, NULL, i_skip );

            if( i_skip < i_peek - p_sys->i_packet_size )
            {
                break;
            }
        }
        if( !( p_pkt = stream_Block( p_demux->s, p_sys->i_packet_size ) ) )
        {
            msg_Dbg( p_demux, "eof ?" );
            return NULL;
        }
    }
    return p_pkt;
}

static mtime_t AdjustPCRWrapAround( demux_t *p_demux, mtime_t i_pcr )
{
    demux_sys_t   *p_sys = p_demux->p_sys;
    /*
     * PCR is 33bit. If PCR reaches to 0x1FFFFFFFF (26:30:43.717), ressets from 0.
     * So, need to add 0x1FFFFFFFF, for calculating duration or current position.
     */
    mtime_t i_adjust = 0;
    int64_t i_pos = stream_Tell( p_demux->s );
    int i;
    for( i = 1; i < p_sys->i_pcrs_num && p_sys->p_pos[i] <= i_pos; ++i )
    {
        if( p_sys->p_pcrs[i-1] > p_sys->p_pcrs[i] )
            i_adjust += 0x1FFFFFFFF;
    }
    if( p_sys->p_pcrs[i-1] > i_pcr )
        i_adjust += 0x1FFFFFFFF;

    return i_pcr + i_adjust;
}

static mtime_t GetPCR( block_t *p_pkt )
{
    const uint8_t *p = p_pkt->p_buffer;

    mtime_t i_pcr = -1;

    if( ( p[3]&0x20 ) && /* adaptation */
        ( p[5]&0x10 ) &&
        ( p[4] >= 7 ) )
    {
        /* PCR is 33 bits */
        i_pcr = ( (mtime_t)p[6] << 25 ) |
                ( (mtime_t)p[7] << 17 ) |
                ( (mtime_t)p[8] << 9 ) |
                ( (mtime_t)p[9] << 1 ) |
                ( (mtime_t)p[10] >> 7 );
    }
    return i_pcr;
}

static int SeekToPCR( demux_t *p_demux, int64_t i_pos )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    mtime_t i_pcr = -1;
    const int64_t i_initial_pos = stream_Tell( p_demux->s );

    if( i_pos < 0 )
        return VLC_EGENERIC;

    int64_t i_last_pos = stream_Size( p_demux->s ) - p_sys->i_packet_size;
    if( i_pos > i_last_pos )
        i_pos = i_last_pos;

    if( stream_Seek( p_demux->s, i_pos ) )
        return VLC_EGENERIC;

    while( vlc_object_alive( p_demux ) )
    {
        block_t *p_pkt;

        if( !( p_pkt = ReadTSPacket( p_demux ) ) )
        {
            break;
        }
        if( PIDGet( p_pkt ) == p_sys->i_pid_ref_pcr )
        {
            i_pcr = GetPCR( p_pkt );
        }
        block_Release( p_pkt );
        if( i_pcr >= 0 )
            break;
        if( stream_Tell( p_demux->s ) >= i_last_pos )
            break;
    }
    if( i_pcr < 0 )
    {
        stream_Seek( p_demux->s, i_initial_pos );
        assert( i_initial_pos == stream_Tell( p_demux->s ) );
        return VLC_EGENERIC;
    }

    p_sys->i_current_pcr = i_pcr;
    return VLC_SUCCESS;
}

static int Seek( demux_t *p_demux, double f_percent )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    int64_t i_initial_pos = stream_Tell( p_demux->s );
    mtime_t i_initial_pcr = p_sys->i_current_pcr;

    /*
     * Find the time position by using binary search algorithm.
     */
    mtime_t i_target_pcr = (p_sys->i_last_pcr - p_sys->i_first_pcr) * f_percent + p_sys->i_first_pcr;

    int64_t i_head_pos = 0;
    int64_t i_tail_pos;
    {
        mtime_t i_adjust = 0;
        int i;
        for( i = 1; i < p_sys->i_pcrs_num; ++i )
        {
            if( p_sys->p_pcrs[i-1] > p_sys->p_pcrs[i] )
                i_adjust += 0x1FFFFFFFF;
            if( p_sys->p_pcrs[i] + i_adjust > i_target_pcr )
                break;
        }
        i_head_pos = p_sys->p_pos[i-1];
        i_tail_pos = ( i < p_sys->i_pcrs_num ) ?  p_sys->p_pos[i] : stream_Size( p_demux->s );
    }
    msg_Dbg( p_demux, "Seek():i_head_pos:%"PRId64", i_tail_pos:%"PRId64, i_head_pos, i_tail_pos);

    bool b_found = false;
    int i_cnt = 0;
    while( i_head_pos <= i_tail_pos )
    {
        /* Round i_pos to a multiple of p_sys->i_packet_size */
        int64_t i_pos = i_head_pos + (i_tail_pos - i_head_pos) / 2;
        int64_t i_div = i_pos % p_sys->i_packet_size;
        i_pos -= i_div;
        if( SeekToPCR( p_demux, i_pos ) )
            break;
        p_sys->i_current_pcr = AdjustPCRWrapAround( p_demux, p_sys->i_current_pcr );
        int64_t i_diff_msec = (p_sys->i_current_pcr - i_target_pcr) * 100 / 9 / 1000;
        if( i_diff_msec > 500 )
        {
            i_tail_pos = i_pos - p_sys->i_packet_size;
        }
        else if( i_diff_msec < -500 )
        {
            i_head_pos = i_pos + p_sys->i_packet_size;
        }
        else
        {
            // diff time <= 500msec
            b_found = true;
            break;
        }
        ++i_cnt;
    }
    if( !b_found )
    {
        msg_Dbg( p_demux, "Seek():cannot find a time position. i_cnt:%d", i_cnt );
        stream_Seek( p_demux->s, i_initial_pos );
        p_sys->i_current_pcr = i_initial_pcr;
        return VLC_EGENERIC;
    }
    else
    {
        msg_Dbg( p_demux, "Seek():can find a time position. i_cnt:%d", i_cnt );
        return VLC_SUCCESS;
    }
}

static void GetFirstPCR( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    int64_t i_initial_pos = stream_Tell( p_demux->s );

    if( stream_Seek( p_demux->s, 0 ) )
        return;

    while( vlc_object_alive (p_demux) )
    {
        block_t     *p_pkt;

        if( !( p_pkt = ReadTSPacket( p_demux ) ) )
        {
            break;
        }
        mtime_t i_pcr = GetPCR( p_pkt );
        if( i_pcr >= 0 )
        {
            p_sys->i_pid_ref_pcr = PIDGet( p_pkt );
            p_sys->i_first_pcr = i_pcr;
            p_sys->i_current_pcr = i_pcr;
        }
        block_Release( p_pkt );
        if( p_sys->i_first_pcr >= 0 )
            break;
    }
    stream_Seek( p_demux->s, i_initial_pos );
}

static void GetLastPCR( demux_t *p_demux )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    const int64_t i_initial_pos = stream_Tell( p_demux->s );
    mtime_t i_initial_pcr = p_sys->i_current_pcr;

    int64_t i_stream_size = stream_Size( p_demux->s );
    int64_t i_last_pos = i_stream_size - p_sys->i_packet_size;
    /* Round i_pos to a multiple of p_sys->i_packet_size */
    int64_t i_pos = i_last_pos - p_sys->i_packet_size * 4500; /* FIXME if the value is not reasonable, please change it. */
    int64_t i_div = i_pos % p_sys->i_packet_size;
    i_pos -= i_div;

    if( i_pos <= i_initial_pos && i_pos >= i_stream_size )
        i_pos = i_initial_pos + p_sys->i_packet_size;
    if( i_pos < 0 && i_pos >= i_stream_size )
        return;

    while( vlc_object_alive( p_demux ) )
    {
        if( SeekToPCR( p_demux, i_pos ) )
            break;
        p_sys->i_last_pcr = AdjustPCRWrapAround( p_demux, p_sys->i_current_pcr );
        if( ( i_pos = stream_Tell( p_demux->s ) ) >= i_last_pos )
            break;
    }
    if( p_sys->i_last_pcr >= 0 )
    {
        int64_t i_size = stream_Size( p_demux->s );
        mtime_t i_duration_msec = ( p_sys->i_last_pcr - p_sys->i_first_pcr ) * 100 / 9 / 1000;
        int64_t i_rate = ( i_size < 0 || i_duration_msec <= 0 ) ? 0 : i_size * 1000 * 8 / i_duration_msec;
        const int64_t TS_SUPPOSED_MAXRATE = 55 * 1000 * 1000; //FIXME I think it's enough.
        const int64_t TS_SUPPOSED_MINRATE = 0.5 * 1000 * 1000; //FIXME
        if( i_rate < TS_SUPPOSED_MINRATE || i_rate > TS_SUPPOSED_MAXRATE )
        {
            msg_Dbg( p_demux, "calculated bitrate (%"PRId64"bit/s) is too low or too high. min bitrate (%"PRId64"bit/s) max bitrate (%"PRId64"bit/s)",
                     i_rate, TS_SUPPOSED_MINRATE, TS_SUPPOSED_MAXRATE );
            p_sys->i_last_pcr = -1;
        }
    }
    stream_Seek( p_demux->s, i_initial_pos );
    assert( i_initial_pos == stream_Tell( p_demux->s ) );
    p_sys->i_current_pcr = i_initial_pcr;
}

static void CheckPCR( demux_t *p_demux )
{
    demux_sys_t   *p_sys = p_demux->p_sys;

    int64_t i_initial_pos = stream_Tell( p_demux->s );
    mtime_t i_initial_pcr = p_sys->i_current_pcr;

    int64_t i_size = stream_Size( p_demux->s );

    int i = 0;
    p_sys->p_pcrs[0] = p_sys->i_first_pcr;
    p_sys->p_pos[0] = i_initial_pos;

    for( i = 1; i < p_sys->i_pcrs_num && vlc_object_alive( p_demux ); ++i )
    {
        /* Round i_pos to a multiple of p_sys->i_packet_size */
        int64_t i_pos = i_size / p_sys->i_pcrs_num * i;
        int64_t i_div = i_pos % p_sys->i_packet_size;
        i_pos -= i_div;
        if( SeekToPCR( p_demux, i_pos ) )
            break;
        p_sys->p_pcrs[i] = p_sys->i_current_pcr;
        p_sys->p_pos[i] = stream_Tell( p_demux->s );
        if( p_sys->p_pcrs[i-1] > p_sys->p_pcrs[i] )
        {
            msg_Dbg( p_demux, "PCR Wrap Around found between %d%% and %d%% (pcr:%"PRId64"(0x%09"PRIx64") pcr:%"PRId64"(0x%09"PRIx64"))",
                    (int)((i-1)*100/p_sys->i_pcrs_num), (int)(i*100/p_sys->i_pcrs_num), p_sys->p_pcrs[i-1], p_sys->p_pcrs[i-1], p_sys->p_pcrs[i], p_sys->p_pcrs[i] );
        }
    }
    if( i < p_sys->i_pcrs_num )
    {
        msg_Dbg( p_demux, "Force Seek Per Percent: Seeking failed at %d%%.", (int)(i*100/p_sys->i_pcrs_num) );
        p_sys->b_force_seek_per_percent = true;
    }

    stream_Seek( p_demux->s, i_initial_pos );
    p_sys->i_current_pcr = i_initial_pcr;
}

static void PCRHandle( demux_t *p_demux, ts_pid_t *pid, block_t *p_bk )
{
    demux_sys_t   *p_sys = p_demux->p_sys;

    if( p_sys->i_pmt_es <= 0 )
        return;

    mtime_t i_pcr = GetPCR( p_bk );
    if( i_pcr < 0 )
        return;

    if( p_sys->i_pid_ref_pcr == pid->i_pid )
        p_sys->i_current_pcr = AdjustPCRWrapAround( p_demux, i_pcr );

    /* Search program and set the PCR */
    int i_group = -1;
    for( int i = 0; i < p_sys->i_pmt && i_group < 0 ; i++ )
    {
        bool b_pmt_has_es = false;

        for( int i_prg = 0; i_prg < p_sys->pmt[i]->psi->i_prg; i_prg++ )
        {
            if( pid->i_pid == p_sys->pmt[i]->psi->prg[i_prg]->i_pid_pcr )
            {
                /* We've found our target group */
                p_sys->pmt[i]->psi->prg[i_prg]->i_pcr_value = i_pcr;
                i_group = p_sys->pmt[i]->psi->prg[i_prg]->i_number;
                for( int j = 0; j < 8192; j++ )
                {
                    const ts_pid_t *pid = &p_sys->pid[j];
                    if( pid->b_valid && pid->p_owner == p_sys->pmt[i]->psi && pid->es )
                    {
                        b_pmt_has_es = true;
                        break;
                    }
                }
            }
        }

        if ( p_sys->b_trust_pcr && i_group > 0 && b_pmt_has_es )
            es_out_Control( p_demux->out, ES_OUT_SET_GROUP_PCR,
              i_group, VLC_TS_0 + i_pcr * 100 / 9 );
    }
}

static bool GatherData( demux_t *p_demux, ts_pid_t *pid, block_t *p_bk )
{
    const uint8_t *p = p_bk->p_buffer;
    const bool b_unit_start = p[1]&0x40;
    const bool b_scrambled  = p[3]&0x80;
    const bool b_adaptation = p[3]&0x20;
    const bool b_payload    = p[3]&0x10;
    const int  i_cc         = p[3]&0x0f; /* continuity counter */
    bool       b_discontinuity = false;  /* discontinuity */

    /* transport_scrambling_control is ignored */
    int         i_skip = 0;
    bool        i_ret  = false;

#if 0
    msg_Dbg( p_demux, "pid=%d unit_start=%d adaptation=%d payload=%d "
             "cc=0x%x", pid->i_pid, b_unit_start, b_adaptation,
             b_payload, i_cc );
#endif

    /* For now, ignore additional error correction
     * TODO: handle Reed-Solomon 204,188 error correction */
    p_bk->i_buffer = TS_PACKET_SIZE_188;

    if( p[1]&0x80 )
    {
        msg_Dbg( p_demux, "transport_error_indicator set (pid=%d)",
                 pid->i_pid );
        if( pid->es->p_data ) //&& pid->es->fmt.i_cat == VIDEO_ES )
            pid->es->p_data->i_flags |= BLOCK_FLAG_CORRUPTED;
    }

    if( p_demux->p_sys->csa )
    {
        vlc_mutex_lock( &p_demux->p_sys->csa_lock );
        csa_Decrypt( p_demux->p_sys->csa, p_bk->p_buffer, p_demux->p_sys->i_csa_pkt_size );
        vlc_mutex_unlock( &p_demux->p_sys->csa_lock );
    }

    if( !b_adaptation )
    {
        /* We don't have any adaptation_field, so payload starts
         * immediately after the 4 byte TS header */
        i_skip = 4;
    }
    else
    {
        /* p[4] is adaptation_field_length minus one */
        i_skip = 5 + p[4];
        if( p[4] > 0 )
        {
            /* discontinuity indicator found in stream */
            b_discontinuity = (p[5]&0x80) ? true : false;
            if( b_discontinuity && pid->es->p_data )
            {
                msg_Warn( p_demux, "discontinuity indicator (pid=%d) ",
                            pid->i_pid );
                /* pid->es->p_data->i_flags |= BLOCK_FLAG_DISCONTINUITY; */
            }
#if 0
            if( p[5]&0x40 )
                msg_Dbg( p_demux, "random access indicator (pid=%d) ", pid->i_pid );
#endif
        }
    }

    /* Test continuity counter */
    /* continuous when (one of this):
        * diff == 1
        * diff == 0 and payload == 0
        * diff == 0 and duplicate packet (playload != 0) <- should we
        *   test the content ?
     */
    const int i_diff = ( i_cc - pid->i_cc )&0x0f;
    if( b_payload && i_diff == 1 )
    {
        pid->i_cc = ( pid->i_cc + 1 ) & 0xf;
    }
    else
    {
        if( pid->i_cc == 0xff )
        {
            msg_Warn( p_demux, "first packet for pid=%d cc=0x%x",
                      pid->i_pid, i_cc );
            pid->i_cc = i_cc;
        }
        else if( i_diff != 0 && !b_discontinuity )
        {
            msg_Warn( p_demux, "discontinuity received 0x%x instead of 0x%x (pid=%d)",
                      i_cc, ( pid->i_cc + 1 )&0x0f, pid->i_pid );

            pid->i_cc = i_cc;
            if( pid->es->p_data && pid->es->fmt.i_cat != VIDEO_ES &&
                pid->es->fmt.i_cat != AUDIO_ES )
            {
                /* Small audio/video artifacts are usually better than
                 * dropping full frames */
                pid->es->p_data->i_flags |= BLOCK_FLAG_CORRUPTED;
            }
        }
    }

    PCRHandle( p_demux, pid, p_bk );

    if( i_skip >= 188 || pid->es->id == NULL )
    {
        block_Release( p_bk );
        return i_ret;
    }

    /* */
    if( !pid->b_scrambled != !b_scrambled )
    {
        msg_Warn( p_demux, "scrambled state changed on pid %d (%d->%d)",
                  pid->i_pid, pid->b_scrambled, b_scrambled );

        pid->b_scrambled = b_scrambled;

        for( int i = 0; i < pid->i_extra_es; i++ )
        {
            es_out_Control( p_demux->out, ES_OUT_SET_ES_SCRAMBLED_STATE,
                            pid->extra_es[i]->id, b_scrambled );
        }
        es_out_Control( p_demux->out, ES_OUT_SET_ES_SCRAMBLED_STATE,
                        pid->es->id, b_scrambled );
    }

    /* We have to gather it */
    p_bk->p_buffer += i_skip;
    p_bk->i_buffer -= i_skip;

    if( b_unit_start )
    {
        if( pid->es->data_type == TS_ES_DATA_TABLE_SECTION && p_bk->i_buffer > 0 )
        {
            int i_pointer_field = __MIN( p_bk->p_buffer[0], p_bk->i_buffer - 1 );
            block_t *p = block_Duplicate( p_bk );
            if( p )
            {
                p->i_buffer = i_pointer_field;
                p->p_buffer++;
                block_ChainLastAppend( &pid->es->pp_last, p );
            }
            p_bk->i_buffer -= 1 + i_pointer_field;
            p_bk->p_buffer += 1 + i_pointer_field;
        }
        if( pid->es->p_data )
        {
            ParseData( p_demux, pid );
            i_ret = true;
        }

        block_ChainLastAppend( &pid->es->pp_last, p_bk );
        if( pid->es->data_type == TS_ES_DATA_PES )
        {
            if( p_bk->i_buffer > 6 )
            {
                pid->es->i_data_size = GetWBE( &p_bk->p_buffer[4] );
                if( pid->es->i_data_size > 0 )
                {
                    pid->es->i_data_size += 6;
                }
            }
        }
        else if( pid->es->data_type == TS_ES_DATA_TABLE_SECTION )
        {
            if( p_bk->i_buffer > 3 && p_bk->p_buffer[0] != 0xff )
            {
                pid->es->i_data_size = 3 + (((p_bk->p_buffer[1] & 0xf) << 8) | p_bk->p_buffer[2]);
            }
        }
        pid->es->i_data_gathered += p_bk->i_buffer;
        if( pid->es->i_data_size > 0 &&
            pid->es->i_data_gathered >= pid->es->i_data_size )
        {
            ParseData( p_demux, pid );
            i_ret = true;
        }
    }
    else
    {
        if( pid->es->p_data == NULL )
        {
            /* msg_Dbg( p_demux, "broken packet" ); */
            block_Release( p_bk );
        }
        else
        {
            block_ChainLastAppend( &pid->es->pp_last, p_bk );
            pid->es->i_data_gathered += p_bk->i_buffer;

            if( pid->es->i_data_size > 0 &&
                pid->es->i_data_gathered >= pid->es->i_data_size )
            {
                ParseData( p_demux, pid );
                i_ret = true;
            }
        }
    }

    return i_ret;
}

static void PIDFillFormat( es_format_t *fmt, int i_stream_type )
{
    switch( i_stream_type )
    {
    case 0x01:  /* MPEG-1 video */
    case 0x02:  /* MPEG-2 video */
    case 0x80:  /* MPEG-2 MOTO video */
        es_format_Init( fmt, VIDEO_ES, VLC_CODEC_MPGV );
        break;
    case 0x03:  /* MPEG-1 audio */
    case 0x04:  /* MPEG-2 audio */
        es_format_Init( fmt, AUDIO_ES, VLC_CODEC_MPGA );
        break;
    case 0x11:  /* MPEG4 (audio) LATM */
    case 0x0f:  /* ISO/IEC 13818-7 Audio with ADTS transport syntax */
    case 0x1c:  /* ISO/IEC 14496-3 Audio, without using any additional
                   transport syntax, such as DST, ALS and SLS */
        es_format_Init( fmt, AUDIO_ES, VLC_CODEC_MP4A );
        break;
    case 0x10:  /* MPEG4 (video) */
        es_format_Init( fmt, VIDEO_ES, VLC_CODEC_MP4V );
        break;
    case 0x1B:  /* H264 <- check transport syntax/needed descriptor */
        es_format_Init( fmt, VIDEO_ES, VLC_CODEC_H264 );
        break;
    case 0x24:  /* HEVC */
        es_format_Init( fmt, VIDEO_ES, VLC_CODEC_HEVC );
        break;
    case 0x42:  /* CAVS (Chinese AVS) */
        es_format_Init( fmt, VIDEO_ES, VLC_CODEC_CAVS );
        break;

    case 0x81:  /* A52 (audio) */
        es_format_Init( fmt, AUDIO_ES, VLC_CODEC_A52 );
        break;
    case 0x82:  /* SCTE-27 (sub) */
        es_format_Init( fmt, SPU_ES, VLC_CODEC_SCTE_27 );
        break;
    case 0x84:  /* SDDS (audio) */
        es_format_Init( fmt, AUDIO_ES, VLC_CODEC_SDDS );
        break;
    case 0x85:  /* DTS (audio) */
        es_format_Init( fmt, AUDIO_ES, VLC_CODEC_DTS );
        break;
    case 0x87: /* E-AC3 */
        es_format_Init( fmt, AUDIO_ES, VLC_CODEC_EAC3 );
        break;

    case 0x91:  /* A52 vls (audio) */
        es_format_Init( fmt, AUDIO_ES, VLC_FOURCC( 'a', '5', '2', 'b' ) );
        break;
    case 0x92:  /* DVD_SPU vls (sub) */
        es_format_Init( fmt, SPU_ES, VLC_FOURCC( 's', 'p', 'u', 'b' ) );
        break;

    case 0x94:  /* SDDS (audio) */
        es_format_Init( fmt, AUDIO_ES, VLC_FOURCC( 's', 'd', 'd', 'b' ) );
        break;

    case 0xa0:  /* MSCODEC vlc (video) (fixed later) */
        es_format_Init( fmt, UNKNOWN_ES, 0 );
        break;

    case 0x06:  /* PES_PRIVATE  (fixed later) */
    case 0x12:  /* MPEG-4 generic (sub/scene/...) (fixed later) */
    case 0xEA:  /* Privately managed ES (VC-1) (fixed later */
    default:
        es_format_Init( fmt, UNKNOWN_ES, 0 );
        break;
    }

    /* PES packets usually contain truncated frames */
    fmt->b_packetized = false;
}

/*****************************************************************************
 * MP4 specific functions (IOD parser)
 *****************************************************************************/
static int  IODDescriptorLength( int *pi_data, uint8_t **pp_data )
{
    unsigned int i_b;
    unsigned int i_len = 0;
    do
    {
        i_b = **pp_data;
        (*pp_data)++;
        (*pi_data)--;
        i_len = ( i_len << 7 ) + ( i_b&0x7f );

    } while( i_b&0x80 && *pi_data > 0 );

    if (i_len > *pi_data)
        i_len = *pi_data;

    return i_len;
}

static int IODGetBytes( int *pi_data, uint8_t **pp_data, size_t bytes )
{
    uint32_t res = 0;
    while( *pi_data > 0 && bytes-- )
    {
        res <<= 8;
        res |= **pp_data;
        (*pp_data)++;
        (*pi_data)--;
    }

    return res;
}

static char* IODGetURL( int *pi_data, uint8_t **pp_data )
{
    int len = IODGetBytes( pi_data, pp_data, 1 );
    if (len > *pi_data)
        len = *pi_data;
    char *url = strndup( (char*)*pp_data, len );
    *pp_data += len;
    *pi_data -= len;
    return url;
}

static iod_descriptor_t *IODNew( int i_data, uint8_t *p_data )
{
    uint8_t i_iod_tag, i_iod_label, byte1, byte2, byte3;

    iod_descriptor_t *p_iod = calloc( 1, sizeof( iod_descriptor_t ) );
    if( !p_iod )
        return NULL;

    if( i_data < 3 )
    {
        return p_iod;
    }

    byte1 = IODGetBytes( &i_data, &p_data, 1 );
    byte2 = IODGetBytes( &i_data, &p_data, 1 );
    byte3 = IODGetBytes( &i_data, &p_data, 1 );
    if( byte2 == 0x02 ) //old vlc's buggy implementation of the IOD_descriptor
    {
        i_iod_label = byte1;
        i_iod_tag = byte2;
    }
    else  //correct implementation of the IOD_descriptor
    {
        i_iod_label = byte2;
        i_iod_tag = byte3;
    }

    ts_debug( "\n* iod label:%d tag:0x%x", i_iod_label, i_iod_tag );

    if( i_iod_tag != 0x02 )
    {
        ts_debug( "\n ERR: tag %02x != 0x02", i_iod_tag );
        return p_iod;
    }

    IODDescriptorLength( &i_data, &p_data );

    uint16_t i_od_id = ( IODGetBytes( &i_data, &p_data, 1 ) << 2 );
    uint8_t i_flags = IODGetBytes( &i_data, &p_data, 1 );
    i_od_id |= i_flags >> 6;
    ts_debug( "\n* od_id:%d", i_od_id );
    ts_debug( "\n* includeInlineProfileLevel flag:%d", ( i_flags >> 4 )&0x01 );
    if ((i_flags >> 5) & 0x01)
    {
        p_iod->psz_url = IODGetURL( &i_data, &p_data );
        ts_debug( "\n* url string:%s", p_iod->psz_url );
        ts_debug( "\n*****************************\n" );
        return p_iod;
    }
    else
    {
        p_iod->psz_url = NULL;
    }

    /* Profile Level Indication */
    IODGetBytes( &i_data, &p_data, 1 ); /* OD */
    IODGetBytes( &i_data, &p_data, 1 ); /* scene */
    IODGetBytes( &i_data, &p_data, 1 ); /* audio */
    IODGetBytes( &i_data, &p_data, 1 ); /* visual */
    IODGetBytes( &i_data, &p_data, 1 ); /* graphics */

    int i_length = 0;
    int i_data_sav = i_data;
    uint8_t *p_data_sav = p_data;
    for (int i = 0; i_data > 0 && i < ES_DESCRIPTOR_COUNT; i++)
    {
        es_mpeg4_descriptor_t *es_descr = &p_iod->es_descr[i];

        p_data = p_data_sav + i_length;
        i_data = i_data_sav - i_length;

        int i_tag = IODGetBytes( &i_data, &p_data, 1 );
        i_length = IODDescriptorLength( &i_data, &p_data );

        i_data_sav = i_data;
        p_data_sav = p_data;

        i_data = i_length;

        if ( i_tag != 0x03 )
        {
            ts_debug( "\n* - OD tag:0x%x Unsupported", i_tag );
            continue;
        }

        es_descr->i_es_id = IODGetBytes( &i_data, &p_data, 2 );
        int i_flags = IODGetBytes( &i_data, &p_data, 1 );
        bool b_streamDependenceFlag = ( i_flags >> 7 )&0x01;
        if( b_streamDependenceFlag )
            IODGetBytes( &i_data, &p_data, 2 ); /* dependOn_es_id */

        if( (i_flags >> 6) & 0x01 )
            es_descr->psz_url = IODGetURL( &i_data, &p_data );

        bool b_OCRStreamFlag = ( i_flags >> 5 )&0x01;
        if( b_OCRStreamFlag )
            IODGetBytes( &i_data, &p_data, 2 ); /* OCR_es_id */

        if( IODGetBytes( &i_data, &p_data, 1 ) != 0x04 )
        {
            ts_debug( "\n* ERR missing DecoderConfigDescr" );
            continue;
        }
        int i_config_desc_length = IODDescriptorLength( &i_data, &p_data ); /* DecoderConfigDescr_length */
        decoder_config_descriptor_t *dec_descr = &es_descr->dec_descr;
        dec_descr->i_objectTypeIndication = IODGetBytes( &i_data, &p_data, 1 );
        i_flags = IODGetBytes( &i_data, &p_data, 1 );
        dec_descr->i_streamType = i_flags >> 2;

        IODGetBytes( &i_data, &p_data, 3); /* bufferSizeDB */
        IODGetBytes( &i_data, &p_data, 4); /* maxBitrate */
        IODGetBytes( &i_data, &p_data, 4 ); /* avgBitrate */

        if( i_config_desc_length > 13 && IODGetBytes( &i_data, &p_data, 1 ) == 0x05 )
        {
            dec_descr->i_extra = IODDescriptorLength( &i_data, &p_data );
            if( dec_descr->i_extra > 0 )
            {
                dec_descr->p_extra = xmalloc( dec_descr->i_extra );
                memcpy(dec_descr->p_extra, p_data, dec_descr->i_extra);
                p_data += dec_descr->i_extra;
                i_data -= dec_descr->i_extra;
            }
        }
        else
        {
            dec_descr->i_extra = 0;
            dec_descr->p_extra = NULL;
        }

        if( IODGetBytes( &i_data, &p_data, 1 ) != 0x06 )
        {
            ts_debug( "\n* ERR missing SLConfigDescr" );
            continue;
        }
        IODDescriptorLength( &i_data, &p_data ); /* SLConfigDescr_length */
        switch( IODGetBytes( &i_data, &p_data, 1 ) /* predefined */ )
        {
        default:
            ts_debug( "\n* ERR unsupported SLConfigDescr predefined" );
        case 0x01:
            // FIXME
            break;
        }
        es_descr->b_ok = true;
    }

    return p_iod;
}

static void IODFree( iod_descriptor_t *p_iod )
{
    if( p_iod->psz_url )
    {
        free( p_iod->psz_url );
        free( p_iod );
        return;
    }

    for( int i = 0; i < 255; i++ )
    {
#define es_descr p_iod->es_descr[i]
        if( es_descr.b_ok )
        {
            if( es_descr.psz_url )
                free( es_descr.psz_url );
            else
                free( es_descr.dec_descr.p_extra );
        }
#undef  es_descr
    }
    free( p_iod );
}

/****************************************************************************
 ****************************************************************************
 ** libdvbpsi callbacks
 ****************************************************************************
 ****************************************************************************/
static bool ProgramIsSelected( demux_t *p_demux, uint16_t i_pgrm )
{
    demux_sys_t          *p_sys = p_demux->p_sys;

    if( ( p_sys->i_current_program == -1 && p_sys->programs_list.i_count == 0 ) ||
        p_sys->i_current_program == 0 )
        return true;
    if( p_sys->i_current_program == i_pgrm )
        return true;

    if( p_sys->programs_list.i_count != 0 )
    {
        for( int i = 0; i < p_sys->programs_list.i_count; i++ )
        {
            if( i_pgrm == p_sys->programs_list.p_values[i].i_int )
                return true;
        }
    }
    return false;
}

static void ValidateDVBMeta( demux_t *p_demux, int i_pid )
{
    demux_sys_t *p_sys = p_demux->p_sys;

    if( !p_sys->b_dvb_meta || ( i_pid != 0x11 && i_pid != 0x12 && i_pid != 0x14 ) )
        return;

    msg_Warn( p_demux, "Switching to non DVB mode" );

    /* This doesn't look like a DVB stream so don't try
     * parsing the SDT/EDT/TDT */

    for( int i = 0x11; i <= 0x14; i++ )
    {
        if( i == 0x13 ) continue;
        ts_pid_t *p_pid = &p_sys->pid[i];
        if( p_pid->psi )
        {

#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
            if( dvbpsi_decoder_present( p_pid->psi->handle ))
                dvbpsi_DetachDemux( p_pid->psi->handle );
            dvbpsi_delete( p_pid->psi->handle );
#else
            dvbpsi_DetachDemux( p_pid->psi->handle );
#endif
            free( p_pid->psi );
            p_pid->psi = NULL;
            p_pid->b_valid = false;
        }
        SetPIDFilter( p_demux, i, false );
    }
    p_sys->b_dvb_meta = false;
}

#include "dvb-text.h"

static char *EITConvertToUTF8( demux_t *p_demux,
                               const unsigned char *psz_instring,
                               size_t i_length,
                               bool b_broken )
{
    demux_sys_t *p_sys = p_demux->p_sys;
#ifdef HAVE_ARIBB24
    if( p_sys->arib.e_mode == ARIBMODE_ENABLED )
    {
        if ( !p_sys->arib.p_instance )
            p_sys->arib.p_instance = arib_instance_new( p_demux );
        if ( !p_sys->arib.p_instance )
            return NULL;
        arib_decoder_t *p_decoder = arib_get_decoder( p_sys->arib.p_instance );
        if ( !p_decoder )
            return NULL;

        char *psz_outstring = NULL;
        size_t i_out;

        i_out = i_length * 4;
        psz_outstring = (char*) calloc( i_out + 1, sizeof(char) );
        if( !psz_outstring )
            return NULL;

        arib_initialize_decoder( p_decoder );
        i_out = arib_decode_buffer( p_decoder, psz_instring, i_length,
                                    psz_outstring, i_out );
        arib_finalize_decoder( p_decoder );

        return psz_outstring;
    }
#else
    VLC_UNUSED(p_sys);
#endif
    /* Deal with no longer broken providers (no switch byte
      but sending ISO_8859-1 instead of ISO_6937) without
      removing them from the broken providers table
      (keep the entry for correctly handling recorded TS).
    */
    b_broken = b_broken && i_length && *psz_instring > 0x20;

    if( b_broken )
        return FromCharset( "ISO_8859-1", psz_instring, i_length );
    return vlc_from_EIT( psz_instring, i_length );
}

static void SDTCallBack( demux_t *p_demux, dvbpsi_sdt_t *p_sdt )
{
    demux_sys_t          *p_sys = p_demux->p_sys;
    ts_pid_t             *sdt = &p_sys->pid[0x11];
    dvbpsi_sdt_service_t *p_srv;

    msg_Dbg( p_demux, "SDTCallBack called" );

    if( sdt->psi->i_sdt_version != -1 &&
        ( !p_sdt->b_current_next ||
          p_sdt->i_version == sdt->psi->i_sdt_version ) )
    {
        dvbpsi_DeleteSDT( p_sdt );
        return;
    }

    msg_Dbg( p_demux, "new SDT ts_id=%d version=%d current_next=%d "
             "network_id=%d",
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
             p_sdt->i_extension,
#else
             p_sdt->i_ts_id,
#endif
             p_sdt->i_version, p_sdt->b_current_next,
             p_sdt->i_network_id );

    p_sys->b_broken_charset = false;

    for( p_srv = p_sdt->p_first_service; p_srv; p_srv = p_srv->p_next )
    {
        vlc_meta_t          *p_meta;
        dvbpsi_descriptor_t *p_dr;

        const char *psz_type = NULL;
        const char *psz_status = NULL;

        msg_Dbg( p_demux, "  * service id=%d eit schedule=%d present=%d "
                 "running=%d free_ca=%d",
                 p_srv->i_service_id, p_srv->b_eit_schedule,
                 p_srv->b_eit_present, p_srv->i_running_status,
                 p_srv->b_free_ca );

        p_meta = vlc_meta_New();
        for( p_dr = p_srv->p_first_descriptor; p_dr; p_dr = p_dr->p_next )
        {
            if( p_dr->i_tag == 0x48 )
            {
                static const char *ppsz_type[17] = {
                    "Reserved",
                    "Digital television service",
                    "Digital radio sound service",
                    "Teletext service",
                    "NVOD reference service",
                    "NVOD time-shifted service",
                    "Mosaic service",
                    "PAL coded signal",
                    "SECAM coded signal",
                    "D/D2-MAC",
                    "FM Radio",
                    "NTSC coded signal",
                    "Data broadcast service",
                    "Reserved for Common Interface Usage",
                    "RCS Map (see EN 301 790 [35])",
                    "RCS FLS (see EN 301 790 [35])",
                    "DVB MHP service"
                };
                dvbpsi_service_dr_t *pD = dvbpsi_DecodeServiceDr( p_dr );
                char *str1 = NULL;
                char *str2 = NULL;

                /* Workarounds for broadcasters with broken EPG */

                if( p_sdt->i_network_id == 133 )
                    p_sys->b_broken_charset = true;  /* SKY DE & BetaDigital use ISO8859-1 */

                /* List of providers using ISO8859-1 */
                static const char ppsz_broken_providers[][8] = {
                    "CSAT",     /* CanalSat FR */
                    "GR1",      /* France televisions */
                    "MULTI4",   /* NT1 */
                    "MR5",      /* France 2/M6 HD */
                    ""
                };
                for( int i = 0; *ppsz_broken_providers[i]; i++ )
                {
                    const size_t i_length = strlen(ppsz_broken_providers[i]);
                    if( pD->i_service_provider_name_length == i_length &&
                        !strncmp( (char *)pD->i_service_provider_name, ppsz_broken_providers[i], i_length ) )
                        p_sys->b_broken_charset = true;
                }

                /* FIXME: Digital+ ES also uses ISO8859-1 */

                str1 = EITConvertToUTF8(p_demux,
                                        pD->i_service_provider_name,
                                        pD->i_service_provider_name_length,
                                        p_sys->b_broken_charset );
                str2 = EITConvertToUTF8(p_demux,
                                        pD->i_service_name,
                                        pD->i_service_name_length,
                                        p_sys->b_broken_charset );

                msg_Dbg( p_demux, "    - type=%d provider=%s name=%s",
                         pD->i_service_type, str1, str2 );

                vlc_meta_SetTitle( p_meta, str2 );
                vlc_meta_SetPublisher( p_meta, str1 );
                if( pD->i_service_type >= 0x01 && pD->i_service_type <= 0x10 )
                    psz_type = ppsz_type[pD->i_service_type];
                free( str1 );
                free( str2 );
            }
        }

        if( p_srv->i_running_status >= 0x01 && p_srv->i_running_status <= 0x04 )
        {
            static const char *ppsz_status[5] = {
                "Unknown",
                "Not running",
                "Starts in a few seconds",
                "Pausing",
                "Running"
            };
            psz_status = ppsz_status[p_srv->i_running_status];
        }

        if( psz_type )
            vlc_meta_AddExtra( p_meta, "Type", psz_type );
        if( psz_status )
            vlc_meta_AddExtra( p_meta, "Status", psz_status );

        es_out_Control( p_demux->out, ES_OUT_SET_GROUP_META,
                        p_srv->i_service_id, p_meta );
        vlc_meta_Delete( p_meta );
    }

    sdt->psi->i_sdt_version = p_sdt->i_version;
    dvbpsi_DeleteSDT( p_sdt );
}

/* i_year: year - 1900  i_month: 0-11  i_mday: 1-31 i_hour: 0-23 i_minute: 0-59 i_second: 0-59 */
static int64_t vlc_timegm( int i_year, int i_month, int i_mday, int i_hour, int i_minute, int i_second )
{
    static const int pn_day[12+1] = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
    int64_t i_day;

    if( i_year < 70 ||
        i_month < 0 || i_month > 11 || i_mday < 1 || i_mday > 31 ||
        i_hour < 0 || i_hour > 23 || i_minute < 0 || i_minute > 59 || i_second < 0 || i_second > 59 )
        return -1;

    /* Count the number of days */
    i_day = 365 * (i_year-70) + pn_day[i_month] + i_mday - 1;
#define LEAP(y) ( ((y)%4) == 0 && (((y)%100) != 0 || ((y)%400) == 0) ? 1 : 0)
    for( int i = 70; i < i_year; i++ )
        i_day += LEAP(1900+i);
    if( i_month > 1 )
        i_day += LEAP(1900+i_year);
#undef LEAP
    /**/
    return ((24*i_day + i_hour)*60 + i_minute)*60 + i_second;
}

static void EITDecodeMjd( int i_mjd, int *p_y, int *p_m, int *p_d )
{
    const int yp = (int)( ( (double)i_mjd - 15078.2)/365.25 );
    const int mp = (int)( ((double)i_mjd - 14956.1 - (int)(yp * 365.25)) / 30.6001 );
    const int c = ( mp == 14 || mp == 15 ) ? 1 : 0;

    *p_y = 1900 + yp + c*1;
    *p_m = mp - 1 - c*12;
    *p_d = i_mjd - 14956 - (int)(yp*365.25) - (int)(mp*30.6001);
}
#define CVT_FROM_BCD(v) ((((v) >> 4)&0xf)*10 + ((v)&0xf))
static int64_t EITConvertStartTime( uint64_t i_date )
{
    const int i_mjd = i_date >> 24;
    const int i_hour   = CVT_FROM_BCD(i_date >> 16);
    const int i_minute = CVT_FROM_BCD(i_date >>  8);
    const int i_second = CVT_FROM_BCD(i_date      );
    int i_year;
    int i_month;
    int i_day;

    /* if all 40 bits are 1, the start is unknown */
    if( i_date == UINT64_C(0xffffffffff) )
        return -1;

    EITDecodeMjd( i_mjd, &i_year, &i_month, &i_day );
    return vlc_timegm( i_year - 1900, i_month - 1, i_day, i_hour, i_minute, i_second );
}
static int EITConvertDuration( uint32_t i_duration )
{
    return CVT_FROM_BCD(i_duration >> 16) * 3600 +
           CVT_FROM_BCD(i_duration >> 8 ) * 60 +
           CVT_FROM_BCD(i_duration      );
}
#undef CVT_FROM_BCD

static void TDTCallBack( demux_t *p_demux, dvbpsi_tot_t *p_tdt )
{
    demux_sys_t        *p_sys = p_demux->p_sys;

    p_sys->i_tdt_delta = CLOCK_FREQ * EITConvertStartTime( p_tdt->i_utc_time )
                         - mdate();
    dvbpsi_DeleteTOT(p_tdt);
}


static void EITCallBack( demux_t *p_demux,
                         dvbpsi_eit_t *p_eit, bool b_current_following )
{
    demux_sys_t        *p_sys = p_demux->p_sys;
    dvbpsi_eit_event_t *p_evt;
    vlc_epg_t *p_epg;

    msg_Dbg( p_demux, "EITCallBack called" );
    if( !p_eit->b_current_next )
    {
        dvbpsi_DeleteEIT( p_eit );
        return;
    }

    msg_Dbg( p_demux, "new EIT service_id=%d version=%d current_next=%d "
             "ts_id=%d network_id=%d segment_last_section_number=%d "
             "last_table_id=%d",
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
             p_eit->i_extension,
#else
             p_eit->i_service_id,
#endif
             p_eit->i_version, p_eit->b_current_next,
             p_eit->i_ts_id, p_eit->i_network_id,
             p_eit->i_segment_last_section_number, p_eit->i_last_table_id );

    p_epg = vlc_epg_New( NULL );
    for( p_evt = p_eit->p_first_event; p_evt; p_evt = p_evt->p_next )
    {
        dvbpsi_descriptor_t *p_dr;
        char                *psz_name = NULL;
        char                *psz_text = NULL;
        char                *psz_extra = strdup("");
        int64_t i_start;
        int i_duration;
        int i_min_age = 0;
        int64_t i_tot_time = 0;

        i_start = EITConvertStartTime( p_evt->i_start_time );
        i_duration = EITConvertDuration( p_evt->i_duration );

        if( p_sys->arib.e_mode == ARIBMODE_ENABLED )
        {
            if( p_sys->i_tdt_delta == 0 )
                p_sys->i_tdt_delta = CLOCK_FREQ * (i_start + i_duration - 5) - mdate();

            i_tot_time = (mdate() + p_sys->i_tdt_delta) / CLOCK_FREQ;

            tzset(); // JST -> UTC
            i_start += timezone; // FIXME: what about DST?
            i_tot_time += timezone;

            if( p_evt->i_running_status == 0x00 &&
                (i_start - 5 < i_tot_time &&
                 i_tot_time < i_start + i_duration + 5) )
            {
                p_evt->i_running_status = 0x04;
                msg_Dbg( p_demux, "  EIT running status 0x00 -> 0x04" );
            }
        }

        msg_Dbg( p_demux, "  * event id=%d start_time:%d duration=%d "
                          "running=%d free_ca=%d",
                 p_evt->i_event_id, (int)i_start, (int)i_duration,
                 p_evt->i_running_status, p_evt->b_free_ca );

        for( p_dr = p_evt->p_first_descriptor; p_dr; p_dr = p_dr->p_next )
        {
            switch(p_dr->i_tag)
            {
            case 0x4d:
            {
                dvbpsi_short_event_dr_t *pE = dvbpsi_DecodeShortEventDr( p_dr );

                /* Only take first description, as we don't handle language-info
                   for epg atm*/
                if( pE && psz_name == NULL )
                {
                    psz_name = EITConvertToUTF8( p_demux,
                                                 pE->i_event_name, pE->i_event_name_length,
                                                 p_sys->b_broken_charset );
                    free( psz_text );
                    psz_text = EITConvertToUTF8( p_demux,
                                                 pE->i_text, pE->i_text_length,
                                                 p_sys->b_broken_charset );
                    msg_Dbg( p_demux, "    - short event lang=%3.3s '%s' : '%s'",
                             pE->i_iso_639_code, psz_name, psz_text );
                }
            }
                break;

            case 0x4e:
            {
                dvbpsi_extended_event_dr_t *pE = dvbpsi_DecodeExtendedEventDr( p_dr );
                if( pE )
                {
                    msg_Dbg( p_demux, "    - extended event lang=%3.3s [%d/%d]",
                             pE->i_iso_639_code,
                             pE->i_descriptor_number, pE->i_last_descriptor_number );

                    if( pE->i_text_length > 0 )
                    {
                        char *psz_text = EITConvertToUTF8( p_demux,
                                                           pE->i_text, pE->i_text_length,
                                                           p_sys->b_broken_charset );
                        if( psz_text )
                        {
                            msg_Dbg( p_demux, "       - text='%s'", psz_text );

                            psz_extra = xrealloc( psz_extra,
                                   strlen(psz_extra) + strlen(psz_text) + 1 );
                            strcat( psz_extra, psz_text );
                            free( psz_text );
                        }
                    }

                    for( int i = 0; i < pE->i_entry_count; i++ )
                    {
                        char *psz_dsc = EITConvertToUTF8( p_demux,
                                                          pE->i_item_description[i],
                                                          pE->i_item_description_length[i],
                                                          p_sys->b_broken_charset );
                        char *psz_itm = EITConvertToUTF8( p_demux,
                                                          pE->i_item[i], pE->i_item_length[i],
                                                          p_sys->b_broken_charset );

                        if( psz_dsc && psz_itm )
                        {
                            msg_Dbg( p_demux, "       - desc='%s' item='%s'", psz_dsc, psz_itm );
#if 0
                            psz_extra = xrealloc( psz_extra,
                                         strlen(psz_extra) + strlen(psz_dsc) +
                                         strlen(psz_itm) + 3 + 1 );
                            strcat( psz_extra, "(" );
                            strcat( psz_extra, psz_dsc );
                            strcat( psz_extra, " " );
                            strcat( psz_extra, psz_itm );
                            strcat( psz_extra, ")" );
#endif
                        }
                        free( psz_dsc );
                        free( psz_itm );
                    }
                }
            }
                break;

            case 0x55:
            {
                dvbpsi_parental_rating_dr_t *pR = dvbpsi_DecodeParentalRatingDr( p_dr );
                if ( pR )
                {
                    for ( int i = 0; i < pR->i_ratings_number; i++ )
                    {
                        const dvbpsi_parental_rating_t *p_rating = & pR->p_parental_rating[ i ];
                        if ( p_rating->i_rating > 0x00 && p_rating->i_rating <= 0x0F )
                        {
                            if ( p_rating->i_rating + 3 > i_min_age )
                                i_min_age = p_rating->i_rating + 3;
                            msg_Dbg( p_demux, "    - parental control set to %d years",
                                     i_min_age );
                        }
                    }
                }
            }
                break;

            default:
                msg_Dbg( p_demux, "    - event unknown dr 0x%x(%d)", p_dr->i_tag, p_dr->i_tag );
                break;
            }
        }

        /* */
        if( i_start > 0 && psz_name && psz_text)
            vlc_epg_AddEvent( p_epg, i_start, i_duration, psz_name, psz_text,
                              *psz_extra ? psz_extra : NULL, i_min_age );

        /* Update "now playing" field */
        if( p_evt->i_running_status == 0x04 && i_start > 0  && psz_name && psz_text )
            vlc_epg_SetCurrent( p_epg, i_start );

        free( psz_name );
        free( psz_text );

        free( psz_extra );
    }
    if( p_epg->i_event > 0 )
    {
        if( b_current_following &&
            (  p_sys->i_current_program == -1 ||
               p_sys->i_current_program ==
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
                    p_eit->i_extension
#else
                    p_eit->i_service_id
#endif
                ) )
        {
            p_sys->i_dvb_length = 0;
            p_sys->i_dvb_start = 0;

            if( p_epg->p_current )
            {
                p_sys->i_dvb_start = CLOCK_FREQ * p_epg->p_current->i_start;
                p_sys->i_dvb_length = CLOCK_FREQ * p_epg->p_current->i_duration;
            }
        }
        es_out_Control( p_demux->out, ES_OUT_SET_GROUP_EPG,
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
                        p_eit->i_extension,
#else
                        p_eit->i_service_id,
#endif
                        p_epg );
    }
    vlc_epg_Delete( p_epg );

    dvbpsi_DeleteEIT( p_eit );
}
static void EITCallBackCurrentFollowing( demux_t *p_demux, dvbpsi_eit_t *p_eit )
{
    EITCallBack( p_demux, p_eit, true );
}
static void EITCallBackSchedule( demux_t *p_demux, dvbpsi_eit_t *p_eit )
{
    EITCallBack( p_demux, p_eit, false );
}

#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
static void PSINewTableCallBack( dvbpsi_t *h, uint8_t i_table_id,
                                 uint16_t i_extension, demux_t *p_demux )
#else
static void PSINewTableCallBack( demux_t *p_demux, dvbpsi_handle h,
                                 uint8_t  i_table_id, uint16_t i_extension )
#endif
{
    assert( h );
#if 0
    msg_Dbg( p_demux, "PSINewTableCallBack: table 0x%x(%d) ext=0x%x(%d)",
             i_table_id, i_table_id, i_extension, i_extension );
#endif
    if( p_demux->p_sys->pid[0].psi->i_pat_version != -1 && i_table_id == 0x42 )
    {
        msg_Dbg( p_demux, "PSINewTableCallBack: table 0x%x(%d) ext=0x%x(%d)",
                 i_table_id, i_table_id, i_extension, i_extension );
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
        if( !dvbpsi_sdt_attach( h, i_table_id, i_extension, (dvbpsi_sdt_callback)SDTCallBack, p_demux ) )
            msg_Err( p_demux, "PSINewTableCallback: failed attaching SDTCallback" );
#else
        dvbpsi_AttachSDT( h, i_table_id, i_extension,
                          (dvbpsi_sdt_callback)SDTCallBack, p_demux );
#endif
    }
    else if( p_demux->p_sys->pid[0x11].psi->i_sdt_version != -1 &&
             ( i_table_id == 0x4e || /* Current/Following */
               (i_table_id >= 0x50 && i_table_id <= 0x5f) ) ) /* Schedule */
    {
        msg_Dbg( p_demux, "PSINewTableCallBack: table 0x%x(%d) ext=0x%x(%d)",
                 i_table_id, i_table_id, i_extension, i_extension );

        dvbpsi_eit_callback cb = i_table_id == 0x4e ?
                                    (dvbpsi_eit_callback)EITCallBackCurrentFollowing :
                                    (dvbpsi_eit_callback)EITCallBackSchedule;
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
        if( !dvbpsi_eit_attach( h, i_table_id, i_extension, cb, p_demux ) )
            msg_Err( p_demux, "PSINewTableCallback: failed attaching EITCallback" );
#else
        dvbpsi_AttachEIT( h, i_table_id, i_extension, cb, p_demux );
#endif
    }
    else if( p_demux->p_sys->pid[0x11].psi->i_sdt_version != -1 &&
            (i_table_id == 0x70 /* TDT */ || i_table_id == 0x73 /* TOT */) )
    {
         msg_Dbg( p_demux, "PSINewTableCallBack: table 0x%x(%d) ext=0x%x(%d)",
                 i_table_id, i_table_id, i_extension, i_extension );
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
        if( !dvbpsi_tot_attach( h, i_table_id, i_extension, (dvbpsi_tot_callback)TDTCallBack, p_demux ) )
            msg_Err( p_demux, "PSINewTableCallback: failed attaching TDTCallback" );
#else
         dvbpsi_AttachTOT( h, i_table_id, i_extension,
                           (dvbpsi_tot_callback)TDTCallBack, p_demux);
#endif
    }
}

/*****************************************************************************
 * PMT callback and helpers
 *****************************************************************************/
static dvbpsi_descriptor_t *PMTEsFindDescriptor( const dvbpsi_pmt_es_t *p_es,
                                                 int i_tag )
{
    dvbpsi_descriptor_t *p_dr = p_es->p_first_descriptor;;
    while( p_dr && ( p_dr->i_tag != i_tag ) )
        p_dr = p_dr->p_next;
    return p_dr;
}
static bool PMTEsHasRegistration( demux_t *p_demux,
                                  const dvbpsi_pmt_es_t *p_es,
                                  const char *psz_tag )
{
    dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, 0x05 );
    if( !p_dr )
        return false;

    if( p_dr->i_length < 4 )
    {
        msg_Warn( p_demux, "invalid Registration Descriptor" );
        return false;
    }

    assert( strlen(psz_tag) == 4 );
    return !memcmp( p_dr->p_data, psz_tag, 4 );
}

static bool PMTEsHasComponentTag( const dvbpsi_pmt_es_t *p_es,
                                  int i_component_tag )
{
    dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, 0x52 );
    if( !p_dr )
        return false;
    dvbpsi_stream_identifier_dr_t *p_si = dvbpsi_DecodeStreamIdentifierDr( p_dr );
    if( !p_si )
        return false;

    return p_si->i_component_tag == i_component_tag;
}

static void PMTSetupEsISO14496( demux_t *p_demux, ts_pid_t *pid,
                                const ts_prg_psi_t *prg, const dvbpsi_pmt_es_t *p_es )
{
    es_format_t *p_fmt = &pid->es->fmt;

    /* MPEG-4 stream: search FMC_DESCRIPTOR (SL Packetized stream) */
    dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, 0x1f );

    if( p_dr && p_dr->i_length == 2 )
    {
        const int i_es_id = ( p_dr->p_data[0] << 8 ) | p_dr->p_data[1];

        msg_Dbg( p_demux, "found FMC_descriptor declaring sl packetization on es_id=%d", i_es_id );

        pid->es->p_mpeg4desc = NULL;

        for( int i = 0; i < ES_DESCRIPTOR_COUNT; i++ )
        {
            iod_descriptor_t *iod = prg->iod;
            if( iod->es_descr[i].i_es_id == i_es_id )
            {
                if ( iod->es_descr[i].b_ok )
                    pid->es->p_mpeg4desc = &iod->es_descr[i];
                else
                    msg_Dbg( p_demux, "MPEG-4 descriptor not yet available on es_id=%d", i_es_id );
                break;
            }
        }
    }
    if( !pid->es->p_mpeg4desc )
    {
        switch( p_es->i_type )
        {
        /* non fatal, set by packetizer */
        case 0x0f: /* ADTS */
        case 0x11: /* LOAS */
            msg_Info( p_demux, "MPEG-4 descriptor not found for pid 0x%x type 0x%x",
                      pid->i_pid, p_es->i_type );
            break;
        default:
            msg_Err( p_demux, "MPEG-4 descriptor not found for pid 0x%x type 0x%x",
                     pid->i_pid, p_es->i_type );
            break;
        }
        return;
    }

    const decoder_config_descriptor_t *dcd = &pid->es->p_mpeg4desc->dec_descr;
    if( dcd->i_streamType == 0x04 )    /* VisualStream */
    {
        p_fmt->i_cat = VIDEO_ES;
        switch( dcd->i_objectTypeIndication )
        {
        case 0x0B: /* mpeg4 sub */
            p_fmt->i_cat = SPU_ES;
            p_fmt->i_codec = VLC_CODEC_SUBT;
            break;

        case 0x20: /* mpeg4 */
            p_fmt->i_codec = VLC_CODEC_MP4V;
            break;
        case 0x21: /* h264 */
            p_fmt->i_codec = VLC_CODEC_H264;
            break;
        case 0x60:
        case 0x61:
        case 0x62:
        case 0x63:
        case 0x64:
        case 0x65: /* mpeg2 */
            p_fmt->i_codec = VLC_CODEC_MPGV;
            break;
        case 0x6a: /* mpeg1 */
            p_fmt->i_codec = VLC_CODEC_MPGV;
            break;
        case 0x6c: /* mpeg1 */
            p_fmt->i_codec = VLC_CODEC_JPEG;
            break;
        default:
            p_fmt->i_cat = UNKNOWN_ES;
            break;
        }
    }
    else if( dcd->i_streamType == 0x05 )    /* AudioStream */
    {
        p_fmt->i_cat = AUDIO_ES;
        switch( dcd->i_objectTypeIndication )
        {
        case 0x40: /* mpeg4 */
            p_fmt->i_codec = VLC_CODEC_MP4A;
            break;
        case 0x66:
        case 0x67:
        case 0x68: /* mpeg2 aac */
            p_fmt->i_codec = VLC_CODEC_MP4A;
            break;
        case 0x69: /* mpeg2 */
            p_fmt->i_codec = VLC_CODEC_MPGA;
            break;
        case 0x6b: /* mpeg1 */
            p_fmt->i_codec = VLC_CODEC_MPGA;
            break;
        default:
            p_fmt->i_cat = UNKNOWN_ES;
            break;
        }
    }
    else
    {
        p_fmt->i_cat = UNKNOWN_ES;
    }

    if( p_fmt->i_cat != UNKNOWN_ES )
    {
        p_fmt->i_extra = dcd->i_extra;
        if( p_fmt->i_extra > 0 )
        {
            p_fmt->p_extra = malloc( p_fmt->i_extra );
            if( p_fmt->p_extra )
                memcpy( p_fmt->p_extra, dcd->p_extra, p_fmt->i_extra );
            else
                p_fmt->i_extra = 0;
        }
    }
}

typedef struct
{
    int  i_type;
    int  i_magazine;
    int  i_page;
    char p_iso639[3];
} ts_teletext_page_t;

static void PMTSetupEsTeletext( demux_t *p_demux, ts_pid_t *pid,
                                const dvbpsi_pmt_es_t *p_es )
{
    es_format_t *p_fmt = &pid->es->fmt;

    ts_teletext_page_t p_page[2 * 64 + 20];
    unsigned i_page = 0;

    /* Gather pages information */
#if defined _DVBPSI_DR_56_H_ && \
    defined DVBPSI_VERSION && DVBPSI_VERSION_INT > DVBPSI_VERSION_WANTED(0,1,5)
    for( unsigned i_tag_idx = 0; i_tag_idx < 2; i_tag_idx++ )
    {
        dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, i_tag_idx == 0 ? 0x46 : 0x56 );
        if( !p_dr )
            continue;

        dvbpsi_teletext_dr_t *p_sub = dvbpsi_DecodeTeletextDr( p_dr );
        if( !p_sub )
            continue;

        for( int i = 0; i < p_sub->i_pages_number; i++ )
        {
            const dvbpsi_teletextpage_t *p_src = &p_sub->p_pages[i];

            if( p_src->i_teletext_type >= 0x06 )
                continue;

            assert( i_page < sizeof(p_page)/sizeof(*p_page) );

            ts_teletext_page_t *p_dst = &p_page[i_page++];

            p_dst->i_type = p_src->i_teletext_type;
            p_dst->i_magazine = p_src->i_teletext_magazine_number
                ? p_src->i_teletext_magazine_number : 8;
            p_dst->i_page = p_src->i_teletext_page_number;
            memcpy( p_dst->p_iso639, p_src->i_iso6392_language_code, 3 );
        }
    }
#endif

    dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, 0x59 );
    if( p_dr )
    {
        dvbpsi_subtitling_dr_t *p_sub = dvbpsi_DecodeSubtitlingDr( p_dr );
        for( int i = 0; p_sub && i < p_sub->i_subtitles_number; i++ )
        {
            dvbpsi_subtitle_t *p_src = &p_sub->p_subtitle[i];

            if( p_src->i_subtitling_type < 0x01 || p_src->i_subtitling_type > 0x03 )
                continue;

            assert( i_page < sizeof(p_page)/sizeof(*p_page) );

            ts_teletext_page_t *p_dst = &p_page[i_page++];

            switch( p_src->i_subtitling_type )
            {
            case 0x01:
                p_dst->i_type = 0x02;
                break;
            default:
                p_dst->i_type = 0x03;
                break;
            }
            /* FIXME check if it is the right split */
            p_dst->i_magazine = (p_src->i_composition_page_id >> 8)
                ? (p_src->i_composition_page_id >> 8) : 8;
            p_dst->i_page = p_src->i_composition_page_id & 0xff;
            memcpy( p_dst->p_iso639, p_src->i_iso6392_language_code, 3 );
        }
    }

    /* */
    es_format_Init( p_fmt, SPU_ES, VLC_CODEC_TELETEXT );

    if( !p_demux->p_sys->b_split_es || i_page <= 0 )
    {
        p_fmt->subs.teletext.i_magazine = -1;
        p_fmt->subs.teletext.i_page = 0;
        p_fmt->psz_description = strdup( vlc_gettext(ppsz_teletext_type[1]) );

        dvbpsi_descriptor_t *p_dr;
        p_dr = PMTEsFindDescriptor( p_es, 0x46 );
        if( !p_dr )
            p_dr = PMTEsFindDescriptor( p_es, 0x56 );

        if( !p_demux->p_sys->b_split_es && p_dr && p_dr->i_length > 0 )
        {
            /* Descriptor pass-through */
            p_fmt->p_extra = malloc( p_dr->i_length );
            if( p_fmt->p_extra )
            {
                p_fmt->i_extra = p_dr->i_length;
                memcpy( p_fmt->p_extra, p_dr->p_data, p_dr->i_length );
            }
        }
    }
    else
    {
        for( unsigned i = 0; i < i_page; i++ )
        {
            ts_es_t *p_es;

            /* */
            if( i == 0 )
            {
                p_es = pid->es;
            }
            else
            {
                p_es = malloc( sizeof(*p_es) );
                if( !p_es )
                    break;

                es_format_Copy( &p_es->fmt, &pid->es->fmt );
                free( p_es->fmt.psz_language );
                free( p_es->fmt.psz_description );
                p_es->fmt.psz_language = NULL;
                p_es->fmt.psz_description = NULL;

                p_es->id      = NULL;
                p_es->p_data  = NULL;
                p_es->i_data_size = 0;
                p_es->i_data_gathered = 0;
                p_es->pp_last = &p_es->p_data;
                p_es->data_type = TS_ES_DATA_PES;
                p_es->p_mpeg4desc = NULL;

                TAB_APPEND( pid->i_extra_es, pid->extra_es, p_es );
            }

            /* */
            const ts_teletext_page_t *p = &p_page[i];
            p_es->fmt.i_priority = (p->i_type == 0x02 || p->i_type == 0x05) ?
                      ES_PRIORITY_SELECTABLE_MIN : ES_PRIORITY_NOT_DEFAULTABLE;
            p_es->fmt.psz_language = strndup( p->p_iso639, 3 );
            p_es->fmt.psz_description = strdup(vlc_gettext(ppsz_teletext_type[p->i_type]));
            p_es->fmt.subs.teletext.i_magazine = p->i_magazine;
            p_es->fmt.subs.teletext.i_page = p->i_page;

            msg_Dbg( p_demux,
                         "    * ttxt type=%s lan=%s page=%d%02x",
                         p_es->fmt.psz_description,
                         p_es->fmt.psz_language,
                         p->i_magazine, p->i_page );
        }
    }
}
static void PMTSetupEsDvbSubtitle( demux_t *p_demux, ts_pid_t *pid,
                                   const dvbpsi_pmt_es_t *p_es )
{
    es_format_t *p_fmt = &pid->es->fmt;

    es_format_Init( p_fmt, SPU_ES, VLC_CODEC_DVBS );

    dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, 0x59 );
    int i_page = 0;
    dvbpsi_subtitling_dr_t *p_sub = dvbpsi_DecodeSubtitlingDr( p_dr );
    for( int i = 0; p_sub && i < p_sub->i_subtitles_number; i++ )
    {
        const int i_type = p_sub->p_subtitle[i].i_subtitling_type;
        if( ( i_type >= 0x10 && i_type <= 0x14 ) ||
            ( i_type >= 0x20 && i_type <= 0x24 ) )
            i_page++;
    }

    if( !p_demux->p_sys->b_split_es  || i_page <= 0 )
    {
        p_fmt->subs.dvb.i_id = -1;
        p_fmt->psz_description = strdup( _("DVB subtitles") );

        if( !p_demux->p_sys->b_split_es && p_dr && p_dr->i_length > 0 )
        {
            /* Descriptor pass-through */
            p_fmt->p_extra = malloc( p_dr->i_length );
            if( p_fmt->p_extra )
            {
                p_fmt->i_extra = p_dr->i_length;
                memcpy( p_fmt->p_extra, p_dr->p_data, p_dr->i_length );
            }
        }
    }
    else
    {
        for( int i = 0; i < p_sub->i_subtitles_number; i++ )
        {
            ts_es_t *p_es;

            /* */
            if( i == 0 )
            {
                p_es = pid->es;
            }
            else
            {
                p_es = malloc( sizeof(*p_es) );
                if( !p_es )
                    break;

                es_format_Copy( &p_es->fmt, &pid->es->fmt );
                free( p_es->fmt.psz_language );
                free( p_es->fmt.psz_description );
                p_es->fmt.psz_language = NULL;
                p_es->fmt.psz_description = NULL;

                p_es->id      = NULL;
                p_es->p_data   = NULL;
                p_es->i_data_size = 0;
                p_es->i_data_gathered = 0;
                p_es->pp_last = &p_es->p_data;
                p_es->data_type = TS_ES_DATA_PES;
                p_es->p_mpeg4desc = NULL;

                TAB_APPEND( pid->i_extra_es, pid->extra_es, p_es );
            }

            /* */
            const dvbpsi_subtitle_t *p = &p_sub->p_subtitle[i];
            p_es->fmt.psz_language = strndup( (char *)p->i_iso6392_language_code, 3 );
            switch( p->i_subtitling_type )
            {
            case 0x10: /* unspec. */
            case 0x11: /* 4:3 */
            case 0x12: /* 16:9 */
            case 0x13: /* 2.21:1 */
            case 0x14: /* HD monitor */
                p_es->fmt.psz_description = strdup( _("DVB subtitles") );
                break;
            case 0x20: /* Hearing impaired unspec. */
            case 0x21: /* h.i. 4:3 */
            case 0x22: /* h.i. 16:9 */
            case 0x23: /* h.i. 2.21:1 */
            case 0x24: /* h.i. HD monitor */
                p_es->fmt.psz_description = strdup( _("DVB subtitles: hearing impaired") );
                break;
            default:
                break;
            }

            /* Hack, FIXME */
            p_es->fmt.subs.dvb.i_id = ( p->i_composition_page_id <<  0 ) |
                                      ( p->i_ancillary_page_id   << 16 );
        }
    }
}

static int vlc_ceil_log2( const unsigned int val )
{
    int n = 31 - clz(val);
    if ((1U << n) != val)
        n++;

    return n;
}

static void OpusSetup(demux_t *demux, uint8_t *p, size_t len, es_format_t *p_fmt)
{
    OpusHeader h;

    /* default mapping */
    static const unsigned char map[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    memcpy(h.stream_map, map, sizeof(map));

    int csc, mapping;
    int channels = 0;
    int stream_count = 0;
    int ccc = p[1]; // channel_config_code
    if (ccc <= 8) {
        channels = ccc;
        if (channels)
            mapping = channels > 2;
        else {
            mapping = 255;
            channels = 2; // dual mono
        }
        static const uint8_t p_csc[8] = { 0, 1, 1, 2, 2, 2, 3, 3 };
        csc = p_csc[channels - 1];
        stream_count = channels - csc;

        static const uint8_t map[6][7] = {
            { 2,1 },
            { 1,2,3 },
            { 4,1,2,3 },
            { 4,1,2,3,5 },
            { 4,1,2,3,5,6 },
            { 6,1,2,3,4,5,7 },
        };
        if (channels > 2)
            memcpy(&h.stream_map[1], map[channels-3], channels - 1);
    } else if (ccc == 0x81) {
        if (len < 4)
            goto explicit_config_too_short;

        channels = p[2];
        mapping = p[3];
        csc = 0;
        if (mapping) {
            bs_t s;
            bs_init(&s, &p[4], len - 4);
            stream_count = 1;
            if (channels) {
                int bits = vlc_ceil_log2(channels);
                if (s.i_left < bits)
                    goto explicit_config_too_short;
                stream_count = bs_read(&s, bits) + 1;
                bits = vlc_ceil_log2(stream_count + 1);
                if (s.i_left < bits)
                    goto explicit_config_too_short;
                csc = bs_read(&s, bits);
            }
            int channel_bits = vlc_ceil_log2(stream_count + csc + 1);
            if (s.i_left < channels * channel_bits)
                goto explicit_config_too_short;

            unsigned char silence = (1U << (stream_count + csc + 1)) - 1;
            for (int i = 0; i < channels; i++) {
                unsigned char m = bs_read(&s, channel_bits);
                if (m == silence)
                    m = 0xff;
                h.stream_map[i] = m;
            }
        }
    } else if (ccc >= 0x80 && ccc <= 0x88) {
        channels = ccc - 0x80;
        if (channels)
            mapping = 1;
        else {
            mapping = 255;
            channels = 2; // dual mono
        }
        csc = 0;
        stream_count = channels;
    } else {
        msg_Err(demux, "Opus channel configuration 0x%.2x is reserved", ccc);
    }

    if (!channels) {
        msg_Err(demux, "Opus channel configuration 0x%.2x not supported yet", p[1]);
        return;
    }

    opus_prepare_header(channels, 0, &h);
    h.preskip = 0;
    h.input_sample_rate = 48000;
    h.nb_coupled = csc;
    h.nb_streams = channels - csc;
    h.channel_mapping = mapping;

    if (h.channels) {
        opus_write_header((uint8_t**)&p_fmt->p_extra, &p_fmt->i_extra, &h, NULL /* FIXME */);
        if (p_fmt->p_extra) {
            p_fmt->i_cat = AUDIO_ES;
            p_fmt->i_codec = VLC_CODEC_OPUS;
            p_fmt->audio.i_channels = h.channels;
            p_fmt->audio.i_rate = 48000;
        }
    }

    return;

explicit_config_too_short:
    msg_Err(demux, "Opus descriptor too short");
}

static void PMTSetupEs0x06( demux_t *p_demux, ts_pid_t *pid,
                            const dvbpsi_pmt_es_t *p_es )
{
    es_format_t *p_fmt = &pid->es->fmt;
    dvbpsi_descriptor_t *p_subs_dr = PMTEsFindDescriptor( p_es, 0x59 );
    dvbpsi_descriptor_t *desc;

    if( PMTEsHasRegistration( p_demux, p_es, "AC-3" ) ||
        PMTEsFindDescriptor( p_es, 0x6a ) ||
        PMTEsFindDescriptor( p_es, 0x81 ) )
    {
        p_fmt->i_cat = AUDIO_ES;
        p_fmt->i_codec = VLC_CODEC_A52;
    }
    else if( (desc = PMTEsFindDescriptor( p_es, 0x7f ) ) && desc->i_length >= 2 &&
              PMTEsHasRegistration(p_demux, p_es, "Opus"))
    {
        OpusSetup(p_demux, desc->p_data, desc->i_length, p_fmt);
    }
    else if( PMTEsFindDescriptor( p_es, 0x7a ) )
    {
        /* DVB with stream_type 0x06 (ETS EN 300 468) */
        p_fmt->i_cat = AUDIO_ES;
        p_fmt->i_codec = VLC_CODEC_EAC3;
    }
    else if( PMTEsHasRegistration( p_demux, p_es, "DTS1" ) ||
             PMTEsHasRegistration( p_demux, p_es, "DTS2" ) ||
             PMTEsHasRegistration( p_demux, p_es, "DTS3" ) ||
             PMTEsFindDescriptor( p_es, 0x73 ) )
    {
        /*registration descriptor(ETSI TS 101 154 Annex F)*/
        p_fmt->i_cat = AUDIO_ES;
        p_fmt->i_codec = VLC_CODEC_DTS;
    }
    else if( PMTEsHasRegistration( p_demux, p_es, "BSSD" ) && !p_subs_dr )
    {
        /* BSSD is AES3 DATA, but could also be subtitles
         * we need to check for secondary descriptor then s*/
        p_fmt->i_cat = AUDIO_ES;
        p_fmt->b_packetized = true;
        p_fmt->i_codec = VLC_CODEC_302M;
    }
    else if( PMTEsHasRegistration( p_demux, p_es, "HEVC" ) )
    {
        p_fmt->i_cat = VIDEO_ES;
        p_fmt->i_codec = VLC_CODEC_HEVC;
    }
    else if ( p_demux->p_sys->arib.e_mode == ARIBMODE_ENABLED )
    {
        /* Lookup our data component descriptor first ARIB STD B10 6.4 */
        dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, 0xFD );
        /* and check that it maps to something ARIB STD B14 Table 5.1/5.2 */
        if ( p_dr && p_dr->i_length >= 2 )
        {
            if( !memcmp( p_dr->p_data, "\x00\x08", 2 ) &&  (
                    PMTEsHasComponentTag( p_es, 0x30 ) ||
                    PMTEsHasComponentTag( p_es, 0x31 ) ||
                    PMTEsHasComponentTag( p_es, 0x32 ) ||
                    PMTEsHasComponentTag( p_es, 0x33 ) ||
                    PMTEsHasComponentTag( p_es, 0x34 ) ||
                    PMTEsHasComponentTag( p_es, 0x35 ) ||
                    PMTEsHasComponentTag( p_es, 0x36 ) ||
                    PMTEsHasComponentTag( p_es, 0x37 ) ) )
            {
                es_format_Init( &pid->es->fmt, SPU_ES, VLC_CODEC_ARIB_A );
                p_fmt->psz_language = strndup ( "jpn", 3 );
                p_fmt->psz_description = strdup( _("ARIB subtitles") );
            }
            else if( !memcmp( p_dr->p_data, "\x00\x12", 2 ) && (
                     PMTEsHasComponentTag( p_es, 0x87 ) ||
                     PMTEsHasComponentTag( p_es, 0x88 ) ) )
            {
                es_format_Init( &pid->es->fmt, SPU_ES, VLC_CODEC_ARIB_C );
                p_fmt->psz_language = strndup ( "jpn", 3 );
                p_fmt->psz_description = strdup( _("ARIB subtitles") );
            }
        }
    }
    else
    {
        /* Subtitle/Teletext/VBI fallbacks */
        dvbpsi_subtitling_dr_t *p_sub;
        if( p_subs_dr && ( p_sub = dvbpsi_DecodeSubtitlingDr( p_subs_dr ) ) )
        {
            for( int i = 0; i < p_sub->i_subtitles_number; i++ )
            {
                if( p_fmt->i_cat != UNKNOWN_ES )
                    break;

                switch( p_sub->p_subtitle[i].i_subtitling_type )
                {
                case 0x01: /* EBU Teletext subtitles */
                case 0x02: /* Associated EBU Teletext */
                case 0x03: /* VBI data */
                    PMTSetupEsTeletext( p_demux, pid, p_es );
                    break;
                case 0x10: /* DVB Subtitle (normal) with no monitor AR critical */
                case 0x11: /*                 ...   on 4:3 AR monitor */
                case 0x12: /*                 ...   on 16:9 AR monitor */
                case 0x13: /*                 ...   on 2.21:1 AR monitor */
                case 0x14: /*                 ...   for display on a high definition monitor */
                case 0x20: /* DVB Subtitle (impaired) with no monitor AR critical */
                case 0x21: /*                 ...   on 4:3 AR monitor */
                case 0x22: /*                 ...   on 16:9 AR monitor */
                case 0x23: /*                 ...   on 2.21:1 AR monitor */
                case 0x24: /*                 ...   for display on a high definition monitor */
                    PMTSetupEsDvbSubtitle( p_demux, pid, p_es );
                    break;
                default:
                    msg_Err( p_demux, "Unrecognized DVB subtitle type (0x%x)",
                             p_sub->p_subtitle[i].i_subtitling_type );
                    break;
                }
            }
        }

        if( p_fmt->i_cat == UNKNOWN_ES &&
            ( PMTEsFindDescriptor( p_es, 0x45 ) ||  /* VBI Data descriptor */
              PMTEsFindDescriptor( p_es, 0x46 ) ||  /* VBI Teletext descriptor */
              PMTEsFindDescriptor( p_es, 0x56 ) ) ) /* EBU Teletext descriptor */
        {
            /* Teletext/VBI */
            PMTSetupEsTeletext( p_demux, pid, p_es );
        }
    }

    /* FIXME is it useful ? */
    if( PMTEsFindDescriptor( p_es, 0x52 ) )
    {
        dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, 0x52 );
        dvbpsi_stream_identifier_dr_t *p_si = dvbpsi_DecodeStreamIdentifierDr( p_dr );

        msg_Dbg( p_demux, "    * Stream Component Identifier: %d", p_si->i_component_tag );
    }
}

static void PMTSetupEs0xEA( demux_t *p_demux, ts_pid_t *pid,
                           const dvbpsi_pmt_es_t *p_es )
{
    /* Registration Descriptor */
    if( !PMTEsHasRegistration( p_demux, p_es, "VC-1" ) )
    {
        msg_Err( p_demux, "Registration descriptor not found or invalid" );
        return;
    }

    es_format_t *p_fmt = &pid->es->fmt;

    /* registration descriptor for VC-1 (SMPTE rp227) */
    p_fmt->i_cat = VIDEO_ES;
    p_fmt->i_codec = VLC_CODEC_VC1;

    /* XXX With Simple and Main profile the SEQUENCE
     * header is modified: video width and height are
     * inserted just after the start code as 2 int16_t
     * The packetizer will take care of that. */
}

static void PMTSetupEs0xD1( demux_t *p_demux, ts_pid_t *pid,
                           const dvbpsi_pmt_es_t *p_es )
{
    /* Registration Descriptor */
    if( !PMTEsHasRegistration( p_demux, p_es, "drac" ) )
    {
        msg_Err( p_demux, "Registration descriptor not found or invalid" );
        return;
    }

    es_format_t *p_fmt = &pid->es->fmt;

    /* registration descriptor for Dirac
     * (backwards compatable with VC-2 (SMPTE Sxxxx:2008)) */
    p_fmt->i_cat = VIDEO_ES;
    p_fmt->i_codec = VLC_CODEC_DIRAC;
}

static void PMTSetupEs0xA0( demux_t *p_demux, ts_pid_t *pid,
                           const dvbpsi_pmt_es_t *p_es )
{
    /* MSCODEC sent by vlc */
    dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, 0xa0 );
    if( !p_dr || p_dr->i_length < 10 )
    {
        msg_Warn( p_demux,
                  "private MSCODEC (vlc) without bih private descriptor" );
        return;
    }

    es_format_t *p_fmt = &pid->es->fmt;
    p_fmt->i_cat = VIDEO_ES;
    p_fmt->i_codec = VLC_FOURCC( p_dr->p_data[0], p_dr->p_data[1],
                                 p_dr->p_data[2], p_dr->p_data[3] );
    p_fmt->video.i_width = GetWBE( &p_dr->p_data[4] );
    p_fmt->video.i_height = GetWBE( &p_dr->p_data[6] );
    p_fmt->i_extra = GetWBE( &p_dr->p_data[8] );

    if( p_fmt->i_extra > 0 )
    {
        p_fmt->p_extra = malloc( p_fmt->i_extra );
        if( p_fmt->p_extra )
            memcpy( p_fmt->p_extra, &p_dr->p_data[10],
                    __MIN( p_fmt->i_extra, p_dr->i_length - 10 ) );
        else
            p_fmt->i_extra = 0;
    }
    /* For such stream we will gather them ourself and don't launch a
     * packetizer.
     * Yes it's ugly but it's the only way to have DIV3 working */
    p_fmt->b_packetized = true;
}

static void PMTSetupEs0x83( const dvbpsi_pmt_t *p_pmt, ts_pid_t *pid )
{
    /* WiDi broadcasts without registration on PMT 0x1, PCR 0x1000 and
     * with audio track pid being 0x1100..0x11FF */
    if ( p_pmt->i_program_number == 0x1 &&
         p_pmt->i_pcr_pid == 0x1000 &&
        ( pid->i_pid >> 8 ) == 0x11 )
    {
        /* Not enough ? might contain 0x83 private descriptor, 2 bytes 0x473F */
        es_format_Init( &pid->es->fmt, AUDIO_ES, VLC_CODEC_WIDI_LPCM );
    }
    else
        es_format_Init( &pid->es->fmt, AUDIO_ES, VLC_CODEC_DVD_LPCM );
}

static bool PMTSetupEsHDMV( demux_t *p_demux, ts_pid_t *pid,
                            const dvbpsi_pmt_es_t *p_es )
{
    es_format_t *p_fmt = &pid->es->fmt;

    /* Blu-Ray mapping */
    switch( p_es->i_type )
    {
    case 0x80:
        p_fmt->i_cat = AUDIO_ES;
        p_fmt->i_codec = VLC_CODEC_BD_LPCM;
        break;
    case 0x82:
    case 0x85: /* DTS-HD High resolution audio */
    case 0x86: /* DTS-HD Master audio */
    case 0xA2: /* Secondary DTS audio */
        p_fmt->i_cat = AUDIO_ES;
        p_fmt->i_codec = VLC_CODEC_DTS;
        break;

    case 0x83: /* TrueHD AC3 */
        p_fmt->i_cat = AUDIO_ES;
        p_fmt->i_codec = VLC_CODEC_TRUEHD;
        break;

    case 0x84: /* E-AC3 */
    case 0xA1: /* Secondary E-AC3 */
        p_fmt->i_cat = AUDIO_ES;
        p_fmt->i_codec = VLC_CODEC_EAC3;
        break;
    case 0x90: /* Presentation graphics */
        p_fmt->i_cat = SPU_ES;
        p_fmt->i_codec = VLC_CODEC_BD_PG;
        break;
    case 0x91: /* Interactive graphics */
    case 0x92: /* Subtitle */
        return false;
    default:
        msg_Info( p_demux, "HDMV registration not implemented for pid 0x%x type 0x%x",
                  p_es->i_pid, p_es->i_type );
        return false;
        break;
    }
    return true;
}

static bool PMTSetupEsRegistration( demux_t *p_demux, ts_pid_t *pid,
                                    const dvbpsi_pmt_es_t *p_es )
{
    static const struct
    {
        char         psz_tag[5];
        int          i_cat;
        vlc_fourcc_t i_codec;
    } p_regs[] = {
        { "AC-3", AUDIO_ES, VLC_CODEC_A52   },
        { "DTS1", AUDIO_ES, VLC_CODEC_DTS   },
        { "DTS2", AUDIO_ES, VLC_CODEC_DTS   },
        { "DTS3", AUDIO_ES, VLC_CODEC_DTS   },
        { "BSSD", AUDIO_ES, VLC_CODEC_302M  },
        { "VC-1", VIDEO_ES, VLC_CODEC_VC1   },
        { "drac", VIDEO_ES, VLC_CODEC_DIRAC },
        { "", UNKNOWN_ES, 0 }
    };
    es_format_t *p_fmt = &pid->es->fmt;

    for( int i = 0; p_regs[i].i_cat != UNKNOWN_ES; i++ )
    {
        if( PMTEsHasRegistration( p_demux, p_es, p_regs[i].psz_tag ) )
        {
            p_fmt->i_cat   = p_regs[i].i_cat;
            p_fmt->i_codec = p_regs[i].i_codec;
            if (p_es->i_type == 0x87)
                p_fmt->i_codec = VLC_CODEC_EAC3;
            return true;
        }
    }
    return false;
}

static char *GetAudioTypeDesc(demux_t *p_demux, int type)
{
    static const char *audio_type[] = {
        NULL,
        N_("clean effects"),
        N_("hearing impaired"),
        N_("visual impaired commentary"),
    };

    if (type < 0 || type > 3)
        msg_Dbg( p_demux, "unknown audio type: %d", type);
    else if (type > 0)
        return strdup(audio_type[type]);

    return NULL;
}
static void PMTParseEsIso639( demux_t *p_demux, ts_pid_t *pid,
                              const dvbpsi_pmt_es_t *p_es )
{
    /* get language descriptor */
    dvbpsi_descriptor_t *p_dr = PMTEsFindDescriptor( p_es, 0x0a );

    if( !p_dr )
        return;

    dvbpsi_iso639_dr_t *p_decoded = dvbpsi_DecodeISO639Dr( p_dr );
    if( !p_decoded )
    {
        msg_Err( p_demux, "Failed to decode a ISO 639 descriptor" );
        return;
    }

#if defined(DR_0A_API_VER) && (DR_0A_API_VER >= 2)
    pid->es->fmt.psz_language = malloc( 4 );
    if( pid->es->fmt.psz_language )
    {
        memcpy( pid->es->fmt.psz_language, p_decoded->code[0].iso_639_code, 3 );
        pid->es->fmt.psz_language[3] = 0;
        msg_Dbg( p_demux, "found language: %s", pid->es->fmt.psz_language);
    }
    int type = p_decoded->code[0].i_audio_type;
    pid->es->fmt.psz_description = GetAudioTypeDesc(p_demux, type);
    if (type == 0)
        pid->es->fmt.i_priority = ES_PRIORITY_SELECTABLE_MIN + 1; // prioritize normal audio tracks

    pid->es->fmt.i_extra_languages = p_decoded->i_code_count-1;
    if( pid->es->fmt.i_extra_languages > 0 )
        pid->es->fmt.p_extra_languages =
            malloc( sizeof(*pid->es->fmt.p_extra_languages) *
                    pid->es->fmt.i_extra_languages );
    if( pid->es->fmt.p_extra_languages )
    {
        for( int i = 0; i < pid->es->fmt.i_extra_languages; i++ )
        {
            pid->es->fmt.p_extra_languages[i].psz_language = malloc(4);
            if( pid->es->fmt.p_extra_languages[i].psz_language )
            {
                memcpy( pid->es->fmt.p_extra_languages[i].psz_language,
                    p_decoded->code[i+1].iso_639_code, 3 );
                pid->es->fmt.p_extra_languages[i].psz_language[3] = '\0';
            }
            int type = p_decoded->code[i].i_audio_type;
            pid->es->fmt.p_extra_languages[i].psz_description = GetAudioTypeDesc(p_demux, type);
        }
    }
#else
    pid->es->fmt.psz_language = malloc( 4 );
    if( pid->es->fmt.psz_language )
    {
        memcpy( pid->es->fmt.psz_language,
                p_decoded->i_iso_639_code, 3 );
        pid->es->fmt.psz_language[3] = 0;
    }
#endif
}

static void PMTCallBack( void *data, dvbpsi_pmt_t *p_pmt )
{
    demux_t      *p_demux = data;
    demux_sys_t  *p_sys = p_demux->p_sys;

    ts_pid_t     *pmt = NULL;
    ts_prg_psi_t *prg;

    msg_Dbg( p_demux, "PMTCallBack called" );

    /* First find this PMT declared in PAT */
    for( int i = 0; !pmt && i < p_sys->i_pmt; i++ )
        for( int i_prg = 0; !pmt && i_prg < p_sys->pmt[i]->psi->i_prg; i_prg++ )
        {
            const int i_pmt_number = p_sys->pmt[i]->psi->prg[i_prg]->i_number;
            if( i_pmt_number != TS_USER_PMT_NUMBER &&
                i_pmt_number == p_pmt->i_program_number )
            {
                pmt = p_sys->pmt[i];
                prg = p_sys->pmt[i]->psi->prg[i_prg];
            }
        }

    if( pmt == NULL )
    {
        msg_Warn( p_demux, "unreferenced program (broken stream)" );
        dvbpsi_DeletePMT(p_pmt);
        return;
    }


    if( prg->i_version != -1 &&
        ( !p_pmt->b_current_next || prg->i_version == p_pmt->i_version ) )
    {
        dvbpsi_DeletePMT( p_pmt );
        return;
    }

    ts_pid_t **pp_clean = NULL;
    int      i_clean = 0;
    /* Clean this program (remove all es) */
    for( int i = 0; i < 8192; i++ )
    {
        ts_pid_t *pid = &p_sys->pid[i];

        if( pid->b_valid && pid->p_owner == pmt->psi &&
            pid->i_owner_number == prg->i_number && pid->psi == NULL )
        {
            TAB_APPEND( i_clean, pp_clean, pid );
        }
    }
    if( prg->iod )
    {
        IODFree( prg->iod );
        prg->iod = NULL;
    }

    msg_Dbg( p_demux, "new PMT program number=%d version=%d pid_pcr=%d",
             p_pmt->i_program_number, p_pmt->i_version, p_pmt->i_pcr_pid );
    prg->i_pid_pcr = p_pmt->i_pcr_pid;
    prg->i_version = p_pmt->i_version;

    ValidateDVBMeta( p_demux, prg->i_pid_pcr );
    if( ProgramIsSelected( p_demux, prg->i_number ) )
        SetPIDFilter( p_demux, prg->i_pid_pcr, true ); /* Set demux filter */

    /* Parse PMT descriptors */
    ts_pmt_registration_type_t registration_type = TS_PMT_REGISTRATION_NONE;
    dvbpsi_descriptor_t  *p_dr;

    /* First pass for standard detection */
    if ( p_sys->arib.e_mode == ARIBMODE_AUTO )
    {
        int i_arib_flags = 0; /* Descriptors can be repeated */
        for( p_dr = p_pmt->p_first_descriptor; p_dr != NULL; p_dr = p_dr->p_next )
        {
            switch(p_dr->i_tag)
            {
            case 0x09:
            {
                dvbpsi_ca_dr_t *p_cadr = dvbpsi_DecodeCADr( p_dr );
                i_arib_flags |= (p_cadr->i_ca_system_id == 0x05);
            }
                break;
            case 0xF6:
                i_arib_flags |= 1 << 1;
                break;
            case 0xC1:
                i_arib_flags |= 1 << 2;
                break;
            default:
                break;
            }
        }
        if ( i_arib_flags == 0b111 )
            p_sys->arib.e_mode = ARIBMODE_ENABLED;
    }

    for( p_dr = p_pmt->p_first_descriptor; p_dr != NULL; p_dr = p_dr->p_next )
    {
        /* special descriptors handling */
        switch(p_dr->i_tag)
        {
        case 0x1d: /* We have found an IOD descriptor */
            msg_Dbg( p_demux, " * PMT descriptor : IOD (0x1d)" );
            prg->iod = IODNew( p_dr->i_length, p_dr->p_data );
            break;

        case 0x9:
            msg_Dbg( p_demux, " * PMT descriptor : CA (0x9) SysID 0x%x",
                    (p_dr->p_data[0] << 8) | p_dr->p_data[1] );
            break;

        case 0x5: /* Registration Descriptor */
            if( p_dr->i_length != 4 )
            {
                msg_Warn( p_demux, " * PMT invalid Registration Descriptor" );
            }
            else
            {
                msg_Dbg( p_demux, " * PMT descriptor : registration %4.4s", p_dr->p_data );
                if( !memcmp( p_dr->p_data, "HDMV", 4 ) || !memcmp( p_dr->p_data, "HDPR", 4 ) )
                    registration_type = TS_PMT_REGISTRATION_HDMV; /* Blu-Ray */
            }
            break;

        case 0x0f:
            msg_Dbg( p_demux, " * PMT descriptor : Private Data (0x0f)" );
            break;

        case 0xC1:
            msg_Dbg( p_demux, " * PMT descriptor : Digital copy control (0xC1)" );
            break;

        case 0x88: /* EACEM Simulcast HD Logical channels ordering */
            msg_Dbg( p_demux, " * descriptor : EACEM Simulcast HD" );
            /* TODO: apply visibility flags */
            break;

        default:
            msg_Dbg( p_demux, " * PMT descriptor : unknown (0x%x)", p_dr->i_tag );
        }
    }
    dvbpsi_pmt_es_t      *p_es;
    for( p_es = p_pmt->p_first_es; p_es != NULL; p_es = p_es->p_next )
    {
        ts_pid_t tmp_pid, *old_pid = 0, *pid = &tmp_pid;

        /* Find out if the PID was already declared */
        for( int i = 0; i < i_clean; i++ )
        {
            if( pp_clean[i] == &p_sys->pid[p_es->i_pid] )
            {
                old_pid = pp_clean[i];
                break;
            }
        }
        ValidateDVBMeta( p_demux, p_es->i_pid );

        if( !old_pid && p_sys->pid[p_es->i_pid].b_valid )
        {
            msg_Warn( p_demux, " * PMT error: pid=%d already defined",
                      p_es->i_pid );
            continue;
        }

        char const * psz_typedesc = "";
        switch(p_es->i_type)
        {
        case 0x00:
            psz_typedesc = "ISO/IEC Reserved";
            break;
        case 0x01:
            psz_typedesc = "ISO/IEC 11172 Video";
            break;
        case 0x02:
            psz_typedesc = "ISO/IEC 13818-2 Video or ISO/IEC 11172-2 constrained parameter video stream";
            break;
        case 0x03:
            psz_typedesc = "ISO/IEC 11172 Audio";
            break;
        case 0x04:
            psz_typedesc = "ISO/IEC 13818-3 Audio";
            break;
        case 0x05:
            psz_typedesc = "ISO/IEC 13818-1 private_sections";
            break;
        case 0x06:
            psz_typedesc = "ISO/IEC 13818-1 PES packets containing private data";
            break;
        case 0x07:
            psz_typedesc = "ISO/IEC 13522 MHEG";
            break;
        case 0x08:
            psz_typedesc = "ISO/IEC 13818-1 Annex A DSM CC";
            break;
        case 0x09:
            psz_typedesc = "ITU-T Rec. H.222.1";
            break;
        case 0x0A:
            psz_typedesc = "ISO/IEC 13818-6 type A";
            break;
        case 0x0B:
            psz_typedesc = "ISO/IEC 13818-6 type B";
            break;
        case 0x0C:
            psz_typedesc = "ISO/IEC 13818-6 type C";
            break;
        case 0x0D:
            psz_typedesc = "ISO/IEC 13818-6 type D";
            break;
        case 0x0E:
            psz_typedesc = "ISO/IEC 13818-1 auxiliary";
            break;
        default:
            if (p_es->i_type >= 0x0F && p_es->i_type <=0x7F)
                psz_typedesc = "ISO/IEC 13818-1 Reserved";
            else
                psz_typedesc = "User Private";
        }

        msg_Dbg( p_demux, "  * pid=%d type=0x%x %s",
                 p_es->i_pid, p_es->i_type, psz_typedesc );

        for( p_dr = p_es->p_first_descriptor; p_dr != NULL;
             p_dr = p_dr->p_next )
        {
            msg_Dbg( p_demux, "    - descriptor tag 0x%x",
                     p_dr->i_tag );
        }

        PIDInit( pid, false, pmt->psi );
        PIDFillFormat( &pid->es->fmt, p_es->i_type );
        pid->i_owner_number = prg->i_number;
        pid->i_pid          = p_es->i_pid;
        pid->b_seen         = p_sys->pid[p_es->i_pid].b_seen;


        bool b_registration_applied = false;
        if ( p_es->i_type >= 0x80 ) /* non standard, extensions */
        {
            if ( registration_type == TS_PMT_REGISTRATION_HDMV )
            {
                if (( b_registration_applied = PMTSetupEsHDMV( p_demux, pid, p_es ) ))
                    msg_Dbg( p_demux, "    + HDMV registration applied to pid %d type 0x%x",
                             p_es->i_pid, p_es->i_type );
            }
            else
            {
                if (( b_registration_applied = PMTSetupEsRegistration( p_demux, pid, p_es ) ))
                    msg_Dbg( p_demux, "    + registration applied to pid %d type 0x%x",
                        p_es->i_pid, p_es->i_type );
            }
        }

        if ( !b_registration_applied )
        {
            switch( p_es->i_type )
            {
            case 0x06:
                /* Handle PES private data */
                PMTSetupEs0x06( p_demux, pid, p_es );
                break;
            /* All other private or reserved types */
            case 0x0f:
            case 0x10:
            case 0x11:
            case 0x12:
                PMTSetupEsISO14496( p_demux, pid, prg, p_es );
                break;
            case 0x83:
                /* LPCM (audio) */
                PMTSetupEs0x83( p_pmt, pid );
                break;
            case 0xa0:
                PMTSetupEs0xA0( p_demux, pid, p_es );
                break;
            case 0xd1:
                PMTSetupEs0xD1( p_demux, pid, p_es );
                break;
            case 0xEA:
                PMTSetupEs0xEA( p_demux, pid, p_es );
            default:
                break;
            }
        }

        if( pid->es->fmt.i_cat == AUDIO_ES ||
            ( pid->es->fmt.i_cat == SPU_ES &&
              pid->es->fmt.i_codec != VLC_CODEC_DVBS &&
              pid->es->fmt.i_codec != VLC_CODEC_TELETEXT ) )
        {
            PMTParseEsIso639( p_demux, pid, p_es );
        }

        switch( pid->es->fmt.i_codec )
        {
        case VLC_CODEC_SCTE_27:
            pid->es->data_type = TS_ES_DATA_TABLE_SECTION;
            break;
        default:
            //pid->es->data_type = TS_ES_DATA_PES;
            break;
        }

        pid->es->fmt.i_group = p_pmt->i_program_number;
        for( int i = 0; i < pid->i_extra_es; i++ )
            pid->extra_es[i]->fmt.i_group = p_pmt->i_program_number;

        if( pid->es->fmt.i_cat == UNKNOWN_ES )
        {
            msg_Dbg( p_demux, "   => pid %d content is *unknown*",
                     p_es->i_pid );
        }
        else
        {
            msg_Dbg( p_demux, "   => pid %d has now es fcc=%4.4s",
                     p_es->i_pid, (char*)&pid->es->fmt.i_codec );

            if( p_sys->b_es_id_pid ) pid->es->fmt.i_id = p_es->i_pid;

            /* Check if we can avoid restarting the ES */
            if( old_pid &&
                pid->es->fmt.i_codec == old_pid->es->fmt.i_codec &&
                pid->es->fmt.i_extra == old_pid->es->fmt.i_extra &&
                pid->es->fmt.i_extra == 0 &&
                pid->i_extra_es == old_pid->i_extra_es &&
                ( ( !pid->es->fmt.psz_language &&
                    !old_pid->es->fmt.psz_language ) ||
                  ( pid->es->fmt.psz_language &&
                    old_pid->es->fmt.psz_language &&
                    !strcmp( pid->es->fmt.psz_language,
                             old_pid->es->fmt.psz_language ) ) ) )
            {
                pid->i_cc = old_pid->i_cc;
                ts_es_t *e = pid->es;
                pid->es = old_pid->es;
                old_pid->es = e;
                for( int i = 0; i < pid->i_extra_es; i++ )
                {
                    e = pid->extra_es[i];
                    pid->extra_es[i] = old_pid->extra_es[i];
                    old_pid->extra_es[i] = e;
                }
            }
            else
            {
                pid->es->id = es_out_Add( p_demux->out, &pid->es->fmt );
                for( int i = 0; i < pid->i_extra_es; i++ )
                {
                    pid->extra_es[i]->id =
                        es_out_Add( p_demux->out, &pid->extra_es[i]->fmt );
                }
                p_sys->i_pmt_es += 1 + pid->i_extra_es;
            }
        }

        /* Add ES to the list */
        if( old_pid )
        {
            PIDClean( p_demux, old_pid );
            TAB_REMOVE( i_clean, pp_clean, old_pid );
        }
        p_sys->pid[p_es->i_pid] = *pid;

        p_dr = PMTEsFindDescriptor( p_es, 0x09 );
        if( p_dr && p_dr->i_length >= 2 )
        {
            msg_Dbg( p_demux, "   * PMT descriptor : CA (0x9) SysID 0x%x",
                     (p_dr->p_data[0] << 8) | p_dr->p_data[1] );
        }

        if( ProgramIsSelected( p_demux, prg->i_number ) && pid->es->id != NULL )
            SetPIDFilter( p_demux, p_es->i_pid, true ); /* Set demux filter */
    }

    /* Set CAM descrambling */
    if( !ProgramIsSelected( p_demux, prg->i_number )
     || stream_Control( p_demux->s, STREAM_SET_PRIVATE_ID_CA,
                        p_pmt ) != VLC_SUCCESS )
        dvbpsi_DeletePMT( p_pmt );

    for( int i = 0; i < i_clean; i++ )
    {
        if( ProgramIsSelected( p_demux, prg->i_number ) )
            SetPIDFilter( p_demux, pp_clean[i]->i_pid, false );

        PIDClean( p_demux, pp_clean[i] );
    }
    if( i_clean )
        free( pp_clean );
}

static void PATCallBack( void *data, dvbpsi_pat_t *p_pat )
{
    demux_t              *p_demux = data;
    demux_sys_t          *p_sys = p_demux->p_sys;
    dvbpsi_pat_program_t *p_program;
    ts_pid_t             *pat = &p_sys->pid[0];

    msg_Dbg( p_demux, "PATCallBack called" );

    if( ( pat->psi->i_pat_version != -1 &&
            ( !p_pat->b_current_next ||
              p_pat->i_version == pat->psi->i_pat_version ) ) ||
        p_sys->b_user_pmt )
    {
        dvbpsi_DeletePAT( p_pat );
        return;
    }

    msg_Dbg( p_demux, "new PAT ts_id=%d version=%d current_next=%d",
             p_pat->i_ts_id, p_pat->i_version, p_pat->b_current_next );

    /* Clean old */
    if( p_sys->i_pmt > 0 )
    {
        int      i_pmt_rm = 0;
        ts_pid_t **pmt_rm = NULL;

        /* Search pmt to be deleted */
        for( int i = 0; i < p_sys->i_pmt; i++ )
        {
            ts_pid_t *pmt = p_sys->pmt[i];
            bool b_keep = false;

            for( p_program = p_pat->p_first_program; !b_keep && p_program;
                 p_program = p_program->p_next )
            {
                if( p_program->i_pid != pmt->i_pid )
                    continue;

                for( int i_prg = 0; !b_keep && i_prg < pmt->psi->i_prg; i_prg++ )
                    if( p_program->i_number == pmt->psi->prg[i_prg]->i_number )
                        b_keep = true;
            }

            if( b_keep )
                continue;

            TAB_APPEND( i_pmt_rm, pmt_rm, pmt );
        }

        /* Delete all ES attached to thoses PMT */
        for( int i = 2; i < 8192; i++ )
        {
            ts_pid_t *pid = &p_sys->pid[i];

            if( !pid->b_valid || pid->psi )
                continue;

            for( int j = 0; j < i_pmt_rm && pid->b_valid; j++ )
            {
                for( int i_prg = 0; i_prg < pid->p_owner->i_prg; i_prg++ )
                {
                    /* We only remove es that aren't defined by extra pmt */
                    if( pid->p_owner->prg[i_prg]->i_pid_pmt != pmt_rm[j]->i_pid )
                        continue;

                    if( pid->es->id )
                        SetPIDFilter( p_demux, i, false );

                    PIDClean( p_demux, pid );
                    break;
                }
            }
        }

        /* Delete PMT pid */
        for( int i = 0; i < i_pmt_rm; i++ )
        {
            ts_pid_t *pid = pmt_rm[i];
            SetPIDFilter( p_demux, pid->i_pid, false );

            for( int i_prg = 0; i_prg < pid->psi->i_prg; i_prg++ )
            {
                const int i_number = pid->psi->prg[i_prg]->i_number;
                es_out_Control( p_demux->out, ES_OUT_DEL_GROUP, i_number );
            }

            PIDClean( p_demux, &p_sys->pid[pid->i_pid] );
            TAB_REMOVE( p_sys->i_pmt, p_sys->pmt, pid );
        }

        free( pmt_rm );
    }

    /* now create programs */
    for( p_program = p_pat->p_first_program; p_program != NULL;
         p_program = p_program->p_next )
    {
        msg_Dbg( p_demux, "  * number=%d pid=%d", p_program->i_number,
                 p_program->i_pid );
        if( p_program->i_number == 0 )
            continue;

        ts_pid_t *pmt = &p_sys->pid[p_program->i_pid];

        ValidateDVBMeta( p_demux, p_program->i_pid );

        if( pmt->b_valid )
        {
            bool b_add = true;
            for( int i_prg = 0; b_add && i_prg < pmt->psi->i_prg; i_prg++ )
                if( pmt->psi->prg[i_prg]->i_number == p_program->i_number )
                    b_add = false;

            if( !b_add )
                continue;
        }
        else
        {
            TAB_APPEND( p_sys->i_pmt, p_sys->pmt, pmt );
        }

        PIDInit( pmt, true, pat->psi );
        ts_prg_psi_t *prg = pmt->psi->prg[pmt->psi->i_prg-1];
#if (DVBPSI_VERSION_INT >= DVBPSI_VERSION_WANTED(1,0,0))
        prg->handle = dvbpsi_new( &dvbpsi_messages, DVBPSI_MSG_DEBUG );
        if( !prg->handle )
        {
            dvbpsi_DeletePAT( p_pat );
            return;
        }
        prg->handle->p_sys = (void *) VLC_OBJECT(p_demux);
        if( !dvbpsi_pmt_attach( prg->handle, p_program->i_number, PMTCallBack, p_demux ) )
            msg_Err( p_demux, "PATCallback failed attaching PMTCallback to program %d",
                     p_program->i_number );
#else
        prg->handle = dvbpsi_AttachPMT( p_program->i_number, PMTCallBack, p_demux );
#endif
        prg->i_number = p_program->i_number;
        prg->i_pid_pmt = p_program->i_pid;

        /* Now select PID at access level */
        if( ProgramIsSelected( p_demux, p_program->i_number ) )
        {
            if( p_sys->i_current_program == 0 )
                p_sys->i_current_program = p_program->i_number;

            if( SetPIDFilter( p_demux, p_program->i_pid, true ) )
                p_sys->b_access_control = false;
        }
    }
    pat->psi->i_pat_version = p_pat->i_version;

    dvbpsi_DeletePAT( p_pat );
}
