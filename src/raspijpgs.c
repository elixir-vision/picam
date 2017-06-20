/**
 * \file raspijpgs.c
 * command line program to capture and stream mjpeg video on a raspberry pi.
 *
 * description
 *
 * raspijpgs is a unix commandline-friendly mjpeg streaming program with parts
 * copied from raspimjpeg, raspivid, and raspistill. it can be run as either
 * a client or server. the server connects to the pi camera via the mmal
 * interface. it can either record video itself or send it to clients. all
 * interprocess communication is done via unix domain sockets.
 *
 * for usage and examples, see readme.md
 */

#define _gnu_source // for asprintf()
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

#define max_data_buffer_size        131072
#define max_request_buffer_size     4096

#define unused(expr) do { (void)(expr); } while (0)

//
// when considering imager features to add, verify that they are supported first.
// see https://www.raspberrypi.org/forums/viewtopic.php?p=1152920&sid=b3a527262eddeb8e00bfcb01dab2036c#p1152920
//

// environment config keys
#define raspijpgs_size              "raspijpgs_size"
#define raspijpgs_fps		    "raspijpgs_fps"
#define raspijpgs_annotation        "raspijpgs_annotation"
#define raspijpgs_anno_background   "raspijpgs_anno_background"
#define raspijpgs_sharpness         "raspijpgs_sharpness"
#define raspijpgs_contrast          "raspijpgs_contrast"
#define raspijpgs_brightness        "raspijpgs_brightness"
#define raspijpgs_saturation        "raspijpgs_saturation"
#define raspijpgs_iso               "raspijpgs_iso"
#define raspijpgs_vstab             "raspijpgs_vstab"
#define raspijpgs_ev                "raspijpgs_ev"
#define raspijpgs_exposure          "raspijpgs_exposure"
#define raspijpgs_awb               "raspijpgs_awb"
#define raspijpgs_imxfx             "raspijpgs_imxfx"
#define raspijpgs_colfx             "raspijpgs_colfx"
#define raspijpgs_sensor_mode       "raspijpgs_sensor_mode"
#define raspijpgs_metering          "raspijpgs_metering"
#define raspijpgs_rotation          "raspijpgs_rotation"
#define raspijpgs_hflip             "raspijpgs_hflip"
#define raspijpgs_vflip             "raspijpgs_vflip"
#define raspijpgs_roi               "raspijpgs_roi"
#define raspijpgs_shutter           "raspijpgs_shutter"
#define raspijpgs_quality           "raspijpgs_quality"
#define raspijpgs_restart_interval  "raspijpgs_restart_interval"

// globals

struct raspijpgs_state
{
    // sensor
    mmal_parameter_camera_info_t sensor_info;

    // current settings
    int width;
    int height;

    // communication
    char *socket_buffer;
    int socket_buffer_ix;
    char *stdin_buffer;
    int stdin_buffer_ix;

    // mmal resources
    mmal_component_t *camera;
    mmal_component_t *jpegencoder;
    mmal_component_t *resizer;
    mmal_connection_t *con_cam_res;
    mmal_connection_t *con_res_jpeg;
    mmal_pool_t *pool_jpegencoder;

    // mmal callback -> main loop
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

    // record the value (called as options are set)
    // set replace=0 to only set the value if it hasn't been set already.
    void (*set)(const struct raspi_config_opt *, const char *value, bool fail_on_error);

    // apply the option (called on every option)
    void (*apply)(const struct raspi_config_opt *, bool fail_on_error);
};
static struct raspi_config_opt opts[];

static void stop_all();
static void start_all();

