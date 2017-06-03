defmodule Exraspijpgs.Camera do
  use GenServer

  def start_link do
    GenServer.start_link(__MODULE__, [], name: __MODULE__)
  end

  def init(_) do
    executable = "./priv/raspijpgs"
    port = Port.open({:spawn_executable, executable},
      [{:args, ["--vflip", "--hflip", "--framing", "header", "--output", "-"]},
       {:packet, 4}, :use_stdio, :binary, :exit_status])
    {:ok, %{port: port, requests: :queue.new}}
  end

  # Public API

  def next_picture do
    GenServer.call(__MODULE__, :next_picture)
  end

  def stop do
    GenServer.cast(__MODULE__, :stop)
  end

  # TODO: Check that this works. ;)
  def set(msg) do
    GenServer.cast(__MODULE__, {:set, msg})
  end

  @doc """
  Set image width
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
  def set_sharpness(anno_sharpness \\ 0)
  def set_sharpness(anno_sharpness) when is_integer(anno_sharpness),
    do: set("sharpness=#{anno_sharpness}")
  def set_anno_sharpness(_other),
    do: {:error, :invalid_anno_sharpness}

  @doc """
  Set image contrast (-100 to 100)
  """
  def set_contrast(contrast \\ 0)
  def set_contrast(contrast) when is_integer(contrast),
    do: set("contrast=#{contrast}")
  def set_contrast(_other),
    do: {:error, :invalid_contrast}

  @doc """
  Set image brightness (0 to 100)
  """
  def set_brightness(brightness \\ 50)
  def set_brightness(brightness) when is_integer(brightness),
    do: set("brightness=#{brightness}")
  def set_brightness(_other),
    do: {:error, :invalid_brightness}

  @doc """
  Set image saturation (-100 to 100)
  """
  def set_saturation(saturation \\ 0)
  def set_saturation(saturation) when is_integer(saturation),
    do: set("saturation=#{saturation}")
  def set_saturation(_other),
    do: {:error, :invalid_saturation}

  @doc """
  Set capture ISO (100 to 800)
  """
  def set_iso(iso \\ 0)
  def set_iso(iso) when is_integer(iso),
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
  Set EV compensation (-10 to 10)
  """
  def set_ev(ev \\ 0)
  def set_ev(ev) when is_integer(ev),
    do: set("ev=#{ev}")
  def set_ev(_other),
    do: {:error, :invalid_ev}

  @doc """
  Set exposure mode
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
  def set_fps(fps) when is_integer(fps),
    do: set("fps=#{fps}")
  def set_fps(_other),
    do: {:error, :invalid_fps}

  @doc """
  Set Automatic White Balance (AWB) mode
  """
  def set_awb(awb \\ "auto")
  def set_awb(awb) when is_binary(awb),
    do: set("awb=#{awb}")
  def set_awb(_other),
    do: {:error, :invalid_awb}

  @doc """
  Set image effect
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
  def set_mode(mode) when is_integer(mode),
    do: set("mode=#{mode}")
  def set_mode(_other),
    do: {:error, :invalid_mode}

  @doc """
  Set metering mode
  """
  def set_metering(metering \\ "average")
  def set_metering(metering) when is_binary(metering),
    do: set("metering=#{metering}")
  def set_metering(_other),
    do: {:error, :invalid_metering}

  @doc """
  Set image rotation (0-359)
  """
  def set_rotation(rotation \\ 0)
  def set_rotation(rotation) when is_integer(rotation),
    do: set("rotation=#{rotation}")
  def set_rotation(_other),
    do: {:error, :invalid_rotation}

  @doc """
  Set horizontal flip
  """
  def set_hflip(hflip \\ "off")
  def set_hflip(hflip) when is_binary(hflip),
    do: set("hflip=#{hflip}")
  def set_hflip(_other),
    do: {:error, :invalid_hflip}

  @doc """
  Set vertical flip
  """
  def set_vflip(vflip \\ "off")
  def set_vflip(vflip) when is_binary(vflip),
    do: set("vflip=#{vflip}")
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
  Set shutter speed
  """
  def set_shutter(shutter \\ 0)
  def set_shutter(shutter) when is_integer(shutter),
    do: set("shutter=#{shutter}")
  def set_shutter(_other),
    do: {:error, :invalid_shutter}

  @doc """
  Set the JPEG quality (0-100)
  """
  def set_quality(quality \\ 15)
  def set_quality(quality) when is_integer(quality),
    do: set("quality=#{quality}")
  def set_quality(_other),
    do: {:error, :invalid_quality}

  @doc """
  Set the JPEG restart interval (default of 0 for none)
  """
  def set_restart_interval(restart_interval \\ 0)
  def set_restart_interval(restart_interval) when is_integer(restart_interval),
    do: set("restart_interval=#{restart_interval}")
  def set_restart_interval(_other),
    do: {:error, :invalid_restart_interval}

  @doc """
  Specify the socket filename for communication
  """
  def set_socket(path \\ "/tmp/raspijpgs_socket")
  def set_socket(path) when is_binary(path),
    do: set("socket=#{path}")
  def set_socket(_other),
    do: {:error, :invalid_socket_path}

  @doc """
  Specify an output filename
  """
  def set_output(path \\ "")
  def set_output(path) when is_binary(path),
    do: set("output=#{path}")
  def set_output(_other),
    do: {:error, :invalid_output_path}

  @doc """
  How many frames to capture before quiting (-1 = no limit)
  """
  def set_count(count \\ -1)
  def set_count(count) when is_integer(count),
    do: set("count=#{count}")
  def set_count(_other),
    do: {:error, :invalid_count}

  @doc """
  Specify a lock filename to prevent multiple runs
  """
  def set_lockfile(path \\ "/tmp/raspijpgs_lock")
  def set_lockfile(path) when is_binary(path),
    do: set("lockfile=#{path}")
  def set_lockfile(_other),
    do: {:error, :invalid_lockfile_path}

  # GenServer callbacks

  def handle_call(:next_picture, from, state) do
    state = %{state | requests: :queue.in(from, state.requests)}
    {:noreply, state}
  end

  def handle_cast({:set, message}, state) do
    send state.port, message
    {:stop, :normal, state}
  end

  def handle_cast(:stop, state) do
    {:stop, :normal, state}
  end

  def handle_info({_, {:data, jpg}}, state) do
    queue = dispatch(:queue.out(state.requests), jpg)
    {:noreply, %{state | requests: queue}}
  end

  def handle_info({_, {:exit_status, _}}, state) do
    {:stop, :unexpected_exit, state}
  end

  # Private helper functions

  defp dispatch({:empty, queue}, _jpg),
    do: queue
  defp dispatch({{:value, client}, queue}, jpg) do
    GenServer.reply(client, jpg)
    next = :queue.out(queue)
    dispatch(next, jpg)
  end
end
