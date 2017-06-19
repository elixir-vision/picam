# Picam

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
  - Display text annotations on the image frame(s)

For specifics on the above features, please consult the [Hex docs].

## Requirements

| Requirement |        | Notes  |
| ----------- | ------ | ------ |
| Host Device | Raspberry Pi 1, 2, 3, Zero, Zero W | Zero and Zero W require a [special ribbon cable] |
| Operating System  | Linux | Works out of the box with Raspian and Nerves builds |
| Camera Module | [V1], [V2] | Regular, NoIR |
| C Libraries | Broadcom VideoCore | Located in `/opt/vc` by default.  Override with `VIDEOCORE_DIR` |

## Installation
The package can be installed by adding `picam` to your list of dependencies in `mix.exs`:

```elixir
def deps do
  [{:picam, "~> 0.1.0"}]
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

More than likely you'll want to put the `Picam.Camera` process in your supervision tree rather than starting it manually:

```elixir
children = [
  worker(Picam.Camera, []),
  # ...
]
```

## Examples

The [examples] directory is where you can find other useful demos of `Picam` in action.  More examples will be added over time.  

| Directory    | Demo   |
| ------------ | ------ |
| [picam_http] | Streaming MJPEG video using [plug] |

## Limitations

- Currently only one camera is supported.  If you intend to use more than one camera through an add-on compute module, please submit an issue.

## Copyright and License

???

[//]: #
[special ribbon cable]: <https://www.adafruit.com/product/3157>
[V1]: <https://www.raspberrypi.org/products/camera-module/>
[V2]: <https://www.raspberrypi.org/products/camera-module-v2/>
[Hex docs]: <https://hexdocs.pm/picam>
[examples]: <https://github.com/electricshaman/picam/tree/master/examples>
[picam_http]: <https://github.com/electricshaman/picam/tree/master/examples/picam_http>
[plug]: <https://hexdocs.pm/plug>
