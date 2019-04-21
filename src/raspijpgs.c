/**
 * \file raspijpgs.c
 * Command line program to capture and stream MJPEG video on a Raspberry Pi.
 *
 * Description
 *
 * Raspijpgs is a Unix commandline-friendly MJPEG streaming program with parts
 * copied from RaspiMJPEG, RaspiVid, and RaspiStill. It can be run as either
 * a client or server. The server connects to the Pi Camera via the MMAL
 * interface. It can either record video itself or send it to clients. All
 * interprocess communication is done via Unix Domain sockets.
 *
 * For usage and examples, see README.md
 */

#define _GNU_SOURCE // for asprintf()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <memory.h>
#include <math.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <err.h>
#include <ctype.h>
#include <poll.h>
#include <stdbool.h>

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <sys/un.h>
#include <unistd.h>
#include <arpa/inet.h> // for ntohl

#include "bcm_host.h"
#include "interface/vcos/vcos.h"
#include "interface/mmal/mmal.h"
#include "interface/mmal/mmal_buffer.h"
#include "interface/mmal/util/mmal_util.h"
#include "interface/mmal/util/mmal_util_params.h"
#include "interface/mmal/util/mmal_default_components.h"
#include "interface/mmal/util/mmal_connection.h"
#include "interface/mmal/mmal_parameters_camera.h"

#define MAX_DATA_BUFFER_SIZE        262144
#include "picam_camera.h"
#define MAX_REQUEST_BUFFER_SIZE     4096

#define UNUSED(expr) do { (void)(expr); } while (0)

//
// When considering imager features to add, verify that they are supported first.
// See https://www.raspberrypi.org/forums/viewtopic.php?p=1152920&sid=b3a527262eddeb8e00bfcb01dab2036c#p1152920
//

// Environment config keys
#define RASPIJPGS_SIZE              "RASPIJPGS_SIZE"
#define RASPIJPGS_FPS               "RASPIJPGS_FPS"
#define RASPIJPGS_ANNOTATION        "RASPIJPGS_ANNOTATION"
#define RASPIJPGS_ANNO_BACKGROUND   "RASPIJPGS_ANNO_BACKGROUND"
#define RASPIJPGS_SHARPNESS         "RASPIJPGS_SHARPNESS"
#define RASPIJPGS_CONTRAST          "RASPIJPGS_CONTRAST"
#define RASPIJPGS_BRIGHTNESS        "RASPIJPGS_BRIGHTNESS"
#define RASPIJPGS_SATURATION        "RASPIJPGS_SATURATION"
#define RASPIJPGS_ISO               "RASPIJPGS_ISO"
#define RASPIJPGS_VSTAB             "RASPIJPGS_VSTAB"
#define RASPIJPGS_EV                "RASPIJPGS_EV"
#define RASPIJPGS_EXPOSURE          "RASPIJPGS_EXPOSURE"
#define RASPIJPGS_AWB               "RASPIJPGS_AWB"
#define RASPIJPGS_IMXFX             "RASPIJPGS_IMXFX"
#define RASPIJPGS_COLFX             "RASPIJPGS_COLFX"
#define RASPIJPGS_SENSOR_MODE       "RASPIJPGS_SENSOR_MODE"
#define RASPIJPGS_METERING          "RASPIJPGS_METERING"
#define RASPIJPGS_ROTATION          "RASPIJPGS_ROTATION"
#define RASPIJPGS_HFLIP             "RASPIJPGS_HFLIP"
#define RASPIJPGS_VFLIP             "RASPIJPGS_VFLIP"
#define RASPIJPGS_ROI               "RASPIJPGS_ROI"
#define RASPIJPGS_SHUTTER           "RASPIJPGS_SHUTTER"
#define RASPIJPGS_QUALITY           "RASPIJPGS_QUALITY"
#define RASPIJPGS_RESTART_INTERVAL  "RASPIJPGS_RESTART_INTERVAL"

#define MMAL_COMPONENT_DEFAULT_NULL_SINK "vc.null_sink"
// Globals

struct raspijpgs_state
{
    // Sensor
    MMAL_PARAMETER_CAMERA_INFO_T sensor_info;

    // Current settings
    int width;
    int height;

    // Communication
    char *socket_buffer;
    int socket_buffer_ix[2];
    char *stdin_buffer;
    int stdin_buffer_ix;

    // MMAL resources
    // camera
    //   [0] -> con_camera_null -> null_sink (TODO)
    //   [1] -> con_camera_splitter -> splitter
    //     [0] -> con_splitter_renderer -> renderer
    //     [1] -> con_splitter_jpeg -> jpegencoder
    //     [2] -> con_splitter_resizer-> resizer
    //       [0] -> con_resizer_alt_encoder -> alt_encoder
    MMAL_COMPONENT_T *camera;
    MMAL_COMPONENT_T *jpegencoder;
    MMAL_COMPONENT_T *null_sink;
    MMAL_COMPONENT_T *renderer;
    MMAL_COMPONENT_T *resizer;
    MMAL_COMPONENT_T *splitter;
    MMAL_COMPONENT_T *alt_encoder;

    MMAL_CONNECTION_T *con_camera_null_sink;
    MMAL_CONNECTION_T *con_camera_splitter;
    MMAL_CONNECTION_T *con_resizer_alt_encoder;
    MMAL_CONNECTION_T *con_splitter_jpeg;
    MMAL_CONNECTION_T *con_splitter_renderer;
    MMAL_CONNECTION_T *con_splitter_resizer;

    MMAL_POOL_T *pool_jpegencoder;
    MMAL_POOL_T *pool_alt_encoder;

    // MMAL callback -> main loop (camera control, jpegencoder, alt_encoder)
    int mmal_callback_pipe[2];
};

static struct raspijpgs_state state;

struct raspi_config_opt
{
    const char *long_option;
    const char *short_option;
    const char *env_key;
    const char *help;

    const char *default_value;

    // Record the value (called as options are set)
    // Set replace=0 to only set the value if it hasn't been set already.
    void (*set)(const struct raspi_config_opt *, const char *value, bool fail_on_error);

    // Apply the option (called on every option)
    void (*apply)(const struct raspi_config_opt *, bool fail_on_error);
};
static struct raspi_config_opt opts[];

struct mmal_encoder_callback_msg
{
    MMAL_PORT_T *port;
    MMAL_BUFFER_HEADER_T *buffer;
    uint8_t channel;
};

static void stop_all();
static void start_all();

static void default_set(const struct raspi_config_opt *opt, const char *value, bool fail_on_error)
{
    if (!opt->env_key)
        return;

    if (value) {
        if (setenv(opt->env_key, value, 1 /*replace*/) < 0)
            err(EXIT_FAILURE, "Error setting %s to %s", opt->env_key, opt->default_value);
    } else {
        if (unsetenv(opt->env_key) < 0)
            err(EXIT_FAILURE, "Error unsetting %s", opt->env_key);
    }
}

static int constrain(int minimum, int value, int maximum)
{
    if (value < minimum)
        return minimum;
    else if (value > maximum)
        return maximum;
    else
        return value;
}

static float constrainf(float minimum, float value, float maximum)
{
    if (value < minimum)
        return minimum;
    else if (value > maximum)
        return maximum;
    else
        return value;
}

