; GameEngine2D Pro - Windows x64 installer
; Generated prerequisite metadata is written by scripts\build_installer.ps1.
; This installer is intentionally unsigned until an Authenticode certificate
; is configured for the product release pipeline.

#define AppName "GameEngine2D Pro"
#define AppVersion "1.0.0"
#define AppPublisher "GameEngine2D Pro"
#define AppExe "hub.exe"
#define StageRoot "..\build\installer_stage"
#include "generated\prerequisite_sizes.iss"

[Setup]
AppId={{14C143A4-72A9-4B7D-9834-A86F6F325B2E}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\GameEngine2D Pro
DefaultGroupName={#AppName}
DisableProgramGroupPage=yes
PrivilegesRequired=lowest
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
OutputDir=..\dist
OutputBaseFilename=GameEngine2DPro-Setup-{#AppVersion}-x64
Compression=lzma2/ultra64
SolidCompression=yes
WizardStyle=modern
UninstallDisplayName={#AppName}
UninstallDisplayIcon={app}\hub.exe
SetupLogging=yes
VersionInfoVersion={#AppVersion}
VersionInfoCompany={#AppPublisher}
VersionInfoDescription={#AppName} installer

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"; Flags: unchecked
Name: "launchhub"; Description: "Launch GameEngine Hub after installation"; Flags: unchecked

[Dirs]
Name: "{app}\games"
Name: "{app}\logs"

[Files]
; Engine runtime and project templates. SDL2 is deliberately bundled here:
; end users never need a separate SDL2 installation to use the engine.
Source: "{#StageRoot}\hub.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\editor.exe"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\SDL2.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\editor_symbols.map"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\shaders\*"; DestDir: "{app}\shaders"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\assets\*"; DestDir: "{app}\assets"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\engine_cpp\*"; DestDir: "{app}\engine_cpp"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\cmake\*"; DestDir: "{app}\cmake"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\editor\CMakeLists.txt"; DestDir: "{app}\editor"; Flags: ignoreversion
Source: "{#StageRoot}\editor\src\*"; DestDir: "{app}\editor\src"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\editor\third_party\imgui\*"; DestDir: "{app}\editor\third_party\imgui"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\editor\scripts_module\*"; DestDir: "{app}\editor\scripts_module"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\nlohmann\*"; DestDir: "{app}\nlohmann"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\third_party\sdl2\*"; DestDir: "{app}\third_party\sdl2"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\templates\*"; DestDir: "{app}\templates"; Flags: ignoreversion recursesubdirs createallsubdirs
; Prebuilt native modules for the shipped Abyss template. Hub copies them
; into the selected project's namespace on creation, avoiding a first-launch
; C++ rebuild while preserving normal targeted Reload Scripts behavior later.
Source: "{#StageRoot}\build\scripts_module_fast\abyss_of_hollows\Release\*.dll"; DestDir: "{app}\build\scripts_module_fast\abyss_of_hollows\Release"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "{#StageRoot}\CMakeLists.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#StageRoot}\LICENSE*"; DestDir: "{app}"; Flags: ignoreversion skipifsourcedoesntexist
Source: "{#StageRoot}\THIRD_PARTY_NOTICES.md"; DestDir: "{app}"; Flags: ignoreversion

; Required first-run prerequisites. The build script obtains these only from
; the official Microsoft, VS Code and LunarG endpoints and emits their exact
; staged size, version and SHA-256 into generated\prerequisite_sizes.iss.
Source: "{#StageRoot}\prerequisites\vc_redist.x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#StageRoot}\prerequisites\vs_buildtools.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#StageRoot}\prerequisites\VSCodeUserSetup-x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#StageRoot}\prerequisites\VulkanRT-x64.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall
Source: "{#StageRoot}\prerequisites\vulkan_sdk.exe"; DestDir: "{tmp}"; Flags: deleteafterinstall

[Icons]
Name: "{group}\GameEngine Hub"; Filename: "{app}\hub.exe"; WorkingDir: "{app}"
Name: "{group}\GameEngine Editor"; Filename: "{app}\editor.exe"; WorkingDir: "{app}"
Name: "{autodesktop}\GameEngine Hub"; Filename: "{app}\hub.exe"; WorkingDir: "{app}"; Tasks: desktopicon

[Run]
Filename: "{app}\hub.exe"; Description: "Launch GameEngine Hub"; WorkingDir: "{app}"; Tasks: launchhub; Flags: nowait postinstall skipifsilent

[Code]
function LoadLibraryW(lpFileName: String): THandle;
  external 'LoadLibraryW@kernel32.dll stdcall setuponly';
function FreeLibrary(hLibModule: THandle): Boolean;
  external 'FreeLibrary@kernel32.dll stdcall setuponly';

function VulkanLoaderAvailable(): Boolean;
var
  Handle: THandle;
begin
  Handle := LoadLibraryW('vulkan-1.dll');
  Result := Handle <> 0;
  if Result then FreeLibrary(Handle);
end;

function VulkanSdkInstalled(): Boolean;
var
  InstallPath: String;
begin
  Result := RegQueryStringValue(HKLM64, 'SOFTWARE\LunarG\VulkanSDK', 'InstallDir', InstallPath) and DirExists(InstallPath);
  if not Result then
    Result := GetEnv('VULKAN_SDK') <> '';
end;

function VSCodeInstalled(): Boolean;
var
  InstallPath: String;
begin
  Result := FileExists(ExpandConstant('{localappdata}\Programs\Microsoft VS Code\Code.exe'));
  if not Result then
    Result := RegQueryStringValue(HKCU, 'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Code.exe', '', InstallPath) and FileExists(InstallPath);
  if not Result then
    Result := RegQueryStringValue(HKLM64, 'SOFTWARE\Microsoft\Windows\CurrentVersion\App Paths\Code.exe', '', InstallPath) and FileExists(InstallPath);
end;

procedure RunElevatedPrerequisite(const FileName, Parameters, Description: String);
var
  ErrorCode: Integer;
begin
  WizardForm.StatusLabel.Caption := Description;
  WizardForm.Update;
  Log(Description + ': ' + ExpandConstant(FileName));
  // The engine is installed per-user, but Microsoft Build Tools, the VC++
  // redistributable and Vulkan packages can require machine changes. Run them
  // through the documented Windows elevation verb and wait before continuing.
  if not ShellExec('runas', ExpandConstant(FileName), Parameters, '', SW_SHOWNORMAL,
                   ewWaitUntilTerminated, ErrorCode) then begin
    MsgBox(Description + ' could not start. Windows error ' + IntToStr(ErrorCode) +
      '. Setup cannot continue without this required dependency.', mbError, MB_OK);
    RaiseException('Required prerequisite launch failed: ' + Description);
  end;
end;

procedure RunUserPrerequisite(const FileName, Parameters, Description: String);
var
  ExitCode: Integer;
begin
  WizardForm.StatusLabel.Caption := Description;
  WizardForm.Update;
  Log(Description + ': ' + ExpandConstant(FileName));
  if (not Exec(ExpandConstant(FileName), Parameters, '', SW_SHOWNORMAL,
      ewWaitUntilTerminated, ExitCode)) or ((ExitCode <> 0) and (ExitCode <> 3010)) then begin
    MsgBox(Description + ' failed (exit code ' + IntToStr(ExitCode) +
      '). Setup cannot continue without this required dependency.', mbError, MB_OK);
    RaiseException('Required prerequisite failed: ' + Description);
  end;
end;

procedure InitializeWizard;
var
  Page: TOutputMsgWizardPage;
  Contents: String;
begin
  Contents :=
    'The following required dependencies are included with this installer:' + #13#10 + #13#10 +
    '  GameEngine Hub, Editor, templates and bundled SDL2: {#EnginePayloadSize}' + #13#10 +
    '  Microsoft Visual C++ x64 Redistributable {#VcRedistVersion}: {#VcRedistSize}' + #13#10 +
    '  Visual Studio Code {#VSCodeVersion}: {#VSCodeSize}' + #13#10 +
    '  Visual Studio Build Tools bootstrap {#BuildToolsVersion}: {#BuildToolsSize}' + #13#10 +
    '    (Microsoft downloads the selected MSVC/CMake workload; reserve about 10 GB and an internet connection.)' + #13#10 +
    '  Vulkan Runtime loader {#VulkanRuntimeVersion}: {#VulkanRuntimeSize}' + #13#10 +
    '  Vulkan SDK {#VulkanSdkVersion}: {#VulkanSdkSize}' + #13#10 + #13#10 +
    'SDL2 is shipped inside GameEngine2D Pro, so there is no separate SDL2 installer.' + #13#10 +
    'Vulkan loader setup is automatic. A Vulkan-capable GPU driver/ICD remains a hardware-vendor requirement; this setup will never overwrite a graphics driver.';
  Page := CreateOutputMsgPage(wpSelectDir, 'Installation contents',
    'Required engine dependencies and download sizes', Contents);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then begin
    RunElevatedPrerequisite('{tmp}\vc_redist.x64.exe', '/install /quiet /norestart',
      'Installing Microsoft Visual C++ x64 Redistributable');
    if not VSCodeInstalled() then
      RunUserPrerequisite('{tmp}\VSCodeUserSetup-x64.exe', '/VERYSILENT /NORESTART /MERGETASKS=!runcode',
        'Installing Visual Studio Code');
    RunElevatedPrerequisite('{tmp}\vs_buildtools.exe',
      '--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --add Microsoft.VisualStudio.Component.VC.Tools.x86.x64 --add Microsoft.VisualStudio.Component.VC.CMake.Project --includeRecommended',
      'Installing the required C++ authoring toolchain (MSVC, MSBuild and CMake)');
    if not VulkanLoaderAvailable() then
      RunElevatedPrerequisite('{tmp}\VulkanRT-x64.exe', '--accept-licenses --default-answer --confirm-command install',
        'Installing Vulkan Runtime loader');
    if not VulkanSdkInstalled() then
      RunElevatedPrerequisite('{tmp}\vulkan_sdk.exe', '--accept-licenses --default-answer --confirm-command install',
        'Installing the required Vulkan SDK for engine and standalone builds');
    if not VulkanLoaderAvailable() then begin
      MsgBox('GameEngine2D Pro was installed, but Windows still cannot load Vulkan. The setup installed the Vulkan Runtime loader; install or update the driver for your actual NVIDIA, AMD, or Intel GPU, then launch Hub again. A generic installer cannot provide a compatible graphics driver or Vulkan ICD for every GPU.', mbError, MB_OK);
    end else begin
      Log('Vulkan loader preflight succeeded. A compatible GPU ICD is supplied by the installed GPU driver.');
    end;
  end;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpFinished then begin
    WizardForm.FinishedLabel.Caption :=
      'GameEngine2D Pro is ready to use.' + #13#10 + #13#10 +
      'Installed location:' + #13#10 + ExpandConstant('{app}') + #13#10 + #13#10 +
      'Start GameEngine Hub from the Start menu, desktop shortcut (if selected), or:' + #13#10 +
      ExpandConstant('{app}\hub.exe') + #13#10 + #13#10 +
      'Projects created by the Hub are stored in:' + #13#10 +
      ExpandConstant('{app}\games');
  end;
end;
