defmodule Exraspijpgs.Camera do
  use GenServer

  # Public API
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

  def next_picture do
    GenServer.call(__MODULE__, :next_picture)
  end

  def stop do
    GenServer.cast(__MODULE__, :stop)
  end

  # TODO: debug why this doesn't work
  def set(msg) do
    GenServer.cast(__MODULE__, {:set, msg})
  end

  def set_vflip(on \\ "on") do
    set("vflip=#{on}")
  end

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
