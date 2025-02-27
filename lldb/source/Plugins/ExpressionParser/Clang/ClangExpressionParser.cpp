//===-- ClangExpressionParser.cpp -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTDiagnostic.h"
#include "clang/AST/ExternalASTSource.h"
#include "clang/AST/PrettyPrinter.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/Version.h"
#include "clang/CodeGen/CodeGenAction.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/Edit/Commit.h"
#include "clang/Edit/EditedSource.h"
#include "clang/Edit/EditsReceiver.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Parse/ParseAST.h"
#include "clang/Rewrite/Core/Rewriter.h"
#include "clang/Rewrite/Frontend/FrontendActions.h"
#include "clang/Sema/CodeCompleteConsumer.h"
#include "clang/Sema/Sema.h"
#include "clang/Sema/SemaConsumer.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/Support/CrashRecoveryContext.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/TargetSelect.h"

#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Signals.h"

#include "ClangDiagnostic.h"
#include "ClangExpressionParser.h"
#include "ClangUserExpression.h"

#include "ASTUtils.h"
#include "ClangASTSource.h"
#include "ClangDiagnostic.h"
#include "ClangExpressionDeclMap.h"
#include "ClangExpressionHelper.h"
#include "ClangExpressionParser.h"
#include "ClangHost.h"
#include "ClangModulesDeclVendor.h"
#include "ClangPersistentVariables.h"
#include "IRDynamicChecks.h"
#include "IRForTarget.h"
#include "ModuleDependencyCollector.h"

#include "lldb/Core/Debugger.h"
#include "lldb/Core/Disassembler.h"
#include "lldb/Core/Module.h"
#include "lldb/Core/StreamFile.h"
#include "lldb/Expression/IRExecutionUnit.h"
#include "lldb/Expression/IRInterpreter.h"
#include "lldb/Host/File.h"
#include "lldb/Host/HostInfo.h"
#include "lldb/Symbol/ClangASTContext.h"
#include "lldb/Symbol/SymbolVendor.h"
#include "lldb/Target/ExecutionContext.h"
#include "lldb/Target/Language.h"
#include "lldb/Target/Process.h"
#include "lldb/Target/Target.h"
#include "lldb/Target/ThreadPlanCallFunction.h"
#include "lldb/Utility/DataBufferHeap.h"
#include "lldb/Utility/LLDBAssert.h"
#include "lldb/Utility/Log.h"
#include "lldb/Utility/Reproducer.h"
#include "lldb/Utility/Stream.h"
#include "lldb/Utility/StreamString.h"
#include "lldb/Utility/StringList.h"

#include "Plugins/LanguageRuntime/ObjC/ObjCLanguageRuntime.h"

#include <cctype>
#include <memory>

using namespace clang;
using namespace llvm;
using namespace lldb_private;

//===----------------------------------------------------------------------===//
// Utility Methods for Clang
//===----------------------------------------------------------------------===//

class ClangExpressionParser::LLDBPreprocessorCallbacks : public PPCallbacks {
  ClangModulesDeclVendor &m_decl_vendor;
  ClangPersistentVariables &m_persistent_vars;
  clang::SourceManager &m_source_mgr;
  StreamString m_error_stream;
  bool m_has_errors = false;

public:
  LLDBPreprocessorCallbacks(ClangModulesDeclVendor &decl_vendor,
                            ClangPersistentVariables &persistent_vars,
                            clang::SourceManager &source_mgr)
      : m_decl_vendor(decl_vendor), m_persistent_vars(persistent_vars),
        m_source_mgr(source_mgr) {}

  void moduleImport(SourceLocation import_location, clang::ModuleIdPath path,
                    const clang::Module * /*null*/) override {
    // Ignore modules that are imported in the wrapper code as these are not
    // loaded by the user.
    llvm::StringRef filename =
        m_source_mgr.getPresumedLoc(import_location).getFilename();
    if (filename == ClangExpressionSourceCode::g_prefix_file_name)
      return;

    SourceModule module;

    for (const std::pair<IdentifierInfo *, SourceLocation> &component : path)
      module.path.push_back(ConstString(component.first->getName()));

    StreamString error_stream;

    ClangModulesDeclVendor::ModuleVector exported_modules;
    if (!m_decl_vendor.AddModule(module, &exported_modules, m_error_stream))
      m_has_errors = true;

    for (ClangModulesDeclVendor::ModuleID module : exported_modules)
      m_persistent_vars.AddHandLoadedClangModule(module);
  }

  bool hasErrors() { return m_has_errors; }

  llvm::StringRef getErrorString() { return m_error_stream.GetString(); }
};

class ClangDiagnosticManagerAdapter : public clang::DiagnosticConsumer {
public:
  ClangDiagnosticManagerAdapter(DiagnosticOptions &opts) {
    DiagnosticOptions *m_options = new DiagnosticOptions(opts);
    m_options->ShowPresumedLoc = true;
    m_options->ShowLevel = false;
    m_os.reset(new llvm::raw_string_ostream(m_output));
    m_passthrough.reset(
        new clang::TextDiagnosticPrinter(*m_os, m_options, false));
  }

  void ResetManager(DiagnosticManager *manager = nullptr) {
    m_manager = manager;
  }

  void HandleDiagnostic(DiagnosticsEngine::Level DiagLevel,
                        const clang::Diagnostic &Info) override {
    // Render diagnostic message to m_output.
    m_output.clear();
    m_passthrough->HandleDiagnostic(DiagLevel, Info);
    m_os->flush();

    if (m_manager) {
      lldb_private::DiagnosticSeverity severity;
      bool make_new_diagnostic = true;

      switch (DiagLevel) {
      case DiagnosticsEngine::Level::Fatal:
      case DiagnosticsEngine::Level::Error:
        severity = eDiagnosticSeverityError;
        break;
      case DiagnosticsEngine::Level::Warning:
        severity = eDiagnosticSeverityWarning;
        break;
      case DiagnosticsEngine::Level::Remark:
      case DiagnosticsEngine::Level::Ignored:
        severity = eDiagnosticSeverityRemark;
        break;
      case DiagnosticsEngine::Level::Note:
        m_manager->AppendMessageToDiagnostic(m_output);
        make_new_diagnostic = false;
      }
      if (make_new_diagnostic) {
        // ClangDiagnostic messages are expected to have no whitespace/newlines
        // around them.
        std::string stripped_output = llvm::StringRef(m_output).trim();

        ClangDiagnostic *new_diagnostic =
            new ClangDiagnostic(stripped_output, severity, Info.getID());
        m_manager->AddDiagnostic(new_diagnostic);

        // Don't store away warning fixits, since the compiler doesn't have
        // enough context in an expression for the warning to be useful.
        // FIXME: Should we try to filter out FixIts that apply to our generated
        // code, and not the user's expression?
        if (severity == eDiagnosticSeverityError) {
          size_t num_fixit_hints = Info.getNumFixItHints();
          for (size_t i = 0; i < num_fixit_hints; i++) {
            const clang::FixItHint &fixit = Info.getFixItHint(i);
            if (!fixit.isNull())
              new_diagnostic->AddFixitHint(fixit);
          }
        }
      }
    }
  }

