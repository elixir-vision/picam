defmodule Exraspijpgs.Application do
  @moduledoc false

  use Application

  def start(_type, _args) do
    import Supervisor.Spec, warn: false

    children = [
      Plug.Adapters.Cowboy.child_spec(:http, Exraspijpgs.Router, [], [port: 4001]),
      worker(Exraspijpgs.Camera, [])
    ]

    opts = [strategy: :one_for_one, name: Exraspijpgs.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
