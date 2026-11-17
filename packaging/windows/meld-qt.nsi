; The Windows installer for Meld Qt, built with NSIS 3 (makensis) from a folder
; that windeployqt has filled, by build-windows-installer.sh, which defines:
;
;   VERSION        the version shown (MELD_VERSION, or the project's)
;   VERSION_NUMBER the project's version, numeric (x.y.z), for the version
;                  information of the installer
;   DIST_DIR       the deployed folder, installed as it is
;   APP_EXE        meld-qt.exe's path in it (meld-qt.exe, or bin\meld-qt.exe
;                  for CMake's installation)
;   FILES_NSH      File commands for the folder's files (apart from the
;                  Visual C++ redistributable)
;   UNFILES_NSH    the Delete and RMDir commands that remove them
;   OUTFILE        the installer that's written
;   VCREDIST       (if windeployqt copied it) vc_redist.x64.exe, which is run
;                  if the Visual C++ runtime isn't installed or is older
;
; The installation is for all users, in Program Files, and needs 64-bit
; Windows 10 1809 or newer, as Qt 6.8 does.

Unicode true
ManifestDPIAware true
SetCompressor /SOLID lzma

!macro Require NAME
    !ifndef ${NAME}
        !error "${NAME} isn't defined: build the installer with build-windows-installer.sh"
    !endif
!macroend
!insertmacro Require VERSION
!insertmacro Require VERSION_NUMBER
!insertmacro Require DIST_DIR
!insertmacro Require APP_EXE
!insertmacro Require FILES_NSH
!insertmacro Require UNFILES_NSH
!insertmacro Require OUTFILE

!include MUI2.nsh
!include LogicLib.nsh
!include x64.nsh
!include WinVer.nsh
!include FileFunc.nsh
!include WordFunc.nsh

!define APP_NAME "Meld Qt"
!define PUBLISHER "Meld Qt"
!define UNINSTALL_KEY "Software\Microsoft\Windows\CurrentVersion\Uninstall\Meld Qt"
!define APP_PATHS_KEY "Software\Microsoft\Windows\CurrentVersion\App Paths\meld-qt.exe"
!define PROG_ID "MeldQt.Comparison"
!define ICON_FILE "meld-qt.ico"
!define SOURCE_DIR "${__FILEDIR__}/../.."

Name "${APP_NAME}"
OutFile "${OUTFILE}"
InstallDir "$PROGRAMFILES64\${APP_NAME}"
InstallDirRegKey HKLM "${UNINSTALL_KEY}" "InstallLocation"
RequestExecutionLevel admin
BrandingText "${APP_NAME} ${VERSION}"

VIProductVersion "${VERSION_NUMBER}.0"
VIFileVersion "${VERSION_NUMBER}.0"
VIAddVersionKey /LANG=0 "ProductName" "${APP_NAME}"
VIAddVersionKey /LANG=0 "ProductVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "FileVersion" "${VERSION}"
VIAddVersionKey /LANG=0 "FileDescription" "${APP_NAME} ${VERSION} Setup"
VIAddVersionKey /LANG=0 "CompanyName" "${PUBLISHER}"
VIAddVersionKey /LANG=0 "LegalCopyright" "Copyright (c) 2026 Alexander R."

!define MUI_ICON "${SOURCE_DIR}/src/app/${ICON_FILE}"
!define MUI_UNICON "${SOURCE_DIR}/src/app/${ICON_FILE}"
!define MUI_ABORTWARNING
!define MUI_COMPONENTSPAGE_SMALLDESC

; The program is started through Explorer, so that it doesn't run as
; administrator, as the installer does
!define MUI_FINISHPAGE_RUN
!define MUI_FINISHPAGE_RUN_TEXT "Run ${APP_NAME}"
!define MUI_FINISHPAGE_RUN_FUNCTION RunProgram

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE "${SOURCE_DIR}/LICENSE"
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_DIRECTORY
!insertmacro MUI_PAGE_INSTFILES
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_UNPAGE_CONFIRM
!insertmacro MUI_UNPAGE_INSTFILES

!insertmacro MUI_LANGUAGE "English"

