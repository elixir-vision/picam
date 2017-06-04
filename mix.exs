defmodule Exraspijpgs.Mixfile do
  use Mix.Project

  def project do
    [app: :exraspijpgs,
     version: "0.1.0",
     elixir: "~> 1.4",
     build_embedded: Mix.env == :prod,
     start_permanent: Mix.env == :prod,
     compilers: [:elixir_make] ++ Mix.compilers,
     deps: deps()]
  end

  def application do
    [extra_applications: [:logger],
     mod: {Exraspijpgs.Application, []}]
  end

  defp deps do
    [{:elixir_make, "~> 0.4", runtime: false},
     {:cowboy, "~> 1.0.0"},
     {:plug, "~> 1.0"},
     {:earmark, "~> 0.1", only: :dev},
     {:ex_doc, "~> 0.11", only: :dev}]
  end
end
