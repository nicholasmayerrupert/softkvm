; SoftKVM per-user installer (no admin rights needed). Built by pc-client/package.sh:
;   makensis -DVERSION=x.y.z -DDIST=<dist dir> -DOUTFILE=<setup.exe> -DICON=<ico> softkvm.nsi
Unicode true
!include "MUI2.nsh"

Name "SoftKVM"
Caption "SoftKVM ${VERSION} Setup"
OutFile "${OUTFILE}"
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\SoftKVM"
InstallDirRegKey HKCU "Software\SoftKVM" "InstallDir"
BrandingText "SoftKVM ${VERSION}"
SetCompressor /SOLID lzma

!define MUI_ICON "${ICON}"
!define MUI_UNICON "${ICON}"
!define MUI_ABORTWARNING
!define MUI_WELCOMEPAGE_TEXT "This installs SoftKVM ${VERSION}, which lets this PC's monitors, keyboard and mouse control your other computers.$\r$\n$\r$\nInstall SoftKVM Host on each Mac you want to control."
!define MUI_FINISHPAGE_RUN "$INSTDIR\softkvm.exe"
!define MUI_FINISHPAGE_RUN_TEXT "Start SoftKVM now"

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\SoftKVM"
!define RUN_KEY "Software\Microsoft\Windows\CurrentVersion\Run"

Section "SoftKVM (required)" SecMain
  SectionIn RO
  ; Replace a running copy.
  nsExec::Exec 'taskkill /IM softkvm.exe /F'
  Sleep 500
  SetOutPath "$INSTDIR"
  File /r "${DIST}\*.*"
  WriteUninstaller "$INSTDIR\Uninstall SoftKVM.exe"
  CreateShortcut "$SMPROGRAMS\SoftKVM.lnk" "$INSTDIR\softkvm.exe" "" "$INSTDIR\softkvm.exe" 0
  WriteRegStr HKCU "Software\SoftKVM" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "SoftKVM"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "SoftKVM"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\softkvm.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall SoftKVM.exe"'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Start with Windows" SecAutostart
  WriteRegStr HKCU "${RUN_KEY}" "SoftKVM" '"$INSTDIR\softkvm.exe" --start-hidden'
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SecMain} "The SoftKVM app (tray icon near the clock)."
  !insertmacro MUI_DESCRIPTION_TEXT ${SecAutostart} "Start SoftKVM in the background when you sign in."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
  nsExec::Exec 'taskkill /IM softkvm.exe /F'
  Sleep 500
  Delete "$SMPROGRAMS\SoftKVM.lnk"
  RMDir /r "$INSTDIR"
  DeleteRegValue HKCU "${RUN_KEY}" "SoftKVM"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\SoftKVM"
  ; Settings and pairings in %APPDATA%\SoftKVM are kept so a reinstall stays paired.
SectionEnd