Function .onInit
    ${IfNot} ${RunningX64}
        MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} needs a 64-bit version of Windows." /SD IDOK
        Abort
    ${EndIf}
    ${IfNot} ${AtLeastWin10}
    ${OrIfNot} ${AtLeastBuild} 17763
        MessageBox MB_OK|MB_ICONSTOP "${APP_NAME} needs Windows 10 version 1809 or newer." /SD IDOK
        Abort
    ${EndIf}
    SetRegView 64
    SetShellVarContext all
    InitPluginsDir
FunctionEnd

Function un.onInit
    SetRegView 64
    SetShellVarContext all
FunctionEnd

Function RunProgram
    Exec '"$WINDIR\explorer.exe" "$INSTDIR\${APP_EXE}"'
FunctionEnd

; Whether the runtime that's installed is at least the version of the
; redistributable (which then needn't run): $0 is 1 if it is, 0 if not
!ifdef VCREDIST
!getdllversion "${VCREDIST}" VCREDIST_
!define VCREDIST_VERSION "${VCREDIST_1}.${VCREDIST_2}.${VCREDIST_3}"
!define VC_RUNTIME_KEY "SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\x64"

!macro CheckVcRuntimeIn VIEW
    SetRegView ${VIEW}
    ReadRegDWORD $1 HKLM "${VC_RUNTIME_KEY}" "Installed"
    ${If} $1 = 1
        ReadRegDWORD $1 HKLM "${VC_RUNTIME_KEY}" "Major"
        ReadRegDWORD $2 HKLM "${VC_RUNTIME_KEY}" "Minor"
        ReadRegDWORD $3 HKLM "${VC_RUNTIME_KEY}" "Bld"
        ; 0: the same version, 1: the installed one is newer
        ${VersionCompare} "$1.$2.$3" "${VCREDIST_VERSION}" $4
        ${If} $4 <> 2
            StrCpy $0 1
        ${EndIf}
    ${EndIf}
!macroend

Function CheckVcRuntime
    StrCpy $0 0
    ; The x64 runtime registers itself in the registry's 32-bit view, which
    ; is read too in case that changes
    !insertmacro CheckVcRuntimeIn 32
    !insertmacro CheckVcRuntimeIn 64
    SetRegView 64
FunctionEnd
!endif

Section "${APP_NAME}" SecProgram
    SectionIn RO

    ; An earlier installation is removed first (keeping the settings, which
    ; the uninstaller doesn't touch), so that files it had and this doesn't
    ; don't stay behind
    ReadRegStr $0 HKLM "${UNINSTALL_KEY}" "UninstallString"
    ReadRegStr $1 HKLM "${UNINSTALL_KEY}" "InstallLocation"
    ${If} $0 != ""
    ${AndIf} $1 != ""
    ${AndIf} ${FileExists} "$1\uninstall.exe"
        DetailPrint "Removing the installed version"
        ExecWait '"$1\uninstall.exe" /S _?=$1'
        Delete "$1\uninstall.exe"
        RMDir "$1"
    ${EndIf}

!ifdef VCREDIST
    Call CheckVcRuntime
    ${If} $0 = 0
        DetailPrint "Installing the Visual C++ runtime ${VCREDIST_VERSION}"
        SetOutPath "$PLUGINSDIR"
        File "/oname=vc_redist.x64.exe" "${VCREDIST}"
        ExecWait '"$PLUGINSDIR\vc_redist.x64.exe" /install /quiet /norestart' $0
        Delete "$PLUGINSDIR\vc_redist.x64.exe"
        ; 1638: a newer version is installed; 3010: Windows has to restart
        ${If} $0 = 3010
            SetRebootFlag true
        ${ElseIf} $0 <> 0
        ${AndIf} $0 <> 1638
            MessageBox MB_OK|MB_ICONEXCLAMATION "The Visual C++ runtime couldn't be installed (error $0). \
                ${APP_NAME} may not start until it is: it can be downloaded from Microsoft as \
                $\"Visual C++ Redistributable for Visual Studio 2015-2022$\" (x64)." /SD IDOK
        ${EndIf}
    ${EndIf}
!endif

    !include "${FILES_NSH}"
    SetOutPath "$INSTDIR"
    File "${SOURCE_DIR}/src/app/${ICON_FILE}"

    WriteUninstaller "$INSTDIR\uninstall.exe"

    ; Settings > Apps, and Programs and Features
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayName" "${APP_NAME}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayVersion" "${VERSION}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "Publisher" "${PUBLISHER}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "DisplayIcon" "$INSTDIR\${ICON_FILE}"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "InstallLocation" "$INSTDIR"
    WriteRegStr HKLM "${UNINSTALL_KEY}" "UninstallString" '"$INSTDIR\uninstall.exe"'
    WriteRegStr HKLM "${UNINSTALL_KEY}" "QuietUninstallString" '"$INSTDIR\uninstall.exe" /S'
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoModify" 1
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "NoRepair" 1
    ${GetSize} "$INSTDIR" "/S=0K" $0 $1 $2
    WriteRegDWORD HKLM "${UNINSTALL_KEY}" "EstimatedSize" $0

    ; "meld-qt" in the Run dialog, and for "start meld-qt"
    WriteRegStr HKLM "${APP_PATHS_KEY}" "" "$INSTDIR\${APP_EXE}"
SectionEnd

Section "Start menu shortcut" SecStartMenu
    CreateShortcut "$SMPROGRAMS\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${ICON_FILE}"
SectionEnd

Section /o "Desktop shortcut" SecDesktop
    CreateShortcut "$DESKTOP\${APP_NAME}.lnk" "$INSTDIR\${APP_EXE}" "" "$INSTDIR\${ICON_FILE}"
SectionEnd

Section "Open saved comparisons (.meldcmp)" SecAssociation
    WriteRegStr HKLM "Software\Classes\${PROG_ID}" "" "${APP_NAME} comparison"
    WriteRegStr HKLM "Software\Classes\${PROG_ID}\DefaultIcon" "" "$INSTDIR\${ICON_FILE}"
    WriteRegStr HKLM "Software\Classes\${PROG_ID}\shell\open\command" "" '"$INSTDIR\${APP_EXE}" "%1"'
    WriteRegStr HKLM "Software\Classes\.meldcmp" "" "${PROG_ID}"
    WriteRegStr HKLM "Software\Classes\.meldcmp\OpenWithProgids" "${PROG_ID}" ""
    ; SHCNE_ASSOCCHANGED, SHCNF_IDLIST
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
    !insertmacro MUI_DESCRIPTION_TEXT ${SecProgram} "${APP_NAME} and the Qt libraries it uses."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecStartMenu} "A shortcut in the Start menu."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecDesktop} "A shortcut on the desktop."
    !insertmacro MUI_DESCRIPTION_TEXT ${SecAssociation} "Open saved comparisons (.meldcmp files) with ${APP_NAME}."
