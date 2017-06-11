defmodule Picam.Router do
  use Plug.Router

  plug :match
  plug :dispatch

  forward "/video", to: Picam.Streamer

  match _ do
    send_resp(conn, 404, "oops. Try /video")
  end

end
