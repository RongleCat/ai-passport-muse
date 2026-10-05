<!--
Copyright (c) Meta Platforms, Inc. and affiliates.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# AI Passport Muse

这是 [facebookincubator/muse-gadget-sdk](https://github.com/facebookincubator/muse-gadget-sdk) 的分支，默认分支 `passport-port` 用来给 **FoloToy AI Passport** 编译 Muse 固件。硬件是 ESP32-C3、8 MB 闪存、没有 PSRAM。屏幕上有中文提示，按住 OK 录音，松开后发送，回复以文字显示。没有语音播报、没有图片推送、也没有家庭网络隧道。

许可证是 Apache-2.0，见 [`LICENSE`](LICENSE)。上游的第三方文件仍用它们原来的许可证，Jollybot 头像不在 Apache-2.0 之内。刷入自制固件可能变砖，也可能影响保修。

板级引脚和内存测量在 [`esp32/devices/passport.md`](esp32/devices/passport.md)。

<p align="center">
  <a href="https://x.com/cgnot996"><img src="https://img.shields.io/badge/X-铁柱AGI%20%40cgnot996-black?logo=x&logoColor=white" alt="X 铁柱AGI" /></a>
  <img src="https://img.shields.io/badge/微信公众号-铁柱AGI-07C160?logo=wechat&logoColor=white" alt="微信公众号 铁柱AGI" />
</p>

<p align="center">
  <a href="https://x.com/cgnot996">关注 X @cgnot996</a>
  ·
  微信搜一搜「铁柱AGI」，或扫下面的码关注公众号
</p>

<p align="center">
  <img src="assets/wechat/mp-search-scan.png" alt="微信搜一搜 铁柱AGI，扫码关注公众号" width="420" />
</p>

## 准备

- 一块 FoloToy AI Passport，以及一根能传数据的 USB 线。
- macOS 或 Linux。
- [ESP-IDF v6.0.1](https://docs.espressif.com/projects/esp-idf/en/v6.0.1/esp32c3/get-started/index.html)，并且安装 `esp32c3` 工具链。其他版本没有验证过。
- Muse 账号里的 SDK token：[gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens)（Account > SDK tokens）。使用前阅读 [Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms)。
- 手机上的 Muse App。配对时打开 Settings > Devices > Developer mode。

安装工具链：

```sh
git clone -b v6.0.1 --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf-v6
~/esp/esp-idf-v6/install.sh esp32c3
. ~/esp/esp-idf-v6/export.sh
```

每开一个新终端都要再执行一次 `export.sh`。

拉下这份代码：

```sh
git clone https://github.com/RongleCat/ai-passport-muse.git
cd ai-passport-muse
```

## 编译

在 `esp32/build-passport/sdkconfig.local` 里写你自己的 token。这个目录被 git 忽略。不要把 token、Wi-Fi 名称、Wi-Fi 密码写进 `devices/sdkconfig.muse-passport` 或任何会提交的文件。

```sh
mkdir -p esp32/build-passport
cat > esp32/build-passport/sdkconfig.local << 'EOF'
CONFIG_GADGET_SDK_TOKEN="mgst_把你的 token 贴在这里"
EOF
```

局域网能不能直接到达 Muse，决定要不要设置代理。这两个变量在每次编译时读取，不会写进仓库。不要在里面放账号或密码。

1. 软路由已经做了透明代理。不要设置 `MUSE_HTTP_PROXY_HOST`。固件直连，流量由软路由转发。
2. 没有透明代理。找一台局域网里已经开着代理、并且允许其他设备使用的机器（例如打开了 Allow LAN 的 Clash），写它的 IPv4 地址和端口。不支持域名。端口省略时默认 7890。

```sh
cd esp32
. ~/esp/esp-idf-v6/export.sh
export MUSE_HTTP_PROXY_HOST=192.168.1.10
export MUSE_HTTP_PROXY_PORT=7890
idf.py -B build-passport -DIDF_TARGET=esp32c3 \
  -DSDKCONFIG=build-passport/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-passport;build-passport/sdkconfig.local" \
  build
```

软路由用户删掉两行 `export`。漏设 `MUSE_HTTP_PROXY_HOST` 再编译，得到的是直连固件。代理开着时，固件先向这台机器发 HTTP CONNECT，再做 TLS。代理看得到目标主机名，看不到请求正文。

不要用 `tools/muse/board.sh build` 来编这个 `build-passport` 目录。那个脚本会删掉 `managed_components`。

## 刷入

先确认按键都松开。GPIO0 既是三个按键，也是芯片的启动脚，复位时按住会进入下载模式。查端口：macOS 用 `ls /dev/cu.usb*`，Linux 用 `ls /dev/ttyACM*`。

```sh
cd esp32/build-passport
python -m esptool --chip esp32c3 -p /dev/cu.usbmodem1101 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash @flash_args
```

把端口换成你机器上的那个。`flash_args` 里已经是 `dio`、80 MHz、`--flash-size keep`，以及这些地址：bootloader `0x0`，分区表 `0x10000`，OTA 数据 `0x1d000`，应用 `0x20000`。每一段都要看到 `Hash of data verified`。

不要用 `idf.py flash`。ESP-IDF 6.0.1 会把 `SERIAL_TOOL` 拆开，这条命令在这里不可靠。也不要加 `--erase-all`，不要运行 `idf.py erase-flash`。那会清掉 NVS 里的配对、Wi-Fi 和屏幕设置。平常升级只覆盖上面的四段，配对会留下来。

打开串口经常会让板子复位。配对过程中不要开串口。看日志可以在刷完之后执行：

```sh
. ~/esp/esp-idf-v6/export.sh
cd esp32
idf.py -p /dev/cu.usbmodem1101 -B build-passport monitor
```

用 `Ctrl-]` 退出。

## 第一次使用

已经配对过的设备，刷完会自己连上原来的 Wi-Fi。没配对过的，开机后自己用蓝牙广播，不用先按键。屏幕上的名字是 `MuseGadget-` 加六位十六进制，和手机里看到的是同一个。

在 Muse App 里：

1. 打开 Settings > Devices > Developer mode。
2. 点右上角的 **+**（Settings > Devices > Add Device），选屏幕上的那个名字。
3. 屏幕弹出「配对码」，中间是六位数字，下面写着「在手机上输入」。把这六位填进手机。
4. 卡片换成「与 Muse 应用配对」，中间是 **OK**，下面写着「按下这个键确认」。一分钟内按一下 OK。这一下只确认配对，不会开始录音。超过一分钟，手机断开，配对卡片消失，从第 2 步再来。
5. 然后在 App 里按提示选择 Wi-Fi。设备连上这个网络，再连上 Muse。连上以后，主页中间显示「就绪」。

「配对码」或「与 Muse 应用配对」还在屏幕上时，UP 和 DOWN 不会打开菜单，也不会打开上一条回复。配对过程中不要开串口。USB 一开，板子经常会复位，这一轮就断了。

配好之后：

- 按住 OK 录音，松开后发送。等这条回复出现在屏幕上，再按下一次。
- 主页短按 UP 打开菜单，短按 DOWN 看上一条回复。进了菜单以后，UP / DOWN 移动，OK 确认。
- 要重新配对，短按 UP 打开菜单，选「重置配对」，再按 OK。屏幕会问是否忘记 Wi-Fi 和 Muse 应用的配对，然后重启。这块板没有“长按 5 秒恢复出厂”的手势。

## 注意事项

- 只在你信任的网络上配对。这是社区固件，配对没有厂商证书，挡不住同一网络里的中间人。
- token、Wi-Fi 密码、代理地址只留在本机的 `build-passport/` 或当前终端的环境变量里。
- 每次编译都会重新看 `MUSE_HTTP_PROXY_HOST`。要换直连或换代理机器，改环境变量后重新编译、重新刷入。
- 没有 PSRAM。回复是屏幕上的文字，不是喇叭里的声音。
- 不要改分区表，也不要改这四个刷写地址。恢复出厂需要你自己事先保存的整片备份，从 `0x0` 整片写回，不要改它的布局。
- 刷机失败时先松开按键再试。仍然连不上，按住按键、点一下复位、再松开，让芯片进入下载模式。

## Build and flash

This branch builds Muse firmware for the FoloToy AI Passport, an ESP32-C3 with 8 MB of flash and no PSRAM. Hold OK to record, release to send, and read the text reply on screen. There is no spoken reply, image push, or home-network tunnel. The license is Apache-2.0; see [`LICENSE`](LICENSE). Pin-level notes are in [`esp32/devices/passport.md`](esp32/devices/passport.md).

Install ESP-IDF v6.0.1 with the `esp32c3` tools, then in every new shell run `. ~/esp/esp-idf-v6/export.sh`. Clone this repository and put your SDK token in the gitignored file `esp32/build-passport/sdkconfig.local`:

```sh
mkdir -p esp32/build-passport
printf '%s\n' 'CONFIG_GADGET_SDK_TOKEN="mgst_paste_your_token_here"' \
  > esp32/build-passport/sdkconfig.local
```

Get the token from [gadgets.muse.ai](https://gadgets.muse.ai/settings/sdk-tokens). If a soft router already transparent-proxies the LAN, leave `MUSE_HTTP_PROXY_HOST` unset. Otherwise set it to the IPv4 address of a LAN machine whose proxy accepts clients, and set `MUSE_HTTP_PROXY_PORT` (default 7890). A hostname does not work. The variables are read on every build and are not stored in git.

```sh
cd esp32
export MUSE_HTTP_PROXY_HOST=192.168.1.10
export MUSE_HTTP_PROXY_PORT=7890
idf.py -B build-passport -DIDF_TARGET=esp32c3 \
  -DSDKCONFIG=build-passport/sdkconfig \
  -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;devices/sdkconfig.muse;devices/sdkconfig.muse-passport;build-passport/sdkconfig.local" \
  build
cd build-passport
python -m esptool --chip esp32c3 -p /dev/cu.usbmodem1101 -b 460800 \
  --before default-reset --after hard-reset \
  write-flash @flash_args
```

Release every button before flashing. GPIO0 is the key ladder and the boot strap. Do not use `idf.py flash` with ESP-IDF 6.0.1 here. Do not pass `--erase-all` or run `erase-flash`; that wipes pairing, Wi-Fi, and screen settings in NVS. Do not run `tools/muse/board.sh build` against this tree; it deletes `managed_components`. Opening the serial port often resets the board, so leave it closed while pairing.

### Pairing

A board that is already paired reconnects to its saved Wi-Fi after a reflash that leaves NVS alone. An unpaired board advertises on its own. The screen shows `MuseGadget-` plus six hex digits, the same name the app lists.

In the Muse app:

1. Turn on **Settings > Devices > Developer mode**.
2. Add a device (**Settings > Devices > Add Device**, the **+** in the top right) and pick the name on the screen.
3. The screen shows **配对码**, six digits, and **在手机上输入**. Type those digits on the phone.
4. The card changes to **与 Muse 应用配对**, with **OK** and **按下这个键确认**. Press OK once within a minute. That press only confirms pairing. It does not start a recording. After a minute the phone disconnects, the card goes away, and you start again from step 2.
5. Choose the Wi-Fi network when the app asks. The board joins it and then connects to Muse. Once that is up, the home screen shows **就绪**.

While either card is up, UP and DOWN do not open the menu or the previous reply.

Hold OK to record and release to send. On the home screen a short UP opens the menu and a short DOWN shows the previous reply. Inside the menu, UP and DOWN move and OK confirms. To pair again, open the menu and choose **重置配对**, then press OK. There is no five-second reset.

Follow [铁柱AGI on X](https://x.com/cgnot996) (@cgnot996). On WeChat, search 铁柱AGI or scan the image in the Chinese section above.

## 上游项目：Muse Gadgets

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset=".github/images/muse-gadgets-dark.png">
    <img src=".github/images/muse-gadgets-light.png" width="900" alt="Muse gadgets: a Waveshare round AMOLED, an M5Stack StickS3, Muse Home Link, a Raspberry Pi and a Seeed reTerminal e-ink display">
  </picture>
</p>

Muse gadgets are open source devices you build yourself. Program an
off-the-shelf ESP32 board or set up a Raspberry Pi with our device SDKs, then
connect Muse to your displays, buttons, sensors, actuators, and whatever else
you've got lying on your workbench.

We open sourced the SDKs and firmware here. It's built by hackers, for hackers,
just for fun. Side effects of tinkering may include bricked boards, voided
warranties, brownouts, or bankruptcies. Proceed at your own risk!

| | |
|---|---|
| [**ESP32 Device SDK**](esp32) | Connect your ESP32 board to Muse through our open source SDK. Throw in a screen to show images, add audio in and out, or wire up other sensors. |
| [**Linux Device SDK**](linux) | Turn that spare Raspberry Pi or Linux box into a Muse gadget. Hack in your own commands to let Muse handle sysadmin chores or your Home Assistant setup. |

Before you flash or pair a gadget, get an
[SDK token](https://gadgets.muse.ai/settings/sdk-tokens) and review the
[Gadget SDK Terms](https://gadgets.muse.ai/sdk-terms). Every gadget needs a
token to pair.

ESP32 and Linux gadgets pair with the Muse app on iOS and Android, via
Settings > Devices. Turn on Developer mode there first, then look for devices
prefixed with "MuseGadget".
Each directory has a `README.md` to get started and an `AGENTS.md` for coding
agents like [Muse Code](https://developer.meta.com/ai/lp/muse-code/).

## Community

Meet other hackers who are building and customizing Muse gadgets in our
community [Discord](https://discord.gg/3bhjCkZdd6). Get inspired, support each
other, and share what you make.

## License

Muse Gadgets is licensed under the Apache License, Version 2.0, found in
[`LICENSE`](LICENSE), except for these third-party files, which keep their
upstream licenses:

| Path | Upstream | License |
|---|---|---|
| [`esp32/components/minimp3/include/minimp3.h`](esp32/components/minimp3) | [lieff/minimp3](https://github.com/lieff/minimp3) | CC0-1.0, see [`LICENSE`](esp32/components/minimp3/LICENSE) |
| [`esp32/main/pixel_font.c`](esp32/main/pixel_font.c) | Adafruit GFX `glcdfont.c` | BSD-2-Clause, in the file header |

Dependencies fetched at build time are under their own licenses: ESP-IDF
components (into `esp32/managed_components/`), and the simulator's LVGL and
SDL (listed in [`esp32/simulator/THIRD_PARTY.md`](esp32/simulator/THIRD_PARTY.md)).

The Apache License does not cover the [Jollybot avatar](esp32/avatar).
