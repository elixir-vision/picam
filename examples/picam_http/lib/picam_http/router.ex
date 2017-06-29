defmodule PicamHTTP.Router do
  use Plug.Router

  plug :match
  plug :dispatch

  get "/" do
    markup = """
    <html>
    <head>
      <title>Picam Video Stream</title>
    </head>
    <body>
      <img src="video.mjpg" />
    </body>
    </html>
    """
    conn
    |> put_resp_header("Content-Type", "text/html")
    |> send_resp(200, markup)
  end

  forward "/video.mjpg", to: PicamHTTP.Streamer

  match _ do
    send_resp(conn, 404, "Oops. Try /")
  end

end
