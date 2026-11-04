; opcan Windows installer (NSIS 3; built by packaging/build-windows.sh and, for MSVC / CI, the `installer` target in
; packaging.cmake; makensis also runs on Linux).
; Per-user install to %LOCALAPPDATA%\Programs\opcan: no admin rights / UAC prompt needed.
; makensis -DVERSION=x.y.z[-beta.1] [-DNUMVERSION=x.y.z] -DSTAGE=<install tree: opcan.exe, assets\, engines\, LICENSE, ...>
;   -DOUTFILE=<setup.exe> opcan.nsi     (STAGE is `cmake --install <build> --component opcan --prefix <dir>`: all of it is installed)
; Silent: setup.exe /S [/D=C:\path]; uninstall: Uninstall.exe /S. User data in %APPDATA%\opcan is kept
; unless "Remove my opcan data" is ticked in the uninstaller. The in-app updater (src/update.cpp) relies on the
; HKCU\Software\opcan InstallDir value, the default directory, /S and /D=.

!ifndef VERSION | STAGE | OUTFILE
  !error "pass -DVERSION=... -DSTAGE=... -DOUTFILE=..."
!endif
!if ! /FileExists "${STAGE}\opcan.exe"
  !error "${STAGE}\opcan.exe not found: STAGE is the installed tree"
!endif
!ifndef NUMVERSION  ; the file version must be numeric: pass it when VERSION has a pre-release suffix
  !define NUMVERSION "${VERSION}"
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

VIProductVersion "${NUMVERSION}.0"
VIAddVersionKey "ProductName" "${APP}"
VIAddVersionKey "ProductVersion" "${VERSION}"
VIAddVersionKey "FileVersion" "${NUMVERSION}"
VIAddVersionKey "FileDescription" "${APP} installer"
VIAddVersionKey "CompanyName" "opcan contributors"
VIAddVersionKey "LegalCopyright" "GPL-3.0-or-later"

!include "MUI2.nsh"
!include "LogicLib.nsh"
!include "x64.nsh"

!define MUI_ICON "opcan.ico"  ; makensis runs in the script's directory
!define MUI_UNICON "opcan.ico"  ; makensis runs in the script's directory
!define MUI_ABORTWARNING
!define MUI_FINISHPAGE_RUN "$INSTDIR\opcan.exe"
!define MUI_COMPONENTSPAGE_NODESC

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${STAGE}\LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_COMPONENTS
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; opcan 0.2.0 shipped CPack's installer: admin, all users, "C:\Program Files\opcan", registered under
; HKLM\Software\opcan contributors\opcan (32-bit view, like this installer's). Its updater runs this setup with /S and
; then restarts its own Program Files\opcan.exe. This installer can't write there (per-user, no admin), so it installs
; per-user as usual and then removes the old copy with that copy's uninstaller (one UAC prompt, data is kept). The old
; updater's restart then finds no opcan.exe, so a silent install that removed it starts the new copy itself.
; Declining the prompt keeps both copies; the old one keeps offering the update.
Var legacyDir

Function .onInit
  ${IfNot} ${RunningX64}
    MessageBox MB_ICONSTOP "opcan needs 64-bit Windows."
    Abort
  ${EndIf}
  ReadRegStr $legacyDir HKLM "Software\opcan contributors\${APP}" ""
FunctionEnd

Section "opcan" SecMain
  SectionIn RO
  ; stop a running copy from blocking the overwrite would need a plugin; a reinstall just replaces files
  SetOutPath "$INSTDIR"
  RMDir /r "$INSTDIR\assets"  ; drop files a previous version had but this one doesn't
  File /r "${STAGE}\*"  ; the whole install tree (MSVC builds: also the VC++ runtime DLLs)

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

Section "-Remove the old all-users install"
  ${If} $legacyDir == ""
  ${OrIf} $legacyDir == $INSTDIR
  ${OrIfNot} ${FileExists} "$legacyDir\Uninstall.exe"
    Return
  ${EndIf}
  MessageBox MB_YESNO|MB_ICONQUESTION "An older opcan is installed for all users in $legacyDir.$\r$\nRemove it? Windows asks for permission; your games and settings are kept." /SD IDYES IDNO done
  ClearErrors
  ExecShellWait "" "$legacyDir\Uninstall.exe" "/S"  ; ShellExecute, so its admin manifest brings up UAC
  IfErrors done  ; declined
  ; the uninstaller reruns itself from %TEMP% and returns at once: wait up to 60 s for it to finish
  ${For} $0 1 240
    ${IfNot} ${FileExists} "$legacyDir\opcan.exe"
      ${Break}
    ${EndIf}
    Sleep 250
  ${Next}
  ${If} ${Silent}
  ${AndIfNot} ${FileExists} "$legacyDir\opcan.exe"
    Exec '"$INSTDIR\opcan.exe"'
  ${EndIf}
  done:
SectionEnd

Section "un.opcan" UnSecMain
  SectionIn RO
  Delete "$INSTDIR\opcan.exe"
  Delete "$INSTDIR\LICENSE"
  Delete "$INSTDIR\README.md"
  Delete "$INSTDIR\*.dll"  ; MSVC builds' runtime DLLs (InstallRequiredSystemLibraries)
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
