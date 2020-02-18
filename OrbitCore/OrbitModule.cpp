//-----------------------------------
// Copyright Pierric Gimmig 2013-2017
//-----------------------------------

#include "OrbitModule.h"

#include <string>

#include "Core.h"
#include "Pdb.h"
#include "Serialization.h"
#include "absl/strings/str_format.h"

#ifndef WIN32
#include "Capture.h"
#include "LinuxUtils.h"
#include "OrbitProcess.h"
#include "OrbitUnreal.h"
#include "Params.h"
#include "Path.h"
#include "ScopeTimer.h"
#endif

//-----------------------------------------------------------------------------
Module::Module() { m_Pdb = std::make_shared<Pdb>(); }

//-----------------------------------------------------------------------------
std::string Module::GetPrettyName() {
  if (m_PrettyName.size() == 0) {
#ifdef WIN32
    m_PrettyName =
        absl::StrFormat("%s [%I64x - %I64x] %s\r\n", m_Name.c_str(),
                        m_AddressStart, m_AddressEnd, m_FullName.c_str());
    m_AddressRange =
        absl::StrFormat("[%I64x - %I64x]", m_AddressStart, m_AddressEnd);
#else
    m_PrettyName = m_FullName;
    m_AddressRange =
        absl::StrFormat("[%016llx - %016llx]", m_AddressStart, m_AddressEnd);
    m_PdbName = m_FullName;
    m_FoundPdb = true;
#endif
  }

  return m_PrettyName;
}

//-----------------------------------------------------------------------------
bool Module::IsDll() const {
  return ToLower(Path::GetExtension(m_FullName)) ==
             std::string(".dll") ||
         Contains(m_Name, ".so");
}

//-----------------------------------------------------------------------------
bool Module::LoadDebugInfo() {
  assert(m_Pdb);
  m_Pdb->SetMainModule(m_AddressStart);

  PRINT_VAR(m_FoundPdb);
  if (m_FoundPdb) {
    return m_Pdb->LoadDataFromPdb();
  }

  return false;
}

//-----------------------------------------------------------------------------
uint64_t Module::ValidateAddress(uint64_t a_Address) {
  if (ContainsAddress(a_Address)) return a_Address;

  // Treat input address as RVA
  uint64_t newAddress = m_AddressStart + a_Address;
  if (ContainsAddress(newAddress)) return newAddress;

  return 0xbadadd;
}

//-----------------------------------------------------------------------------
void Module::SetLoaded(bool a_Value) { m_Loaded = a_Value; }

//-----------------------------------------------------------------------------
ORBIT_SERIALIZE(Module, 0) {
  ORBIT_NVP_VAL(0, m_Name);
  ORBIT_NVP_VAL(0, m_FullName);
  ORBIT_NVP_VAL(0, m_PdbName);
  ORBIT_NVP_VAL(0, m_Directory);
  ORBIT_NVP_VAL(0, m_PrettyName);
  ORBIT_NVP_VAL(0, m_AddressRange);
  ORBIT_NVP_VAL(0, m_DebugSignature);
  ORBIT_NVP_VAL(0, m_AddressStart);
  ORBIT_NVP_VAL(0, m_AddressEnd);
  ORBIT_NVP_VAL(0, m_EntryPoint);
  ORBIT_NVP_VAL(0, m_FoundPdb);
  ORBIT_NVP_VAL(0, m_Selected);
  ORBIT_NVP_VAL(0, m_Loaded);
  ORBIT_NVP_VAL(0, m_PdbSize);
}

#ifndef WIN32

//-----------------------------------------------------------------------------
Function* Pdb::FunctionFromName(const std::string& a_Name) {
  uint64_t hash = StringHash(a_Name);
  auto iter = m_StringFunctionMap.find(hash);
  return (iter == m_StringFunctionMap.end()) ? nullptr : iter->second;
}

//-----------------------------------------------------------------------------
std::string FindSymbols(const std::string& module_path) {
  // Look for .debug files associated with passed in module name
  // TODO: .debug file might not have same name as module, we should check
  //       for unique identifier in the symbols file...

  std::string dir = Path::GetDirectory(module_path);
  std::vector<std::string> symbolDirectories = {"~/", Path::GetHome(), dir,
                                                dir + "debug_symbols/"};
  std::string file = Path::StripExtension(Path::GetFileName(module_path));

  for (auto& symbolDirectory : symbolDirectories) {
    std::string debugFile = symbolDirectory + file + ".debug";
    if (Path::FileExists(debugFile)) {
      PRINT_VAR(debugFile);
      return debugFile;
    }

    debugFile = symbolDirectory + file + ".elf.debug";
    if (Path::FileExists(debugFile)) {
      PRINT_VAR(debugFile);
      return debugFile;
    }
  }

  return module_path;
}

