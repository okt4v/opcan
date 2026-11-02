; opcan Windows installer (NSIS 3, built by packaging/build-windows.sh; makensis also runs on Linux).
; Per-user install to %LOCALAPPDATA%\Programs\opcan: no admin rights / UAC prompt needed.
; makensis -DVERSION=x.y.z -DSTAGE=<dir with opcan.exe, assets\, engines\, LICENSE> -DOUTFILE=<setup.exe> opcan.nsi
; Silent: setup.exe /S [/D=C:\path]; uninstall: Uninstall.exe /S. User data in %APPDATA%\opcan is kept
; unless "Remove my opcan data" is ticked in the uninstaller.

!ifndef VERSION | STAGE | OUTFILE
  !error "pass -DVERSION=... -DSTAGE=... -DOUTFILE=..."
!endif

Unicode true
SetCompressor /SOLID lzma
ManifestDPIAware true
RequestExecutionLevel user

!define APP "opcan"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APP}"

Name "${APP}"
OutFile "${OUTFILE}"
InstallDir "$LOCALAPPDATA\Programs\${APP}"
InstallDirRegKey HKCU "Software\${APP}" "InstallDir"
BrandingText "${APP} ${VERSION}"

VIProductVersion "${VERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${VERSION}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "CompanyName" "opcan contributors"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later"

!include "MUI2.nsh"
!include "x64.nsh"

!define MUI_ICON "opcan.ico"  ; makensis runs in the script's directory
!define MUI_UNICON "opcan.ico"  ; makensis runs in the script's directory
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\opcan.exe"
!define MUI_COMPONENTSPAGE_NODESC

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}/LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "opcan needs 64-bit Windows."
    Abort
  ${EndIf}
FunctionEnd

Section "opcan" SecMain
  SectionIn RO
  ; stop a running copy from blocking the overwrite would need a plugin; a reinstall just replaces files
  SetOutPath "$INSTDIR"
  RMDir /r "$INSTDIR\assets"  ; drop files a previous version had but this one doesn't
  File "${STAGE}/opcan.exe"
  File "${STAGE}/LICENSE"
  !if /FileExists "${STAGE}/README.md"
    File "${STAGE}/README.md"
  !endif
  File /r "${STAGE}/assets"
  File /r "${STAGE}/engines"

  WriteUninstaller "$INSTDIR\Uninstall.exe"
  CreateShortcut "$SMPROGRAMS\${APP}.lnk" "$INSTDIR\opcan.exe"

  WriteRegStr HKCU "Software\${APP}" "InstallDir" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APP}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "opcan contributors"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\opcan.exe,0"
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" '"$INSTDIR\Uninstall.exe"'
  WriteRegStr HKCU "${UNINST_KEY}" "QuietUninstallString" '"$INSTDIR\Uninstall.exe" /S'
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Desktop shortcut" SecDesktop
  CreateShortcut "$DESKTOP\${APP}.lnk" "$INSTDIR\opcan.exe"
SectionEnd

Section "un.opcan" UnSecMain
  SectionIn RO
  Delete "$INSTDIR\opcan.exe"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\README.md"
  RMDir /r "$INSTDIR\assets"
  RMDir /r "$INSTDIR\engines"
  Delete "$INSTDIR\Uninstall.exe"
  RMDir "$INSTDIR"  ; only if empty: never removes files we didn't put there
  Delete "$SMPROGRAMS\${APP}.lnk"
  Delete "$DESKTOP\${APP}.lnk"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\${APP}"
SectionEnd

; off by default (and in silent uninstalls): settings, game database, downloaded AI models and tablebases
Section /o "un.Remove my opcan data (settings, games, AI models)" UnSecData
  RMDir /r "$APPDATA\${APP}"
SectionEnd
