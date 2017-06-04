defmodule Exraspijpgs.Camera do
  @moduledoc """
  GenServer which starts and manages the `raspijpgs` application as a port.
  """

  use GenServer
  require Logger

  def start_link do
    GenServer.start_link(__MODULE__, [], name: __MODULE__)
  end

  def init(_) do
    executable = :code.priv_dir(:exraspijpgs) ++ '/raspijpgs'
    port = Port.open({:spawn_executable, executable},
      [{:args, []},
       {:packet, 4}, :use_stdio, :binary, :exit_status])
    {:ok, %{port: port, requests: :queue.new}}
  end

  # GenServer callbacks

  def handle_call(:next_frame, from, state) do
    state = %{state | requests: :queue.in(from, state.requests)}
    {:noreply, state}
  end

  def handle_cast({:set, message}, state) do
    send state.port, {self(), {:command, message}}
    {:noreply, state}
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

  def terminate(reason, _state) do
    Logger.warn "Camera GenServer exiting due to reason: #{inspect reason}"
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
