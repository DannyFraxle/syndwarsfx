; SyndWarsFX 3D (FX3D) update installer script for NSIS-3
;
; Unlike `syndwarsfx-setup.nsi`, this installer does NOT install the game from
; scratch. It only updates an EXISTING Syndicate Wars fan port installation with
; the FX3D OpenGL renderer build. It therefore never touches an original CD or a
; GOG copy, and needs none of the external ripper/encoder utilities.
;
; Requires:
; - NSIS 3.x with the InetC, NSISunzU (nsisunz) and Md5dll plugins installed
; - A staged build tree, produced by `make pkg-dist` in the build directory
;
; Build with:
;   cd release && make pkg-dist && makensis util/syndwarsfx-fx3d-setup.nsi
;
; See util/README-fx3d-installer.md for the full recipe.


SetCompressor /SOLID lzma

OutFile syndwarsfx-fx3d-setup.exe
!include WinMessages.nsh
!include FileFunc.nsh
!include LogicLib.nsh
!include "MUI2.nsh"
!include x64.nsh
!include ..\res\syndwarsfx-config.nsh

ShowInstDetails show

LicenseLangString LIC_ENG 1033 ..\res\license-eng.txt

Name "Syndicate Wars Fan Expansion - 3D Renderer"
BrandingText "SyndWarsFX 3D, by fans for fans"

; --------------------
; VARIABLES

Var update_path
Var selected_menu_shortcut
Var selected_desk_shortcut
Var portable_install
Var backup_dir
Var levels_md5
Var gfx_md5
Var sfx_md5
Var dir_request

; Search for built files where `make pkg-dist` puts them
!if /FileExists "..\pkg\syndwarsfx\syndwarsfx3d.exe"
  !define BUILDENV_PKG_DIR "..\pkg\syndwarsfx"
!else if /FileExists "..\pkg\mingw64\syndwarsfx\syndwarsfx3d.exe"
  !define BUILDENV_PKG_DIR "..\pkg\mingw64\syndwarsfx"
!else if /FileExists "..\pkg\mingw32\syndwarsfx\syndwarsfx3d.exe"
  !define BUILDENV_PKG_DIR "..\pkg\mingw32\syndwarsfx"
!else
  !define BUILDENV_PKG_DIR ".\syndwarsfx"
!endif

; Allow the InetC / nsisunz / md5dll plugins to live in the build tree, so the
; installer can be built without administrator rights. This is the same layout
; the CI workflow sets up. Falls back to the plugins installed in NSIS itself.
!if /FileExists "..\nsis-plugins\x86-unicode\md5dll.dll"
  !addplugindir /x86-unicode "..\nsis-plugins\x86-unicode"
!endif
!if /FileExists "..\nsis-plugins\x86-ansi\md5dll.dll"
  !addplugindir /x86-ansi "..\nsis-plugins\x86-ansi"
!endif

!define MUI_ICON "..\res\syndwarsfx_icon.ico"
!define MUI_WELCOMEPAGE_TITLE "Welcome To The SyndWarsFX 3D Renderer Setup"
!define MUI_WELCOMEPAGE_TEXT "This will update an existing Syndicate Wars fan port installation with the FX3D hardware accelerated (OpenGL) renderer.$\r$\n$\r$\nYou must already have a working SyndWarsFX or Syndicate Wars fan port installation - this setup does not install the game itself, and does not copy anything from your original CD or GOG copy.$\r$\n$\r$\nYour existing executable and conf folder are backed up before anything is replaced.$\r$\n$\r$\nBuild ${PRODUCT_VERSION}$\t$\tGraphics ${GFX_VERSION}$\nLevels ${LEVELS_VERSION}$\t$\tSound ${SFX_VERSION}"

; --------------------
; PAGES

!insertmacro MUI_PAGE_WELCOME
!insertmacro MUI_PAGE_LICENSE $(LIC_ENG)
Page custom UpdateInstall UpdateInstallLeave
!insertmacro MUI_PAGE_COMPONENTS
!insertmacro MUI_PAGE_INSTFILES
!define MUI_FINISHPAGE_NOAUTOCLOSE
!insertmacro MUI_PAGE_FINISH

!insertmacro MUI_LANGUAGE "English"


Function .onInit
  StrCpy $portable_install "0"
  StrCpy $selected_menu_shortcut "0"
  StrCpy $selected_desk_shortcut "0"