  clang::TextDiagnosticPrinter *GetPassthrough() { return m_passthrough.get(); }

private:
  DiagnosticManager *m_manager = nullptr;
  std::shared_ptr<clang::TextDiagnosticPrinter> m_passthrough;
  /// Output stream of m_passthrough.
  std::shared_ptr<llvm::raw_string_ostream> m_os;
  /// Output string filled by m_os.
  std::string m_output;
};

static void SetupModuleHeaderPaths(CompilerInstance *compiler,
                                   std::vector<std::string> include_directories,
                                   lldb::TargetSP target_sp) {
  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));

  HeaderSearchOptions &search_opts = compiler->getHeaderSearchOpts();

  for (const std::string &dir : include_directories) {
    search_opts.AddPath(dir, frontend::System, false, true);
    LLDB_LOG(log, "Added user include dir: {0}", dir);
  }

  llvm::SmallString<128> module_cache;
  auto props = ModuleList::GetGlobalModuleListProperties();
  props.GetClangModulesCachePath().GetPath(module_cache);
  search_opts.ModuleCachePath = module_cache.str();
  LLDB_LOG(log, "Using module cache path: {0}", module_cache.c_str());

  search_opts.ResourceDir = GetClangResourceDir().GetPath();

  search_opts.ImplicitModuleMaps = true;
}

//===----------------------------------------------------------------------===//
// Implementation of ClangExpressionParser
//===----------------------------------------------------------------------===//

