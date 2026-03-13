defmodule SupersonicNifTest.MixProject do
  use Mix.Project

  def project do
    [
      app: :supersonic_nif_test,
      version: "0.1.0",
      elixir: "~> 1.15",
      erlc_paths: ["../../src/nif"],
      start_permanent: false,
      deps: []
    ]
  end

  def application do
    [extra_applications: [:logger]]
  end
end
