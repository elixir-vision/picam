defmodule PicamHTTP.Router do
  use Plug.Router

  plug :match
  plug :dispatch

  get "/video" do
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
    put_resp_header(conn, "Content-Type", "text/html")
    |> send_resp(200, markup)
  end

  forward "/video.mjpg", to: PicamHTTP.Streamer

  match _ do
    send_resp(conn, 404, "oops. Try /video")
  end

end
