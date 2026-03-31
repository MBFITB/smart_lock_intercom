/*
 * camera_win.c — Windows test camera: color bar pattern + OpenH264 encoder
 *
 * Generates YUV420 test pattern and encodes to H.264 using OpenH264.
 * On the real BK7258 target, this file is replaced by camera_bk7258.c
 * which receives H.264 NAL units directly from CV1812C via UVC.
 */
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <wels/codec_api.h>
#include "camera.h"

/* Max NAL units per frame (SPS + PPS + slices) */
#define MAX_NALUS  32

static struct {
	ISVCEncoder *enc;
	int          width;
	int          height;
	int          fps;

	/* YUV420 buffer for test pattern */
	uint8_t     *yuv_buf;
	size_t       yuv_size;

	/* Encoded output */
	SFrameBSInfo bs_info;
	struct camera_nalu nalus[MAX_NALUS];
	struct camera_frame frame;

	/* Cached SPS / PPS for SDP */
	uint8_t      sps[64];
	size_t       sps_len;
	uint8_t      pps[64];
	size_t       pps_len;
	bool         has_params;

	uint32_t     frame_idx;
	bool         force_idr;
	bool         opened;
} g_cam;


/*
 * Generate YUV420 color bars with a moving white line
 */
static void generate_test_pattern(uint8_t *yuv, int w, int h,
				  uint32_t frame_idx)
{
	uint8_t *y_plane = yuv;
	uint8_t *u_plane = yuv + w * h;
	uint8_t *v_plane = u_plane + (w * h / 4);

	/* 8 vertical color bars */
	static const uint8_t bars_y[] = {235,162,131,112,84, 65, 35, 16};
	static const uint8_t bars_u[] = {128, 44,156, 72,184,100,212,128};
	static const uint8_t bars_v[] = {128,142, 44,158, 98,214, 72,128};

	/* Y plane */
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int bar = x * 8 / w;
			y_plane[y * w + x] = bars_y[bar];
		}
	}

	/* U and V planes (half resolution) */
	int hw = w / 2, hh = h / 2;
	for (int y = 0; y < hh; y++) {
		for (int x = 0; x < hw; x++) {
			int bar = x * 8 / hw;
			u_plane[y * hw + x] = bars_u[bar];
			v_plane[y * hw + x] = bars_v[bar];
		}
	}

	/* Moving white line */
	int line_y = (frame_idx * 4) % (unsigned)h;
	memset(y_plane + line_y * w, 235, (size_t)w);
}


int camera_open(int width, int height, int fps, int bitrate_kbps)
{
	SEncParamBase param;
	int rv;

	if (g_cam.opened)
		return EALREADY;

	memset(&g_cam, 0, sizeof(g_cam));

	/* Create OpenH264 encoder */
	rv = WelsCreateSVCEncoder(&g_cam.enc);
	if (rv != 0 || !g_cam.enc)
		return EINVAL;

	/* Basic encoder parameters */
	memset(&param, 0, sizeof(param));
	param.iUsageType     = CAMERA_VIDEO_REAL_TIME;
	param.iPicWidth      = width;
	param.iPicHeight     = height;
	param.iTargetBitrate = bitrate_kbps * 1000;
	param.iRCMode        = RC_BITRATE_MODE;
	param.fMaxFrameRate  = (float)fps;

	rv = (*g_cam.enc)->Initialize(g_cam.enc, &param);
	if (rv != 0) {
		WelsDestroySVCEncoder(g_cam.enc);
		g_cam.enc = NULL;
		return EINVAL;
	}

	/* Set IDR interval (keyframe every 2 seconds) */
	int idr_interval = fps * 2;
	rv = (*g_cam.enc)->SetOption(g_cam.enc,
				    ENCODER_OPTION_IDR_INTERVAL,
				    &idr_interval);
	if (rv != 0) {
		/* Non-fatal: use default IDR interval */
	}

	/* Set video format to I420 */
	int video_fmt = videoFormatI420;
	(*g_cam.enc)->SetOption(g_cam.enc, ENCODER_OPTION_DATAFORMAT,
				&video_fmt);

	/* Allocate YUV420 buffer */
	g_cam.yuv_size = (size_t)(width * height * 3 / 2);
	g_cam.yuv_buf = malloc(g_cam.yuv_size);
	if (!g_cam.yuv_buf) {
		(*g_cam.enc)->Uninitialize(g_cam.enc);
		WelsDestroySVCEncoder(g_cam.enc);
		g_cam.enc = NULL;
		return ENOMEM;
	}

	g_cam.width  = width;
	g_cam.height = height;
	g_cam.fps    = fps;
	g_cam.opened = true;

	return 0;
}


