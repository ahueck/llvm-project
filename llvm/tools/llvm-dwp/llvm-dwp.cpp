//===-- llvm-dwp.cpp - Split DWARF merging tool for llvm ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A utility for merging DWARF 5 Split DWARF .dwo files into .dwp (DWARF
// package files).
//
//===----------------------------------------------------------------------===//
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/DWP/DWP.h"
#include "llvm/DWP/DWPError.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/Error.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/OffloadBinary.h"
#include "llvm/Object/OffloadBundle.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LLVMDriver.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ToolOutputFile.h"
#include <iterator>
#include <optional>

using namespace llvm;
using namespace llvm::object;

// Command-line option boilerplate.
namespace {
enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(...) LLVM_MAKE_OPT_ID(__VA_ARGS__),
#include "Opts.inc"
#undef OPTION
};

#define OPTTABLE_STR_TABLE_CODE
#include "Opts.inc"
#undef OPTTABLE_STR_TABLE_CODE

#define OPTTABLE_PREFIXES_TABLE_CODE
#include "Opts.inc"
#undef OPTTABLE_PREFIXES_TABLE_CODE

using namespace llvm::opt;
static constexpr opt::OptTable::Info InfoTable[] = {
#define OPTION(...) LLVM_CONSTRUCT_OPT_INFO(__VA_ARGS__),
#include "Opts.inc"
#undef OPTION
};

class DwpOptTable : public opt::GenericOptTable {
public:
  DwpOptTable()
      : GenericOptTable(OptionStrTable, OptionPrefixesTable, InfoTable) {}
};

struct DWPArchKey {
  StringRef Label;

  static DWPArchKey createHostKey() { return {StringRef{}}; }

  static DWPArchKey createArchKey(const object::ObjectFile &Obj) {
    const auto *ELFObj = dyn_cast<object::ELFObjectFileBase>(&Obj);
    if (!ELFObj)
      return createHostKey();
    // Only AMDGPU and NVIDIA/CUDA objects are grouped by their GPU arch name
    // (e.g. "gfx90a", "sm_80"), everything else shares the host key.
    switch (ELFObj->getEMachine()) {
    case ELF::EM_AMDGPU:
    case ELF::EM_CUDA:
      return {ELFObj->tryGetCPUName().value_or("")};
    default:
      return createHostKey();
    }
  }

  StringRef getLabel() const { return Label; }

  friend bool operator==(const DWPArchKey &LHS, const DWPArchKey &RHS) {
    return LHS.Label == RHS.Label;
  }
};

struct DWPOutputGroup {
  std::vector<std::string> Inputs;
  std::string Path;
};

enum class CollectionMode : uint8_t { Flat, ByArch };

} // end anonymous namespace

namespace llvm {
template <> struct DenseMapInfo<DWPArchKey> {
  static unsigned getHashValue(const DWPArchKey &Key) {
    return DenseMapInfo<StringRef>::getHashValue(Key.Label);
  }
  static bool isEqual(const DWPArchKey &LHS, const DWPArchKey &RHS) {
    return LHS == RHS;
  }
};
} // namespace llvm

// Options
static std::vector<std::string> ExecFilenames;
static std::string OutputFilename;
static std::string ContinueOption;

using DWPOutputGroups = SmallMapVector<DWPArchKey, DWPOutputGroup, 4>;

static void appendDWOFilenames(const ObjectFile &Obj,
                               SmallVectorImpl<std::string> &DWOPaths) {
  std::unique_ptr<DWARFContext> DWARFCtx = DWARFContext::create(Obj);

  for (const auto &CU : DWARFCtx->compile_units()) {
    const DWARFDie &Die = CU->getUnitDIE();
    std::string DWOName = dwarf::toString(
        Die.find({dwarf::DW_AT_dwo_name, dwarf::DW_AT_GNU_dwo_name}), "");
    if (DWOName.empty())
      continue;
    std::string DWOCompDir =
        dwarf::toString(Die.find(dwarf::DW_AT_comp_dir), "");
    if (!DWOCompDir.empty()) {
      SmallString<16> DWOPath(DWOName);
      sys::path::make_absolute(DWOCompDir, DWOPath);
      if (!sys::fs::exists(DWOPath) && sys::fs::exists(DWOName))
        DWOPaths.push_back(std::move(DWOName));
      else
        DWOPaths.emplace_back(DWOPath.data(), DWOPath.size());
    } else {
      DWOPaths.push_back(std::move(DWOName));
    }
  }
}

