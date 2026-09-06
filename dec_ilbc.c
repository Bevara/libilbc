/*
 *			GPAC - Multimedia Framework C SDK
 *
 *  This file is part of GPAC / iLBC decoder filter, based on libilbc, which
 *  packages WebRTC's fixed-point iLBC as a standalone library.
 *
 *  iLBC (RFC 3951) is a narrowband speech codec built for packet loss: every
 *  frame decodes on its own, so a lost one costs exactly one frame. It is
 *  always 8 kHz mono, and comes in two modes - 20 ms frames of 38 bytes, or
 *  30 ms frames of 50 bytes.
 *
 *  The file format is the one the reference implementation writes and ffmpeg
 *  reads: a nine-byte header, "#!iLBC20\n" or "#!iLBC30\n", then one packed
 *  frame after another. The mode therefore comes from the header rather than
 *  being guessed, and a file that does not carry one is refused: 38 and 50
 *  bytes divide too many stream lengths for a guess to be safe.
 */

#include <gpac/filters.h>
#include <gpac/constants.h>
#include <string.h>
#include <stdlib.h>

#include <ilbc.h>

#define ILBC_RATE          8000
#define ILBC_HEADER_SIZE   9
#define ILBC_20MS_BYTES    38
#define ILBC_20MS_SAMPLES  160
#define ILBC_30MS_BYTES    50
#define ILBC_30MS_SAMPLES  240

typedef struct
{
	GF_FilterPid *ipid, *opid;
} GF_ILBCDecCtx;

static GF_Err ilbcdec_configure_pid(GF_Filter *filter, GF_FilterPid *pid, Bool is_remove)
{
	GF_ILBCDecCtx *ctx = (GF_ILBCDecCtx *)gf_filter_get_udta(filter);

	if (is_remove)
	{
		if (ctx->opid)
		{
			gf_filter_pid_remove(ctx->opid);
			ctx->opid = NULL;
		}
		ctx->ipid = NULL;
		return GF_OK;
	}
	if (!gf_filter_pid_check_caps(pid))
		return GF_NOT_SUPPORTED;

	ctx->ipid = pid;
	gf_filter_pid_set_framing_mode(pid, GF_TRUE);

	if (!ctx->opid)
		ctx->opid = gf_filter_pid_new(filter);

	/* Everything about the output is fixed by the codec: 8 kHz mono, 16-bit. */
	gf_filter_pid_copy_properties(ctx->opid, pid);
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_STREAM_TYPE, &PROP_UINT(GF_STREAM_AUDIO));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CODECID, &PROP_UINT(GF_CODECID_RAW));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_AUDIO_FORMAT, &PROP_UINT(GF_AUDIO_FMT_S16));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_SAMPLE_RATE, &PROP_UINT(ILBC_RATE));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_TIMESCALE, &PROP_UINT(ILBC_RATE));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_NUM_CHANNELS, &PROP_UINT(1));
	gf_filter_pid_set_property(ctx->opid, GF_PROP_PID_CHANNEL_LAYOUT, &PROP_LONGUINT(GF_AUDIO_CH_FRONT_CENTER));

	return GF_OK;
}