static void default_set(const struct raspi_config_opt *opt, const char *value, bool fail_on_error)
{
    if (!opt->env_key)
        return;

    if (value) {
        if (setenv(opt->env_key, value, 1 /*replace*/) < 0)
            err(exit_failure, "error setting %s to %s", opt->env_key, opt->default_value);
    } else {
        if (unsetenv(opt->env_key) < 0)
            err(exit_failure, "error unsetting %s", opt->env_key);
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
    // find out the max dimensions for calculations below.
    // only the first imager is currently supported.

    int imager_width = state.sensor_info.cameras[0].max_width;
    int imager_height = state.sensor_info.cameras[0].max_height;

    int raw_width;
    int raw_height;
    const char *str = getenv(raspijpgs_size);
    if (sscanf(str, "%d,%d", &raw_width, &raw_height) != 2 ||
            (raw_height <= 0 && raw_width <= 0)) {
        // use defaults
        raw_width = 320;
        raw_height = 0;
    }

    raw_width = constrain(0, raw_width, imager_width);
    raw_height = constrain(0, raw_height, imager_height);

    // force to multiple of 16 for jpeg encoder
    raw_width &= ~0xf;
    raw_height &= ~0xf;

    // check if the user wants us to auto-calculate one of
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
    unused(opt);
    unused(fail_on_error);

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
static void annotation_apply(const struct raspi_config_opt *opt, bool fail_on_error) { unused(opt); }
static void anno_background_apply(const struct raspi_config_opt *opt, bool fail_on_error) { unused(opt); }
static void rational_param_apply(int mmal_param, const struct raspi_config_opt *opt, bool fail_on_error)
{
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    if (value > 100) {
        if (fail_on_error)
            errx(exit_failure, "%s must be between 0 and 100", opt->long_option);
        else
            return;
    }
    mmal_rational_t mmal_value = {value, 100};
    mmal_status_t status = mmal_port_parameter_set_rational(state.camera->control, mmal_param, mmal_value);
    if(status != mmal_success)
        errx(exit_failure, "could not set %s (%d)", opt->long_option, status);
}

static void sharpness_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    rational_param_apply(mmal_parameter_sharpness, opt, fail_on_error);
}
static void contrast_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    rational_param_apply(mmal_parameter_contrast, opt, fail_on_error);
}
static void brightness_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    rational_param_apply(mmal_parameter_brightness, opt, fail_on_error);
}
static void saturation_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    rational_param_apply(mmal_parameter_saturation, opt, fail_on_error);
}

