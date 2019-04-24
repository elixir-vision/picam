#ifndef PICAM_CAMERA_H
#define PICAM_CAMERA_H

#define CAMERA_PORT_PREVIEW 0
#define CAMERA_PORT_VIDEO   1
#define CAMERA_PORT_STILL   2

void picam_camera_init(MMAL_COMPONENT_T *camera, uint32_t max_width, uint32_t max_height);
void picam_camera_configure_format(MMAL_COMPONENT_T *camera, uint32_t width, uint32_t height, uint32_t fps256);

#endif