static void appendObjectDWOsToGroups(const ObjectFile &Obj, CollectionMode Mode,
                                     DWPOutputGroups &Groups) {
  SmallVector<std::string, 16> DWOs;
  appendDWOFilenames(Obj, DWOs);
  if (DWOs.empty())
    return;
  const DWPArchKey Key = Mode == CollectionMode::ByArch
                             ? DWPArchKey::createArchKey(Obj)
                             : DWPArchKey::createHostKey();
  auto &Inputs = Groups[Key].Inputs;
  Inputs.insert(Inputs.end(), DWOs.begin(), DWOs.end());
}

namespace offload {

static Expected<MemoryBufferRef>
getBundleEntryBuffer(const ObjectFile &HostObj, OffloadBundleFatBin &Bundle,
                     const OffloadBundleEntry &Entry) {
  MemoryBufferRef Source;
  if (Bundle.isDecompressed()) {
    Source = Bundle.DecompressedBuffer->getMemBufferRef();
  } else {
    Expected<MemoryBufferRef> SourceOrErr = HostObj.getMemoryBufferRef();
    if (!SourceOrErr)
      return SourceOrErr.takeError();
    Source = *SourceOrErr;
  }

  StringRef Contents = Source.getBuffer();
  if (Entry.Offset > Contents.size())
    return createStringError("offload bundle entry '%s' offset (%llu"
                             ") is beyond the end of the source (%zu)",
                             Entry.ID.c_str(),
                             static_cast<unsigned long long>(Entry.Offset),
                             Contents.size());
  if (Entry.Size > Contents.size() - Entry.Offset)
    return createStringError(
        "offload bundle entry '%s' offset + size (%llu"
        " + %llu"
        ") is beyond the end of the source (%zu)",
        Entry.ID.c_str(), static_cast<unsigned long long>(Entry.Offset),
        static_cast<unsigned long long>(Entry.Size), Contents.size());

  StringRef Slice = Contents.slice(Entry.Offset, Entry.Offset + Entry.Size);
  return MemoryBufferRef(Slice, Entry.ID);
}

static Error appendObjectBufferDWOsToGroups(MemoryBufferRef Buffer,
                                            StringRef HostFilename,
                                            DWPOutputGroups &Groups) {
  Expected<std::unique_ptr<ObjectFile>> Obj =
      ObjectFile::createObjectFile(Buffer);
  if (!Obj) {
    bool InvalidFileType = false;
    Error Remaining = handleErrors(
        Obj.takeError(), [&](std::unique_ptr<ECError> EC) -> Error {
          if (EC->convertToErrorCode() == object_error::invalid_file_type) {
            InvalidFileType = true;
            return Error::success();
          }
          return Error(std::move(EC));
        });
    if (InvalidFileType)
      return Error::success();
    return createFileError(HostFilename, std::move(Remaining));
  }

  appendObjectDWOsToGroups(**Obj, CollectionMode::ByArch, Groups);
  return Error::success();
}

static Error addFatbinDWOsToGroups(const ObjectFile &HostObj,
                                   StringRef HostFilename,
                                   DWPOutputGroups &Groups) {
  SmallVector<OffloadBundleFatBin, 4> Bundles;
  if (Error Err = extractOffloadBundleFatBinary(HostObj, Bundles))
    return createFileError(HostFilename, std::move(Err));

  for (OffloadBundleFatBin &Bundle : Bundles) {
    for (const OffloadBundleEntry &Entry : Bundle.getEntries()) {
      if (Entry.Size == 0 || StringRef(Entry.ID).starts_with("host-"))
        continue;

      Expected<MemoryBufferRef> EntryBuffer =
          getBundleEntryBuffer(HostObj, Bundle, Entry);
      if (!EntryBuffer)
        return createFileError(HostFilename, EntryBuffer.takeError());

      if (Error Err =
              appendObjectBufferDWOsToGroups(*EntryBuffer, HostFilename, Groups))
        return Err;
    }
  }

  return Error::success();
}

static Error addOffloadBinaryDWOsToGroups(const ObjectFile &HostObj,
                                          StringRef HostFilename,
                                          DWPOutputGroups &Groups) {
  SmallVector<OffloadFile, 4> Files;
  Expected<MemoryBufferRef> HostBuffer = HostObj.getMemoryBufferRef();
  if (!HostBuffer)
    return createFileError(HostFilename, HostBuffer.takeError());
  if (Error Err = extractOffloadBinaries(*HostBuffer, Files))
    return createFileError(HostFilename, std::move(Err));

  for (OffloadFile &File : Files) {
    StringRef Image = File.getBinary()->getImage();
    if (Image.empty())
      continue;

    MemoryBufferRef EntryBuffer(Image, HostFilename);
    if (Error Err =
            appendObjectBufferDWOsToGroups(EntryBuffer, HostFilename, Groups))
      return Err;
  }

  return Error::success();
}

} // namespace offload