static void parse_requested_dimensions(int *width, int *height)
{
    // Find out the max dimensions for calculations below.
    // Only the first imager is currently supported.

    int imager_width = state.sensor_info.cameras[0].max_width;
    int imager_height = state.sensor_info.cameras[0].max_height;

    int raw_width;
    int raw_height;
    const char *str = getenv(RASPIJPGS_SIZE);
    if (sscanf(str, "%d,%d", &raw_width, &raw_height) != 2 ||
            (raw_height <= 0 && raw_width <= 0)) {
        // Use defaults
        raw_width = 320;
        raw_height = 0;
    }

    raw_width = constrain(0, raw_width, imager_width);
    raw_height = constrain(0, raw_height, imager_height);

    // Force to multiple of 16 for JPEG encoder
    raw_width &= ~0xf;
    raw_height &= ~0xf;

    // Check if the user wants us to auto-calculate one of
    // the dimensions.
    if (raw_height == 0) {
        raw_height = imager_height * raw_width / imager_width;
        raw_height &= ~0xf;
    } else if (raw_width == 0) {
        raw_width = imager_width * raw_height / imager_height;
        raw_width &= ~0xf;
    }

    *width = raw_width;
    *height = raw_height;
}

static void help(const struct raspi_config_opt *opt, const char *value, bool fail_on_error);

static void size_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    UNUSED(opt);
    UNUSED(fail_on_error);

    int desired_width;
    int desired_height;
    parse_requested_dimensions(&desired_width, &desired_height);

    if (desired_width != state.width ||
            desired_height != state.height) {
        stop_all();

        state.width = desired_width;
        state.height = desired_height;

        start_all();
    }
}
static void annotation_apply(const struct raspi_config_opt *opt, bool fail_on_error) { UNUSED(opt); }
static void anno_background_apply(const struct raspi_config_opt *opt, bool fail_on_error) { UNUSED(opt); }
static void rational_param_apply(int mmal_param, const struct raspi_config_opt *opt, bool fail_on_error)
{
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    if (value > 100) {
        if (fail_on_error)
            errx(EXIT_FAILURE, "%s must be between 0 and 100", opt->long_option);
        else
            return;
    }
    MMAL_RATIONAL_T mmal_value = {value, 100};
    MMAL_STATUS_T status = mmal_port_parameter_set_rational(state.camera->control, mmal_param, mmal_value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s (%d)", opt->long_option, status);
}

static void sharpness_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    rational_param_apply(MMAL_PARAMETER_SHARPNESS, opt, fail_on_error);
}
static void contrast_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    rational_param_apply(MMAL_PARAMETER_CONTRAST, opt, fail_on_error);
}
static void brightness_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    rational_param_apply(MMAL_PARAMETER_BRIGHTNESS, opt, fail_on_error);
}
static void saturation_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    rational_param_apply(MMAL_PARAMETER_SATURATION, opt, fail_on_error);
}

