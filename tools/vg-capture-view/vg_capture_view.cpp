#include "capture/capture.h"

#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {
void usage() { std::cerr << "usage: vg-capture-view [--format markdown|json] [--output PATH] capture.json\n"; }

bool read_file(const std::string& path, std::string* text) {
  std::ifstream input(path);
  if (!input) return false;
  std::stringstream buffer;
  buffer << input.rdbuf();
  *text = buffer.str();
  return true;
}

bool write_file(const std::string& path, const std::string& text) {
  std::ofstream output(path);
  if (!output) return false;
  output << text;
  return static_cast<bool>(output);
}
}

int main(int argc, char** argv) {
  std::string format = "markdown";
  std::string output_path;
  std::string input_path;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--help" || arg == "-h") {
      usage();
      return 0;
    }
    if (arg == "--format") {
      if (index + 1 >= argc) {
        usage();
        return 2;
      }
      format = argv[++index];
      continue;
    }
    if (arg == "--output" || arg == "-o") {
      if (index + 1 >= argc) {
        usage();
        return 2;
      }
      output_path = argv[++index];
      continue;
    }
    if (!arg.empty() && arg[0] == '-') {
      usage();
      return 2;
    }
    if (!input_path.empty()) {
      usage();
      return 2;
    }
    input_path = arg;
  }
  if (input_path.empty() || (format != "markdown" && format != "json")) {
    usage();
    return 2;
  }

  std::string text;
  if (!read_file(input_path, &text)) {
    std::cerr << "cannot read capture\n";
    return 1;
  }
  vg::capture::Capture capture;
  std::string error;
  if (!vg::capture::deserialize(text, &capture, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  vg::capture::ViewReport report;
  if (!vg::capture::write_view(capture, &report, &error)) {
    std::cerr << error << "\n";
    return 1;
  }
  const std::string& body = format == "json" ? report.json : report.markdown;
  if (!output_path.empty()) {
    if (!write_file(output_path, body)) {
      std::cerr << "cannot write report\n";
      return 1;
    }
    return 0;
  }
  std::cout << body;
  if (!body.empty() && body.back() != '\n') std::cout << '\n';
  return 0;
}