static Expected<DWPOutputGroups>
collectInputGroups(ArrayRef<std::string> DWOFilenames,
                   ArrayRef<std::string> ExecFilenames, CollectionMode Mode) {
  DWPOutputGroups Groups;
  if (Mode == CollectionMode::ByArch) {
    for (const auto &Filename : DWOFilenames) {
      auto Obj = ObjectFile::createObjectFile(Filename);
      if (!Obj)
        return createFileError(Filename, Obj.takeError());
      const DWPArchKey Key = DWPArchKey::createArchKey(*Obj->getBinary());
      Groups[Key].Inputs.push_back(Filename);
    }
  } else {
    Groups[DWPArchKey::createHostKey()].Inputs = DWOFilenames.vec();
  }

  for (const auto &ExecFilename : ExecFilenames) {
    auto Obj = ObjectFile::createObjectFile(ExecFilename);
    if (!Obj)
      return createFileError(ExecFilename, Obj.takeError());

    appendObjectDWOsToGroups(*Obj->getBinary(), Mode, Groups);

    if (Mode == CollectionMode::ByArch) {
      if (Error Err = offload::addFatbinDWOsToGroups(*Obj->getBinary(),
                                                     ExecFilename, Groups))
        return std::move(Err);
      if (Error Err = offload::addOffloadBinaryDWOsToGroups(
              *Obj->getBinary(), ExecFilename, Groups))
        return std::move(Err);
    }
  }
  return Groups;
}

static std::string deriveOutputPath(StringRef OutputFilename, StringRef Label) {
  // Host/default group keeps the user-provided -o path, e.g. "out.dwp".
  if (Label.empty())
    return OutputFilename.str();

  SmallString<128> Path(OutputFilename);
  StringRef Extension = sys::path::extension(Path);

  // No extension: append ".<label>", e.g. "out" -> "out.gfx1031".
  if (Extension.empty()) {
    Path += ".";
    Path += Label;
    return Path.str().str();
  }

  // Existing extension: insert ".<label>" before it, e.g. "out.dwp" ->
  // "out.gfx1031.dwp".
  SmallString<16> NewExtension(".");
  NewExtension += Label;
  NewExtension += Extension;
  sys::path::replace_extension(Path, NewExtension);
  return Path.str().str();
}

static int error(const Twine &Error, const Twine &Context) {
  errs() << Twine("while processing ") + Context + ":\n";
  errs() << Twine("error: ") + Error + "\n";
  return 1;
}