FunctionEnd


; --------------------
; SECTIONS

Section "SyndWarsFX 3D renderer" Section_0
  SectionIn RO
  SetOutPath $INSTDIR

  Call BackupExistingInstall
  IfErrors inst_game_fail

  DetailPrint "Installing the FX3D executable and libraries..."
  File "${BUILDENV_PKG_DIR}\syndwarsfx3d.exe"
  File "${BUILDENV_PKG_DIR}\libgcc_s_dw2-1.dll"
  File "${BUILDENV_PKG_DIR}\libstdc++-6.dll"
  File "${BUILDENV_PKG_DIR}\libwinpthread-1.dll"
  File "${BUILDENV_PKG_DIR}\libopenal-1.dll"
  File "${BUILDENV_PKG_DIR}\libpng16-16.dll"
  File "${BUILDENV_PKG_DIR}\zlib1.dll"
  File "${BUILDENV_PKG_DIR}\libogg-0.dll"
  File "${BUILDENV_PKG_DIR}\libvorbis-0.dll"
  File "${BUILDENV_PKG_DIR}\libvorbisfile-3.dll"
  File "${BUILDENV_PKG_DIR}\libWildMidi.dll"
  IfFileExists "${BUILDENV_PKG_DIR}\SDL2.dll" 0 +1
  File /nonfatal "${BUILDENV_PKG_DIR}\SDL2.dll"
  IfFileExists "${BUILDENV_PKG_DIR}\SDL.dll" 0 +1
  File /nonfatal "${BUILDENV_PKG_DIR}\SDL.dll"
  IfErrors inst_game_fail

  ; Configuration files. `rules.ini` is left alone as it is commonly edited by
  ; the player, and `fx3d.ini` is handled separately below for the same
  ; reason - it is the FX3D renderer tuning file.
  DetailPrint "Installing configuration files..."
  SetOutPath $INSTDIR\conf
  File /r /x "rules.ini" /x "fx3d.ini" "${BUILDENV_PKG_DIR}\conf\"
  IfErrors inst_game_fail
  Call InstallLightsConf
  IfErrors inst_game_fail

  DetailPrint "Installing language files..."
  SetOutPath $INSTDIR\language
  File /r "${BUILDENV_PKG_DIR}\language\"
  IfErrors inst_game_fail

  SetOutPath $INSTDIR
  ${If} $portable_install == 0
    Call InstallRegistry
  ${EndIf}

  StrCmp $selected_menu_shortcut 1 0 inst_game_menu_end
  CreateDirectory "$SMPROGRAMS\Vexillium"
  CreateShortCut "$SMPROGRAMS\Vexillium\Syndicate Wars Fan Expansion 3D.lnk" "$INSTDIR\syndwarsfx3d.exe" "" "$INSTDIR\syndwarsfx3d.exe" "" SW_SHOWNORMAL
inst_game_menu_end:
  StrCmp $selected_desk_shortcut 1 0 inst_game_done
  CreateShortCut "$DESKTOP\Syndicate Wars Fan Expansion 3D.lnk" "$INSTDIR\syndwarsfx3d.exe" "" "$INSTDIR\syndwarsfx3d.exe" "" SW_SHOWNORMAL
  Goto inst_game_done

inst_game_fail:
  ; Never remove $INSTDIR here - it is the player's pre-existing game folder,
  ; not something this installer created.
  DetailPrint "An unexpected error has occured while installing the FX3D files."
  DetailPrint "Your previous files are preserved in $backup_dir"
  Abort "FX3D installation failed. Your previous install was backed up to $backup_dir"
inst_game_done:
  nop
SectionEnd


Section "Update game data packages" Section_1
  ; Downloads the free levels/graphics/sound packages from the project's GitHub
  ; releases. Deselect this if you are offline or already have current packages.
  Call UpdateDataPackages
  IfErrors inst_data_fail
  Goto inst_data_done
inst_data_fail:
  DetailPrint "The game data packages could not be updated."
  DetailPrint "The FX3D renderer is installed; re-run setup with a working"
  DetailPrint "network connection to refresh the data packages."
inst_data_done:
  nop
SectionEnd

