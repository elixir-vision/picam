defmodule Picam.Application do
  @moduledoc false

  use Application

  def start(_type, _args) do
    import Supervisor.Spec, warn: false

    children = [
      Plug.Adapters.Cowboy.child_spec(:http, Picam.Router, [], [port: 4001]),
      worker(Picam.Camera, [])
    ]

    opts = [strategy: :one_for_one, name: Picam.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