static void iso_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    unused(fail_on_error);
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    mmal_status_t status = mmal_port_parameter_set_uint32(state.camera->control, mmal_parameter_iso, value);
    if(status != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void vstab_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    unused(fail_on_error);
    unsigned int value = (strcmp(getenv(opt->env_key), "on") == 0);
    mmal_status_t status = mmal_port_parameter_set_uint32(state.camera->control, mmal_parameter_video_stabilisation, value);
    if(status != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void ev_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    unused(fail_on_error);
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    mmal_status_t status = mmal_port_parameter_set_int32(state.camera->control, mmal_parameter_exposure_comp , value);
    if(status != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void exposure_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    mmal_param_exposuremode_t mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "off") == 0) mode = mmal_param_exposuremode_off;
    else if(strcmp(str, "auto") == 0) mode = mmal_param_exposuremode_auto;
    else if(strcmp(str, "night") == 0) mode = mmal_param_exposuremode_night;
    else if(strcmp(str, "nightpreview") == 0) mode = mmal_param_exposuremode_nightpreview;
    else if(strcmp(str, "backlight") == 0) mode = mmal_param_exposuremode_backlight;
    else if(strcmp(str, "spotlight") == 0) mode = mmal_param_exposuremode_spotlight;
    else if(strcmp(str, "sports") == 0) mode = mmal_param_exposuremode_sports;
    else if(strcmp(str, "snow") == 0) mode = mmal_param_exposuremode_snow;
    else if(strcmp(str, "beach") == 0) mode = mmal_param_exposuremode_beach;
    else if(strcmp(str, "verylong") == 0) mode = mmal_param_exposuremode_verylong;
    else if(strcmp(str, "fixedfps") == 0) mode = mmal_param_exposuremode_fixedfps;
    else if(strcmp(str, "antishake") == 0) mode = mmal_param_exposuremode_antishake;
    else if(strcmp(str, "fireworks") == 0) mode = mmal_param_exposuremode_fireworks;
    else {
        if (fail_on_error)
            errx(exit_failure, "invalid %s", opt->long_option);
        else
            return;
    }

    mmal_parameter_exposuremode_t param = {{mmal_parameter_exposure_mode,sizeof(param)}, mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void awb_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    mmal_param_awbmode_t awb_mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "off") == 0) awb_mode = mmal_param_awbmode_off;
    else if(strcmp(str, "auto") == 0) awb_mode = mmal_param_awbmode_auto;
    else if(strcmp(str, "sun") == 0) awb_mode = mmal_param_awbmode_sunlight;
    else if(strcmp(str, "cloudy") == 0) awb_mode = mmal_param_awbmode_cloudy;
    else if(strcmp(str, "shade") == 0) awb_mode = mmal_param_awbmode_shade;
    else if(strcmp(str, "tungsten") == 0) awb_mode = mmal_param_awbmode_tungsten;
    else if(strcmp(str, "fluorescent") == 0) awb_mode = mmal_param_awbmode_fluorescent;
    else if(strcmp(str, "incandescent") == 0) awb_mode = mmal_param_awbmode_incandescent;
    else if(strcmp(str, "flash") == 0) awb_mode = mmal_param_awbmode_flash;
    else if(strcmp(str, "horizon") == 0) awb_mode = mmal_param_awbmode_horizon;
    else {
        if (fail_on_error)
            errx(exit_failure, "invalid %s", opt->long_option);
        else
            return;
    }
    mmal_parameter_awbmode_t param = {{mmal_parameter_awb_mode,sizeof(param)}, awb_mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void imxfx_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    mmal_param_imagefx_t imagefx;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "none") == 0) imagefx = mmal_param_imagefx_none;
    else if(strcmp(str, "negative") == 0) imagefx = mmal_param_imagefx_negative;
    else if(strcmp(str, "solarise") == 0) imagefx = mmal_param_imagefx_solarize;
    else if(strcmp(str, "solarize") == 0) imagefx = mmal_param_imagefx_solarize;
    else if(strcmp(str, "sketch") == 0) imagefx = mmal_param_imagefx_sketch;
    else if(strcmp(str, "denoise") == 0) imagefx = mmal_param_imagefx_denoise;
    else if(strcmp(str, "emboss") == 0) imagefx = mmal_param_imagefx_emboss;
    else if(strcmp(str, "oilpaint") == 0) imagefx = mmal_param_imagefx_oilpaint;
    else if(strcmp(str, "hatch") == 0) imagefx = mmal_param_imagefx_hatch;
    else if(strcmp(str, "gpen") == 0) imagefx = mmal_param_imagefx_gpen;
    else if(strcmp(str, "pastel") == 0) imagefx = mmal_param_imagefx_pastel;
    else if(strcmp(str, "watercolour") == 0) imagefx = mmal_param_imagefx_watercolour;
    else if(strcmp(str, "watercolor") == 0) imagefx = mmal_param_imagefx_watercolour;
    else if(strcmp(str, "film") == 0) imagefx = mmal_param_imagefx_film;
    else if(strcmp(str, "blur") == 0) imagefx = mmal_param_imagefx_blur;
    else if(strcmp(str, "saturation") == 0) imagefx = mmal_param_imagefx_saturation;
    else if(strcmp(str, "colourswap") == 0) imagefx = mmal_param_imagefx_colourswap;
    else if(strcmp(str, "colorswap") == 0) imagefx = mmal_param_imagefx_colourswap;
    else if(strcmp(str, "washedout") == 0) imagefx = mmal_param_imagefx_washedout;
    else if(strcmp(str, "posterise") == 0) imagefx = mmal_param_imagefx_posterise;
    else if(strcmp(str, "posterize") == 0) imagefx = mmal_param_imagefx_posterise;
    else if(strcmp(str, "colourpoint") == 0) imagefx = mmal_param_imagefx_colourpoint;
    else if(strcmp(str, "colorpoint") == 0) imagefx = mmal_param_imagefx_colourpoint;
    else if(strcmp(str, "colourbalance") == 0) imagefx = mmal_param_imagefx_colourbalance;
    else if(strcmp(str, "colorbalance") == 0) imagefx = mmal_param_imagefx_colourbalance;
    else if(strcmp(str, "cartoon") == 0) imagefx = mmal_param_imagefx_cartoon;
    else {
        if (fail_on_error)
            errx(exit_failure, "invalid %s", opt->long_option);
        else
            return;
    }
    mmal_parameter_imagefx_t param = {{mmal_parameter_image_effect,sizeof(param)}, imagefx};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void colfx_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    // color effect is specified as u:v. anything else means off.
    mmal_parameter_colourfx_t param = {{mmal_parameter_colour_effect,sizeof(param)}, 0, 0, 0};
    const char *str = getenv(opt->env_key);
    if (sscanf(str, "%d:%d", &param.u, &param.v) == 2 &&
            param.u < 256 &&
            param.v < 256)
        param.enable = 1;
    else
        param.enable = 0;
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void metering_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    mmal_param_exposuremeteringmode_t m_mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "average") == 0) m_mode = mmal_param_exposuremeteringmode_average;
    else if(strcmp(str, "spot") == 0) m_mode = mmal_param_exposuremeteringmode_spot;
    else if(strcmp(str, "backlit") == 0) m_mode = mmal_param_exposuremeteringmode_backlit;
    else if(strcmp(str, "matrix") == 0) m_mode = mmal_param_exposuremeteringmode_matrix;
    else {
        if (fail_on_error)
            errx(exit_failure, "invalid %s", opt->long_option);
        else
            return;
    }
    mmal_parameter_exposuremeteringmode_t param = {{mmal_parameter_exp_metering_mode,sizeof(param)}, m_mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void rotation_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    unused(fail_on_error);
    int value = strtol(getenv(opt->env_key), null, 0);
    if (mmal_port_parameter_set_int32(state.camera->output[0], mmal_parameter_rotation, value) != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void flip_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    unused(fail_on_error);

    mmal_parameter_mirror_t mirror = {{mmal_parameter_mirror, sizeof(mmal_parameter_mirror_t)}, mmal_param_mirror_none};
    if (strcmp(getenv(raspijpgs_hflip), "on") == 0)
        mirror.value = mmal_param_mirror_horizontal;
    if (strcmp(getenv(raspijpgs_vflip), "on") == 0)
        mirror.value = (mirror.value == mmal_param_mirror_horizontal ? mmal_param_mirror_both : mmal_param_mirror_vertical);

    if (mmal_port_parameter_set(state.camera->output[0], &mirror.hdr) != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void sensor_mode_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    // todo
    unused(opt);
    unused(fail_on_error);
}
static void roi_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    const char *str = getenv(opt->env_key);
    if (str[0] == '\0')
        str = "0:0:1:1";

    float x, y, w, h;
    if (sscanf(str, "%f:%f:%f:%f", &x, &y, &w, &h) != 4) {
        warnx("invalid roi format: %s", str);
        return;
    }
    x = constrainf(0, x, 1.f);
    y = constrainf(0, y, 1.f);
    w = constrainf(0, w, 1.f - x);
    h = constrainf(0, h, 1.f - y);

    mmal_parameter_input_crop_t crop;
    crop.hdr.id = mmal_parameter_input_crop;
    crop.hdr.size = sizeof(mmal_parameter_input_crop_t);
    crop.rect.x = lrintf(65536.f * x);
    crop.rect.y = lrintf(65536.f * y);
    crop.rect.width = lrintf(65536.f * w);
    crop.rect.height = lrintf(65536.f * h);

   if (mmal_port_parameter_set(state.camera->control, &crop.hdr) != mmal_success)
     errx(exit_failure, "could not set %s", opt->long_option);
}
static void shutter_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    unused(fail_on_error);
    int value = strtoul(getenv(opt->env_key), null, 0);
    if (mmal_port_parameter_set_uint32(state.camera->control, mmal_parameter_shutter_speed, value) != mmal_success)
        errx(exit_failure, "could not set %s", opt->long_option);
}
static void quality_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    unused(fail_on_error);
    int value = strtoul(getenv(opt->env_key), null, 0);
    value = constrain(0, value, 100);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], mmal_parameter_jpeg_q_factor, value) != mmal_success)
        errx(exit_failure, "could not set %s to %d", opt->long_option, value);
}
static void restart_interval_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    unused(fail_on_error);
    int value = strtoul(getenv(opt->env_key), null, 0);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], mmal_parameter_jpeg_restart_interval, value) != mmal_success)
        errx(exit_failure, "could not set %s to %d", opt->long_option, value);
}
static void fps_apply(const struct raspi_config_opt *opt, bool fail_on_error)
{
    int fps256 = lrint(256.0 * strtod(getenv(opt->env_key), 0));
    if (fps256 < 0)
        fps256 = 0;

    mmal_parameter_frame_rate_t rate = {{mmal_parameter_frame_rate, sizeof(mmal_parameter_frame_rate_t)}, {fps256, 256}};
    mmal_status_t status = mmal_port_parameter_set(state.camera->output[0], &rate.hdr);
    if(status != mmal_success)
        errx(exit_failure, "could not set %s=%d/256 (%d)", opt->long_option, fps256, status);
}