!insertmacro MUI_FUNCTION_DESCRIPTION_BEGIN
  !insertmacro MUI_DESCRIPTION_TEXT ${Section_0} "Install the FX3D OpenGL renderer executable, libraries and configuration files into your existing game folder."
  !insertmacro MUI_DESCRIPTION_TEXT ${Section_1} "Download and install the latest free levels, graphics and sound packages from GitHub.$\r$\nRequires an internet connection."
!insertmacro MUI_FUNCTION_DESCRIPTION_END


; --------------------
; INSTALL LOCATION PAGE

Function UpdateInstall
  !insertmacro MUI_HEADER_TEXT "Existing Installation Location" "Select the folder holding your existing Syndicate Wars fan port installation"

  SetRegView 32
  ReadRegStr $update_path HKCU "Software\SyndWarsFX\CurrentVersion" "InstallPath"
  ${If} $update_path == ""
    ReadRegStr $update_path HKLM "Software\SyndWarsFX\CurrentVersion" "InstallPath"
  ${EndIf}

  nsDialogs::Create 1018
  Pop $0

  ${NSD_CreateGroupBox} 5% 20u 90% 70% "Existing game folder"
  Pop $0

  ${NSD_CreateDirRequest} 15% 35u 49% 12u $update_path
  Pop $dir_request

  ${NSD_CreateBrowseButton} 65% 35u 20% 12u "Browse..."
  Pop $0
  ${NSD_OnClick} $0 BrowseButtonClickedUpdate

  ${NSD_CreateLabel} 20u 54u 200u 12u "Additional install options:"
  Pop $0

  ${NSD_CreateCheckbox} 37u 65u 160u 15u "Portable install (do not write to the registry)"
  Pop $7

  ${NSD_CreateCheckbox} 37u 80u 160u 15u "Create a shortcut on the desktop"
  Pop $5

  ${NSD_CreateCheckbox} 37u 95u 160u 15u "Create an entry in the Start Menu"
  Pop $6

  ${If} $update_path == ""
    ${NSD_SetState} $7 ${BST_CHECKED}
  ${Else}
    ${NSD_SetState} $7 ${BST_UNCHECKED}
  ${EndIf}

  nsDialogs::Show
FunctionEnd


Function BrowseButtonClickedUpdate
  nsDialogs::SelectFolderDialog /NOUNLOAD "Select your existing Syndicate Wars install directory" "$update_path"
  Pop $3
  ${If} $3 != "error"
    StrCpy $update_path "$3"
    ${NSD_SetText} $dir_request "$3"
  ${EndIf}
FunctionEnd


Function UpdateInstallLeave
  ${NSD_GetText} $dir_request $update_path

  ${If} $update_path == ""
    MessageBox MB_ICONEXCLAMATION|MB_OK "You must indicate your existing Syndicate Wars install path before proceeding."
    Abort
  ${EndIf}

  ; Refuse to write into anything that is not recognisably a game install. This
  ; installer replaces files in a folder it did not create, so a mistyped path
  ; must never get as far as the install section.
  IfFileExists "$update_path\data\*.*" 0 not_a_game_dir
  IfFileExists "$update_path\qdata\*.*" 0 not_a_game_dir
  Goto is_a_game_dir
not_a_game_dir:
  MessageBox MB_ICONEXCLAMATION|MB_OK "$update_path does not look like a Syndicate Wars installation - it has no 'data' and 'qdata' folders.$\r$\n$\r$\nThis setup only updates an existing fan port install. Install the game first using the standard SyndWarsFX installer."
  Abort
is_a_game_dir:

  ${NSD_GetState} $5 $3  ; desktop icon
  ${NSD_GetState} $6 $4  ; start menu entry
  ${NSD_GetState} $7 $8  ; portable install

  ${If} $3 == ${BST_CHECKED}
    StrCpy $selected_desk_shortcut "1"
  ${Else}
    StrCpy $selected_desk_shortcut "0"
  ${EndIf}

  ${If} $4 == ${BST_CHECKED}
    StrCpy $selected_menu_shortcut "1"
  ${Else}
    StrCpy $selected_menu_shortcut "0"
  ${EndIf}

  ${If} $8 == ${BST_CHECKED}
    StrCpy $portable_install "1"
  ${Else}
    StrCpy $portable_install "0"
  ${EndIf}

  StrCpy $INSTDIR $update_path
FunctionEnd


; --------------------
; INSTALL HELPERS

