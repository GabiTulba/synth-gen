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
      "  synthc build [PROJECT_DIR] [-v]  one-shot build of a project\n"
      "                                   (PROJECT_DIR defaults to '.')\n"
      "  synthc watch [PROJECT_DIR] [-v]  build daemon: watch the project's\n"
      "                                   sources, manifest and audio inputs\n"
      "                                   and rebuild on change\n"
      "  synthc lint FILE...              front-end checks only (parse +\n"
      "                                   type-check), no artifacts\n"
      "\n"
      "  -v, --verbose   stream the build log: phase timings, per-target\n"
      "                  worker threads and durations, dependency stats\n";
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

void logToStderr(const std::string& line) { std::cerr << line << "\n"; }

int cmdBuild(const std::string& dir, bool verbose) {
  synth::BuildOptions options;
  if (verbose) options.log = logToStderr;
  synth::BuildResult r = synth::buildProject(dir, options);
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

int cmdWatch(const std::string& dir, bool verbose) {
  std::cout << "watching '" << dir << "' (Ctrl-C to stop)\n";
  synth::watchProject(
      dir,
      [](const synth::BuildResult& r) {
        printDiags(r.diags);
        for (auto& t : r.targets) {
          if (t.ok)
            std::cout << "  rendered '" << t.name << "' -> " << t.artifact
                      << "\n";
          else
            std::cout << "  FAILED '" << t.name << "': " << t.error << "\n";
        }
        std::cout << (r.ok ? "build succeeded" : "build failed")
                  << "; watching for changes...\n";
      },
      [] { return true; }, 300,
      verbose ? std::function<void(const std::string&)>(logToStderr)
              : std::function<void(const std::string&)>{});
  return 0;
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
  bool verbose = false;
  for (auto it = args.begin(); it != args.end();) {
    if (*it == "-v" || *it == "--verbose") {
      verbose = true;
      it = args.erase(it);
    } else {
      ++it;
    }
  }
  if (args.empty()) return usage();
  const std::string& cmd = args[0];
  if (cmd == "build") {
    if (args.size() > 2) return usage();
    return cmdBuild(args.size() == 2 ? args[1] : ".", verbose);
  }
  if (cmd == "watch") {
    if (args.size() > 2) return usage();
    return cmdWatch(args.size() == 2 ? args[1] : ".", verbose);
  }
  if (cmd == "lint") {
    if (args.size() < 2) return usage();
    return cmdLint({args.begin() + 1, args.end()});
  }
  return usage();
}
