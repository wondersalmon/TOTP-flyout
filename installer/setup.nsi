Unicode true
!include "MUI2.nsh"
!include "x64.nsh"

!ifndef APP_ID
  !define APP_ID "TOTP-flyout"
!endif
!ifndef APP_NAME
  !define APP_NAME "TOTP flyout"
!endif
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP_ID}"

Name "${APP_NAME}"
OutFile "${SETUP_EXE}"
InstallDir "$LOCALAPPDATA\Programs\${APP_NAME}"
RequestExecutionLevel user
VIProductVersion "${APP_VERSION}.0"
VIAddVersionKey "ProductName" "${APP_NAME}"
VIAddVersionKey "FileDescription" "${APP_NAME} Setup"
VIAddVersionKey "FileVersion" "${APP_VERSION}"
VIAddVersionKey "LegalCopyright" "${APP_PUBLISHER}"

!define MUI_ABORTWARNING
!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH
!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES
!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} requires 64-bit Windows." /SD IDOK
    SetErrorLevel 1
    Quit
  ${EndIf}
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "Install"
  ClearErrors
  SetOutPath "$INSTDIR"
  File /oname=TOTP-flyout.exe.new "${APP_EXE}"
  IfErrors installFailed
  System::Call 'kernel32::MoveFileExW(w "$INSTDIR\TOTP-flyout.exe.new", w "$INSTDIR\TOTP-flyout.exe", i 9) i.r0'
  StrCmp $0 0 installFailed
  File /oname=LICENSE.txt "${APP_LICENSE}"

  WriteUninstaller "$INSTDIR\uninstall.exe"
  CreateShortCut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\TOTP-flyout.exe" "--show"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayVersion" "${APP_VERSION}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "Publisher" "${APP_PUBLISHER}"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\TOTP-flyout.exe"
  WriteRegStr HKCU "${UNINSTALL_KEY}" "UninstallString" '$\"$INSTDIR\uninstall.exe$\"'
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINSTALL_KEY}" "NoRepair" 1
  IfErrors installFailed
  Goto installed

installFailed:
  Delete "$INSTDIR\TOTP-flyout.exe.new"
  MessageBox MB_OK|MB_ICONSTOP "Installation failed. Close ${APP_NAME} in the tray and check that the installation folder is writable." /SD IDOK
  SetErrorLevel 1
  Abort

installed:
SectionEnd

Function un.onInit
  SetShellVarContext current
  SetRegView 64
FunctionEnd

Section "Uninstall"
  IfFileExists "$INSTDIR\TOTP-flyout.exe" 0 removeRegistration
  ClearErrors
  Delete "$INSTDIR\TOTP-flyout.exe"
  IfErrors 0 removeRegistration
  MessageBox MB_OK|MB_ICONSTOP "Close ${APP_NAME} in the tray, then run uninstall again." /SD IDOK
  SetErrorLevel 1
  Abort

removeRegistration:
  Delete "$SMPROGRAMS\${APP_NAME}.lnk"
  DeleteRegKey HKCU "${UNINSTALL_KEY}"
  Delete "$INSTDIR\LICENSE.txt"
  Delete "$INSTDIR\uninstall.exe"
  RMDir "$INSTDIR"
SectionEnd
