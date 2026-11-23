/*
 * Stegseek 0.6 - a steghide cracker
 * Copyright (C) 2021 Rick de Jager
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
 */

#include "BatchCracker.h"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <list>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "Arguments.h"
#include "SteghideError.h"
#include "common.h"
#include "error.h"
#include "msg.h"

namespace {

const char *imageExtensions[] = {".jpg", ".jpeg", ".bmp", ".wav", ".au"};

bool isImageFile(const std::string &path) {
    const size_t dot = path.find_last_of('.');
    const size_t slash = path.find_last_of('/');
    if (dot == std::string::npos || (slash != std::string::npos && dot < slash)) {
        return false;
    }

    std::string extension = path.substr(dot);
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    for (const char *candidate : imageExtensions) {
        if (extension == candidate) {
            return true;
        }
    }
    return false;
}

bool isRegularFile(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool isDirectory(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

std::string canonicalPath(const std::string &path) {
    char resolved[PATH_MAX];
    if (realpath(path.c_str(), resolved) != nullptr) {
        return std::string(resolved);
    }
    return path;
}

std::string baseName(const std::string &path) {
    const size_t slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

} // namespace

BatchCracker::BatchCracker() : jobs(1), threads(1), found(0), notFound(0), errors(0) {
    jobs = std::max(1, Args.Jobs.getValue());
    threads = Args.Threads.is_set() ? std::max(1, Args.Threads.getValue()) : 1;

    resolveInputs();

    if (!isRegularFile(wordlist)) {
        throw SteghideError(_("could not open the wordlist \"%s\"."), wordlist.c_str());
    }

    for (const std::string &input : Args.BatchInputs.getValue()) {
        collectFiles(input, true);
    }

    if (files.empty()) {
        throw SteghideError(_("no supported image files were found."));
    }

    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());
    jobs = std::min<unsigned int>(jobs, files.size());

    if (Args.ExtFn.is_set() && !Args.ExtFn.getValue().empty() && files.size() > 1 &&
        !isDirectory(Args.ExtFn.getValue())) {
        throw ArgError(_("in batch mode, \"%s\" must be a directory when extracting "
                         "from multiple files."),
                       Args.ExtFn.getValue().c_str());
    }
}

void BatchCracker::resolveInputs() {
    std::vector<std::string> raw;
    for (const std::string &input : Args.BatchInputs.getValue()) {
        raw.push_back(input);
    }

    if (raw.empty()) {
        throw SteghideError(_("batch mode requires at least one image file or directory."));
    }

    wordlist = Args.WordlistFn.getValue();

    if (!Args.WordlistFn.is_set() && raw.size() >= 2) {
        const std::string &first = raw.front();
        const std::string &last = raw.back();

        // Accept both "--batch <images> <wordlist>" (the normal stegseek order)
        // and "--batch <wordlist> <images>".
        if (isRegularFile(last) && !isImageFile(last)) {
            wordlist = last;
            raw.pop_back();
        } else if (isRegularFile(first) && !isImageFile(first) &&
                   (isImageFile(last) || isDirectory(last))) {
            wordlist = first;
            raw.erase(raw.begin());
        }
    }

    std::list<std::string> inputs;
    for (const std::string &input : raw) {
        inputs.push_back(input);
    }
    Args.BatchInputs.setValue(inputs);
}

void BatchCracker::collectFiles(const std::string &path, bool explicitFile) {
    if (path.empty()) {
        throw SteghideError(_("empty image path in batch mode."));
    }

    if (isDirectory(path)) {
        collectDirectory(canonicalPath(path));
        return;
    }

    if (isRegularFile(path) && (explicitFile || isImageFile(path))) {
        files.push_back(canonicalPath(path));
        return;
    }

    throw SteghideError(_("could not read batch input \"%s\"."), path.c_str());
}

void BatchCracker::collectDirectory(const std::string &path) {
    const std::string canonical = canonicalPath(path);
    if (!visitedDirectories.insert(canonical).second) {
        return;
    }

    DIR *directory = opendir(canonical.c_str());
    if (directory == nullptr) {
        throw SteghideError(_("could not open batch directory \"%s\"."), canonical.c_str());
    }

    std::vector<std::string> entries;
    while (struct dirent *entry = readdir(directory)) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        entries.push_back(canonical + "/" + entry->d_name);
    }
    closedir(directory);

    std::sort(entries.begin(), entries.end());
    for (const std::string &entry : entries) {
        if (isDirectory(entry)) {
            collectDirectory(entry);
        } else if (isRegularFile(entry) && isImageFile(entry)) {
            files.push_back(canonicalPath(entry));
        }
    }
}

std::string BatchCracker::outputFor(const std::string &file) const {
    if (!Args.ExtFn.is_set()) {
        return "";
    }

    const std::string requested = Args.ExtFn.getValue();
    if (requested.empty() || files.size() == 1) {
        return requested;
    }

    return canonicalPath(requested) + "/" + baseName(file) + ".out";
}

std::string BatchCracker::executablePath() const {
    char path[PATH_MAX];
    const ssize_t length = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (length > 0) {
        path[length] = '\0';
        return std::string(path);
    }

#ifdef __APPLE__
    uint32_t size = sizeof(path);
    if (_NSGetExecutablePath(path, &size) == 0) {
        return canonicalPath(path);
    }
#endif

    return "stegseek";
}

std::vector<std::string> BatchCracker::commandFor(const std::string &file) const {
    std::vector<std::string> command{
        executablePath(), "--crack", "-sf", file, "-wl", wordlist, "-t", std::to_string(threads),
    };

    if (Args.Force.getValue()) {
        command.push_back("-f");
    }
    if (Args.SkipDefaultGuesses.getValue()) {
        command.push_back("-s");
    }
    if (!Args.Color.getValue()) {
        command.push_back("-n");
    }
    if (Args.Accessible.getValue()) {
        command.push_back("-a");
    }
    if (Args.ContinueAfterFirstResult.getValue()) {
        command.push_back("-c");
    }
    if (Args.Verbosity.getValue() == QUIET) {
        command.push_back("-q");
    } else if (Args.Verbosity.getValue() == VERBOSE) {
        command.push_back("-v");
    }
    if (Args.ExtFn.is_set()) {
        command.push_back("-xf");
        const std::string output = outputFor(file);
        command.push_back(output);
    }

    return command;
}

BatchCracker::RunningJob BatchCracker::startImage(size_t fileIndex) {
    const std::string &file = files[fileIndex];
    FILE *capturedOut = tmpfile();
    FILE *capturedErr = tmpfile();
    if (capturedOut == nullptr || capturedErr == nullptr) {
        if (capturedOut != nullptr) {
            fclose(capturedOut);
        }
        if (capturedErr != nullptr) {
            fclose(capturedErr);
        }
        throw SteghideError(_("could not create temporary output for batch mode."));
    }

    const int outFd = fileno(capturedOut);
    const int errFd = fileno(capturedErr);
    fcntl(outFd, F_SETFD, FD_CLOEXEC);
    fcntl(errFd, F_SETFD, FD_CLOEXEC);

    std::vector<std::string> command = commandFor(file);
    std::vector<char *> argv;
    for (std::string &argument : command) {
        argv.push_back(const_cast<char *>(argument.c_str()));
    }
    argv.push_back(nullptr);

    fflush(nullptr);
    const pid_t child = fork();
    if (child == 0) {
        if (dup2(outFd, STDOUT_FILENO) < 0 || dup2(errFd, STDERR_FILENO) < 0) {
            _exit(127);
        }
        const int nullFd = open("/dev/null", O_RDONLY);
        if (nullFd >= 0) {
            dup2(nullFd, STDIN_FILENO);
            close(nullFd);
        }
        close(outFd);
        close(errFd);

        if (command.front() == "stegseek") {
            execvp(command.front().c_str(), argv.data());
        } else {
            execv(command.front().c_str(), argv.data());
        }
        dprintf(STDERR_FILENO, "could not start stegseek worker: %s\n", strerror(errno));
        _exit(127);
    }

    if (child < 0) {
        fclose(capturedOut);
        fclose(capturedErr);
        throw SteghideError(_("could not start a batch worker: %s."), strerror(errno));
    }

    return RunningJob{fileIndex, child, capturedOut, capturedErr};
}

BatchCracker::Result BatchCracker::finishImage(RunningJob &job, int status) {
    rewind(job.capturedOut);
    rewind(job.capturedErr);

    Message::print("Batch file: \"%s\"\n", files[job.fileIndex].c_str());

    char buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), job.capturedErr)) > 0) {
        fwrite(buffer, 1, count, stderr);
    }
    while ((count = fread(buffer, 1, sizeof(buffer), job.capturedOut)) > 0) {
        fwrite(buffer, 1, count, stdout);
    }
    fflush(stdout);
    fflush(stderr);

    fclose(job.capturedOut);
    fclose(job.capturedErr);

    const bool exitedCleanly = WIFEXITED(status) && WEXITSTATUS(status) == 0;
    return Result{exitedCleanly, status};
}

