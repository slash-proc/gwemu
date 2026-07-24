; GWemu Windows installer (NSIS).
;
; Deliberately per-user: installs under $LOCALAPPDATA\Programs\GWemu with
; RequestExecutionLevel user, so no UAC prompt and no admin rights needed
; -- the modern default for a standalone app like this. Settings/saves
; already live in the user profile (SDL pref path), never in the install
; dir, so upgrade/uninstall stays clean.
;
; Built on Linux with makensis (mingw32-nsis in the gwemu-win-cross
; container) -- see the release workflow's build-windows job. Inputs:
;   makensis -DDISTDIR=<staged dist dir> -DVERSION=<x.y.z> \
;            -DOUTFILE=<output .exe> contrib/gwemu-installer/gwemu.nsi
; DISTDIR must contain the fully packaged tree (exes + DLLs + scripts/),
; with gwemu.exe already flipped to GUI subsystem (make-gwemu-alias.py).

!ifndef DISTDIR
  !error "Pass -DDISTDIR=<packaged dist directory>"
!endif
!ifndef VERSION
  !define VERSION "0.0.0"
!endif
!ifndef OUTFILE
  !define OUTFILE "gwemu-setup.exe"
!endif

!define APPNAME "GWemu"
!define UNINST_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\${APPNAME}"

Name "${APPNAME} ${VERSION}"
OutFile "${OUTFILE}"
Unicode true
RequestExecutionLevel user
InstallDir "$LOCALAPPDATA\Programs\GWemu"
; Remember the install dir across upgrades.
InstallDirRegKey HKCU "Software\${APPNAME}" "InstallDir"

Page directory
Page instfiles
UninstPage uninstConfirm
UninstPage instfiles

Section "Install"
  SetOutPath "$INSTDIR"
  File /r "${DISTDIR}\*"

  WriteRegStr HKCU "Software\${APPNAME}" "InstallDir" "$INSTDIR"
  WriteUninstaller "$INSTDIR\uninstall.exe"

  CreateDirectory "$SMPROGRAMS\${APPNAME}"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk" "$INSTDIR\gwemu.exe"
  CreateShortcut "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk" "$INSTDIR\uninstall.exe"

  ; Add/Remove Programs entry.
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayName" "${APPNAME}"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayVersion" "${VERSION}"
  WriteRegStr HKCU "${UNINST_KEY}" "Publisher" "slash-proc/gwemu"
  WriteRegStr HKCU "${UNINST_KEY}" "DisplayIcon" "$INSTDIR\gwemu.exe"
  WriteRegStr HKCU "${UNINST_KEY}" "UninstallString" "$\"$INSTDIR\uninstall.exe$\""
  WriteRegStr HKCU "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoModify" 1
  WriteRegDWORD HKCU "${UNINST_KEY}" "NoRepair" 1
SectionEnd

Section "Uninstall"
  ; Only remove what we installed -- user data (settings, saves, flash
  ; images) lives in the profile, not here, and stays untouched.
  RMDir /r "$INSTDIR"
  Delete "$SMPROGRAMS\${APPNAME}\${APPNAME}.lnk"
  Delete "$SMPROGRAMS\${APPNAME}\Uninstall ${APPNAME}.lnk"
  RMDir "$SMPROGRAMS\${APPNAME}"
  DeleteRegKey HKCU "${UNINST_KEY}"
  DeleteRegKey HKCU "Software\${APPNAME}"
SectionEnd
