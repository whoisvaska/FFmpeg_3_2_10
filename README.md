# Build steps using MSVS 2022, MSYS2, YASM for amd64 (x86_64) architecture:
1) Install MSYS2
2) Install YASM
3) Open x64_x86 Cross Tools Command Prompt for VS 2022
4) Run these commands to enable the 64bit compiler and linker:
```
C:\> cd "C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC\"
C:\Program Files (x86)\Microsoft Visual Studio 14.0\VC> vcvarsall.bat amd64
```
5) Open msys2_shell.cmd from the Command Prompt:
```
cd C:\msys64
C:\msys64> msys2_shell.cmd
```
6) Set msys path to use proper cl.exe, link.exe:
```
$ export PATH="/c/Program Files (x86)/Microsoft Visual Studio 14.0/VC/BIN/amd64/":$PATH
$ which cl.exe
$ which link.exe
```
Example output:
```
/c/Program Files (x86)/Microsoft Visual Studio 14.0/VC/BIN/amd64/cl.exe
/c/Program Files (x86)/Microsoft Visual Studio 14.0/VC/BIN/amd64/link.exe
```
7) Set msys path to use YASM:
```
$ export PATH="/c/yasm/":$PATH
```
Example output:
```
$ yasm
yasm: No input files specified
```

8) Clone repository and cd to the folder:
```
 /d/Projects
$ git clone https://github.com/whoisvaska/FFmpeg_3_2_10.git 
 
 /d/Projects
$ cd FFmpeg_3_2_10/
```
9) Checkout to 3.2.10 version:
```
 /d/Projects/FFmpeg_3_2_10
$ git checkout tags/n3.2.10

/d/Projects/FFmpeg_3_2_10
$ git branch

```
Example output:
```
* (HEAD detached at n3.2.10)
  release/3.2
```
10) Configure using win64 as target OS, specify setting to build static libraries:
```
  /d/Projects/FFmpeg_3_2_10
$ ./configure --target-os=win64 --arch=amd64 --toolchain=msvc --enable-static --datadir=$PWD --prefix="./install/"
```
Example output:
```
install prefix            ./install/
source path               .
C compiler                cl
C library                 msvcrt
```
11) Run make and wait:
```
/d/Projects/FFmpeg_3_2_10
$ make
```
Example output:
```
LD      ffmpeg_g.exe
CP      ffmpeg.exe
STRIP   ffmpeg.exe
skipping strip ffmpeg.exe
```
12) Run make install:
```
  /d/Projects/FFmpeg_3_2_10
$ make install
```
Example output:
```
INSTALL doc/ffmpeg.1
INSTALL doc/ffprobe.1
INSTALL doc/ffmpeg-all.1
INSTALL doc/ffprobe-all.1
```
13) Check if installation script generated needed dirs/files:
```
  /d/Projects/FFmpeg_3_2_10
$ cd install/
  
  /d/Projects/FFmpeg_3_2_10/install
$ ls

  /d/Projects/FFmpeg_3_2_10/install
$ cd lib

 /d/Projects/FFmpeg_3_2_10/install/lib
$ ls
```
Example output:
```
bin  include  lib  share

libavcodec.a   libavfilter.a  libavutil.a      libswscale.a
libavdevice.a  libavformat.a  libswresample.a  pkgconfig
```


FFmpeg README
=============

FFmpeg is a collection of libraries and tools to process multimedia content
such as audio, video, subtitles and related metadata.

## Libraries

* `libavcodec` provides implementation of a wider range of codecs.
* `libavformat` implements streaming protocols, container formats and basic I/O access.
* `libavutil` includes hashers, decompressors and miscellaneous utility functions.
* `libavfilter` provides a mean to alter decoded Audio and Video through chain of filters.
* `libavdevice` provides an abstraction to access capture and playback devices.
* `libswresample` implements audio mixing and resampling routines.
* `libswscale` implements color conversion and scaling routines.

## Tools

* [ffmpeg](https://ffmpeg.org/ffmpeg.html) is a command line toolbox to
  manipulate, convert and stream multimedia content.
* [ffplay](https://ffmpeg.org/ffplay.html) is a minimalistic multimedia player.
* [ffprobe](https://ffmpeg.org/ffprobe.html) is a simple analysis tool to inspect
  multimedia content.
* [ffserver](https://ffmpeg.org/ffserver.html) is a multimedia streaming server
  for live broadcasts.
* Additional small tools such as `aviocat`, `ismindex` and `qt-faststart`.

## Documentation

The offline documentation is available in the **doc/** directory.

The online documentation is available in the main [website](https://ffmpeg.org)
and in the [wiki](https://trac.ffmpeg.org).

### Examples

Coding examples are available in the **doc/examples** directory.

## License

FFmpeg codebase is mainly LGPL-licensed with optional components licensed under
GPL. Please refer to the LICENSE file for detailed information.

## Contributing

Patches should be submitted to the ffmpeg-devel mailing list using
`git format-patch` or `git send-email`. Github pull requests should be
avoided because they are not part of our review process and will be ignored.