Function BackupExistingInstall
  ClearErrors
  StrCpy $backup_dir "$INSTDIR\fx3d-backup-${PRODUCT_VERSION}"
  DetailPrint "Backing up the previous install to $backup_dir ..."
  CreateDirectory "$backup_dir"

  IfFileExists "$INSTDIR\syndwarsfx3d.exe" 0 +2
    CopyFiles /SILENT "$INSTDIR\syndwarsfx3d.exe" "$backup_dir\syndwarsfx3d.exe"
  IfFileExists "$INSTDIR\syndwarsfx.exe" 0 +2
    CopyFiles /SILENT "$INSTDIR\syndwarsfx.exe" "$backup_dir\syndwarsfx.exe"
  IfFileExists "$INSTDIR\swars.exe" 0 +2
    CopyFiles /SILENT "$INSTDIR\swars.exe" "$backup_dir\swars.exe"

  IfFileExists "$INSTDIR\conf\*.*" 0 backup_done
    CreateDirectory "$backup_dir\conf"
    CopyFiles /SILENT "$INSTDIR\conf\*.*" "$backup_dir\conf"
backup_done:
FunctionEnd


Function InstallLightsConf
  ; `fx3d.ini` carries the renderer tuning and gains new sections with
  ; every FX3D release, but players edit it. Never overwrite an existing one -
  ; drop the new defaults alongside it instead.
  ClearErrors
  SetOutPath $PLUGINSDIR\conf
  File "${BUILDENV_PKG_DIR}\conf\fx3d.ini"
  IfFileExists "$INSTDIR\conf\fx3d.ini" lights_exists 0
    DetailPrint "Installing conf\fx3d.ini ..."
    CopyFiles /SILENT "$PLUGINSDIR\conf\fx3d.ini" "$INSTDIR\conf\fx3d.ini"
    Goto lights_done
lights_exists:
    DetailPrint "Keeping your existing conf\fx3d.ini"
    DetailPrint "New defaults written to conf\fx3d.ini.new - merge in any"
    DetailPrint "new sections to pick up this release's renderer settings."
    CopyFiles /SILENT "$PLUGINSDIR\conf\fx3d.ini" "$INSTDIR\conf\fx3d.ini.new"
lights_done:
  SetOutPath $INSTDIR
FunctionEnd


Function InstallRegistry
  ; The game reads HKCU first and falls back to HKLM (see sys_get_data_path in
  ; src/oswindws.c), so write both - HKLM may belong to a different account if
  ; setup was elevated with other credentials.
  DetailPrint "Adding the installation path to registry..."
  SetRegView 32
  WriteRegStr HKCU "Software\SyndWarsFX\CurrentVersion" InstallPath $INSTDIR
  WriteRegStr HKLM "Software\SyndWarsFX\CurrentVersion" InstallPath $INSTDIR
  ClearErrors ; HKLM write may legitimately fail without elevation
FunctionEnd


; --------------------
; DATA PACKAGE UPDATES

Function UpdateDataPackages
  ClearErrors

  ;Update levels and maps from syndwarsfx-levels repository

  StrCpy $levels_md5 "${LEVELS_PKG_MD5}"

  IfFileExists "$PLUGINSDIR\${LEVELS_PACKAGE}.zip" +2 0
  Call DownloadLevels

  md5dll::GetMD5File "$PLUGINSDIR\${LEVELS_PACKAGE}.zip"
  Pop $0
  StrCmp $0 $levels_md5 extract_level_files retry_level_download

retry_level_download:
  DetailPrint "Error detected with level files zip, retrying download"
  Call DownloadLevels
  md5dll::GetMD5File "$PLUGINSDIR\${LEVELS_PACKAGE}.zip"
  Pop $0
  StrCmp $0 $levels_md5 extract_level_files 0
  DetailPrint "Error downloading level files from syndwarsfx-levels repository, please check the source file."
  Goto packages_fail

extract_level_files:
  DetailPrint "Extracting updated game levels..."
  nsisunz::Unzip "$PLUGINSDIR\${LEVELS_PACKAGE}.zip" "$PLUGINSDIR\"
  CreateDirectory $INSTDIR\levels
  CreateDirectory $INSTDIR\maps
  CopyFiles /SILENT $PLUGINSDIR\syndwarsfx\levels\* $INSTDIR\levels
  CopyFiles /SILENT $PLUGINSDIR\syndwarsfx\maps\* $INSTDIR\maps

  ;Update graphics files from syndwarsfx-gfx repository

  StrCpy $gfx_md5 "${GFX_PKG_MD5}"

  IfFileExists "$PLUGINSDIR\${GFX_PACKAGE}.zip" +2 0
  Call DownloadGfx

  md5dll::GetMD5File "$PLUGINSDIR\${GFX_PACKAGE}.zip"
  Pop $0
  StrCmp $0 $gfx_md5 extract_gfx_files retry_gfx_download

