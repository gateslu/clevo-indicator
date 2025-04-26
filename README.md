Clevo Fan Control Indicator for Ubuntu
======================================

This program is an Ubuntu indicator to control the fan of Clevo laptops, using reversed-engineering port information from ECView.

It shows the CPU temperature on the left and the GPU temperature on the right, and a menu for manual control.

支持蓝天clevo-x370，我的笔记本型号是七彩虹将星x17 pro max，目前支持cpu和gpu的自动风扇调速，并且可以通过配置文件自由组合

![Clevo Indicator Screen](http://i.imgur.com/ucwWxLq.png)

```
sudo chown root clevo-indicator  && sudo chgrp adm clevo-indicator && sudo chmod 4750 clevo-indicator
```

For command-line, use *-h* to display help, or a number representing percentage of fan duty to control the fan (from 40% to 100%).


Build and Install
-----------------
For Ubuntu 22.04:
```shell
sudo apt install libayatana-appindicator3-dev libgtk-3-dev install nlohmann-json3-dev
git clone https://github.com/grizzlei/clevo-indicator.git
cd clevo-indicator
mkdir cmake-build-release
cd cmake-build-release
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
sudo chown root clevo-indicator  && sudo chgrp adm clevo-indicator && sudo chmod 4750 clevo-indicator
sudo rm -rf /usr/local/bin/clevo-indicator
sudo make install
sudo chown root /usr/local/bin/clevo-indicator && sudo chgrp adm /usr/local/bin/clevo-indicator && sudo chmod 4750 /usr/local/bin/clevo-indicator
```
For older Ubuntu versions:
```shell
sudo apt-get install libappindicator3-dev libgtk-3-dev install nlohmann-json3-dev
git clone https://github.com/SkyLandTW/clevo-indicator.git
cd clevo-indicator
mkdir cmake-build-release
cd cmake-build-release
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
sudo chown root clevo-indicator && sudo chgrp adm clevo-indicator && sudo chmod 4750 clevo-indicator
sudo rm -rf /usr/local/bin/clevo-indicator
sudo make install
sudo chown root /usr/local/bin/clevo-indicator && sudo chgrp adm /usr/local/bin/clevo-indicator && sudo chmod 4750 /usr/local/bin/clevo-indicator
```

---

### Manual Fan Control Configuration

If you want to manually set custom fan control rules, create a configuration file at `/etc/fan_config.json` with the following content:

```json
{
    "cpu": [
        { "temp": 10, "duty": 0 },
        { "temp": 20, "duty": 20 },
        { "temp": 30, "duty": 25 },
        { "temp": 40, "duty": 35 },
        { "temp": 50, "duty": 45 },
        { "temp": 60, "duty": 60 },
        { "temp": 70, "duty": 75 },
        { "temp": 80, "duty": 85 },
        { "temp": 90, "duty": 100 }
    ],
    "gpu": [
        { "temp": 10, "duty": 0 },
        { "temp": 20, "duty": 20 },
        { "temp": 30, "duty": 25 },
        { "temp": 40, "duty": 30 },
        { "temp": 50, "duty": 35 },
        { "temp": 60, "duty": 45 },
        { "temp": 70, "duty": 60 },
        { "temp": 80, "duty": 75 },
        { "temp": 90, "duty": 90 },
        { "temp": 95, "duty": 100 }
    ]
}
```

Each entry defines a mapping between temperature (`temp`) and the corresponding fan duty cycle (`duty`, in percentage).  
The system will adjust the CPU and GPU fan speeds based on the temperature according to these mappings.

Make sure that `/etc/fan_config.json` is readable by the program.
---


Notes
-----

The executable has setuid flag on, but must be run by the current desktop user,
because only the desktop user is allowed to display a desktop indicator in
Ubuntu, while a non-root user is not allowed to control Clevo EC by low-level
IO ports. The setuid=root creates a special situation in which this program can
fork itself and run under two users (one for desktop/indicator and the other
for EC control), so you could see two processes in ps, and killing either one
of them would immediately terminate the other.

Be careful not to use any other program accessing the EC by low-level IO
syscalls (inb/outb) at the same time - I don't know what might happen, since
every EC actions require multiple commands to be issued in correct sequence and
there is no kernel-level protection to ensure each action must be completed
before other actions can be performed... The program also attempts to prevent
abortion while issuing commands by catching all termination signals except
SIGKILL - don't kill the indicator by "kill -9" unless absolutely necessary.