static void ISO_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    UNUSED(fail_on_error);
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    MMAL_STATUS_T status = mmal_port_parameter_set_uint32(state.camera->control, MMAL_PARAMETER_ISO, value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void vstab_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    UNUSED(fail_on_error);
    unsigned int value = (strcmp(getenv(opt->env_key), "on") == 0);
    MMAL_STATUS_T status = mmal_port_parameter_set_uint32(state.camera->control, MMAL_PARAMETER_VIDEO_STABILISATION, value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void ev_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    UNUSED(fail_on_error);
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    MMAL_STATUS_T status = mmal_port_parameter_set_int32(state.camera->control, MMAL_PARAMETER_EXPOSURE_COMP , value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void exposure_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    MMAL_PARAM_EXPOSUREMODE_T mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "off") == 0) mode = MMAL_PARAM_EXPOSUREMODE_OFF;
    else if(strcmp(str, "auto") == 0) mode = MMAL_PARAM_EXPOSUREMODE_AUTO;
    else if(strcmp(str, "night") == 0) mode = MMAL_PARAM_EXPOSUREMODE_NIGHT;
    else if(strcmp(str, "nightpreview") == 0) mode = MMAL_PARAM_EXPOSUREMODE_NIGHTPREVIEW;
    else if(strcmp(str, "backlight") == 0) mode = MMAL_PARAM_EXPOSUREMODE_BACKLIGHT;
    else if(strcmp(str, "spotlight") == 0) mode = MMAL_PARAM_EXPOSUREMODE_SPOTLIGHT;
    else if(strcmp(str, "sports") == 0) mode = MMAL_PARAM_EXPOSUREMODE_SPORTS;
    else if(strcmp(str, "snow") == 0) mode = MMAL_PARAM_EXPOSUREMODE_SNOW;
    else if(strcmp(str, "beach") == 0) mode = MMAL_PARAM_EXPOSUREMODE_BEACH;
    else if(strcmp(str, "verylong") == 0) mode = MMAL_PARAM_EXPOSUREMODE_VERYLONG;
    else if(strcmp(str, "fixedfps") == 0) mode = MMAL_PARAM_EXPOSUREMODE_FIXEDFPS;
    else if(strcmp(str, "antishake") == 0) mode = MMAL_PARAM_EXPOSUREMODE_ANTISHAKE;
    else if(strcmp(str, "fireworks") == 0) mode = MMAL_PARAM_EXPOSUREMODE_FIREWORKS;
    else {
        if (fail_on_error)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }

    MMAL_PARAMETER_EXPOSUREMODE_T param = {{MMAL_PARAMETER_EXPOSURE_MODE,sizeof(param)}, mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void awb_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    MMAL_PARAM_AWBMODE_T awb_mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "off") == 0) awb_mode = MMAL_PARAM_AWBMODE_OFF;
    else if(strcmp(str, "auto") == 0) awb_mode = MMAL_PARAM_AWBMODE_AUTO;
    else if(strcmp(str, "sun") == 0) awb_mode = MMAL_PARAM_AWBMODE_SUNLIGHT;
    else if(strcmp(str, "cloudy") == 0) awb_mode = MMAL_PARAM_AWBMODE_CLOUDY;
    else if(strcmp(str, "shade") == 0) awb_mode = MMAL_PARAM_AWBMODE_SHADE;
    else if(strcmp(str, "tungsten") == 0) awb_mode = MMAL_PARAM_AWBMODE_TUNGSTEN;
    else if(strcmp(str, "fluorescent") == 0) awb_mode = MMAL_PARAM_AWBMODE_FLUORESCENT;
    else if(strcmp(str, "incandescent") == 0) awb_mode = MMAL_PARAM_AWBMODE_INCANDESCENT;
    else if(strcmp(str, "flash") == 0) awb_mode = MMAL_PARAM_AWBMODE_FLASH;
    else if(strcmp(str, "horizon") == 0) awb_mode = MMAL_PARAM_AWBMODE_HORIZON;
    else {
        if (fail_on_error)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }
    MMAL_PARAMETER_AWBMODE_T param = {{MMAL_PARAMETER_AWB_MODE,sizeof(param)}, awb_mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void imxfx_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    MMAL_PARAM_IMAGEFX_T imageFX;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "none") == 0) imageFX = MMAL_PARAM_IMAGEFX_NONE;
    else if(strcmp(str, "negative") == 0) imageFX = MMAL_PARAM_IMAGEFX_NEGATIVE;
    else if(strcmp(str, "solarise") == 0) imageFX = MMAL_PARAM_IMAGEFX_SOLARIZE;
    else if(strcmp(str, "solarize") == 0) imageFX = MMAL_PARAM_IMAGEFX_SOLARIZE;
    else if(strcmp(str, "sketch") == 0) imageFX = MMAL_PARAM_IMAGEFX_SKETCH;
    else if(strcmp(str, "denoise") == 0) imageFX = MMAL_PARAM_IMAGEFX_DENOISE;
    else if(strcmp(str, "emboss") == 0) imageFX = MMAL_PARAM_IMAGEFX_EMBOSS;
    else if(strcmp(str, "oilpaint") == 0) imageFX = MMAL_PARAM_IMAGEFX_OILPAINT;
    else if(strcmp(str, "hatch") == 0) imageFX = MMAL_PARAM_IMAGEFX_HATCH;
    else if(strcmp(str, "gpen") == 0) imageFX = MMAL_PARAM_IMAGEFX_GPEN;
    else if(strcmp(str, "pastel") == 0) imageFX = MMAL_PARAM_IMAGEFX_PASTEL;
    else if(strcmp(str, "watercolour") == 0) imageFX = MMAL_PARAM_IMAGEFX_WATERCOLOUR;
    else if(strcmp(str, "watercolor") == 0) imageFX = MMAL_PARAM_IMAGEFX_WATERCOLOUR;
    else if(strcmp(str, "film") == 0) imageFX = MMAL_PARAM_IMAGEFX_FILM;
    else if(strcmp(str, "blur") == 0) imageFX = MMAL_PARAM_IMAGEFX_BLUR;
    else if(strcmp(str, "saturation") == 0) imageFX = MMAL_PARAM_IMAGEFX_SATURATION;
    else if(strcmp(str, "colourswap") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURSWAP;
    else if(strcmp(str, "colorswap") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURSWAP;
    else if(strcmp(str, "washedout") == 0) imageFX = MMAL_PARAM_IMAGEFX_WASHEDOUT;
    else if(strcmp(str, "posterise") == 0) imageFX = MMAL_PARAM_IMAGEFX_POSTERISE;
    else if(strcmp(str, "posterize") == 0) imageFX = MMAL_PARAM_IMAGEFX_POSTERISE;
    else if(strcmp(str, "colourpoint") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURPOINT;
    else if(strcmp(str, "colorpoint") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURPOINT;
    else if(strcmp(str, "colourbalance") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURBALANCE;
    else if(strcmp(str, "colorbalance") == 0) imageFX = MMAL_PARAM_IMAGEFX_COLOURBALANCE;
    else if(strcmp(str, "cartoon") == 0) imageFX = MMAL_PARAM_IMAGEFX_CARTOON;
    else {
        if (fail_on_error)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }
    MMAL_PARAMETER_IMAGEFX_T param = {{MMAL_PARAMETER_IMAGE_EFFECT,sizeof(param)}, imageFX};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void colfx_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    // Color effect is specified as u:v. Anything else means off.
    MMAL_PARAMETER_COLOURFX_T param = {{MMAL_PARAMETER_COLOUR_EFFECT,sizeof(param)}, 0, 0, 0};
    const char *str = getenv(opt->env_key);
    if (sscanf(str, "%d:%d", &param.u, &param.v) == 2 &&
            param.u < 256 &&
            param.v < 256)
        param.enable = 1;
    else
        param.enable = 0;
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void metering_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    MMAL_PARAM_EXPOSUREMETERINGMODE_T m_mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "average") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_AVERAGE;
    else if(strcmp(str, "spot") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_SPOT;
    else if(strcmp(str, "backlit") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_BACKLIT;
    else if(strcmp(str, "matrix") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_MATRIX;
    else {
        if (fail_on_error)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }
    MMAL_PARAMETER_EXPOSUREMETERINGMODE_T param = {{MMAL_PARAMETER_EXP_METERING_MODE,sizeof(param)}, m_mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void rotation_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    UNUSED(fail_on_error);
    int value = strtol(getenv(opt->env_key), NULL, 0);
    MMAL_STATUS_T status;
    status = mmal_port_parameter_set_int32(state.camera->output[CAMERA_PORT_PREVIEW], MMAL_PARAMETER_ROTATION, value);
    status |= mmal_port_parameter_set_int32(state.camera->output[CAMERA_PORT_VIDEO], MMAL_PARAMETER_ROTATION, value);
    if (status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void flip_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    UNUSED(fail_on_error);

    MMAL_PARAMETER_MIRROR_T mirror = {{MMAL_PARAMETER_MIRROR, sizeof(MMAL_PARAMETER_MIRROR_T)}, MMAL_PARAM_MIRROR_NONE};
    if (strcmp(getenv(RASPIJPGS_HFLIP), "on") == 0)
        mirror.value = MMAL_PARAM_MIRROR_HORIZONTAL;
    if (strcmp(getenv(RASPIJPGS_VFLIP), "on") == 0)
        mirror.value = (mirror.value == MMAL_PARAM_MIRROR_HORIZONTAL ? MMAL_PARAM_MIRROR_BOTH : MMAL_PARAM_MIRROR_VERTICAL);

    MMAL_STATUS_T status;
    status = mmal_port_parameter_set(state.camera->output[CAMERA_PORT_PREVIEW], &mirror.hdr);
    status |= mmal_port_parameter_set(state.camera->output[CAMERA_PORT_VIDEO], &mirror.hdr);
    if (status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void sensor_mode_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    // TODO
    UNUSED(opt);
    UNUSED(fail_on_error);
}
static void roi_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    const char *str = getenv(opt->env_key);
    if (str[0] == '\0')
        str = "0:0:1:1";

    float x, y, w, h;
    if (sscanf(str, "%f:%f:%f:%f", &x, &y, &w, &h) != 4) {
        warnx("Invalid roi format: %s", str);
        return;
    }
    x = constrainf(0, x, 1.f);
    y = constrainf(0, y, 1.f);
    w = constrainf(0, w, 1.f - x);
    h = constrainf(0, h, 1.f - y);

    MMAL_PARAMETER_INPUT_CROP_T crop;
    crop.hdr.id = MMAL_PARAMETER_INPUT_CROP;
    crop.hdr.size = sizeof(MMAL_PARAMETER_INPUT_CROP_T);
    crop.rect.x = lrintf(65536.f * x);
    crop.rect.y = lrintf(65536.f * y);
    crop.rect.width = lrintf(65536.f * w);
    crop.rect.height = lrintf(65536.f * h);

   if (mmal_port_parameter_set(state.camera->control, &crop.hdr) != MMAL_SUCCESS)
     errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void shutter_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    UNUSED(fail_on_error);
    int value = strtoul(getenv(opt->env_key), NULL, 0);
    if (mmal_port_parameter_set_uint32(state.camera->control, MMAL_PARAMETER_SHUTTER_SPEED, value) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void quality_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    UNUSED(fail_on_error);
    int value = strtoul(getenv(opt->env_key), NULL, 0);
    value = constrain(0, value, 100);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], MMAL_PARAMETER_JPEG_Q_FACTOR, value) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s to %d", opt->long_option, value);
}
static void restart_interval_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    UNUSED(fail_on_error);
    int value = strtoul(getenv(opt->env_key), NULL, 0);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], MMAL_PARAMETER_JPEG_RESTART_INTERVAL, value) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s to %d", opt->long_option, value);
}
static void fps_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    int fps256 = lrint(256.0 * strtod(getenv(opt->env_key), 0));
    if (fps256 < 0)
        fps256 = 0;

    MMAL_PARAMETER_FRAME_RATE_T rate = {{MMAL_PARAMETER_FRAME_RATE, sizeof(MMAL_PARAMETER_FRAME_RATE_T)}, {fps256, 256}};

    MMAL_STATUS_T status;
    status = mmal_port_parameter_set(state.camera->output[CAMERA_PORT_PREVIEW], &rate.hdr);
    status |= mmal_port_parameter_set(state.camera->output[CAMERA_PORT_VIDEO], &rate.hdr);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s=%d/256 (%d)", opt->long_option, fps256, status);
}

static struct raspi_config_opt opts[] =
{
    // long_option  short   env_key                  help                                                    default
    {"size",       " s",    RASPIJPGS_SIZE,        "Set image size <w,h> (h=0, calculate from w)",         "320,0",    default_set, size_apply},
    {"annotation",  "a",    RASPIJPGS_ANNOTATION,   "Annotate the video frames with this text",             "",         default_set, annotation_apply},
    {"anno_background", "ab", RASPIJPGS_ANNO_BACKGROUND, "Turn on a black background behind the annotation", "off",     default_set, anno_background_apply},
    {"sharpness",   "sh",   RASPIJPGS_SHARPNESS,    "Set image sharpness (-100 to 100)",                    "0",        default_set, sharpness_apply},
    {"contrast",    "co",   RASPIJPGS_CONTRAST,     "Set image contrast (-100 to 100)",                     "0",        default_set, contrast_apply},
    {"brightness",  "br",   RASPIJPGS_BRIGHTNESS,   "Set image brightness (0 to 100)",                      "50",       default_set, brightness_apply},
    {"saturation",  "sa",   RASPIJPGS_SATURATION,   "Set image saturation (-100 to 100)",                   "0",        default_set, saturation_apply},
    {"ISO",         "ISO",  RASPIJPGS_ISO,          "Set capture ISO (100 to 800)",                         "0",        default_set, ISO_apply},
    {"vstab",       "vs",   RASPIJPGS_VSTAB,        "Turn on video stabilisation",                          "off",      default_set, vstab_apply},
    {"ev",          "ev",   RASPIJPGS_EV,           "Set EV compensation (-10 to 10)",                      "0",        default_set, ev_apply},
    {"exposure",    "ex",   RASPIJPGS_EXPOSURE,     "Set exposure mode",                                    "auto",     default_set, exposure_apply},
    {"fps",         0,      RASPIJPGS_FPS,          "Limit the frame rate (0 = auto)",                      "0",        default_set, fps_apply},
    {"awb",         "awb",  RASPIJPGS_AWB,          "Set Automatic White Balance (AWB) mode",               "auto",     default_set, awb_apply},
    {"imxfx",       "ifx",  RASPIJPGS_IMXFX,        "Set image effect",                                     "none",     default_set, imxfx_apply},
    {"colfx",       "cfx",  RASPIJPGS_COLFX,        "Set colour effect <U:V>",                              "",         default_set, colfx_apply},
    {"mode",        "md",   RASPIJPGS_SENSOR_MODE,  "Set sensor mode (0 to 7)",                             "0",        default_set, sensor_mode_apply},
    {"metering",    "mm",   RASPIJPGS_METERING,     "Set metering mode",                                    "average",  default_set, metering_apply},
    {"rotation",    "rot",  RASPIJPGS_ROTATION,     "Set image rotation (0-359)",                           "0",        default_set, rotation_apply},
    {"hflip",       "hf",   RASPIJPGS_HFLIP,        "Set horizontal flip",                                  "off",      default_set, flip_apply},
    {"vflip",       "vf",   RASPIJPGS_VFLIP,        "Set vertical flip",                                    "off",      default_set, flip_apply},
    {"roi",         "roi",  RASPIJPGS_ROI,          "Set region of interest (x,y,w,d as normalised coordinates [0.0-1.0])", "0:0:1:1", default_set, roi_apply},
    {"shutter",     "ss",   RASPIJPGS_SHUTTER,      "Set shutter speed",                                    "0",        default_set, shutter_apply},
    {"quality",     "q",    RASPIJPGS_QUALITY,      "Set the JPEG quality (0-100)",                         "15",       default_set, quality_apply},
    {"restart_interval", "rs", RASPIJPGS_RESTART_INTERVAL, "Set the JPEG restart interval (default of 0 for none)", "0", default_set, restart_interval_apply},

    // options that can't be overridden using environment variables
    {"help",        "h",    0,                       "Print this help message",                              0,          help, 0},
    {0,             0,      0,                       0,                                                      0,          0,           0}
};

static void help(const struct raspi_config_opt *opt, const char *value, bool fail_on_error)
{
    UNUSED(opt); UNUSED(value); UNUSED(fail_on_error);

    fprintf(stderr, "raspijpgs [options]\n");

    const struct raspi_config_opt *o;
    for (o = opts; o->long_option; o++) {
        if (o->short_option)
            fprintf(stderr, "  --%-15s (-%s)\t %s\n", o->long_option, o->short_option, o->help);
        else
            fprintf(stderr, "  --%-20s\t %s\n", o->long_option, o->help);
    }

    fprintf(stderr,
            "\n"
            "Exposure (--exposure) options: auto, night, nightpreview, backlight,\n"
            "    spotlight, sports, snow, beach, verylong, fixedfps, antishake,\n"
            "    fireworks\n"
            "White balance (--awb) options: auto, sun, cloudy, shade, tungsten,\n"
            "    fluorescent, flash, horizon\n"
            "Image effect (--imxfx) options: none, negative, solarize, sketch,\n"
            "    denoise, emboss, oilpaint, hatch, gpen, pastel, watercolor, film,\n"
            "    blur, saturation, colorswap, washedout, posterize, colorpoint,\n"
            "    colorbalance, cartoon\n"
            "Metering (--metering) options: average, spot, backlit, matrix\n"
            "Sensor mode (--mode) options:\n"
            "       0   automatic selection\n"
            "       1   1920x1080 (16:9) 1-30 fps\n"
            "       2   2592x1944 (4:3)  1-15 fps\n"
            "       3   2592x1944 (4:3)  0.1666-1 fps\n"
            "       4   1296x972  (4:3)  1-42 fps, 2x2 binning\n"
            "       5   1296x730  (16:9) 1-49 fps, 2x2 binning\n"
            "       6   640x480   (4:3)  42.1-60 fps, 2x2 binning plus skip\n"
            "       7   640x480   (4:3)  60.1-90 fps, 2x2 binning plus skip\n"
            );

    exit(EXIT_FAILURE);
}

static int is_long_option(const char *str)
{
    return strlen(str) >= 3 && str[0] == '-' && str[1] == '-';
}
static int is_short_option(const char *str)
{
    return strlen(str) >= 2 && str[0] == '-' && str[1] != '-';
}

static void fillin_defaults()
{
    const struct raspi_config_opt *opt;
    for (opt = opts; opt->long_option; opt++) {
        if (opt->env_key && opt->default_value) {
            // The replace option is set to 0, so that anything set in the environment
            // is an override.
            if (setenv(opt->env_key, opt->default_value, 0) < 0)
                err(EXIT_FAILURE, "Error setting %s to %s", opt->env_key, opt->default_value);
        }
    }
}

static void apply_parameters(bool fail_on_error)
{
    const struct raspi_config_opt *opt;
    for (opt = opts; opt->long_option; opt++) {
        if (opt->apply)
            opt->apply(opt, fail_on_error);
    }
}

static void parse_args(int argc, char *argv[])
{
    int i;
    for (i = 1; i < argc; i++) {
        const struct raspi_config_opt *opt = 0;
        char *value;
        if (is_long_option(argv[i])) {
            char *key = argv[i] + 2; // skip over "--"
            value = strchr(argv[i], '=');
            if (value)
                *value = '\0'; // zap the '=' so that key is trimmed
            for (opt = opts; opt->long_option; opt++)
                if (strcmp(opt->long_option, key) == 0)
                    break;
            if (!opt->long_option) {
                warnx("Unknown option '%s'", key);
                help(0, 0, true);
            }

            if (!value && i < argc - 1 && !is_long_option(argv[i + 1]) && !is_short_option(argv[i + 1]))
                value = argv[++i];
            if (!value)
                value = "on"; // if no value, then this is a boolean argument, so set to on
        } else if (is_short_option(argv[i])) {
            char *key = argv[i] + 1; // skip over the "-"
            for (opt = opts; opt->long_option; opt++)
                if (opt->short_option && strcmp(opt->short_option, key) == 0)
                    break;
            if (!opt->long_option) {
                warnx("Unknown option '%s'", key);
                help(0, 0, true);
            }

            if (i < argc - 1)
                value = argv[++i];
            else
                value = "on"; // if no value, then this is a boolean argument, so set to on
        } else {
            warnx("Unexpected parameter '%s'", argv[i]);
            help(0, 0, true);
        }

        if (opt)
            opt->set(opt, value, true);
    }
}

static void trim_whitespace(char *s)
{
    char *left = s;
    while (*left != 0 && isspace(*left))
        left++;
    char *right = s + strlen(s) - 1;
    while (right >= left && isspace(*right))
        right--;

    int len = right - left + 1;
    if (len)
        memmove(s, left, len);
    s[len] = 0;
}

static void parse_config_line(const char *line)
{
    char *str = strdup(line);
    // Trim everything after a comment
    char *comment = strchr(str, '#');
    if (comment)
        *comment = '\0';

    // Trim whitespace off the beginning and end
    trim_whitespace(str);

    if (*str == '\0') {
        free(str);
        return;
    }

    char *key = str;
    char *value = strchr(str, '=');
    if (value) {
        *value = '\0';
        value++;
        trim_whitespace(value);
        trim_whitespace(key);
    } else
        value = "on";

    const struct raspi_config_opt *opt;
    for (opt = opts; opt->long_option; opt++)
        if (strcmp(opt->long_option, key) == 0)
            break;
    if (!opt->long_option) {
        // Ignore the bad option
        free(str);
        return;
    }

    opt->set(opt, value, false);
    if (opt->apply)
        opt->apply(opt, false);

    free(str);
}


static void output_jpeg(const char *buf, int len, uint8_t channel)
{
    struct iovec iovs[3];
    uint32_t len32 = htonl(len + sizeof(uint8_t));
    iovs[0].iov_base = &len32;
    iovs[0].iov_len = sizeof(int32_t);
    iovs[1].iov_base = &channel;
    iovs[1].iov_len = sizeof(uint8_t);
    iovs[2].iov_base = (char *) buf; // silence warning
    iovs[2].iov_len = len;
    ssize_t count = writev(STDOUT_FILENO, iovs, 3);
    if (count < 0)
        err(EXIT_FAILURE, "Error writing to stdout");
    else if (count != (ssize_t) (iovs[0].iov_len + iovs[1].iov_len + iovs[2].iov_len))
        warnx("Unexpected truncation of JPEG when writing to stdout");
}

static void recycle_buffer_in_pool(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer, MMAL_POOL_T *pool)
{
    mmal_buffer_header_release(buffer);

    if (port->is_enabled) {
        MMAL_BUFFER_HEADER_T *new_buffer;

        if (!(new_buffer = mmal_queue_get(pool->queue)) ||
             mmal_port_send_buffer(port, new_buffer) != MMAL_SUCCESS)
            errx(EXIT_FAILURE, "Could not send buffers to port");
    }
}

static void encoder_buffer_callback_impl()
{
    struct mmal_encoder_callback_msg msg;
    if (read(state.mmal_callback_pipe[0], &msg, sizeof(msg)) != sizeof(msg))
        err(EXIT_FAILURE, "read from internal pipe broke");

    MMAL_PORT_T *port = (MMAL_PORT_T *) msg.port;
    MMAL_BUFFER_HEADER_T *buffer = (MMAL_BUFFER_HEADER_T *) msg.buffer;
    uint8_t channel = (uint8_t) msg.channel;

    mmal_buffer_header_mem_lock(buffer);

    if (state.socket_buffer_ix[channel] == 0 &&
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) &&
            buffer->length <= MAX_DATA_BUFFER_SIZE) {
        // Easy case: JPEG all in one buffer
        output_jpeg((const char *) buffer->data, buffer->length, channel);
    } else {
        // Hard case: assemble JPEG
        if (state.socket_buffer_ix[channel] + buffer->length > MAX_DATA_BUFFER_SIZE) {
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) {
                state.socket_buffer_ix[channel] = 0;
            } else if (state.socket_buffer_ix[channel] != MAX_DATA_BUFFER_SIZE) {
                // Warn when frame crosses threshold
                warnx("Frame too large (%d bytes). Dropping. Adjust MAX_DATA_BUFFER_SIZE.", state.socket_buffer_ix[channel] + buffer->length);
                state.socket_buffer_ix[channel] = MAX_DATA_BUFFER_SIZE;
            }
        } else {
            memcpy(&state.socket_buffer[state.socket_buffer_ix[channel]], buffer->data, buffer->length);
            state.socket_buffer_ix[channel] += buffer->length;
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) {
                output_jpeg(state.socket_buffer, state.socket_buffer_ix[channel], channel);
                state.socket_buffer_ix[channel] = 0;
            }
        }
    }

    mmal_buffer_header_mem_unlock(buffer);

    //cam_set_annotation();

    if (channel == 0)
      recycle_buffer_in_pool(port, buffer, state.pool_jpegencoder);
    else
      recycle_buffer_in_pool(port, buffer, state.pool_alt_encoder);
}