void BatchCracker::crack() {
    Message::print("Batch mode: %zu files, %u jobs, %u threads per job.\n", files.size(), jobs,
                   threads);

    size_t next = 0;
    std::vector<RunningJob> active;

    // The worker output is captured to temporary files, so we can run up to
    // "jobs" children without interleaving partial lines or binary output.
    while (next < files.size() || !active.empty()) {
        while (next < files.size() && active.size() < jobs) {
            active.push_back(startImage(next++));
        }

        int status = 0;
        pid_t child = 0;
        do {
            child = waitpid(-1, &status, 0);
        } while (child < 0 && errno == EINTR);
        if (child < 0) {
            throw SteghideError(_("could not wait for a batch worker: %s."), strerror(errno));
        }

        auto completed = std::find_if(active.begin(), active.end(),
                                      [child](const RunningJob &job) { return job.pid == child; });
        myassert(completed != active.end());

        RunningJob job = *completed;
        active.erase(completed);
        const Result result = finishImage(job, status);
        if (result.found) {
            found++;
        } else if (WIFEXITED(result.status) && WEXITSTATUS(result.status) == 1) {
            notFound++;
        } else {
            errors++;
        }
    }

    Message::print("Batch complete: %zu files, %lu found, %lu not found, %lu errors.\n",
                   files.size(), found, notFound, errors);

    if (found == 0) {
        throw SteghideError(_("could not find a valid passphrase in any batch input."));
    }
}