!insertmacro MUI_FUNCTION_DESCRIPTION_END

Section "Uninstall"
    Delete "$SMPROGRAMS\${APP_NAME}.lnk"
    Delete "$DESKTOP\${APP_NAME}.lnk"

    ; The extension is left to another program that has taken it since
    ReadRegStr $0 HKLM "Software\Classes\.meldcmp" ""
    ${If} $0 == "${PROG_ID}"
        DeleteRegValue HKLM "Software\Classes\.meldcmp" ""
    ${EndIf}
    DeleteRegValue HKLM "Software\Classes\.meldcmp\OpenWithProgids" "${PROG_ID}"
    DeleteRegKey /ifempty HKLM "Software\Classes\.meldcmp\OpenWithProgids"
    DeleteRegKey /ifempty HKLM "Software\Classes\.meldcmp"
    DeleteRegKey HKLM "Software\Classes\${PROG_ID}"
    System::Call 'shell32::SHChangeNotify(i 0x08000000, i 0, p 0, p 0)'

    DeleteRegKey HKLM "${APP_PATHS_KEY}"
    DeleteRegKey HKLM "${UNINSTALL_KEY}"

    ; Only the files that were installed: the folder goes if it's empty then
    !include "${UNFILES_NSH}"
    Delete "$INSTDIR\${ICON_FILE}"
    Delete "$INSTDIR\uninstall.exe"
    RMDir "$INSTDIR"
SectionEnd
