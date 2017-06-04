defmodule Exraspijpgs do
  @moduledoc """
  Documentation for Exraspijpgs.
  """

  @camera Exraspijpgs.Camera

  @doc """
  Get a single JPEG frame from the camera
  """
  def next_picture do
    GenServer.call(@camera, :next_picture)
  end

  @doc """
  Set the image width.
  """
  def set_width(width \\ 320)
  def set_width(width) when is_integer(width),
    do: set("width=#{width}")
  def set_width(_other),
    do: {:error, :invalid_width}

  @doc """
  Set image height (0 = calculate from width)
  """
  def set_height(height \\ 0)
  def set_height(height) when is_integer(height),
    do: set("height=#{height}")
  def set_height(_other),
    do: {:error, :invalid_height}

  @doc """
  Annotate the video frames with this text
  """
  def set_annotation(annotation \\ "")
  def set_annotation(annotation) when is_binary(annotation),
    do: set("annotation=#{annotation}")
  def set_annotation(_other),
    do: {:error, :invalid_annotation}

  @doc """
  Turn on a black background behind the annotation
  """
  def set_anno_background(anno_background \\ "off")
  def set_anno_background(anno_background) when is_binary(anno_background),
    do: set("anno_background=#{anno_background}")
  def set_anno_background(_other),
    do: {:error, :invalid_anno_background}

  @doc """
  Set image sharpness (-100 to 100)
  """
  def set_sharpness(sharpness \\ 0)
  def set_sharpness(sharpness) when is_integer(sharpness) and sharpness in -100..100,
    do: set("sharpness=#{sharpness}")
  def set_sharpness(_other),
    do: {:error, :invalid_sharpness}

  @doc """
  Set image contrast (-100 to 100)
  """
  def set_contrast(contrast \\ 0)
  def set_contrast(contrast) when is_integer(contrast) and contrast in -100..100,
    do: set("contrast=#{contrast}")
  def set_contrast(_other),
    do: {:error, :invalid_contrast}

  @doc """
  Set image brightness (0 to 100)
  """
  def set_brightness(brightness \\ 50)
  def set_brightness(brightness) when is_integer(brightness) and brightness in 0..100,
    do: set("brightness=#{brightness}")
  def set_brightness(_other),
    do: {:error, :invalid_brightness}

  @doc """
  Set image saturation (-100 to 100)
  """
  def set_saturation(saturation \\ 0)
  def set_saturation(saturation) when is_integer(saturation) and saturation in -100..100,
    do: set("saturation=#{saturation}")
  def set_saturation(_other),
    do: {:error, :invalid_saturation}

  @doc """
  Set capture ISO (100 to 800)
  """
  def set_iso(iso \\ 0)
  def set_iso(iso) when is_integer(iso) and iso in 100..800,
    do: set("ISO=#{iso}")
  def set_iso(_other),
    do: {:error, :invalid_iso}

  @doc """
  Turn on video stabilisation
  """
  def set_vstab(vstab \\ "off")
  def set_vstab(vstab) when is_binary(vstab),
    do: set("vstab=#{vstab}")
  def set_vstab(_other),
    do: {:error, :invalid_vstab}

  @doc """
  Set EV compensation (-25 to 25)
  """
  def set_ev(ev \\ 0)
  def set_ev(ev) when is_integer(ev) and ev in -10..10,
    do: set("ev=#{ev}")
  def set_ev(_other),
    do: {:error, :invalid_ev}

  @doc """
  Set the exposure mode.

  Options are:
  auto,night,nightpreview,backlight,spotlight,sports,snow,beach,verylong,fixedfps,antishake,fireworks
  """
  def set_exposure(exposure \\ "auto")
  def set_exposure(exposure) when is_binary(exposure),
    do: set("exposure=#{exposure}")
  def set_exposure(_other),
    do: {:error, :invalid_exposure}

  @doc """
  Limit the frame rate (0 = auto)
  """
  def set_fps(fps \\ 0)
  def set_fps(fps) when is_integer(fps) and fps in 0..90,
    do: set("fps=#{fps}")
  def set_fps(_other),
    do: {:error, :invalid_fps}

  @doc """
  Set the Automatic White Balance (AWB) mode.

  Options are:
  off,auto,sun,cloud,shade,tungsten,fluorescent,incandescent,flash,horizon
  """
  def set_awb(awb \\ "auto")
  def set_awb(awb) when is_binary(awb),
    do: set("awb=#{awb}")
  def set_awb(_other),
    do: {:error, :invalid_awb}

  @doc """
  Set the image effect.

  Options are:
  none,negative,solarise,sketch,denoise,emboss,oilpaint,hatch,gpen,pastel,watercolour,
  film,blur,saturation,colourswap,washedout,posterise,colourpoint,colourbalance,cartoon
  """
  def set_imxfx(imxfx \\ "none")
  def set_imxfx(imxfx) when is_binary(imxfx),
    do: set("imxfx=#{imxfx}")
  def set_imxfx(_other),
    do: {:error, :invalid_imxfx}

  @doc """
  Set colour effect <U:V>
  """
  def set_colfx(colfx \\ "")
  def set_colfx(colfx) when is_binary(colfx),
    do: set("colfx=#{colfx}")
  def set_colfx(_other),
    do: {:error, :invalid_colfx}

  @doc """
  Set sensor mode (0 to 7)
  """
  def set_mode(mode \\ 0)
  def set_mode(mode) when is_integer(mode) and mode in 0..7,
    do: set("mode=#{mode}")
  def set_mode(_other),
    do: {:error, :invalid_mode}

  @doc """
  Set the metering mode.

  Options are:
  average, spot, backlit, matrix
  """
  def set_metering(metering \\ "average")
  def set_metering(metering) when is_binary(metering),
    do: set("metering=#{metering}")
  def set_metering(_other),
    do: {:error, :invalid_metering}

  @doc """
  Set the image rotation in degrees (0-359)
  """
  def set_rotation(rotation \\ 0)
  def set_rotation(rotation) when is_integer(rotation) and rotation in 0..359,
    do: set("rotation=#{rotation}")
  def set_rotation(_other),
    do: {:error, :invalid_rotation}

  @doc """
  Flip the image horizontally.
  """
  def set_hflip(false), do: set("hflip=off")
  def set_hflip(true), do: set("hflip=on")
  def set_hflip(_other),
    do: {:error, :invalid_hflip}

  @doc """
  Flip the image vertically.
  """
  def set_vflip(false), do: set("vflip=off")
  def set_vflip(true), do: set("vflip=on")
  def set_vflip(_other),
    do: {:error, :invalid_vflip}

  @doc """
  Set region of interest (x,y,w,d as normalised coordinates [0.0-1.0])
  """
  def set_roi(roi \\ "0:0:1:1")
  def set_roi(roi) when is_binary(roi),
    do: set("roi=#{roi}")
  def set_roi(_other),
    do: {:error, :invalid_roi}

  @doc """
  Set shutter speed in microseconds
  """
  def set_shutter(shutter \\ 0)
  def set_shutter(shutter) when is_integer(shutter),
    do: set("shutter=#{shutter}")
  def set_shutter(_other),
    do: {:error, :invalid_shutter}

  @doc """
  Set the JPEG quality (1-100)
  """
  def set_quality(quality \\ 15)
  def set_quality(quality) when is_integer(quality) and quality in 0..100,
    do: set("quality=#{quality}")
  def set_quality(_other),
    do: {:error, :invalid_quality}

  @doc """
  Set the JPEG restart interval (default of 0 for none)
  """
  def set_restart_interval(interval \\ 0)
  def set_restart_interval(interval) when is_integer(interval) and interval >= 0,
    do: set("restart_interval=#{interval}")
  def set_restart_interval(_other),
    do: {:error, :invalid_restart_interval}

  defp set(msg) do
    GenServer.cast(@camera, {:set, msg})
  end
end