retry_gfx_download:
  DetailPrint "Error detected with graphics files zip, retrying download"
  Call DownloadGfx
  md5dll::GetMD5File "$PLUGINSDIR\${GFX_PACKAGE}.zip"
  Pop $0
  StrCmp $0 $gfx_md5 extract_gfx_files 0
  DetailPrint "Error downloading graphics files from syndwarsfx-gfx repository, please check the source file."
  Goto packages_fail

extract_gfx_files:
  DetailPrint "Extracting updated graphics files..."
  nsisunz::Unzip "$PLUGINSDIR\${GFX_PACKAGE}.zip" "$PLUGINSDIR\"
  CopyFiles /SILENT $PLUGINSDIR\syndwarsfx\data\* $INSTDIR\data
  CopyFiles /SILENT $PLUGINSDIR\syndwarsfx\qdata\* $INSTDIR\qdata
  CopyFiles /SILENT $PLUGINSDIR\syndwarsfx\language\* $INSTDIR\language

  ;Update sound files from syndwarsfx-sfx repository

  StrCpy $sfx_md5 "${SFX_PKG_MD5}"

  IfFileExists "$PLUGINSDIR\${SFX_PACKAGE}.zip" +2 0
  Call DownloadSfx

  md5dll::GetMD5File "$PLUGINSDIR\${SFX_PACKAGE}.zip"
  Pop $0
  StrCmp $0 $sfx_md5 extract_sfx_files retry_sfx_download

retry_sfx_download:
  DetailPrint "Error detected with sound files zip, retrying download"
  Call DownloadSfx
  md5dll::GetMD5File "$PLUGINSDIR\${SFX_PACKAGE}.zip"
  Pop $0
  StrCmp $0 $sfx_md5 extract_sfx_files 0
  DetailPrint "Error downloading sound files from syndwarsfx-sfx repository, please check the source file."
  Goto packages_fail

extract_sfx_files:
  DetailPrint "Extracting updated sound files..."
  nsisunz::Unzip "$PLUGINSDIR\${SFX_PACKAGE}.zip" "$PLUGINSDIR\"
  CreateDirectory $INSTDIR\sound
  CopyFiles /SILENT $PLUGINSDIR\syndwarsfx\sound\* $INSTDIR\sound
  Return

packages_fail:
  SetErrors
FunctionEnd


Function DownloadLevels
  DetailPrint "Downloading latest game levels from Github..."
  DetailPrint "https://github.com/swfans/syndwarsfx-levels/releases/download/${LEVELS_VERSION}/${LEVELS_PACKAGE}.zip"
  inetc::get "https://github.com/swfans/syndwarsfx-levels/releases/download/${LEVELS_VERSION}/${LEVELS_PACKAGE}.zip" "$PLUGINSDIR\${LEVELS_PACKAGE}.zip"
  Pop $0
FunctionEnd


Function DownloadGfx
  DetailPrint "Downloading latest game graphics from Github..."
  DetailPrint "https://github.com/swfans/syndwarsfx-gfx/releases/download/${GFX_VERSION}/${GFX_PACKAGE}.zip"
  inetc::get "https://github.com/swfans/syndwarsfx-gfx/releases/download/${GFX_VERSION}/${GFX_PACKAGE}.zip" "$PLUGINSDIR\${GFX_PACKAGE}.zip"
  Pop $0
FunctionEnd


Function DownloadSfx
  DetailPrint "Downloading latest game sound files from Github..."
  DetailPrint "https://github.com/swfans/syndwarsfx-sfx/releases/download/${SFX_VERSION}/${SFX_PACKAGE}.zip"
  inetc::get "https://github.com/swfans/syndwarsfx-sfx/releases/download/${SFX_VERSION}/${SFX_PACKAGE}.zip" "$PLUGINSDIR\${SFX_PACKAGE}.zip"
  Pop $0
FunctionEnd
