defmodule Picam do
  @moduledoc """
  This module contains functions to manipulate, capture, and stream
  MJPEG video on a Raspberry Pi using the camera module.
  """

  @camera Application.get_env(:picam, :camera, Picam.Camera)

  @doc """
  Returns a binary with the contents of a single JPEG frame from the camera.
  """
  def next_frame do
    GenServer.call(@camera, :next_frame)
  end

  @doc """
  Set the image size. One of the dimensions may be set
  to 0 to auto-calculate it based on the aspect ratio of
  the camera.
  """
  def set_size(width, height)
      when is_integer(width) and is_integer(height) and (width > 0 or height > 0),
      do: set("size=#{width},#{height}")

  def set_size(_width, _height), do: {:error, :invalid_size}

  @doc """
  Annotate the JPEG frames with the text in `annotation`.
  """
  def set_annotation_text(annotation \\ "")

  def set_annotation_text(annotation) when is_binary(annotation),
    do: set("annotation=#{annotation}")

  def set_annotation_text(_other), do: {:error, :invalid_annotation}

  @doc """
  Enable or disable a black background behind the annotation.
  """
  def set_annotation_bg(false), do: set("anno_background=off")
  def set_annotation_bg(true), do: set("anno_background=on")
  def set_annotation_bg(_other), do: {:error, :invalid_annotation_bg}

  @doc """
  Set the image sharpness.

  The accepted range is [-100, 100].
  """
  def set_sharpness(sharpness \\ 0)
  def set_sharpness(sharpness) when sharpness in -100..100, do: set("sharpness=#{sharpness}")
  def set_sharpness(_other), do: {:error, :invalid_sharpness}

  @doc """
  Set the image contrast.

  The accepted range is [-100, 100].
  """
  def set_contrast(contrast \\ 0)
  def set_contrast(contrast) when contrast in -100..100, do: set("contrast=#{contrast}")
  def set_contrast(_other), do: {:error, :invalid_contrast}

  @doc """
  Set the image brightness.

  The accepted range is [0, 100].
  """
  def set_brightness(brightness \\ 50)
  def set_brightness(brightness) when brightness in 0..100, do: set("brightness=#{brightness}")
  def set_brightness(_other), do: {:error, :invalid_brightness}

  @doc """
  Set the image saturation.

  The accepted range is [-100, 100].
  """
  def set_saturation(saturation \\ 0)
  def set_saturation(saturation) when saturation in -100..100, do: set("saturation=#{saturation}")
  def set_saturation(_other), do: {:error, :invalid_saturation}

  @doc """
  Set the capture ISO.

  The accepted range is [0, 800].

  If the `iso` given is 0, it will be automatically regulated by the camera.
  """
  def set_iso(iso \\ 0)
  def set_iso(iso) when iso in 0..800, do: set("ISO=#{iso}")
  def set_iso(_other), do: {:error, :invalid_iso}

  @doc """
  Enable or disable video stabilization.
  """
  def set_vstab(false), do: set("vstab=off")
  def set_vstab(true), do: set("vstab=on")
  def set_vstab(_other), do: {:error, :invalid_vstab}

  @doc """
  Set the exposure compensation (EV) level.

  The accepted range is [-25, 25].
  """
  def set_ev(ev \\ 0)
  def set_ev(ev) when ev in -25..25, do: set("ev=#{ev}")
  def set_ev(_other), do: {:error, :invalid_ev}

  @doc """
  Set the exposure mode.

  The accepted modes are:

    * `:auto`
    * `:night`
    * `:nightpreview`
    * `:backlight`
    * `:spotlight`
    * `:sports`
    * `:snow`
    * `:beach`
    * `:verylong`
    * `:fixedfps`
    * `:antishake`
    * `:fireworks`

  """

  @exposure_modes [
    :auto,
    :night,
    :nightpreview,
    :backlight,
    :spotlight,
    :sports,
    :snow,
    :beach,
    :verylong,
    :fixedfps,
    :antishake,
    :fireworks
  ]

  def set_exposure_mode(mode \\ :auto)
  def set_exposure_mode(mode) when mode in @exposure_modes, do: set("exposure=#{mode}")
  def set_exposure_mode(_other), do: {:error, :unknown_exposure_mode}

  @doc """
  Limit the frame rate to the given `rate`.

  The accepted range is [0.0, 90.0], but the actual rate used is governed
  by the current `sensor_mode`.

  If the `rate` given is 0 (or 0.0), frame rate will be automatically regulated.
  """
  def set_fps(rate \\ 0)
  def set_fps(rate) when is_integer(rate) and rate in 0..90, do: set_fps(:erlang.float(rate))
  def set_fps(rate) when is_float(rate) and rate >= 0.0 and rate <= 90.0, do: set("fps=#{rate}")
  def set_fps(_other), do: {:error, :invalid_frame_rate}

  @doc """
  Set the Automatic White Balance (AWB) mode.

  The accepted modes are:

    * `:off`
    * `:auto`
    * `:sun`
    * `:cloud`
    * `:shade`
    * `:tungsten`
    * `:fluorescent`
    * `:incandescent`
    * `:flash`
    * `:horizon`

  """

  @awb_modes [
    :off,
    :auto,
    :sun,
    :cloud,
    :shade,
    :tungsten,
    :fluorescent,
    :incandescent,
    :flash,
    :horizon
  ]

  def set_awb_mode(mode \\ :auto)
  def set_awb_mode(mode) when mode in @awb_modes, do: set("awb=#{mode}")
  def set_awb_mode(_other), do: {:error, :unknown_awb_mode}

  @doc """
  Set the image effect.

  The accepted effects are:

    * `:none`
    * `:negative`
    * `:solarise`
    * `:sketch`
    * `:denoise`
    * `:emboss`
    * `:oilpaint`
    * `:hatch`
    * `:gpen`
    * `:pastel`
    * `:watercolor`
    * `:film`
    * `:blur`
    * `:saturation`
    * `:colorswap`
    * `:washedout`
    * `:posterise`
    * `:colorpoint`
    * `:colorbalance`
    * `:cartoon`

  """

  @img_effects [
    :none,
    :negative,
    :solarise,
    :sketch,
    :denoise,
    :emboss,
    :oilpaint,
    :hatch,
    :gpen,
    :pastel,
    :watercolour,
    :watercolor,
    :film,
    :blur,
    :saturation,
    :colourswap,
    :colorswap,
    :washedout,
    :posterise,
    :colourpoint,
    :colorpoint,
    :colourbalance,
    :colorbalance,
    :cartoon
  ]

  def set_img_effect(effect \\ :none)
  def set_img_effect(effect) when effect in @img_effects, do: set("imxfx=#{effect}")
  def set_img_effect(_other), do: {:error, :unknown_image_effect}

  @doc """
  Set the color effect applied by the camera.

  The effect is set with the tuple `{u,v}`.

  The accepted range for both values is [0, 255].

  If the `effect` given is `:none`, color effects will be disabled.

  ## Examples

      iex> Picam.set_col_effect({128,128}) # Black and white
      :ok

  """
  def set_col_effect(effect \\ :none)
  def set_col_effect({u, v}) when u in 0..255 and v in 0..255, do: set("colfx=#{u}:#{v}")
  def set_col_effect(:none), do: set("colfx=")
  def set_col_effect(_other), do: {:error, :invalid_color_effect}

  @doc """
  Set the sensor mode.

  Details on the accepted modes (0-7) are listed in the tables below:

  ## V1 Camera Module
  | # | Resolution | Ratio | FPS Range | Video | Image | FoV     | Binning |
  |---|------------|-------|-----------|-------|-------|---------|---------|
  | 1 | 1920x1080  | 16:9  | (1, 30]   | Y     |       | Partial | None    |
  | 2 | 2592x1944  | 4:3   | (1, 15]   | Y     | Y     | Full    | None    |
  | 3 | 2592x1944  | 4:3   | [0.16, 1] | Y     | Y     | Full    | None    |
  | 4 | 1296x972   | 4:3   | (1, 42]   | Y     |       | Full    | 2x2     |
  | 5 | 1296x730   | 16:9  | (1, 49]   | Y     |       | Full    | 2x2     |
  | 6 | 640x480    | 4:3   | (42, 60]  | Y     |       | Full    | 4x4     |
  | 7 | 640x480    | 4:3   | (60, 90]  | Y     |       | Full    | 4x4     |

  ## V2 Camera Module
  | # | Resolution | Ratio | FPS Range  | Video | Image | FoV     | Binning |
  |---|------------|-------|------------|-------|-------|---------|---------|
  | 1 | 1920x1080  | 16:9  | [0.10, 30] | Y     |       | Partial | None    |
  | 2 | 3280x2464  | 4:3   | [0.10, 15] | Y     | N     | Full    | None    |
  | 3 | 3280x2464  | 4:3   | [0.10, 15] | Y     | N     | Full    | None    |
  | 4 | 1640x1232  | 4:3   | [0.10, 40] | Y     |       | Full    | 2x2     |
  | 5 | 1640x922   | 16:9  | [0.10, 40] | Y     |       | Full    | 2x2     |
  | 6 | 1280x720   | 16:9  | (40, 90]   | Y     |       | Partial | 2x2     |
  | 7 | 640x480    | 4:3   | (40, 90]   | Y     |       | Partial | 2x2     |

  If the `mode` given is 0, the camera will select a mode automatically.

  """
  def set_sensor_mode(mode \\ 0)
  def set_sensor_mode(mode) when mode in 0..7, do: set("mode=#{mode}")
  def set_sensor_mode(_other), do: {:error, :unknown_sensor_mode}

  @doc """
  Set the metering mode.

  The accepted modes are:

    * `:average`
    * `:spot`
    * `:backlit`
    * `:matrix`

  """

  @metering_modes [:average, :spot, :backlit, :matrix]

  def set_metering_mode(mode \\ :average)
  def set_metering_mode(mode) when mode in @metering_modes, do: set("metering=#{mode}")
  def set_metering_mode(_other), do: {:error, :unknown_metering_mode}

  @doc """
  Set the image rotation angle in degrees.

  The accepted angles are 0, 90, 180, or 270.
  """
  def set_rotation(angle \\ 0)
  def set_rotation(angle) when angle in [0, 90, 180, 270], do: set("rotation=#{angle}")
  def set_rotation(_other), do: {:error, :invalid_rotation_angle}

  @doc """
  Flip the image horizontally.
  """
  def set_hflip(false), do: set("hflip=off")
  def set_hflip(true), do: set("hflip=on")
  def set_hflip(_other), do: {:error, :invalid_hflip}

  @doc """
  Flip the image vertically.
  """
  def set_vflip(false), do: set("vflip=off")
  def set_vflip(true), do: set("vflip=on")
  def set_vflip(_other), do: {:error, :invalid_vflip}

  @doc """
  Set a region of interest.

  (x,y,w,h as normalized coordinates [0.0, 1.0])
  """
  def set_roi(roi \\ "0:0:1:1")
  def set_roi(roi) when is_binary(roi), do: set("roi=#{roi}")
  def set_roi(_other), do: {:error, :invalid_roi}

  @doc """
  Set the shutter speed in microseconds

  If the `speed` given is 0, it will be automatically regulated.
  """
  def set_shutter_speed(speed \\ 0)
  def set_shutter_speed(speed) when is_integer(speed) and speed >= 0, do: set("shutter=#{speed}")
  def set_shutter_speed(_other), do: {:error, :invalid_shutter_speed}

  @doc """
  Set the JPEG quality.

  The accepted range is [1, 100].
  """
  def set_quality(quality \\ 15)
  def set_quality(quality) when quality in 1..100, do: set("quality=#{quality}")
  def set_quality(_other), do: {:error, :invalid_quality}

  @doc """
  Set the JPEG restart interval.

  If the `interval` given is 0, restart intervals will not be used.
  """
  def set_restart_interval(interval \\ 0)

  def set_restart_interval(interval) when is_integer(interval) and interval >= 0,
    do: set("restart_interval=#{interval}")

  def set_restart_interval(_other), do: {:error, :invalid_restart_interval}

  @doc """
  Enable or disable video preview output to the attached display(s).

  Defaults to `false`.
  """
  def set_preview_enabled(false), do: set("preview=off")
  def set_preview_enabled(true), do: set("preview=on")
  def set_preview_enabled(_), do: {:error, :invalid_preview}

  @doc """
  Enable or disable full-screen output for video preview.

  Defaults to `true`.
  """
  def set_preview_fullscreen(false), do: set("preview_fullscreen=off")
  def set_preview_fullscreen(true), do: set("preview_fullscreen=on")
  def set_preview_fullscreen(_), do: {:error, :invalid_preview_fullscreen}

  @doc """
  Set the location and size of the (non-fullscreen) video preview.
  This option is ignored when fullscreen preview is enabled.

  Defaults to `true`.
  """
  def set_preview_window(x, y, width, height) when \
    is_integer(x) and x >= 0 and \
    is_integer(y) and y >= 0 and \
    is_integer(width) and width >= 0 and \
    is_integer(height) and height >= 0 \
  do
    set("preview_window=#{x},#{y},#{width},#{height}")
  end
  def set_preview_window(_), do: {:error, :invalid_preview_window}

  # Private helper functions

  defp set(msg) do
    GenServer.cast(@camera, {:set, msg})
  end
end