void camera_close(void)
{
	if (!g_cam.opened)
		return;

	if (g_cam.enc) {
		(*g_cam.enc)->Uninitialize(g_cam.enc);
		WelsDestroySVCEncoder(g_cam.enc);
	}

	free(g_cam.yuv_buf);
	memset(&g_cam, 0, sizeof(g_cam));
}


int camera_get_frame(struct camera_frame *frame)
{
	SSourcePicture pic;
	int rv, nalu_idx = 0;

	if (!g_cam.opened || !frame)
		return EINVAL;

	/* Force IDR if requested */
	if (g_cam.force_idr) {
		(*g_cam.enc)->ForceIntraFrame(g_cam.enc, true);
		g_cam.force_idr = false;
	}

	/* Generate test pattern */
	generate_test_pattern(g_cam.yuv_buf, g_cam.width, g_cam.height,
			      g_cam.frame_idx);

	/* Set up source picture */
	memset(&pic, 0, sizeof(pic));
	pic.iColorFormat = videoFormatI420;
	pic.iPicWidth    = g_cam.width;
	pic.iPicHeight   = g_cam.height;
	pic.iStride[0]   = g_cam.width;
	pic.iStride[1]   = g_cam.width / 2;
	pic.iStride[2]   = g_cam.width / 2;
	pic.pData[0]     = g_cam.yuv_buf;
	pic.pData[1]     = g_cam.yuv_buf + g_cam.width * g_cam.height;
	pic.pData[2]     = pic.pData[1] + (g_cam.width * g_cam.height / 4);

	/* Encode */
	memset(&g_cam.bs_info, 0, sizeof(g_cam.bs_info));
	rv = (*g_cam.enc)->EncodeFrame(g_cam.enc, &pic, &g_cam.bs_info);
	if (rv != 0)
		return EIO;

	/* Skip frames (encoder may drop) */
	if (g_cam.bs_info.eFrameType == videoFrameTypeSkip)
		return EAGAIN;

	/* Extract NAL units from all layers */
	for (int i = 0; i < g_cam.bs_info.iLayerNum; i++) {
		SLayerBSInfo *layer = &g_cam.bs_info.sLayerInfo[i];
		unsigned char *buf = layer->pBsBuf;

		for (int j = 0; j < layer->iNalCount; j++) {
			int nal_len = layer->pNalLengthInByte[j];
			if (nal_len <= 0 || nalu_idx >= MAX_NALUS)
				continue;

			/*
			 * OpenH264 outputs NALs with 4-byte Annex B start codes
			 * (0x00 0x00 0x00 0x01). Strip them for RTP.
			 */
			const uint8_t *p = buf;
			int skip = 0;

			if (nal_len >= 4 &&
			    p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
				skip = 4;
			} else if (nal_len >= 3 &&
				   p[0] == 0 && p[1] == 0 && p[2] == 1) {
				skip = 3;
			}

			g_cam.nalus[nalu_idx].data = p + skip;
			g_cam.nalus[nalu_idx].len  = (size_t)(nal_len - skip);

			/* Cache SPS (NAL type 7) and PPS (NAL type 8) */
			if (!g_cam.has_params && nal_len - skip > 0) {
				uint8_t nal_type = p[skip] & 0x1F;
				size_t nlen = (size_t)(nal_len - skip);

				if (nal_type == 7 && nlen <= sizeof(g_cam.sps)) {
					memcpy(g_cam.sps, p + skip, nlen);
					g_cam.sps_len = nlen;
				}
				else if (nal_type == 8 && nlen <= sizeof(g_cam.pps)) {
					memcpy(g_cam.pps, p + skip, nlen);
					g_cam.pps_len = nlen;
				}

				if (g_cam.sps_len > 0 && g_cam.pps_len > 0)
					g_cam.has_params = true;
			}

			nalu_idx++;

			buf += nal_len;
		}
	}

	if (nalu_idx == 0)
		return EAGAIN;

	g_cam.frame.nalus       = g_cam.nalus;
	g_cam.frame.count       = nalu_idx;
	g_cam.frame.is_keyframe =
		(g_cam.bs_info.eFrameType == videoFrameTypeIDR);

	*frame = g_cam.frame;
	g_cam.frame_idx++;

	return 0;
}


void camera_request_keyframe(void)
{
	g_cam.force_idr = true;
}


int camera_get_sps_pps(const uint8_t **sps, size_t *sps_len,
                       const uint8_t **pps, size_t *pps_len)
{
	if (!g_cam.has_params)
		return EAGAIN;

	*sps = g_cam.sps;
	*sps_len = g_cam.sps_len;
	*pps = g_cam.pps;
	*pps_len = g_cam.pps_len;
	return 0;
}