ClangExpressionParser::ClangExpressionParser(
    ExecutionContextScope *exe_scope, Expression &expr,
    bool generate_debug_info, std::vector<std::string> include_directories,
    std::string filename)
    : ExpressionParser(exe_scope, expr, generate_debug_info), m_compiler(),
      m_pp_callbacks(nullptr),
      m_include_directories(std::move(include_directories)),
      m_filename(std::move(filename)) {
  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));

  // We can't compile expressions without a target.  So if the exe_scope is
  // null or doesn't have a target, then we just need to get out of here.  I'll
  // lldb_assert and not make any of the compiler objects since
  // I can't return errors directly from the constructor.  Further calls will
  // check if the compiler was made and
  // bag out if it wasn't.

  if (!exe_scope) {
    lldb_assert(exe_scope, "Can't make an expression parser with a null scope.",
                __FUNCTION__, __FILE__, __LINE__);
    return;
  }

  lldb::TargetSP target_sp;
  target_sp = exe_scope->CalculateTarget();
  if (!target_sp) {
    lldb_assert(target_sp.get(),
                "Can't make an expression parser with a null target.",
                __FUNCTION__, __FILE__, __LINE__);
    return;
  }

  // 1. Create a new compiler instance.
  m_compiler.reset(new CompilerInstance());

  // When capturing a reproducer, hook up the file collector with clang to
  // collector modules and headers.
  if (repro::Generator *g = repro::Reproducer::Instance().GetGenerator()) {
    repro::FileProvider &fp = g->GetOrCreate<repro::FileProvider>();
    m_compiler->setModuleDepCollector(
        std::make_shared<ModuleDependencyCollectorAdaptor>(
            fp.GetFileCollector()));
    DependencyOutputOptions &opts = m_compiler->getDependencyOutputOpts();
    opts.IncludeSystemHeaders = true;
    opts.IncludeModuleFiles = true;
  }

  // Make sure clang uses the same VFS as LLDB.
  m_compiler->createFileManager(FileSystem::Instance().GetVirtualFileSystem());

  lldb::LanguageType frame_lang =
      expr.Language(); // defaults to lldb::eLanguageTypeUnknown
  bool overridden_target_opts = false;
  lldb_private::LanguageRuntime *lang_rt = nullptr;

  std::string abi;
  ArchSpec target_arch;
  target_arch = target_sp->GetArchitecture();

  const auto target_machine = target_arch.GetMachine();

  // If the expression is being evaluated in the context of an existing stack
  // frame, we introspect to see if the language runtime is available.

  lldb::StackFrameSP frame_sp = exe_scope->CalculateStackFrame();
  lldb::ProcessSP process_sp = exe_scope->CalculateProcess();

  // Make sure the user hasn't provided a preferred execution language with
  // `expression --language X -- ...`
  if (frame_sp && frame_lang == lldb::eLanguageTypeUnknown)
    frame_lang = frame_sp->GetLanguage();

  if (process_sp && frame_lang != lldb::eLanguageTypeUnknown) {
    lang_rt = process_sp->GetLanguageRuntime(frame_lang);
    LLDB_LOGF(log, "Frame has language of type %s",
              Language::GetNameForLanguageType(frame_lang));
  }

  // 2. Configure the compiler with a set of default options that are
  // appropriate for most situations.
  if (target_arch.IsValid()) {
    std::string triple = target_arch.GetTriple().str();
    m_compiler->getTargetOpts().Triple = triple;
    LLDB_LOGF(log, "Using %s as the target triple",
              m_compiler->getTargetOpts().Triple.c_str());
  } else {
    // If we get here we don't have a valid target and just have to guess.
    // Sometimes this will be ok to just use the host target triple (when we
    // evaluate say "2+3", but other expressions like breakpoint conditions and
    // other things that _are_ target specific really shouldn't just be using
    // the host triple. In such a case the language runtime should expose an
    // overridden options set (3), below.
    m_compiler->getTargetOpts().Triple = llvm::sys::getDefaultTargetTriple();
    LLDB_LOGF(log, "Using default target triple of %s",
              m_compiler->getTargetOpts().Triple.c_str());
  }
  // Now add some special fixes for known architectures: Any arm32 iOS
  // environment, but not on arm64
  if (m_compiler->getTargetOpts().Triple.find("arm64") == std::string::npos &&
      m_compiler->getTargetOpts().Triple.find("arm") != std::string::npos &&
      m_compiler->getTargetOpts().Triple.find("ios") != std::string::npos) {
    m_compiler->getTargetOpts().ABI = "apcs-gnu";
  }
  // Supported subsets of x86
  if (target_machine == llvm::Triple::x86 ||
      target_machine == llvm::Triple::x86_64) {
    m_compiler->getTargetOpts().Features.push_back("+sse");
    m_compiler->getTargetOpts().Features.push_back("+sse2");
  }

  // Set the target CPU to generate code for. This will be empty for any CPU
  // that doesn't really need to make a special
  // CPU string.
  m_compiler->getTargetOpts().CPU = target_arch.GetClangTargetCPU();

  // Set the target ABI
  abi = GetClangTargetABI(target_arch);
  if (!abi.empty())
    m_compiler->getTargetOpts().ABI = abi;

  // 3. Now allow the runtime to provide custom configuration options for the
  // target. In this case, a specialized language runtime is available and we
  // can query it for extra options. For 99% of use cases, this will not be
  // needed and should be provided when basic platform detection is not enough.
  if (lang_rt)
    overridden_target_opts =
        lang_rt->GetOverrideExprOptions(m_compiler->getTargetOpts());

  if (overridden_target_opts)
    if (log && log->GetVerbose()) {
      LLDB_LOGV(
          log, "Using overridden target options for the expression evaluation");

      auto opts = m_compiler->getTargetOpts();
      LLDB_LOGV(log, "Triple: '{0}'", opts.Triple);
      LLDB_LOGV(log, "CPU: '{0}'", opts.CPU);
      LLDB_LOGV(log, "FPMath: '{0}'", opts.FPMath);
      LLDB_LOGV(log, "ABI: '{0}'", opts.ABI);
      LLDB_LOGV(log, "LinkerVersion: '{0}'", opts.LinkerVersion);
      StringList::LogDump(log, opts.FeaturesAsWritten, "FeaturesAsWritten");
      StringList::LogDump(log, opts.Features, "Features");
    }

  // 4. Create and install the target on the compiler.
  m_compiler->createDiagnostics();
  auto target_info = TargetInfo::CreateTargetInfo(
      m_compiler->getDiagnostics(), m_compiler->getInvocation().TargetOpts);
  if (log) {
    LLDB_LOGF(log, "Using SIMD alignment: %d",
              target_info->getSimdDefaultAlign());
    LLDB_LOGF(log, "Target datalayout string: '%s'",
              target_info->getDataLayout().getStringRepresentation().c_str());
    LLDB_LOGF(log, "Target ABI: '%s'", target_info->getABI().str().c_str());
    LLDB_LOGF(log, "Target vector alignment: %d",
              target_info->getMaxVectorAlign());
  }
  m_compiler->setTarget(target_info);

  assert(m_compiler->hasTarget());

  // 5. Set language options.
  lldb::LanguageType language = expr.Language();
  LangOptions &lang_opts = m_compiler->getLangOpts();

  switch (language) {
  case lldb::eLanguageTypeC:
  case lldb::eLanguageTypeC89:
  case lldb::eLanguageTypeC99:
  case lldb::eLanguageTypeC11:
    // FIXME: the following language option is a temporary workaround,
    // to "ask for C, get C++."
    // For now, the expression parser must use C++ anytime the language is a C
    // family language, because the expression parser uses features of C++ to
    // capture values.
    lang_opts.CPlusPlus = true;
    break;
  case lldb::eLanguageTypeObjC:
    lang_opts.ObjC = true;
    // FIXME: the following language option is a temporary workaround,
    // to "ask for ObjC, get ObjC++" (see comment above).
    lang_opts.CPlusPlus = true;

    // Clang now sets as default C++14 as the default standard (with
    // GNU extensions), so we do the same here to avoid mismatches that
    // cause compiler error when evaluating expressions (e.g. nullptr not found
    // as it's a C++11 feature). Currently lldb evaluates C++14 as C++11 (see
    // two lines below) so we decide to be consistent with that, but this could
    // be re-evaluated in the future.
    lang_opts.CPlusPlus11 = true;
    break;
  case lldb::eLanguageTypeC_plus_plus:
  case lldb::eLanguageTypeC_plus_plus_11:
  case lldb::eLanguageTypeC_plus_plus_14:
    lang_opts.CPlusPlus11 = true;
    m_compiler->getHeaderSearchOpts().UseLibcxx = true;
    LLVM_FALLTHROUGH;
  case lldb::eLanguageTypeC_plus_plus_03:
    lang_opts.CPlusPlus = true;
    if (process_sp)
      lang_opts.ObjC =
          process_sp->GetLanguageRuntime(lldb::eLanguageTypeObjC) != nullptr;
    break;
  case lldb::eLanguageTypeObjC_plus_plus:
  case lldb::eLanguageTypeUnknown:
  default:
    lang_opts.ObjC = true;
    lang_opts.CPlusPlus = true;
    lang_opts.CPlusPlus11 = true;
    m_compiler->getHeaderSearchOpts().UseLibcxx = true;
    break;
  }

  lang_opts.Bool = true;
  lang_opts.WChar = true;
  lang_opts.Blocks = true;
  lang_opts.DebuggerSupport =
      true; // Features specifically for debugger clients
  if (expr.DesiredResultType() == Expression::eResultTypeId)
    lang_opts.DebuggerCastResultToId = true;

  lang_opts.CharIsSigned = ArchSpec(m_compiler->getTargetOpts().Triple.c_str())
                               .CharIsSignedByDefault();

  // Spell checking is a nice feature, but it ends up completing a lot of types
  // that we didn't strictly speaking need to complete. As a result, we spend a
  // long time parsing and importing debug information.
  lang_opts.SpellChecking = false;

  auto *clang_expr = dyn_cast<ClangUserExpression>(&m_expr);
  if (clang_expr && clang_expr->DidImportCxxModules()) {
    LLDB_LOG(log, "Adding lang options for importing C++ modules");

    lang_opts.Modules = true;
    // We want to implicitly build modules.
    lang_opts.ImplicitModules = true;
    // To automatically import all submodules when we import 'std'.
    lang_opts.ModulesLocalVisibility = false;

    // We use the @import statements, so we need this:
    // FIXME: We could use the modules-ts, but that currently doesn't work.
    lang_opts.ObjC = true;

    // Options we need to parse libc++ code successfully.
    // FIXME: We should ask the driver for the appropriate default flags.
    lang_opts.GNUMode = true;
    lang_opts.GNUKeywords = true;
    lang_opts.DoubleSquareBracketAttributes = true;
    lang_opts.CPlusPlus11 = true;

    SetupModuleHeaderPaths(m_compiler.get(), m_include_directories,
                           target_sp);
  }

  if (process_sp && lang_opts.ObjC) {
    if (auto *runtime = ObjCLanguageRuntime::Get(*process_sp)) {
      if (runtime->GetRuntimeVersion() ==
          ObjCLanguageRuntime::ObjCRuntimeVersions::eAppleObjC_V2)
        lang_opts.ObjCRuntime.set(ObjCRuntime::MacOSX, VersionTuple(10, 7));
      else
        lang_opts.ObjCRuntime.set(ObjCRuntime::FragileMacOSX,
                                  VersionTuple(10, 7));

      if (runtime->HasNewLiteralsAndIndexing())
        lang_opts.DebuggerObjCLiteral = true;
    }
  }

  lang_opts.ThreadsafeStatics = false;
  lang_opts.AccessControl = false; // Debuggers get universal access
  lang_opts.DollarIdents = true;   // $ indicates a persistent variable name
  // We enable all builtin functions beside the builtins from libc/libm (e.g.
  // 'fopen'). Those libc functions are already correctly handled by LLDB, and
  // additionally enabling them as expandable builtins is breaking Clang.
  lang_opts.NoBuiltin = true;

  // Set CodeGen options
  m_compiler->getCodeGenOpts().EmitDeclMetadata = true;
  m_compiler->getCodeGenOpts().InstrumentFunctions = false;
  m_compiler->getCodeGenOpts().setFramePointer(
                                    CodeGenOptions::FramePointerKind::All);
  if (generate_debug_info)
    m_compiler->getCodeGenOpts().setDebugInfo(codegenoptions::FullDebugInfo);
  else
    m_compiler->getCodeGenOpts().setDebugInfo(codegenoptions::NoDebugInfo);

  // Disable some warnings.
  m_compiler->getDiagnostics().setSeverityForGroup(
      clang::diag::Flavor::WarningOrError, "unused-value",
      clang::diag::Severity::Ignored, SourceLocation());
  m_compiler->getDiagnostics().setSeverityForGroup(
      clang::diag::Flavor::WarningOrError, "odr",
      clang::diag::Severity::Ignored, SourceLocation());

  // Inform the target of the language options
  //
  // FIXME: We shouldn't need to do this, the target should be immutable once
  // created. This complexity should be lifted elsewhere.
  m_compiler->getTarget().adjust(m_compiler->getLangOpts());

  // 6. Set up the diagnostic buffer for reporting errors

  auto diag_mgr = new ClangDiagnosticManagerAdapter(
      m_compiler->getDiagnostics().getDiagnosticOptions());
  m_compiler->getDiagnostics().setClient(diag_mgr);

  // 7. Set up the source management objects inside the compiler
  m_compiler->createFileManager();
  if (!m_compiler->hasSourceManager())
    m_compiler->createSourceManager(m_compiler->getFileManager());
  m_compiler->createPreprocessor(TU_Complete);

  if (ClangModulesDeclVendor *decl_vendor =
          target_sp->GetClangModulesDeclVendor()) {
    ClangPersistentVariables *clang_persistent_vars =
        llvm::cast<ClangPersistentVariables>(
            target_sp->GetPersistentExpressionStateForLanguage(
                lldb::eLanguageTypeC));
    std::unique_ptr<PPCallbacks> pp_callbacks(new LLDBPreprocessorCallbacks(
        *decl_vendor, *clang_persistent_vars, m_compiler->getSourceManager()));
    m_pp_callbacks =
        static_cast<LLDBPreprocessorCallbacks *>(pp_callbacks.get());
    m_compiler->getPreprocessor().addPPCallbacks(std::move(pp_callbacks));
  }

  // 8. Most of this we get from the CompilerInstance, but we also want to give
  // the context an ExternalASTSource.

  auto &PP = m_compiler->getPreprocessor();
  auto &builtin_context = PP.getBuiltinInfo();
  builtin_context.initializeBuiltins(PP.getIdentifierTable(),
                                     m_compiler->getLangOpts());

  m_compiler->createASTContext();
  clang::ASTContext &ast_context = m_compiler->getASTContext();

  m_ast_context.reset(new ClangASTContext(ast_context));

  std::string module_name("$__lldb_module");

  m_llvm_context.reset(new LLVMContext());
  m_code_generator.reset(CreateLLVMCodeGen(
      m_compiler->getDiagnostics(), module_name,
      m_compiler->getHeaderSearchOpts(), m_compiler->getPreprocessorOpts(),
      m_compiler->getCodeGenOpts(), *m_llvm_context));
}

