# RTSP到VLC视频流转发工具

这是一个简单的C语言程序，用于从RTSP摄像头拉取视频流并将其转发到本地UDP端口，以便使用VLC播放器进行查看。

## 功能特点

- 从RTSP摄像头拉取视频流
- 使用TCP传输协议获取RTSP流（更稳定）
- 将视频流转发到本地UDP端口
- 支持与VLC播放器集成

## 系统要求

- Linux操作系统
- FFmpeg开发库（libavformat, libavcodec, libavutil, libswscale）
- GCC编译器
- VLC媒体播放器（用于查看视频流）

## 安装依赖

在Debian/Ubuntu系统上安装所需的依赖：

```bash
sudo apt-get update
sudo apt-get install build-essential libavformat-dev libavcodec-dev libavutil-dev libswscale-dev vlc
```

在CentOS/RHEL系统上：

```bash
sudo yum install gcc make ffmpeg-devel vlc
```

## 编译

使用提供的Makefile进行编译：

```bash
make
```

或者手动编译：

```bash
gcc -o rtsp_to_vlc rtsp_to_vlc.c -lavformat -lavcodec -lavutil -lswscale
```

## 使用方法

1. 运行程序，提供RTSP URL作为参数：

```bash
./rtsp_to_vlc rtsp://用户名:密码@摄像头IP地址:端口/流路径
```


## 自定义设置

如果需要更改输出UDP地址或端口，请修改源代码中的 `OUTPUT_UDP` 宏定义。

## 故障排除

- 如果连接RTSP流失败，请检查URL、用户名和密码是否正确
- 确保摄像头可以从您的网络访问
- 检查防火墙设置，确保允许RTSP和UDP流量

## 许可证

MIT

