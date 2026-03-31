#ifndef RTSP_SERVER_H
#define RTSP_SERVER_H

#include <re.h>

/*
 * Lightweight RTSP server for intercom streaming
 *
 * Supports single-client RTSP session:
 *   DESCRIBE  → SDP with H.264 + G.711 media
 *   SETUP     → allocate RTP UDP ports (server→client)
 *   PLAY      → start sending RTP
 *   TEARDOWN  → stop sending, release resources
 *
 * Designed for go2rtc bridge: RTSP → WebRTC conversion
 */

int rtsp_server_init(const struct sa *laddr);
void rtsp_server_close(void);

#endif /* RTSP_SERVER_H */