static void assignOutputPaths(DWPOutputGroups &Groups,
                              StringRef OutputFilename) {
  if (Groups.size() == 1) {
    // -o name.dwp is respected even if .dwo set is from non-host target
    // for single group.
    DWPOutputGroup &Group = Groups.front().second;
    Group.Path = OutputFilename.str();
  } else {
    for (auto &[Key, Group] : Groups) {
      Group.Path = deriveOutputPath(OutputFilename, Key.getLabel());
    }
  }
}

static int writeOutputFile(StringRef Path, ArrayRef<std::string> Inputs,
                           OnCuIndexOverflow OverflowOptValue,
                           Dwarf64StrOffsetsPromotion Dwarf64StrOffsetsValue) {
  std::error_code EC;
  ToolOutputFile OutFile(Path, EC, sys::fs::OF_None);
  if (EC)
    return error(Twine(Path) + ": " + EC.message(), "dwp output init");
  std::optional<buffer_ostream> BOS;
  raw_pwrite_stream *OS;
  if (OutFile.os().supportsSeeking()) {
    OS = &OutFile.os();
  } else {
    BOS.emplace(OutFile.os());
    OS = &*BOS;
  }

  // Use DWPWriter for direct ELF output
  DWPWriter Writer;

  auto Err =
      write(Writer, Inputs, OverflowOptValue, Dwarf64StrOffsetsValue, OS);
  if (Err) {
    logAllUnhandledErrors(std::move(Err), WithColor::error());
    return 1;
  }
  OutFile.keep();
  return 0;
}

static int
emitOutputForGroups(DWPOutputGroups &Groups, StringRef OutputFilename,
                    bool HasValidDiscardPrefix,
                    StringRef CanonicalDiscardPrefix,
                    OnCuIndexOverflow OverflowOptValue,
                    Dwarf64StrOffsetsPromotion Dwarf64StrOffsetsValue) {
  if (llvm::none_of(Groups, [](const auto &Group) {
        return !Group.second.Inputs.empty();
      })) {
    WithColor::defaultWarningHandler(make_error<DWPError>(
        "executable file does not contain any references to dwo files"));
    return 0;
  }

  if (HasValidDiscardPrefix) {
    const auto PrioritizeNonDiscardedInputs =
        [&](const std::string &Name) -> bool {
      SmallString<256> CanonicalDWO;
      if (sys::fs::real_path(Name, CanonicalDWO))
        return true;
      StringRef DWORef(CanonicalDWO);
      if (!DWORef.starts_with(CanonicalDiscardPrefix))
        return true;
      if (DWORef.size() == CanonicalDiscardPrefix.size())
        return false;
      if (sys::path::is_separator(DWORef[CanonicalDiscardPrefix.size()]))
        return false;
      return true;
    };
    for (auto &[Key, Group] : Groups)
      std::stable_partition(Group.Inputs.begin(), Group.Inputs.end(),
                            PrioritizeNonDiscardedInputs);
  }

  assignOutputPaths(Groups, OutputFilename);
  for (const auto &[Key, Group] : Groups) {
    if (writeOutputFile(Group.Path, Group.Inputs, OverflowOptValue,
                        Dwarf64StrOffsetsValue))
      return 1;
  }
  return 0;
}

