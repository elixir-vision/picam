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

#include "picam_preview.h"

void picam_preview_set_defaults(PREVIEW_CONFIG_T *config)
{
    config->component = NULL;
    config->enable = MMAL_FALSE;
    config->fullscreen = MMAL_TRUE;
    config->dest_rect.x = 0;
    config->dest_rect.y = 0;
    config->dest_rect.width = 0;
    config->dest_rect.height = 0;
}

void picam_preview_configure(PREVIEW_CONFIG_T *config)
{
    // The following options don't need to be set if preview is disabled.
    // If it later becomes enabled, they'll be set at that point.
    if(config->enable)
    {
        MMAL_DISPLAYREGION_T param;
        param.hdr.id = MMAL_PARAMETER_DISPLAYREGION;
        param.hdr.size = sizeof(MMAL_DISPLAYREGION_T);

        param.set = MMAL_DISPLAY_SET_LAYER;
        param.layer = config->layer;

        param.set |= MMAL_DISPLAY_SET_FULLSCREEN;
        param.fullscreen = config->fullscreen;

        param.set |= MMAL_DISPLAY_SET_DEST_RECT;
        param.dest_rect = config->dest_rect;

        mmal_port_parameter_set(config->component->input[0], &param.hdr);
    }
}

void picam_preview_init(PREVIEW_CONFIG_T *config)
{
    // If we're re-initializing to enable or disable preview,
    // destroy the existing component.
    if(config->component != NULL)
    {
        mmal_component_destroy(config->component);
        config->component = NULL;
    }

    if(config->enable)
    {
        if(mmal_component_create(MMAL_COMPONENT_DEFAULT_VIDEO_RENDERER, &config->component) != MMAL_SUCCESS)
            errx(EXIT_FAILURE, "Could not create preview renderer\r\n");

        picam_preview_configure(config);
    } else {
        // When preview is not used, a null-sink is still required for the
        // auto-exposure feature to work. The image slowly fades to black if
        // nothing is consuming the preview port.
        if(mmal_component_create("vc.null_sink", &config->component) != MMAL_SUCCESS)
            errx(EXIT_FAILURE, "Could not create preview null-sink\r\n");
    }

    if (mmal_component_enable(config->component) != MMAL_SUCCESS)
        errx(EXIT_FAILURE, "Could not enable preview component");
}
