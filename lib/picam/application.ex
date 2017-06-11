defmodule Picam.Application do
  @moduledoc false

  use Application

  def start(_type, _args) do
    import Supervisor.Spec, warn: false

    children = [
      worker(Picam.Camera, [])
    ]

    opts = [strategy: :one_for_one, name: Picam.Supervisor]
    Supervisor.start_link(children, opts)
  end
end