int llvm_dwp_main(int argc, char **argv, const llvm::ToolContext &) {
  DwpOptTable Tbl;
  llvm::BumpPtrAllocator A;
  llvm::StringSaver Saver{A};
  OnCuIndexOverflow OverflowOptValue = OnCuIndexOverflow::HardStop;
  Dwarf64StrOffsetsPromotion Dwarf64StrOffsetsValue =
      Dwarf64StrOffsetsPromotion::Disabled;

  opt::InputArgList Args =
      Tbl.parseArgs(argc, argv, OPT_UNKNOWN, Saver, [&](StringRef Msg) {
        llvm::errs() << Msg << '\n';
        std::exit(1);
      });

  if (Args.hasArg(OPT_help)) {
    Tbl.printHelp(llvm::outs(), "llvm-dwp [options] <input files>",
                  "merge split dwarf (.dwo) files");
    std::exit(0);
  }

  if (Args.hasArg(OPT_version)) {
    llvm::cl::PrintVersionMessage();
    std::exit(0);
  }

  const bool SplitByArch = Args.hasArg(OPT_splitByArch);
  OutputFilename = Args.getLastArgValue(OPT_outputFileName, "");
  if (Arg *Arg = Args.getLastArg(OPT_continueOnCuIndexOverflow,
                                 OPT_continueOnCuIndexOverflow_EQ)) {
    if (Arg->getOption().matches(OPT_continueOnCuIndexOverflow)) {
      OverflowOptValue = OnCuIndexOverflow::Continue;
    } else {
      ContinueOption = Arg->getValue();
      if (ContinueOption == "soft-stop") {
        OverflowOptValue = OnCuIndexOverflow::SoftStop;
      } else if (ContinueOption == "continue") {
        OverflowOptValue = OnCuIndexOverflow::Continue;
      } else {
        llvm::errs() << "invalid value for --continue-on-cu-index-overflow"
                     << ContinueOption << '\n';
        exit(1);
      }
    }
  }

  if (Arg *Arg = Args.getLastArg(OPT_dwarf64StringOffsets,
                                 OPT_dwarf64StringOffsets_EQ)) {
    if (Arg->getOption().matches(OPT_dwarf64StringOffsets)) {
      Dwarf64StrOffsetsValue = Dwarf64StrOffsetsPromotion::Enabled;
    } else {
      std::string OptValue = Arg->getValue();
      if (OptValue == "disabled") {
        Dwarf64StrOffsetsValue = Dwarf64StrOffsetsPromotion::Disabled;
      } else if (OptValue == "enabled") {
        Dwarf64StrOffsetsValue = Dwarf64StrOffsetsPromotion::Enabled;
      } else if (OptValue == "always") {
        Dwarf64StrOffsetsValue = Dwarf64StrOffsetsPromotion::Always;
      } else {
        llvm::errs()
            << "invalid value for --dwarf64-str-offsets-promotion. Valid "
               "values are one of: \"enabled\", \"disabled\" or \"always\".\n";
        exit(1);
      }
    }
  }

  for (const llvm::opt::Arg *A : Args.filtered(OPT_execFileNames))
    ExecFilenames.emplace_back(A->getValue());

  std::vector<std::string> DWOFilenames;
  for (const llvm::opt::Arg *A : Args.filtered(OPT_INPUT))
    DWOFilenames.emplace_back(A->getValue());

  StringRef DiscardPrefix = Args.getLastArgValue(OPT_prioritizeDiscardPath, "");
  SmallString<256> CanonicalDiscardPrefix;
  bool HasValidDiscardPrefix = false;
  if (OverflowOptValue == OnCuIndexOverflow::SoftStop &&
      !DiscardPrefix.empty()) {
    if (std::error_code EC =
            sys::fs::real_path(DiscardPrefix, CanonicalDiscardPrefix)) {
      WithColor::warning() << "invalid --prioritize-discard-path '"
                           << DiscardPrefix << "': " << EC.message()
                           << "; ignoring option.\n";
    } else {
      HasValidDiscardPrefix = true;
    }
  }

  const CollectionMode Mode =
      SplitByArch ? CollectionMode::ByArch : CollectionMode::Flat;
  auto GroupsOrErr = collectInputGroups(DWOFilenames, ExecFilenames, Mode);
  if (!GroupsOrErr) {
    logAllUnhandledErrors(GroupsOrErr.takeError(), WithColor::error());
    return 1;
  }

  return emitOutputForGroups(*GroupsOrErr, OutputFilename,
                             HasValidDiscardPrefix, CanonicalDiscardPrefix,
                             OverflowOptValue, Dwarf64StrOffsetsValue);
}