static void jpegencoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    // If the buffer contains something, notify our main thread to process it.
    // If not, recycle it immediately.
    if (buffer->length) {
        struct mmal_encoder_callback_msg msg;
        msg.port = port;
        msg.buffer = buffer;
        msg.channel = 0;
        if (write(state.mmal_callback_pipe[1], &msg, sizeof(msg)) != sizeof(msg))
            err(EXIT_FAILURE, "write to internal pipe broke");
    } else {
        recycle_buffer_in_pool(port, buffer, state.pool_jpegencoder);
    }
}

static void alt_encoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    // If the buffer contains something, notify our main thread to process it.
    // If not, recycle it immediately.
    if (buffer->length) {
        struct mmal_encoder_callback_msg msg;
        msg.port = port;
        msg.buffer = buffer;
        msg.channel = 1;
        if (write(state.mmal_callback_pipe[1], &msg, sizeof(msg)) != sizeof(msg))
            err(EXIT_FAILURE, "write to internal pipe broke");
    } else {
        recycle_buffer_in_pool(port, buffer, state.pool_alt_encoder);
    }
}

static void discover_sensors(MMAL_PARAMETER_CAMERA_INFO_T *camera_info)
{
    MMAL_COMPONENT_T *camera_component;

    // Try to get the camera name and maximum supported resolution
    MMAL_STATUS_T status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO, &camera_component);
    if (status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Failed to create camera_info component");

    camera_info->hdr.id = MMAL_PARAMETER_CAMERA_INFO;
    camera_info->hdr.size = sizeof(MMAL_PARAMETER_CAMERA_INFO_T)-4;  // Deliberately undersize to check firmware version
    status = mmal_port_parameter_get(camera_component->control, &camera_info->hdr);

    if (status != MMAL_SUCCESS) {
        // Running on newer firmware
        camera_info->hdr.size = sizeof(MMAL_PARAMETER_CAMERA_INFO_T);
        status = mmal_port_parameter_get(camera_component->control, &camera_info->hdr);
        if (status != MMAL_SUCCESS)
            errx(EXIT_FAILURE, "Failed to get imager information even on new firmware");
    } else {
        // Older firmware. Assume one OV5647
        camera_info->num_cameras = 1;
        camera_info->num_flashes = 0;
        camera_info->cameras[0].port_id = 0;
        camera_info->cameras[0].max_width = 2592;
        camera_info->cameras[0].max_height = 1944;
        camera_info->cameras[0].lens_present = 0;
        strcpy(camera_info->cameras[0].camera_name, "OV5647");
    }

    mmal_component_destroy(camera_component);
}