static GF_Err ilbcdec_process(GF_Filter *filter)
{
	GF_FilterPacket *pck, *dst_pck;
	const u8 *data;
	u8 *output;
	u32 size, pos, nb_frames, i;
	s16 mode, frame_bytes, frame_samples;
	IlbcDecoderInstance *dec = NULL;
	GF_ILBCDecCtx *ctx = (GF_ILBCDecCtx *)gf_filter_get_udta(filter);

	pck = gf_filter_pid_get_packet(ctx->ipid);
	if (!pck)
	{
		if (gf_filter_pid_is_eos(ctx->ipid))
		{
			gf_filter_pid_set_eos(ctx->opid);
			return GF_EOS;
		}
		return GF_OK;
	}
	data = gf_filter_pck_get_data(pck, &size);
	if (!data || (size < ILBC_HEADER_SIZE + ILBC_20MS_BYTES))
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[iLBCDec] File too short to hold a header and a frame\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if (!memcmp(data, "#!iLBC20\n", ILBC_HEADER_SIZE))
	{
		mode = 20;
		frame_bytes = ILBC_20MS_BYTES;
		frame_samples = ILBC_20MS_SAMPLES;
	}
	else if (!memcmp(data, "#!iLBC30\n", ILBC_HEADER_SIZE))
	{
		mode = 30;
		frame_bytes = ILBC_30MS_BYTES;
		frame_samples = ILBC_30MS_SAMPLES;
	}
	else
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[iLBCDec] Not an iLBC file: no \"#!iLBC20\" or \"#!iLBC30\" header, and nothing else in the stream says which of the two modes it uses\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	pos = ILBC_HEADER_SIZE;
	nb_frames = (size - pos) / (u32)frame_bytes;
	if (!nb_frames)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[iLBCDec] No complete frame after the header\n"));
		return GF_NON_COMPLIANT_BITSTREAM;
	}

	if (WebRtcIlbcfix_DecoderCreate(&dec) < 0)
	{
		gf_filter_pid_drop_packet(ctx->ipid);
		GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[iLBCDec] Could not create the decoder\n"));
		return GF_OUT_OF_MEM;
	}
	WebRtcIlbcfix_DecoderInit(dec, mode);

	dst_pck = gf_filter_pck_new_alloc(ctx->opid, nb_frames * (u32)frame_samples * 2, &output);
	if (!dst_pck)
	{
		WebRtcIlbcfix_DecoderFree(dec);
		gf_filter_pid_drop_packet(ctx->ipid);
		return GF_OUT_OF_MEM;
	}

	for (i = 0; i < nb_frames; i++)
	{
		s16 speech_type = 0;
		int got = WebRtcIlbcfix_Decode(dec, data + pos + (size_t)i * frame_bytes, frame_bytes,
		                               (s16 *)(output + (size_t)i * frame_samples * 2), &speech_type);
		if (got != frame_samples)
		{
			/* A frame that does not decode is not skipped silently: iLBC is
			 * predictive within a frame only, so one bad frame means the file
			 * is not what its header says. */
			WebRtcIlbcfix_DecoderFree(dec);
			gf_filter_pck_discard(dst_pck);
			gf_filter_pid_drop_packet(ctx->ipid);
			GF_LOG(GF_LOG_ERROR, GF_LOG_CODEC, ("[iLBCDec] Frame %u of %u did not decode (%d samples, expected %d)\n",
			                                    i, nb_frames, got, frame_samples));
			return GF_NON_COMPLIANT_BITSTREAM;
		}
	}
	WebRtcIlbcfix_DecoderFree(dec);

	gf_filter_pck_set_cts(dst_pck, 0);
	gf_filter_pck_set_duration(dst_pck, nb_frames * (u32)frame_samples);
	gf_filter_pck_set_sap(dst_pck, GF_FILTER_SAP_1);
	gf_filter_pck_send(dst_pck);

	gf_filter_pid_drop_packet(ctx->ipid);
	gf_filter_pid_set_eos(ctx->opid);
	return GF_EOS;
}

static void ilbcdec_finalize(GF_Filter *filter)
{
}

static const GF_FilterCapability ILBCDecCaps[] =
	{
		CAP_UINT(GF_CAPS_INPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_FILE),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_FILE_EXT, "lbc"),
		CAP_STRING(GF_CAPS_INPUT, GF_PROP_PID_MIME, "audio/ilbc|audio/x-ilbc"),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_STREAM_TYPE, GF_STREAM_AUDIO),
		CAP_UINT(GF_CAPS_OUTPUT, GF_PROP_PID_CODECID, GF_CODECID_RAW),
};

GF_FilterRegister ILBCDecoderRegister = {
	.name = "ilbcdec",
	GF_FS_SET_DESCRIPTION("iLBC (Internet Low Bitrate Codec) decoder")
		GF_FS_SET_HELP("This filter decodes iLBC speech files (.lbc, with the nine-byte header that names the 20 ms or 30 ms mode) using libilbc. Output is always 8 kHz mono.")
			.private_size = sizeof(GF_ILBCDecCtx),
	SETCAPS(ILBCDecCaps),
	.configure_pid = ilbcdec_configure_pid,
	.process = ilbcdec_process,
	.finalize = ilbcdec_finalize,
};

const GF_FilterRegister *EMSCRIPTEN_KEEPALIVE ilbcdec_register(GF_FilterSession *session)
{
	return &ILBCDecoderRegister;
}

#include "filter_register.h"
__attribute__((constructor))
void register_ilbcdec(void) {
    gf_filter_auto_register("ilbcdec", ilbcdec_register);
}