ClangExpressionParser::~ClangExpressionParser() {}

namespace {

/// \class CodeComplete
///
/// A code completion consumer for the clang Sema that is responsible for
/// creating the completion suggestions when a user requests completion
/// of an incomplete `expr` invocation.
class CodeComplete : public CodeCompleteConsumer {
  CodeCompletionTUInfo m_info;

  std::string m_expr;
  unsigned m_position = 0;
  CompletionRequest &m_request;
  /// The printing policy we use when printing declarations for our completion
  /// descriptions.
  clang::PrintingPolicy m_desc_policy;

  /// Returns true if the given character can be used in an identifier.
  /// This also returns true for numbers because for completion we usually
  /// just iterate backwards over iterators.
  ///
  /// Note: lldb uses '$' in its internal identifiers, so we also allow this.
  static bool IsIdChar(char c) {
    return c == '_' || std::isalnum(c) || c == '$';
  }

  /// Returns true if the given character is used to separate arguments
  /// in the command line of lldb.
  static bool IsTokenSeparator(char c) { return c == ' ' || c == '\t'; }

  /// Drops all tokens in front of the expression that are unrelated for
  /// the completion of the cmd line. 'unrelated' means here that the token
  /// is not interested for the lldb completion API result.
  StringRef dropUnrelatedFrontTokens(StringRef cmd) {
    if (cmd.empty())
      return cmd;

    // If we are at the start of a word, then all tokens are unrelated to
    // the current completion logic.
    if (IsTokenSeparator(cmd.back()))
      return StringRef();

    // Remove all previous tokens from the string as they are unrelated
    // to completing the current token.
    StringRef to_remove = cmd;
    while (!to_remove.empty() && !IsTokenSeparator(to_remove.back())) {
      to_remove = to_remove.drop_back();
    }
    cmd = cmd.drop_front(to_remove.size());

    return cmd;
  }

  /// Removes the last identifier token from the given cmd line.
  StringRef removeLastToken(StringRef cmd) {
    while (!cmd.empty() && IsIdChar(cmd.back())) {
      cmd = cmd.drop_back();
    }
    return cmd;
  }

