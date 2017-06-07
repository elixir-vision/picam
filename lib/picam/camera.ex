defmodule Picam.Camera do
  @moduledoc """
  GenServer which starts and manages the `raspijpgs` application as a port.
  """

  use GenServer
  require Logger

  def start_link do
    GenServer.start_link(__MODULE__, [], name: __MODULE__)
  end

  def init(_) do
    executable = Path.join(:code.priv_dir(:picam), "raspijpgs")
    port = Port.open({:spawn_executable, executable},
      [{:packet, 4}, :use_stdio, :binary, :exit_status])
    {:ok, %{port: port, requests: []}}
  end

  # GenServer callbacks

  def handle_call(:next_frame, from, state) do
    state = %{state | requests: [from | state.requests]}
    {:noreply, state}
  end

  def handle_cast({:set, message}, state) do
    send state.port, {self(), {:command, message}}
    {:noreply, state}
  end

  def handle_info({_, {:data, jpg}}, state) do
    Task.start(fn -> dispatch(state.requests, jpg) end)
    {:noreply, %{state | requests: []}}
  end

  def handle_info({_, {:exit_status, _}}, state) do
    {:stop, :unexpected_exit, state}
  end

  def terminate(reason, _state) do
    Logger.warn "Camera GenServer exiting: #{inspect reason}"
  end

  # Private helper functions

  defp dispatch(requests, jpg) do
    for req <- Enum.reverse(requests),
      do: GenServer.reply(req, jpg)
  end
end
