defmodule PicamUDP.Application do
  @moduledoc false

  use Application

  def start(_type, _args) do
    import Supervisor.Spec, warn: false

    config = Application.get_env(:picam_udp, :multicast)

    children = [
      worker(Picam.Camera, []),
      worker(PicamUDP.MulticastServer, [config[:port], config[:broadcast]])
    ]

    opts = [strategy: :one_for_one, name: PicamUDP.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