static struct raspi_config_opt opts[] =
{
    // long_option  short   env_key                  help                                                    default
    {"size",       " s",    raspijpgs_size,        "set image size <w,h> (h=0, calculate from w)",         "320,0",    default_set, size_apply},
    {"annotation",  "a",    raspijpgs_annotation,   "annotate the video frames with this text",             "",         default_set, annotation_apply},
    {"anno_background", "ab", raspijpgs_anno_background, "turn on a black background behind the annotation", "off",     default_set, anno_background_apply},
    {"sharpness",   "sh",   raspijpgs_sharpness,    "set image sharpness (-100 to 100)",                    "0",        default_set, sharpness_apply},
    {"contrast",    "co",   raspijpgs_contrast,     "set image contrast (-100 to 100)",                     "0",        default_set, contrast_apply},
    {"brightness",  "br",   raspijpgs_brightness,   "set image brightness (0 to 100)",                      "50",       default_set, brightness_apply},
    {"saturation",  "sa",   raspijpgs_saturation,   "set image saturation (-100 to 100)",                   "0",        default_set, saturation_apply},
    {"iso",         "iso",  raspijpgs_iso,          "set capture iso (100 to 800)",                         "0",        default_set, iso_apply},
    {"vstab",       "vs",   raspijpgs_vstab,        "turn on video stabilisation",                          "off",      default_set, vstab_apply},
    {"ev",          "ev",   raspijpgs_ev,           "set ev compensation (-10 to 10)",                      "0",        default_set, ev_apply},
    {"exposure",    "ex",   raspijpgs_exposure,     "set exposure mode",                                    "auto",     default_set, exposure_apply},
    {"fps",         0,      raspijpgs_fps,          "limit the frame rate (0 = auto)",                      "0",        default_set, fps_apply},
    {"awb",         "awb",  raspijpgs_awb,          "set automatic white balance (awb) mode",               "auto",     default_set, awb_apply},
    {"imxfx",       "ifx",  raspijpgs_imxfx,        "set image effect",                                     "none",     default_set, imxfx_apply},
    {"colfx",       "cfx",  raspijpgs_colfx,        "set colour effect <u:v>",                              "",         default_set, colfx_apply},
    {"mode",        "md",   raspijpgs_sensor_mode,  "set sensor mode (0 to 7)",                             "0",        default_set, sensor_mode_apply},
    {"metering",    "mm",   raspijpgs_metering,     "set metering mode",                                    "average",  default_set, metering_apply},
    {"rotation",    "rot",  raspijpgs_rotation,     "set image rotation (0-359)",                           "0",        default_set, rotation_apply},
    {"hflip",       "hf",   raspijpgs_hflip,        "set horizontal flip",                                  "off",      default_set, flip_apply},
    {"vflip",       "vf",   raspijpgs_vflip,        "set vertical flip",                                    "off",      default_set, flip_apply},
    {"roi",         "roi",  raspijpgs_roi,          "set region of interest (x,y,w,d as normalised coordinates [0.0-1.0])", "0:0:1:1", default_set, roi_apply},
    {"shutter",     "ss",   raspijpgs_shutter,      "set shutter speed",                                    "0",        default_set, shutter_apply},
    {"quality",     "q",    raspijpgs_quality,      "set the jpeg quality (0-100)",                         "15",       default_set, quality_apply},
    {"restart_interval", "rs", raspijpgs_restart_interval, "set the jpeg restart interval (default of 0 for none)", "0", default_set, restart_interval_apply},

