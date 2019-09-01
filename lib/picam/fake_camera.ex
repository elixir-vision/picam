defmodule Picam.FakeCamera do
  @moduledoc """
  A fake version of `Picam.Camera` that can be used in test and development
  when a real Raspberry Pi Camera isn't available.

  All the normal configuration commands are accepted and silently ignored,
  except that:

    * `size` can be set to one of 1920x1080, 1280x720, or 640x480. This will
      result in a static image of that size being displayed on each frame.

    * `fps` can be set, which will result in the static images being displayed
      at roughly the requested rate.

  In addition, a custom image can be set using `set_image/1`, which is be
  displayed on each frame until a new image is set or the `size` is changed.
  """

  use GenServer
  require Logger

  @doc false
  def start_link(opts \\ []) do
    GenServer.start_link(__MODULE__, opts, name: __MODULE__)
  end

  @doc false
  def init(_opts) do
    state = %{jpg: image_data(1280, 720), fps: 30, requests: []}
    schedule_next_frame(state)

    {:ok, state}
  end

  @doc """
  Set the image that is being displayed by the fake camera on each frame.
  This is intended for development or testing things that depend on the
  content of the image seen by the camera.

  `jpg` should be a JPEG-encoded binary, for example:

  ```elixir
  "image.jpg"
  |> File.read!()
  |> Picam.FakeCamera.set_image()
  ```
  """
  def set_image(jpg) do
    GenServer.cast(__MODULE__, {:set_image, jpg})
  end

  # GenServer callbacks

  @doc false
  def handle_call(:next_frame, from, state) do
    state = %{state | requests: [from | state.requests]}
    {:noreply, state}
  end

  @doc false
  def handle_cast({:set, "size=" <> size}, state) do
    [width, height] =
      size
      |> String.split(",", limit: 2)
      |> Enum.map(&String.to_integer/1)

    {:noreply, %{state | jpg: image_data(width, height)}}
  end

  def handle_cast({:set, "fps=" <> fps}, state) do
    {:noreply, %{state | fps: fps |> String.to_float() |> round()}}
  end

  def handle_cast({:set, _message}, state) do
    {:noreply, state}
  end

  def handle_cast({:set_image, jpg}, state) do
    {:noreply, %{state | jpg: jpg}}
  end

  @doc false
  def handle_info(:send_frame, state) do
    schedule_next_frame(state)
    Task.start(fn -> dispatch(state.requests, state.jpg) end)
    {:noreply, %{state | requests: []}}
  end

  @doc false
  def terminate(reason, _state) do
    Logger.warn("FakeCamera GenServer exiting: #{inspect(reason)}")
  end

  # Private helper functions

  defp dispatch(requests, jpg) do
    for req <- Enum.reverse(requests), do: GenServer.reply(req, jpg)
  end

  defp schedule_next_frame(%{fps: fps}) do
    Process.send_after(self(), :send_frame, Integer.floor_div(1000, fps))
  end

  defp image_data(1920, 1080), do: image_data("1920_1080.jpg")
  defp image_data(640, 480), do: image_data("640_480.jpg")
  defp image_data(_, _), do: image_data("1280_720.jpg")

  defp image_data(filename) when is_binary(filename) do
    :code.priv_dir(:picam)
    |> Path.join("fake_camera_images/#{filename}")
    |> File.read!()
  end
end