  /// Attemps to merge the given completion from the given position into the
  /// existing command. Returns the completion string that can be returned to
  /// the lldb completion API.
  std::string mergeCompletion(StringRef existing, unsigned pos,
                              StringRef completion) {
    StringRef existing_command = existing.substr(0, pos);
    // We rewrite the last token with the completion, so let's drop that
    // token from the command.
    existing_command = removeLastToken(existing_command);
    // We also should remove all previous tokens from the command as they
    // would otherwise be added to the completion that already has the
    // completion.
    existing_command = dropUnrelatedFrontTokens(existing_command);
    return existing_command.str() + completion.str();
  }

public:
  /// Constructs a CodeComplete consumer that can be attached to a Sema.
  /// \param[out] matches
  ///    The list of matches that the lldb completion API expects as a result.
  ///    This may already contain matches, so it's only allowed to append
  ///    to this variable.
  /// \param[out] expr
  ///    The whole expression string that we are currently parsing. This
  ///    string needs to be equal to the input the user typed, and NOT the
  ///    final code that Clang is parsing.
  /// \param[out] position
  ///    The character position of the user cursor in the `expr` parameter.
  ///
  CodeComplete(CompletionRequest &request, clang::LangOptions ops,
               std::string expr, unsigned position)
      : CodeCompleteConsumer(CodeCompleteOptions()),
        m_info(std::make_shared<GlobalCodeCompletionAllocator>()), m_expr(expr),
        m_position(position), m_request(request), m_desc_policy(ops) {

    // Ensure that the printing policy is producing a description that is as
    // short as possible.
    m_desc_policy.SuppressScope = true;
    m_desc_policy.SuppressTagKeyword = true;
    m_desc_policy.FullyQualifiedName = false;
    m_desc_policy.TerseOutput = true;
    m_desc_policy.IncludeNewlines = false;
    m_desc_policy.UseVoidForZeroParams = false;
    m_desc_policy.Bool = true;
  }

  /// Deregisters and destroys this code-completion consumer.
  ~CodeComplete() override {}

  /// \name Code-completion filtering
  /// Check if the result should be filtered out.
  bool isResultFilteredOut(StringRef Filter,
                           CodeCompletionResult Result) override {
    // This code is mostly copied from CodeCompleteConsumer.
    switch (Result.Kind) {
    case CodeCompletionResult::RK_Declaration:
      return !(
          Result.Declaration->getIdentifier() &&
          Result.Declaration->getIdentifier()->getName().startswith(Filter));
    case CodeCompletionResult::RK_Keyword:
      return !StringRef(Result.Keyword).startswith(Filter);
    case CodeCompletionResult::RK_Macro:
      return !Result.Macro->getName().startswith(Filter);
    case CodeCompletionResult::RK_Pattern:
      return !StringRef(Result.Pattern->getAsString()).startswith(Filter);
    }
    // If we trigger this assert or the above switch yields a warning, then
    // CodeCompletionResult has been enhanced with more kinds of completion
    // results. Expand the switch above in this case.
    assert(false && "Unknown completion result type?");
    // If we reach this, then we should just ignore whatever kind of unknown
    // result we got back. We probably can't turn it into any kind of useful
    // completion suggestion with the existing code.
    return true;
  }

  /// \name Code-completion callbacks
  /// Process the finalized code-completion results.
  void ProcessCodeCompleteResults(Sema &SemaRef, CodeCompletionContext Context,
                                  CodeCompletionResult *Results,
                                  unsigned NumResults) override {

    // The Sema put the incomplete token we try to complete in here during
    // lexing, so we need to retrieve it here to know what we are completing.
    StringRef Filter = SemaRef.getPreprocessor().getCodeCompletionFilter();

    // Iterate over all the results. Filter out results we don't want and
    // process the rest.
    for (unsigned I = 0; I != NumResults; ++I) {
      // Filter the results with the information from the Sema.
      if (!Filter.empty() && isResultFilteredOut(Filter, Results[I]))
        continue;

      CodeCompletionResult &R = Results[I];
      std::string ToInsert;
      std::string Description;
      // Handle the different completion kinds that come from the Sema.
      switch (R.Kind) {
      case CodeCompletionResult::RK_Declaration: {
        const NamedDecl *D = R.Declaration;
        ToInsert = R.Declaration->getNameAsString();
        // If we have a function decl that has no arguments we want to
        // complete the empty parantheses for the user. If the function has
        // arguments, we at least complete the opening bracket.
        if (const FunctionDecl *F = dyn_cast<FunctionDecl>(D)) {
          if (F->getNumParams() == 0)
            ToInsert += "()";
          else
            ToInsert += "(";
          raw_string_ostream OS(Description);
          F->print(OS, m_desc_policy, false);
          OS.flush();
        } else if (const VarDecl *V = dyn_cast<VarDecl>(D)) {
          Description = V->getType().getAsString(m_desc_policy);
        } else if (const FieldDecl *F = dyn_cast<FieldDecl>(D)) {
          Description = F->getType().getAsString(m_desc_policy);
        } else if (const NamespaceDecl *N = dyn_cast<NamespaceDecl>(D)) {
          // If we try to complete a namespace, then we can directly append
          // the '::'.
          if (!N->isAnonymousNamespace())
            ToInsert += "::";
        }
        break;
      }
      case CodeCompletionResult::RK_Keyword:
        ToInsert = R.Keyword;
        break;
      case CodeCompletionResult::RK_Macro:
        ToInsert = R.Macro->getName().str();
        break;
      case CodeCompletionResult::RK_Pattern:
        ToInsert = R.Pattern->getTypedText();
        break;
      }
      // At this point all information is in the ToInsert string.

      // We also filter some internal lldb identifiers here. The user
      // shouldn't see these.
      if (StringRef(ToInsert).startswith("$__lldb_"))
        continue;
      if (!ToInsert.empty()) {
        // Merge the suggested Token into the existing command line to comply
        // with the kind of result the lldb API expects.
        std::string CompletionSuggestion =
            mergeCompletion(m_expr, m_position, ToInsert);
        m_request.AddCompletion(CompletionSuggestion, Description);
      }
    }
  }

  /// \param S the semantic-analyzer object for which code-completion is being
  /// done.
  ///
  /// \param CurrentArg the index of the current argument.
  ///
  /// \param Candidates an array of overload candidates.
  ///
  /// \param NumCandidates the number of overload candidates
  void ProcessOverloadCandidates(Sema &S, unsigned CurrentArg,
                                 OverloadCandidate *Candidates,
                                 unsigned NumCandidates,
                                 SourceLocation OpenParLoc) override {
    // At the moment we don't filter out any overloaded candidates.
  }

  CodeCompletionAllocator &getAllocator() override {
    return m_info.getAllocator();
  }

  CodeCompletionTUInfo &getCodeCompletionTUInfo() override { return m_info; }
};
} // namespace

