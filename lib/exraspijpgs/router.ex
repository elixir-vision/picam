defmodule Exraspijpgs.Router do
  use Plug.Router

  plug :match
  plug :dispatch

  @boundry "w58EW1cEpjzydSCq"

  get "/video" do
    conn
    |> put_resp_header("content-type", "multipart/x-mixed-replace; boundary=#{@boundry}")
    |> send_chunked(200)
    |> send_pictures
  end

  #forward "/users", to: UsersRouter

  match _ do
    send_resp(conn, 404, "oops. Try /video")
  end

  defp send_pictures(conn) do
    send_picture(conn)
    send_pictures(conn)
  end

  defp send_picture(conn) do
    jpg = Exraspijpgs.Camera.next_picture
    size = byte_size(jpg)
    header = "------#{@boundry}\r\nContent-Type: \"image/jpeg\"\r\nContent-length: #{size}\r\n\r\n"
    footer = "\r\n"
    with {:ok, conn} <- chunk(conn, header),
         {:ok, conn} <- chunk(conn, jpg),
         {:ok, conn} <- chunk(conn, footer), do: send_picture(conn)
    conn
  end
end
