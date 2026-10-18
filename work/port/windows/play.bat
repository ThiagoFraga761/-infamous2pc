@echo off
rem inFAMOUS 2 port on Windows: the play.sh equivalent. Put this file, the
rem .exe and its DLLs in the folder that holds disc\ (the extracted game,
rem with disc\PS3_GAME\USRDIR\EBOOT.elf decrypted) and dev_flash\.
rem Settings are in user\settings.ini (the launcher and F1 write it).
setlocal
cd /d "%~dp0"
if not exist "disc\PS3_GAME\USRDIR\EBOOT.elf" (
    echo disc\PS3_GAME\USRDIR\EBOOT.elf not found next to this file.
    pause
    exit /b 1
)
set PS3RECOMP_VK_WINDOW=1
set PS3RECOMP_VK_GUEST_PROGRAMS=1
set PS3RECOMP_ENG_FPSLOG=1
set PS3_VERBOSE=0
if not exist logs mkdir logs
MyGameRecomp.exe disc\PS3_GAME\USRDIR\EBOOT.elf %* > logs\last.log 2>&1
endlocal