bool ClangExpressionParser::Complete(CompletionRequest &request, unsigned line,
                                     unsigned pos, unsigned typed_pos) {
  DiagnosticManager mgr;
  // We need the raw user expression here because that's what the CodeComplete
  // class uses to provide completion suggestions.
  // However, the `Text` method only gives us the transformed expression here.
  // To actually get the raw user input here, we have to cast our expression to
  // the LLVMUserExpression which exposes the right API. This should never fail
  // as we always have a ClangUserExpression whenever we call this.
  ClangUserExpression *llvm_expr = cast<ClangUserExpression>(&m_expr);
  CodeComplete CC(request, m_compiler->getLangOpts(), llvm_expr->GetUserText(),
                  typed_pos);
  // We don't need a code generator for parsing.
  m_code_generator.reset();
  // Start parsing the expression with our custom code completion consumer.
  ParseInternal(mgr, &CC, line, pos);
  return true;
}

unsigned ClangExpressionParser::Parse(DiagnosticManager &diagnostic_manager) {
  return ParseInternal(diagnostic_manager);
}

unsigned
ClangExpressionParser::ParseInternal(DiagnosticManager &diagnostic_manager,
                                     CodeCompleteConsumer *completion_consumer,
                                     unsigned completion_line,
                                     unsigned completion_column) {
  ClangDiagnosticManagerAdapter *adapter =
      static_cast<ClangDiagnosticManagerAdapter *>(
          m_compiler->getDiagnostics().getClient());
  auto diag_buf = adapter->GetPassthrough();

  adapter->ResetManager(&diagnostic_manager);

  const char *expr_text = m_expr.Text();

  clang::SourceManager &source_mgr = m_compiler->getSourceManager();
  bool created_main_file = false;

  // Clang wants to do completion on a real file known by Clang's file manager,
  // so we have to create one to make this work.
  // TODO: We probably could also simulate to Clang's file manager that there
  // is a real file that contains our code.
  bool should_create_file = completion_consumer != nullptr;

  // We also want a real file on disk if we generate full debug info.
  should_create_file |= m_compiler->getCodeGenOpts().getDebugInfo() ==
                        codegenoptions::FullDebugInfo;

  if (should_create_file) {
    int temp_fd = -1;
    llvm::SmallString<128> result_path;
    if (FileSpec tmpdir_file_spec = HostInfo::GetProcessTempDir()) {
      tmpdir_file_spec.AppendPathComponent("lldb-%%%%%%.expr");
      std::string temp_source_path = tmpdir_file_spec.GetPath();
      llvm::sys::fs::createUniqueFile(temp_source_path, temp_fd, result_path);
    } else {
      llvm::sys::fs::createTemporaryFile("lldb", "expr", temp_fd, result_path);
    }

    if (temp_fd != -1) {
      lldb_private::NativeFile file(temp_fd, File::eOpenOptionWrite, true);
      const size_t expr_text_len = strlen(expr_text);
      size_t bytes_written = expr_text_len;
      if (file.Write(expr_text, bytes_written).Success()) {
        if (bytes_written == expr_text_len) {
          file.Close();
          if (auto fileEntry =
                  m_compiler->getFileManager().getFile(result_path)) {
            source_mgr.setMainFileID(source_mgr.createFileID(
                *fileEntry,
                SourceLocation(), SrcMgr::C_User));
            created_main_file = true;
          }
        }
      }
    }
  }

  if (!created_main_file) {
    std::unique_ptr<MemoryBuffer> memory_buffer =
        MemoryBuffer::getMemBufferCopy(expr_text, m_filename);
    source_mgr.setMainFileID(source_mgr.createFileID(std::move(memory_buffer)));
  }

  diag_buf->BeginSourceFile(m_compiler->getLangOpts(),
                            &m_compiler->getPreprocessor());

  ClangExpressionHelper *type_system_helper =
      dyn_cast<ClangExpressionHelper>(m_expr.GetTypeSystemHelper());

  // If we want to parse for code completion, we need to attach our code
  // completion consumer to the Sema and specify a completion position.
  // While parsing the Sema will call this consumer with the provided
  // completion suggestions.
  if (completion_consumer) {
    auto main_file = source_mgr.getFileEntryForID(source_mgr.getMainFileID());
    auto &PP = m_compiler->getPreprocessor();
    // Lines and columns start at 1 in Clang, but code completion positions are
    // indexed from 0, so we need to add 1 to the line and column here.
    ++completion_line;
    ++completion_column;
    PP.SetCodeCompletionPoint(main_file, completion_line, completion_column);
  }

  ASTConsumer *ast_transformer =
      type_system_helper->ASTTransformer(m_code_generator.get());

  std::unique_ptr<clang::ASTConsumer> Consumer;
  if (ast_transformer) {
    Consumer.reset(new ASTConsumerForwarder(ast_transformer));
  } else if (m_code_generator) {
    Consumer.reset(new ASTConsumerForwarder(m_code_generator.get()));
  } else {
    Consumer.reset(new ASTConsumer());
  }

  clang::ASTContext &ast_context = m_compiler->getASTContext();

  m_compiler->setSema(new Sema(m_compiler->getPreprocessor(), ast_context,
                               *Consumer, TU_Complete, completion_consumer));
  m_compiler->setASTConsumer(std::move(Consumer));

  if (ast_context.getLangOpts().Modules) {
    m_compiler->createModuleManager();
    m_ast_context->setSema(&m_compiler->getSema());
  }

  ClangExpressionDeclMap *decl_map = type_system_helper->DeclMap();
  if (decl_map) {
    decl_map->InstallCodeGenerator(&m_compiler->getASTConsumer());

    clang::ExternalASTSource *ast_source = decl_map->CreateProxy();

    if (ast_context.getExternalSource()) {
      auto module_wrapper =
          new ExternalASTSourceWrapper(ast_context.getExternalSource());

      auto ast_source_wrapper = new ExternalASTSourceWrapper(ast_source);

      auto multiplexer =
          new SemaSourceWithPriorities(*module_wrapper, *ast_source_wrapper);
      IntrusiveRefCntPtr<ExternalASTSource> Source(multiplexer);
      ast_context.setExternalSource(Source);
    } else {
      ast_context.setExternalSource(ast_source);
    }
    decl_map->InstallASTContext(ast_context, m_compiler->getFileManager());
  }

  // Check that the ASTReader is properly attached to ASTContext and Sema.
  if (ast_context.getLangOpts().Modules) {
    assert(m_compiler->getASTContext().getExternalSource() &&
           "ASTContext doesn't know about the ASTReader?");
    assert(m_compiler->getSema().getExternalSource() &&
           "Sema doesn't know about the ASTReader?");
  }

  {
    llvm::CrashRecoveryContextCleanupRegistrar<Sema> CleanupSema(
        &m_compiler->getSema());
    ParseAST(m_compiler->getSema(), false, false);
  }

  // Make sure we have no pointer to the Sema we are about to destroy.
  if (ast_context.getLangOpts().Modules)
    m_ast_context->setSema(nullptr);
  // Destroy the Sema. This is necessary because we want to emulate the
  // original behavior of ParseAST (which also destroys the Sema after parsing).
  m_compiler->setSema(nullptr);

  diag_buf->EndSourceFile();

  unsigned num_errors = diag_buf->getNumErrors();

  if (m_pp_callbacks && m_pp_callbacks->hasErrors()) {
    num_errors++;
    diagnostic_manager.PutString(eDiagnosticSeverityError,
                                 "while importing modules:");
    diagnostic_manager.AppendMessageToDiagnostic(
        m_pp_callbacks->getErrorString());
  }

  if (!num_errors) {
    if (type_system_helper->DeclMap() &&
        !type_system_helper->DeclMap()->ResolveUnknownTypes()) {
      diagnostic_manager.Printf(eDiagnosticSeverityError,
                                "Couldn't infer the type of a variable");
      num_errors++;
    }
  }

  if (!num_errors) {
    type_system_helper->CommitPersistentDecls();
  }

  adapter->ResetManager();

  return num_errors;
}

