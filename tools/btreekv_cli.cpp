// Command-line front end for a BTreeKV database file.
//
//   btreekv_cli <db-file> <command> [args...]   run one command
//   btreekv_cli <db-file>                        read commands from stdin
//
// Commands:
//   put <key> <value>      insert or overwrite (in the REPL the value is the
//                          rest of the line and may contain spaces)
//   get <key>              print the value
//   del <key>              delete the key
//   scan [start] [limit]   print key<TAB>value pairs from `start` onward
//   stats                  tree and buffer pool statistics
//   check                  verify structural invariants
//   flush                  write dirty pages and the header to disk
//   help

#include <unistd.h>

#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include "btreekv/btree.h"

namespace {

void PrintUsage(std::ostream& os) {
  os << "usage: btreekv_cli <db-file> [command args...]\n"
        "commands:\n"
        "  put <key> <value>     insert or overwrite\n"
        "  get <key>             print the value for key\n"
        "  del <key>             delete key\n"
        "  scan [start] [limit]  list entries with key >= start\n"
        "  stats                 tree and buffer pool statistics\n"
        "  check                 verify B+tree invariants\n"
        "  flush                 persist dirty pages\n"
        "  help                  this message\n"
        "with no command, commands are read from stdin (quit/exit to leave)\n";
}

// Returns a process exit code: 0 success, 1 key not found, 2 usage error.
int RunCommand(btreekv::BTree& tree, const std::vector<std::string>& args, std::ostream& out) {
  if (args.empty()) {
    return 0;
  }
  const std::string& cmd = args[0];

  if (cmd == "put") {
    if (args.size() != 3) {
      out << "usage: put <key> <value>\n";
      return 2;
    }
    const bool inserted = tree.Put(args[1], args[2]);
    out << (inserted ? "OK (inserted)\n" : "OK (updated)\n");
    return 0;
  }

  if (cmd == "get") {
    if (args.size() != 2) {
      out << "usage: get <key>\n";
      return 2;
    }
    std::string value;
    if (!tree.Get(args[1], &value)) {
      out << "(not found)\n";
      return 1;
    }
    out << value << "\n";
    return 0;
  }

  if (cmd == "del") {
    if (args.size() != 2) {
      out << "usage: del <key>\n";
      return 2;
    }
    if (!tree.Delete(args[1])) {
      out << "(not found)\n";
      return 1;
    }
    out << "OK (deleted)\n";
    return 0;
  }

  if (cmd == "scan") {
    if (args.size() > 3) {
      out << "usage: scan [start] [limit]\n";
      return 2;
    }
    const std::string start = args.size() > 1 ? args[1] : "";
    uint64_t limit = std::numeric_limits<uint64_t>::max();
    if (args.size() > 2) {
      limit = std::stoull(args[2]);
    }
    uint64_t printed = 0;
    for (auto it = tree.Seek(start); it.Valid() && printed < limit; it.Next(), ++printed) {
      out << it.key() << "\t" << it.value() << "\n";
    }
    out << "(" << printed << " entries)\n";
    return 0;
  }

  if (cmd == "stats") {
    const btreekv::BufferPoolStats pool = tree.PoolStats();
    out << "entries:        " << tree.Size() << "\n"
        << "height:         " << tree.Height() << "\n"
        << "pages:          " << tree.PageCount() << " (" << tree.FreeListLength() << " free)\n"
        << "page size:      " << btreekv::kPageSize << " bytes\n"
        << "pool hits:      " << pool.hits << "\n"
        << "pool misses:    " << pool.misses << "\n"
        << "pool evictions: " << pool.evictions << "\n"
        << "pool writes:    " << pool.dirty_writes << "\n";
    return 0;
  }

  if (cmd == "check") {
    tree.CheckInvariants();
    out << "OK\n";
    return 0;
  }

  if (cmd == "flush") {
    tree.Flush();
    out << "OK\n";
    return 0;
  }

  if (cmd == "help") {
    PrintUsage(out);
    return 0;
  }

  out << "unknown command '" << cmd << "' (try help)\n";
  return 2;
}

// Splits a REPL line into a command and its arguments. For `put`, everything
// after the key is the value, so values may contain spaces.
std::vector<std::string> ParseLine(const std::string& line) {
  std::istringstream in(line);
  std::vector<std::string> args;
  std::string word;
  if (!(in >> word)) {
    return args;
  }
  args.push_back(word);

  if (word == "put") {
    std::string key;
    if (in >> key) {
      args.push_back(key);
      std::string rest;
      std::getline(in, rest);
      const size_t begin = rest.find_first_not_of(" \t");
      if (begin != std::string::npos) {
        args.push_back(rest.substr(begin));
      }
    }
    return args;
  }

  while (in >> word) {
    args.push_back(word);
  }
  return args;
}

int Repl(btreekv::BTree& tree) {
  const bool interactive = ::isatty(STDIN_FILENO) != 0;
  std::string line;
  while (true) {
    if (interactive) {
      std::cout << "btreekv> " << std::flush;
    }
    if (!std::getline(std::cin, line)) {
      break;
    }
    const std::vector<std::string> args = ParseLine(line);
    if (args.empty()) {
      continue;
    }
    if (args[0] == "quit" || args[0] == "exit") {
      break;
    }
    try {
      RunCommand(tree, args, std::cout);
    } catch (const std::exception& e) {
      std::cout << "error: " << e.what() << "\n";
    }
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2 || std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help") {
    PrintUsage(argc < 2 ? std::cerr : std::cout);
    return argc < 2 ? 2 : 0;
  }

  try {
    btreekv::BTree tree(argv[1]);
    if (argc == 2) {
      return Repl(tree);
    }
    std::vector<std::string> args(argv + 2, argv + argc);
    return RunCommand(tree, args, std::cout);
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
}
