/*
Copyright (c) 2013, Broadcom Europe Ltd
Copyright (c) 2013, Silvan Melchior
Copyright (c) 2013, James Hughes
Copyright (c) 2015, Frank Hunleth
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
    * Redistributions of source code must retain the above copyright
      notice, this list of conditions and the following disclaimer.
    * Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions and the following disclaimer in the
      documentation and/or other materials provided with the distribution.
    * Neither the name of the copyright holder nor the
      names of its contributors may be used to endorse or promote products
      derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

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

#define MAX_CLIENTS                 8
#define MAX_DATA_BUFFER_SIZE        131072
#define MAX_REQUEST_BUFFER_SIZE     4096

#define UNUSED(expr) do { (void)(expr); } while (0)

// Environment config keys
#define RASPIJPGS_WIDTH             "RASPIJPGS_WIDTH"
#define RASPIJPGS_HEIGHT            "RASPIJPGS_HEIGHT"
#define RASPIJPGS_FPS		    "RASPIJPGS_FPS"
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
#define RASPIJPGS_SOCKET            "RASPIJPGS_SOCKET"
#define RASPIJPGS_OUTPUT            "RASPIJPGS_OUTPUT"
#define RASPIJPGS_COUNT             "RASPIJPGS_COUNT"
#define RASPIJPGS_LOCKFILE          "RASPIJPGS_LOCKFILE"

// Globals

enum config_context {
    config_context_parse_cmdline,
    config_context_file,
    config_context_server_start,
    config_context_client_request
};

struct raspijpgs_state
{
    // Settings
    char *lock_filename;
    char *config_filename;
    char *sendlist;
    int count;

    // Commandline options to only run in client or server mode
    int user_wants_server;
    int user_wants_client;

    // 1 if we're a server; 0 if we're a client
    int is_server;

    // Communication
    int socket_fd;
    char *socket_buffer;
    int socket_buffer_ix;
    char *stdin_buffer;
    int stdin_buffer_ix;

    struct sockaddr_un server_addr;
    struct sockaddr_un client_addrs[MAX_CLIENTS];

    // Output
    int no_output;
    int output_fd;
    char *output_filename;
    char *output_tmp_filename;
    char *framing;
    int http_ready_for_images;

    // MMAL resources
    MMAL_COMPONENT_T *camera;
    MMAL_COMPONENT_T *jpegencoder;
    MMAL_COMPONENT_T *resizer;
    MMAL_CONNECTION_T *con_cam_res;
    MMAL_CONNECTION_T *con_res_jpeg;
    MMAL_POOL_T *pool_jpegencoder;

    // MMAL callback -> main loop
    int mmal_callback_pipe[2];
};

static struct raspijpgs_state state = {0};

struct raspi_config_opt
{
    const char *long_option;
    const char *short_option;
    const char *env_key;
    const char *help;

    const char *default_value;

    // Record the value (called as options are set)
    // Set replace=0 to only set the value if it hasn't been set already.
    void (*set)(const struct raspi_config_opt *, const char *value, enum config_context context);

    // Apply the option (called on every option)
    void (*apply)(const struct raspi_config_opt *, enum config_context context);
};
static struct raspi_config_opt opts[];

static const char *http_ok_response = "HTTP/1.1 200 OK\r\n" \
                                      "Server: raspijpgs\r\n";
static const char *http_500_response = "HTTP/1.1 500 Internal Server Error\r\n";
static const char *http_404_response = "HTTP/1.1 404 Not Found\r\n";
static const char *http_index_html_response = "Content-Type: text/html; charset=UTF-8\r\n" \
                                              "Connection: close\r\n" \
                                              "\r\n" \
                                              "<!DOCTYPE html>\r\n" \
                                              "<html>\r\n" \
                                              "<head>\r\n" \
                                              "  <title>raspijpg</title>\r\n" \
                                              "</head>\r\n" \
                                              "<body>\r\n" \
                                              "  <img src=\"/video\"/>\r\n" \
                                              "</body>\r\n" \
                                              "</html>\r\n";
static const char *mime_header = "MIME-Version: 1.0\r\n" \
                                 "content-type: multipart/x-mixed-replace;boundary=--jpegboundary\r\n";
static const char *mime_boundary = "\r\n--jpegboundary\r\n";
static const char *mime_multipart_header_format = "Content-Type: image/jpeg\r\n" \
                                                  "Content-Length: %d\r\n\r\n";

static void default_set(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    if (!opt->env_key)
        return;

    // setenv's 3rd parameter is whether to replace a value if it already
    // exists. Sets are done in the order of Environment, commandline, file.
    // Since the file should be the lowest priority, set it to not replace
    // here.
    int replace = (context != config_context_file);

    if (value) {
        if (setenv(opt->env_key, value, replace) < 0)
            err(EXIT_FAILURE, "Error setting %s to %s", opt->env_key, opt->default_value);
    } else {
        if (replace && (unsetenv(opt->env_key) < 0))
            err(EXIT_FAILURE, "Error unsetting %s", opt->env_key);
    }
}

static void setstring(char **left, const char *right)
{
    if (*left)
        free(*left);
    *left = strdup(right);
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

static void config_set(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    UNUSED(opt); UNUSED(context);
    setstring(&state.config_filename, value);
}
static void framing_set(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    UNUSED(opt); UNUSED(context);
    setstring(&state.framing, value);
}
static void send_set(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    UNUSED(opt);

    // If a client is telling us to send, then ignore it
    // since it doesn't make sense.
    if (context == config_context_client_request)
        return;

    // Send lists are intended to look like config files for ease of parsing
    const char *equals = strchr(value, '=');
    char *key = equals ? strndup(value, equals - value) : strdup(value);
    const struct raspi_config_opt *o;
    for (o = opts; o->long_option; o++) {
        if (strcmp(key, o->long_option) == 0)
            break;
    }
    if (!o->long_option)
        errx(EXIT_FAILURE, "Unexpected key '%s' used in --send. Check help", key);
    free(key);

    if (state.sendlist) {
        char *old_sendlist = state.sendlist;
        if (asprintf(&state.sendlist, "%s\n%s", old_sendlist, value) < 0)
            err(EXIT_FAILURE, "asprintf");
        free(old_sendlist);
    } else
        state.sendlist = strdup(value);
}
static void quit_set(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    UNUSED(opt); UNUSED(value); UNUSED(context);
    state.count = 0;
}
static void server_set(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    UNUSED(opt); UNUSED(value); UNUSED(context);
    state.user_wants_server = 1;
}
static void client_set(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    UNUSED(opt); UNUSED(value); UNUSED(context);
    state.user_wants_client = 1;
}

static void help(const struct raspi_config_opt *opt, const char *value, enum config_context context);

static void width_apply(const struct raspi_config_opt *opt, enum config_context context) { UNUSED(opt); }
static void height_apply(const struct raspi_config_opt *opt, enum config_context context) { UNUSED(opt); }
static void annotation_apply(const struct raspi_config_opt *opt, enum config_context context) { UNUSED(opt); }
static void anno_background_apply(const struct raspi_config_opt *opt, enum config_context context) { UNUSED(opt); }
static void rational_param_apply(int mmal_param, const struct raspi_config_opt *opt, enum config_context context)
{
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    if (value > 100) {
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "%s must be between 0 and 100", opt->long_option);
        else
            return;
    }
    MMAL_RATIONAL_T mmal_value = {value, 100};
    MMAL_STATUS_T status = mmal_port_parameter_set_rational(state.camera->control, mmal_param, mmal_value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}

static void sharpness_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    rational_param_apply(MMAL_PARAMETER_SHARPNESS, opt, context);
}
static void contrast_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    rational_param_apply(MMAL_PARAMETER_CONTRAST, opt, context);
}
static void brightness_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    rational_param_apply(MMAL_PARAMETER_BRIGHTNESS, opt, context);
}
static void saturation_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    rational_param_apply(MMAL_PARAMETER_SATURATION, opt, context);
}

static void ISO_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    unsigned int value = strtoul(getenv(opt->env_key), 0, 0);
    MMAL_STATUS_T status = mmal_port_parameter_set_uint32(state.camera->control, MMAL_PARAMETER_ISO, value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void vstab_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    unsigned int value = (strcmp(getenv(opt->env_key), "on") == 0);
    MMAL_STATUS_T status = mmal_port_parameter_set_uint32(state.camera->control, MMAL_PARAMETER_VIDEO_STABILISATION, value);
    if(status != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void ev_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}
static void exposure_apply(const struct raspi_config_opt *opt, enum config_context context)
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
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }

    MMAL_PARAMETER_EXPOSUREMODE_T param = {{MMAL_PARAMETER_EXPOSURE_MODE,sizeof(param)}, mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void awb_apply(const struct raspi_config_opt *opt, enum config_context context)
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
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }
    MMAL_PARAMETER_AWBMODE_T param = {{MMAL_PARAMETER_AWB_MODE,sizeof(param)}, awb_mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void imxfx_apply(const struct raspi_config_opt *opt, enum config_context context)
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
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }
    MMAL_PARAMETER_IMAGEFX_T param = {{MMAL_PARAMETER_IMAGE_EFFECT,sizeof(param)}, imageFX};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void colfx_apply(const struct raspi_config_opt *opt, enum config_context context)
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
static void metering_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    MMAL_PARAM_EXPOSUREMETERINGMODE_T m_mode;
    const char *str = getenv(opt->env_key);
    if(strcmp(str, "average") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_AVERAGE;
    else if(strcmp(str, "spot") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_SPOT;
    else if(strcmp(str, "backlit") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_BACKLIT;
    else if(strcmp(str, "matrix") == 0) m_mode = MMAL_PARAM_EXPOSUREMETERINGMODE_MATRIX;
    else {
        if (context == config_context_server_start)
            errx(EXIT_FAILURE, "Invalid %s", opt->long_option);
        else
            return;
    }
    MMAL_PARAMETER_EXPOSUREMETERINGMODE_T param = {{MMAL_PARAMETER_EXP_METERING_MODE,sizeof(param)}, m_mode};
    if (mmal_port_parameter_set(state.camera->control, &param.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void rotation_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    int value = strtol(getenv(opt->env_key), NULL, 0);
    if (mmal_port_parameter_set_int32(state.camera->output[0], MMAL_PARAMETER_ROTATION, value) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void flip_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);

    MMAL_PARAMETER_MIRROR_T mirror = {{MMAL_PARAMETER_MIRROR, sizeof(MMAL_PARAMETER_MIRROR_T)}, MMAL_PARAM_MIRROR_NONE};
    if (strcmp(getenv(RASPIJPGS_HFLIP), "on") == 0)
        mirror.value = MMAL_PARAM_MIRROR_HORIZONTAL;
    if (strcmp(getenv(RASPIJPGS_VFLIP), "on") == 0)
        mirror.value = (mirror.value == MMAL_PARAM_MIRROR_HORIZONTAL ? MMAL_PARAM_MIRROR_BOTH : MMAL_PARAM_MIRROR_VERTICAL);

    if (mmal_port_parameter_set(state.camera->output[0], &mirror.hdr) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void sensor_mode_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}
static void roi_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}
static void shutter_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    int value = strtoul(getenv(opt->env_key), NULL, 0);
    if (mmal_port_parameter_set_uint32(state.camera->control, MMAL_PARAMETER_SHUTTER_SPEED, value) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s", opt->long_option);
}
static void quality_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    UNUSED(context);
    int value = strtoul(getenv(opt->env_key), NULL, 0);
    value = constrain(0, value, 100);
    if (mmal_port_parameter_set_uint32(state.jpegencoder->output[0], MMAL_PARAMETER_JPEG_Q_FACTOR, value) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not set %s to %d", opt->long_option, value);
}
static void restart_interval_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}
static void fps_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    // TODO
    UNUSED(opt);
    UNUSED(context);
}
static void count_apply(const struct raspi_config_opt *opt, enum config_context context)
{
    state.count = strtol(getenv(opt->env_key), NULL, 0);
}

static struct raspi_config_opt opts[] =
{
    // long_option  short   env_key                  help                                                    default
    {"width",       "w",    RASPIJPGS_WIDTH,        "Set image width <size>",                               "320",      default_set, width_apply},
    {"height",      "h",    RASPIJPGS_HEIGHT,       "Set image height <size> (0 = calculate from width",    "0",        default_set, height_apply},
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
    {"socket",      0,      RASPIJPGS_SOCKET,       "Specify the socket filename for communication",        "/tmp/raspijpgs_socket", default_set, 0},
    {"output",      "o",    RASPIJPGS_OUTPUT,       "Specify an output filename or '-' for stdout",         "",         default_set, 0},
    {"count",       0,      RASPIJPGS_COUNT,        "How many frames to capture before quiting (-1 = no limit)", "-1",  default_set, count_apply},
    {"lockfile",    0,      RASPIJPGS_LOCKFILE,     "Specify a lock filename to prevent multiple runs",     "/tmp/raspijpgs_lock", default_set, 0},

    // options that can't be overridden using environment variables
    {"config",      "c",    0,                       "Specify a config file to read for options",            0,          config_set, 0},
    {"framing",     "fr",   0,                       "Specify the output framing (cat, mime, http, header, replace)", "cat",   framing_set, 0},
    {"send",        0,      0,                       "Send this parameter on the server (e.g. --send shutter=1000)", 0,  send_set, 0},
    {"server",      0,      0,                       "Run as a server",                                      0,          server_set, 0},
    {"client",      0,      0,                       "Run as a client",                                      0,          client_set, 0},
    {"quit",        0,      0,                       "Tell a server to quit",                                0,          quit_set, 0},
    {"help",        "h",    0,                       "Print this help message",                              0,          help, 0},
    {0,             0,      0,                       0,                                                      0,          0,           0}
};

static void help(const struct raspi_config_opt *opt, const char *value, enum config_context context)
{
    UNUSED(opt); UNUSED(value);

    // Don't provide help if this is a request from a connected client
    // since the user won't see it.
    if (context == config_context_client_request)
        return;

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

    // It make sense to exit in all non-client request contexts
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

    // TODO: To expose framing to the environment or not????
    if (!state.framing)
        state.framing = "cat";
}

static void apply_parameters(enum config_context context)
{
    const struct raspi_config_opt *opt;
    for (opt = opts; opt->long_option; opt++) {
        if (opt->apply)
            opt->apply(opt, context);
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
                help(0, 0, 0);
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
                help(0, 0, 0);
            }

            if (i < argc - 1)
                value = argv[++i];
            else
                value = "on"; // if no value, then this is a boolean argument, so set to on
        } else {
            warnx("Unexpected parameter '%s'", argv[i]);
            help(0, 0, 0);
        }

        if (opt)
            opt->set(opt, value, config_context_parse_cmdline);
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

static void parse_config_line(const char *line, enum config_context context)
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
        // Error out if we're parsing a file; otherwise ignore the bad option
        if (context == config_context_file)
            errx(EXIT_FAILURE, "Unknown option '%s' in file '%s'", key, state.config_filename);
        else {
            free(str);
            return;
        }
    }

    switch (context) {
    case config_context_file:
        opt->set(opt, value, context);
        break;

    case config_context_client_request:
        opt->set(opt, value, context);
        if (opt->apply)
            opt->apply(opt, context);
        break;

    default:
        // Ignore
        break;
    }
    free(str);
}

static void load_config_file()
{
    if (!state.config_filename)
        return;

    FILE *fp = fopen(state.config_filename, "r");
    if (!fp)
        err(EXIT_FAILURE, "Cannot open '%s'", state.config_filename);

    char line[128];
    while (fgets(line, sizeof(line), fp))
        parse_config_line(line, config_context_file);

    fclose(fp);
}

static void remove_server_lock()
{
    unlink(state.lock_filename);
}

// Return 0 if a server's running; 1 if we are the server now
static int acquire_server_lock()
{
    // This lock isn't meant to protect against race conditions. It's just meant
    // to provide a better error message if the user accidentally starts up a
    // second server.
    const char *lockfile = getenv(RASPIJPGS_LOCKFILE);
    FILE *fp = fopen(lockfile, "r");
    if (fp) {
        char server_pid_str[16];
        pid_t server_pid;

        // Check that there's a process behind the pid in the lock file.
        if (fgets(server_pid_str, sizeof(server_pid_str), fp) != NULL &&
            (server_pid = strtoul(server_pid_str, NULL, 10)) != 0 &&
            server_pid > 0 &&
            kill(server_pid, 0) == 0) {
            // Yes, so we can't be a server.
            fclose(fp);
            return 0;
        }

        fclose(fp);
    }

    fp = fopen(lockfile, "w");
    if (!fp)
        err(EXIT_FAILURE, "Can't open lock file '%s'", lockfile);

    if (fprintf(fp, "%d", getpid()) < 0)
        err(EXIT_FAILURE, "Can't write to '%s'", lockfile);
    fclose(fp);

    // Record the name of the lock file that we used so that it
    // can be removed automatically on termination.
    state.lock_filename = strdup(lockfile);
    atexit(remove_server_lock);

    return 1;
}

static void add_client(const struct sockaddr_un *client_addr)
{
    int i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (state.client_addrs[i].sun_family == 0) {
            state.client_addrs[i] = *client_addr;
            return;
        }
    }
    warnx("Reached max number of clients (%d)", MAX_CLIENTS);
}

static void term_sighandler(int signum)
{
    UNUSED(signum);
    // Capture no more frames.
    state.count = 0;
}

static void cleanup_server()
{
    close(state.socket_fd);
    unlink(state.server_addr.sun_path);
}

static void camera_control_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    // This is called from another thread. Don't access any data here.
    UNUSED(port);

    if (buffer->cmd == MMAL_EVENT_ERROR)
       errx(EXIT_FAILURE, "No data received from sensor. Check all connections, including the Sunny one on the camera board");
    else if(buffer->cmd != MMAL_EVENT_PARAMETER_CHANGED)
        errx(EXIT_FAILURE, "Camera sent invalid data: 0x%08x", buffer->cmd);

    mmal_buffer_header_release(buffer);
}

static void output_jpeg(const char *buf, int len)
{
    if (state.no_output)
        return;

    if (strcmp(state.framing, "mime") == 0 ||
	(state.http_ready_for_images && (strcmp(state.framing, "http") == 0))) {
        char multipart_header[256];
        int multipart_header_len =
            sprintf(multipart_header, mime_multipart_header_format, len);

        struct iovec iovs[3];
        iovs[0].iov_base = multipart_header;
        iovs[0].iov_len = multipart_header_len;
        iovs[1].iov_base = (char *) buf; // silence warning
        iovs[1].iov_len = len;
        iovs[2].iov_base = (char *) mime_boundary; // silence warning
        iovs[2].iov_len = strlen(mime_boundary);
        int count = writev(state.output_fd, iovs, 3);
        if (count < 0)
            err(EXIT_FAILURE, "Error writing to %s", state.output_filename);
        else if (count != iovs[0].iov_len + iovs[1].iov_len + iovs[2].iov_len)
            warnx("Unexpected truncation of JPEG when writing to %s", state.output_filename);
    } else if (strcmp(state.framing, "header") == 0) {
        struct iovec iovs[2];
        uint32_t len32 = htonl(len);
        iovs[0].iov_base = &len32;
        iovs[0].iov_len = sizeof(int32_t);
        iovs[1].iov_base = (char *) buf; // silence warning
        iovs[1].iov_len = len;
        int count = writev(state.output_fd, iovs, 2);
        if (count < 0)
            err(EXIT_FAILURE, "Error writing to %s", state.output_filename);
        else if (count != iovs[0].iov_len + iovs[1].iov_len)
            warnx("Unexpected truncation of JPEG when writing to %s", state.output_filename);
    } else if (strcmp(state.framing, "replace") == 0) {
        // replace the output file with the latest image
        int fd = open(state.output_tmp_filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0)
            err(EXIT_FAILURE, "Can't create %s", state.output_tmp_filename);
        int count = write(fd, buf, len);
        if (count < 0)
            err(EXIT_FAILURE, "Error writing to %s", state.output_tmp_filename);
        else if (count != len)
            warnx("Unexpected truncation of JPEG when writing to %s", state.output_tmp_filename);
        close(fd);
        if (rename(state.output_tmp_filename, state.output_filename) < 0)
            err(EXIT_FAILURE, "Can't rename %s to %s", state.output_tmp_filename, state.output_filename);
    } else if (strcmp(state.framing, "cat") == 0) {
        // cat (aka concatenate)
        // TODO - Loop to make sure that everything is written.
        int count = write(state.output_fd, buf, len);
        if (count < 0)
            err(EXIT_FAILURE, "Error writing to %s", state.output_filename);
        else if (count != len)
            warnx("Unexpected truncation of JPEG when writing to %s", state.output_filename);
    }
}

static void distribute_jpeg(const char *buf, size_t len)
{
    // Send the JPEG to all of our clients
    size_t i;
    for (i = 0; i < MAX_CLIENTS; i++) {
        if (state.client_addrs[i].sun_family) {
            if (sendto(state.socket_fd, buf, len, 0, &state.client_addrs[i], sizeof(struct sockaddr_un)) < 0) {
                // If failure, then remove client.
                state.client_addrs[i].sun_family = 0;
            }
        }
    }

    // Handle it ourselves
    output_jpeg(buf, len);
}

static void recycle_jpegencoder_buffer(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    mmal_buffer_header_release(buffer);

    if (port->is_enabled) {
        MMAL_BUFFER_HEADER_T *new_buffer;

        if (!(new_buffer = mmal_queue_get(state.pool_jpegencoder->queue)) ||
             mmal_port_send_buffer(port, new_buffer) != MMAL_SUCCESS)
            errx(EXIT_FAILURE, "Could not send buffers to port");
    }
}

static void jpegencoder_buffer_callback_impl()
{
    void *msg[2];
    if (read(state.mmal_callback_pipe[0], msg, sizeof(msg)) != sizeof(msg))
        err(EXIT_FAILURE, "read from internal pipe broke");

    MMAL_PORT_T *port = (MMAL_PORT_T *) msg[0];
    MMAL_BUFFER_HEADER_T *buffer = (MMAL_BUFFER_HEADER_T *) msg[1];

    mmal_buffer_header_mem_lock(buffer);

    if (state.socket_buffer_ix == 0 &&
            (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) &&
            buffer->length <= MAX_DATA_BUFFER_SIZE) {
        // Easy case: JPEG all in one buffer
        distribute_jpeg((const char *) buffer->data, buffer->length);
    } else {
        // Hard case: assemble JPEG
        if (state.socket_buffer_ix + buffer->length > MAX_DATA_BUFFER_SIZE) {
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) {
                state.socket_buffer_ix = 0;
            } else if (state.socket_buffer_ix != MAX_DATA_BUFFER_SIZE) {
                // Warn when frame crosses threshold
                warnx("Frame too large (%d bytes). Dropping. Adjust MAX_DATA_BUFFER_SIZE.", state.socket_buffer_ix + buffer->length);
                state.socket_buffer_ix = MAX_DATA_BUFFER_SIZE;
            }
        } else {
            memcpy(&state.socket_buffer[state.socket_buffer_ix], buffer->data, buffer->length);
            state.socket_buffer_ix += buffer->length;
            if (buffer->flags & MMAL_BUFFER_HEADER_FLAG_FRAME_END) {
                distribute_jpeg(state.socket_buffer, state.socket_buffer_ix);
                state.socket_buffer_ix = 0;
            }
        }
    }

    mmal_buffer_header_mem_unlock(buffer);

    if (state.count >= 0)
        state.count--;

    //cam_set_annotation();

    recycle_jpegencoder_buffer(port, buffer);
}

static void jpegencoder_buffer_callback(MMAL_PORT_T *port, MMAL_BUFFER_HEADER_T *buffer)
{
    // If the buffer contains something, notify our main thread to process it.
    // If not, recycle it.
    if (buffer->length) {
        void *msg[2];
        msg[0] = port;
        msg[1] = buffer;
        if (write(state.mmal_callback_pipe[1], msg, sizeof(msg)) != sizeof(msg))
            err(EXIT_FAILURE, "write to internal pipe broke");
    } else {
        recycle_jpegencoder_buffer(port, buffer);
    }
}

static void find_sensor_dimensions(int camera_ix, int *imager_width, int *imager_height)
{
    MMAL_COMPONENT_T *camera_info;

    // Default to OV5647 full resolution
    *imager_width = 2592;
    *imager_height = 1944;

    // Try to get the camera name and maximum supported resolution
    MMAL_STATUS_T status = mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA_INFO, &camera_info);
    if (status == MMAL_SUCCESS) {
        MMAL_PARAMETER_CAMERA_INFO_T param;
        param.hdr.id = MMAL_PARAMETER_CAMERA_INFO;
        param.hdr.size = sizeof(param)-4;  // Deliberately undersize to check firmware version
        status = mmal_port_parameter_get(camera_info->control, &param.hdr);

        if (status != MMAL_SUCCESS) {
            // Running on newer firmware
            param.hdr.size = sizeof(param);
            status = mmal_port_parameter_get(camera_info->control, &param.hdr);
            if (status == MMAL_SUCCESS && param.num_cameras > camera_ix) {
                // Take the parameters from the first camera listed.
                *imager_width = param.cameras[camera_ix].max_width;
                *imager_height = param.cameras[camera_ix].max_height;
            } else
                warnx("Cannot read camera info, keeping the defaults for OV5647");
        } else {
            // Older firmware
            // Nothing to do here, keep the defaults for OV5647
        }

        mmal_component_destroy(camera_info);
    } else {
        warnx("Failed to create camera_info component");
    }
}

void start_all()
{
    // Find out which Raspberry Camera is attached for the defaults
    int imager_width;
    int imager_height;
    find_sensor_dimensions(0, &imager_width, &imager_height);

    //
    // create camera
    //
    if (mmal_component_create(MMAL_COMPONENT_DEFAULT_CAMERA, &state.camera) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not create camera");
    if (mmal_port_enable(state.camera->control, camera_control_callback) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable camera control port");

    int fps100 = lrint(100.0 * strtod(getenv(RASPIJPGS_FPS), 0));
    int width = strtol(getenv(RASPIJPGS_WIDTH), 0, 0);
    if (width <= 0)
        width = 320;
    else if (width > imager_width)
        width = imager_width;
    width = width & ~0xf; // Force to multiple of 16 for JPEG encoder
    int height = strtol(getenv(RASPIJPGS_HEIGHT), 0, 0);
    if (height <= 0)
        height = imager_height * width / imager_width; // Default to the camera's aspect ratio
    else if (height > imager_height)
        height = imager_height;
    height = height & ~0xf;

    // TODO: The fact that this seems to work implies that there's a scaler
    //       in the camera block and we don't need a resizer??
    int video_width = width;
    int video_height = height;

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
    format->es->video.frame_rate.num = fps100;
    format->es->video.frame_rate.den = 100;
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
    format->es->video.width = width;
    format->es->video.height = height;
    format->es->video.crop.x = 0;
    format->es->video.crop.y = 0;
    format->es->video.crop.width = width;
    format->es->video.crop.height = height;
    format->es->video.frame_rate.num = fps100;
    format->es->video.frame_rate.den = 1;
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
}

static void parse_config_lines(char *lines)
{
    char *line = lines;
    char *line_end;
    do {
        line_end = strchr(line, '\n');
        if (line_end)
            *line_end = '\0';
        parse_config_line(line, config_context_client_request);
        line = line_end + 1;
    } while (line_end);
}

static void server_service_client()
{
    struct sockaddr_un from_addr = {0};
    socklen_t from_addr_len = sizeof(struct sockaddr_un);

    int bytes_received = recvfrom(state.socket_fd,
                                  state.socket_buffer, MAX_DATA_BUFFER_SIZE, 0,
                                  &from_addr, &from_addr_len);
    if (bytes_received < 0) {
        if (errno == EINTR)
            return;

        err(EXIT_FAILURE, "recvfrom");
    }

    add_client(&from_addr);

    state.socket_buffer[bytes_received] = 0;
    parse_config_lines(state.socket_buffer);
}

static void process_stdin_line_framing()
{
    // Process all of the lines that we know are lines.
    // (i.e., they have a '\n' at the end)
    state.stdin_buffer[state.stdin_buffer_ix] = '\0';

    char *line = state.stdin_buffer;
    char *line_end = strchr(line, '\n');
    while (line_end) {
        *line_end = '\0';
        parse_config_line(line, config_context_client_request);
        line = line_end + 1;
        line_end = strchr(line, '\n');
    }

    // Advance the buffer to process any leftovers next time
    int amount_processed = line - state.stdin_buffer;
    state.stdin_buffer_ix -= amount_processed;
    if (amount_processed > 0)
        memmove(state.stdin_buffer, line, state.stdin_buffer_ix);
}

static unsigned int from_uint32_be(const char *buffer)
{
    uint8_t *buf = (uint8_t*) buffer;
    return (buf[0] << 24) | (buf[1] << 16) | (buf[2] << 8) | buf[4];
}

static void process_stdin_header_framing()
{
    // Each packet is length (4 bytes big endian), data
    unsigned int len = 0;
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

static void write_string(const char *str)
{
    if (write(state.output_fd, str, strlen(str)) < 0)
        err(EXIT_FAILURE, "Error writing to %s", state.output_filename);
}

static void write_mime_header()
{
    write_string(mime_header);
    write_string(mime_boundary);
}

static void write_initial_framing()
{
    // Emit the MIME header if in mime framing mode
    if (state.output_fd >= 0 && strcmp(state.framing, "mime") == 0)
        write_mime_header();
}

static void process_stdin_http_framing()
{
    // If the request has already been processed, then ignore everything else.
    if (state.http_ready_for_images)
        state.stdin_buffer_ix = 0;

    state.stdin_buffer[state.stdin_buffer_ix] = '\0';
    char *end_of_request = strstr(state.stdin_buffer, "\r\n\r\n");

    // Return if we haven't received the complete request
    if (!end_of_request)
        return;

    // Respond only to GET requests
    if (memcmp(state.stdin_buffer, "GET", 3) == 0) {
       write_string(http_ok_response);

       if (memcmp(&state.stdin_buffer[4], "/ ", 2) == 0 ||
           memcmp(&state.stdin_buffer[4], "/index.html ", 12) == 0) {
           // Provide the client with a webpage to load the video
           write_string(http_index_html_response);
           state.count = 0;
       } else if (memcmp(&state.stdin_buffer[4], "/video ", 7) == 0) {
           // /video for images.
           write_mime_header();
           state.http_ready_for_images = 1;
       } else {
           write_string(http_404_response);
           state.count = 0;
       }
    } else {
       // If not a GET, then respond with an error and quit.
       write_string(http_500_response);
       state.count = 0;
    }
    state.stdin_buffer_ix = 0;
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
    if (strcmp(state.framing, "header") == 0)
        process_stdin_header_framing();
    else if (strcmp(state.framing, "http") == 0)
        process_stdin_http_framing();
    else
        process_stdin_line_framing();

    return amount_read;
}

static int client_service_stdin()
{
    // Read in everything on stdin and see what gets processed
    int amount_read = read(STDIN_FILENO, &state.stdin_buffer[state.stdin_buffer_ix], MAX_REQUEST_BUFFER_SIZE - state.stdin_buffer_ix - 1);
    if (amount_read < 0)
        err(EXIT_FAILURE, "Error reading stdin");

    // Check if stdin was closed.
    if (amount_read == 0)
        return 0;

    state.stdin_buffer_ix += amount_read;

    // Only HTTP framing is supported on the client from stdin
    // for now.
    if (strcmp(state.framing, "http") == 0)
        process_stdin_http_framing();
    else
        state.stdin_buffer_ix = 0;

    return amount_read;
}


static void server_service_mmal()
{
    jpegencoder_buffer_callback_impl();
}

static void server_loop()
{
    // Check if the user meant to run as a client and the server is dead
    if (state.sendlist)
        errx(EXIT_FAILURE, "Trying to send a message to a raspijpgs server, but one isn't running.");

    // Init hardware
    bcm_host_init();

    // Create the file descriptors for getting back to the main thread
    // from the MMAL callbacks.
    if (pipe(state.mmal_callback_pipe) < 0)
        err(EXIT_FAILURE, "pipe");

    start_all();
    apply_parameters(config_context_server_start);

    // Init communications
    unlink(state.server_addr.sun_path);
    if (bind(state.socket_fd, (const struct sockaddr *) &state.server_addr, sizeof(struct sockaddr_un)) < 0)
        err(EXIT_FAILURE, "Can't create Unix Domain socket at %s", state.server_addr.sun_path);
    atexit(cleanup_server);

    write_initial_framing();

    // Main loop - keep going until we don't want any more JPEGs.
    struct pollfd fds[3];
    int fds_count = 2;
    fds[0].fd = state.mmal_callback_pipe[0];
    fds[0].events = POLLIN;
    fds[1].fd = state.socket_fd;
    fds[1].events = POLLIN;

    if (!isatty(STDIN_FILENO)) {
        // Only allow stdin if not a terminal (e.g., pipe, etc.)
        state.stdin_buffer = (char*) malloc(MAX_REQUEST_BUFFER_SIZE);
        fds[2].fd = STDIN_FILENO;
        fds[2].events = POLLIN;
        fds_count = 3;
    }
    while (state.count != 0) {
        int ready = poll(fds, fds_count, 2000);
        if (ready < 0) {
            if (errno != EINTR)
                err(EXIT_FAILURE, "poll");
        } else if (ready == 0) {
            // Time out - something is wrong that we're not getting MMAL callbacks
            errx(EXIT_FAILURE, "MMAL unresponsive. Video stuck?");
        } else {
            if (fds[0].revents)
                server_service_mmal();
            if (fds[1].revents)
                server_service_client();
            if (fds_count == 3 && fds[2].revents) {
                if (server_service_stdin() <= 0)
                    state.count = 0;
            }
        }
    }

    stop_all();
    close(state.mmal_callback_pipe[0]);
    close(state.mmal_callback_pipe[1]);
    free(state.stdin_buffer);
}

static void cleanup_client()
{
    close(state.socket_fd);
    unlink(state.client_addrs[0].sun_path);
}

static void client_service_server()
{
    struct sockaddr_un from_addr = {0};
    socklen_t from_addr_len = sizeof(struct sockaddr_un);

    int bytes_received = recvfrom(state.socket_fd,
                                  state.socket_buffer, MAX_DATA_BUFFER_SIZE, 0,
                                  &from_addr, &from_addr_len);
    if (bytes_received < 0) {
        if (errno == EINTR)
            return;

        err(EXIT_FAILURE, "recvfrom");
    }
    if (from_addr.sun_family != state.server_addr.sun_family ||
        strcmp(from_addr.sun_path, state.server_addr.sun_path) != 0) {
        warnx("Dropping message from unexpected sender %s. Server should be %s",
              from_addr.sun_path,
              state.server_addr.sun_path);
        return;
    }

    output_jpeg(state.socket_buffer, bytes_received);
    if (state.count > 0)
        state.count--;
}

static void client_loop()
{
    if (state.no_output) {
        // If no output, force the number of jpegs to capture to be 0 (no place to store them)
        setenv(RASPIJPGS_COUNT, "0", 1);

        if (!state.sendlist)
            errx(EXIT_FAILURE, "No sends and no place to store output, so nothing to do.\n"
                               "If you meant to start a server, there's one already running.");
    }
    // Apply client only options - FIXME
    state.count = strtol(getenv(RASPIJPGS_COUNT), NULL, 0);

    // Create a unix domain socket for messages from the server.
    state.client_addrs[0].sun_family = AF_UNIX;
    sprintf(state.client_addrs[0].sun_path, "%s.client.%d", state.server_addr.sun_path, getpid());
    unlink(state.client_addrs[0].sun_path);
    if (bind(state.socket_fd, (const struct sockaddr *) &state.client_addrs[0], sizeof(struct sockaddr_un)) < 0)
        err(EXIT_FAILURE, "Can't create Unix Domain socket at %s", state.client_addrs[0].sun_path);
    atexit(cleanup_client);

    // Send our requests to the server or an empty string to make
    // contact with the server so that it knows about us.
    const char *sendlist = state.sendlist;
    if (!sendlist)
        sendlist = "";
    int tosend = strlen(sendlist);
    int sent = sendto(state.socket_fd, sendlist, tosend, 0,
                      (struct sockaddr *) &state.server_addr,
                      sizeof(struct sockaddr_un));
    if (sent != tosend)
        err(EXIT_FAILURE, "Error communicating with server");

    write_initial_framing();

    // Main loop - keep going until we don't want any more JPEGs.
    struct pollfd fds[2];
    int fds_count = 1;
    fds[0].fd = state.socket_fd;
    fds[0].events = POLLIN;
    if (!isatty(STDIN_FILENO)) {
        // Only allow stdin if not a terminal (e.g., pipe, etc.)
        state.stdin_buffer = (char*) malloc(MAX_REQUEST_BUFFER_SIZE);
        fds[1].fd = STDIN_FILENO;
        fds[1].events = POLLIN;
        fds_count = 2;
    }
    while (state.count != 0) {
        int ready = poll(fds, fds_count, 2000);
        if (ready < 0) {
            if (errno != EINTR)
                err(EXIT_FAILURE, "poll");
        } else if (ready == 0) {
            // If we timeout, then something isn't good with the server.
            // We should be getting frames like crazy.
            errx(EXIT_FAILURE, "Server unresponsive");
        } else {
            if (fds[0].revents)
                client_service_server();
            if (fds_count == 2 && fds[1].revents) {
                // Service stdin, but quit if the user closes it.
                if (client_service_stdin() <= 0)
                    state.count = 0;
            }
        }
    }
}

int main(int argc, char* argv[])
{
    // Parse commandline and config file arguments
    parse_args(argc, argv);
    load_config_file();

    // If anything still isn't set, then fill-in with defaults
    fillin_defaults();

    if (state.user_wants_client && state.user_wants_server)
        errx(EXIT_FAILURE, "Both --client and --server requested");

    // Allocate buffers
    state.socket_buffer = (char *) malloc(MAX_DATA_BUFFER_SIZE);
    if (!state.socket_buffer)
        err(EXIT_FAILURE, "malloc");

    // Create output files if any
    state.output_filename = getenv(RASPIJPGS_OUTPUT);
    if (strcmp(state.output_filename, "-") == 0) {
        // stdout
        state.output_fd = STDOUT_FILENO;

        if (strcmp(state.framing, "replace") == 0)
            errx(EXIT_FAILURE, "Cannot use 'replace' framing with stdout");
    } else if (strlen(state.output_filename) > 0) {
        if (strcmp(state.framing, "replace") == 0) {
            // With 'replace' framing, we create a new file every time and
            // rename it to the output file.
            if (asprintf(&state.output_tmp_filename, "%s.tmp", state.output_filename) < 0)
                err(EXIT_FAILURE, "asprintf");

            state.output_fd = -1;
        } else {
            // For all other framing, we can open the file once
            state.output_fd = open(state.output_filename, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
            if (state.output_fd < 0)
                err(EXIT_FAILURE, "Can't create %s", state.output_filename);
        }
    } else {
        // No output, so make sure that we don't even try.
        state.output_fd = -1;
        state.no_output = 1;
    }

    // Capture SIGINT and SIGTERM so that we exit gracefully
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = term_sighandler;
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGINT, &action, NULL);

    state.is_server = acquire_server_lock();
    if (state.user_wants_client && state.is_server)
        errx(EXIT_FAILURE, "Server not running");
    if (state.user_wants_server && !state.is_server)
        errx(EXIT_FAILURE, "Server already running");

    // Init datagram socket - needed for both server and client
    state.socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (state.socket_fd < 0)
        err(EXIT_FAILURE, "socket");

    state.server_addr.sun_family = AF_UNIX;
    strncpy(state.server_addr.sun_path, getenv(RASPIJPGS_SOCKET), sizeof(state.server_addr.sun_path) - 1);
    state.server_addr.sun_path[sizeof(state.server_addr.sun_path) - 1] = '\0';

    if (state.is_server)
        server_loop();
    else
        client_loop();

    free(state.socket_buffer);
    if (state.output_fd >= 0 && state.output_fd != STDOUT_FILENO)
        close(state.output_fd);

    exit(EXIT_SUCCESS);
}
