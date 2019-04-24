#ifndef PICAM_PREVIEW_H
#define PICAM_PREVIEW_H

typedef struct
{
    MMAL_COMPONENT_T *component;
    MMAL_CONNECTION_T *connection;
    MMAL_BOOL_T enable;
    MMAL_BOOL_T fullscreen;
    MMAL_RECT_T dest_rect;
    uint8_t opacity;
    uint8_t layer;
} PREVIEW_CONFIG_T;


void picam_preview_set_defaults(PREVIEW_CONFIG_T *config);
void picam_preview_init(PREVIEW_CONFIG_T *config);
void picam_preview_configure(PREVIEW_CONFIG_T *config);

#endif

