// Copyright 2021 The Verible Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//

#include "verilog/tools/ls/symbol-table-handler.h"

#include <filesystem>

#include "common/strings/line_column_map.h"
#include "verilog/analysis/verilog_filelist.h"

namespace verilog {

static constexpr absl::string_view fileschemeprefix = "file://";
static const std::string filelistname = "verible.filelist";

absl::string_view LSPUriToPath(absl::string_view uri) {
  if (!absl::StartsWith(uri, fileschemeprefix)) return "";
  return uri.substr(fileschemeprefix.size());
}

std::string PathToLSPUri(absl::string_view path) {
  std::filesystem::path p(path.begin(), path.end());
  return absl::StrCat(fileschemeprefix, std::filesystem::absolute(p).string());
}

void SymbolTableHandler::setProject(
    absl::string_view root, const std::vector<std::string> &include_paths,
    absl::string_view corpus) {
  currproject = std::make_unique<VerilogProject>(root, include_paths, corpus);
  resetSymbolTable();
}

void SymbolTableHandler::resetSymbolTable() {
  checkedfiles.clear();
  symboltable = std::make_unique<SymbolTable>(currproject.get());
}

void SymbolTableHandler::buildSymbolTableFor(VerilogSourceFile &file) {
  auto result = BuildSymbolTable(file, symboltable.get(), currproject.get());
}

void SymbolTableHandler::buildProjectSymbolTable() {
  resetSymbolTable();
  if (!currproject) {
    return;
  }
  LOG(INFO) << "Parsing project files...";
  std::vector<absl::Status> buildstatus;
  symboltable->Build(&buildstatus);
  for (const auto &diagnostic : buildstatus) {
    LOG(WARNING) << diagnostic.message();
  }
  std::vector<absl::Status> resolvestatus;
  symboltable->Resolve(&resolvestatus);
  for (const auto &diagnostic : resolvestatus) {
    LOG(WARNING) << diagnostic.message();
  }
  files_dirty_ = false;
}

void SymbolTableHandler::loadProjectFileList(absl::string_view current_dir) {
  LOG(INFO) << __FUNCTION__;
  if (!currproject) return;
  // search for FileList file up the directory hierarchy
  std::filesystem::path currentdirpath{current_dir.begin(), current_dir.end()};
  std::filesystem::path projectpath = currentdirpath / filelistname;
  FileList filelist;
  while (true) {
    projectpath = currentdirpath / filelistname;
    // file found
    LOG(INFO) << "Checking existence of " << projectpath;
    if (std::filesystem::exists(projectpath)) break;
    // file not found, reached root directory
    if (currentdirpath.parent_path() == currentdirpath) {
      LOG(INFO) << filelistname << " not found";
      return;
    }
    currentdirpath = currentdirpath.parent_path();
  }
  LOG(INFO) << "Found file list under " << projectpath;
  // fill the FileList object
  absl::Status status = AppendFileListFromFile(projectpath.string(), &filelist);
  // if failed to parse
  if (!status.ok()) {
    LOG(WARNING) << "Failed to parse file list in " << projectpath.string();
    return;
  }
  // update include directories in project
  for (auto &incdir : filelist.preprocessing.include_dirs) {
    LOG(INFO) << "Adding include path:  " << incdir;
    currproject->addIncludePath(incdir);
  }
  // add files from file list to the project
  for (auto &incfile : filelist.file_paths) {
    auto incsource = currproject->OpenIncludedFile(incfile);
    if (!incsource.ok()) {
      LOG(WARNING) << "File included in " << projectpath
                   << " not found:  " << incfile;
      continue;
    }
    LOG(INFO) << "Creating symbol table for:  " << incfile;
    buildSymbolTableFor(*incsource.value());
  }
}

const SymbolTableNode *SymbolTableHandler::ScanSymbolTreeForDefinition(
    const SymbolTableNode *context, absl::string_view symbol) {
  if (!context) {
    return nullptr;
  }
  if (context->Key() && *context->Key() == symbol) {
    return context;
  }
  for (const auto &child : context->Children()) {
    auto res = ScanSymbolTreeForDefinition(&child.second, symbol);
    if (res) {
      return res;
    }
  }
  return nullptr;
}

std::vector<verible::lsp::Location> SymbolTableHandler::findDefinition(
    const verible::lsp::DefinitionParams &params,
    const verilog::BufferTrackerContainer &parsed_buffers) {
  if (files_dirty_) {
    buildProjectSymbolTable();
  }
  absl::string_view filepath = LSPUriToPath(params.textDocument.uri);
  if (filepath.empty()) {
    std::cerr << "Could not convert URI " << params.textDocument.uri
              << " to filesystem path." << std::endl;
    return {};
  }
  std::string relativepath = currproject->GetRelativePathToSource(filepath);
  const verilog::ParsedBuffer *parsedbuffer =
      parsed_buffers.FindBufferTrackerOrNull(params.textDocument.uri)
          ->current();
  if (!parsedbuffer) {
    LOG(ERROR) << "Buffer not found among opened buffers:  "
               << params.textDocument.uri;
    return {};
  }
  const verible::LineColumn cursor{params.position.line,
                                   params.position.character};
  const verible::TextStructureView &text = parsedbuffer->parser().Data();

  const verible::TokenInfo cursor_token = text.FindTokenAt(cursor);
  auto symbol = cursor_token.text();
  auto reffile = currproject->LookupRegisteredFile(relativepath);
  if (!reffile) {
    LOG(ERROR) << "Unable to lookup " << params.textDocument.uri;
    return {};
  }

  auto &root = symboltable->Root();

  auto node = ScanSymbolTreeForDefinition(&root, symbol);
  if (!node) {
    LOG(INFO) << "Symbol " << symbol << " not found in symbol table:  " << node;
    return {};
  }
  // TODO add iterating over multiple definitions?
  verible::lsp::Location location;
  const verilog::SymbolInfo &symbolinfo = node->Value();
  if (!symbolinfo.file_origin) {
    LOG(ERROR) << "Origin file not available";
    return {};
  }
  location.uri = PathToLSPUri(symbolinfo.file_origin->ResolvedPath());
  auto *textstructure = symbolinfo.file_origin->GetTextStructure();
  if (!textstructure) {
    LOG(ERROR) << "Origin file's text structure is not parsed";
    return {};
  }
  verible::LineColumnRange symbollocation =
      textstructure->GetRangeForText(*node->Key());
  location.range.start = {.line = symbollocation.start.line,
                          .character = symbollocation.start.column};
  location.range.end = {.line = symbollocation.end.line,
                        .character = symbollocation.end.column};
  return {location};
}

};  // namespace verilog
