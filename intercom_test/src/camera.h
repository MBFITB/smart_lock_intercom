#ifndef CAMERA_H
#define CAMERA_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

/*
 * Platform-agnostic camera interface
 *
 * Outputs H.264 NAL units (without Annex B start codes).
 * Each call to camera_get_frame() returns one encoded frame
 * consisting of one or more NAL units.
 *
 * Platform implementations:
 *   - camera_win.c  : Test pattern + OpenH264 software encoder (Windows dev)
 *   - camera_bk7258.c : UVC host receiving H.264 from CV1812C (target HW)
 */

/* Single NAL unit within an encoded frame */
struct camera_nalu {
	const uint8_t *data;   /* NAL unit data (no start code prefix) */
	size_t         len;    /* length in bytes */
};

/* Encoded frame: array of NAL units */
struct camera_frame {
	struct camera_nalu *nalus;  /* array of NAL units */
	int                 count;  /* number of NAL units */
	bool           is_keyframe; /* true if IDR frame */
};

/*
 * Initialize the camera / encoder.
 * Returns 0 on success, errno on failure.
 */
int camera_open(int width, int height, int fps, int bitrate_kbps);

/*
 * Close the camera / encoder, release resources.
 */
void camera_close(void);

/*
 * Get the next encoded frame (pull model).
 * Fills `frame` with pointers to internal buffers — valid until
 * the next call to camera_get_frame() or camera_close().
 *
 * Returns 0 on success, EAGAIN if no new frame available.
 */
int camera_get_frame(struct camera_frame *frame);

/*
 * Request that the next frame be an IDR keyframe.
 */
void camera_request_keyframe(void);

/*
 * Get cached SPS and PPS NAL units (without start codes).
 * Available after the first frame has been encoded (camera_get_frame).
 * Returns 0 on success, EAGAIN if not yet available.
 */
int camera_get_sps_pps(const uint8_t **sps, size_t *sps_len,
                       const uint8_t **pps, size_t *pps_len);

#endif /* CAMERA_H */
