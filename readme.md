## Running pipeline in C


### Pre requisites

This needs full installation of Deepstream as in  https://docs.nvidia.com/metropolis/deepstream/dev-guide/text/DS_Quickstart.html follow the appropriate steps for Jetson and dGPU in cloud.

For now we use Deepstream-6.2 on Jetson Orin and Deepstream-6.3 in Cloud

### Compile the code

goto the directory realtime/rtsp_restreamer and modify the Makefile depending on your platform. Select the appropriate lines for Cloud and Jetson. After selecting the correct lines use `make` command to
compile the codes and create the file `pipeline`.

If you already have something compiled and want to clean the directory use `make clean`
```
cd realtime/rtsp_restreamer
make clean
make
```

### Running the code

This has to be run from realtime folder (This change is done to enable stream launching from main.py)

if using H265 stream the flag `--stream-enc 1` should be added. 

To Run on Jetson board (disable hardware encoding)
```
./rtsp_restreamer/pipeline <rtsp-url> --enc-type 1 --camera-id <int> --port <port>
```

To Run on cloud (by default this will use hardware encoding)
```
./rtsp_restreamer/pipeline <rtsp-url> --camera-id <int> --port <port>
```

Information on flags:
```
Application Options:
  -e, --bbox-enable     0: Disable bboxes,  1: Enable bboxes,   Default: bboxes disabled
  -c, --enc-type    0: Hardware encoder,    1: Software encoder,    Default: Hardware encoder
  -m, --sr-mode     SR mode: 0 = Audio + Video, 1 = Video only, 2 = Audio only
  -p, --pgie-type   PGIE type: 0 = Nvinfer, 1 = Nvinferserver,  Default: Nvinfer
  -h, --stream-enc  Streram type: 0 = H264, 1 = H265,        Default: H264
  -i, --camera-id   Camera ID as an int
  -m, --mac     Mac address of a camera
  -r, --port    RTSP output port
  -s, --running-mode 1 = No RTSP output, 2 = Processed RTSP Output, Default: No RTSP Output
  -o, --motion 0: motion disabled, 1: motion enabled, Default: Disabled
  -a, --stream-record 0: disable stream recording, 1: enable stream recording, Default: stream record disabled
  -t, --record-chunk Stream record chunk size in seconds, Default: 10800 sec
  -n, --person-detection 0: Disable person detection, 1: Enable person detection, Default: Enabled
  -v, --vehicle-detection 0: Disable vehicle detection, 1: Enable vehicle Detection, Default: Disabled
```
The video clips will be saved to a folder at <camera-id> and processed RTSP stream will be available at `rtsp://localhost:<port>/ds-test`

Note: If the stream has some special characters in the rtsp url, that has to be escaped by add a single backslash (\\) 

### Viewing RTSP Streams 

Successful initiation of the ```cloud-pipeline.py``` will start a stream on `rtsp://localhost:<port>/ds-test`. As we have already exposed the port we can use another terminal to publish the RTSP stream to internet using ngrok.

#### Installing and using ngrok

Create an account in ngrok and follow the steps below.

Run the following commands to download and install ngork
```
wget https://bin.equinox.io/c/bNyj1mQVY4c/ngrok-v3-stable-linux-amd64.tgz
tar -xvf ngrok-v3-stable-linux-amd64.tgz
./ngrok config add-authtoken <your_token>
```

To expose the RTSP to ngrok run
```
./ngrok tcp 8554
```
This will expose the activities on port 8554 to a ngrok url (something like `tcp://0.tcp.eu.ngrok.io:11465`, but the port will differ each time you run this).

You can view the RTSP stream using VLC by replacing the address provided by ngrok. The public rtsp url will be `rtsp://<ngrok_url>/<rtsp_path_in_local_stream>` (For example `rtsp://0.tcp.eu.ngrok.io:11465/ds-test`)