//-----------------------------------------------------------------------------
bool Pdb::LoadPdb(const char* a_PdbName) {
  m_FileName = FindSymbols(a_PdbName);
  m_Name = Path::GetFileName(m_FileName);

  {
    SCOPE_TIMER_LOG("nm");
    // nm
    // TODO: If we need linenumber information at some point, we need to find an
    // alternative, as "nm -l" is super slow.
    std::string nmCommand =
        std::string("nm ") + m_FileName + std::string(" -n");
    std::string nmResult = LinuxUtils::ExecuteCommand(nmCommand.c_str());
    std::stringstream nmStream(nmResult);
    std::string line;
    while (std::getline(nmStream, line, '\n')) {
      std::vector<std::string> tokens = Tokenize(line, " \t");
      if (tokens.size() > 2)  // nm
      {
        const auto& name = tokens[2];
        Function func(name, Path::GetFileName(a_PdbName),
                      std::stoull(tokens[0], nullptr, 16), 0, this);
        func.SetPrettyName(LinuxUtils::Demangle(name.c_str()));
        this->AddFunction(func);
      }
    }
  }

  ProcessData();

  {
    SCOPE_TIMER_LOG("objdump -tT");
    // find functions that can receive uprobes
    std::string objdumpCommand =
        std::string("objdump -tT ") + m_FileName +
        std::string(" | grep \"F .text\" | grep -oE '[^[:space:]]+$'");
    std::string objdumpResult =
        LinuxUtils::ExecuteCommand(objdumpCommand.c_str());
    std::stringstream objdumpStream(objdumpResult);
    std::string mangled;
    while (std::getline(objdumpStream, mangled, '\n')) {
      std::string demangled = LinuxUtils::Demangle(mangled.c_str());

      Function* func = FunctionFromName(demangled);
      if (func && func->Probe().empty()) {
        std::string probe = m_FileName + std::string(":") + mangled;
        func->SetProbe(probe);
      }
    }
  }

  return true;
}

//-----------------------------------------------------------------------------
void Pdb::LoadPdbAsync(const char* a_PdbName,
                       std::function<void()> a_CompletionCallback) {
  m_LoadingCompleteCallback = a_CompletionCallback;
  LoadPdb(a_PdbName);
  a_CompletionCallback();
}

//-----------------------------------------------------------------------------
void Pdb::ProcessData() {
  SCOPE_TIMER_LOG("ProcessData");
  ScopeLock lock(Capture::GTargetProcess->GetDataMutex());

  auto& functions = Capture::GTargetProcess->GetFunctions();
  auto& globals = Capture::GTargetProcess->GetGlobals();

  functions.reserve(functions.size() + m_Functions.size());

  for (Function& func : m_Functions) {
    func.SetPdb(this);
    functions.push_back(&func);
    GOrbitUnreal.OnFunctionAdded(&func);
  }

  if (GParams.m_FindFileAndLineInfo) {
    SCOPE_TIMER_LOG("Find File and Line info");
    for (Function& func : m_Functions) {
      func.FindFile();
    }
  }

  for (Type& type : m_Types) {
    type.m_Pdb = this;
    Capture::GTargetProcess->AddType(type);
    GOrbitUnreal.OnTypeAdded(&type);
  }

  for (Variable& var : m_Globals) {
    var.m_Pdb = this;
    globals.push_back(&var);
  }

  for (auto& it : m_TypeMap) {
    it.second.m_Pdb = this;
  }

  PopulateFunctionMap();
  PopulateStringFunctionMap();
}

//-----------------------------------------------------------------------------
void Pdb::PopulateFunctionMap() {
  SCOPE_TIMER_LOG("Pdb::PopulateFunctionMap");
  for (Function& function : m_Functions) {
    m_FunctionMap.insert(std::make_pair(function.Address(), &function));
  }
}

//-----------------------------------------------------------------------------
void Pdb::PopulateStringFunctionMap() {
  {
    // SCOPE_TIMER_LOG("Reserving map");
    m_StringFunctionMap.reserve(unsigned(1.5f * (float)m_Functions.size()));
  }

  {
    // SCOPE_TIMER_LOG("Map inserts");
    for (Function& Function : m_Functions) {
      m_StringFunctionMap[Function.Hash()] = &Function;
    }
  }
}

//-----------------------------------------------------------------------------
Function* Pdb::GetFunctionFromExactAddress(uint64_t a_Address) {
  uint64_t RVA = a_Address - (uint64_t)GetHModule();
  auto iter = m_FunctionMap.find(RVA);
  return (iter != m_FunctionMap.end()) ? iter->second : nullptr;
}

//-----------------------------------------------------------------------------
Function* Pdb::GetFunctionFromProgramCounter(uint64_t a_Address) {
  uint64_t RVA = a_Address - (uint64_t)GetHModule();

  auto it = m_FunctionMap.upper_bound(RVA);
  if (!m_FunctionMap.empty() && it != m_FunctionMap.begin()) {
    --it;
    Function* func = it->second;
    return func;
  }

  return nullptr;
}

#endif

//-----------------------------------------------------------------------------
ORBIT_SERIALIZE(ModuleDebugInfo, 0) {
  ORBIT_NVP_VAL(0, m_Pid);
  ORBIT_NVP_VAL(0, m_Name);
  ORBIT_NVP_VAL(0, m_Functions);
}
