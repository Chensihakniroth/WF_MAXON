@echo off
setlocal EnableExtensions EnableDelayedExpansion
set "ROOT=%~dp0"
set "SRC=%ROOT%src"
set "TESTS=%ROOT%tests"
set "BUILD=%ROOT%build"
set "OBJEXE=%BUILD%/obj/exe"
set "OBJDLL=%BUILD%/obj/dll"
set "OBJTEST=%BUILD%/obj/test"
set "OBJLOADER=%BUILD%/obj/loader"
set "VCVARS=D:/VS2022BT/VC/Auxiliary/Build/vcvars64.bat"
if not exist "%VCVARS%" (
  echo [WF_MAXON] ERROR: MSVC environment not found at %VCVARS%
  exit /b 2
)
call "%VCVARS%" >nul
if errorlevel 1 exit /b 3
for %%D in ("%BUILD%" "%OBJEXE%" "%OBJDLL%" "%OBJTEST%" "%OBJLOADER%") do if not exist "%%~D" mkdir "%%~D"
ml64 /nologo /c /Fo"%OBJEXE%/damage_context.obj" "%SRC%/damage_context.asm"
if errorlevel 1 (
  echo [WF_MAXON] CONTEXT ASM EXE BUILD FAILED
  exit /b 4
)
ml64 /nologo /c /Fo"%OBJDLL%/damage_context.obj" "%SRC%/damage_context.asm"
if errorlevel 1 (
  echo [WF_MAXON] CONTEXT ASM DLL BUILD FAILED
  exit /b 5
)
ml64 /nologo /c /Fo"%OBJDLL%/script_hook.obj" "%SRC%/script_hook.asm"
if errorlevel 1 (
  echo [WF_MAXON] SCRIPT HOOK ASM BUILD FAILED
  exit /b 15
)
set "LUA_SRC=%SRC%/lua"
set "LUA_OBJEXE=%BUILD%/obj/lua_exe"
set "LUA_OBJDLL=%BUILD%/obj/lua_dll"
for %%D in ("%LUA_OBJEXE%" "%LUA_OBJDLL%") do if not exist "%%~D" mkdir "%%~D"
set "LUA_FILES=lapi.cpp lcode.cpp lctype.cpp ldebug.cpp ldo.cpp ldump.cpp lfunc.cpp lgc.cpp llex.cpp lmem.cpp lobject.cpp lopcodes.cpp lparser.cpp lstate.cpp lstring.cpp ltable.cpp ltm.cpp lundump.cpp lvm.cpp lzio.cpp lauxlib.cpp lbaselib.cpp lmathlib.cpp lstrlib.cpp ltablib.cpp liolib.cpp loslib.cpp linit.cpp lua.cpp lstub.cpp"
echo [WF_MAXON] Compiling Lua core...
for %%F in (%LUA_FILES%) do (
  if not exist "%LUA_OBJDLL%/%%~nF.obj" (
    cl /nologo /std:c++20 /EHsc /O2 /W0 /D_CRT_SECURE_NO_WARNINGS /I"%LUA_SRC%" /c "%LUA_SRC%/%%F" /Fo"%LUA_OBJDLL%/%%~nF.obj"
    if errorlevel 1 (
      echo [WF_MAXON] LUA %%F BUILD FAILED
      exit /b 16
    )
  )
)
set "SOUP_FILES=vendor/Soup/soup/Asn1Identifier.cpp vendor/Soup/soup/Asn1Sequence.cpp vendor/Soup/soup/Bigint.cpp vendor/Soup/soup/Canvas.cpp vendor/Soup/soup/Capture.cpp vendor/Soup/soup/CpuInfo.cpp vendor/Soup/soup/Curve25519.cpp vendor/Soup/soup/DefaultRngInterface.cpp vendor/Soup/soup/DetachedScheduler.cpp vendor/Soup/soup/HardwareRng.cpp vendor/Soup/soup/HttpRequest.cpp vendor/Soup/soup/HttpRequestTask.cpp vendor/Soup/soup/IpAddr.cpp vendor/Soup/soup/JsonArray.cpp vendor/Soup/soup/JsonBool.cpp vendor/Soup/soup/JsonFloat.cpp vendor/Soup/soup/JsonInt.cpp vendor/Soup/soup/JsonNode.cpp vendor/Soup/soup/JsonNull.cpp vendor/Soup/soup/JsonObject.cpp vendor/Soup/soup/JsonString.cpp vendor/Soup/soup/LangDesc.cpp vendor/Soup/soup/LexemeParser.cpp vendor/Soup/soup/MimeMessage.cpp vendor/Soup/soup/Mixed.cpp vendor/Soup/soup/Oid.cpp vendor/Soup/soup/ParserState.cpp vendor/Soup/soup/Pattern.cpp vendor/Soup/soup/Process.cpp vendor/Soup/soup/ProcessHandle.cpp vendor/Soup/soup/Promise.cpp vendor/Soup/soup/QrCode.cpp vendor/Soup/soup/Range.cpp vendor/Soup/soup/RasterFont.cpp vendor/Soup/soup/Rc4State.cpp vendor/Soup/soup/Reader.cpp vendor/Soup/soup/Regex.cpp vendor/Soup/soup/RegexConstraint.cpp vendor/Soup/soup/RegexGroup.cpp vendor/Soup/soup/RegexGroupConstraint.cpp vendor/Soup/soup/ResolveIpAddrTask.cpp vendor/Soup/soup/Rgb.cpp vendor/Soup/soup/Scheduler.cpp vendor/Soup/soup/SelfDeletingThread.cpp vendor/Soup/soup/Server.cpp vendor/Soup/soup/SharedLibrary.cpp vendor/Soup/soup/Socket.cpp vendor/Soup/soup/SocketAddr.cpp vendor/Soup/soup/SocketTlsEncrypter.cpp vendor/Soup/soup/SocketTlsHandshaker.cpp vendor/Soup/soup/Task.cpp vendor/Soup/soup/Thread.cpp vendor/Soup/soup/TinyPngOut.cpp vendor/Soup/soup/TrustStore.cpp vendor/Soup/soup/Uri.cpp vendor/Soup/soup/Worker.cpp vendor/Soup/soup/Writer.cpp vendor/Soup/soup/X509Certchain.cpp vendor/Soup/soup/X509Certificate.cpp vendor/Soup/soup/X509RelativeDistinguishedName.cpp vendor/Soup/soup/adler32.cpp vendor/Soup/soup/aes.cpp vendor/Soup/soup/alloc.cpp vendor/Soup/soup/base.cpp vendor/Soup/soup/base32.cpp vendor/Soup/soup/base64.cpp vendor/Soup/soup/bitutil.cpp vendor/Soup/soup/crc32.cpp vendor/Soup/soup/crc32c.cpp vendor/Soup/soup/deflate.cpp vendor/Soup/soup/dnsHttpResolver.cpp vendor/Soup/soup/dnsName.cpp vendor/Soup/soup/dnsOsResolver.cpp vendor/Soup/soup/dnsRawResolver.cpp vendor/Soup/soup/dnsResolver.cpp vendor/Soup/soup/dnsSmartResolver.cpp vendor/Soup/soup/dnsType.cpp vendor/Soup/soup/dnsUdpResolver.cpp vendor/Soup/soup/dns_records.cpp vendor/Soup/soup/ecc.cpp vendor/Soup/soup/ffi.cpp vendor/Soup/soup/ffi_call_win_x64.cpp vendor/Soup/soup/filesystem.cpp vendor/Soup/soup/joaat.cpp vendor/Soup/soup/json.cpp vendor/Soup/soup/log.cpp vendor/Soup/soup/lzf.cpp vendor/Soup/soup/md5.cpp vendor/Soup/soup/memGuard.cpp vendor/Soup/soup/netConfig.cpp vendor/Soup/soup/netConnectTask.cpp vendor/Soup/soup/netStatus.cpp vendor/Soup/soup/os.cpp vendor/Soup/soup/parse_tree.cpp vendor/Soup/soup/pem.cpp vendor/Soup/soup/rand.cpp vendor/Soup/soup/rflParser.cpp vendor/Soup/soup/ripemd160.cpp vendor/Soup/soup/rsa.cpp vendor/Soup/soup/sha1.cpp vendor/Soup/soup/sha256.cpp vendor/Soup/soup/sha384.cpp vendor/Soup/soup/sha512.cpp vendor/Soup/soup/spaceship.cpp vendor/Soup/soup/string.cpp vendor/Soup/soup/time.cpp vendor/Soup/soup/unicode.cpp vendor/Soup/soup/urlenc.cpp vendor/Soup/soup/version_compare.cpp vendor/Soup/soup/wasm.cpp vendor/Soup/soup/whirlpool.cpp vendor/Soup/soup/xml.cpp
echo [WF_MAXON] Compiling Soup vendor...
for %%F in (!SOUP_FILES!) do (
  if not exist "%LUA_OBJDLL%/%%~nF.obj" (
    cl /nologo /std:c++20 /EHsc /O2 /W0 /D_CRT_SECURE_NO_WARNINGS /I"%LUA_SRC%" /c "%LUA_SRC%/%%F" /Fo"%LUA_OBJDLL%/%%~nF.obj"
    if errorlevel 1 (
      echo [WF_MAXON] SOUP %%F BUILD FAILED
      exit /b 17
    )
  )
)
set "IMGUI_SRC=%ROOT%third_party/imgui"
set "OBJIMGUI=%BUILD%/obj/imgui"
for %%D in ("%OBJIMGUI%") do if not exist "%%~D" mkdir "%%~D"

