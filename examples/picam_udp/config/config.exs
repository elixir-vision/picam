use Mix.Config

config :picam_udp, :multicast,
  broadcast: {239,5,2,1},
  port: 6670
