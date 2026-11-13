#pragma once

#include <stdint.h>

#define WX_RADAR_SIZE 360
// DE: the weather image is shown magnified (rings and labels scale with it, km stay right):
// 360 px x WX_ZOOM_PCT % = ~440 px, filling more of the round screen. No extra memory.
#define WX_ZOOM_PCT   122
#define WX_RADAR_CY   232   // screen y of the weather image centre (rings and labels sit around it)
#define WX_PX(v)      ((v) * WX_ZOOM_PCT / 100)   // a size/offset in the unzoomed layout -> on screen
#define WX_RADAR_SOURCE_SIZE 512

void wx_radar_begin(void);
uint16_t *wx_radar_back_buffer(void);                    // network/sim: decode here
void wx_radar_commit(uint32_t frameTime, double lat, double lon);
// who made the next committed frame ("DWD", "RAINVIEWER"); copied over in wx_radar_commit()
void wx_radar_set_source(const char *source);
void wx_radar_source(char *buf, unsigned len);            // the shown frame's source
bool wx_radar_front(const uint16_t **pixels, uint32_t *frameTime,
                    double *lat, double *lon, uint32_t *version);