    // options that can't be overridden using environment variables
    {"help",        "h",    0,                       "print this help message",                              0,          help, 0},
    {0,             0,      0,                       0,                                                      0,          0,           0}
};

static void help(const struct raspi_config_opt *opt, const char *value, bool fail_on_error)
{
    unused(opt); unused(value); unused(fail_on_error);

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
            "exposure (--exposure) options: auto, night, nightpreview, backlight,\n"
            "    spotlight, sports, snow, beach, verylong, fixedfps, antishake,\n"
            "    fireworks\n"
            "white balance (--awb) options: auto, sun, cloudy, shade, tungsten,\n"
            "    fluorescent, flash, horizon\n"
            "image effect (--imxfx) options: none, negative, solarize, sketch,\n"
            "    denoise, emboss, oilpaint, hatch, gpen, pastel, watercolor, film,\n"
            "    blur, saturation, colorswap, washedout, posterize, colorpoint,\n"
            "    colorbalance, cartoon\n"
            "metering (--metering) options: average, spot, backlit, matrix\n"
            "sensor mode (--mode) options:\n"
            "       0   automatic selection\n"
            "       1   1920x1080 (16:9) 1-30 fps\n"
            "       2   2592x1944 (4:3)  1-15 fps\n"
            "       3   2592x1944 (4:3)  0.1666-1 fps\n"
            "       4   1296x972  (4:3)  1-42 fps, 2x2 binning\n"
            "       5   1296x730  (16:9) 1-49 fps, 2x2 binning\n"
            "       6   640x480   (4:3)  42.1-60 fps, 2x2 binning plus skip\n"
            "       7   640x480   (4:3)  60.1-90 fps, 2x2 binning plus skip\n"
            );

    exit(exit_failure);
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
            // the replace option is set to 0, so that anything set in the environment
            // is an override.
            if (setenv(opt->env_key, opt->default_value, 0) < 0)
                err(exit_failure, "error setting %s to %s", opt->env_key, opt->default_value);
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
                warnx("unknown option '%s'", key);
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
                warnx("unknown option '%s'", key);
                help(0, 0, true);
            }

            if (i < argc - 1)
                value = argv[++i];
            else
                value = "on"; // if no value, then this is a boolean argument, so set to on
        } else {
            warnx("unexpected parameter '%s'", argv[i]);
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
    // trim everything after a comment
    char *comment = strchr(str, '#');
    if (comment)
        *comment = '\0';

    // trim whitespace off the beginning and end
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
        // ignore the bad option
        free(str);
        return;
    }

    opt->set(opt, value, false);
    if (opt->apply)
        opt->apply(opt, false);

    free(str);
}