std::string
ClangExpressionParser::GetClangTargetABI(const ArchSpec &target_arch) {
  std::string abi;

  if (target_arch.IsMIPS()) {
    switch (target_arch.GetFlags() & ArchSpec::eMIPSABI_mask) {
    case ArchSpec::eMIPSABI_N64:
      abi = "n64";
      break;
    case ArchSpec::eMIPSABI_N32:
      abi = "n32";
      break;
    case ArchSpec::eMIPSABI_O32:
      abi = "o32";
      break;
    default:
      break;
    }
  }
  return abi;
}

bool ClangExpressionParser::RewriteExpression(
    DiagnosticManager &diagnostic_manager) {
  clang::SourceManager &source_manager = m_compiler->getSourceManager();
  clang::edit::EditedSource editor(source_manager, m_compiler->getLangOpts(),
                                   nullptr);
  clang::edit::Commit commit(editor);
  clang::Rewriter rewriter(source_manager, m_compiler->getLangOpts());

  class RewritesReceiver : public edit::EditsReceiver {
    Rewriter &rewrite;

  public:
    RewritesReceiver(Rewriter &in_rewrite) : rewrite(in_rewrite) {}

    void insert(SourceLocation loc, StringRef text) override {
      rewrite.InsertText(loc, text);
    }
    void replace(CharSourceRange range, StringRef text) override {
      rewrite.ReplaceText(range.getBegin(), rewrite.getRangeSize(range), text);
    }
  };

  RewritesReceiver rewrites_receiver(rewriter);

  const DiagnosticList &diagnostics = diagnostic_manager.Diagnostics();
  size_t num_diags = diagnostics.size();
  if (num_diags == 0)
    return false;

  for (const Diagnostic *diag : diagnostic_manager.Diagnostics()) {
    const ClangDiagnostic *diagnostic = llvm::dyn_cast<ClangDiagnostic>(diag);
    if (diagnostic && diagnostic->HasFixIts()) {
      for (const FixItHint &fixit : diagnostic->FixIts()) {
        // This is cobbed from clang::Rewrite::FixItRewriter.
        if (fixit.CodeToInsert.empty()) {
          if (fixit.InsertFromRange.isValid()) {
            commit.insertFromRange(fixit.RemoveRange.getBegin(),
                                   fixit.InsertFromRange, /*afterToken=*/false,
                                   fixit.BeforePreviousInsertions);
          } else
            commit.remove(fixit.RemoveRange);
        } else {
          if (fixit.RemoveRange.isTokenRange() ||
              fixit.RemoveRange.getBegin() != fixit.RemoveRange.getEnd())
            commit.replace(fixit.RemoveRange, fixit.CodeToInsert);
          else
            commit.insert(fixit.RemoveRange.getBegin(), fixit.CodeToInsert,
                          /*afterToken=*/false, fixit.BeforePreviousInsertions);
        }
      }
    }
  }

  // FIXME - do we want to try to propagate specific errors here?
  if (!commit.isCommitable())
    return false;
  else if (!editor.commit(commit))
    return false;

  // Now play all the edits, and stash the result in the diagnostic manager.
  editor.applyRewrites(rewrites_receiver);
  RewriteBuffer &main_file_buffer =
      rewriter.getEditBuffer(source_manager.getMainFileID());

  std::string fixed_expression;
  llvm::raw_string_ostream out_stream(fixed_expression);

  main_file_buffer.write(out_stream);
  out_stream.flush();
  diagnostic_manager.SetFixedExpression(fixed_expression);

  return true;
}

static bool FindFunctionInModule(ConstString &mangled_name,
                                 llvm::Module *module, const char *orig_name) {
  for (const auto &func : module->getFunctionList()) {
    const StringRef &name = func.getName();
    if (name.find(orig_name) != StringRef::npos) {
      mangled_name.SetString(name);
      return true;
    }
  }

  return false;
}

