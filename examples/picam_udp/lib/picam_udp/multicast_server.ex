defmodule PicamUDP.MulticastServer do
  use GenServer

  require Logger

  def start_link(port, broadcast) do
    GenServer.start_link(__MODULE__, [port, broadcast], name: __MODULE__)
  end

  def init([port, broadcast]) do
    {:ok, socket} = :gen_udp.open(port, [{:broadcast, true}, {:reuseaddr, true}, {:add_membership, {broadcast, {0,0,0,0}}}])
    send(self(), :start_streaming)
    {:ok, %{socket: socket,
      port: port,
      broadcast: broadcast,
      stream_pid: nil}}
  end

  def handle_info(:start_streaming, state) do
    Logger.debug "Starting stream"
    {:ok, stream_pid} = Task.start_link(fn -> stream(state.socket, state.broadcast, state.port) end)
    {:noreply, %{state | stream_pid: stream_pid}}
  end

  def handle_info(:stop_streaming, %{stream_pid: stream_pid} = state) when not is_nil(stream_pid) do
    Logger.debug "Stopping stream"
    Process.unlink(stream_pid)
    Process.exit(stream_pid, :kill)
    {:noreply, %{state | stream_pid: nil}}
  end

  def handle_info(:stop_streaming, state) do
    {:noreply, state}
  end

  def handle_info(_msg, state) do
    {:noreply, state}
  end

  def terminate(_reason, %{socket: socket}) do
    Logger.warn "Terminating #{__MODULE__}"
    :gen_udp.close(socket)
  end

  defp stream(socket, broadcast, port) do
    frame = Picam.next_frame
    :gen_udp.send(socket, broadcast, port, frame)
    stream(socket, broadcast, port)
  end
end