static void camera_control_callback(mmal_port_t *port, mmal_buffer_header_t *buffer)
{
    // this is called from another thread. don't access any data here.
    unused(port);

    if (buffer->cmd == mmal_event_error)
       errx(exit_failure, "no data received from sensor. check all connections, including the sunny one on the camera board");
    else if(buffer->cmd != mmal_event_parameter_changed)
        errx(exit_failure, "camera sent invalid data: 0x%08x", buffer->cmd);

    mmal_buffer_header_release(buffer);
}

static void output_jpeg(const char *buf, int len)
{
    struct iovec iovs[2];
    uint32_t len32 = htonl(len);
    iovs[0].iov_base = &len32;
    iovs[0].iov_len = sizeof(int32_t);
    iovs[1].iov_base = (char *) buf; // silence warning
    iovs[1].iov_len = len;
    ssize_t count = writev(stdout_fileno, iovs, 2);
    if (count < 0)
        err(exit_failure, "error writing to stdout");
    else if (count != (ssize_t) (iovs[0].iov_len + iovs[1].iov_len))
        warnx("unexpected truncation of jpeg when writing to stdout");
}

static void recycle_jpegencoder_buffer(mmal_port_t *port, mmal_buffer_header_t *buffer)
{
    mmal_buffer_header_release(buffer);

    if (port->is_enabled) {
        mmal_buffer_header_t *new_buffer;

        if (!(new_buffer = mmal_queue_get(state.pool_jpegencoder->queue)) ||
             mmal_port_send_buffer(port, new_buffer) != mmal_success)
            errx(exit_failure, "could not send buffers to port");
    }
}

