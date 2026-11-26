# Windows binaries

Extract the Windows x64 ZIP to a directory of your choice. Start
`bin/lincupl.exe` for the editor or `bin/linsim.exe` for the waveform viewer.
The command-line tools are in the same directory and run from PowerShell
or Command Prompt. Keep the DLLs and plugin directories beside them.

The binaries are built with Visual C++ 2022. The archive includes the
Visual C++ runtime and Qt libraries; no development environment is needed.

The archive includes `share/lincupl/lincupl.dl`, the default device library
with 660 device mnemonics. The tools find it beside `bin`; no download or
Python installation is needed to use the binaries.

To build from source, install Visual Studio 2022 with the C++ desktop
workload, CMake 3.21 or newer, Python 3 and a Qt 6 MSVC 2022 x64 kit. For example:

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64 -DCMAKE_PREFIX_PATH=C:/Qt/6.8.3/msvc2022_64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Qt is dynamically linked. Its license texts are in `licenses/Qt`; the
matching Qt source is available from <https://download.qt.io/archive/qt/>.
