defmodule Exraspijpgs.Camera do
  use GenServer

  alias __MODULE__

  defstruct port: nil,
            requests: []

  # Public API
  def start_link(somearg, opts \\ []) do
    GenServer.start_link(__MODULE__, somearg, opts)
  end

  def next_picture(pid) do
    GenServer.call(pid, :next_picture)
  end

  def stop(pid) do
    GenServer.cast(pid, :stop)
  end

  # TODO: debug why this doesn't work
  def set(pid, msg) do
    GenServer.cast(pid, {:set, msg})
  end

  def set_vflip(pid, on \\ "on") do
    set(pid, "vflip=#{on}")
  end

  # gen_server callbacks
  def init(_arg) do
    executable = "./priv/raspijpgs"
    port = Port.open({:spawn_executable, executable},
      [{:args, ["--vflip", "--hflip", "--framing", "header", "--output", "-"]}, {:packet, 4}, :use_stdio, :binary, :exit_status])
    { :ok, %Camera{port: port} }
  end

  def handle_call(:next_picture, from, state) do
    state = %{state | :requests => state.requests ++ [from]}
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
    {:noreply, %{state | :requests => []}}
  end
  def handle_info({_, {:exit_status, _}}, state) do
    {:stop, :unexpected_exit, state}
  end

  # Private helper functions
end