static void jpegencoder_buffer_callback_impl()
{
    void *msg[2];
    if (read(state.mmal_callback_pipe[0], msg, sizeof(msg)) != sizeof(msg))
        err(exit_failure, "read from internal pipe broke");

    mmal_port_t *port = (mmal_port_t *) msg[0];
    mmal_buffer_header_t *buffer = (mmal_buffer_header_t *) msg[1];

    mmal_buffer_header_mem_lock(buffer);

    if (state.socket_buffer_ix == 0 &&
            (buffer->flags & mmal_buffer_header_flag_frame_end) &&
            buffer->length <= max_data_buffer_size) {
        // easy case: jpeg all in one buffer
        output_jpeg((const char *) buffer->data, buffer->length);
    } else {
        // hard case: assemble jpeg
        if (state.socket_buffer_ix + buffer->length > max_data_buffer_size) {
            if (buffer->flags & mmal_buffer_header_flag_frame_end) {
                state.socket_buffer_ix = 0;
            } else if (state.socket_buffer_ix != max_data_buffer_size) {
                // warn when frame crosses threshold
                warnx("frame too large (%d bytes). dropping. adjust max_data_buffer_size.", state.socket_buffer_ix + buffer->length);
                state.socket_buffer_ix = max_data_buffer_size;
            }
        } else {
            memcpy(&state.socket_buffer[state.socket_buffer_ix], buffer->data, buffer->length);
            state.socket_buffer_ix += buffer->length;
            if (buffer->flags & mmal_buffer_header_flag_frame_end) {
                output_jpeg(state.socket_buffer, state.socket_buffer_ix);
                state.socket_buffer_ix = 0;
            }
        }
    }

    mmal_buffer_header_mem_unlock(buffer);

    //cam_set_annotation();

    recycle_jpegencoder_buffer(port, buffer);
}

static void jpegencoder_buffer_callback(mmal_port_t *port, mmal_buffer_header_t *buffer)
{
    // if the buffer contains something, notify our main thread to process it.
    // if not, recycle it immediately.
    if (buffer->length) {
        void *msg[2];
        msg[0] = port;
        msg[1] = buffer;
        if (write(state.mmal_callback_pipe[1], msg, sizeof(msg)) != sizeof(msg))
            err(exit_failure, "write to internal pipe broke");
    } else {
        recycle_jpegencoder_buffer(port, buffer);
    }
}

