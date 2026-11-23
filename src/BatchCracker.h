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

#ifndef SH_BATCHCRACKER_H
#define SH_BATCHCRACKER_H

#include <cstdio>
#include <set>
#include <string>
#include <sys/types.h>
#include <vector>

class BatchCracker {
  public:
    BatchCracker();
    void crack();

  private:
    struct Result {
        bool found;
        int status;
    };

    struct RunningJob {
        size_t fileIndex;
        pid_t pid;
        FILE *capturedOut;
        FILE *capturedErr;
    };

    void resolveInputs();
    void collectFiles(const std::string &path, bool explicitFile);
    void collectDirectory(const std::string &path);
    RunningJob startImage(size_t fileIndex);
    Result finishImage(RunningJob &job, int status);
    std::string outputFor(const std::string &file) const;
    std::string executablePath() const;
    std::vector<std::string> commandFor(const std::string &file) const;

    std::string wordlist;
    std::vector<std::string> files;
    std::set<std::string> visitedDirectories;
    unsigned int jobs;
    unsigned int threads;
    unsigned long found;
    unsigned long notFound;
    unsigned long errors;
};

#endif // ndef SH_BATCHCRACKER_H
