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
    {:ok, %{port: port, requests: []}}
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
  def set_width(width \\ 320) do
    set("width=#{width}")
  end

  @doc """
  Set image height (0 = calculate from width)
  """
  def set_height(height \\ 0) do
    set("height=#{height}")
  end

  @doc """
  Annotate the video frames with this text
  """
  def set_annotation(annotation \\ "") do
    set("annotation=#{annotation}")
  end

  @doc """
  Turn on a black background behind the annotation
  """
  def set_anno_background(anno_background \\ "off") do
    set("anno_background=#{anno_background}")
  end

  @doc """
  Set image sharpness (-100 to 100)
  """
  def set_sharpness(anno_sharpness \\ 0) do
    set("sharpness=#{anno_sharpness}")
  end

  @doc """
  Set image contrast (-100 to 100)
  """
  def set_contrast(contrast \\ 0) do
    set("contrast=#{contrast}")
  end

  @doc """
  Set image brightness (0 to 100)
  """
  def set_brightness(brightness \\ 50) do
    set("brightness=#{brightness}")
  end

  @doc """
  Set image saturation (-100 to 100)
  """
  def set_saturation(saturation \\ 0) do
    set("saturation=#{saturation}")
  end

  @doc """
  Set capture ISO (100 to 800)
  """
  def set_iso(iso \\ 0) do
    set("ISO=#{iso}")
  end

  @doc """
  Turn on video stabilisation
  """
  def set_vstab(vstab \\ "off") do
    set("vstab=#{vstab}")
  end

  @doc """
  Set EV compensation (-10 to 10)
  """
  def set_ev(ev \\ 0) do
    set("ev=#{ev}")
  end

  @doc """
  Set exposure mode
  """
  def set_exposure(exposure \\ "auto") do
    set("exposure=#{exposure}")
  end

  @doc """
  Limit the frame rate (0 = auto)
  """
  def set_fps(fps \\ 0) do
    set("fps=#{fps}")
  end

  @doc """
  Set Automatic White Balance (AWB) mode
  """
  def set_awb(awb \\ "auto") do
    set("awb=#{awb}")
  end

  @doc """
  Set image effect
  """
  def set_imxfx(imxfx \\ "none") do
    set("imxfx=#{imxfx}")
  end

  @doc """
  Set colour effect <U:V>
  """
  def set_colfx(colfx \\ "") do
    set("colfx=#{colfx}")
  end

  @doc """
  Set sensor mode (0 to 7)
  """
  def set_mode(mode \\ 0) do
    set("mode=#{mode}")
  end

  @doc """
  Set metering mode
  """
  def set_metering(metering \\ "average") do
    set("metering=#{metering}")
  end

  @doc """
  Set image rotation (0-359)
  """
  def set_rotation(rotation \\ 0) do
    set("rotation=#{rotation}")
  end

  @doc """
  Set horizontal flip
  """
  def set_hflip(hflip \\ "off") do
    set("hflip=#{hflip}")
  end

  @doc """
  Set vertical flip
  """
  def set_vflip(vflip \\ "off") do
    set("vflip=#{vflip}")
  end

  @doc """
  Set region of interest (x,y,w,d as normalised coordinates [0.0-1.0])
  """
  def set_roi(roi \\ "0:0:1:1") do
    set("roi=#{roi}")
  end

  @doc """
  Set shutter speed
  """
  def set_shutter(shutter \\ 0) do
    set("shutter=#{shutter}")
  end

  @doc """
  Set the JPEG quality (0-100)
  """
  def set_quality(quality \\ 15) do
    set("quality=#{quality}")
  end

  @doc """
  Set the JPEG restart interval (default of 0 for none)
  """
  def set_restart_interval(restart_interval \\ 0) do
    set("restart_interval=#{restart_interval}")
  end

  @doc """
  Specify the socket filename for communication
  """
  def set_socket(path \\ "/tmp/raspijpgs_socket") do
    set("socket=#{path}")
  end

  @doc """
  Specify an output filename
  """
  def set_output(path \\ "") do
    set("output=#{path}")
  end

  @doc """
  How many frames to capture before quiting (-1 = no limit)
  """
  def set_count(count \\ -1) do
    set("count=#{count}")
  end

  @doc """
  Specify a lock filename to prevent multiple runs
  """
  def set_lockfile(path \\ "/tmp/raspijpgs_lock") do
    set("lockfile=#{path}")
  end

  # GenServer callbacks

  def handle_call(:next_picture, from, state) do
    state = %{state | requests: state.requests ++ [from]}
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
    for client <- state.requests do
      GenServer.reply client, jpg
    end
    {:noreply, %{state | requests: []}}
  end

  def handle_info({_, {:exit_status, _}}, state) do
    {:stop, :unexpected_exit, state}
  end

  # Private helper functions
end
