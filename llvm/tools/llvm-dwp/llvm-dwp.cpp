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
#include "llvm/ADT/STLExtras.h"
#include "llvm/DWP/DWP.h"
#include "llvm/DWP/DWPError.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ObjectFile.h"
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
} // end anonymous namespace

// Options
static std::vector<std::string> ExecFilenames;
static std::string OutputFilename;
static std::string ContinueOption;

static Expected<SmallVector<std::string, 16>>
getDWOFilenames(StringRef ExecFilename) {
  auto ErrOrObj = object::ObjectFile::createObjectFile(ExecFilename);
  if (!ErrOrObj)
    return ErrOrObj.takeError();

  const ObjectFile &Obj = *ErrOrObj.get().getBinary();
  std::unique_ptr<DWARFContext> DWARFCtx = DWARFContext::create(Obj);

  SmallVector<std::string, 16> DWOPaths;
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
  return std::move(DWOPaths);
}

struct DWPArchKey {
  std::string Label;

  friend bool operator==(const DWPArchKey &LHS, const DWPArchKey &RHS) {
    return LHS.Label == RHS.Label;
  }

  friend bool operator<(const DWPArchKey &LHS, const DWPArchKey &RHS) {
    return LHS.Label < RHS.Label;
  }
};

struct DWPInputGroup {
  DWPArchKey Key;
  std::vector<std::string> Inputs;
};

struct DWPOutputGroup {
  DWPArchKey Key;
  ArrayRef<std::string> Inputs;
  std::string Label;
  std::string Path;
};

static DWPArchKey getArchKey(const object::ObjectFile &Obj) {
  const auto *ELFObj = dyn_cast<object::ELFObjectFileBase>(&Obj);
  if (!ELFObj)
    return {};
  // tryGetCPUName() maps the ELF identity to a target CPU name for us:
  // "gfx1031"/"gfx90a" for AMDGPU, "sm_80"/... for CUDA, and std::nullopt for
  // targets without a CPU concept (e.g. host x86-64) -> no filename suffix.
  return {ELFObj->tryGetCPUName().value_or("").str()};
}

static StringRef getArchLabel(const DWPArchKey &Key) { return Key.Label; }

static Expected<SmallVector<DWPInputGroup, 4>>
groupInputsByArch(ArrayRef<std::string> DWOFilenames) {
  SmallVector<DWPInputGroup, 4> Groups;
  for (const auto &Filename : DWOFilenames) {
    auto Obj = ObjectFile::createObjectFile(Filename);
    if (!Obj)
      return createFileError(Filename, Obj.takeError());
    const auto Key = getArchKey(*Obj->getBinary());
    auto It = llvm::find_if(
        Groups, [&](const DWPInputGroup &Group) { return Group.Key == Key; });
    if (It == Groups.end()) {
      Groups.emplace_back(DWPInputGroup{Key, {}});
      It = std::prev(Groups.end());
    }
    It->Inputs.emplace_back(Filename);
  }
  llvm::stable_sort(Groups,
                    [](const DWPInputGroup &LHS, const DWPInputGroup &RHS) {
                      if (LHS.Key.Label.empty() != RHS.Key.Label.empty())
                        return LHS.Key.Label.empty();
                      return LHS.Key < RHS.Key;
                    });
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

static SmallVector<DWPOutputGroup, 4>
assignOutputPaths(ArrayRef<DWPInputGroup> Groups, StringRef OutputFilename) {
  SmallVector<DWPOutputGroup, 4> OutputGroups;
  if (Groups.size() == 1) {
    // -o name.dwp is respected even if .dwo set is from non-host target
    // for single group.
    OutputGroups.emplace_back(DWPOutputGroup{
        Groups.front().Key, Groups.front().Inputs, "", OutputFilename.str()});
  } else {
    for (const DWPInputGroup &Group : Groups) {
      auto Label = getArchLabel(Group.Key).str();
      auto Path = deriveOutputPath(OutputFilename, Label);
      OutputGroups.emplace_back(DWPOutputGroup{
          Group.Key, Group.Inputs, std::move(Label), std::move(Path)});
    }
  }
  return OutputGroups;
}

static int writeOutputGroup(const DWPOutputGroup &Group,
                            OnCuIndexOverflow OverflowOptValue,
                            Dwarf64StrOffsetsPromotion Dwarf64StrOffsetsValue) {
  std::error_code EC;
  ToolOutputFile OutFile(Group.Path, EC, sys::fs::OF_None);
  if (EC)
    return error(Twine(Group.Path) + ": " + EC.message(), "dwp output init");
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
      write(Writer, Group.Inputs, OverflowOptValue, Dwarf64StrOffsetsValue, OS);
  if (Err) {
    logAllUnhandledErrors(std::move(Err), WithColor::error());
    return 1;
  }
  OutFile.keep();
  return 0;
}

static int
writeOutputGroups(ArrayRef<DWPOutputGroup> Groups,
                  OnCuIndexOverflow OverflowOptValue,
                  Dwarf64StrOffsetsPromotion Dwarf64StrOffsetsValue) {
  for (const auto &Group : Groups) {
    if (writeOutputGroup(Group, OverflowOptValue, Dwarf64StrOffsetsValue))
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

  for (const auto &ExecFilename : ExecFilenames) {
    auto DWOs = getDWOFilenames(ExecFilename);
    if (!DWOs) {
      logAllUnhandledErrors(
          handleErrors(DWOs.takeError(),
                       [&](std::unique_ptr<ECError> EC) -> Error {
                         return createFileError(ExecFilename,
                                                Error(std::move(EC)));
                       }),
          WithColor::error());
      return 1;
    }
    DWOFilenames.insert(DWOFilenames.end(),
                        std::make_move_iterator(DWOs->begin()),
                        std::make_move_iterator(DWOs->end()));
  }

  if (DWOFilenames.empty()) {
    WithColor::defaultWarningHandler(make_error<DWPError>(
        "executable file does not contain any references to dwo files"));
    return 0;
  }

  StringRef DiscardPrefix = Args.getLastArgValue(OPT_prioritizeDiscardPath, "");
  if (OverflowOptValue == OnCuIndexOverflow::SoftStop &&
      !DiscardPrefix.empty()) {
    SmallString<256> CanonicalDiscardPrefix(DiscardPrefix);
    if (std::error_code EC =
            sys::fs::real_path(DiscardPrefix, CanonicalDiscardPrefix)) {
      WithColor::warning() << "invalid --prioritize-discard-path '"
                           << DiscardPrefix << "': " << EC.message()
                           << "; ignoring option.\n";
    } else {
      StringRef PrefixRef(CanonicalDiscardPrefix);
      auto IsNonDiscarded = [&](const std::string &Name) {
        SmallString<256> CanonicalDWO;
        if (sys::fs::real_path(Name, CanonicalDWO))
          return true;
        StringRef DWORef(CanonicalDWO);
        if (!DWORef.starts_with(PrefixRef))
          return true;
        if (DWORef.size() == PrefixRef.size())
          return false;
        if (sys::path::is_separator(DWORef[PrefixRef.size()]))
          return false;
        return true;
      };
      std::stable_partition(DWOFilenames.begin(), DWOFilenames.end(),
                            IsNonDiscarded);
    }
  }

  auto Groups = groupInputsByArch(DWOFilenames);
  if (!Groups) {
    logAllUnhandledErrors(Groups.takeError(), WithColor::error());
    return 1;
  }

  auto OutputGroups = assignOutputPaths(*Groups, OutputFilename);
  if (writeOutputGroups(OutputGroups, OverflowOptValue, Dwarf64StrOffsetsValue))
    return 1;

  return 0;
}
