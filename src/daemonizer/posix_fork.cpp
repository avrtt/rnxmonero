// Copyright (c) 2003-2011 Christopher M. Kohlhoff (chris at kohlhoff dot com)
//
// Distributed under the Boost Software License, Version 1.0. (See accompanying
// file LICENSE_1_0.txt or copy at http://www.boost.org/LICENSE_1_0.txt)

#include "daemonizer/posix_fork.h"
#include "misc_log_ex.h"

#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>
#include <stdexcept>
#include <fstream>
#include <string>

#ifndef TMPDIR
#define TMPDIR "/tmp"
#endif

namespace posix {

namespace {
  void quit(const std::string &message) {
    LOG_ERROR(message);
    throw std::runtime_error(message);
  }

  // Helper function to check and handle existing PID file.
  void handle_existing_pid(const std::string &pidfile) {
    if (pidfile.empty()) {
      return;
    }

    int oldpid;
    std::ifstream pidrifs(pidfile);

    if (pidrifs.is_open()) {
      // Check if the process with the existing PID is still running.
      if (pidrifs >> oldpid && oldpid > 1 && kill(oldpid, 0) == 0) {
        quit("PID file " + pidfile + " already exists and the PID is valid");
      }
    }
  }

  // Helper function to write the new PID to the file.
  void write_pid_to_file(const std::string &pidfile) {
    if (pidfile.empty()) {
      return;
    }

    std::ofstream pidofs(pidfile, std::fstream::out | std::fstream::trunc);
    if (!pidofs.is_open()) {
      quit("Failed to open specified PID file for writing");
    }

    pidofs << ::getpid() << std::endl;
  }

  // Helper function to safely fork the process.
  void safe_fork(const std::string &error_message) {
    if (pid_t pid = ::fork()) {
      if (pid > 0) {
        // Parent process exits.
        exit(0);
      } else {
        quit(error_message);
      }
    }
  }

  // Helper function to close standard streams and redirect them to /dev/null.
  void redirect_streams() {
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);

    if (open("/dev/null", O_RDONLY) < 0) {
      quit("Unable to open /dev/null for reading");
    }

#ifdef DEBUG_TMPDIR_LOG
    const char *tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = TMPDIR;
    
    std::string output = std::string(tmpdir) + "/bitmonero.daemon.stdout.stderr";
    const int flags = O_WRONLY | O_CREAT | O_APPEND;
    const mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
    
    if (open(output.c_str(), flags, mode) < 0) {
      quit("Unable to open output file: " + output);
    }
#else
    if (open("/dev/null", O_WRONLY) < 0) {
      quit("Unable to open /dev/null for writing");
    }
#endif

    // Redirect stderr to stdout.
    if (dup(STDOUT_FILENO) < 0) {
      quit("Unable to dup output descriptor");
    }
  }
}

void fork(const std::string &pidfile) {
  // Handle existing PID file.
  handle_existing_pid(pidfile);

  // Perform first fork, exit parent process.
  safe_fork("First fork failed");

  // Make the process a new session leader.
  setsid();

  // Perform second fork, exit parent process again.
  safe_fork("Second fork failed");

  // Write the new PID to the file if necessary.
  write_pid_to_file(pidfile);

  // Redirect input/output streams.
  redirect_streams();
}

} // namespace posix
