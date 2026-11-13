; Neon Synth — NSIS installer script
; Build:  makensis /DPRODUCT_VERSION=1.7.1 installer.nsi

Unicode True

; ── Version (override from CI with /DPRODUCT_VERSION=x.y.z) ──────────────────
!ifndef PRODUCT_VERSION
  !define PRODUCT_VERSION "1.7.1"
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
!define MUI_FINISHPAGE_TEXT      "${PRODUCT_NAME} has been installed.$\r$\n$\r$\nVST3 plugins are scanned automatically by your DAW on next launch.$\r$\n$\r$\n${PRODUCT_NAME} is free software under the GNU AGPL v3. Source code: https://github.com/kv244/Synth"
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

  ; Before v1.4 the plugin was called BasicSynth. Remove the old bundle so the
  ; DAW doesn't find two copies of the same plugin (they share a plugin ID).
  RMDir /r "$COMMONFILES64\VST3\BasicSynth.vst3"

  SetOutPath "$COMMONFILES64\VST3"
  File /r "build\NeonSynth_artefacts\Release\VST3\NeonSynth.vst3"
SectionEnd

Section "Standalone Application" SEC_STANDALONE
  Delete "$INSTDIR\BasicSynth.exe"   ; the Standalone's name before v1.4
  SetOutPath "$INSTDIR"
  File "build\NeonSynth_artefacts\Release\Standalone\NeonSynth.exe"

  CreateDirectory "$SMPROGRAMS\${PRODUCT_PUBLISHER}"
  CreateShortcut  "$SMPROGRAMS\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}.lnk" \
                  "$INSTDIR\NeonSynth.exe"
SectionEnd

; The user manual as a PDF, next to the Standalone in the main NeonSynth folder. The release
; workflow builds the PDF first (build\manual\), so the installer carries the same file that
; is attached to the release.
Section "User Manual (PDF)" SEC_MANUAL
  ; Earlier versions' manuals carry their version in the name; remove them on upgrade.
  Delete "$INSTDIR\NeonSynth-User-Manual-*.pdf"
  SetOutPath "$INSTDIR"
  File "build\manual\NeonSynth-User-Manual-${PRODUCT_VERSION}.pdf"

  CreateDirectory "$SMPROGRAMS\${PRODUCT_PUBLISHER}"
  CreateShortcut  "$SMPROGRAMS\${PRODUCT_PUBLISHER}\${PRODUCT_NAME} User Manual.lnk" \
                  "$INSTDIR\NeonSynth-User-Manual-${PRODUCT_VERSION}.pdf"
SectionEnd

; ── Descriptions (shown in component page) ───────────────────────────────────
!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_VST3}       "VST3 plugin → $COMMONFILES64\VST3 (required)"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_STANDALONE} "Standalone app with its own audio/MIDI settings"
  !insertmacro MUI_DESCRIPTION_TEXT ${SEC_MANUAL}     "The user manual as a PDF, in the NeonSynth folder"
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
  RMDir /r "$COMMONFILES64\VST3\NeonSynth.vst3"

  Delete    "$INSTDIR\NeonSynth.exe"
  Delete    "$INSTDIR\NeonSynth-User-Manual-*.pdf"
  Delete    "$INSTDIR\uninstall.exe"
  RMDir     "$INSTDIR"
  RMDir     "$PROGRAMFILES64\${PRODUCT_PUBLISHER}"

  Delete    "$SMPROGRAMS\${PRODUCT_PUBLISHER}\${PRODUCT_NAME}.lnk"
  Delete    "$SMPROGRAMS\${PRODUCT_PUBLISHER}\${PRODUCT_NAME} User Manual.lnk"
  RMDir     "$SMPROGRAMS\${PRODUCT_PUBLISHER}"

  DeleteRegKey HKLM "${UNINST_KEY}"
SectionEnd