if not exist "%OBJIMGUI%/imgui.obj" (
  echo [WF_MAXON] Compiling ImGui...
  cl /nologo /std:c++20 /EHsc /O2 /W3 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /I"%IMGUI_SRC%" /I"%IMGUI_SRC%/backends" /c "%IMGUI_SRC%/imgui.cpp" "%IMGUI_SRC%/imgui_draw.cpp" "%IMGUI_SRC%/imgui_tables.cpp" "%IMGUI_SRC%/imgui_widgets.cpp" "%IMGUI_SRC%/backends/imgui_impl_win32.cpp" "%IMGUI_SRC%/backends/imgui_impl_dx11.cpp" /Fo"%OBJIMGUI%/"
  if errorlevel 1 (
    echo [WF_MAXON] IMGUI BUILD FAILED
    exit /b 18
  )
)

set "COMMON=/nologo /std:c++20 /EHsc /O2 /W4 /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 /D_CRT_SECURE_NO_WARNINGS /I"%IMGUI_SRC%" /I"%IMGUI_SRC%/backends""
set "GUILIBS=user32.lib gdi32.lib shell32.lib shlwapi.lib dwmapi.lib comdlg32.lib ole32.lib ws2_32.lib crypt32.lib advapi32.lib d3d11.lib dxgi.lib"
cl %COMMON% /I"%SRC%" /c "%SRC%/main.cpp" /Fo"%OBJEXE%/main.obj"
cl %COMMON% /I"%SRC%" /c "%SRC%/config.cpp" /Fo"%OBJEXE%/config.obj"
cl %COMMON% /I"%SRC%" /c "%SRC%/engine.cpp" /Fo"%OBJEXE%/engine.obj"
cl %COMMON% /I"%SRC%" /c "%SRC%/script_engine.cpp" /Fo"%OBJEXE%/script_engine.obj"
cl %COMMON% /I"%SRC%" /c "%SRC%/imgui_layer.cpp" /Fo"%OBJEXE%/imgui_layer.obj"
link /nologo /SUBSYSTEM:WINDOWS /OUT:"%BUILD%/WF_MAXON.exe" "%OBJEXE%/main.obj" "%OBJEXE%/config.obj" "%OBJEXE%/engine.obj" "%OBJEXE%/script_engine.obj" "%OBJEXE%/imgui_layer.obj" "%OBJIMGUI%/*.obj" "%LUA_OBJDLL%/*.obj" "%OBJEXE%/damage_context.obj" "%OBJDLL%/script_hook.obj" %GUILIBS%
if errorlevel 1 (
  echo [WF_MAXON] EXE BUILD FAILED
  exit /b 4
)
cl %COMMON% /DWF_MAXON_DLL /I"%SRC%" /c "%SRC%/main.cpp" /Fo"%OBJDLL%/main.obj"
cl %COMMON% /DWF_MAXON_DLL /I"%SRC%" /c "%SRC%/config.cpp" /Fo"%OBJDLL%/config.obj"
cl %COMMON% /DWF_MAXON_DLL /I"%SRC%" /c "%SRC%/engine.cpp" /Fo"%OBJDLL%/engine.obj"
cl %COMMON% /DWF_MAXON_DLL /I"%SRC%" /c "%SRC%/bootstrap.cpp" /Fo"%OBJDLL%/bootstrap.obj"
cl %COMMON% /DWF_MAXON_DLL /I"%SRC%" /c "%SRC%/script_engine.cpp" /Fo"%OBJDLL%/script_engine.obj"
cl %COMMON% /DWF_MAXON_DLL /I"%SRC%" /c "%SRC%/imgui_layer.cpp" /Fo"%OBJDLL%/imgui_layer.obj"
link /nologo /DLL /OUT:"%BUILD%/WF_MAXON.dll" /IMPLIB:"%BUILD%/WF_MAXON.lib" "%OBJDLL%/main.obj" "%OBJDLL%/config.obj" "%OBJDLL%/engine.obj" "%OBJDLL%/bootstrap.obj" "%OBJDLL%/script_engine.obj" "%OBJDLL%/imgui_layer.obj" "%OBJIMGUI%/*.obj" "%LUA_OBJDLL%/*.obj" "%OBJDLL%/damage_context.obj" "%OBJDLL%/script_hook.obj" %GUILIBS%
if errorlevel 1 (
  echo [WF_MAXON] DLL BUILD FAILED
  exit /b 5
)
cl %COMMON% /Fo"%OBJLOADER%/loader.obj" "%SRC%/loader.cpp" /link /nologo /SUBSYSTEM:WINDOWS /OUT:"%BUILD%/WF_MAXONLoader.exe" user32.lib shlwapi.lib advapi32.lib
if errorlevel 1 (
  echo [WF_MAXON] LOADER BUILD FAILED
  exit /b 6
)
cl %COMMON% /Fo"%OBJTEST%/test_host.obj" "%TESTS%/test_host.cpp" /link /nologo /SUBSYSTEM:CONSOLE /OUT:"%BUILD%/test_host.exe" user32.lib
if errorlevel 1 (
  echo [WF_MAXON] TEST HOST BUILD FAILED
  exit /b 7
)
ml64 /nologo /c /Fo"%OBJTEST%/xmm_probe.obj" "%TESTS%/xmm_probe.asm"
if errorlevel 1 (
  echo [WF_MAXON] XMM PROBE ASM BUILD FAILED
  exit /b 10
)
cl %COMMON% "%TESTS%/damage_context_unit.cpp" "%OBJDLL%/damage_context.obj" "%OBJTEST%/xmm_probe.obj" /link /nologo /SUBSYSTEM:CONSOLE /OUT:"%BUILD%/damage_context_unit.exe"
if errorlevel 1 (
  echo [WF_MAXON] CONTEXT UNIT BUILD FAILED
  exit /b 10
)
"%BUILD%\damage_context_unit.exe"
if errorlevel 1 (
  echo [WF_MAXON] CONTEXT UNIT FAILED
  exit /b 11
)
cl %COMMON% /I"%SRC%" "%TESTS%/damage_policy_unit.cpp" /link /nologo /SUBSYSTEM:CONSOLE /OUT:"%BUILD%/damage_policy_unit.exe"
if errorlevel 1 (
  echo [WF_MAXON] DAMAGE POLICY UNIT BUILD FAILED
  exit /b 13
)
"%BUILD%\damage_policy_unit.exe"
if errorlevel 1 (
  echo [WF_MAXON] DAMAGE POLICY UNIT FAILED
  exit /b 14
)
echo [WF_MAXON] BUILD SUCCEEDED
for %%I in ("%BUILD%\WF_MAXON.exe") do echo [WF_MAXON] CONTROLLER %%~zI bytes
for %%I in ("%BUILD%\WF_MAXON.dll") do echo [WF_MAXON] PAYLOAD    %%~zI bytes
for %%I in ("%BUILD%\WF_MAXONLoader.exe") do echo [WF_MAXON] LOADER     %%~zI bytes
"%BUILD%\WF_MAXON.exe" --selftest "%BUILD%\wf_maxon_selftest.log"
if errorlevel 1 (
  echo [WF_MAXON] CONFIG SELFTEST FAILED
  type "%BUILD%\wf_maxon_selftest.log"
  exit /b 8
)
type "%BUILD%\wf_maxon_selftest.log"
set "WF_MAXON_GAME_DIR=%TEMP%/WF_MAXON_selftest"
set "WF_MAXON_DISABLE_LIVE_APPLY=1"
"%BUILD%\test_host.exe" "%BUILD%\WF_MAXON.dll"
set "WF_MAXON_DISABLE_LIVE_APPLY="
if errorlevel 1 (
  echo [WF_MAXON] PAYLOAD SMOKE TEST FAILED
  exit /b 9
)
echo [WF_MAXON] ALL TESTS PASSED
endlocal
