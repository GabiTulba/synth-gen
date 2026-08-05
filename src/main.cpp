#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "build.hpp"
#include "checker.hpp"

namespace {

int usage() {
  std::cerr <<
      "synthc - SynthGraph compiler & build tool\n"
      "\n"
      "Usage:\n"
      "  synthc build [PROJECT_DIR]     one-shot build of a project\n"
      "                                 (PROJECT_DIR defaults to '.')\n"
      "  synthc lint FILE...            front-end checks only (parse +\n"
      "                                 type-check), no artifacts\n";
  return 2;
}

std::string readAll(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  std::ostringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

void printDiags(const synth::DiagnosticBag& diags) {
  std::map<std::string, std::string> cache;
  for (auto& d : diags.items) {
    if (!d.file.empty() && !cache.count(d.file)) cache[d.file] = readAll(d.file);
    std::cerr << synth::renderDiagnostic(d, d.file.empty() ? std::string{}
                                                           : cache[d.file]);
  }
}

int cmdBuild(const std::string& dir) {
  synth::BuildResult r = synth::buildProject(dir);
  printDiags(r.diags);
  for (auto& t : r.targets) {
    if (t.ok)
      std::cout << "  rendered '" << t.name << "' -> " << t.artifact << " ("
                << t.durationSeconds << "s, " << t.channelCount << " ch, "
                << t.rate << " Hz)\n";
    else
      std::cout << "  FAILED '" << t.name << "': " << t.error << "\n";
  }
  if (!r.metadataPath.empty())
    std::cout << "  metadata: " << r.metadataPath << "\n";
  std::cout << (r.ok ? "build succeeded" : "build failed") << "\n";
  return r.ok ? 0 : 1;
}

int cmdLint(const std::vector<std::string>& files) {
  synth::DiagnosticBag diags = synth::lintFiles(files);
  printDiags(diags);
  if (diags.hasErrors()) return 1;
  std::cout << "no problems found\n";
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  if (args.empty()) return usage();
  const std::string& cmd = args[0];
  if (cmd == "build") {
    if (args.size() > 2) return usage();
    return cmdBuild(args.size() == 2 ? args[1] : ".");
  }
  if (cmd == "lint") {
    if (args.size() < 2) return usage();
    return cmdLint({args.begin() + 1, args.end()});
  }
  return usage();
}
