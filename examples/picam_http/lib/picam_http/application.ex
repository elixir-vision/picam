defmodule PicamHTTP.Application do
  @moduledoc false

  use Application

  def start(_type, _args) do
    import Supervisor.Spec, warn: false

    port = Application.get_env(:picam_http, :port)

    children = [
      worker(Picam.Camera, []),
      Plug.Adapters.Cowboy.child_spec(:http, PicamHTTP.Router, [], [port: port]),
    ]

    opts = [strategy: :one_for_one, name: PicamHTTP.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
