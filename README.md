# Picam

[![Hex version](https://img.shields.io/hexpm/v/picam.svg "Hex version")](https://hex.pm/packages/picam)

Picam is an Elixir library that provides a simple API for streaming MJPEG video and capturing JPEG stills using the camera module on Raspberry Pi devices running Linux.

Features currently supported by the API:

  - Set sharpness, contrast, brightness, saturation, ISO, and shutter speed values
  - Set the exposure, sensor, metering, and white balance modes
  - Set image and color effects
  - Rotate and flip the image vertically and horizontally
  - Set the exposure compensation (EV) level
  - Change the image size
  - Adjust JPEG fidelity through quality level, restart intervals, and region of interest
  - Enable or disable video stabilization
  - Adjust the video framerate
  - Render fullscreen or windowed video preview to HDMI and CSI displays

For specifics on the above features, please consult the [Hex docs].

## Requirements

| Requirement |        | Notes  |
| ----------- | ------ | ------ |
| Host Device | Raspberry Pi 1, 2, 3, Zero/W | Zero and Zero W require a [special ribbon cable] |
| Operating System  | Linux | Works out of the box with Raspian and Nerves builds |
| Camera Module | [V1], [V2] | Regular, NoIR. Note for V2 module, `gpu_mem` in `/boot/config.txt` must be set >= `192` |
| C Libraries | Broadcom VideoCore | Located in `/opt/vc` by default.  Override with `VIDEOCORE_DIR` |

## Installation

The package can be installed by adding `picam` to your list of dependencies in `mix.exs`:

```elixir
def deps do
  [{:picam, "~> 0.4.0"}]
end
```

## Usage

`Picam` uses a port application named `raspijpgs` that interfaces with the underlying Multi-Media Abstraction Layer (MMAL) API in VideoCore.  The port is started by the `Picam.Camera` process.

For example, to write a JPEG still using the `:sketch` image effect to `/tmp/frame.jpg`:

```elixir
iex(1)> Picam.Camera.start_link
{:ok, #PID<0.160.0>}

iex(2)> Picam.set_img_effect(:sketch)
:ok

iex(3)> Picam.set_size(640, 0) # 0 automatically calculates height
:ok

iex(4)> File.write!(Path.join(System.tmp_dir!, "frame.jpg"), Picam.next_frame)
:ok

iex(5)> Picam.set_img_effect(:none) # Disable the effect
:ok
```

If you receive an `:unexpected_exit` error immediately after starting the `Picam.Camera` process and you're using a V2 camera module, please check that you've set `gpu_mem` to a value >= 192 in `/boot/config.txt`.  You can verify this has taken effect in your terminal using `vcgencmd get_mem gpu`.

More than likely you'll want to put the `Picam.Camera` process in your supervision tree rather than starting it manually:

```elixir
# lib/my_app/application.ex

children = [
  worker(Picam.Camera, []),
  # ...
]
```

## Faking the camera for development and testing

In order to facilitate running in `dev` and `test` modes on your development host, you can override the real `Picam.Camera` worker with `Picam.FakeCamera` by setting the `:camera` config option:

```elixir
# config.exs

# ...

import_config "#{Mix.Project.config[:target]}.exs"
```

```elixir
# config/host.exs

use Mix.Config
config :picam, camera: Picam.FakeCamera
```

This will cause `Picam` to use the `FakeCamera` back-end instead of the real `Camera` back-end, which streams a static image of the specified `size` at approximately the specified `fps` rate (using a naïve `sleep`-based delay between frames).
In order for this to work, you will need to make sure you are staring the matching worker for your environment:

```elixir
# lib/my_app/application.ex

camera = Application.get_env(:picam, :camera, Picam.Camera)

children = [
  worker(camera, []),
  # ...
]
```

When using the `FakeCamera`, all the normal `Picam` API commands will be validated but silently ignored, with the following exceptions:

* `Picam.set_fps/1` will set the desired frame rate (in frames per second)
* `Picam.set_size/2` only has static images built-in for the following resolutions:

  * 1920 x 1080
  * 1280 x 720
  * 640 x 480

  If any other resolution is specified, the static image will default back to 1280 x 720.
  If you want to test with an image of a particular size or with specific image contents, you can specify your own image with `Picam.FakeCamera.set_image/1`, which accepts a JPEG-encoded binary.

  For example:

  ```elixir
  "image.jpg"
  |> File.read!()
  |> Picam.FakeCamera.set_image()
  ```

## Examples

The [examples] directory is where you can find other useful demos of `Picam` in action.  More examples will be added over time.

| Directory    | Demo   |
| ------------ | ------ |
| [picam_http] | Streaming MJPEG video using [plug] |

## Limitations

- Currently only one camera is supported.  If you intend to use more than one camera through an add-on compute module, please [submit an issue].

## Copyright and License

Copyright (c) 2013-2017, Broadcom Europe Ltd, Silvan Melchior, James Hughes, Frank Hunleth, Jeff Smith

Picam source code is licensed under the [BSD 3-Clause License].

[//]: #
[special ribbon cable]: <https://www.adafruit.com/product/3157>
[V1]: <https://www.raspberrypi.org/products/camera-module/>
[V2]: <https://www.raspberrypi.org/products/camera-module-v2/>
[Hex docs]: <https://hexdocs.pm/picam>
[examples]: <https://github.com/electricshaman/picam/tree/master/examples>
[picam_http]: <https://github.com/electricshaman/picam/tree/master/examples/picam_http>
[plug]: <https://hexdocs.pm/plug>
[submit an issue]: <https://github.com/electricshaman/picam/issues/new>
[BSD 3-Clause License]: <https://github.com/electricshaman/picam/blob/master/LICENSE>
