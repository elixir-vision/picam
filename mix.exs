defmodule Picam.Mixfile do
  use Mix.Project

  def project do
    [app: :picam,
     version: "0.1.0",
     elixir: "~> 1.4",
     build_embedded: Mix.env == :prod,
     start_permanent: Mix.env == :prod,
     compilers: [:elixir_make] ++ Mix.compilers,
     deps: deps()
     description: description(),
     package: package(),
     name: "Picam",
     homepage_url: "https://github.com/electricshaman/picam",
     source_url: "https://github.com/electricshaman/picam"]
  end

  def application do
    [extra_applications: [:logger]]
  end

  defp deps do
    [{:elixir_make, "~> 0.4", runtime: false},
     {:earmark, "~> 0.1", only: :dev},
     {:ex_doc, "~> 0.11", only: :dev}]
  end

  defp description do
    """
    Picam is a library that provides a simple API for streaming MJPEG video and capturing JPEG stills
    using the camera module on Raspberry Pi devices running Linux.
    """
  end

  defp package do
    [
      name: :picam,
      files: ["lib", "src/*.[ch]", "Makefile", "test", "priv", "mix.exs", "README.md", "LICENSE"],
      maintainers: ["Frank Hunleth", "Jeff Smith"],
      licenses: ["BSD 3-Clause License"],
      links: %{"GitHub" => "https://github.com/electricshaman/picam"}
    ]
  end
end
