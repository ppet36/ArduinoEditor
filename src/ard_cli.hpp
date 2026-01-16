/*
 * Arduino Editor
 * Copyright (c) 2025 Pavel Petržela
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include "utils.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <vector>
#include <variant>
#include <wx/weakref.h>
#include <wx/wx.h>

using json = nlohmann::json;

struct SerialPortInfo {
  std::string address;  // "/dev/cu.usbmodem2101" or "COM3"
  std::string label;    // what is displayed in choice
  std::string protocol; // "serial" / "network" ...
};

struct ArduinoProgrammerInfo {
  std::string id;   // e.g. "usbasp"
  std::string name; // e.g. "USBasp"
};

struct ArduinoCoreBoard {
  std::string name; // "Arduino Uno"
  std::string fqbn; // "arduino:avr:uno" - can be empty in older releases
};

struct ArduinoCoreRelease {
  std::string version; // key from the "releases" map, e.g. "1.8.6"
  std::string name;    // "Arduino AVR Boards"

  std::vector<std::string> types; // "types": ["Arduino"]
  std::vector<ArduinoCoreBoard> boards;

  bool compatible = false; // "compatible": true/false
  bool installed = false;  // "installed": true for the just installed version
};

struct ArduinoCoreInfo {
  std::string id;         // "arduino:avr"
  std::string maintainer; // "Arduino"
  std::string website;    // "http://www.arduino.cc/"
  std::string email;      // "packages@arduino.cc"
  bool indexed = false;   // "indexed": true/false

  std::vector<std::string> availableVersions; // keys in "releases": "1.6.2", "1.6.3", ...
  std::string installedVersion;               // "installed_version": "1.8.6"
  std::string latestVersion;                  // "latest_version": "1.8.6"

  std::vector<ArduinoCoreRelease> releases; // details of all releases
};

struct ArduinoLibraryRelease {
  std::string version;
  std::string author;
  std::string maintainer;
  std::string sentence;
  std::string paragraph;
  std::string website;
  std::string category;
  std::vector<std::string> architectures;
  std::vector<std::string> types;

  std::string url;             // resources.url
  std::string archiveFileName; // resources.archive_filename
  std::string checksum;        // resources.checksum
  int size = 0;                // resources.size

  std::vector<std::string> providesIncludes;
  std::vector<std::string> dependencies;

  std::string installDir; // install_dir
  std::string sourceDir;  // source_dir
  bool isLegacy = false;
  std::string location;              // user / builtin / ...
  std::string layout;                // flat / recursive / ...
  std::vector<std::string> examples; // list of paths to examples
};

struct ResolvedLibraryInfo {
  std::string name;
  std::string version;
  std::string includePath;
  bool isCoreLibrary;
};

struct ArduinoLibraryInfo {
  std::string name;

  // last (newest) release, as reported by "latest"
  ArduinoLibraryRelease latest;

  // list of available versions (strings only)
  std::vector<std::string> availableVersions;

  // complete list of releases according to "releases" (you can then draw more detailed information from it)
  std::vector<ArduinoLibraryRelease> releases;
};

// What type of library installation source
enum class ArduinoLibraryInstallSource {
  Repo, // name in the Arduino registry, e.g. "AudioZero" or "AudioZero@1.0.0"
  Git,  // URL to git, e.g. "https://github.com/arduino-libraries/WiFi101.git#0.16.0"
  Zip   // path to the ZIP archive
};

struct ArduinoLibraryInstallSpec {
  ArduinoLibraryInstallSource source;
  std::string spec; // meaning according to source: library name / git URL / zip path
};

struct ArduinoCliConfig {
  // entire raw JSON from `arduino-cli config dump --format json`
  std::string rawJson;

  // known keys
  std::vector<std::string> boardManagerAdditionalUrls; // board_manager.additional_urls
  std::string networkConnectionTimeout;                // network.connection_timeout
  bool boardManagerEnableUnsafeInstall = false;
  std::string networkProxy; // expected format: user:pass@host:port OR host:port
};

struct ArduinoBoardOptionValue {
  std::string id;    // internal ID (e.g. "cdc", "default", "240")
  std::string label; // text for the user (e.g. "Enabled", "240 MHz")
  bool selected = false;
};

struct ArduinoBoardOption {
  std::string id;    // Menu ID (e.g., "CDCOnBoot", "CPUFreq")
  std::string label; // menu description ("USB CDC on Boot", "CPU Frequency")
  std::vector<ArduinoBoardOptionValue> values;
};

struct MemUsage {
  long long flashUsed = -1;
  int flashPct = -1;
  long long flashMax = -1;

  long long ramUsed = -1;
  int ramPct = -1;
  long long ramFree = -1;
  long long ramMax = -1;

  bool HasFlash() const { return flashUsed >= 0 && flashMax >= 0; }
  bool HasRam() const { return (ramUsed >= 0 && ramMax >= 0) || ramFree >= 0; }
};

using ArduinoOutdatedItem = std::variant<ArduinoCoreInfo, ArduinoLibraryInfo>;

class ArduinoCli {
private:
  struct ResolveLibInfo {
    std::filesystem::path libRoot;    // library folder (with library.properties)
    std::filesystem::path srcRoot;    // library folder for -I
    std::string name;                 // library name
    std::string normalizedName;       // normalizeLibName(name)
    std::vector<std::string> depends; // depends= from library.properties / JSON
    bool isCore = false;              // core vs user library (only info)
  };

  // Process termination
  std::atomic<bool> m_cancelRequested{false};
  std::mutex m_cancelMtx;

#if defined(__WXMSW__)
  HANDLE m_runningProcess{NULL};
#else
  pid_t m_runningPid{-1};
#endif

  // Cache for ResolveLibraries
  mutable std::mutex m_resolveCacheMutex;
  bool m_hasResolveLibrariesCache = false;
  std::vector<ResolveLibInfo> m_resolveLibs;
  std::unordered_map<std::string, std::string> m_resolveHeaderToLibSrc; // header -> srcRoot
  std::unordered_map<std::string, size_t> m_resolveSrcRootToLibIndex;   // srcRoot.string() -> index in m_resolveLibs
  std::unordered_map<std::string, size_t> m_resolveNameToLibIndex;      // normalizedName -> index in m_resolveLibs;

  std::string fqbn;
  std::string sketchPath;
  bool m_initializedFromCompileCommands = false;
  std::string m_cli = "";
  std::vector<std::string> clangArgs;
  std::string serialPort;
  std::string programmer;
  std::string m_platformPath;
  std::string m_corePlatformPath;
  std::vector<ArduinoLibraryInfo> libraries;
  std::vector<ArduinoLibraryInfo> installedLibraries;
  std::vector<ArduinoOutdatedItem> outdatedItems;
  std::vector<ArduinoCoreInfo> cores;
  std::vector<std::string> m_compileCommandsResolvedLibraries;

  mutable std::mutex m_usageMtx;
  MemUsage m_lastCompileUsage;

  std::atomic<bool> m_cancelAsync{false};

  int RunCliStreaming(const std::string &args, const wxWeakRef<wxEvtHandler> &weak, const char *finishedLabel);

  bool GetBoardOptions(const std::string &fqbn, std::vector<ArduinoBoardOption> &outOptions);
  std::vector<std::string> BuildClangArgsFromCompileCommands(const std::string &inoBaseName);
  std::vector<std::string> GetSystemIncludeArgsForCompiler(const std::string &compilerPath) const;
  std::string DetectClangTarget(const std::string &compilerPath) const;

  std::vector<std::string> BuildClangArgsFromBoardDetails(const nlohmann::json &j);

  bool ResolveLibInfoFromIncludePath(const std::string &includePath, ResolvedLibraryInfo &outInfo) const;

  bool LoadLibraries();
  bool LoadInstalledLibraries();

  std::string GetCliBaseCommand() const;

  void InitAttachedBoard();

  void QueueUiEvent(const wxWeakRef<wxEvtHandler> &weak, wxEvent *event);

  void TryParseCompileUsageLine(const std::string &line);

public:
  ArduinoCli(const std::string &sketchPath_, const std::string &cliPath = std::string());

  // Finds arduino-cli and returns the path to the cli. Inserts the version into version.
  static std::string DetectCliExecutable(const std::string &configValue, std::string *version);

  static int ExecuteCommand(const std::string &cmd, std::string &output);

  inline std::string GetPlatformPath() const { return m_platformPath; }

  // Kill for asynchronous currently running process
  bool CancelRunning();

  // Configurations
  ArduinoCliConfig GetConfig() const;
  bool SetConfigValue(const std::string &key, const std::string &value);
  bool SetConfigValue(const std::string &key, const std::vector<std::string> &values);
  bool ApplyConfig(const ArduinoCliConfig &cfg);

  // Load compiler directives includes, defines, libraries etc...
  bool LoadProperties();
  void LoadPropertiesAsync(wxEvtHandler *handler);
  std::string GetCachedEnviromentFqbn() const;
  inline bool IsInitializedFromCompileCommands() { return m_initializedFromCompileCommands; }

  // board details
  bool LoadBoardParameters(std::string &errorOut);
  void LoadBoardParametersAsync(wxEvtHandler *handler);

  // resolve libs
  std::vector<std::string> ResolveLibraries(const std::vector<std::string> &includes);
  // Resolve libraries used by the given sketch files (unsaved buffers included).
  bool GetResolvedLibraries(const std::vector<SketchFileBuffer> &files, std::vector<ResolvedLibraryInfo> &outLibs);
  // Asynchronous variant – copies the buffers into the worker thread.
  void GetResolvedLibrariesAsync(const std::vector<SketchFileBuffer> &files, wxEvtHandler *handler);
  // Returns include paths of libraries that came out of arduino-cli compile_commands
  // (detected via library.properties in include dir or one level up).
  std::vector<ResolvedLibraryInfo> GetResolvedLibrariesFromCompileCommands() const;

  bool LoadCores();
  void LoadCoresAsync(wxEvtHandler *handler);
  const std::vector<ArduinoCoreInfo> &GetCores() const;

  void CleanCachedEnvironment();
  void CleanBuildDirectory();
  void InvalidateLibraryCache();

  void CancelAsyncOperations();

  const std::vector<std::string> &GetCompilerArgs() const {
    return clangArgs;
  }

  bool GetDefaultBoardOptions(std::vector<ArduinoBoardOption> &outOptions);
  bool GetBoardOptions(std::vector<ArduinoBoardOption> &outOptions);
  void GetBoardOptionsAsync(wxEvtHandler *handler);

  void CompileAsync(wxEvtHandler *handler);
  MemUsage GetLastCompileUsage() const;
  void ClearLastCompileUsage();

  void UploadAsync(wxEvtHandler *handler);
  void BurnBootloaderAsync(wxEvtHandler *handler);

  std::string BuildFqbnFromOptions(const std::vector<ArduinoBoardOption> &options);

  void SetFQBN(const std::string &newFqbn);

  std::string GetFQBN();
  std::string GetBoardName() const;
  std::string GetTargetFromFQBN() const;

  bool IsLibraryArchitectureCompatible(const std::string &architectureRaw) const;
  bool IsLibraryArchitectureCompatible(const std::vector<std::string> &architectures) const;

  std::string GetSketchPath() const;
  std::string GetCliPath() const;

  std::vector<ArduinoCoreBoard> GetAvailableBoards();
  void GetAvailableBoardsAsync(wxEvtHandler *handler);

  // library searching
  bool SearchLibraryProvidingHeader(const std::string &header, std::vector<ArduinoLibraryInfo> &out);
  void SearchLibraryProvidingHeaderAsync(const std::string &header, wxEvtHandler *handler);

  // arduino libs management
  void LoadLibrariesAsync(wxEvtHandler *handler);
  void LoadInstalledLibrariesAsync(wxEvtHandler *handler);
  // Asynchronous installation (can install multiple libraries at once)
  // - specs: list (Repo/Git/Zip, spec)
  // - handler: receives EVT_COMMANDLINE_OUTPUT_MSG (output text)
  //            and after completion of EVT_INSTALLED_LIBRARIES_UPDATED
  void InstallLibrariesAsync(const std::vector<ArduinoLibraryInstallSpec> &specs, wxEvtHandler *handler);
  // Asynchronous uninstallation of libraries from the registry (by name).
  // - names: list of library names to uninstall
  // - handler: receives EVT_COMMANDLINE_OUTPUT_MSG (output from arduino-cli)
  //            and after completion of EVT_INSTALLED_LIBRARIES_UPDATED
  void UninstallLibrariesAsync(const std::vector<std::string> &names, wxEvtHandler *handler);

  // core(board) management
  void InstallCoresAsync(const std::vector<std::string> &coreIds, wxEvtHandler *handler);
  void UninstallCoresAsync(const std::vector<std::string> &coreIds, wxEvtHandler *handler);
  void UpdateCoreIndexAsync(wxEvtHandler *handler);
  void UpdateCoreIndexBackgroundAsync(wxEvtHandler *handler);

  void UpdateLibraryIndexAsync(wxEvtHandler *handler);
  void UpdateLibraryIndexBackgroundAsync(wxEvtHandler *handler);
  const std::vector<ArduinoLibraryInfo> &GetLibraries() const;
  const std::vector<ArduinoLibraryInfo> &GetInstalledLibraries() const;
  bool IsArduinoLibraryInstalled(const ArduinoLibraryInfo &info);

  bool LoadOutdated();
  void LoadOutdatedAsync(wxEvtHandler *handler);
  const std::vector<ArduinoOutdatedItem> &GetOutdatedItems() const;

  std::vector<SerialPortInfo> GetSerialPorts();
  void SetSerialPort(const std::string &port);
  const std::string &GetSerialPort() const;

  bool GetProgrammersForFqbn(const std::string &fqbn, std::vector<ArduinoProgrammerInfo> &out);
  void GetProgrammersForFqbnAsync(wxEvtHandler *handler, const std::string &fqbnArg = std::string());
  bool GetProgrammers(std::vector<ArduinoProgrammerInfo> &outProgrammers) {
    return GetProgrammersForFqbn(fqbn, outProgrammers);
  }

  bool SetProgrammer(const std::string &id);
  const std::string &GetProgrammer() const;
};