static void discover_sensors(mmal_parameter_camera_info_t *camera_info)
{
    mmal_component_t *camera_component;

    // try to get the camera name and maximum supported resolution
    mmal_status_t status = mmal_component_create(mmal_component_default_camera_info, &camera_component);
    if (status != mmal_success)
        errx(exit_failure, "failed to create camera_info component");

    camera_info->hdr.id = mmal_parameter_camera_info;
    camera_info->hdr.size = sizeof(mmal_parameter_camera_info_t)-4;  // deliberately undersize to check firmware version
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
    // Create the file descriptors for getting back to the main thread
    // from the MMAL callbacks.
    if (pipe(state.mmal_callback_pipe) < 0)
        err(EXIT_FAILURE, "pipe");

    // Only the first camera is currently supported.
    int imager_width = state.sensor_info.cameras[0].max_width;
    int imager_height = state.sensor_info.cameras[0].max_width;

    //
    // create camera
    //
    if (mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &state.camera) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create camera");
    if (mmal_port_enable(state.camera->control, camera_control_callback) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable camera control port");

    int fps256 = lrint(256.0 * strtod(getenv(RASPIJPGS_FPS), 0));

    parse_requested_dimensions(&state.width, &state.height);

    // TODO: The fact that this seems to work implies that there's a scaler
    //       in the camera block and we don't need a resizer??
    int video_width = state.width;
    int video_height = state.height;

    MMAL_PARAMETER_CAMERA_CONFIG_T cam_config = {
        {MMAL_PARAMETER_CAMERA_CONFIG, sizeof(cam_config)},
        .max_stills_w = 0,
        .max_stills_h = 0,
        .stills_yuv422 = 0,
        .one_shot_stills = 0,
        .max_preview_video_w = imager_width,
        .max_preview_video_h = imager_height,
        .num_preview_video_frames = 3,
        .stills_capture_circular_buffer_height = 0,
        .fast_preview_resume = 0,
        .use_stc_timestamp = MMAL_PARAM_TIMESTAMP_MODE_RESET_STC
    };
    if (mmal_port_parameter_set(state.camera->control, &cam_config.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Error configuring camera");

    MMAL_ES_FORMAT_T *format = state.camera->output[0]->format;
    format->es->video.width = video_width;
    format->es->video.height = video_height;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = video_width;
    format->es->video.crop.height = video_height;
    format->es->video.frame_rate.num = fps256;
    format->es->video.frame_rate.den = 256;
    if (mmal_port_format_commit(state.camera->output[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set preview format");

    if (mmal_component_enable(state.camera) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable camera");

    //
    // create jpeg-encoder
    //
    MMAL_STATUS_T status = mmal_component_create(MMAL_COMPONENT_DEFAULT_IMAGE_ENCODER, &state.jpegencoder);
    if (status != MMAL_SUCCESS && status != MMAL_ENOSYS)
        errx(EXIT_FAILURE, "Could not create image encoder");

    state.jpegencoder->output[0]->format->encoding = MMAL_ENCODING_JPEG;
    state.jpegencoder->output[0]->buffer_size = state.jpegencoder->output[0]->buffer_size_recommended;
    if (state.jpegencoder->output[0]->buffer_size < state.jpegencoder->output[0]->buffer_size_min)
        state.jpegencoder->output[0]->buffer_size = state.jpegencoder->output[0]->buffer_size_min;
    state.jpegencoder->output[0]->buffer_num = state.jpegencoder->output[0]->buffer_num_recommended;
    if(state.jpegencoder->output[0]->buffer_num < state.jpegencoder->output[0]->buffer_num_min)
        state.jpegencoder->output[0]->buffer_num = state.jpegencoder->output[0]->buffer_num_min;
    if (mmal_port_format_commit(state.jpegencoder->output[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set image format");

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
    // create image-resizer
    //
    status = mmal_component_create("vc.ril.resize", &state.resizer);
    if (status != MMAL_SUCCESS && status != MMAL_ENOSYS)
        errx(EXIT_FAILURE, "Could not create image resizer");

    format = state.resizer->output[0]->format;
    format->es->video.width = state.width;
    format->es->video.height = state.height;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = state.width;
    format->es->video.crop.height = state.height;
    format->es->video.frame_rate.num = fps256;
    format->es->video.frame_rate.den = 256;
    if (mmal_port_format_commit(state.resizer->output[0]) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set image resizer output");

    if (mmal_component_enable(state.resizer) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable image resizer");

    //
    // connect
    //
    if (mmal_connection_create(&state.con_cam_res, state.camera->output[0], state.resizer->input[0], MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create connection camera -> resizer");
    if (mmal_connection_enable(state.con_cam_res) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable connection camera -> resizer");

    if (mmal_connection_create(&state.con_res_jpeg, state.resizer->output[0], state.jpegencoder->input[0], MMAL_CONNECTION_FLAG_TUNNELLING | MMAL_CONNECTION_FLAG_ALLOCATION_ON_INPUT) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create connection resizer -> encoder");
    if (mmal_connection_enable(state.con_res_jpeg) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable connection resizer -> encoder");

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

    //
    // Set all parameters
    //
    apply_parameters(true);
}

void stop_all()
{
    mmal_port_disable(state.jpegencoder->output[0]);
    mmal_connection_destroy(state.con_cam_res);
    mmal_connection_destroy(state.con_res_jpeg);
    mmal_port_pool_destroy(state.jpegencoder->output[0], state.pool_jpegencoder);
    mmal_component_disable(state.jpegencoder);
    mmal_component_disable(state.camera);
    mmal_component_destroy(state.jpegencoder);
    mmal_component_destroy(state.camera);

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
        struct pollfd fds[3];
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
                jpegencoder_buffer_callback_impl();
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
