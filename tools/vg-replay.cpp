#include "capture/capture.h"
#include <fstream>
#include <iostream>
#include <sstream>
int main(int argc, char** argv) {
  if (argc != 2) { std::cerr << "usage: vg-replay capture.json\n"; return 2; }
  std::ifstream input(argv[1]); std::stringstream text; text << input.rdbuf(); vg::capture::Capture capture; std::string error;
  if (!vg::capture::deserialize(text.str(), &capture, &error)) { std::cerr << error << "\n"; return 1; }
  vg::capture::ReplayResult replay;
  if (!vg::capture::replay(capture, &replay, &error)) { std::cerr << error << "\n"; return 1; }
  if (!replay.execution.ok) { std::cerr << replay.execution.message << "\n"; return 1; }
  std::cout << capture.module.hash << "\n"; return 0;
}