lldb_private::Status ClangExpressionParser::PrepareForExecution(
    lldb::addr_t &func_addr, lldb::addr_t &func_end,
    lldb::IRExecutionUnitSP &execution_unit_sp, ExecutionContext &exe_ctx,
    bool &can_interpret, ExecutionPolicy execution_policy) {
  func_addr = LLDB_INVALID_ADDRESS;
  func_end = LLDB_INVALID_ADDRESS;
  Log *log(lldb_private::GetLogIfAllCategoriesSet(LIBLLDB_LOG_EXPRESSIONS));

  lldb_private::Status err;

  std::unique_ptr<llvm::Module> llvm_module_up(
      m_code_generator->ReleaseModule());

  if (!llvm_module_up) {
    err.SetErrorToGenericError();
    err.SetErrorString("IR doesn't contain a module");
    return err;
  }

  ConstString function_name;

  if (execution_policy != eExecutionPolicyTopLevel) {
    // Find the actual name of the function (it's often mangled somehow)

    if (!FindFunctionInModule(function_name, llvm_module_up.get(),
                              m_expr.FunctionName())) {
      err.SetErrorToGenericError();
      err.SetErrorStringWithFormat("Couldn't find %s() in the module",
                                   m_expr.FunctionName());
      return err;
    } else {
      LLDB_LOGF(log, "Found function %s for %s", function_name.AsCString(),
                m_expr.FunctionName());
    }
  }

  SymbolContext sc;

  if (lldb::StackFrameSP frame_sp = exe_ctx.GetFrameSP()) {
    sc = frame_sp->GetSymbolContext(lldb::eSymbolContextEverything);
  } else if (lldb::TargetSP target_sp = exe_ctx.GetTargetSP()) {
    sc.target_sp = target_sp;
  }

  LLVMUserExpression::IRPasses custom_passes;
  {
    auto lang = m_expr.Language();
    LLDB_LOGF(log, "%s - Current expression language is %s\n", __FUNCTION__,
              Language::GetNameForLanguageType(lang));
    lldb::ProcessSP process_sp = exe_ctx.GetProcessSP();
    if (process_sp && lang != lldb::eLanguageTypeUnknown) {
      auto runtime = process_sp->GetLanguageRuntime(lang);
      if (runtime)
        runtime->GetIRPasses(custom_passes);
    }
  }

  if (custom_passes.EarlyPasses) {
    LLDB_LOGF(log,
              "%s - Running Early IR Passes from LanguageRuntime on "
              "expression module '%s'",
              __FUNCTION__, m_expr.FunctionName());

    custom_passes.EarlyPasses->run(*llvm_module_up);
  }

  execution_unit_sp = std::make_shared<IRExecutionUnit>(
      m_llvm_context, // handed off here
      llvm_module_up, // handed off here
      function_name, exe_ctx.GetTargetSP(), sc,
      m_compiler->getTargetOpts().Features);

  ClangExpressionHelper *type_system_helper =
      dyn_cast<ClangExpressionHelper>(m_expr.GetTypeSystemHelper());
  ClangExpressionDeclMap *decl_map =
      type_system_helper->DeclMap(); // result can be NULL

  if (decl_map) {
    Target *target = exe_ctx.GetTargetPtr();
    auto &error_stream = target->GetDebugger().GetErrorStream();
    IRForTarget ir_for_target(decl_map, m_expr.NeedsVariableResolution(),
                              *execution_unit_sp, error_stream,
                              function_name.AsCString());

    bool ir_can_run =
        ir_for_target.runOnModule(*execution_unit_sp->GetModule());

    if (!ir_can_run) {
      err.SetErrorString(
          "The expression could not be prepared to run in the target");
      return err;
    }

    Process *process = exe_ctx.GetProcessPtr();

    if (execution_policy != eExecutionPolicyAlways &&
        execution_policy != eExecutionPolicyTopLevel) {
      lldb_private::Status interpret_error;

      bool interpret_function_calls =
          !process ? false : process->CanInterpretFunctionCalls();
      can_interpret = IRInterpreter::CanInterpret(
          *execution_unit_sp->GetModule(), *execution_unit_sp->GetFunction(),
          interpret_error, interpret_function_calls);

      if (!can_interpret && execution_policy == eExecutionPolicyNever) {
        err.SetErrorStringWithFormat("Can't run the expression locally: %s",
                                     interpret_error.AsCString());
        return err;
      }
    }

    if (!process && execution_policy == eExecutionPolicyAlways) {
      err.SetErrorString("Expression needed to run in the target, but the "
                         "target can't be run");
      return err;
    }

    if (!process && execution_policy == eExecutionPolicyTopLevel) {
      err.SetErrorString("Top-level code needs to be inserted into a runnable "
                         "target, but the target can't be run");
      return err;
    }

    if (execution_policy == eExecutionPolicyAlways ||
        (execution_policy != eExecutionPolicyTopLevel && !can_interpret)) {
      if (m_expr.NeedsValidation() && process) {
        if (!process->GetDynamicCheckers()) {
          ClangDynamicCheckerFunctions *dynamic_checkers =
              new ClangDynamicCheckerFunctions();

          DiagnosticManager install_diagnostics;

          if (!dynamic_checkers->Install(install_diagnostics, exe_ctx)) {
            if (install_diagnostics.Diagnostics().size())
              err.SetErrorString(install_diagnostics.GetString().c_str());
            else
              err.SetErrorString("couldn't install checkers, unknown error");

            return err;
          }

          process->SetDynamicCheckers(dynamic_checkers);

          LLDB_LOGF(log, "== [ClangExpressionParser::PrepareForExecution] "
                         "Finished installing dynamic checkers ==");
        }

        if (auto *checker_funcs = llvm::dyn_cast<ClangDynamicCheckerFunctions>(
                process->GetDynamicCheckers())) {
          IRDynamicChecks ir_dynamic_checks(*checker_funcs,
                                            function_name.AsCString());

          llvm::Module *module = execution_unit_sp->GetModule();
          if (!module || !ir_dynamic_checks.runOnModule(*module)) {
            err.SetErrorToGenericError();
            err.SetErrorString("Couldn't add dynamic checks to the expression");
            return err;
          }

          if (custom_passes.LatePasses) {
            LLDB_LOGF(log,
                      "%s - Running Late IR Passes from LanguageRuntime on "
                      "expression module '%s'",
                      __FUNCTION__, m_expr.FunctionName());

            custom_passes.LatePasses->run(*module);
          }
        }
      }
    }

    if (execution_policy == eExecutionPolicyAlways ||
        execution_policy == eExecutionPolicyTopLevel || !can_interpret) {
      execution_unit_sp->GetRunnableInfo(err, func_addr, func_end);
    }
  } else {
    execution_unit_sp->GetRunnableInfo(err, func_addr, func_end);
  }

  return err;
}

lldb_private::Status ClangExpressionParser::RunStaticInitializers(
    lldb::IRExecutionUnitSP &execution_unit_sp, ExecutionContext &exe_ctx) {
  lldb_private::Status err;

  lldbassert(execution_unit_sp.get());
  lldbassert(exe_ctx.HasThreadScope());

  if (!execution_unit_sp.get()) {
    err.SetErrorString(
        "can't run static initializers for a NULL execution unit");
    return err;
  }

  if (!exe_ctx.HasThreadScope()) {
    err.SetErrorString("can't run static initializers without a thread");
    return err;
  }

  std::vector<lldb::addr_t> static_initializers;

  execution_unit_sp->GetStaticInitializers(static_initializers);

  for (lldb::addr_t static_initializer : static_initializers) {
    EvaluateExpressionOptions options;

    lldb::ThreadPlanSP call_static_initializer(new ThreadPlanCallFunction(
        exe_ctx.GetThreadRef(), Address(static_initializer), CompilerType(),
        llvm::ArrayRef<lldb::addr_t>(), options));

    DiagnosticManager execution_errors;
    lldb::ExpressionResults results =
        exe_ctx.GetThreadRef().GetProcess()->RunThreadPlan(
            exe_ctx, call_static_initializer, options, execution_errors);

    if (results != lldb::eExpressionCompleted) {
      err.SetErrorStringWithFormat("couldn't run static initializer: %s",
                                   execution_errors.GetString().c_str());
      return err;
    }
  }

  return err;
}
