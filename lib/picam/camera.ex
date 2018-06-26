defmodule Picam.Camera do
  @moduledoc """
  GenServer which starts and manages the `raspijpgs` application as a port.
  """

  use GenServer
  require Logger

  def start_link(opts \\ []) do
    GenServer.start_link(__MODULE__, opts, name: __MODULE__)
  end

  def init(_opts) do
    executable = Path.join(:code.priv_dir(:picam), "raspijpgs")

    port =
      Port.open({:spawn_executable, executable}, [{:packet, 4}, :use_stdio, :binary, :exit_status])

    offline_image = image_data("offline_1280_720.jpg")

    {:ok, %{port: port, requests: [], offline: false, offline_image: offline_image}}
  end

  # GenServer callbacks

  def handle_call(:next_frame, _from, state = %{offline: true, offline_image: offline_image}) do
    Task.start(fn -> dispatch(state.requests, offline_image) end)
    {:reply, offline_image, %{state | requests: []}}
  end

  def handle_call(:next_frame, from, state) do
    state = %{state | requests: [from | state.requests]}
    {:noreply, state}
  end

  def handle_cast({:set, message}, state) do
    send(state.port, {self(), {:command, message}})
    {:noreply, state}
  end

  def handle_info({_, {:data, jpg}}, state) do
    Task.start(fn -> dispatch(state.requests, jpg) end)
    {:noreply, %{state | requests: [], offline: false}}
  end

  def handle_info(:reconnect_port, state) do
    executable = Path.join(:code.priv_dir(:picam), "raspijpgs")
    port = Port.open({:spawn_executable, executable}, [{:packet, 4}, :use_stdio, :binary, :exit_status])

    {:noreply, %{state | port: port}}
  end

  def handle_info({_, {:exit_status, _}}, state) do
    Process.send_after(self(), :reconnect_port, 10_000)
    {:noreply, %{state | offline: true}}
  end

  def terminate(reason, _state) do
    Logger.warn("Camera GenServer exiting: #{inspect(reason)}")
  end

  # Private helper functions

  defp dispatch(requests, jpg) do
    for req <- Enum.reverse(requests), do: GenServer.reply(req, jpg)
  end

  defp image_data(filename) when is_binary(filename) do
    :code.priv_dir(:picam)
    |> Path.join("fake_camera_images/#{filename}")
    |> File.read!()
  end
end
