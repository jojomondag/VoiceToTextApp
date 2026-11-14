#define AppName "VoiceToText"
#define AppVersion "0.1.0"
#define AppPublisher "VoiceToText"
#define AppExeName "VoiceToText.exe"

[Setup]
AppId={{F1E4F1C6-CE4D-4E6B-8F8D-7B9C8D52D2E1}
AppName={#AppName}
AppVersion={#AppVersion}
AppPublisher={#AppPublisher}
DefaultDirName={localappdata}\Programs\VoiceToText
DefaultGroupName={#AppName}
OutputDir=..\..\dist\installer
OutputBaseFilename=VoiceToText-Windows-Setup
Compression=lzma2
SolidCompression=yes
PrivilegesRequired=lowest
ArchitecturesInstallIn64BitMode=x64
WizardStyle=modern
UninstallDisplayIcon={app}\{#AppExeName}

[Files]
Source: "..\..\dist\VoiceToText\*"; DestDir: "{app}"; Flags: ignoreversion recursesubdirs createallsubdirs

[Icons]
Name: "{group}\VoiceToText"; Filename: "{app}\{#AppExeName}"
Name: "{autodesktop}\VoiceToText"; Filename: "{app}\{#AppExeName}"; Tasks: desktopicon

[Tasks]
Name: "desktopicon"; Description: "Create a desktop shortcut"; GroupDescription: "Additional shortcuts:"

[Run]
Filename: "{app}\{#AppExeName}"; Description: "Launch VoiceToText"; Flags: nowait postinstall skipifsilent
