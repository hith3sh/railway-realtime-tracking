## Running Recording Pipeline


### Compile the code using the make file

Run the makefile using 'make'
```
make
```

### Running the code

Run the recording_pipeline using following command

```
./recording_pipeline <rtsp-url>
```

Information on flags:
```
Application Options:
    -h, --stream-enc    0 = H264, 1 = H265,     Default: H264
    -m, --mac   Mac address of a camera
    -t, --record-chunk      Stream record chunk size in seconds,     Default: 300 sec
```

### To View Info Logs

Please execute following commands on the terminal that you're going to run the recording_pipeline to view LOG(INFO) level logs
```
export GLOG_logtostderr=1
export GLOG_v=2
```