void start_all()
{
    warnx("Starting start_all\r\n");
    // Create the file descriptors for getting back to the main thread
    // from the MMAL callbacks.
    if (pipe(state.mmal_callback_pipe) < 0)
        err(EXIT_FAILURE, "pipe");

    // Only the first camera is currently supported.
    int imager_width = state.sensor_info.cameras[0].max_width;
    int imager_height = state.sensor_info.cameras[0].max_height;

    //
    // create camera
    //
    if (mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &state.camera) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create camera");
    int fps = lrint(strtod(getenv(RASPIJPGS_FPS), 0));

    parse_requested_dimensions(&state.width, &state.height);

    int video_width = state.width;
    int video_height = state.height;

    MMAL_ES_FORMAT_T *format;
    MMAL_STATUS_T status;

    picam_camera_init(state.camera, imager_width, imager_height);
    picam_camera_configure_format(state.camera, video_width, video_height, fps);

    if (mmal_component_enable(state.camera) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable camera");

    //
    // create renderer
    // This is required for the auto-exposure feature to work.
    // The image slowly fades to black if nothing is consuming the preview port.
    //
    //if (mmal_component_create(MMAL_COMPONENT_DEFAULT_NULL_SINK, &null_sink) != MMAL_SUCCESS)
    if (mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &state.renderer) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create renderer");

    MMAL_DISPLAYREGION_T param;
    param.hdr.id = MMAL_PARAMETER_DISPLAYREGION;
    param.hdr.size = sizeof(MMAL_DISPLAYREGION_T);

    param.set = MMAL_DISPLAY_SET_LAYER;
    param.layer = 2;

    param.set |= MMAL_DISPLAY_SET_FULLSCREEN;
    param.fullscreen = 1;

    //param.set |= (MMAL_DISPLAY_SET_DEST_RECT | MMAL_DISPLAY_SET_FULLSCREEN);
    //param.fullscreen = 0;
    //param.dest_rect.x = 50;
    //param.dest_rect.y = 50;
    //param.dest_rect.width = 640;
    //param.dest_rect.height = 480;

    mmal_port_parameter_set(state.renderer->input[0], &param.hdr);

    if (mmal_component_enable(state.renderer) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable renderer");

    //
    // create video splitter
    //
    if (mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_SPLITTER, &state.splitter) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create splitter");

    mmal_format_copy(state.splitter->input[0]->format, state.camera->output[CAMERA_PORT_VIDEO]->format);
    if (mmal_port_format_commit(state.splitter->input[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set splitter input format");

    mmal_format_copy(state.splitter->output[0]->format, state.splitter->input[0]->format);
    if (mmal_port_format_commit(state.splitter->output[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set splitter output 0 format");

    mmal_format_copy(state.splitter->output[1]->format, state.splitter->input[0]->format);
    if (mmal_port_format_commit(state.splitter->output[1]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set splitter output 1 format");

    mmal_format_copy(state.splitter->output[2]->format, state.splitter->input[0]->format);
    if (mmal_port_format_commit(state.splitter->output[2]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set splitter output 2 format");

    if (mmal_component_enable(state.splitter) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable splitter");

    //
    // create jpeg-encoder
    //
    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &state.jpegencoder);
    if (status != MMAL_SUCCESS && status != MMAL_ENOSYS)
        errx(EXIT_FAILURE, "Could not create jpeg encoder");

    mmal_format_copy(state.jpegencoder->input[0]->format, state.splitter->output[0]->format);
    if (mmal_port_format_commit(state.jpegencoder->input[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set jpeg encoder input format");

    format = state.jpegencoder->output[0]->format;
    format->encoding = MMAL_ENCODING_JPEG;
    if (mmal_port_format_commit(state.jpegencoder->output[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set jpeg encoder output format");

    state.jpegencoder->output[0]->buffer_size = state.jpegencoder->output[0]->buffer_size_recommended;
    if (state.jpegencoder->output[0]->buffer_size < state.jpegencoder->output[0]->buffer_size_min)
        state.jpegencoder->output[0]->buffer_size = state.jpegencoder->output[0]->buffer_size_min;
    state.jpegencoder->output[0]->buffer_num = state.jpegencoder->output[0]->buffer_num_recommended;
    if(state.jpegencoder->output[0]->buffer_num < state.jpegencoder->output[0]->buffer_num_min)
        state.jpegencoder->output[0]->buffer_num = state.jpegencoder->output[0]->buffer_num_min;

    int quality = strtol(getenv(RASPIJPGS_QUALITY), 0, 0);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], MMAL_PARAMETER_JPEG_Q_FACTOR, quality) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set jpeg quality to %d", quality);

    // Set the JPEG restart interval
    int restart_interval = strtol(getenv(RASPIJPGS_RESTART_INTERVAL), 0, 0);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], MMAL_PARAMETER_JPEG_RESTART_INTERVAL, restart_interval) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Unable to set JPEG restart interval");

    if (mmal_port_parameter_set_boolean(state.jpegencoder->output[0], MMAL_PARAMETER_EXIF_DISABLE, 1) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not turn off EXIF");

    if (mmal_component_enable(state.jpegencoder) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable image encoder");

    state.pool_jpegencoder = mmal_port_pool_create(state.jpegencoder->output[0], state.jpegencoder->output[0]->buffer_num, state.jpegencoder->output[0]->buffer_size);
    if (!state.pool_jpegencoder)
        errx(EXIT_FAILURE, "Could not create image buffer pool");

    //
    // create resizer
    //
    status = mmal_component_create("vc.ril.resize", &state.resizer);
    if (status != MMAL_SUCCESS && status != MMAL_ENOSYS)
        errx(EXIT_FAILURE, "Could not create resizer");

    mmal_format_copy(state.resizer->input[0]->format, state.splitter->output[0]->format);
    if (mmal_port_format_commit(state.resizer->input[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set resizer input format");

    mmal_format_copy(state.resizer->output[0]->format, state.splitter->output[0]->format);
    if (mmal_port_format_commit(state.resizer->input[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set resizer output format");

    if (mmal_component_enable(state.resizer) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable resizer");

    //
    // create alternate encoder
    //
    status = mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &state.alt_encoder);
    if (status != MMAL_SUCCESS && status != MMAL_ENOSYS)
        errx(EXIT_FAILURE, "Could not create alt_encoder");

    mmal_format_copy(state.alt_encoder->input[0]->format, state.splitter->output[0]->format);
    if (mmal_port_format_commit(state.alt_encoder->input[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set alt_encoder input format");

    format = state.alt_encoder->output[0]->format;
    format->encoding = MMAL_ENCODING_JPEG;
    if (mmal_port_format_commit(state.alt_encoder->output[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set alt_encoder output format");

    state.alt_encoder->output[0]->buffer_size = state.alt_encoder->output[0]->buffer_size_recommended;
    if (state.alt_encoder->output[0]->buffer_size < state.alt_encoder->output[0]->buffer_size_min)
        state.alt_encoder->output[0]->buffer_size = state.alt_encoder->output[0]->buffer_size_min;
    state.alt_encoder->output[0]->buffer_num = state.alt_encoder->output[0]->buffer_num_recommended;
    if(state.alt_encoder->output[0]->buffer_num < state.alt_encoder->output[0]->buffer_num_min)
        state.alt_encoder->output[0]->buffer_num = state.alt_encoder->output[0]->buffer_num_min;

    quality = strtol(getenv(RASPIJPGS_QUALITY), 0, 0);
    if (mmal_port_parameter_set_uint32(state.alt_encoder->output[0], MMAL_PARAMETER_JPEG_Q_FACTOR, quality) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set jpeg quality to %d", quality);

    // Set the JPEG restart interval
    restart_interval = strtol(getenv(RASPIJPGS_RESTART_INTERVAL), 0, 0);
    if (mmal_port_parameter_set_uint32(state.alt_encoder->output[0], MMAL_PARAMETER_JPEG_RESTART_INTERVAL, restart_interval) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Unable to set JPEG restart interval");

    if (mmal_port_parameter_set_boolean(state.alt_encoder->output[0], MMAL_PARAMETER_EXIF_DISABLE, 1) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not turn off EXIF");

    if (mmal_component_enable(state.alt_encoder) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable image encoder");

    state.pool_alt_encoder = mmal_port_pool_create(state.alt_encoder->output[0], state.alt_encoder->output[0]->buffer_num, state.alt_encoder->output[0]->buffer_size);
    if (!state.pool_alt_encoder)
        errx(EXIT_FAILURE, "Could not create image buffer pool");

    warnx("starting connections in start_all\r\n");
    //
    // connect
    //
    // camera[0] -> renderer
    //if (
    //    mmal_connection_create(
    //      &state.con_cam_renderer,
    //      state.camera->output[CAMERA_PORT_PREVIEW],
    //      state.renderer->input[0],
    //      MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
    //    ) != MMAL_SUCCESS)
    //    errx(EXIT_FAILURE, "Could not create connection camera -> renderer");
    //if (mmal_connection_enable(state.con_cam_renderer) != MMAL_SUCCESS)
    //    errx(EXIT_FAILURE, "Could not enable connection camera -> renderer");

    warnx("Connecting camera to splitter\r\n");
    // camera[1] -> splitter
    if (mmal_connection_create(
          &state.con_camera_splitter,
          state.camera->output[CAMERA_PORT_VIDEO],
          state.splitter->input[0],
          MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
        ) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create connection camera -> splitter");
    if (mmal_connection_enable(state.con_camera_splitter) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable connection camera -> splitter");

    warnx("Connecting splitter to renderer\r\n");
    // splitter[0] -> renderer
    if (
        mmal_connection_create(
          &state.con_splitter_renderer,
          state.splitter->output[0],
          state.renderer->input[0],
          MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
        ) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create connection splitter -> renderer");
    if (mmal_connection_enable(state.con_splitter_renderer) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable connection splitter -> renderer");

    warnx("Connecting splitter to jpegencoder\r\n");
    // splitter[1] -> jpegencoder
    if (mmal_connection_create(
          &state.con_splitter_jpeg,
          state.splitter->output[1],
          state.jpegencoder->input[0],
          MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
        ) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create connection splitter -> encoder");
    if (mmal_connection_enable(state.con_splitter_jpeg) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable connection splitter -> encoder");

    //warnx("Connecting splitter to alt_encoder\r\n");
    //// splitter[2] -> alt_encoder
    //if (mmal_connection_create(
    //      &state.con_resizer_alt_encoder,
    //      state.splitter->output[2],
    //      state.alt_encoder->input[0],
    //      MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
    //    ) != MMAL_SUCCESS)
    //    errx(EXIT_FAILURE, "Could not create connection splitter -> alt_encoder");
    //if (mmal_connection_enable(state.con_resizer_alt_encoder) != MMAL_SUCCESS)
    //    errx(EXIT_FAILURE, "Could not enable connection splitter -> alt_encoder");

    warnx("Connecting splitter to resizer\r\n");
    // splitter[2] -> resizer
    if (mmal_connection_create(
          &state.con_splitter_resizer,
          state.splitter->output[2],
          state.resizer->input[0],
          MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
        ) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create connection splitter -> resizer");
    if (mmal_connection_enable(state.con_splitter_resizer) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable connection splitter -> resizer");

    warnx("Connecting resizer to alt_encoder\r\n");
    // resizer[0] -> alt_encoder
    if (mmal_connection_create(
          &state.con_resizer_alt_encoder,
          state.resizer->output[0],
          state.alt_encoder->input[0],
          MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT
        ) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create connection resizer -> alt_encoder");
    if (mmal_connection_enable(state.con_resizer_alt_encoder) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable connection resizer -> alt_encoder");

    warnx("enabling jpegencoder\r\n");
    //
    // enable jpegencoder
    //
    if (mmal_port_enable(state.jpegencoder->output[0], jpegencoder_buffer_callback) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable jpeg port");
    int max = mmal_queue_length(state.pool_jpegencoder->queue);
    int i;
    for (i = 0; i < max; i++) {
        MMAL_BUFFER_HEADER_T *jpegbuffer = mmal_queue_get(state.pool_jpegencoder->queue);
        if (!jpegbuffer)
            errx(EXIT_FAILURE, "Could not create jpeg buffer header");
        if (mmal_port_send_buffer(state.jpegencoder->output[0], jpegbuffer) != MMAL_SUCCESS)
            errx(EXIT_FAILURE, "Could not send buffers to jpeg port");
    }

    warnx("enabling alt_encoder\r\n");
    //
    // enable alt_encoder
    //
    if (mmal_port_enable(state.alt_encoder->output[0], alt_encoder_buffer_callback) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable alt_encoder port");
    max = mmal_queue_length(state.pool_alt_encoder->queue);
    for (i = 0; i < max; i++) {
        MMAL_BUFFER_HEADER_T *altbuffer = mmal_queue_get(state.pool_alt_encoder->queue);
        if (!altbuffer)
            errx(EXIT_FAILURE, "Could not create alt_encoder buffer header");
        if (mmal_port_send_buffer(state.alt_encoder->output[0], altbuffer) != MMAL_SUCCESS)
            errx(EXIT_FAILURE, "Could not send buffers to alt_encoder port");
    }

    //
    // Set all parameters
    //
    warnx("setting parameters\r\n");
    apply_parameters(true);
    warnx("Finished start_all\r\n");
}

void stop_all()
{
    warnx("disabling encoder outputs\r\n");
    mmal_port_disable(state.jpegencoder->output[0]);
    mmal_port_disable(state.alt_encoder->output[0]);

    warnx("destroying pools\r\n");
    mmal_port_pool_destroy(state.jpegencoder->output[0], state.pool_jpegencoder);
    mmal_port_pool_destroy(state.alt_encoder->output[0], state.pool_alt_encoder);

    warnx("disabling components\r\n");
    mmal_component_disable(state.jpegencoder);
    mmal_component_disable(state.alt_encoder);
    mmal_component_disable(state.renderer);
    //mmal_component_disable(state.null_sink);
    mmal_component_disable(state.resizer);
    mmal_component_disable(state.splitter);
    mmal_component_disable(state.camera);

    warnx("destroying components\r\n");
    mmal_component_destroy(state.jpegencoder);
    mmal_component_destroy(state.alt_encoder);
    mmal_component_destroy(state.renderer);
    //mmal_component_destroy(state.null_sink);
    mmal_component_destroy(state.resizer);
    mmal_component_destroy(state.splitter);
    mmal_component_destroy(state.camera);

    warnx("destroying connections\r\n");
    //mmal_connection_destroy(state.con_camera_null_sink);
    //mmal_connection_destroy(state.con_splitter_jpeg);
    //mmal_connection_destroy(state.con_resizer_alt_encoder);
    //mmal_connection_destroy(state.con_splitter_resizer);
    //mmal_connection_destroy(state.con_splitter_renderer);
    //mmal_connection_destroy(state.con_camera_splitter);

    warnx("closing pipes\r\n");
    close(state.mmal_callback_pipe[0]);
    close(state.mmal_callback_pipe[1]);
}

static void parse_config_lines(char *lines)
{
    char *line = lines;
    char *line_end;
    do {
        line_end = strchr(line, '\n');
        if (line_end)
            *line_end = '\0';
        parse_config_line(line);
        line = line_end + 1;
    } while (line_end);
}

static unsigned int from_uint32_be(const char *buffer)
{
    uint8_t *buf = (uint8_t*) buffer;
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[3];
}

static void process_stdin_header_framing()
{
    // Each packet is length (4 bytes big endian), data
    int len = 0;
    while (state.stdin_buffer_ix > 4 &&
           (len = from_uint32_be(state.stdin_buffer)) &&
           state.stdin_buffer_ix >= 4 + len) {
        // Copy over the lines to process so that they can be
        // null terminated.
        char lines[len + 1];
        memcpy(lines, state.stdin_buffer + 4, len);
        lines[len] = '\0';

        parse_config_lines(lines);

        // Advance to the next packet
        state.stdin_buffer_ix -= 4 + len;
        memmove(state.stdin_buffer, state.stdin_buffer + 4 + len, state.stdin_buffer_ix);
    }

    // Check if we got a bogus length packet
    if (len >= MAX_DATA_BUFFER_SIZE - 4 - 1)
        errx(EXIT_FAILURE, "Invalid packet size. Out of sync?");
}

static int server_service_stdin()
{
    // Make sure that we have room to receive more data. If not,
    // a line is ridiculously long or a frame is messed up, so exit.
    if (state.stdin_buffer_ix >= MAX_REQUEST_BUFFER_SIZE - 1)
        err(EXIT_FAILURE, "Line too long on stdin");

    // Read in everything on stdin and see what gets processed
    int amount_read = read(STDIN_FILENO, &state.stdin_buffer[state.stdin_buffer_ix], MAX_REQUEST_BUFFER_SIZE - state.stdin_buffer_ix - 1);
    if (amount_read < 0)
        err(EXIT_FAILURE, "Error reading stdin");

    // Check if stdin was closed.
    if (amount_read == 0)
        return 0;

    state.stdin_buffer_ix += amount_read;

    // If we're in header framing mode, then everything sent and
    // received is prepended by a length. Otherwise it's just text
    // lines.
        process_stdin_header_framing();

    return amount_read;
}

static void server_loop()
{
    if (isatty(STDIN_FILENO))
        errx(EXIT_FAILURE, "stdin should be a program and not a tty");

    // Init hardware
    bcm_host_init();

    discover_sensors(&state.sensor_info);
    if (state.sensor_info.num_cameras == 0)
        errx(EXIT_FAILURE, "No imagers detected!");

    start_all();

    // Main loop - keep going until we don't want any more JPEGs.
    state.stdin_buffer = (char*) malloc(MAX_REQUEST_BUFFER_SIZE);

    for (;;) {
        struct pollfd fds[2];
        int fds_count = 2;
        fds[0].fd = state.mmal_callback_pipe[0];
        fds[0].events = POLLIN;
        fds[1].fd = STDIN_FILENO;
        fds[1].events = POLLIN;

        int ready = poll(fds, fds_count, 2000);
        if (ready < 0) {
            if (errno != EINTR)
                err(EXIT_FAILURE, "poll");
        } else if (ready == 0) {
            // Time out - something is wrong that we're not getting MMAL callbacks
            errx(EXIT_FAILURE, "MMAL unresponsive. Video stuck?");
        } else {
            if (fds[0].revents)
                encoder_buffer_callback_impl();
            if (fds[1].revents) {
                if (server_service_stdin() <= 0)
                    break;
            }
        }
    }

    stop_all();
    free(state.stdin_buffer);
}

int main(int argc, char* argv[])
{
    memset(&state, 0, sizeof(state));

    // Parse commandline and config file arguments
    parse_args(argc, argv);

    // If anything still isn't set, then fill-in with defaults
    fillin_defaults();

    // Allocate buffers
    state.socket_buffer = (char *) malloc(MAX_DATA_BUFFER_SIZE);
    if (!state.socket_buffer)
        err(EXIT_FAILURE, "malloc");

    server_loop();

    free(state.socket_buffer);

    exit(EXIT_SUCCESS);
}
