#include <err.h>

#include "bcm_host.h"
#include "interface/vcos/vcos.h"
#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/mmal_parameters_camera.h"

#include "picam_camera.h"

static void camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    // This is called from another thread. Don't access any data here.
    warnx("Processing camera control callback\r\n");
    if (buffer->cmd == MMAL_EVENT_ERROR)
        errx(EXIT_FAILURE, "No data received from sensor. Check all connections, including the Sunny one on the camera board");
    else if(buffer->cmd != MMAL_EVENT_PARAMETER_CHANGED)
        errx(EXIT_FAILURE, "Camera sent invalid data: 0x%08x", buffer->cmd);

    mmal_buffer_header_release(buffer);
}

void picam_camera_init(MMAL_COMPONENT_T *camera, uint32_t max_width, uint32_t max_height)
{
    if (mmal_port_enable(camera->control, camera_control_callback) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable camera control port");

    MMAL_PARAMETER_CAMERA_CONFIG_T cam_config = {
        {MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config)},
        .max_stills_w = 0,
        .max_stills_h = 0,
        .stills_yuv422 = 0,
        .one_shot_stills = 0,
        .max_preview_video_w = max_width,
        .max_preview_video_h = max_height,
        .num_preview_video_frames = 3,
        .stills_capture_circular_buffer_height = 0,
        .fast_preview_resume = 0,
        .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
    };
    if (mmal_port_parameter_set(camera->control, &cam_config.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Error configuring camera");
}

void picam_camera_configure_format(MMAL_COMPONENT_T *camera, uint32_t width, uint32_t height, uint32_t fps256)
{
    MMAL_ES_FORMAT_T *format;

    warnx("Setting preview format\r\n");
    format = camera->output[CAMERA_PORT_PREVIEW]->format;
    format->encoding = MMAL_ENCODING_OPAQUE;
    format->encoding_variant = MMAL_ENCODING_I420;
    format->es->video.width = VCOS_ALIGN_UP(width, 32);
    format->es->video.height = VCOS_ALIGN_UP(height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = width;
    format->es->video.crop.height = height;
    format->es->video.frame_rate.num = fps256;
    format->es->video.frame_rate.den = 256;
    if (mmal_port_format_commit(camera->output[CAMERA_PORT_PREVIEW]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set preview format");

    warnx("Setting video format\r\n");
    format = camera->output[CAMERA_PORT_VIDEO]->format;
    format->encoding = MMAL_ENCODING_I420;
    format->encoding_variant = MMAL_ENCODING_I420;
    format->es->video.width = VCOS_ALIGN_UP(width, 32);
    format->es->video.height = VCOS_ALIGN_UP(height, 16);
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = width;
    format->es->video.crop.height = height;
    format->es->video.frame_rate.num = fps256;
    format->es->video.frame_rate.den = 256;
    if (mmal_port_format_commit(camera->output[CAMERA_PORT_VIDEO]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set video format");

    warnx("Enabling video capture\r\n");
    if (mmal_port_parameter_set_boolean(camera->output[CAMERA_PORT_VIDEO], MMAL_PARAMETER_CAPTURE, 1) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable video capture");
}
