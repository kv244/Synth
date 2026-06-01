; Neon Synth — NSIS installer script
; Build:  makensis /DPRODUCT_VERSION=1.0.0 installer.nsi

Unicode True

; ── Version (override from CI with /DPRODUCT_VERSION=x.y.z) ──────────────────
!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "1.0.0"
!endif

; ── Constants ────────────────────────────────────────────────────────────────
!define PRODUCT_NAME      "Neon Synth"
!define PRODUCT_PUBLISHER "AudioDSP"
!define PRODUCT_AUTHOR    "Razvan Julian Petrescu"
!define UNINST_KEY        "Software\Microsoft\Windows\CurrentVersion\Uninstall\NeonSynth"

; ── Output ───────────────────────────────────────────────────────────────────
Name    "${PRODUCT_NAME} ${PRODUCT_VERSION}"
OutFile "NeonSynth-${PRODUCT_VERSION}-Windows-Setup.exe"

InstallDir          "$PROGRAMFILES64\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}"
InstallDirRegKey    HKLM "${UNINST_KEY}" "InstallLocation"
RequestExecutionLevel admin
SetCompressor       /SOLID lzma

; ── MUI2 ─────────────────────────────────────────────────────────────────────
!include "MUI2.nsh"

!define MUI_WELCOMEPAGE_TITLE    "Welcome to ${PRODUCT_NAME} ${PRODUCT_VERSION}"
!define MUI_WELCOMEPAGE_TEXT     "This will install ${PRODUCT_NAME} by ${PRODUCT_AUTHOR}.$\r$\n$\r$\nClick Next to continue."
!define MUI_FINISHPAGE_TITLE     "Installation complete"
!define MUI_FINISHPAGE_TEXT      "${PRODUCT_NAME} has been installed.$\r$\n$\r$\nVST3 plugins are scanned automatically by your DAW on next launch."
!define MUI_FINISHPAGE_NOAUTOCLOSE

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE    "LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

; ── Sections ─────────────────────────────────────────────────────────────────

Section "VST3 Plugin" SEC_VST3
  ; Required — cannot be deselected
  SectionIn RO

  SetOutPath "$COMMONFILES64\VST3"
  File /r "build\BasicSynth_artefacts\Release\VST3\BasicSynth.vst3"
SectionEnd

Section "Standalone Application" SEC_STANDALONE
  SetOutPath "$INSTDIR"
  File "build\BasicSynth_artefacts\Release\Standalone\BasicSynth.exe"

  CreateDirectory "$SMPROGRAMS\${PRODUCT_PUBLISHER}"
  CreateShortcut  "$SMPROGRAMS\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}.lnk" \
                  "$INSTDIR\BasicSynth.exe"
SectionEnd

; ── Descriptions (shown in component page) ───────────────────────────────────
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_VST3}       "VST3 plugin → $COMMONFILES64\VST3 (required)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_STANDALONE} "Standalone app with its own audio/MIDI settings"
!insertmacro MUI_FUNCTION_DESCRIPTION_END

; ── Post-install: write uninstaller + registry ───────────────────────────────
Section -Post
  WriteUninstaller "$INSTDIR\uninstall.exe"

  WriteRegStr  HKLM "${UNINST_KEY}" "DisplayName"     "${PRODUCT_NAME}"
  WriteRegStr  HKLM "${UNINST_KEY}" "DisplayVersion"  "${PRODUCT_VERSION}"
  WriteRegStr  HKLM "${UNINST_KEY}" "Publisher"       "${PRODUCT_PUBLISHER}"
  WriteRegStr  HKLM "${UNINST_KEY}" "InstallLocation" "$INSTDIR"
  WriteRegStr  HKLM "${UNINST_KEY}" "UninstallString" "$INSTDIR\uninstall.exe"
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoModify"       1
  WriteRegDWORD HKLM "${UNINST_KEY}" "NoRepair"       1
SectionEnd

; ── Uninstaller ──────────────────────────────────────────────────────────────
Section "Uninstall"
  RMDir /r "$COMMONFILES64\VST3\BasicSynth.vst3"

  Delete    "$INSTDIR\BasicSynth.exe"
  Delete    "$INSTDIR\uninstall.exe"
  RMDir     "$INSTDIR"
  RMDir     "$PROGRAMFILES64\${PRODUCT_PUBLISHER}"

  Delete    "$SMPROGRAMS\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}.lnk"
  RMDir     "$SMPROGRAMS\${PRODUCT_PUBLISHER}"

  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
