WinGlass 1.2.0  -  global frosted glass for Windows 11
=====================================================

Browsers, terminals, file managers and chat apps all get a real acrylic
backdrop: the blur is composited live by the Windows compositor, and every
window is restored the moment you quit. No injection, no hooking, no patching.

This file is the readme for a built release. To build from source, see the
repository README instead.


Requirements
------------
Windows 11 (x64). No administrator rights, no .NET, no third-party runtime.


Getting started
---------------
1. Copy one of the example configurations to config.yaml:

       copy config.example.yaml config.yaml

2. Run winglass.exe.

There is no main window; the program only shows a notification-area icon.

The configuration is read from this folder, next to the executables, so keep
the files together. If there is no config.yaml the program falls back to
config.ini and then to its built-in defaults.


The three executables
---------------------
winglass.exe            The program itself. Run this one.
winglass-config.exe     Visual configuration editor. Saving writes config.yaml,
                        and the running program picks the change up by itself.
winglass-watchdog.exe   Crash recovery. Started automatically by winglass.exe -
                        do not run it yourself.


Tray menu
---------
Right-click the notification-area icon. The menu follows the configured
interface language (or the Windows display language when it is set to auto):

    打开配置编辑器      Open the config editor
    打开配置文件        Open the config file
    重新加载效果        Reload the effect
    启用/关闭开机启动    Enable / disable autostart
    退出                Exit

When a newer version is found, the menu also offers a link to its download
page. Update checks can be disabled with `check_updates: false` in config.yaml.

Autostart only writes the current user's Run key; it needs no administrator
rights.


Files created next to the executables
-------------------------------------
config.yaml             Your settings
config.yaml.bak         Previous configuration saved before editor replacement
winglass.state          Recovery journal - removed on a clean exit
winglass.log            Low-frequency diagnostics
winglass.startup-state  Cached autostart toggle state


Uninstalling
------------
Turn autostart off from the tray menu if you enabled it, exit from the tray,
then delete this folder.


Source, issues and updates
--------------------------
https://github.com/heathacosta259519-dot/WinGlass

Licensed under the MIT license - see LICENSE.
